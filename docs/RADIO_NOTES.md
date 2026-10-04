# Historical ESP32 radio notes

> Historical captures and build snapshots referenced below were removed during
> cleanup. Protocol code now lives in `lib/pictochat`; see [the library guide](LIBRARY.md).

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

The September 22 reference `real-ds-jordan-host-175637-COM6.log` captured a working
Jordan-host/Ash-client startup after explicitly powering Ash off until Jordan was
in Room B. The first attempt instead had Ash as host and did not arm the Jordan
MP filter. The successful window contains 64 MP frames (29 CMD, 28 CMD-ACK,
one empty reply, six body-bearing replies), with zero reported queue drops.
Ash sends type 6 after a granted type-5 CMD, followed by Jordan's type-4 roster;
the user confirmed messages both ways. Granted CMD Duration is `0x04e0`, ungranted
is `0x00f0`; the C6 trace has zero. These are comparisons, not established causes.
RX timestamps still repeat in this working real-DS exchange: do not infer latency.

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

The admission-gated C6 experiment now waits on type-5 polls until a complete,
validated type-6 reply arrives from the associated client. It then sends seven
type-4 roster polls (matching this reference sample, not a proven required count)
before the existing ordered identity sequence. A short critical section protects
admission state and client identity; ACK transmission and logging stay outside it.
The parser accepts the observed 108-byte payload with declared size `0x0068` and
excludes the four-byte capture trailer. The admission-only trial
`mp-trace-200521-COM6.log` recorded 40,993 polls and 3,824 empty replies with no
body-bearing replies; no C6 disconnect was logged during the ten-minute capture.
The first 64 MP frames were one empty poll plus 31 type-5 polls and 32 ACKs, with
zero reported queue drops. Jordan stayed connected but had no PICTOBOT or Send.
No admission occurred, so the gated identity exchange did not run.

The next isolated grant-policy experiment grants the first typed poll after each
join and toggles WM on every typed poll independently of grants. It preserves
one-in-three grant cadence (polls 1, 4, 7, ...), rather than exactly replaying the
reference's first/fifth grants. The initial empty poll does not advance this state.
This applies to type-5/type-4 polls; identity-frame builders remain unchanged.
Duration handling, ACK delay, admission gating, trailer counter and application
payload are unchanged. In `mp-trace-105726-COM6/COM12.log` (September 24 timestamps),
the first type-5 grant and independent low/high WM toggles were verified on air.
Jordan remained connected during the observation but sent only empty replies;
PICTOBOT and Send did not appear.

A separate C6-only TX Duration diagnostic wraps `hal_mac_tx_set_ppdu` without
changing frames. The final linked ELF confirms `lmacSetTxFrame` calls the IRAM
wrapper, which calls the original setup function. In `mp-trace-115130-COM6/COM12.log`
(September 24 timestamps), the probe saw 9,745 CMD frames with nonzero Duration
both before and after setup (`before_zero=0`, `after_zero=0`, `changed=0`), while
all 31 sampled on-air CMDs had Duration zero. WROOM counted 8,540 CMDs, 481 empty
replies, and zero data replies; C6 recorded 481 replies in its own window. These
counter windows differ and should not be equated. This localizes the discrepancy
to after the observed setup point or to an alternate transmitted representation;
it does not prove a silicon limitation or that Duration alone gates admission.
The USB capture was stopped deliberately after 147 seconds, so its process exit
code 1 does not indicate a firmware or capture-format error.

September 24 follow-up: `captures_out/2026-09-24/edca-repeat-121635-971785-*`
extends the read-only probe to the linked `hal_mac_tx_config_edca` call, after
PPDU setup and before queue enable. It counted 8,351 CMDs with nonzero header
Duration there; all 95 sampled WROOM CMDs had zero Duration and queue drops were
zero. No body-bearing replies were observed. The first diagnostic boot failed
to associate; the previous binary and a repeat of the identical diagnostic both
associated, so that initial failure was not reproducible. User confirmed room B,
no PICTOBOT, and disabled Send with the restored previous firmware.

`nav-active-121919-637860-*` then wrote the ROM-derived Duration register
(`0x600a54c0 - queue*116`) from that verified hook, saving/restoring each queue's
prior value. All 2,229 CMD writes read back correctly on queue 0, but all 17
sampled on-air CMDs still had Duration zero. C6 saw zero replies while WROOM
continued counting empty replies. The mutation was **reverted in source and on
the board**; `edca-restored-122009-839635-*` confirms C6 reception recovered
(86 replies / 2,094 CMDs). These experiments narrow the transmit discrepancy;
they do not prove Duration is the admission blocker. The current firmware keeps
only the read-only PPDU/EDCA probes. Experimental binaries/sources are archived
under the dated capture directory. Deliberately interrupted capture processes
exit 1; their line-buffered logs remain usable.

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

The experimental `esp32c6host` mode now gives each roster, heartbeat, or identity
CMD its own reply/ACK cycle. Each join resets the identity schedule; announcements
and the two profile stages no longer run on independent modulo counters.
September 22 hardware testing showed PICTOBOT rendering on Ash and Jordan, but
Send remained disabled and connection errors followed. Rendering is only a partial
result; the full session and Send path are not working yet.
The next isolated experiment changes the six startup polls from type 4 to type 5,
following an empty poll, with the later identity sequence and all other fields
unchanged. Working captures show type 5 before client type-6 admission, then type 4.
The September 22 type-5-first test still rendered PICTOBOT and then disconnected
on Jordan (two joins, approximately 6.1 and 6.7 seconds). In the first join, WROOM
observed 436 host polls and three empty replies, matching C6 counts; no body-bearing
reply was observed. Startup order alone did not resolve the failure.

