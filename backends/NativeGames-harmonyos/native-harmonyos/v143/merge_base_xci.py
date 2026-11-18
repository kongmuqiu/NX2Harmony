"""The reference's scripts/extract_update_romfs.py, for a base XCI that also carries an update ("1G+1U").

extract_update_romfs.py takes the base game's Program NCA through extract_exefs.program_nca, which refuses a package
holding more than one Program NCA. A combined cartridge image holds the base Program NCA (title 0100F2C0115B6000, no
BKTR section) next to an older update's Program NCA (BKTR, same title ID) and its Meta NCA. Here the base side picks
the one base Program NCA by that rule; the update side and everything else (ticket checks, hactool merge, IVFC
verification, index) run unchanged. Run from the reference's games/totk directory with the same arguments as
extract_update_romfs.py.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path('scripts').resolve()))
import extract_update_romfs as m  # noqa: E402

BASE_TITLE = '0100F2C0115B6000'
_program_nca = m.program_nca


def base_program(records, update=False):
    if update:
        return _program_nca(records, update=True)
    bases = [(r, h) for r, h in records
             if r['content_type'] == 0 and r['title_id'] == BASE_TITLE
             and not any(h[0x400 + i * 512 + 4] == 4 for i in range(4))]
    m.require(len(bases) == 1, f'Expected one base Program NCA in the cartridge image, found {len(bases)}')
    print(f'base Program NCA {bases[0][0]["name"]} ({bases[0][0]["size"]} bytes) of {len(records)} NCAs', flush=True)
    return bases[0]


m.program_nca = base_program
if __name__ == '__main__':
    parser = m.argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('update', type=Path)
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--hactool', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    m.merge(args.base, args.update, args.keys, args.hactool, args.output)
