"""Decode independent sniffer MP records and report complete/incomplete drawings."""
import argparse
from pathlib import Path
import re

from pictochat.capture import wlan_header_len
from pictochat.serial import checksum
from pictochat.message import Decoder
from pictochat.canvas import detile, write_png


def frames(lines):
    for line in lines:
        match = re.search(r'frame=([0-9a-fA-F]+)', line)
        if not match:
            continue
        declared = re.search(r'\blen=(\d+)', line)
        if len(match[1]) % 2 or (declared and len(match[1]) != 2 * int(declared[1])):
            print('incomplete serial frame record skipped')
            continue
        raw = bytes.fromhex(match[1])
        header = wlan_header_len(raw)
        if len(raw) < header + 4 or header < 24:
            continue
        yield (raw[10:16], raw[4:10], raw[16:22],
               (raw[0] >> 2) & 3, raw[0] >> 4, raw[header:-4])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('-o', '--outdir', type=Path, default=Path('captures_out'))
    args = parser.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)
    decoder = Decoder()
    count = 0
    for frame in frames(args.log.read_text(encoding='utf-8').splitlines()):
        message = decoder.feed(frame)
        if message is None:
            continue
        base = args.outdir / f'{args.log.stem}-air-{count:02d}-{message.sender.hex()}'
        base.with_suffix('.bin').write_bytes(message.body)
        write_png(base.with_suffix('.png'), detile(message.bitmap))
        print(f'complete sender={message.sender.hex(":")} bytes={len(message.body)} '
              f'hash={checksum(message.body):08x} file={base.name}')
        count += 1
    for key, transfer in decoder.transfers.items():
        if transfer.emitted:
            continue
        print(f'pending source={key[0].hex(":")} slot={key[-1]} '
              f'covered={sum(transfer.covered)}/{len(transfer.data)} '
              f'final={transfer.final_seen} invalid={transfer.invalid}')
    print(f'complete={count} rejected={decoder.rejected}; '
          'missing sniffer data does not prove a DS receive failure')


if __name__ == '__main__':
    main()
