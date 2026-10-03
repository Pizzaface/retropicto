import pathlib
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'python'))
sys.path.insert(0, str(ROOT / 'tools'))
from pictochat import canvas, drawing
from usb_bridge import decode, encode

FIXTURES = ROOT / 'tests/fixtures'
MAC = bytes.fromhex('0022d739bca3')


class CanvasTests(unittest.TestCase):
    def test_tile_detile_round_trip_keeps_colour(self):
        grid = [[(x * 7 + y) % 16 for x in range(256)] for y in range(24)]
        tiles = canvas.tile(grid)
        self.assertEqual(len(tiles), 3 * 1024)
        self.assertEqual(canvas.detile_indices(tiles), grid)
        # Low nibble is the left pixel.
        single = [[0] * 256 for _ in range(8)]
        single[0][0], single[0][1] = 1, 2
        self.assertEqual(canvas.tile(single)[:1], b'\x21')

    def test_captured_message_round_trips(self):
        bitmap = (FIXTURES / 'send-message.bin').read_bytes()[36:]
        self.assertEqual(canvas.tile(canvas.detile_indices(bitmap)), bitmap)
        mono = canvas.detile(bitmap)
        self.assertEqual([[0 if v else 255 for v in row] for row in canvas.detile_indices(bitmap)], mono)

    def test_tile_rejects_bad_grids(self):
        for grid in ([], [[0] * 255] * 8, [[0] * 256] * 7, [[0] * 256] * 88, [[16] + [0] * 255] * 8):
            with self.assertRaises(ValueError):
                canvas.tile(grid)

    def test_indexed_png(self):
        grid = [[(x // 16) % 16 for x in range(256)] for y in range(8)]
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'c.png'
            canvas.write_png(path, grid, scale=1, palette=canvas.PALETTE)
            data = path.read_bytes()
        self.assertTrue(data.startswith(b'\x89PNG'))
        width, height, depth, colour_type = struct.unpack_from('>IIBB', data, 16)
        self.assertEqual((width, height, depth, colour_type), (256, 8, 8, 3))
        plte = data.index(b'PLTE')
        self.assertEqual(data[plte + 4:plte + 4 + 48], b''.join(canvas.PALETTE))
        idat = data.index(b'IDAT')
        size = struct.unpack_from('>I', data, idat - 4)[0]
        raw = zlib.decompress(data[idat + 4:idat + 4 + size])
        self.assertEqual(list(raw[1:257]), grid[0])


class DrawingTests(unittest.TestCase):
    def test_profile_matches_captured_layout(self):
        captured = bytes(_own_data0())[12:]
        built = drawing.profile(MAC, 'Jordan', 'Can you', colour=11, birthday=(7, 16))
        self.assertEqual(built, captured)
        self.assertEqual(drawing.profile(MAC, 'Jordan', 'Can you', stage=1)[1], 1)
        self.assertEqual(drawing.profile(MAC, 'A\u00e9')[8:12], b'A\0\xe9\0')
        for bad in (dict(name='X' * 11), dict(name='x', bio='B' * 27), dict(name='x', colour=16),
                    dict(name='x', birthday=(13, 1)), dict(name='\U0001f600')):
            with self.assertRaises(ValueError):
                drawing.profile(MAC, **bad)

    def test_message_body_uses_shared_header_fixture(self):
        header = (FIXTURES / 'drawing-header-full.bin').read_bytes()
        self.assertEqual(drawing.FULL_HEADER, header)
        bitmap = bytes(range(256)) * 40
        body = drawing.message_body(bitmap, MAC)
        self.assertEqual(len(body), 10276)
        self.assertEqual(body[:2], b'\x03\x02')
        self.assertEqual(body[2:8], bytes.fromhex('2200 39d7 a3bc'))
        self.assertEqual(body[8:36], header[8:])
        self.assertEqual(body[36:], bitmap)
        with self.assertRaises(ValueError):
            drawing.message_body(bytes(1024), MAC)  # full-height template only
        captured = (FIXTURES / 'send-message.bin').read_bytes()
        self.assertEqual(drawing.message_body(captured[36:], MAC, template=captured[:36]), captured)

    def test_announcement_matches_capture(self):
        self.assertEqual(drawing.announcement(2084, slot=1, token=0x5a4e49b5),
                         bytes.fromhex('000014000100ffff2408000000000000b5494e5a'))

    def test_relay_payloads_encode_and_decode(self):
        identity = drawing.profile(MAC, 'WEB', 'inbox', colour=2)
        state = drawing.state_payload(0x01020304, 1, identity)
        self.assertEqual(len(state), 92)
        self.assertEqual(decode(encode(1, 0, state, 1, 0))[2], state)
        grid = [[0] * 256 for _ in range(80)]
        grid[79][255] = 5
        bitmap = canvas.tile(grid)
        payload = drawing.drawing_payload(7, 1, drawing.message_body(bitmap, MAC), slot=1, token=9)
        kind, seq, body, sender, recipient = decode(encode(2, 3, payload, 1, 0))
        self.assertEqual((kind, seq, sender, recipient), (2, 3, 1, 0))
        self.assertEqual(drawing.drawing_bitmap(body), bitmap)
        self.assertEqual(canvas.detile_indices(drawing.drawing_bitmap(body))[79][255], 5)
        self.assertIsNone(drawing.drawing_bitmap(body[:-1]))
        self.assertIsNone(drawing.drawing_bitmap(b'\0' * len(body)))


def _own_data0():
    text = (ROOT / 'tests/host_identity_fixture.h').read_text()
    line = next(l for l in text.splitlines() if l.startswith('static const uint8_t own_data0[]'))
    return [int(v, 16) for v in line[line.index('{') + 1:line.index('}')].split(',')]


if __name__ == '__main__':
    unittest.main()
