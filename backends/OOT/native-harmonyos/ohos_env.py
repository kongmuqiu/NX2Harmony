"""Machine-specific values for the HarmonyOS Python tools: the environment first, then native-harmonyos/local.ohos.ps1
(the same file the PowerShell scripts read; its `$env:NAME = 'value'` lines) and native-harmonyos/local.ohos.env
(optional KEY=VALUE lines). Both files are ignored by git. Names: TOTK_DEVECO, TOTK_HDC, TOTK_HDC_SERVER, TOTK_HDC_TARGET,
TOTK_UPSTREAM, TOTK_OHOS_SDK; hdc and the SDK default to their places inside TOTK_DEVECO."""
import os, re, shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent            # native-harmonyos
PORT = HERE.parent                                # the project (repository) root
REPO = PORT                                       # mounted as /repo in the containers
_PS_LINE = re.compile(r"""^\s*\$env:(\w+)\s*=\s*(['"])(.*?)\2\s*(#.*)?$""")


def read_local(path):
    """The settings in one local file: `$env:NAME = 'value'` lines (.ps1) or NAME=VALUE lines (.env)."""
    values = {}
    if not path.exists():
        return values
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if path.suffix == ".ps1":
            m = _PS_LINE.match(line)
            if m: values[m.group(1)] = m.group(3)
        else:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                values[k.strip()] = v.strip()
    return values


for _name in ("local.ohos.ps1", "local.ohos.env"):
    for _k, _v in read_local(HERE / _name).items():
        if _v: os.environ.setdefault(_k, _v)

DEVECO = Path(os.environ.get("TOTK_DEVECO") or r"C:\Program Files\Huawei\DevEco Studio")
HDC = os.environ.get("TOTK_HDC") or shutil.which("hdc") or str(DEVECO / "sdk/default/openharmony/toolchains/hdc.exe")
DEV = []
if os.environ.get("TOTK_HDC_SERVER"): DEV += ["-s", os.environ["TOTK_HDC_SERVER"]]
if os.environ.get("TOTK_HDC_TARGET"): DEV += ["-t", os.environ["TOTK_HDC_TARGET"]]
UPSTREAM = Path(os.environ.get("TOTK_UPSTREAM") or (PORT.parent / "totk-android"))
SDK = Path(os.environ.get("TOTK_OHOS_SDK") or (DEVECO / "sdk/default/openharmony/native"))
BUNDLE = os.environ.get("TOTK_OHOS_BUNDLE") or "nx2h.u000000.oot"   # NX2Harmony sets the name it built


def docker_path(p):
    """A host path for a docker -v mount on Windows (forward slashes) or elsewhere (as is)."""
    return str(Path(p).resolve()).replace("\\", "/")
