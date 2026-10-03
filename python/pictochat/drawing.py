"""Build host-side PictoChat payloads: 84-byte profiles, drawing message bodies,
20-byte announcements and PCTR v2 state/drawing payload bodies.

Byte layouts come from captured DS traffic (tests/host_identity_fixture.h,
tests/fixtures/). The 36-byte message header is the full-height (256x80)
template that DS consoles are known to display; its bytes 8..21 are not yet
decoded, so smaller canvases need a captured template of their own.
"""
import struct

PROFILE_SIZE = 84
NAME_OFFSET, NAME_UNITS = 8, 10
BIO_OFFSET, BIO_UNITS = 28, 26
COLOUR_OFFSET, BIRTHDAY_OFFSET = 80, 82
MESSAGE_HEADER = 36
FULL_BITMAP = 10240
# Full-height header captured from a real DS send; sender MAC (bytes 2..7) is filled in.
FULL_HEADER = bytes.fromhex('03020000000000000005000000000306080d080d121b0000000000000000000000000000')


def swap_mac(mac):
    """ConsoleId/message sender order: each 16-bit half swapped."""
    mac = bytes(mac)
    if len(mac) != 6:
        raise ValueError("MAC must be 6 bytes")
    return bytes(mac[i ^ 1] for i in range(6))


def _utf16(text, units):
    if len(text) > units:
        raise ValueError(f"text exceeds {units} UTF-16 units")
    data = text.encode('utf-16le')
    if len(data) != 2 * len(text):
        raise ValueError("text must fit in single UTF-16 code units")
    return data.ljust(2 * units, b'\0')


def profile(mac, name, bio='', colour=11, birthday=(7, 16), stage=0):
    """84-byte identity: type 3, stage, swapped MAC, name, bio, favourite colour 0..15, birthday."""
    if not 0 <= colour <= 15 or stage not in (0, 1):
        raise ValueError("colour must be 0..15 and stage 0 or 1")
    month, day = birthday
    if not (1 <= month <= 12 and 1 <= day <= 31):
        raise ValueError("birthday must be (month, day)")
    return (bytes([3, stage]) + swap_mac(mac) + _utf16(name, NAME_UNITS) + _utf16(bio, BIO_UNITS)
            + bytes([colour, 0, month, day]))


def message_body(bitmap, mac, template=FULL_HEADER):
    """36-byte header plus 4bpp tile rows. The default template requires a full-height bitmap."""
    bitmap = bytes(bitmap)
    if len(template) != MESSAGE_HEADER or template[:2] != b'\x03\x02':
        raise ValueError("template must be a 36-byte 03 02 message header")
    if template == FULL_HEADER and len(bitmap) != FULL_BITMAP:
        raise ValueError("full-height template needs a 10240-byte bitmap")
    if not bitmap or len(bitmap) > FULL_BITMAP or len(bitmap) % 1024:
        raise ValueError("bitmap must contain 1..10 complete tile rows")
    return template[:2] + swap_mac(mac) + template[8:] + bitmap


def announcement(total, slot=1, token=0):
    """Client type-0 transfer announcement for a message body of `total` bytes."""
    if not 1 <= slot <= 15 or not MESSAGE_HEADER < total <= MESSAGE_HEADER + FULL_BITMAP:
        raise ValueError("slot must be 1..15 and total a supported body size")
    return struct.pack('<HHBBHHHII', 0, 20, slot, 0, 0xffff, total, 0, 0, token & 0xffffffff)


def state_payload(boot, generation, identity):
    """PCTR state body (92 bytes): boot id, generation, 84-byte profile."""
    if len(identity) != PROFILE_SIZE:
        raise ValueError("identity must be 84 bytes")
    return struct.pack('<II', boot, generation) + bytes(identity)


def drawing_payload(boot, generation, body, slot=1, token=0):
    """PCTR drawing body: boot id, generation, announcement, message body."""
    if len(body) < MESSAGE_HEADER + 1024 or body[:2] != b'\x03\x02':
        raise ValueError("body must be a message body from message_body()")
    return struct.pack('<II', boot, generation) + announcement(len(body), slot, token) + bytes(body)


def drawing_bitmap(payload):
    """Tile rows carried by a PCTR drawing payload, or None if the layout is wrong."""
    if len(payload) < 8 + 20 + MESSAGE_HEADER + 1024 or (len(payload) - 64) % 1024:
        return None
    ann, body = payload[8:28], payload[28:]
    if ann[:4] != b'\x00\x00\x14\x00' or not 1 <= ann[4] <= 15 or ann[10:12] != b'\0\0':
        return None
    if struct.unpack_from('<H', ann, 8)[0] != len(body) or body[:2] != b'\x03\x02':
        return None
    return body[MESSAGE_HEADER:]
