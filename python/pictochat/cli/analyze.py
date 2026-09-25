#!/usr/bin/env python3
"""analyze.py — structural analysis of captured Nintendo DS / PictoChat frames.

Parses a libpcap file (radiotap + 802.11, as produced by this project) WITHOUT
any external deps, groups frames by destination "port" address, and reports:

  * talkers: which source/destination pairs exist and their payload sizes
  * a per-offset variability map for a chosen stream: which payload bytes are
    constant across frames (protocol scaffolding) vs. which change (the data)
  * sample hexdumps

Diff mode compares two captures (e.g. "idle room" vs. "after I drew something")
and prints exactly which payload offsets changed — the fast path to locating the
message bitmap inside the frame.

Usage:
  python analyze.py capture.pcap
  python analyze.py capture.pcap --dst 03:09:bf:00:00:00
  python analyze.py --diff idle.pcap drawing.pcap --dst 03:09:bf:00:00:00
"""

import argparse
import struct
from collections import defaultdict, Counter

from pictochat.capture import mac, parse_pcap


def collect(path):
    frames = list(parse_pcap(path))
    return frames


def talkers(frames):
    print("=== talkers: (source -> destination)  count  payload len min/med/max ===")
    groups = defaultdict(list)
    for src, dst, _bssid, _t, _s, pl in frames:
        groups[(mac(src), mac(dst))].append(len(pl))
    for (s, d), lens in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        lens.sort()
        med = lens[len(lens) // 2]
        print(f"  {s} -> {d}   n={len(lens):5d}   len {lens[0]}/{med}/{lens[-1]}")


def variability(frames, dst_filter, maxcols=200, force_len=None):
    sel = [pl for src, dst, _b, _t, _s, pl in frames
           if dst_filter is None or mac(dst) == dst_filter]
    if not sel:
        print(f"\nNo frames to {dst_filter}. Try one from the talkers list.")
        return
    label = dst_filter if dst_filter else "all destinations"
    print(f"\n=== stream {label}: {len(sel)} frames ===")
    lenhist = Counter(len(p) for p in sel)
    print("payload-length histogram:",
          ", ".join(f"{l}B x{c}" for l, c in sorted(lenhist.items())))

    # Restrict the column analysis to one length so short keepalive frames don't
    # blank out every offset as "absent in some".
    chosen = force_len if force_len is not None else lenhist.most_common(1)[0][0]
    sel = [p for p in sel if len(p) == chosen]
    print(f"(analyzing the {len(sel)} frames of length {chosen}B)")
    if not sel:
        print("no frames of that length")
        return

    width = min(maxcols, max(len(p) for p in sel))
    print(f"\nper-offset variability (first {width} bytes):")
    print("  '.' = constant across all frames   'X' = varies   ' ' = absent in some")
    distinct = []
    for off in range(width):
        vals = set()
        present = 0
        for p in sel:
            if off < len(p):
                vals.add(p[off])
                present += 1
        distinct.append((len(vals), present, len(sel)))
    # Print in rows of 16 with offset labels.
    for base in range(0, width, 16):
        row = ""
        for off in range(base, min(base + 16, width)):
            nd, present, total = distinct[off]
            if present < total:
                row += " "
            elif nd == 1:
                row += "."
            else:
                row += "X"
        print(f"  {base:04d}: {row}")

    # Show the constant bytes (likely header/framing) from the shortest frame.
    shortest = min(sel, key=len)
    print("\nconstant-byte template (value where byte is constant, '..' where it varies):")
    tmpl = ""
    for off in range(min(width, len(shortest))):
        nd, present, total = distinct[off]
        if present == total and nd == 1:
            tmpl += f"{shortest[off]:02x} "
        else:
            tmpl += ".. "
        if (off + 1) % 16 == 0:
            tmpl += "\n"
    print(tmpl)

    print("sample frames (first 48 bytes):")
    for p in sel[:3]:
        print("  " + " ".join(f"{b:02x}" for b in p[:48]))


def diff(path_a, path_b, dst_filter):
    a = [pl for src, dst, _b, _t, _s, pl in collect(path_a) if mac(dst) == dst_filter]
    b = [pl for src, dst, _b, _t, _s, pl in collect(path_b) if mac(dst) == dst_filter]
    if not a or not b:
        raise SystemExit("one of the captures has no frames to that destination")
    # Union of byte values seen at each offset in each capture.
    def valset(frames):
        m = defaultdict(set)
        for p in frames:
            for off, byte in enumerate(p):
                m[off].add(byte)
        return m
    va, vb = valset(a), valset(b)
    width = max(max(va), max(vb)) + 1
    print(f"=== diff {dst_filter}: {path_a}({len(a)}) vs {path_b}({len(b)}) ===")
    print("offsets whose byte values differ between the two captures:")
    changed = []
    for off in range(width):
        sa, sb = va.get(off, set()), vb.get(off, set())
        # "Changed" = a value appears in B that never appeared in A (new content).
        new_in_b = sb - sa
        if new_in_b and sa:
            changed.append(off)
    if not changed:
        print("  (none — captures look identical at the byte level)")
    else:
        # Group consecutive offsets into ranges for readability.
        ranges = []
        start = prev = changed[0]
        for off in changed[1:]:
            if off == prev + 1:
                prev = off
            else:
                ranges.append((start, prev))
                start = prev = off
        ranges.append((start, prev))
        for s, e in ranges:
            span = f"{s}" if s == e else f"{s}-{e}"
            print(f"  offset {span:>9}  ({e - s + 1} byte(s))")
        print(f"\n{len(changed)} changed offsets in {len(ranges)} contiguous region(s).")
        print("These regions are your prime candidates for the message/drawing data.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap", nargs="?", help="capture to analyze")
    ap.add_argument("--dst", help="destination MAC to focus the analysis on")
    ap.add_argument("--len", type=int, dest="length",
                    help="only analyze frames whose payload is exactly this many bytes")
    ap.add_argument("--diff", nargs=2, metavar=("A", "B"),
                    help="compare two captures at --dst")
    args = ap.parse_args()

    if args.diff:
        if not args.dst:
            raise SystemExit("--diff requires --dst")
        diff(args.diff[0], args.diff[1], args.dst.lower())
        return

    if not args.pcap:
        raise SystemExit("give a pcap file, or use --diff A B --dst ...")

    frames = collect(args.pcap)
    print(f"parsed {len(frames)} frames from {args.pcap}\n")
    if not frames:
        return
    talkers(frames)
    if args.dst or args.length:
        variability(frames, args.dst.lower() if args.dst else None,
                    force_len=args.length)
    else:
        # Auto-pick the busiest large-payload stream.
        groups = defaultdict(list)
        for src, dst, _b, _t, _s, pl in frames:
            groups[mac(dst)].append(len(pl))
        big = max(groups, key=lambda d: max(groups[d]) * len(groups[d]))
        print(f"\n(no --dst given; auto-analyzing busiest large stream: {big})")
        variability(frames, big)


if __name__ == "__main__":
    main()
