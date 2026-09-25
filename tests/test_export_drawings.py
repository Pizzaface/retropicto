import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'python'))
from pictochat.serial import checksum, read_drawings


class DrawingDumpTests(unittest.TestCase):
    def setUp(self):
        self.body = (ROOT / 'tests/fixtures/send-message.bin').read_bytes()
        self.lines = [f'DRAW BEGIN id=1 len={len(self.body)} hash={checksum(self.body):08x}']
        self.lines += [f'DRAW DATA id=1 offset={i} hex={self.body[i:i+64].hex()}'
                       for i in range(0, len(self.body), 64)]
        self.lines.append('DRAW END id=1')

    def test_exact_drawing_roundtrip(self):
        self.assertEqual(list(read_drawings(self.lines)), [(1, self.body)])

    def test_missing_chunk_rejected(self):
        with self.assertRaises(ValueError):
            list(read_drawings(self.lines[:2] + self.lines[3:]))

    def test_corrupt_dump_rejected(self):
        self.lines[0] = self.lines[0][:-1] + ('1' if self.lines[0][-1] == '0' else '0')
        with self.assertRaises(ValueError):
            list(read_drawings(self.lines))

    def test_missing_end_rejected(self):
        with self.assertRaises(ValueError):
            list(read_drawings(self.lines[:-1]))

    def test_reused_id_after_reboot_keeps_both_messages(self):
        self.assertEqual(len(list(read_drawings(self.lines + self.lines))), 2)


if __name__ == '__main__':
    unittest.main()
