"""Convert 4bpp tile rows to and from pixel grids and write PNG images.

Pixels are palette indices 0..15: zero is blank, nonzero is ink. `detile`
keeps the historical monochrome 255/0 grid; `detile_indices`/`tile` round-trip
the indices themselves so colour survives. PALETTE is the rendering used by the
retropic.to public inbox; it is a display choice, not a DS-verified table.
"""
import struct
import zlib

TILES_WIDE = 32
TILE_BYTES = 32
WIDTH = TILES_WIDE * 8
MAX_BITMAP = 10240
PALETTE = tuple(bytes.fromhex(h) for h in (
    'ffffff', '000000', 'ffffff', 'e060d0', 'f050a0', 'f02040', 'f07020', 'f0a020',
    'f0e020', 'a0e020', '30c030', '20e0c0', '20c0f0', '2090f0', '3060e0', '5050d0'))


def detile_indices(buf):
    """4bpp tile rows -> rows of palette indices (low nibble is the left pixel)."""
    if not buf or len(buf) > MAX_BITMAP or len(buf) % 1024:
        raise ValueError("bitmap must contain 1..10 complete tile rows")
    tall = len(buf) // 1024
    grid = [[0] * WIDTH for _ in range(tall * 8)]
    for t in range(TILES_WIDE * tall):
        tx, ty, base = t % TILES_WIDE, t // TILES_WIDE, t * TILE_BYTES
        for py in range(8):
            row = grid[ty * 8 + py]
            for bx in range(4):
                byte = buf[base + py * 4 + bx]
                row[tx * 8 + bx * 2] = byte & 0x0F
                row[tx * 8 + bx * 2 + 1] = byte >> 4
    return grid


def detile(buf):
    """Monochrome 8-bit grey grid: 255 blank, 0 ink."""
    return [[0 if v else 255 for v in row] for row in detile_indices(buf)]


def tile(grid):
    """Rows of palette indices (256 wide, 8..80 tall in whole tiles) -> 4bpp tile rows."""
    h = len(grid)
    if not h or h % 8 or h > 80 or any(len(row) != WIDTH for row in grid):
        raise ValueError("grid must be 256 pixels wide with 1..10 complete 8-pixel rows")
    out = bytearray(h * 128)
    for y, row in enumerate(grid):
        for x, v in enumerate(row):
            if not 0 <= v <= 15:
                raise ValueError("palette indices must be 0..15")
            if v:
                at = ((y >> 3) * TILES_WIDE + (x >> 3)) * TILE_BYTES + (y & 7) * 4 + ((x & 7) >> 1)
                out[at] |= v << (4 * (x & 1))
    return bytes(out)


def write_png(path, grid, scale=4, palette=None):
    """Write an 8-bit grey PNG, or an indexed-colour PNG when palette (RGB bytes per index) is given."""
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
    colour_type = 3 if palette is not None else 0
    data = b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, colour_type, 0, 0, 0))
    if palette is not None:
        data += ch(b'PLTE', b''.join(palette))
    data += ch(b'IDAT', zlib.compress(bytes(raw), 9)) + ch(b'IEND', b'')
    with open(path, 'wb') as output:
        output.write(data)


def ink(buf):
    return sum(1 for b in buf if b & 0x0F) + sum(1 for b in buf if b & 0xF0)
