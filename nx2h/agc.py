"""Signing in the background: what DevEco Studio's automatic signing does, without opening DevEco Studio.

DevEco Studio 26.0.0.621 (plugins/harmony/lib: login-*.jar, hos-project-mgmt-*.jar, messages/SignatureMgmt.properties;
service addresses from its GRS cache, caches/grs.json) signs a debug build like this, and so does this module:

  login    a local HTTP receiver on 127.0.0.1:<port>/callback; the browser opens
           https://cn.devecostudio.huawei.com/console/DevEcoIDE/apply?port=<port>&appid=1007&code=<secret>, the player
           logs in with a Huawei account there, the page posts code (= secret), tempToken and siteId back;
           GET authrouter/auth/api/temptoken/check?tempToken&site&version&appid gives a JWT, and
           GET authrouter/auth/api/jwToken/check (headers jwtToken, refresh=false|true) gives userInfo.accessToken
  team     GET  https://connect-api.cloud.huawei.com/api/ups/user-permission-service/v1/user-team-list (uid, oauth2Token)
  cert     an EC P-256 key and CSR made with keytool (device.java_home: DevEco's own JDK first),
           POST .../harmony-cert-manage/v1/cert/add {csr, certName, certType 1}, .../cert/list for its object id,
           .../amis/app-manage/v1/objects/url/reapply {sourceUrls} for a download address
  devices  GET .../device-manage/v1/device/list; a connected phone not on it: POST .../device/add {deviceName, udid,
           deviceType}
  profile  POST .../provision-manage/v1/ide/test/provision/add {certList, packageName, deviceList, provisionName,
           aclPermissionList}: a debug profile for this bundle name and every phone of the account, downloaded the same way
  headers  uid, teamId, oauth2Token (the access token); a 401 refreshes the token once

One key and one debug certificate serve every game of this PC; each game's bundle name gets its own profile. hvigor
signs with them as with DevEco's files: build-profile.json5 "material" names the .p12, .cer and .p7b, and the two
passwords are encrypted the way hvigor-ohos-plugin's utils/decipher-util.js decrypts them (AES-128-GCM with a work key,
itself sealed with a root key from the "material" folder beside the .p12). The material here is this app's own,
not DevEco's.

Kept in %USERPROFILE%\\.nx2h (signing belongs to the Windows user, like ~/.ohos): account.dat and keys/key.dat are
sealed with Windows DPAPI (only this Windows user can open them); keys/ also holds nx2h.p12, nx2h.cer, material/ and
profiles/<bundle>.p7b. Nothing of it is printed, logged or copied off this PC."""
from __future__ import annotations

import base64
import ctypes
import email
import hashlib
import json
import os
import re
import secrets
import string
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import bundle as bundles
from . import device
from . import proc

LOGIN_WEB = "https://cn.devecostudio.huawei.com"
API = "https://connect-api.cloud.huawei.com"
APPID = "1007"
HOME = Path.home() / ".nx2h"
ACCOUNT = HOME / "account.dat"
KEYS = HOME / "keys"
PROFILES = KEYS / "profiles"
ALIAS = "debugKey"
DNAME = "CN=DebugKey,OU=,O=,L=,ST=,C="
UA = {"User-Agent": "Chrome/49.0.2623.75", "accept-language": "zh-CN"}
SITES = {"1": "CN", "5": "SG", "7": "DE", "8": "RU"}
COMPONENT = bytes([49, 243, 9, 115, 214, 175, 91, 184, 211, 190, 177, 88, 101, 131, 192, 119])   # decipher-util.js

P_TEAMS = "/api/ups/user-permission-service/v1/user-team-list"
P_CERTS = "/api/cps/harmony-cert-manage/v1/cert/list"
P_CERT_ADD = "/api/cps/harmony-cert-manage/v1/cert/add"
P_CERT_DEL = "/api/cps/harmony-cert-manage/v1/cert/delete"
P_DEVICES = "/api/cps/device-manage/v1/device/list"
P_DEVICE_ADD = "/api/cps/device-manage/v1/device/add"
P_PROFILE_ADD = "/api/cps/provision-manage/v1/ide/test/provision/add"
P_PROFILE_DEL = "/api/cps/provision-manage/v1/provision/delete"
P_DOWNLOAD = "/api/amis/app-manage/v1/objects/url/reapply"

