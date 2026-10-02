# ESP32 adapter

The adapter in `firmware/esp32/main.c` supplies radio I/O, timing, queues, logging,
and the example echo-bot policy. It uses the portable library for host protocol
state and frame encoding. PlatformIO builds it with ESP-IDF.

## Source map

- `firmware_config.h`: compile-time mode selection, room/channel defaults, capture
  settings, and demo slot constants. PlatformIO build flags retain precedence.
- `capture_packet.h`: internal, allocation-free frame-control, MAC-header-length,
  and radiotap helpers. Native tests check these bytes without ESP-IDF.
- `main.c`: radio identities and state, target-specific transmit hooks, the
  promiscuous callback, mode tasks, and startup. Host profile construction and
  periodic status logging are separate helpers so the host loop is easier to scan.
- `online.c`: local/remote member state, drawing queues, relay scheduling, and LAN
  transport. `ble_transport.inc` and `usb_transport.inc` are private implementation
  fragments included by this translation unit; they share its state intentionally.

Keep the radio callbacks, ACK ownership, completion waits, attributes, and timing
constants together when reviewing changes. Moving a helper does not establish that
radio behavior has been validated on hardware.

## Board modes

Select a mode by environment; `platformio.ini` supplies `SNIFFER_MODE`. Changing
the fallback define in `firmware_config.h` does not override that build flag.

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
configuration in `main.c` (radio identities) and `firmware_config.h` (mode, room,
channel and AP defaults), not library defaults.

For discovery, build/upload `esp32dev_disc`, then watch serial while DS consoles
enter a room. Set `CAPTURE_CHANNEL` to their observed channel before using a
fixed-channel mode. One ESP32 radio cannot monitor two channels at once.

For streaming, build/upload `esp32dev`, connect the computer to its configured
SoftAP, and use `tools/udp_to_wireshark.py` to receive the PCAP stream. The optional
`tools/pictochat.lua` dissector provides Nintendo framing in Wireshark. See each
tool's `--help` for output/interface options.

## A–D drawing-echo trials

`echo_a`, `echo_b`, `echo_c`, and `echo_d` run the existing standalone PICTOBOT
with room IDs 0–3. A uses radio channel 1 and B uses channel 7, the
hardware-confirmed pairs. A on channel 7 was invisible to the DS (see
[historical trials](WIFI_BRIDGE.md#superseded-bench-path)). C/channel 13 was
hardware-confirmed on September 28: a 10,276-byte drawing echoed correctly and
Send re-enabled, with a paired passive WROOM capture. D/channel 7 was also
user-confirmed on September 28: drawing echo displayed and Send re-enabled,
without a power cycle after the final flash. All four trials retain 2 Mbps long
preamble. Do not assume arbitrary room/channel pairs work.

An intermittent failure remains unresolved: on both B and D, the host reported
CMD completions while the WROOM saw ACKs but no CMD polls or DS replies. B
recovered after USB power cycling; D later recovered after reflash without a
power cycle. Successful room trials do not establish reliable startup or a
root-cause fix. These targets use the pinned multihop SDK platform.

```sh
pio run -e echo_a -t upload --upload-port /dev/cu.usbmodem1101
```

Use the connected board's actual port. For each target, join the matching room
on a DS, wait for Send, send a drawing, and confirm the PICTOBOT echo appears
and Send becomes available again. Leave the room before flashing the next
image. Record serial logs and DS observations separately: a successful build
or `DRAW outbound TX complete` alone does not prove the echo displayed.

### Independent WROOM witness

`sniffer_b` builds the existing passive serial-management sniffer for ESP32-WROOM
on channel 7, using the pinned SDK. It does not advertise a room. Flash it to the
WROOM's actual port, not the C6's, and start both serial recordings **before**
joining B so the sniffer sees the association that arms its bounded MP trace.

```sh
pio run -e sniffer_b -t upload --upload-port /dev/cu.usbserial-0001
pio run -e echo_b -t upload --upload-port /dev/cu.usbmodem21201
```

Those are example hub ports; verify chip identities before flashing. The sniffer
captures the first 64 MP frames per association plus a bounded host-application
window. Check capture drops and repeated `rx_us` values before using timings;
PC timestamps and serial-log prefixes are not over-the-air timing evidence.

## Runtime heap measurements

Host builds log `HEAP stage=...` at startup, association/leave, standalone drawing
receipt, outbound completion, and every five seconds. Values are bytes of
internal, byte-addressable heap: `internal_free`, `internal_min` (allocator
low-water marks), and `internal_largest` (largest available block). Drawing
allocation failures also log the requested size. Compare idle, joined, and
repeated drawing transfers; static RAM usage alone excludes runtime Wi-Fi,
queue, stack, and drawing allocations. These are diagnostics, not a RAM fix.

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
