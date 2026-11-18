"""Download sources, chosen on the player's PC when something is about to be downloaded.

Each run tests the sources once and takes the fastest; when nothing has to be downloaded (the
image is there, the packages are installed) nothing is tested. So ranked() probes a kind of source the first time a
step asks for it in this process (one build), keeps the answer for the rest of the process, and the caller tries the
sources in that order (the next one when a download fails).

Kinds and what is measured (each candidate in its own thread, at most SAMPLE bytes or SECONDS, all of a kind together):
  registry  python:3.14 from Docker Hub or a pull-through mirror, the .NET SDK from MCR or a mirror: the tag's manifest
            (a mirror is only used when it serves the same manifest digest as the official registry, when that one
            answers), the linux/amd64 manifest (checked against its digest), then the start of its largest layer
  debian    the Debian archive the nativegames-tools:1 image installs from (d2r.Dockerfile APT_MIRROR): InRelease,
            the security suite, the start of main/binary-amd64/Packages.xz. Always over HTTPS: plain-HTTP downloads
            can be altered on the way (captive or guest networks: an InRelease of the wrong size, a .deb with the wrong hash)
  ubuntu    the same for the Ubuntu archive of the .NET SDK image (xlate/build_xlate.sh: NX2H_UBUNTU_MIRROR)
  pypi      the package index pip uses (PIP_INDEX_URL): the lz4 project page and the start of a file it lists
  nuget     the NuGet feed of the translator's restore (NX2H_NUGET_SOURCE): the service index, the start of a package
apt and pip check what they download themselves (signed Release files, the hashes the index lists); docker checks every
layer against the manifest. Speed is bytes over the whole time, the first answer included, so a far source with a slow
start counts as slow. The official source goes first when it is within PREFER_OFFICIAL of the fastest. When no source
answers (offline, a proxy), the official one alone is returned: the same as before this module."""
from __future__ import annotations

import concurrent.futures
import hashlib
import html
import json
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

# Off until it has been tested on a fresh PC. Off, nothing is measured and every caller
# behaves as before this module: the published images are pulled by docker run itself, the image build tries the
# Dockerfile's own sources (LEGACY_APT), and no source is handed to pip, apt or NuGet (a package's backend scripts may
# preset theirs).
ENABLED = False
LEGACY_APT = ["", "", "mirrors.tuna.tsinghua.edu.cn", "mirrors.ustc.edu.cn"]

SAMPLE = 1 << 20          # bytes read from a source's sample file
SECONDS = 6.0             # reading time a source gets (after its first answer)
TIMEOUT = 6               # seconds for a connection or an answer
PREFER_OFFICIAL = 0.85    # the official source first when it reaches this share of the fastest one's speed
UA = "NX2Harmony download-source test"

DEBIAN_SUITE = "trixie"   # python:3.14 (Debian trixie) is FROM of d2r.Dockerfile
UBUNTU_SUITE = "noble"    # mcr.microsoft.com/dotnet/sdk:10.0-noble-aot

# (shown name, registry host or base URL, official)
REGISTRIES = {
    "docker.io": [("Docker Hub", "registry-1.docker.io", True), ("docker.1ms.run", "docker.1ms.run", False),
                  ("mirror.gcr.io", "mirror.gcr.io", False), ("dockerproxy.net", "dockerproxy.net", False),
                  ("docker.m.daocloud.io", "docker.m.daocloud.io", False)],
    "mcr.microsoft.com": [("MCR", "mcr.microsoft.com", True), ("mcr.dockerproxy.net", "mcr.dockerproxy.net", False),
                          ("mcr.m.daocloud.io", "mcr.m.daocloud.io", False)],
}
# Debian: what replaces http://deb.debian.org in python:3.14's debian.sources (/debian and /debian-security below it)
DEBIAN = [("deb.debian.org", "https://deb.debian.org", True), ("华为云", "https://repo.huaweicloud.com", False),
          ("腾讯云", "https://mirrors.cloud.tencent.com", False), ("清华", "https://mirrors.tuna.tsinghua.edu.cn", False),
          ("中科大", "https://mirrors.ustc.edu.cn", False), ("北外", "https://mirrors.bfsu.edu.cn", False),
          ("阿里云", "https://mirrors.aliyun.com", False)]