ERRORS = {"205389859": "账号登记的设备数量已到上限（AppGallery Connect → 设备管理里删掉不用的）",
          "205389857": "设备名称重复",
          "205389938": "调试 Profile 数量已到上限（AppGallery Connect → Profile 管理里删掉不用的）",
          "205389959": "这个包名的应用 ID 无效",
          "205389830": "Profile 名称重复"}


class SigningError(Exception):
    pass


class NoJava(SigningError):
    """No Java to run keytool (device.java_home): installing one is the way on, not the Huawei account."""


# ---------- Windows DPAPI: files only this Windows user can open ----------
class _Blob(ctypes.Structure):
    _fields_ = [("cbData", ctypes.c_uint32), ("pbData", ctypes.POINTER(ctypes.c_char))]


def _dpapi(data: bytes, protect: bool) -> bytes:
    crypt32, kernel32 = ctypes.windll.crypt32, ctypes.windll.kernel32
    kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    buf = ctypes.create_string_buffer(data, len(data))
    src = _Blob(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
    dst = _Blob()
    fn = crypt32.CryptProtectData if protect else crypt32.CryptUnprotectData
    if not fn(ctypes.byref(src), None, None, None, None, 0x1, ctypes.byref(dst)):   # CRYPTPROTECT_UI_FORBIDDEN
        raise SigningError("Windows 数据保护（DPAPI）打不开签名数据：换了 Windows 用户？")
    try:
        return ctypes.string_at(dst.pbData, dst.cbData)
    finally:
        kernel32.LocalFree(ctypes.cast(dst.pbData, ctypes.c_void_p))


def _save(path: Path, obj: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_bytes(_dpapi(json.dumps(obj).encode("utf-8"), True))
    os.replace(tmp, path)


def _load(path: Path) -> dict:
    try:
        return json.loads(_dpapi(path.read_bytes(), False).decode("utf-8"))
    except (OSError, ValueError):
        return {}


# ---------- HTTP ----------
def _http(method: str, url: str, headers=None, body=None, timeout=60):
    """(status, bytes)."""
    data = None
    h = dict(UA, **(headers or {}))
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        h["Content-Type"] = "application/json;charset=UTF-8"
    req = urllib.request.Request(url, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read() or b""
    except (urllib.error.URLError, OSError) as e:
        raise SigningError(f"连不上华为的服务器（{getattr(e, 'reason', e)}）：检查网络或代理")


def _json(raw: bytes) -> dict:
    try:
        v = json.loads(raw.decode("utf-8", "replace"))
        return v if isinstance(v, dict) else {"data": v}
    except ValueError:
        return {}


def _ok(r: dict) -> bool:
    ret = r.get("ret")
    return isinstance(ret, dict) and ret.get("code") in (0, "0")


def _fail(what: str, r: dict, status: int) -> SigningError:
    ret = r.get("ret") if isinstance(r.get("ret"), dict) else {}
    code = str(ret.get("code", status))
    msg = ERRORS.get(code) or ret.get("msg") or r.get("message") or f"HTTP {status}"
    return SigningError(f"{what}失败：{msg}（{code}）")


# ---------- login ----------
def _trace(line: str) -> None:
    """What the login receiver saw (request line and field names, never values): %USERPROFILE%\\.nx2h\\login.log."""
    try:
        HOME.mkdir(parents=True, exist_ok=True)
        with open(HOME / "login.log", "a", encoding="utf-8") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + line + "\n")
    except OSError:
        pass


def _multipart(body: bytes, ctype: str) -> dict:
    msg = email.message_from_bytes(b"Content-Type: " + ctype.encode() + b"\r\n\r\n" + body)
    out = {}
    for part in msg.walk() if msg.is_multipart() else []:
        name = part.get_param("name", header="content-disposition")
        if name:
            out[name] = (part.get_payload(decode=True) or b"").decode("utf-8", "replace")
    return out


def _deveco_version() -> str:
    try:
        return json.loads((device.deveco_root() / "product-info.json").read_text(encoding="utf-8")).get("version") or "26.0.0.621"
    except (OSError, ValueError):
        return "26.0.0.621"


def _jwt_payload(jwt: str) -> dict:
    try:
        part = jwt.split(".")[1]
        return json.loads(base64.urlsafe_b64decode(part + "=" * (-len(part) % 4)))
    except (IndexError, ValueError):
        return {}


def _check_jwt(jwt: str, refresh: bool) -> dict:
    status, raw = _http("GET", f"{LOGIN_WEB}/authrouter/auth/api/jwToken/check",
                        headers={"jwtToken": jwt, "refresh": "true" if refresh else "false"})
    r = _json(raw)
    info = r.get("userInfo") if isinstance(r.get("userInfo"), dict) else {}
    if not r.get("status") or not info.get("accessToken"):
        raise SigningError("华为账号登录已失效，需要重新登录")
    return info


def login(say=print, timeout: int = 600) -> dict:
    """Open the Huawei account login page in the browser and wait for it to call back; returns the account."""
    secret = uuid.uuid4().hex
    got, done = {}, threading.Event()

    class Callback(BaseHTTPRequestHandler):
        timeout = 20   # a browser's spare connection that never sends a request must not hold the receiver

        def _cors(self) -> None:
            self.send_header("Access-Control-Allow-Origin", self.headers.get("Origin") or "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", self.headers.get("Access-Control-Request-Headers") or "*")
            self.send_header("Access-Control-Allow-Private-Network", "true")

        def _body(self) -> bytes:
            if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
                out = b""
                while True:
                    size = int(self.rfile.readline().split(b";")[0].strip() or b"0", 16)
                    if not size:
                        self.rfile.readline()
                        return out
                    out += self.rfile.read(size)
                    self.rfile.readline()
            return self.rfile.read(int(self.headers.get("Content-Length") or 0))

        def _handle(self, body: bytes) -> None:
            u = urllib.parse.urlparse(self.path)
            fields = dict(urllib.parse.parse_qsl(u.query))
            ctype = self.headers.get("Content-Type", "")
            if "multipart/form-data" in ctype:
                fields.update(_multipart(body, ctype))
            elif "json" in ctype and body:
                fields.update({k: str(v) for k, v in _json(body).items()})
            elif body:
                fields.update(urllib.parse.parse_qsl(body.decode("utf-8", "replace")))
            _trace(f"{self.command} {u.path} type={ctype or '-'} fields={sorted(fields)}")   # names only
            ok = u.path == "/callback" and fields.get("code") == secret
            if ok:
                got.update(fields)
                done.set()
            if self.command == "GET" and not ok:
                page = "<meta charset=utf-8><p>NX2Harmony 正在等待华为账号登录完成……</p>".encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(page)))
                self._cors()
                self.end_headers()
                self.wfile.write(page)
                return
            page = "loginSuccess" if ok and fields.get("tempToken") else "loginFailed"
            self.send_response(302)
            self.send_header("Location", f"{LOGIN_WEB}/console/DevEcoIDE/{page}")
            self._cors()
            self.end_headers()

        def do_OPTIONS(self) -> None:   # a page calling us with fetch() asks first
            _trace(f"OPTIONS {urllib.parse.urlparse(self.path).path}")
            self.send_response(204)
            self._cors()
            self.end_headers()

        def do_POST(self) -> None:
            self._handle(self._body())

        def do_GET(self) -> None:
            self._handle(b"")

        def log_message(self, *a) -> None:
            pass

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Callback)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        url = f"{LOGIN_WEB}/console/DevEcoIDE/apply?port={srv.server_address[1]}&appid={APPID}&code={secret}"
        webbrowser.open(url)
        say("    已在浏览器里打开华为账号登录页：登录后回到这里（只需要一次）")
        if not done.wait(timeout):
            raise SigningError("等了 10 分钟没有完成登录")
    finally:
        srv.shutdown()
    temp = (got.get("tempToken") or "").split("&")[0]
    if not temp:
        raise SigningError("登录被取消了")
    params = urllib.parse.urlencode({"tempToken": temp, "site": SITES.get(got.get("siteId", "1"), "CN"),
                                     "version": _deveco_version(), "appid": APPID})
    status, raw = _http("GET", f"{LOGIN_WEB}/authrouter/auth/api/temptoken/check?{params}")
    jwt = raw.decode("utf-8", "replace").strip().strip('"')
    if jwt.startswith("{"):
        r = _json(raw)
        jwt = r.get("jwtToken") or (r.get("data") if isinstance(r.get("data"), str) else "") or ""
    if jwt.count(".") != 2:
        raise SigningError(f"登录没有换到令牌（HTTP {status}）")
    info = _check_jwt(jwt, refresh=False)
    claims = _jwt_payload(jwt)
    acct = {"jwt": jwt, "accessToken": info["accessToken"], "userId": str(claims.get("userId") or ""),
            "userName": str(claims.get("userName") or ""), "realName": bool(info.get("realName")), "time": time.time()}
    if not acct["userId"]:
        raise SigningError("登录令牌里没有用户 ID")
    status, raw = _http("GET", API + P_TEAMS, headers={"uid": acct["userId"], "oauth2Token": acct["accessToken"]})
    teams = [t for t in (_json(raw).get("teams") or []) if isinstance(t, dict) and t.get("id")]
    if not teams:
        raise SigningError("这个华为账号还没有开发者团队：先在 developer.huawei.com 完成开发者注册（实名认证）")
    team = next((t for t in teams if str(t.get("id")) == acct["userId"]), teams[0])
    acct.update(teamId=str(team["id"]), teamName=str(team.get("name") or ""))
    _save(ACCOUNT, acct)
    say(f"    已登录华为账号：{acct['userName'] or acct['userId']}（团队 {acct['teamName'] or acct['teamId']}）")
    return acct


