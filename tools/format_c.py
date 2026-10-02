#!/usr/bin/env python3
"""Apply (or check) the repository's clang-format style for first-party C code."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("firmware/esp32", "lib/pictochat", "tests")
SOURCE_SUFFIXES = {".c", ".h", ".inc"}
FORMAT_VERSION = "18.1.8"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report differences without editing")
    parser.add_argument("--clang-format", default="clang-format", help="formatter executable")
    args = parser.parse_args()
    formatter = shutil.which(args.clang_format)
    if not formatter:
        parser.error("clang-format is missing; install it with: "
                     f"python -m pip install clang-format=={FORMAT_VERSION}")
    version = subprocess.run([formatter, "--version"], check=True, capture_output=True, text=True)
    if f"clang-format version {FORMAT_VERSION}" not in version.stdout:
        parser.error(f"use clang-format {FORMAT_VERSION} for reproducible formatting")
    sources = sorted(str(path.relative_to(ROOT))
                     for directory in SOURCE_ROOTS
                     for path in (ROOT / directory).rglob("*")
                     if path.is_file() and path.suffix in SOURCE_SUFFIXES
                     and path.name != "wifi_credentials.h")
    options = ["--dry-run", "--Werror"] if args.check else ["-i"]
    return subprocess.run([formatter, "--style=file", *options, *sources], cwd=ROOT).returncode


if __name__ == "__main__":
    sys.exit(main())
