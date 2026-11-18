"""Source edits to the vendored Ryujinx shader translator for the Z-A port, shared by build_xlate.sh (in the container)
and prepare_xlate_src.py (on the PC).

DrawParametersReplace (taken when the accessor answers QueryHasConstantBufferDrawParameters, TOTK_XLATE_DRAW_PARAMS=1)
replaces NVN's draw constants in c0 with Vulkan's draw parameters. Only the base instance (c0[0x644]) is replaced here:
the translator makes InstanceID = InstanceIndex - BaseInstance and Trinity's shaders add c0[0x644] back (793 vertex
shaders; this backend left it 0, so a draw with a base instance read instance 0's data). The base vertex (c0[0x640]) and
draw index (c0[0x648]) stay in the driver bank (0): VertexID is Vulkan's VertexIndex, which already includes the vertex
offset / first vertex, and the shaders add c0[0x640] to it, so BaseVertex there would count the offset twice.
  python3 xlate_edits.py <vendor/ryujinx folder>
"""
import sys
from pathlib import Path

CASES = {
    'BaseVertex': """                        case Constants.NvnBaseVertexByteOffset / 4:
                            operation.SetSource(srcIndex, GenerateLoad(IoVariable.BaseVertex));
                            modified = true;
                            break;
""",
    'DrawIndex': """                        case Constants.NvnDrawIndexByteOffset / 4:
                            operation.SetSource(srcIndex, GenerateLoad(IoVariable.DrawIndex));
                            modified = true;
                            break;
""",
}


def base_instance_only(ryujinx):
    p = Path(ryujinx) / 'src/Ryujinx.Graphics.Shader/Translation/Transforms/DrawParametersReplace.cs'
    s = p.read_bytes().decode('utf-8').replace('\r\n', '\n')
    for name, text in CASES.items():
        assert s.count(text) == 1, f'DrawParametersReplace: the {name} case is not the expected text'
        s = s.replace(text, '')
    assert 'GenerateLoad(IoVariable.BaseInstance)' in s
    p.write_bytes(s.encode('utf-8'))
    print('DrawParametersReplace: base instance only', p)


if __name__ == '__main__':
    base_instance_only(sys.argv[1])