def account() -> dict:
    return _load(ACCOUNT) if ACCOUNT.is_file() else {}


def logout() -> None:
    ACCOUNT.unlink(missing_ok=True)


def _session(say=print) -> dict:
    """The saved account with a fresh access token, else a new login."""
    acct = account()
    if acct.get("jwt"):
        try:
            acct["accessToken"] = _check_jwt(acct["jwt"], refresh=True)["accessToken"]
            _save(ACCOUNT, acct)
            return acct
        except SigningError:
            pass
    return login(say)


def _api(acct: dict, method: str, path: str, body=None, query=None, what: str = "", retry: bool = True) -> dict:
    url = API + path + ("?" + urllib.parse.urlencode(query) if query else "")
    headers = {"uid": acct["userId"], "teamId": acct["teamId"], "oauth2Token": acct["accessToken"]}
    status, raw = _http(method, url, headers=headers, body=body)
    if status in (401, 403) and retry:
        acct["accessToken"] = _check_jwt(acct["jwt"], refresh=True)["accessToken"]
        _save(ACCOUNT, acct)
        return _api(acct, method, path, body, query, what, retry=False)
    r = _json(raw)
    if status != 200 or not _ok(r):
        raise _fail(what or path.rsplit("/", 2)[-2], r, status)
    return r


