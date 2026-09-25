"""Reassemble the bounded PictoChat bitmap transfers observed in local captures.

Input is capture.parse_pcap's frame tuples, with capture FCS removed, or
application bytes passed directly to Decoder.feed_application().
Only complete, announced 03/02 message bodies are emitted. Missing packets are
not filled from other messages or other participants.
"""
from dataclasses import dataclass, field
from typing import Callable

CMD_GROUP = bytes.fromhex('0309bf000000')
REPLY_GROUP = bytes.fromhex('0309bf000010')
MESSAGE_HEADER = 36
MAX_BITMAP = 10240


@dataclass(frozen=True)
class Message:
    transport_source: bytes
    sender: bytes
    body: bytes
    chunks: int

    @property
    def bitmap(self):
        return self.body[MESSAGE_HEADER:]


@dataclass
class Transfer:
    announcement: bytes
    data: bytearray
    covered: bytearray
    final_seen: bool = False
    emitted: bool = False
    invalid: bool = False
    chunks: int = 0


@dataclass
class Decoder:
    transfers: dict = field(default_factory=dict)
    rejected: int = 0
    on_message: Callable[[Message], None] | None = None

    def feed(self, frame):
        src, dst, bssid, frame_type, subtype, payload = frame
        if frame_type != 2:
            return None
        if dst == CMD_GROUP and subtype == 2:
            prefix, trailer = 6, 4  # client time, grant mask, WM; host footer
        elif bssid == REPLY_GROUP and subtype == 1:
            prefix, trailer = 2, 2  # WM; client sequence footer
        else:
            return None
        if len(payload) < prefix + 4 + trailer:
            return None
        return self.feed_application(src, payload[prefix:-trailer], (dst, bssid))

    def feed_application(self, source, app, route=()):
        """Feed application bytes without WM headers/footers.

        source and route identify independent transport streams; neither is I/O.
        Returns a completed Message once or None. Malformed input increments rejected.
        """
        if len(app) < 4:
            self.rejected += 1
            return None
        kind = int.from_bytes(app[:2], 'little')
        size = int.from_bytes(app[2:4], 'little')
        if size != len(app):
            self.rejected += 1
            return None
        if kind not in (0, 1, 2) or len(app) < 12:
            return None
        key = (source, *route, app[4])
        if kind in (0, 1):
            if size != 20:
                self.rejected += 1
                return None
            total = int.from_bytes(app[8:10], 'little')
            bitmap_size = total - MESSAGE_HEADER
            # Observed bitmaps are 256 pixels wide, 4bpp, whole 8-pixel tile rows.
            if not (0 < bitmap_size <= MAX_BITMAP and bitmap_size % 1024 == 0):
                self.transfers.pop(key, None)
                return None
            previous = self.transfers.get(key)
            if previous is None or previous.announcement != app:
                self.transfers[key] = Transfer(app, bytearray(total), bytearray(total))
            return None
        transfer = self.transfers.get(key)
        if transfer is None or transfer.invalid or transfer.emitted:
            return None
        count = app[6]
        offset = int.from_bytes(app[8:10], 'little')
        end = offset + count
        if size != 12 + count or count == 0 or end > len(transfer.data):
            self.rejected += 1
            return None
        final = bool(app[7] & 1)
        if final and end != len(transfer.data):
            self.rejected += 1
            return None
        data = app[12:]  # WM + app header = 14 bytes in a client reply, not 16.
        for index, value in enumerate(data, offset):
            if transfer.covered[index] and transfer.data[index] != value:
                transfer.invalid = True
                self.rejected += 1
                return None
        transfer.data[offset:end] = data
        transfer.covered[offset:end] = b'\x01' * count
        transfer.chunks += 1
        transfer.final_seen |= final
        if not transfer.final_seen or not all(transfer.covered):
            return None
        transfer.emitted = True
        if transfer.data[:2] != b'\x03\x02':
            self.rejected += 1
            return None
        sender = bytes(transfer.data[i ^ 1] for i in range(2, 8))
        message = Message(source, sender, bytes(transfer.data), transfer.chunks)
        if self.on_message is not None:
            self.on_message(message)
        return message


def decode(frames, *, on_message=None):
    decoder = Decoder(on_message=on_message)
    for frame in frames:
        message = decoder.feed(frame)
        if message is not None:
            yield message
