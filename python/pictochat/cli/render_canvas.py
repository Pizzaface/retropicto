#!/usr/bin/env python3
"""Render complete PictoChat messages from a radiotap PCAP.

Reassembles announced transfers using declared fragment sizes and offsets,
including the short final fragment. The first 36 transfer bytes are message
metadata. Remaining bytes are a 256-pixel-wide 4bpp tiled bitmap. Sequence
footers are not pixels; no masking or inpainting is needed.

Usage: python tools/render_canvas.py send.pcap --all -o captures_out
"""
import argparse
import os
import struct
import zlib

from pictochat import capture as an
from pictochat.message import decode
from pictochat.canvas import detile, write_png

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pcap')
    parser.add_argument('--all', action='store_true', help='render every complete message')
    parser.add_argument('-o', '--outdir', default='.')
    parser.add_argument('--inpaint', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.inpaint:
        print('--inpaint is obsolete: sequence footers are now excluded from bitmap data.')
    messages = list(decode(an.parse_pcap(args.pcap)))
    if not messages:
        raise SystemExit('No complete announced bitmap messages found; incomplete captures are not filled in.')
    os.makedirs(args.outdir, exist_ok=True)
    base = os.path.splitext(os.path.basename(args.pcap))[0]
    selected = enumerate(messages) if args.all else [(len(messages) - 1, messages[-1])]
    for index, message in selected:
        suffix = f'_snap{index:02d}' if args.all else '_canvas'
        path = os.path.join(args.outdir, base + suffix + '.png')
        print(f'message {index}: sender={an.mac(message.sender)}, '
              f'{len(message.bitmap)} bitmap bytes, {message.chunks} captured fragments')
        write_png(path, detile(message.bitmap))


if __name__ == '__main__':
    main()
