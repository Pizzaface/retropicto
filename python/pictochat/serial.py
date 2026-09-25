"""Validate complete DRAW records from firmware logs."""
import re

def checksum(body):
    value = 2166136261
    for byte in body:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def read_drawings(lines):
    active = {}
    for line in lines:
        begin = re.search(r'DRAW BEGIN id=(\d+) len=(\d+) hash=([0-9a-fA-F]+)', line)
        data = re.search(r'DRAW DATA id=(\d+) offset=(\d+) hex=([0-9a-fA-F]+)', line)
        end = re.search(r'DRAW END id=(\d+)', line)
        if begin:
            ident, length, digest = int(begin[1]), int(begin[2]), int(begin[3], 16)
            if not (36 < length <= 10276 and (length - 36) % 1024 == 0):
                raise ValueError(f'drawing {ident}: invalid bitmap length {length}')
            if ident in active:
                raise ValueError(f'drawing {ident}: repeated BEGIN before END')
            active[ident] = (bytearray(length), bytearray(length), digest)
        elif data:
            ident, offset = int(data[1]), int(data[2])
            if ident not in active:
                raise ValueError(f'drawing {ident}: data without BEGIN')
            body, seen, _ = active[ident]
            chunk = bytes.fromhex(data[3])
            if offset + len(chunk) > len(body):
                raise ValueError(f'drawing {ident}: data outside announced length')
            for index, byte in enumerate(chunk, offset):
                if seen[index] and body[index] != byte:
                    raise ValueError(f'drawing {ident}: conflicting serial data')
                body[index], seen[index] = byte, 1
        elif end:
            ident = int(end[1])
            if ident not in active:
                raise ValueError(f'drawing {ident}: END without BEGIN')
            body, seen, digest = active.pop(ident)
            if not all(seen) or checksum(body) != digest or body[:2] != b'\x03\x02':
                raise ValueError(f'drawing {ident}: incomplete or corrupt dump')
            yield ident, bytes(body)
    if active:
        raise ValueError(f'unfinished drawing dumps: {sorted(active)}')


