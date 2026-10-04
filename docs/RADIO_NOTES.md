# Historical ESP32 radio notes

> Protocol code now lives in `lib/pictochat`; see [the library guide](LIBRARY.md).

Watch the wireless traffic between two Nintendo DS consoles running **PictoChat**
using an **ESP32-WROOM**. The ESP32 captures the raw 802.11 frames the DS uses
for its local-wireless protocol, filters them to Nintendo hardware, and streams
them live into **Wireshark** as PCAP-over-UDP. A Lua dissector then decodes the
Nintendo framing and gives you a scaffold for the PictoChat payload.

## How it works

The DS doesn't use normal WiFi. PictoChat and DS local multiplayer ride directly
on raw **IEEE 802.11 beacon + data frames** in the 2.4 GHz band, with Nintendo's
own MAC-layer protocol on top. The ESP32's **promiscuous (monitor) mode** can
hear those frames. Every DS radio's MAC starts with a Nintendo OUI, so we filter
on that to isolate DS traffic cheaply.

```
  Two DSs  ── 802.11 ──▶  ESP32 (promiscuous)  ── PCAP/UDP ──▶  PC  ──▶  Wireshark
 (PictoChat)              filters Nintendo OUIs    over SoftAP        + pictochat.lua
```

### The single-radio constraint (important)

The ESP32 has **one radio**. It can only listen on **one channel at a time**, and
to also send UDP it needs a working WiFi link on *that same channel*. The DS host
picks its channel (1–13) unpredictably, so the workflow is two steps:

1. **Find the channel** with discovery mode (serial only, hops all channels).
2. **Stream** on that channel: the ESP32 runs its own SoftAP there, your PC joins
   it, and frames flow to Wireshark. Using the ESP's SoftAP pins the capture
   channel to the DS channel with no home-router involvement.

## Step 1 — Find the DS channel (discovery mode)

The `esp32dev_disc` environment selects `SNIFFER_MODE=0` (discovery).
Build, flash, and watch serial (PlatformIO; there is no `default_envs`, so a plain
`pio run -t upload` would build every environment):

```
pio run -e esp32dev_disc -t upload
pio device monitor
```

Start PictoChat on both DSs, enter the same room, and scribble. Watch for lines
like:

```
I (12345) pictochat: ch 7 rssi -42 BEACON src=00:09:BF:12:34:56 len=120
I (12346) pictochat: ch 7 rssi -45 DATA  src=00:09:BF:AB:CD:EF len=96
```

The `ch` column is your DS channel. Note it (e.g. **7**).

## Step 2 — Stream to Wireshark

The `esp32dev` environment selects `SNIFFER_MODE=1` (stream). Channel and SoftAP
defaults live in `firmware/esp32/firmware_config.h`:

```c
#define CAPTURE_CHANNEL 7          // the channel you found in step 1
#define AP_SSID  "pictochat-sniffer"
#define AP_PASS  "dspackets"       // >= 8 chars, or "" for an open AP
```

Reflash: `pio run -e esp32dev -t upload` (then `pio device monitor`)

Then on your PC:

1. Join the WiFi network **`pictochat-sniffer`** (password `dspackets`).
2. Install the dissector: copy `tools/pictochat.lua` into Wireshark's
   *Personal Lua Plugins* folder (Help ▸ About Wireshark ▸ Folders), then
   Analyze ▸ Reload Lua Plugins.
3. Pipe the UDP stream into Wireshark live:

   ```powershell
   python tools\udp_to_wireshark.py | & "C:\Program Files\Wireshark\Wireshark.exe" -k -i -
   ```

   Or save to a file to open later:

   ```powershell
   python tools\udp_to_wireshark.py --out capture.pcap
   ```

DS frames show up with protocol **PictoChat** and an `[DS]` info prefix. Beacons
get their Nintendo vendor IE broken out; data frames expose the payload as a
labelled scaffold.

## USB-only handshake diagnostics

If your PC needs to stay on its normal WiFi network, the WROOM can capture
Nintendo authentication, association, and disconnect frames over USB instead:

```powershell
pio run -e esp32dev_serial -t upload
pio device monitor -e esp32dev_serial
```