def _download(acct: dict, source: str, what: str) -> bytes:
    r = _api(acct, "POST", P_DOWNLOAD, body={"sourceUrls": source}, what=f"取{what}下载地址")
    urls = r.get("urlsInfo") or []
    if not urls or not urls[0].get("newUrl"):
        raise SigningError(f"{what}没有下载地址")
    status, raw = _http("GET", urls[0]["newUrl"], timeout=120)
    if status != 200 or not raw:
        raise SigningError(f"下载{what}失败（HTTP {status}）")
    return raw


# ---------- key, password material ----------
def _gcm_seal(key: bytes, data: bytes) -> bytes:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    iv = os.urandom(12)
    sealed = AESGCM(key).encrypt(iv, data, None)          # ciphertext + 16-byte tag
    return len(sealed).to_bytes(4, "big") + iv + sealed   # decipher-util.js: u32 BE length, IV, ciphertext, tag


def _gcm_open(key: bytes, blob: bytes) -> bytes:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    n = int.from_bytes(blob[:4], "big")
    iv = blob[4:len(blob) - n]
    return AESGCM(key).decrypt(iv, blob[len(blob) - n:], None)


def _one_file(d: Path) -> bytes:
    files = [f for f in d.iterdir() if f.is_file()]
    if len(files) != 1:
        raise SigningError(f"签名材料损坏：{d}")
    return files[0].read_bytes()


