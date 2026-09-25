"""Decode 4bpp tile rows and write monochrome PNG images."""
import struct
import zlib

TILES_WIDE = 32
TILE_BYTES = 32


def detile(buf):
    if not buf or len(buf) > 10240 or len(buf) % 1024:
        raise ValueError("bitmap must contain 1..10 complete tile rows")
    ntiles = len(buf) // TILE_BYTES
    tall = ntiles // TILES_WIDE
    W, H = TILES_WIDE * 8, tall * 8
    grid = [[255] * W for _ in range(H)]
    for t in range(TILES_WIDE * tall):
        tx, ty, base = t % TILES_WIDE, t // TILES_WIDE, t * TILE_BYTES
        for py in range(8):
            for bx in range(4):
                byte = buf[base + py * 4 + bx]
                if byte & 0x0F:
                    grid[ty * 8 + py][tx * 8 + bx * 2] = 0
                if byte & 0xF0:
                    grid[ty * 8 + py][tx * 8 + bx * 2 + 1] = 0
    return grid


def write_png(path, grid, scale=4):
    if not isinstance(scale, int) or scale < 1 or not grid or not grid[0]:
        raise ValueError("nonempty grid and positive integer scale required")
    h, w = len(grid), len(grid[0])
    if any(len(row) != w for row in grid):
        raise ValueError("grid must be rectangular")
    W, H = w * scale, h * scale
    raw = bytearray()
    for y in range(H):
        raw.append(0); srow = grid[y // scale]
        for x in range(W):
            raw.append(srow[x // scale])
    def ch(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    data = b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0))
    data += ch(b'IDAT', zlib.compress(bytes(raw), 9)) + ch(b'IEND', b'')
    with open(path, 'wb') as output:
        output.write(data)


def ink(buf):
    return sum(1 for b in buf if b & 0x0F) + sum(1 for b in buf if b & 0xF0)


