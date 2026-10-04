"""Create and verify a local RetroPicto source + firmware ZIP."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path, PurePosixPath
import zipfile


ROOT = Path(__file__).resolve().parents[1]
NAME = "RetroPicto"
SOURCE_DIRS = {"docs", "firmware", "lib", "python", "tests", "tools"}
ROOT_FILES = {
    ".clang-format", ".gitattributes", ".gitignore", "CMakeLists.txt", "PROTOCOL.md",
    "README.md", "platformio.ini", "pyproject.toml", "sdkconfig.defaults",
}
EXCLUDED_PARTS = {
    ".git", ".pio", ".venv", "venv", "build", "dist", "node_modules",
    "__pycache__", ".pytest_cache", ".mypy_cache",
}


def allowed(name: str) -> bool:
    path = PurePosixPath(name)
    if any(part in EXCLUDED_PARTS or part.endswith(".egg-info") for part in path.parts):
        return False
    if (path.name.startswith(".env") or path.name.endswith(".local.json")
            or path.name in {"local.properties", "wifi_credentials.h"}
            or path.suffix in {".log", ".pyc", ".pyo", ".pcapng", ".jks", ".keystore"}):
        return False
    return name in ROOT_FILES or (len(path.parts) > 1 and path.parts[0] in SOURCE_DIRS)


def collect() -> dict[str, bytes]:
    payload: dict[str, bytes] = {}
    for base in (ROOT / name for name in sorted(SOURCE_DIRS)):
        if not base.exists():
            raise RuntimeError(f"Required source directory missing: {base.name}")
        for source in sorted(base.rglob("*")):
            if source.is_symlink():
                raise RuntimeError(f"Refusing symlink in release source: {source.relative_to(ROOT)}")
            if not source.is_file():
                continue
            name = source.relative_to(ROOT).as_posix()
            if allowed(name):
                payload[name] = source.read_bytes()
    for name in sorted(ROOT_FILES):
        source = ROOT / name
        if source.is_file():
            payload[name] = source.read_bytes()
    required = {"README.md", "platformio.ini", "firmware/esp32/main.c", "tests/fixtures/send.pcap"}
    missing = required - payload.keys()
    if missing:
        raise RuntimeError(f"Required release file missing: {sorted(missing)[0]}")
    if any(name.startswith(("android/", "server/")) for name in payload):
        raise RuntimeError("Android and relay sources do not belong in the RetroPicto archive")
    return payload


def package(output: Path) -> tuple[int, str]:
    payload = collect()
    sums = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in sorted(payload.items())
    ).encode()
    payload["SHA256SUMS.txt"] = sums
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(payload.items()):
            entry = zipfile.ZipInfo(f"{NAME}/{name}")
            entry.create_system = 3
            entry.external_attr = (0o100755 if name.endswith(".sh") else 0o100644) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, data)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise RuntimeError("ZIP integrity check failed")
        for name, data in payload.items():
            if archive.read(f"{NAME}/{name}") != data:
                raise RuntimeError(f"ZIP verification failed: {name}")
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix + ".sha256").write_text(
        f"{digest}  {output.name}\n", encoding="utf-8"
    )
    return len(payload), digest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "dist" / "RetroPicto-source-firmware.zip")
    args = parser.parse_args()
    count, digest = package(args.output)
    print(f"Created {args.output} ({args.output.stat().st_size:,} bytes, {count} files)")
    print(f"SHA-256: {digest}")


if __name__ == "__main__":
    main()
