#!/usr/bin/env python3
"""udp_to_wireshark.py — bridge the ESP32's PCAP-over-UDP stream into Wireshark.

The ESP32 (STREAM mode) broadcasts, per captured frame, one UDP datagram whose
payload is a complete libpcap record: a 16-byte record header followed by a
radiotap + 802.11 frame. This script writes the libpcap *global* header once,
then forwards each datagram's bytes, producing a valid libpcap stream.

Two ways to use it (Windows paths shown; adjust for your Wireshark install):

  Live into Wireshark (recommended):
      python udp_to_wireshark.py | "C:\\Program Files\\Wireshark\\Wireshark.exe" -k -i -

  Save to a file you can open later:
      python udp_to_wireshark.py --out capture.pcap

Both require your PC to be joined to the ESP32's SoftAP ("pictochat-sniffer").
"""

import argparse
import socket
import struct
import sys

# Must match PCAP_LINKTYPE in the firmware (127 = radiotap).
LINKTYPE = 127
GLOBAL_HEADER = struct.pack(
    "<IHHiIII",
    0xA1B2C3D4,  # magic (little-endian)
    2, 4,        # version major, minor
    0,           # thiszone
    0,           # sigfigs
    65535,       # snaplen
    LINKTYPE,    # network
)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=5555,
                    help="UDP port to listen on (default 5555)")
    ap.add_argument("--bind", default="0.0.0.0",
                    help="local address to bind (default all interfaces)")
    ap.add_argument("--out", default=None,
                    help="write to this .pcap file instead of stdout")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))

    if args.out:
        out = open(args.out, "wb")
        log = sys.stderr
    else:
        # Binary stdout so the pipe into Wireshark stays byte-exact.
        out = sys.stdout.buffer
        log = sys.stderr

    out.write(GLOBAL_HEADER)
    out.flush()
    print(f"[udp_to_wireshark] listening on {args.bind}:{args.port}, "
          f"linktype {LINKTYPE}", file=log)

    count = 0
    try:
        while True:
            data, _addr = sock.recvfrom(65535)
            # Each datagram is already a full libpcap record — forward as-is.
            out.write(data)
            out.flush()
            count += 1
            if count % 50 == 0:
                print(f"[udp_to_wireshark] forwarded {count} frames", file=log)
    except KeyboardInterrupt:
        print(f"\n[udp_to_wireshark] stopped after {count} frames", file=log)
    finally:
        if args.out:
            out.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