def _work_key() -> bytes:
    """The key hvigor decrypts the passwords with, from material/ beside the .p12 (made once), laid out as DevEco's:
    fd/0, fd/1, fd/2 (one 16-byte part each), ac (the salt), ce (the sealed work key), one file per folder."""
    m = KEYS / "material"
    if not (m / "ce").is_dir():
        target = "".join(secrets.choice(string.ascii_letters + string.digits) for _ in range(16)).encode()
        a, b = os.urandom(16), os.urandom(16)
        c = bytes(x ^ y ^ z ^ w for x, y, z, w in zip(target, a, b, COMPONENT))   # so fd0^fd1^fd2^component is ASCII
        salt, work = os.urandom(16), os.urandom(16)
        root = hashlib.pbkdf2_hmac("sha256", target, salt, 10000, 16)
        for sub, data in (("fd/0", a), ("fd/1", b), ("fd/2", c), ("ac", salt), ("ce", _gcm_seal(root, work))):
            (m / sub).mkdir(parents=True, exist_ok=True)
            (m / sub / secrets.token_hex(16)).write_bytes(data)
    parts = [_one_file(d) for d in sorted((m / "fd").iterdir()) if d.is_dir()]
    if len(parts) != 3 or any(len(p) != 16 for p in parts):
        raise SigningError(f"签名材料损坏：{m / 'fd'}")
    x = bytes(p ^ q ^ r ^ s for p, q, r, s in zip(*parts, COMPONENT))
    root = hashlib.pbkdf2_hmac("sha256", x.decode("utf-8", "replace").encode("utf-8"), _one_file(m / "ac"), 10000, 16)
    return _gcm_open(root, _one_file(m / "ce"))


def encrypt_password(pwd: str) -> str:
    return _gcm_seal(_work_key(), pwd.encode("utf-8")).hex()


def decrypt_password(hexed: str) -> str:
    return _gcm_open(_work_key(), bytes.fromhex(hexed)).decode("utf-8")


def _keytool(args: list, pwd: str) -> None:
    # device.java_home, looked for now (one installed meanwhile counts): the jbr of the DevEco Studio in use first,
    # else the Java hvigor runs (PATH), JAVA_HOME, a JDK where its installer puts it
    jdk = device.java_home()
    if jdk is None:   # a toolchain without its own Java: a JDK; no toolchain at all: DevEco Studio, which brings one
        raise NoJava("没找到 Java（keytool）：" + ("请安装 Microsoft OpenJDK 21" if device.find_deveco().use
                                                  else "先装好 DevEco Studio（自带 Java）"))
    exe = jdk / "bin" / "keytool.exe"
    env = dict(os.environ, NX2H_KS_PASS=pwd)
    r = proc.run([str(exe)] + args + ["-storepass:env", "NX2H_KS_PASS", "-keypass:env", "NX2H_KS_PASS"],
                 capture_output=True, text=True, errors="replace", env=env, timeout=120)
    if r.returncode:
        raise SigningError("生成签名密钥失败：" + (r.stdout + r.stderr).strip().splitlines()[-1][:200])


