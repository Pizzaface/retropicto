"""Binary PCTR over BLE fragments; shared contract for future phone clients."""
import struct
from usb_bridge import decode, PREFIX
SERVICE = '50494354-0000-0080-5049-505100000001'
RX = '50494354-0000-0080-5049-505100000002'
TX = '50494354-0000-0080-5049-505100000003'
MAX_PACKET = 10324

def valid_packet(data):
    return decode(PREFIX + bytes(data).hex().encode()) is not None

def fragments(data, mtu=247):
    if not valid_packet(data):
        raise ValueError('Invalid relay packet')
    chunk = min(mtu - 7, 240)
    if chunk < 16:
        raise ValueError('BLE MTU below 23')
    for offset in range(0, len(data), chunk):
        yield struct.pack('<HH', offset, len(data)) + data[offset:offset + chunk]

class Assembly:
    def __init__(self):
        self.data = bytearray()
        self.total = 0

    def feed(self, fragment):
        if not 5 <= len(fragment) <= 244:
            self.data.clear(); self.total = 0
            return None
        offset, total = struct.unpack_from('<HH', fragment)
        if offset == 0:
            self.data.clear(); self.total = total
        if not 20 <= total <= MAX_PACKET or total != self.total or offset != len(self.data) or offset + len(fragment) - 4 > total:
            self.data.clear(); self.total = 0
            return None
        self.data.extend(fragment[4:])
        if len(self.data) != total:
            return None
        packet = bytes(self.data)
        self.data.clear(); self.total = 0
        return packet if valid_packet(packet) else None