This mode listens on fixed channel 7 (`CAPTURE_CHANNEL`) without a SoftAP.
`MGMT` lines contain RSSI, driver-reported frame hex (including its four-byte
trailer), and a cumulative queue drop count. Do not interpret the trailer as
information elements or application data. `rx_us` is the radio's 32-bit local
receive timestamp in microseconds (wraps after ~71 minutes); the log prefix and
PC timestamps are delayed output times, unsuitable for packet timing.
The current `esp32dev_serial` configuration selects C6 (`00:09:bf:c6:c6:c6`).
Add `-D SERIAL_TRACE_JORDAN=1` to its build flags and rebuild/flash WROOM to select
Jordan (`00:22:d7:39:bc:a3`) for a reference capture. For a real-DS comparison, power off C6,
start USB recording on COM6, enter Room B on Jordan first, then join with Ash
(`64:b5:c6:9c:60:a0`). Check the boot log's `trace host=` before joining. Remove
`-D SERIAL_TRACE_JORDAN=1` from that environment and rebuild/flash WROOM to restore
the C6 target; neither setting changes the C6 host's identity or firmware.

A working real-DS reference (Jordan host, Ash client, with Ash powered off until
Jordan is in Room B) shows 64 startup MP frames: 29 CMD, 28 CMD-ACK, one empty
reply, and six body-bearing replies. Ash sends type 6 after a granted type-5 CMD,
followed by Jordan's type-4 roster. Granted CMD Duration is `0x04e0`, ungranted
is `0x00f0`. RX timestamps repeat even in this working exchange: do not infer
latency.

After a successful association response from the selected host, up to 64 `MP` frames
(polls, replies, CMD-ACKs) are queued with full hex and RX timestamps. Duplicate
association responses with the same sequence/client do not restart that budget.
An 80-item queue buffers this bounded burst for 115200-baud output; check `drops`
before drawing conclusions from missing frames. Outside that startup window,
only management frames and counters are logged. `USB MP totals` reports cumulative
host CMD, empty reply, and body-bearing reply counts every five seconds. Reply
payload byte counts exclude the trailer. This is not a full PCAP replacement.
Restore UDP streaming with `pio run -e esp32dev -t upload --upload-port COM6`.
The C6 host firmware is not changed by flashing the WROOM.

Capture both boards with `python tools/capture_serial.py --seconds 120 --label trial`.
It records each port independently under the actual date; `--ports COM12` selects
only the host. Start capture before asking the DS to join.

The portable filter test can be run with a complete native C toolchain:
`gcc -std=c11 -Wall -Wextra -Werror -I lib/pictochat/include tests/test_handshake_filter.c -o build/test_handshake_filter`
then execute `test_handshake_filter`. The same command with
`tests/test_host_sequence.c` tests admission waiting, roster/identity ordering and reset;
`tests/test_host_admission.c` tests type-6 address/type/size/trailer validation;
`tests/test_host_poll_fields.c` tests grant cadence, independent WM toggling and reset;
`tests/test_mp_reply.c` checks MP reply classification and trailer exclusion;
`tests/test_mp_trace.c` checks startup trace selection, limits, retry handling,
and empty/body-bearing replies for both C6/Jordan and Jordan/Ash MAC pairs.
The ACK ownership test uses C11 atomics and pthreads:
`gcc -std=c11 -Wall -Wextra -Werror -pthread -I lib/pictochat/include tests/test_ack_gate.c -o build/test_ack_gate`.

## C6 host bring-up findings

`esp32c6host` gives each roster, heartbeat, or identity CMD its own reply/ACK
cycle, and each join resets the identity schedule. The following conclusions came
out of getting a DS to admit the C6 host and enable Send:

- **Admission gating.** The host waits on type-5 polls until a complete, validated
  type-6 reply arrives from the associated client, then sends seven type-4 roster
  polls (matching the reference, not a proven required count) before the ordered
  identity sequence. The parser accepts the observed 108-byte payload with declared
  size `0x0068` and excludes the four-byte capture trailer. Gating, grant policy
  (first typed poll granted after each join, one-in-three cadence, WM toggled on
  every typed poll independently of grants), and type-5-before-type-4 startup
  order did not by themselves produce admission.
- **Duplicate ACKs.** Callback and fallback send paths could both ACK the same
  cycle. Atomic ACK ownership now lets whichever path claims first send.