# Ubuntu: what replaces http://archive.ubuntu.com/ubuntu (and http://security.ubuntu.com/ubuntu, except for the official)
UBUNTU = [("archive.ubuntu.com", "https://archive.ubuntu.com/ubuntu", True),
          ("华为云", "https://repo.huaweicloud.com/ubuntu", False), ("腾讯云", "https://mirrors.cloud.tencent.com/ubuntu", False),
          ("清华", "https://mirrors.tuna.tsinghua.edu.cn/ubuntu", False), ("中科大", "https://mirrors.ustc.edu.cn/ubuntu", False),
          ("北外", "https://mirrors.bfsu.edu.cn/ubuntu", False), ("阿里云", "https://mirrors.aliyun.com/ubuntu", False)]
UBUNTU_SECURITY = "https://security.ubuntu.com/ubuntu"
PYPI = [("pypi.org", "https://pypi.org/simple/", True),
        ("华为云", "https://repo.huaweicloud.com/repository/pypi/simple/", False),
        ("腾讯云", "https://mirrors.cloud.tencent.com/pypi/simple/", False),
        ("清华", "https://pypi.tuna.tsinghua.edu.cn/simple/", False), ("北外", "https://mirrors.bfsu.edu.cn/pypi/web/simple/", False),
        ("阿里云", "https://mirrors.aliyun.com/pypi/simple/", False), ("中科大", "https://mirrors.ustc.edu.cn/pypi/simple/", False)]
NUGET = [("nuget.org", "https://api.nuget.org/v3/index.json", True),
         ("华为云", "https://repo.huaweicloud.com/repository/nuget/v3/index.json", False)]

KIND_NAMES = {"docker.io": "Docker Hub 镜像", "mcr.microsoft.com": "微软镜像仓库", "debian": "Debian 软件源",
              "ubuntu": "Ubuntu 软件源", "pypi": "Python 包源", "nuget": "NuGet 包源"}


class Source:
    def __init__(self, name: str, url: str, official: bool):
        self.name, self.url, self.official = name, url, official
        self.kbps = 0.0          # measured KB/s (0: not measured or failed)
        self.digest = ""         # registries: the manifest digest of the tag this source serves
        self.error = ""

    def ref(self, image: str) -> str:
        """The name docker pull takes for image through this registry source (the image itself for the official one)."""
        if self.official:
            return image
        _, repo, tag = split_image(image)
        return f"{self.url}/{repo}:{tag}"

    def __repr__(self) -> str:
        return f"Source({self.name!r}, {self.kbps:.0f} KB/s{', ' + self.error if self.error else ''})"


def split_image(image: str) -> tuple:
    """(registry, repository, tag): python:3.14 -> (docker.io, library/python, 3.14);
    mcr.microsoft.com/dotnet/sdk:10.0-noble-aot -> (mcr.microsoft.com, dotnet/sdk, 10.0-noble-aot)."""
    name, _, tag = image.rpartition(":")
    if not name or "/" in tag:
        name, tag = image, "latest"
    first = name.split("/", 1)[0]
    if "/" in name and ("." in first or ":" in first or first == "localhost"):
        registry, repo = name.split("/", 1)
    else:
        registry, repo = "docker.io", name
    if registry == "docker.io" and "/" not in repo:
        repo = "library/" + repo
    return registry, repo, tag


# ---------- HTTP ----------
class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None   # the 3xx comes back as an HTTPError: a blob's storage URL is opened without the registry's token


_follow = urllib.request.build_opener()
_stay = urllib.request.build_opener(_NoRedirect)


def _open(url: str, headers=None, follow: bool = True):
    req = urllib.request.Request(url, headers=dict({"User-Agent": UA}, **(headers or {})))
    return (_follow if follow else _stay).open(req, timeout=TIMEOUT)


def _read(resp, deadline: float, limit: int = SAMPLE) -> int:
    """Bytes read from resp until limit or the deadline."""
    n = 0
    while n < limit and time.monotonic() < deadline:
        chunk = resp.read(min(1 << 16, limit - n))
        if not chunk:
            break
        n += len(chunk)
    return n


# ---------- the probes: (bytes, extra) ----------
def _bearer(header: str, repo: str) -> str:
    """A pull token for repo from the realm a registry's 401 names (Docker Hub: auth.docker.io)."""
    if not header.lower().startswith("bearer "):
        return ""
    params = dict(re.findall(r'(\w+)="([^"]*)"', header))
    realm = params.pop("realm", "")
    if not realm.startswith("https://"):
        return ""
    params.setdefault("scope", f"repository:{repo}:pull")
    with _open(realm + ("&" if "?" in realm else "?") + urllib.parse.urlencode(params)) as r:
        j = json.loads(r.read(1 << 16))
    return j.get("token") or j.get("access_token") or ""


