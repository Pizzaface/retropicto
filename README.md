# RetroPicto

**Bring Nintendo DS PictoChat rooms together online.** The project home is
[retropic.to](https://retropic.to).

```
DS ⇄ ESP32-C6 (esp32c6usb) ⇄ USB ⇄ Android app ⇄ retropic.to (MLS /mls)
```

This repository holds the ESP32 firmware, the portable C protocol library, and
Python capture tools. The Android app lives in `retropicto-android`; the server
in `retro-picto-server`. The `pictochat` C/Python API names are stable.

## First setup

Follow [retropic.to/docs/setup](https://retropic.to/docs/setup/). The Android
app and the web flasher at retropic.to/flash install the production firmware;
you do not need this repository to get online.

## Build firmware

Install PlatformIO Core (`python -m pip install platformio`), then:

```sh
pio run -e esp32c6usb
```

The image is `.pio/build/esp32c6usb/firmware.factory.bin` (bootloader,
partitions and app merged at offset 0, 4 MB flash header; boots on 4 MB and
8 MB C6 modules). This is the file retropic.to serves. Upload directly with
`pio run -e esp32c6usb -t upload --upload-port <port>`. Bench and research
environments are listed in [`docs/FIRMWARE.md`](docs/FIRMWARE.md).

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

## Project contents

| Path | Contents |
| --- | --- |
| `lib/pictochat/` | Portable C11 session and room engine, frame encoder, public headers |
| `python/pictochat/` | Capture decoder, drawing helpers, Python command-line tools |
| `firmware/esp32/` | ESP-IDF adapter, USB transport, sdkconfig fragments |
| `examples/` | `ai_chat.py`: join a Relay's room as an AI that answers drawings |
| `tools/` | Serial capture, USB bench bridge, analysis, release packaging |
| `tests/fixtures/` | Shared protocol regression captures and expected message bytes |
| `docs/` | API, setup, transport and protocol notes |

The portable library is single-owner state; an adapter supplies radio admission,
queues, timing, ACK handling, clocks, and logging. Multi-client scheduling is
experimental until validated with physical DS consoles. A consumed-packet ACK
does not prove that a drawing appeared on a console screen. See
[`docs/LIBRARY.md`](docs/LIBRARY.md) and [`PROTOCOL.md`](PROTOCOL.md).

## Checks and packaging

Run native C and Python library checks with CMake/CTest and
`python -m unittest discover -s tests -p 'test_*.py' -v`. To create a local source ZIP,
run `python tools/package_release.py`; it uses only files in this checkout and
does not include local credentials or generated build folders.

### C source style

First-party C sources, headers, tests, and transport fragments use the checked-in
`.clang-format`. Include order, protocol constants, and captured byte arrays are
kept intact. Use the same formatter version for repeatable changes:

```sh
python -m pip install clang-format==18.1.8
python tools/format_c.py --check
python tools/format_c.py
```

The `--check` command checks formatting without editing; omit it to apply the style.
The formatter excludes third-party code. Run the
native C tests after editing, and build the relevant PlatformIO environments before
hardware validation. Formatting or a native test pass is not a radio smoke test.

Released under the [MIT License](LICENSE). Third-party firmware notices are in
[`docs/third-party/`](docs/third-party/README.md).

## Trademarks and affiliation

- **Trademarks.** Nintendo, Nintendo DS, Nintendo DS Lite, Nintendo DSi, and
  PictoChat are trademarks of Nintendo. They appear here only to describe
  compatibility.
- **No affiliation.** RetroPicto is an independent fan project. It is not
  affiliated with, endorsed by, sponsored by, or approved by Nintendo.
- **No Nintendo material.** This repository contains no Nintendo code,
  firmware, ROMs, BIOS images, encryption keys, or other copyrighted assets.
- **How it works.** Compatibility comes from observing over-the-air traffic
  between consoles the authors own. RetroPicto does not modify consoles.
- **Bring your own DS.** You need your own Nintendo DS hardware to use it.