- **ACK timing.** The fallback ACK was submitted 44–46 µs before the driver
  reported CMD completion. It now waits for successful CMD completion before its
  1300 µs reply window; RX-triggered ACKs keep atomic ownership.
- **2 Mbps long preamble** is what reached type-6 admission (PHY code 1 on both C6
  TX completion and WROOM RX). On this C6/ESP-IDF build, the rate APIs return
  `ESP_FAIL` before Wi-Fi startup even with 802.11b selected first;
  `esp_wifi_config_80211_tx()` succeeds after startup with PHY mode 11B and
  `WIFI_PHY_RATE_2M_L`, contrary to the installed header's instruction to configure
  before startup.
- **CMD Duration** reads nonzero at PPDU setup (`hal_mac_tx_set_ppdu`) and EDCA
  config (`hal_mac_tx_config_edca`) but is zero on air; writing the Duration
  register (`0x600a54c0 - queue*116`) directly also left it zero and broke C6
  reception, so that was reverted. Admission works with zero Duration, so nonzero
  Duration is not necessary. The firmware keeps only read-only PPDU/EDCA probes.
- **Roster footer.** After admission, type-4/5 CMD footers target the granted
  client mask instead of zero. This enabled Send; see
  [IDENTITY_EXCHANGE.md](IDENTITY_EXCHANGE.md). Drawing relay and host replies are in
  [DRAWING_TRANSFER.md](DRAWING_TRANSFER.md).
- **WROOM timestamps.** `rx_us` values repeat across packets even with modem sleep
  disabled (`esp_wifi_set_ps(WIFI_PS_NONE)`; ESP-IDF defaults to
  `WIFI_PS_MIN_MODEM` regardless of `CONFIG_PM_ENABLE`, see
  [Espressif issue #2468](https://github.com/espressif/esp-idf/issues/2468)).
  **Precise latency measurements are not validated**; do not infer ACK latency
  from these traces.

## What's solid vs. what's a scaffold

- **Captured message decoding:** `python tools/render_canvas.py tests/fixtures/send.pcap -o captures_out`
  reassembles complete announced transfers, including short final fragments,
  then removes the 36-byte message header and detiles the bitmap. It no longer
  mistakes sequence footers for pixels or masks/inpaints the resulting artifacts.
  Missing fragments are reported as incomplete rather than filled from other
  messages. `--all` renders every complete decoded message. This is offline
  capture decoding; the separate live ESP drawing path is documented in
  `docs/DRAWING_TRANSFER.md`.
  Regression tests: `python -m unittest discover -s tests -p test_message_transfer.py -v`.
- **Solid:** 802.11 capture, Nintendo OUI filtering, radiotap + libpcap
  encoding, live Wireshark streaming, beacon vendor-IE detection.
- **Scaffold (reverse-engineering in progress):** the PictoChat *application*
  payload — chat-room state and the drawn-message bitmaps — is only partially
  documented publicly. `pictochat.lua` marks those fields as hypotheses. Confirm
  offsets against live captures before trusting them; that's the fun part.

## Files

| Path | Purpose |
|------|---------|
| `platformio.ini` | PlatformIO build config (ESP-IDF framework) |
| `firmware/esp32/main.c` | ESP32 firmware: capture, filter, stream/discover |
| `lib/pictochat/include/pictochat/nintendo.h` | Nintendo OUI table + DS notes |
| `firmware/esp32/CMakeLists.txt` | ESP-IDF component registration for the radio adapter |
| `sdkconfig.defaults` | ESP-IDF Kconfig defaults |
| `tools/udp_to_wireshark.py` | Bridges PCAP-over-UDP into Wireshark or a file |
| `tools/pictochat.lua` | Wireshark dissector for DS / PictoChat frames |

## Tuning notes

- **Channels 12/13:** allowed only in some regions. If your DSs land there, call
  `esp_wifi_set_country()` in the firmware for a region that permits them.
- **FCS trailer:** if Wireshark shows a bogus 4-byte trailer, set
  `#define FCS_AT_END 0` in `firmware_config.h`.
- **Dropped frames:** raise `CAP_QUEUE_LEN` in the firmware if the heartbeat
  count outruns Wireshark's.

## Legal / ethical

This is for inspecting **your own** consoles on **your own** local wireless, for
learning and reverse-engineering. Don't capture traffic you're not authorized to.