ACCEPT = ", ".join(("application/vnd.oci.image.index.v1+json", "application/vnd.docker.distribution.manifest.list.v2+json",
                    "application/vnd.oci.image.manifest.v1+json", "application/vnd.docker.distribution.manifest.v2+json"))


def _probe_registry(s: Source, repo: str, tag: str, deadline: float) -> int:
    auth = {}

    def get(path: str, accept: str = "", follow: bool = True):
        url = f"https://{s.url}/v2/{path}"
        for attempt in (0, 1):
            h = dict(auth, **({"Accept": accept} if accept else {}))
            try:
                return _open(url, h, follow)
            except urllib.error.HTTPError as e:
                if e.code == 401 and attempt == 0 and not auth:
                    token = _bearer(e.headers.get("WWW-Authenticate", ""), repo)
                    if token:
                        auth["Authorization"] = "Bearer " + token
                        continue
                raise
        raise OSError("unauthorized")

    with get(f"{repo}/manifests/{tag}", ACCEPT) as r:
        body = r.read(1 << 20)
        s.digest = r.headers.get("Docker-Content-Digest") or "sha256:" + hashlib.sha256(body).hexdigest()
    n = len(body)
    doc = json.loads(body)
    if "manifests" in doc:   # an index: its linux/amd64 image
        m = next((m for m in doc["manifests"] if (m.get("platform") or {}).get("os") == "linux"
                  and m["platform"].get("architecture") == "amd64"), None)
        if not m:
            raise ValueError("no linux/amd64 image")
        with get(f"{repo}/manifests/{m['digest']}", ACCEPT) as r:
            body = r.read(1 << 20)
        if "sha256:" + hashlib.sha256(body).hexdigest() != m["digest"]:
            raise ValueError("manifest does not match its digest")
        n += len(body)
        doc = json.loads(body)
    layers = doc.get("layers") or []
    if not layers:
        raise ValueError("manifest without layers")
    blob = f"{repo}/blobs/{max(layers, key=lambda l: l.get('size', 0))['digest']}"
    try:
        resp = get(blob, follow=False)
    except urllib.error.HTTPError as e:
        where = e.headers.get("Location") if e.code in (301, 302, 303, 307, 308) else ""
        if not where:
            raise
        resp = _open(urllib.parse.urljoin(f"https://{s.url}/v2/{blob}", where))   # storage (a signed URL): no token
    with resp:
        return n + _read(resp, deadline)


def _probe_apt(base: str, suite: str, security: str, deadline: float) -> int:
    with _open(f"{base}/dists/{suite}/InRelease") as r:
        text = r.read(1 << 20)
    if f"Codename: {suite}".encode() not in text:
        raise ValueError(f"no {suite} InRelease")
    with _open(security) as r:   # the security suite the same sources line switches to
        r.read(1 << 10)
    with _open(f"{base}/dists/{suite}/main/binary-amd64/Packages.xz") as r:
        head = r.read(6)
        if head != b"\xfd7zXZ\x00":
            raise ValueError("Packages.xz is not xz")
        return len(text) + 6 + _read(r, deadline)


def _probe_pypi(index: str, deadline: float) -> int:
    page = index + "lz4/"
    with _open(page, {"Accept": "text/html"}) as r:
        body = r.read(4 << 20)
    m = re.search(r'href="([^"]*/lz4-4\.4\.5\.tar\.gz[^"]*)"', body.decode("utf-8", "replace"))
    if not m:
        raise ValueError("lz4 4.4.5 not listed")
    url = urllib.parse.urljoin(page, html.unescape(m.group(1)))
    if "sha256=" not in url:
        raise ValueError("no sha256 in the index")   # pip checks the file against it
    with _open(url.split("#", 1)[0]) as r:
        if r.read(2) != b"\x1f\x8b":
            raise ValueError("not a .tar.gz")
        return len(body) + 2 + _read(r, deadline)