The subsequent WROOM startup trace captured two distinct, non-retry CMD-ACKs
following one empty client reply, consistent with competing callback/fallback
send paths. Both receivers counted 931 polls and four replies across two joins;
WROOM reported no body-bearing replies or queue drops. Its `rx_us` values repeated
across multiple packets, so **precise latency measurements are not validated**.
The next WROOM-only diagnostic disables modem sleep with
`esp_wifi_set_ps(WIFI_PS_NONE)` and verifies/logs the setting before capture.
ESP-IDF defaults to `WIFI_PS_MIN_MODEM` independently of `CONFIG_PM_ENABLE` (off
in this build); the RX timestamp contract requires modem/light sleep to be off.
[Espressif issue #2468](https://github.com/espressif/esp-idf/issues/2468) reports
this exact symptom and fix. Hardware testing confirmed the setting changed from
1 to 0, but **did not resolve repeated timestamps**: the first startup window in
`mp-trace-174033-COM6.log` had 43 repeats among 63 adjacent MP pairs (the older
trace had 255/315). The initial `173405` recording captured no join/MP frames and
cannot validate timing. Non-repeating timestamps would only be a necessary sanity
check, not proof of calibrated RF latency. C6 was unchanged; Jordan still rendered
PICTOBOT and then disconnected. Do not infer ACK latency from these traces.
The next isolated C6 experiment uses atomic ACK ownership: whichever path claims
first sends, and the other skips that cycle. Timing, Duration, and startup payloads
are unchanged. This prevents duplicate submissions within a cycle; it does not
resolve late-reply attribution or prove a Send fix. The September 22 single-ACK
trial still rendered PICTOBOT and disconnected. Across five sampled startup windows
(320 MP frames, including three replies), no consecutive CMD-ACK pairs were observed
and queue drops remained zero. This supports the duplicate-submission fix, but no
body-bearing reply or successful admission was observed; Send remains unresolved.

The September 24 TX-completion diagnostic found a separate scheduling defect:
for all 15 sampled full CMD frames, the fallback submitted ACK 44–46 microseconds
before the driver reported CMD completion. The fallback now waits for successful
CMD completion before starting its 1300-microsecond reply window; RX-triggered ACKs
retain atomic ownership of the cycle. The corrected capture had no early ACK
submissions, 3563 CMD and ACK completions each, 102 received empty replies and
102 RX-triggered ACKs, with zero reported TX failures, rejections, or trace drops.
These are software event measurements, not calibrated RF timings. Jordan still
showed **no PICTOBOT and Send disabled**; neither receiver observed a body-bearing
reply or type-6 admission. Keep the scheduling fix, but admission remains unresolved.
Evidence: `captures_out/2026-09-24/tx-completion-122948-174622-COM12.log`
and paired `tx-window-fix-123209-621054-COM12.log` / `COM6.log` captures.

The following 2 Mbps long-preamble trial reached validated type-6 admission on
three joins. Both C6 TX completion and independent WROOM RX reported PHY code 1
(2 Mbps long preamble); WROOM observed body-bearing replies, including type-6
admission and repeated type-3 packets. Jordan displayed PICTOBOT, but Send
remained disabled and a connection error followed. Admission is now demonstrated;
the identity exchange and messaging remain incomplete. This trial also retained
the completion-based ACK timing fix. Host CMD Duration still measured zero on air,
so nonzero Duration is not a necessary condition for this observed admission.
Evidence: `captures_out/2026-09-24/rate-2m-started-125449-822924-COM12.log`
and its paired COM6 log; firmware/source snapshot in `rate-2m-admission/`.

On this C6/ESP-IDF build, both rate APIs returned ESP_FAIL before Wi-Fi startup,
even with 802.11b selected first. `esp_wifi_config_80211_tx()` succeeded after
startup with PHY mode 11B and rate `WIFI_PHY_RATE_2M_L`. The linked implementation
checks for an allocated interface; see `c6-rate-api-disassembly.txt` in that
capture directory. This observed behavior differs from the installed header's
instruction to configure before startup. Earlier failed startup captures are
retained as `rate-2m-long-*`, `rate-2m-long-b-*`, and `rate-2m-phy-*`.

The subsequent roster-footer correction enabled Send. After admission, type-4/5
CMD footers now target the granted client instead of always using mask zero.
The DS uploaded both identity stages, which the host relayed; the user confirmed
Send enabled and a roughly 102-second session ending by their own choice.
The working snapshot is `captures_out/2026-09-24/roster-target-build/`, with paired
`roster-target-mask-132416-595966` logs. See `docs/IDENTITY_EXCHANGE.md` for packet
evidence and tests. The drawing relay and host reply are now implemented. The
first live drawing was received and exported as a clean “123” image, and Send
recovered. After requiring a DS reply before advancing granted application
polls, the user confirmed two correct PICTOBOT replies and Send recovery after
each. Four drawings were received and replied to in that session with verified
body checksums. The working firmware is preserved in
`captures_out/2026-09-24/drawing-reply-confirmed-build/`. See
`docs/DRAWING_TRANSFER.md` for the implementation and validation evidence.

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