def _cert_name() -> str:
    return f"nx2h_debug_{bundles.pc_part()}"


def _ensure_cert(acct: dict, say) -> str:
    """The debug certificate of this PC's key: reused while it is on the account and valid, else made anew."""
    key = _load(KEYS / "key.dat") if (KEYS / "key.dat").is_file() else {}
    certs = _api(acct, "POST", P_CERTS, body={}, what="查询证书").get("certList") or []
    mine = [c for c in certs if c.get("certName") == key.get("certName", _cert_name())]
    p12, cer = KEYS / "nx2h.p12", KEYS / "nx2h.cer"
    now_ms = time.time() * 1000
    for c in mine:
        if (key.get("certId") == str(c.get("id")) and p12.is_file() and cer.is_file()
                and float(c.get("expireTime") or 0) > now_ms + 7 * 86400e3):
            return key["certId"]
    for c in mine:   # its key is gone from this PC (or it expires): replace it, as DevEco does
        _api(acct, "DELETE", P_CERT_DEL, body={"certIds": [c["id"]]}, what="删除旧证书")
    say("    生成签名密钥并申请调试证书")
    KEYS.mkdir(parents=True, exist_ok=True)
    pwd = "".join(secrets.choice(string.ascii_letters + string.digits) for _ in range(24))
    for f in (p12, KEYS / "nx2h.csr", cer):
        f.unlink(missing_ok=True)
    _keytool(["-genkeypair", "-alias", ALIAS, "-keyalg", "EC", "-sigalg", "SHA256withECDSA", "-groupname", "secp256r1",
              "-keystore", str(p12), "-storetype", "pkcs12", "-validity", "9125", "-dname", DNAME], pwd)
    _keytool(["-certreq", "-alias", ALIAS, "-sigalg", "SHA256withECDSA", "-keystore", str(p12),
              "-file", str(KEYS / "nx2h.csr")], pwd)
    name = _cert_name()
    _api(acct, "POST", P_CERT_ADD, what="申请调试证书",
         body={"csr": (KEYS / "nx2h.csr").read_text(encoding="utf-8"), "certName": name, "certType": "1"})
    certs = _api(acct, "POST", P_CERTS, body={}, what="查询证书").get("certList") or []
    c = next((c for c in certs if c.get("certName") == name), None)
    if not c:
        raise SigningError("证书申请后在账号里找不到它")
    cer.write_bytes(_download(acct, c["certObjectId"], "证书"))
    _save(KEYS / "key.dat", {"password": pwd, "certId": str(c["id"]), "certName": name})
    return str(c["id"])


# ---------- devices ----------
def _devices(acct: dict) -> list:
    r = _api(acct, "GET", P_DEVICES, query={"encodeFlag": "0", "start": "1", "pageSize": "100"}, what="查询设备")
    out = list(r.get("list") or [])
    total = int(r.get("totalCount") or 0)
    if total > len(out):
        r = _api(acct, "GET", P_DEVICES, query={"encodeFlag": "0", "start": "1", "pageSize": str(total)}, what="查询设备")
        out = list(r.get("list") or [])
    return [d for d in out if isinstance(d, dict) and d.get("id")]


def phone_udid(hdc) -> str:
    """The connected phone's UDID (bm get --udid), "" without one."""
    if not hdc or not hdc.pick():
        return ""
    m = re.search(r"\b([0-9A-Fa-f]{64})\b", hdc.shell("bm get --udid") or "")
    return m.group(1).upper() if m else ""


def _device_type(hdc, known: list) -> str:
    kind = (hdc.shell("param get const.product.devicetype") or "").strip().lower() if hdc else ""
    types = {"liteWearable": "1", "wearable": "2", "tv": "8", "router": "9", "phone": "4", "tablet": "4", "2in1": "4"}
    if kind in {k.lower(): v for k, v in types.items()}:
        return {k.lower(): v for k, v in types.items()}[kind]
    seen = [str(d.get("deviceType")) for d in known if d.get("deviceType") is not None]
    return max(set(seen), key=seen.count) if seen else "4"


