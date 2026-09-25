"""Verify and render complete DRAW dumps from a C6 serial capture."""
import argparse
from pathlib import Path
import re

from pictochat.canvas import detile, write_png


from pictochat.serial import checksum, read_drawings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('-o', '--outdir', type=Path, default=Path('captures_out'))
    args = parser.parse_args()
    drawings = list(read_drawings(args.log.read_text(encoding='utf-8').splitlines()))
    if not drawings:
        raise SystemExit('No complete DRAW dumps found.')
    args.outdir.mkdir(parents=True, exist_ok=True)
    for index, (ident, body) in enumerate(drawings):
        base = args.outdir / f'{args.log.stem}-drawing-{index:02d}-id{ident}'
        base.with_suffix('.bin').write_bytes(body)
        write_png(base.with_suffix('.png'), detile(body[36:]))
        sender = bytes(body[2 + (i ^ 1)] for i in range(6)).hex(':')
        print(f'id={ident} sender={sender} bytes={len(body)} checksum={checksum(body):08x}')


if __name__ == '__main__':
    main()