def _probe_nuget(index: str, deadline: float) -> int:
    with _open(index) as r:
        body = r.read(1 << 20)
    res = json.loads(body).get("resources") or []
    base = next((x.get("@id", "") for x in res if str(x.get("@type", "")).startswith("PackageBaseAddress/3.0.0")), "")
    if not base:
        raise ValueError("no PackageBaseAddress")
    with _open(base.rstrip("/") + "/newtonsoft.json/13.0.3/newtonsoft.json.13.0.3.nupkg") as r:
        if r.read(2) != b"PK":
            raise ValueError("not a .nupkg")
        return len(body) + 2 + _read(r, deadline)


def _measure(s: Source, probe) -> Source:
    t0 = time.monotonic()
    try:
        n = probe(s, t0 + TIMEOUT + SECONDS)
        s.kbps = n / 1024 / max(time.monotonic() - t0, 1e-3)
    except Exception as e:   # every way a source can fail counts the same: not used
        s.kbps, s.error = 0.0, (f"HTTP {e.code}" if isinstance(e, urllib.error.HTTPError) else type(e).__name__)
    return s


# ---------- ranking ----------
_cache = {}
_lock = threading.Lock()


def _rank(sources: list) -> list:
    """The sources that answered, fastest first (the official one first when it is nearly as fast); the official one
    alone when none did."""
    ok = sorted((s for s in sources if s.kbps > 0), key=lambda s: -s.kbps)
    official = next((s for s in sources if s.official), None)
    if not ok:
        return [official] if official else []
    if official in ok and official is not ok[0] and official.kbps >= PREFER_OFFICIAL * ok[0].kbps:
        ok.remove(official)
        ok.insert(0, official)
    return ok


def _speed(kbps: float) -> str:
    return f"{kbps / 1024:.1f} MB/s" if kbps >= 1024 else f"{kbps:.0f} KB/s"


def _report(kind: str, sources: list, ranked: list, say, note: str = "") -> None:
    if not say:
        return
    if not any(s.kbps > 0 for s in sources):
        say(f"    测速选源（{KIND_NAMES.get(kind, kind)}）：都没测通，用官方源")
        return
    fast = "、".join(f"{s.name} {_speed(s.kbps)}" for s in ranked[:4])
    bad = [s.name for s in sources if s.kbps <= 0]
    say(f"    测速选源（{KIND_NAMES.get(kind, kind)}）：{fast}" + (f"；连不上 {len(bad)} 个" if bad else "") + note)


def _probe_all(sources: list, probe) -> list:
    """Every source measured at once."""
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=len(sources))
    try:
        futures = [pool.submit(_measure, s, probe) for s in sources]
        concurrent.futures.wait(futures, timeout=TIMEOUT * 4 + SECONDS)
    finally:
        pool.shutdown(wait=False)
    for s in sources:   # one still running counts as failed (its thread ends with its socket timeout)
        if s.kbps <= 0 and not s.error:
            s.error = "timeout"
    return sources


def ranked(kind: str, say=None, image: str = "") -> list:
    """The sources of kind ("registry" with image, "debian", "ubuntu", "pypi", "nuget"), fastest first, measured on the
    first call of this process (say(text) gets one line about it); later calls get the same list."""
    if not ENABLED:
        table = {"debian": DEBIAN, "ubuntu": UBUNTU, "pypi": PYPI, "nuget": NUGET}.get(kind)
        if kind == "registry":
            registry = split_image(image)[0]
            table = REGISTRIES.get(registry) or [(registry, registry, True)]
        if table is None:
            raise ValueError(kind)
        return [Source(*next(c for c in table if c[2]))]
    key = (kind, image)
    with _lock:
        if key in _cache:
            return list(_cache[key])
        note = ""
        if kind == "registry":
            registry, repo, tag = split_image(image)
            if registry not in REGISTRIES:
                return [Source(registry, registry, True)]
            sources = [Source(*c) for c in REGISTRIES[registry]]
            _probe_all(sources, lambda s, d: _probe_registry(s, repo, tag, d))
            official = next(s for s in sources if s.official)
            if official.digest:   # the official tag answered: a mirror serving another digest (stale, or not the image) is not used
                for s in sources:
                    if not s.official and s.kbps > 0 and s.digest != official.digest:
                        s.kbps, s.error = 0.0, "digest differs"
            elif any(s.kbps > 0 for s in sources):
                note = "（官方仓库没连上，镜像的摘要无法和官方核对）"
            shown = registry
        elif kind == "debian":
            sources = [Source(*c) for c in DEBIAN]
            _probe_all(sources, lambda s, d: _probe_apt(
                s.url + "/debian", DEBIAN_SUITE, f"{s.url}/debian-security/dists/{DEBIAN_SUITE}-security/InRelease", d))
            shown = kind
        elif kind == "ubuntu":
            sources = [Source(*c) for c in UBUNTU]
            _probe_all(sources, lambda s, d: _probe_apt(
                s.url, UBUNTU_SUITE, f"{UBUNTU_SECURITY if s.official else s.url}/dists/{UBUNTU_SUITE}-security/InRelease", d))
            shown = kind
        elif kind == "pypi":
            sources = [Source(*c) for c in PYPI]
            _probe_all(sources, lambda s, d: _probe_pypi(s.url, d))
            shown = kind
        elif kind == "nuget":
            sources = [Source(*c) for c in NUGET]
            _probe_all(sources, lambda s, d: _probe_nuget(s.url, d))
            shown = kind
        else:
            raise ValueError(kind)
        result = _rank(sources)
        _report(shown, sources, result, say, note)
        _cache[key] = result
        return list(result)


