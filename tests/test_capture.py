import pathlib
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'python'))
from pictochat.capture import parse_pcap
from pictochat.canvas import detile

class CaptureTests(unittest.TestCase):
    def parse(self, packet, order='<', link=127):
        data = struct.pack(order+'IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, link)
        data += struct.pack(order+'IIII', 0, 0, len(packet), len(packet)) + packet
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'sample.pcap'
            path.write_bytes(data)
            return list(parse_pcap(path))

    def test_byte_order_and_fcs_flag(self):
        frame = bytes([0x18, 2]) + bytes(22) + b'payload'
        for order in ('<', '>'):
            for fcs in (False, True):
                rt = struct.pack('<BBHI', 0, 0, 9, 2) + bytes([0x10 if fcs else 0])
                packet = rt + frame + (b'FCS!' if fcs else b'')
                self.assertEqual(self.parse(packet, order)[0][-1], b'payload')
        self.assertEqual(self.parse(frame, link=105)[0][-1], b'payload')

    def test_extended_presence_and_tsft_alignment(self):
        rt = struct.pack('<BBHII', 0, 0, 25, 0x80000003, 0) + bytes(12) + b'\x10'
        frame = bytes([0x18, 2]) + bytes(22) + b'payloadFCS!'
        self.assertEqual(self.parse(rt + frame)[0][-1], b'payload')

    def test_bad_radiotap_rejected(self):
        for packet in (bytes(3), struct.pack('<BBHI', 0, 0, 999, 0),
                       struct.pack('<BBHI', 0, 0, 8, 2)):
            with self.assertRaises(ValueError):
                self.parse(packet)

    def test_partial_bitmap_rejected(self):
        for bitmap in (b'', bytes(1023), bytes(10241)):
            with self.assertRaises(ValueError):
                detile(bitmap)
        self.assertEqual(len(detile(bytes(1024))), 8)

if __name__ == '__main__':
    unittest.main()