# ---------- profiles ----------
def profile_path(bundle: str) -> Path:
    return PROFILES / f"{bundle}.p7b"


def device_ids(p7b: Path) -> list:
    try:
        m = re.search(rb'"device-ids"\s*:\s*\[([^\]]*)\]', p7b.read_bytes())
    except OSError:
        return []
    return [x.decode().upper() for x in re.findall(rb'"([0-9A-Za-z]+)"', m.group(1))] if m else []


def ensure(bundle: str, say=print, hdc=None) -> Path:
    """A debug profile for this bundle name signed for this PC's key, covering every phone of the account (the
    connected one registered first); logs in when needed. Returns the .p7b."""
    if not bundles.valid(bundle):
        raise SigningError(f"包名 {bundle} 不符合鸿蒙规则")
    acct = _session(say)
    cert_id = _ensure_cert(acct, say)
    known = _devices(acct)
    udid = phone_udid(hdc)
    if udid and udid not in {str(d.get("udid", "")).upper() for d in known}:
        say("    把连着的手机登记为调试设备")
        _api(acct, "POST", P_DEVICE_ADD, what="登记手机",
             body={"deviceName": f"nx2h_{int(time.time())}", "udid": udid, "deviceType": _device_type(hdc, known)})
        known = _devices(acct)
    if not known:
        raise SigningError("这个华为账号还没有登记任何手机：把手机连上电脑（开发者模式、USB 调试）再构建一次")
    say(f"    为 {bundle} 申请调试 Profile（{len(known)} 台设备）")
    tag = hashlib.sha256(f"{bundle}:{time.time()}".encode()).hexdigest()[:8]
    r = _api(acct, "POST", P_PROFILE_ADD, what="申请调试 Profile",
             body={"certList": [cert_id], "packageName": bundle, "deviceList": [d["id"] for d in known],
                   "provisionName": f"nx2h_{bundle.rsplit('.', 1)[-1]}_{tag}", "aclPermissionList": []})
    url = r.get("provisionFileUrl") or (r.get("provisionInfo") or {}).get("provisionFileUrl")
    if not url:
        raise SigningError("Profile 申请成功但没有下载地址")
    data = _download(acct, url, "Profile")
    PROFILES.mkdir(parents=True, exist_ok=True)
    out = profile_path(bundle)
    tmp = out.with_suffix(".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, out)
    key = _load(KEYS / "key.dat")
    old = (key.get("profiles") or {}).get(bundle)
    new_id = str(r.get("id") or (r.get("provisionInfo") or {}).get("id") or "")
    if old and old != new_id:   # the one it replaces (one profile per game on the account)
        try:
            _api(acct, "DELETE", P_PROFILE_DEL, query={"id": old}, body={}, what="删除旧 Profile")
        except SigningError:
            pass
    key["profiles"] = dict(key.get("profiles") or {}, **{bundle: new_id})
    _save(KEYS / "key.dat", key)
    say(f"    签名就绪：{bundle}")
    return out


def config_for(bundle: str):
    """The hvigor signing configuration for a profile made here, or None."""
    p7b, p12, cer = profile_path(bundle), KEYS / "nx2h.p12", KEYS / "nx2h.cer"
    if not (p7b.is_file() and p12.is_file() and cer.is_file() and (KEYS / "key.dat").is_file()):
        return None
    pwd = _load(KEYS / "key.dat").get("password")
    if not pwd:
        return None
    sealed = encrypt_password(pwd)
    return {"name": "default", "type": "HarmonyOS",
            "material": {"certpath": str(cer), "keyAlias": ALIAS, "keyPassword": sealed, "profile": str(p7b),
                         "signAlg": "SHA256withECDSA", "storeFile": str(p12), "storePassword": sealed}}
