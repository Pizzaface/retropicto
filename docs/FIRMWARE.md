# ESP32 adapter

The adapter in `firmware/esp32/main.c` supplies radio I/O, timing, queues, logging,
and the example echo-bot policy. It uses the portable library for host protocol
state and frame encoding. PlatformIO builds it with ESP-IDF.

## Board modes

Select a mode by environment; `platformio.ini` supplies `SNIFFER_MODE`. Changing
the fallback define in the C file does not override that build flag.

| Environment | Board | Behavior | Configured port |
| --- | --- | --- | --- |
| `esp32dev` | ESP32-WROOM | fixed-channel PCAP-over-UDP sniffer | automatic |
| `esp32dev_disc` | ESP32-WROOM | channel-hopping serial discovery | COM6 |
| `esp32dev_serial` | ESP32-WROOM | fixed-channel serial packet tracing | COM6 |
| `esp32c6` | ESP32-C6 | experimental joiner/injector | COM12 |
| `esp32c6host` | ESP32-C6 | experimental four-client room and drawing echo bot | COM12 |
| `esp32c6online` | ESP32-C6 | direct Wi-Fi bridge | COM12 |
| `esp32c6usb` | ESP32-C6 | USB bridge | COM12 |
| `esp32c6ble` | ESP32-C6 | BLE gateway relay | COM12 |
| `esp32c6ghost` | ESP32-C6 | local GHOST identity and drawing-echo experiment; three physical clients | COM12 |

```sh
pio run -e esp32c6host
pio run -e esp32c6host -t upload --upload-port COM12
pio device monitor -e esp32c6host --port COM12
```

Use the actual serial ports on your machine. Builds do not flash devices.
Shared Kconfig choices are in `sdkconfig.defaults`; joiner and discovery preserve their
original 100 Hz tick through `firmware/esp32/sdkconfig.tick100`. Per-environment `sdkconfig.*`
files and `.pio/` are generated output. The existing 2 MB flash partition setting
can produce a board-size warning on larger boards; it is unchanged by the library
extraction.

## Profile and channel

Edit the example `host_profile` and `host_profile_bio` in `main.c`. The Bio is a
UTF-16 literal with at most 26 code units; library users can set it at runtime with
`host_profile_set_bio`. Rebuild and rejoin to test a firmware profile change.
Host/joiner MACs, chat room, capture channel and sniffer AP credentials are example
configuration near the top of `main.c`, not library defaults.

For discovery, build/upload `esp32dev_disc`, then watch serial while DS consoles
enter a room. Set `CAPTURE_CHANNEL` to their observed channel before using a
fixed-channel mode. One ESP32 radio cannot monitor two channels at once.

For streaming, build/upload `esp32dev`, connect the computer to its configured
SoftAP, and use `tools/udp_to_wireshark.py` to receive the PCAP stream. The optional
`tools/pictochat.lua` dissector provides Nintendo framing in Wireshark. See each
tool's `--help` for output/interface options.

## Regression workflow

Run native C/Python tests first as described in the root README. Compile the host
and sniffer modes, then perform a deliberate hardware trial when needed: join the
room, wait for Send, send multiple drawings, verify clean PICTOBOT replies and that
Send becomes available after each. Do not treat driver completion logs as proof
that a DS displayed a drawing.

For multi-client validation, join two DS consoles, check that both see the bot and
each other's names, and send different drawings in both directions, including
overlapping sends. Add a late joiner and verify its roster/identities. Disconnect
and rejoin either console while the other sends; verify the surviving console stays
usable and the reused AID receives no stale input. Capture the traffic independently
to check targeted footer masks and reply timing. The scheduler grants one console
per cycle, so per-console throughput decreases as the room grows. The radio timing
constants and cross-client identity replay still require this hardware trial.

The configured capacity is four DS clients plus the host. A full 16-member room
means 15 clients plus the host. The installed ESP-IDF native Wi-Fi types cap C6
SoftAP associations at 10 (11 total room members), while ESP32/S2/S3 allow 15.
Increasing the room array alone therefore cannot reach 16 on the current C6
SoftAP adapter. A different radio target requires porting the target-specific
injection hooks, sharing drawing buffers to reduce per-client RAM, and validating
polling throughput/timing with a full room.

`tools/capture_serial.py` records serial logs. `tools/export_drawings.py` verifies
DRAW dump coverage/checksums and renders complete messages; `tools/analyze_serial_drawings.py`
examines independent sniffer records. Write captures and exports to `captures_out/`,
which is ignored. Only intentional regression inputs belong in `tests/fixtures/`.

[Drawing evidence](DRAWING_TRANSFER.md), [identity evidence](IDENTITY_EXCHANGE.md),
and [historical radio notes](RADIO_NOTES.md) document earlier trials. Their removed
experimental snapshots are not current build inputs. `tools/linux/` is historical
Linux radio experimentation and is not an implementation of the new library API.

Application reactions live in `host_room_event`: receive events copy a drawing
into the echo queue; sent events update completion counters. Add custom behavior
there and offload slow work to a worker queue so radio timing stays bounded.

See [ghost users](GHOST_USERS.md) for the local virtual-member trial and proposed online relay.

See [the two-C6 Wi-Fi bridge](WIFI_BRIDGE.md) for the direct-LAN experiment on COM12 and COM5.

For the router-independent USB path, see [USB bridge](USB_BRIDGE.md).

The experimental esp32c6ble environment and PC gateway are described in [BLE_GATEWAY.md](BLE_GATEWAY.md).
