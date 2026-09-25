"""Read radiotap captures into protocol decoder frame tuples."""
import struct

def mac(b):
    return ":".join(f"{x:02x}" for x in b)


def wlan_header_len(f):
    """802.11 MAC header length, accounting for addr4 (ToDS&FromDS) + QoS."""
    if len(f) < 24:
        return len(f)
    fc0, fc1 = f[0], f[1]
    ftype = (fc0 >> 2) & 0x3
    subtype = (fc0 >> 4) & 0xF
    hdr = 24
    if ftype == 2 and (fc1 & 0x01) and (fc1 & 0x02):
        hdr += 6  # addr4
    if ftype == 2 and (subtype & 0x08):
        hdr += 2  # QoS control
    return hdr


def parse_pcap(path):
    """Yield (src, dst, bssid, type, subtype, payload) from classic PCAP.

    Accepts radiotap (127) and raw 802.11 (105), either byte order and micro/nano
    timestamps. Radiotap's Flags field controls FCS stripping; raw frames have
    no implied FCS. Invalid/truncated records raise ValueError, never exit().
    """
    with open(path, "rb") as fh:
        header = fh.read(24)
        if len(header) != 24:
            raise ValueError("truncated PCAP header")
        orders = {b"\xd4\xc3\xb2\xa1": "<", b"\xa1\xb2\xc3\xd4": ">",
                  b"\x4d\x3c\xb2\xa1": "<", b"\xa1\xb2\x3c\x4d": ">"}
        order = orders.get(header[:4])
        if order is None:
            raise ValueError("not a classic PCAP file")
        major, minor, _, _, snaplen, link = struct.unpack(order + "HHIIII", header[4:])
        if (major, minor) != (2, 4) or link not in (105, 127):
            raise ValueError("expected PCAP 2.4 with raw 802.11 or radiotap frames")
        while True:
            record = fh.read(16)
            if not record:
                return
            if len(record) != 16:
                raise ValueError("truncated PCAP record header")
            _, _, incl, original = struct.unpack(order + "IIII", record)
            if incl > snaplen or incl > original or incl > 16 * 1024 * 1024:
                raise ValueError("invalid PCAP record length")
            packet = fh.read(incl)
            if len(packet) != incl:
                raise ValueError("truncated PCAP record")
            has_fcs = False
            if link == 127:
                if len(packet) < 8 or packet[0] != 0:
                    raise ValueError("invalid radiotap header")
                rt_len, present = struct.unpack_from("<HI", packet, 2)
                if rt_len < 8 or rt_len > len(packet):
                    raise ValueError("invalid radiotap length")
                # Locate the field area after all extended presence words.
                field = 8
                word = present
                while word & (1 << 31):
                    if field + 4 > rt_len:
                        raise ValueError("truncated radiotap presence bitmap")
                    word = struct.unpack_from("<I", packet, field)[0]
                    field += 4
                if present & 1:  # TSFT aligns to 8 bytes from radiotap start
                    field = (field + 7) & ~7
                    field += 8
                if field > rt_len:
                    raise ValueError("truncated radiotap TSFT")
                if present & 2:
                    if field >= rt_len:
                        raise ValueError("truncated radiotap Flags")
                    has_fcs = bool(packet[field] & 0x10)
                packet = packet[rt_len:]
            if has_fcs:
                if len(packet) < 4:
                    raise ValueError("missing frame check sequence")
                packet = packet[:-4]
            if len(packet) < 24 or ((packet[0] >> 2) & 3) not in (0, 2):
                continue
            size = wlan_header_len(packet)
            if size > len(packet):
                continue
            yield (packet[10:16], packet[4:10], packet[16:22],
                   (packet[0] >> 2) & 3, packet[0] >> 4, packet[size:])