# ---------- what the steps use ----------
def pip_env(say=None) -> dict:
    """PIP_INDEX_URL for a pip run (on the PC, or forwarded into a container): the fastest index when it is not PyPI's."""
    best = ranked("pypi", say)[0]
    return {} if best.official else {"PIP_INDEX_URL": best.url}


def _xlate_env(ub: Source, nu: Source) -> dict:
    env = {"NX2H_UBUNTU_MIRROR": ub.url, "NX2H_UBUNTU_SECURITY": UBUNTU_SECURITY if ub.official else ub.url}
    if not nu.official:
        env["NX2H_NUGET_SOURCE"] = nu.url
    return env


def xlate_envs(say=None) -> list:
    """The translator build's Ubuntu archive and NuGet feed (native-harmonyos/xlate/build_xlate.sh reads them), for
    its attempts: the fastest of each, then (when that differs) the second Ubuntu source with nuget.org. HTTPS always
    for apt (the official archive too); the NuGet feed only named when it is not nuget.org. [{}] while ENABLED is off."""
    if not ENABLED:
        return [{}]
    ub, nu = ranked("ubuntu", say), ranked("nuget", say)
    official_nu = next((s for s in nu if s.official), None) or Source(*NUGET[0])
    out = [_xlate_env(ub[0], nu[0])]
    second = _xlate_env(ub[1] if len(ub) > 1 else ub[0], official_nu)
    return out + ([second] if second != out[0] else [])


def nuget_envs(say=None) -> list:
    """For a .NET build that only restores packages (no apt): NX2H_NUGET_SOURCE of the fastest feed when it is not
    nuget.org, then nuget.org itself ({}) as the second attempt."""
    best = ranked("nuget", say)[0]
    return [{}] if best.official else [{"NX2H_NUGET_SOURCE": best.url}, {}]


def apt_attempts(say=None, tries: int = 4) -> list:
    """APT_MIRROR values for the image build's attempts: the fastest Debian source twice (a failure is often
    passing), then the next ones; padded from the list when few answered. LEGACY_APT while ENABLED is off."""
    if not ENABLED:
        return list(LEGACY_APT[:tries])
    rank = [s.url for s in ranked("debian", say)]
    out = rank[:1] * 2 + rank[1:]
    out += [url for _, url, _ in DEBIAN if url not in out]
    return out[:tries]


def pip_attempts(say=None) -> list:
    """PIP_MIRROR values for the same attempts ("" for PyPI itself), fastest first."""
    return ["" if s.official else s.url for s in ranked("pypi", say)]


# the user-level NuGet.Config a container writes before a restore when NX2H_NUGET_SOURCE is set (the project is built
# from a copy under /tmp, so no project-level config is found and this one decides the package sources)
NUGET_CONFIG_SH = ('if [ -n "${NX2H_NUGET_SOURCE:-}" ]; then mkdir -p "$HOME/.nuget/NuGet" && printf '
                   "'<?xml version=\"1.0\" encoding=\"utf-8\"?>\\n<configuration><packageSources><clear />"
                   "<add key=\"nx2h\" value=\"%s\" /></packageSources></configuration>\\n' \"$NX2H_NUGET_SOURCE\" "
                   '> "$HOME/.nuget/NuGet/NuGet.Config"; fi; ')
