#!/usr/bin/env python3
"""Check standalone echo telemetry against its drawing size and lifecycle.

Usage: python tools/check_echo_timing.py captures_out/echo-D-timing-*.log
Run on a single boot's capture after at least one echo completes.
"""
import re
import sys
from pathlib import Path


def check(text):
    sizes, stages = {}, {}
    completed = 0
    for line in text.splitlines():
        if "RX REPEAT " in line:
            counts = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
            assert counts["same_sequence"] <= counts["same_payload"] < counts["packets"], counts
            assert counts["announcements"] + counts["chunks"] == counts["packets"], counts
        received = re.search(r"DRAW received=(\d+) aid=\d+ len=(\d+)", line)
        if received:
            sizes[int(received[1])] = int(received[2])
        event = re.search(r"ECHO (queued|start|done) (.*)", line)
        if not event:
            continue
        values = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", event[2])}
        key = values["id"], values["aid"]
        stage = event[1]
        previous = stages.get(key)
        assert previous == {"queued": None, "start": "queued", "done": "start"}[stage], (key, previous, stage)
        stages[key] = stage
        if stage != "done":
            continue
        assert all(v >= 0 for v in values.values()), values
        # Each announcement and 160-byte fragment requires three delivered copies.
        expected = 3 * (1 + (sizes[values["id"]] + 159) // 160)
        assert values["delivered"] == expected, (values, expected)
        parts = sum(values[k] for k in ("queue_ms", "start_wait_ms", "transfer_ms"))
        assert 0 <= values["total_ms"] - parts <= 2, values
        completed += 1
        print(event[0])
    assert completed, "No completed timing records; capture an echo first"
    print(f"Checked {completed} completed recipient echoes")


if __name__ == "__main__":
    check(Path(sys.argv[1]).read_text())
