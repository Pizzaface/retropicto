# Multi-Hop board compatibility

The BLE Multi-Hop builds below share the Android/PC gateways, relay server, and
PictoChat protocol. Flash one board as A and the other as B. Each side may use a
different chip family, but mixed-chip operation requires hardware validation.

| Chip / reference board | Side A build | Side B build | Status |
| --- | --- | --- | --- |
| ESP32-C6 / DevKitC-1 | `multihop_c6_a` | `multihop_c6_b` | Based on the C6 BLE path used in the confirmed two-DS setup; new explicit-role images need a hardware smoke test |
| ESP32-S3 / DevKitC-1-N8, no PSRAM | `multihop_s3_a` | `multihop_s3_b` | Experimental port; DS interoperability unverified |
| Original ESP32 / generic WROOM dev board | `multihop_esp32_a` | `multihop_esp32_b` | Experimental port; DS interoperability unverified |

Compilation alone does not establish that a DS can join or exchange drawings.
The original `esp32dev*` targets remain sniffers, not Multi-Hop hosts.

## Flash the bundled images

Six prebuilt image sets are checked into `firmware/prebuilt/`. The root-level
`tools/flash_multihop.py` helper verifies SHA-256 hashes and offsets before
showing a dry run. Use that helper to list ports, preview, and flash. Do not
select a prebuilt by chip family alone; the reference board's flash size and
settings must also match. Flashing replaces the existing firmware.

The bundled C6 and S3 images specify **8 MiB flash, DIO, 80 MHz**; original
ESP32/WROOM images specify **4 MiB flash, DIO, 40 MHz**. For a C6 board
with 4 MiB flash, rebuild using its own board profile even though the application
partition itself fits within 2 MiB. Chip family alone is not enough to choose a
prebuilt image.

Install Python and esptool, close serial monitors, and run from the repository
root. For C6 side A, for example:

```powershell
python -m pip install esptool
python tools/flash_multihop.py --list-ports
python tools/flash_multihop.py --board c6 --role a
python tools/flash_multihop.py --board c6 --role a --port YOUR_PORT --flash
```

Replace `YOUR_PORT` with your board's actual port. Select `s3` for ESP32-S3 or
`esp32` for original ESP32; repeat with the matching B role. The helper obtains
offsets from the tracked manifest and checks chip and flash capacity before it
starts writing. Original ESP32 has a different bootloader offset.

The images select their role directly and contain no relay/Wi-Fi credentials.
Return to the project root and continue relay/gateway installation. Prebuilt
S3/WROOM images remain experimental until the DS checks below pass on hardware.

## Build from source

Install PlatformIO Core (`python -m pip install platformio`). From the extracted
project root, substitute your board's environment and serial port:

```powershell
pio device list
pio run -e multihop_c6_a
pio run -e multihop_c6_a -t upload --upload-port YOUR_PORT
pio run -e multihop_c6_b -t upload --upload-port OTHER_PORT
```

For S3, substitute `multihop_s3_a` and `multihop_s3_b`. For original ESP32,
use `multihop_esp32_a` and `multihop_esp32_b`. These six builds select A/B at
compile time, independent of the factory MAC. A advertises PICTO-A and hosts
DS Room A on channel 1; B advertises PICTO-B and hosts Room B on channel 7.
Flashing two A images will not make an A/B pair.

These targets pin the [pioarduino platform to 55.03.39 / ESP-IDF 5.5.4](https://github.com/pioarduino/platform-espressif32/releases/tag/55.03.39), matching
the development toolchain. First build downloads tools and SDK dependencies.
The older `esp32c6ble` target retains its original factory-MAC selection for
existing setups. Do not mix its MAC-edit instructions with the new explicit roles.

Continue with [relay and gateway setup](../INSTALL.md). PC and Android
gateways use the same service and packet format for all three chip families.

## Other boards using the same chip

The profiles describe specific flash and board configurations, not every board
sold under an ESP32 family name. Confirm the chip, flash capacity, flash mode,
and upload connection against the board's documentation. Do not flash a C6
binary onto S3 or original ESP32.

For a different C6 board, add an environment to `platformio.ini` that inherits
the desired role and uses its PlatformIO board ID, for example:

```ini
[env:my_c6_a]
extends = env:multihop_c6_a
board = YOUR_PLATFORMIO_BOARD_ID
```

Create the matching B environment inheriting `env:multihop_c6_b`. Build from
source for a board whose flash configuration differs. The same approach applies
to S3 and WROOM profiles. The application partition ends at 2 MiB; a board must
have enough flash for that layout. These builds do not require PSRAM.

C6 keeps the existing native USB Serial/JTAG diagnostic output. S3 and WROOM
use the SDK console (UART by default), so the correct UART/USB-to-serial port
may differ from a board's native USB connector. BLE carries the relay traffic;
USB provides power, flashing, and diagnostics. The existing USB bridge firmware
has not been ported to WROOM by this change.

## What still needs hardware validation

Only C6 has previously demonstrated the complete two-phone/two-DS workflow.
The S3/WROOM ports exclude C6-only private TX-driver diagnostic structure reads
and use a portable console path. Original ESP32 reserves the room state from
internal heap at startup to fit its smaller static DRAM window; allocation
failure stops startup explicitly. It does not require PSRAM. Runtime heap
headroom during BLE connection and drawing transfer still needs a hardware test.
The host still relies on private Wi-Fi beacon
and raw-frame hooks; their behavior and radio timing require validation on
each chip. A successful link or BLE connection is insufficient.

For each new board/chip:

1. Check boot, BLE name, and gateway connection without resets.
2. Confirm the corresponding room appears on a real DS and can be joined.
3. Verify remote identities and different full-size drawings in both directions.
4. Leave/rejoin the room; reconnect the gateway; repeat the drawing checks.
5. Test Android background behavior and check for disconnects or lost drawings.

Record board model, chip, environment, SDK version, and results before describing
an experimental port as supported. If a room is absent or admission fails,
capture serial logs and radio traces for a chip-specific driver investigation.
