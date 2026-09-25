import pathlib
import sys
import unittest
sys.path.insert(0,str(pathlib.Path(__file__).parents[1]/'tools'))
from ble_wire import Assembly, fragments, MAX_PACKET
from usb_bridge import encode

def packet(rows=10):
    return bytes.fromhex(encode(2,7,bytes((i%251 for i in range(64+1024*rows))))[6:].decode().strip())

class BLETests(unittest.TestCase):
    def test_all_sizes_and_negotiated_mtus(self):
        for rows in range(1,11):
            raw=packet(rows)
            for mtu in (23,64,185,247,517):
                rx=Assembly(); got=[]
                for f in fragments(raw,mtu):
                    self.assertLessEqual(len(f),min(mtu-3,244))
                    result=rx.feed(f)
                    if result is not None: got.append(result)
                self.assertEqual(got,[raw])
    def test_loss_corruption_and_restart(self):
        raw=packet(); chunks=list(fragments(raw)); rx=Assembly()
        self.assertIsNone(rx.feed(chunks[0]))
        for c in chunks[2:]: self.assertIsNone(rx.feed(c))
        results=[rx.feed(c) for c in chunks]
        self.assertEqual(results[-1],raw)
        bad=bytearray(chunks[-1]); bad[-1]^=1
        for c in chunks[:-1]: rx.feed(c)
        self.assertIsNone(rx.feed(bad))
        self.assertIsNone(rx.feed(b'\0\0\xff\xffx'))
        self.assertLessEqual(len(rx.data),MAX_PACKET)
        self.assertEqual([rx.feed(c) for c in chunks][-1],raw)

if __name__=='__main__': unittest.main()
