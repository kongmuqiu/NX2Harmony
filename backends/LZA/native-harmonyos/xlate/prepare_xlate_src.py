"""Host half of the translator build, for when the build container cannot install python3 / patch (an Ubuntu mirror in
the middle of a sync refuses apt for hours). Does on the PC what build_xlate.sh does in the container before publishing:
copies the reference's games/totk/tools/totk_xlate and vendor/ryujinx into <out>, LF line ends, the HarmonyOS
accessor answers of build_xlate.sh (TOTK_XLATE_SB_ALIGN, TOTK_XLATE_DRAW_PARAMS) and the Ryujinx patches applied with
Git Bash's patch; publish_native.sh then only publishes (its own patch loop is turned off).
  python native-harmonyos/xlate/prepare_xlate_src.py <out folder>
  docker run --rm -v <out>:/src -v <bin>:/out mcr.microsoft.com/dotnet/sdk:10.0-noble-aot \
      sh /src/games/totk/tools/totk_xlate/publish_native.sh dotnet --out /out
"""
import re, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from ohos_env import UPSTREAM   # noqa: E402

out = Path(sys.argv[1]).resolve()
if out.exists():
    shutil.rmtree(out)
xl = out / 'games/totk/tools/totk_xlate'
shutil.copytree(UPSTREAM / 'games/totk/tools/totk_xlate', xl, ignore=shutil.ignore_patterns('bin', 'obj'))
shutil.copytree(UPSTREAM / 'vendor/ryujinx', out / 'vendor/ryujinx', ignore=shutil.ignore_patterns('bin', 'obj'))
TEXT = {'.cs', '.csproj', '.props', '.targets', '.patch', '.sh', '.json', '.py'}
for p in out.rglob('*'):
    if p.is_file() and p.suffix in TEXT:
        b = p.read_bytes()
        if b'\r\n' in b:
            p.write_bytes(b.replace(b'\r\n', b'\n'))

# the accessor answers, word for word as build_xlate.sh adds them
script = (HERE / 'build_xlate.sh').read_text(encoding='utf-8')
add = re.search(r'add = anchor \+ """(.*?)"""', script, re.S).group(1)
prog = xl / 'Program.cs'
s = prog.read_text(encoding='utf-8')
anchor = "        public int QueryTextureArrayLengthFromPool() => 32;\n"
assert s.count(anchor) == 1 and 'QueryHasUnalignedStorageBuffer' not in s and 'QueryHasConstantBufferDrawParameters' not in s
prog.write_bytes(s.replace(anchor, anchor + add).encode('utf-8'))   # LF kept (write_text would write CRLF on Windows)
sys.path.insert(0, str(HERE))
import xlate_edits   # noqa: E402
xlate_edits.base_instance_only(out / 'vendor/ryujinx')

# the Ryujinx patches, applied here; publish_native.sh's loop is turned off
for p in sorted((xl / 'patches').glob('*.patch')):
    r = subprocess.run(['patch', '-s', '-d', str(out / 'vendor/ryujinx'), '-p1', '--forward', '-i', str(p)], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'{p.name}: {r.stdout}{r.stderr}')
pub = xl / 'publish_native.sh'
t = pub.read_text(encoding='utf-8')
loop = 'for p in "$HERE"/patches/*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done\n'
assert t.count(loop) == 1
pub.write_bytes(t.replace(loop, ': # patches applied by prepare_xlate_src.py\n').encode('utf-8'))
crlf = [str(p.relative_to(out)) for p in out.rglob('*') if p.is_file() and p.suffix in TEXT and b'\r\n' in p.read_bytes()]
assert not crlf, f'CRLF left in {crlf[:5]}'
print('prepared', out)
