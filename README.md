# RetroPicto

**Bring Nintendo DS PictoChat rooms together.** RetroPicto is an ESP32 project
with a portable C protocol library, Python capture tools, and PC
BLE/USB gateways. The project home is [retropic.to](https://retropic.to).

This repository contains the ESP32 firmware and PC tools. The companion local
repositories are `retropicto-android` (Android gateway) and `retropicto-relay`
(WebSocket relay). Protocol identifiers, BLE service names, and the `pictochat`
C/Python APIs remain stable for existing integrations.

## First setup: flash two ESP32 boards

The quickest supported path connects two DS consoles through two ESP32-C6
DevKitC-1 compatible boards, two phones or PCs, and a relay server. C6 images
are ready in `firmware/prebuilt/`. S3 and original ESP32/WROOM images are
experimental; compile-checking them does not prove radio interoperability.

1. Install Python 3.11 or newer for the PC BLE gateway and a USB data cable
   for each board. Python 3.10+ is enough to use the capture decoder by itself.
2. Open a terminal in this repository and make an environment:

   ```powershell
   python -m venv .venv
   .\.venv\Scripts\Activate.ps1
   python -m pip install --upgrade pip esptool
   ```

   On macOS or Linux, activate with `source .venv/bin/activate`.
3. List serial ports and preview flashing board A. Preview mode checks the
   image manifest and hashes and does not open a serial port:

   ```powershell
   python tools/flash_multihop.py --list-ports
   python tools/flash_multihop.py --board c6 --role a --port COM5
   ```

4. Flash after reviewing the detected board and image. The command checks the
   chip and installed flash capacity before writing:

   ```powershell
   python tools/flash_multihop.py --board c6 --role a --port COM5 --flash
   python tools/flash_multihop.py --board c6 --role b --port COM12 --flash
   ```

   Replace the example ports with those shown on your computer. Close serial
   monitors first. The two images select their A/B roles in firmware; they do
   not need MAC address edits. Flashing replaces the current application.
5. Install the PC or Android gateways and give each its secure WebSocket URL,
   matching role (`a` or `b`), and distinct relay token. Follow `INSTALL.md`
   for the shared relay setup, then join DS Room A beside board A and DS Room B
   beside board B.

The bundled C6 images were compiled from the prior `pictochat-redux` snapshot
and reused unchanged for this local repository extraction. Their embedded
build metadata can still show the old project name. The C6 configuration has
not yet had a fresh smoke test on hardware. Check each image's SHA-256 against
`firmware/prebuilt/manifest.json`. The S3 and WROOM builds have not been tested
on hardware. Use the board guide for flash size, profiles, and limitations.

## Build firmware from source

Install PlatformIO Core (`python -m pip install platformio`) and build a role:

```powershell
pio run -e multihop_c6_a
pio run -e multihop_c6_b
```

Use `pio run -e multihop_s3_a` / `multihop_s3_b` for ESP32-S3 DevKitC-1, or
`multihop_esp32_a` / `multihop_esp32_b` for a 4 MiB ESP32-WROOM dev board.
Generic alternate boards may need their own PlatformIO profile and partition
configuration. Do not assume a same-chip board has enough flash or matching
radio behavior. Upload from source only after checking the correct serial port:

```powershell
pio run -e multihop_c6_a -t upload --upload-port COM5
```

PlatformIO does not assign a fixed upload or monitor port. See
[`docs/BOARD_SUPPORT.md`](docs/BOARD_SUPPORT.md) and [`docs/FIRMWARE.md`](docs/FIRMWARE.md).

## Portable libraries

The C11 API is in `lib/pictochat/include/pictochat/`; implementations are in
`lib/pictochat/src/`. It has no ESP-IDF or operating-system dependency:

```powershell
cmake -S . -B build/native -DPICTOCHAT_NATIVE=ON
cmake --build build/native --config Debug
ctest --test-dir build/native -C Debug --output-on-failure
```

Embed it with `add_subdirectory(path/to/retropicto/lib/pictochat)` and link
`pictochat`, or use its CMake install/export targets. The API guide describes
memory ownership, event callbacks, byte contracts, and adapter responsibilities.

The Python package in `python/pictochat/` requires Python 3.10+ and has no
runtime dependencies. The existing `pictochat` import and CLI names remain
available; `retropicto-*` command aliases are provided for new scripts:

```powershell
python -m pip install .
pictochat-render tests/fixtures/send.pcap --all -o captures_out
retropicto-render tests/fixtures/send.pcap --all -o captures_out
```

The decoder processes captures; it is not a Python radio or live-host backend.
PC BLE and USB gateways have separate dependencies in `tools/requirements-ble.txt`
and `INSTALL.md`.

## Project contents

| Path | Contents |
| --- | --- |
| `lib/pictochat/` | Portable C11 session and room engine, frame encoder, public headers |
| `python/pictochat/` | Capture decoder, drawing helpers, Python command-line tools |
| `firmware/esp32/` | ESP-IDF adapters, BLE/USB transports, board profiles and prebuilt images |
| `tools/` | PC gateways, serial capture, release packaging and flash helper |
| `examples/` | Online relay guide and optional AI participant example |
| `tests/fixtures/` | Shared protocol regression captures and expected message bytes |
| `docs/` | API, setup, transport and protocol notes |

The portable library is single-owner state; an adapter supplies radio admission,
queues, timing, ACK handling, clocks, and logging. Multi-client scheduling is
experimental until validated with physical DS consoles. A consumed-packet ACK
does not prove that a drawing appeared on a console screen. See
[`docs/LIBRARY.md`](docs/LIBRARY.md) and [`PROTOCOL.md`](PROTOCOL.md).

## Checks and packaging

Run native C and Python library checks with CMake/CTest and
`python -m unittest discover -s tests -p 'test_*.py' -v`. PC gateway tests are
included alongside library tests. To create a local source-and-firmware ZIP,
run `python tools/package_release.py`; it uses only files in this checkout and
does not include local credentials or generated build folders.

No source license is declared yet. Third-party AI example assets retain their
own notices; read their adjacent license files before redistribution.
