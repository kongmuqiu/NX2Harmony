"""Container half of a translation probe (python:3.14, /android = the reference, read-only): translates the requests in
<requests dir> with the translator's --service mode, as fill_xlate_in_container.py does, and keeps everything it wrote
(<variant>.spv, .res.json, _b.json, the SPIR-V disassembly when spirv-dis is there) in <out dir>, to read what a shader's
resources came out as. Nothing goes to the seed.
usage: xlate_probe_in_container.py <requests dir> <out dir> <translator> <spirv-cross>"""
import json, shutil, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, '/android/games/totk/scripts')
from prepare_shader_seed import CHAIN

req_dir, out, translator, spirv_cross = (Path(p) for p in sys.argv[1:5])
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp); xl = tmp / 'out'; xl.mkdir()
    proc = subprocess.Popen([str(translator), '--service', '--spirv-cross', str(spirv_cross), '--out', str(xl), '--chain', CHAIN],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    for n in sorted(p.name for p in req_dir.glob('*.json')):
        rq = json.loads((req_dir / n).read_text())
        code = tmp / f'{rq["variant"]}.code'; code.write_bytes(bytes.fromhex(rq.pop('code_hex')))
        rq['code'] = str(code)
        proc.stdin.write(json.dumps(rq) + '\n'); proc.stdin.flush()
        reply = json.loads(proc.stdout.readline())
        print(n, 'ok' if reply.get('ok') else reply.get('error'), flush=True)
    proc.stdin.close(); proc.wait()
    for f in xl.iterdir():
        shutil.copy2(f, out / f.name)
print(sorted(p.name for p in out.iterdir()))
