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
- `online.c`: local/remote member state, drawing queues, and relay scheduling.
  `usb_transport.inc` (USB Serial/JTAG line protocol) is a private fragment included
  by this translation unit; it shares its state intentionally.

Keep the radio callbacks, ACK ownership, completion waits, attributes, and timing
constants together when reviewing changes. Moving a helper does not establish that
radio behavior has been validated on hardware.

## Board modes

Select a mode by environment; `platformio.ini` supplies `SNIFFER_MODE`. Changing
the fallback define in `firmware_config.h` does not override that build flag.

| Environment | Board | Behavior | Status |
| --- | --- | --- | --- |
| `esp32c6usb` | ESP32-C6 | USB room host for the Android app / MLS relay; default 4 slots → 6 here (2 local DS + 4 remote ghosts) | production |
| `esp32c6host` | ESP32-C6 | standalone room and drawing echo bot (default 4 clients) | bench |
| `echo_a`..`echo_d` | ESP32-C6 | echo bot pinned to room A–D | bench |
| `esp32c6ghost` | ESP32-C6 | local GHOST identity and drawing echo; three physical clients | research |
| `esp32dev` | ESP32-WROOM | fixed-channel PCAP-over-UDP sniffer | research |
| `esp32dev_disc` | ESP32-WROOM | channel-hopping serial discovery | research |
| `esp32dev_serial` | ESP32-WROOM | fixed-channel serial packet tracing | research |
| `sniffer_b`, `sniffer_d_transfer` | ESP32-WROOM | passive channel-7 witnesses | research |

```sh
pio run -e esp32c6usb
pio run -e esp32c6host -t upload --upload-port <port>
pio device monitor -e esp32c6host --port <port>
```

Use the actual serial ports on your machine. Builds do not flash devices.
Shared Kconfig choices are in `sdkconfig.defaults`; discovery keeps its original
100 Hz tick through `firmware/esp32/sdkconfig.tick100`. Per-environment `sdkconfig.*`
files and `.pio/` are generated output. All environments use the pinned pioarduino
platform in `[env]`. `esp32c6usb` writes a 4 MB flash header into
`firmware.factory.bin`; it boots on 4 MB and 8 MB modules.

## Profile and channel

Edit the example `host_profile` and `host_profile_bio` in `main.c`. The Bio is a
UTF-16 literal with at most 26 code units; library users can set it at runtime with
`host_profile_set_bio`. Rebuild and rejoin to test a firmware profile change.
Host MACs, chat room, capture channel and sniffer AP credentials are example
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
with room IDs 0–3 on channels A=1, B=7, C=13, D=7 (B and D share channel 7).
A on channel 7 was invisible to the DS. All four pairs are hardware-confirmed:
drawing echo displays and Send re-enables (C/channel 13 with a 10,276-byte drawing
and a paired passive WROOM capture; D/channel 7 without a power cycle after the
final flash). All four use 2 Mbps long preamble. Do not assume arbitrary room/channel pairs work.

An intermittent failure remains unresolved: on both B and D, the host reported
CMD completions while the WROOM saw ACKs but no CMD polls or DS replies. B
recovered after USB power cycling; D later recovered after reflash without a
power cycle. Successful room trials do not establish reliable startup or a
root-cause fix.

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
on channel 7. It does not advertise a room. Flash it to the
WROOM's actual port, not the C6's, and start both serial recordings **before**
joining B so the sniffer sees the association that arms its bounded MP trace.

```sh
pio run -e sniffer_b -t upload --upload-port /dev/cu.usbserial-0001
pio run -e echo_b -t upload --upload-port /dev/cu.usbmodem21201
```

Those are example hub ports; verify chip identities before flashing. The sniffer
captures the first 64 MP frames per association plus a bounded bidirectional
application window. `sniffer_d_transfer` extends that window to 8192 applications
on channel 7, plus 65536 other MP frames so ACKs and empty replies remain visible
during drawings. It uses a **921600-baud console** (upload remains 115200). Record
at that baud and rejoin D after starting the witness. Both budgets are bounded;
check for exhausted budgets and malformed records as well as queue drops. Its custom UART defaults
are in `firmware/esp32/sdkconfig.capture`; existing generated SDK configs must
also select `CONFIG_ESP_CONSOLE_UART_CUSTOM=y` to honor the higher baud. Verify
`CONFIG_ESP_CONSOLE_UART_BAUDRATE` in the generated `config/sdkconfig.h`.
Check capture drops and repeated `rx_us` values before using timings;
PC timestamps and serial-log prefixes are not over-the-air timing evidence.

## Runtime heap measurements

Host builds log `HEAP stage=...` at startup, association/leave, standalone drawing
receipt, outbound completion, and every five seconds. Values are bytes of
internal, byte-addressable heap: `internal_free`, `internal_min` (allocator
low-water marks), and `internal_largest` (largest available block). Drawing
allocation failures also log the requested size. Compare idle, joined, and
repeated drawing transfers; static RAM usage alone excludes runtime Wi-Fi,
queue, stack, and drawing allocations. These are diagnostics, not a RAM fix.

Standalone host echoes also log `ECHO queued`, `ECHO start`, and `ECHO done`,
correlated by drawing ID and recipient AID. `queue_ms` measures completed receipt
until the recipient's sender accepts the echo; `start_wait_ms` measures acceptance
until its first transmission attempt; `transfer_ms` includes retries and other
polls until committed completion. `delivered`, `no_reply`, and `failed` count only
echo-fragment attempts (including announcements); `other_polls` counts that
recipient's non-echo polls after acceptance. These are software timings, not
DS display times. Ghost and online sends are not included. Radio settings and
scheduling are unchanged. Validate a single boot's capture after an echo with
`python tools/check_echo_timing.py captures_out/<capture>.log`.

`RX REPEAT` reports per-recipient, per-association cumulative accepted application
counts once per second. `same_payload` counts byte-identical consecutive packets;
`same_sequence` is the subset also carrying the same two-byte client WM sequence
footer (before the FCS). Compare counter deltas over a drawing, not identity
handshake totals. These counters do not deduplicate or change relay behavior.

### Experimental duplicate-relay pacing

Building `echo_d` with `-D HOST_PACE_RELAY_REPEATS=1` (no checked-in environment)
enables pacing; plain `echo_d` is the control. Once a drawing relay is delivered, byte-identical repeats with the same
client WM sequence and association generation are consumed without another relay
for 100 ms. Radio ACKs are unchanged. New accepted data invalidates the cache;
failed/no-reply relays and identity handshakes are not suppressed. After 100 ms,
a retry is allowed because a radio reply does not prove application receipt.
`RELAY PACING suppressed=...` counts skipped repeats. This is an experimental
single-DS comparison, not a validated protocol fix or a multi-client guarantee.
Compare echo times, receive drops, drawing appearance and Send re-enabling with
the control before enabling it elsewhere.

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

The default capacity is four DS clients plus the host (`esp32c6usb`: six slots,
two local DS plus four remote ghosts). A full 16-member room
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
and [historical radio notes](RADIO_NOTES.md) document earlier trials.

Application reactions live in `host_room_event`: receive events copy a drawing
into the echo queue; sent events update completion counters. Add custom behavior
there and offload slow work to a worker queue so radio timing stays bounded.

See [ghost users](GHOST_USERS.md) for the local virtual-member trial and
[USB bridge](USB_BRIDGE.md) for the production transport.
