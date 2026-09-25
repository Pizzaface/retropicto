#!/usr/bin/env python3
"""Verify and flash a bundled RetroPicto Multi-Hop image set safely."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "firmware" / "prebuilt" / "manifest.json"
PORT_SIZE = re.compile(r"\((\d+)\s*MB\)|Detected flash size:\s*(\d+)\s*MB", re.I)


def load_entry(board: str, role: str) -> tuple[dict, Path]:
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    matches = [item for item in data["entries"] if item["board_key"] == board and item["role"] == role]
    if len(matches) != 1:
        raise ValueError(f"No unique image entry for board={board!r} role={role!r}")
    entry = matches[0]
    folder = (ROOT / "firmware" / "prebuilt" / entry["environment"]).resolve()
    if not folder.is_relative_to((ROOT / "firmware" / "prebuilt").resolve()):
        raise ValueError("Manifest image path escapes the prebuilt directory")
    for item in entry["files"].values():
        filename = item["file"]
        if Path(filename).name != filename:
            raise ValueError(f"Unsafe image filename in manifest: {filename!r}")
        path = folder / filename
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != item["sha256"]:
            raise ValueError(f"SHA-256 mismatch for {path.relative_to(ROOT)}")
        if len(data) != item["size_bytes"]:
            raise ValueError(f"Size mismatch for {path.relative_to(ROOT)}")
        offset = int(item["offset"], 16)
        if offset < 0 or offset + len(data) > entry["flash_mb"] * 1024 * 1024:
            raise ValueError(f"{filename} does not fit the {entry['flash_mb']} MiB image profile")
    return entry, folder


def command_for(entry: dict, folder: Path, port: str) -> list[str]:
    args = [
        sys.executable, "-m", "esptool", "--chip", entry["chip"], "--port", port,
        "write-flash", "--flash-mode", entry["flash_mode"], "--flash-size", "keep",
        "--flash-freq", f"{entry['flash_freq_mhz']}m",
    ]
    for name in ("bootloader", "partition-table", "app"):
        image = entry["files"][name]
        args.extend((image["offset"], str(folder / image["file"])))
    return args


def list_ports() -> int:
    try:
        from serial.tools import list_ports
    except ImportError:
        print("Port listing needs esptool: python -m pip install esptool", file=sys.stderr)
        return 2
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
    for port in ports:
        label = f" — {port.description}" if port.description else ""
        print(f"{port.device}{label}")
    return 0


def detected_flash_mb(entry: dict, port: str) -> int:
    probe = [sys.executable, "-m", "esptool", "--chip", entry["chip"], "--port", port, "flash-id"]
    result = subprocess.run(probe, text=True, capture_output=True, check=False)
    output = result.stdout + result.stderr
    print(output, end="" if output.endswith("\n") else "\n")
    if result.returncode:
        raise RuntimeError("Chip identification failed; no image was written")
    sizes = [int(a or b) for a, b in PORT_SIZE.findall(output)]
    if not sizes:
        raise RuntimeError("Could not read detected flash capacity; no image was written")
    if any(size != sizes[0] for size in sizes):
        raise RuntimeError("Conflicting flash capacity readings; no image was written")
    if sizes[0] < entry["flash_mb"]:
        raise RuntimeError(
            f"Board has {sizes[0]} MiB flash; this {entry['flash_mb']} MiB profile needs at least "
            "that much capacity. No image was written."
        )
    return sizes[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", choices=("c6", "s3", "esp32"), help="ESP32 family")
    parser.add_argument("--role", choices=("a", "b"), help="Relay / DS-room side")
    parser.add_argument("--port", help="Serial port, for example COM5 or /dev/ttyUSB0")
    parser.add_argument("--list-ports", action="store_true", help="List available serial ports")
    parser.add_argument("--flash", action="store_true", help="Probe the board and write the verified images")
    args = parser.parse_args()

    if args.list_ports:
        if args.board or args.role or args.port or args.flash:
            parser.error("--list-ports cannot be combined with board, role, port or --flash")
        return list_ports()
    if not (args.board and args.role):
        parser.error("--board and --role are required (use --list-ports to inspect serial ports)")
    if args.flash and not args.port:
        parser.error("--port is required with --flash")
    if args.port and not args.flash:
        parser.error("--port is only used with --flash; preview mode never accesses hardware")

    try:
        entry, folder = load_entry(args.board, args.role)
        print(f"Image: {entry['environment']} ({entry['chip']}, side {entry['role'].upper()})")
        print(f"Minimum flash: {entry['flash_mb']} MiB; write mode: {entry['flash_mode']} @ {entry['flash_freq_mhz']} MHz")
        print(f"Provenance: {entry['provenance']}")
        if not args.flash:
            print("All bundled image hashes and offsets are valid. Dry run: no serial port was opened and nothing was written.")
            print("To flash, add --flash --port PORT. The detected chip and flash size will be checked before writing.")
            return 0

        capacity = detected_flash_mb(entry, args.port)
        print(f"Detected capacity: {capacity} MiB; required profile: {entry['flash_mb']} MiB.")
        print(f"Writing verified {entry['role'].upper()} firmware to {args.port}.")
        result = subprocess.run(command_for(entry, folder, args.port), check=False)
        return result.returncode
    except (OSError, ValueError, KeyError, json.JSONDecodeError, RuntimeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
