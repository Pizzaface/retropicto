# PictoChat / DS Local Wireless — Reverse-Engineering Notes

Findings from live captures with this project (ESP32 promiscuous → Wireshark).
Confidence is marked per item. This is a working document — correct it as more
captures come in.

## Radio / addressing (confidence: high)

- 802.11b, 2.4 GHz. Observed session on **channel 7** (radiotap 2442 MHz).
- Host DS MAC `00:22:D7:39:BC:A3` (OUI `00:22:D7`). Client `64:B5:C6:9C:60:A0`
  (OUI `64:B5:C6`, Nintendo, newer block — caught via shared BSSID).
- Two addressing styles seen across sessions:
  - **Broadcast/port style:** host → `03:09:BF:00:00:xx` (locally-administered
    multicast). `xx=00` = message/canvas stream, `xx=03` = poll ticks.
  - **Unicast style:** host → client MAC directly (seen during active drawing).
- Beacons: host → `FF:FF:FF:FF:FF:FF`, 60-byte payload.

## Message transfer (revalidated September 24)

Payload length alone does not identify a stroke or a canvas row. Earlier notes
misclassified short fragments and transfer announcements as drawing events.
For the decoded client replies, after the 802.11 header and before capture FCS:

| Offset | Field |
|---|---|
| 0 | WM header, 2 bytes |
| 2 | Application type, little-endian u16 |
| 4 | Application size including its header, little-endian u16 |
| 6 | Sender slot |
| 7 | Payload type |
| 8 | Fragment data length (type 2) |
| 9 | Transfer flags (bit 0 marks the final fragment) |
| 10 | Transfer byte offset, little-endian u16 |
| 12 | Two reserved/magic bytes |
| 14 | Fragment data, exactly the declared number of bytes |
| after data | Two-byte sequence footer, not pixels |

Type 0/1 announcements declare the total transfer length at application offsets
8-9. Type 2 has a 12-byte application header plus its data. Host CMDs use a
6-byte prefix (client time, grant bitmap, WM) and a 4-byte footer instead of the
client's 2-byte prefix/footer. `python/pictochat/message.py` validates these
layouts, separates senders, handles identical retransmissions, and requires
complete coverage plus the final fragment before emitting a message.

`send.pcap` announces 2,084 bytes: 36 bytes of message metadata followed by
2,048 bitmap bytes. Its final fragment contains only four bytes at offset
2,080. `corners.pcap` announces 10,276 bytes with a 36-byte final fragment at
10,240, but only 9,636 transfer bytes were captured; it is incomplete.

**Correction:** the old renderer started client fragment data at byte 16,
losing its first two bytes and appending the sequence footer as pixels. The
resulting artifacts were incorrectly described as counters overwriting pixels.
The counters do not overwrite pixels. The message's 36-byte metadata header
also must be removed before detiling; masking the first tile is incorrect.
The corrected renderer neither masks nor inpaints pixels, and refuses to merge
missing fragments from unrelated messages. `--inpaint` is now an obsolete no-op.

## Pixel format (confirmed on complete captured messages)

- 256 pixels wide, 4 bits per pixel in row-major 8x8 tiles (32 bytes per tile).
- Low nibble is the left pixel; zero is blank and nonzero is ink in the
  monochrome renderer. `pictochat.canvas.detile_indices`/`tile` keep the 0..15
  index per pixel; `--colour` on the render/export tools writes an indexed PNG
  with `canvas.PALETTE`, the retropic.to display palette (not DS-verified).
- Bitmap height follows the announced transfer length after removing 36 bytes
  of metadata; observed complete examples include 16 and 64 pixels. The maximum
  supported bitmap is 256x80 (10,240 bitmap bytes).
- The corrected `send.pcap` reconstruction renders "12345" without artificial
  gaps. Run `python tools/render_canvas.py send.pcap -o captures_out`.
- These are captured message transfers. Live ESP message reception/transmission
  and incremental drawing semantics still require implementation and DS tests.

## Join / association handshake (confidence: high — CAPTURED)

Captured fresh in `join01.pcap` by recording the joiner DS entering the host's
room (repeated ~4x). Host `00:22:D7:39:BC:A3` = room creator/AP; joiner
`64:B5:C6:9C:60:A0`. **Roles are inverted vs. infrastructure Wi-Fi: the HOST
initiates auth *and* the assoc-request toward the joiner.** Sequence:

1. **deauth** host→joiner, reason **3** (body `03 00`) — clears prior state.
2. **auth** host→joiner — **Open System** (algo 0), seq **1**, status 0
   (`00 00 01 00 00 00`). No crypto challenge.
3. **auth** joiner→host — algo 0, seq **2**, status 0 (`00 00 02 00 00 00`),
   **~1 ms** after (2).
4. **assoc-req** host→joiner — cap `0x0021` (ESS+ShortPreamble), listen=1, 42-byte
   body carrying a **per-join counter** (byte 10: e5→e7→ea…) and trailing rates
   IE `01 02 82 84` (1 & 2 Mbps, basic). ~4 ms after (3).
5. **assoc-resp** joiner→host — cap `0x0021`, **status 0 = success, AID `0xC001`**
   (body `21 00 00 00 01 c0 01 02 82 84`), **~1–1.5 ms** after (4).

Observed a **failure** variant too: assoc-resp **status 1, AID 0**
(`21 00 01 00 00 00 …`), immediately retried from step 1.

**Implications for TX:** auth is open-system (no secret), so the frames are
trivial fixed templates. Timing is NOT the ~1 ms wall it first appeared — the
host retries over seconds and once accepted a 318 ms-late assoc-resp.

### TX / join injection experiment (2026-08-08) — findings

Second board (ESP32-C6, COM12) flashed with `MODE_JOIN`: impersonates the joiner
MAC, spams the presence data/1 frame, answers auth/assoc.

- **Injection works** — 23,915 presence frames confirmed on air.
- **But the host never engaged the C6.** When no client is active the host just
  **beacons** (no CF-Poll/PS-Poll — the ESP32 delivers **zero 802.11 control
  frames** even with `WIFI_PROMIS_FILTER_MASK_CTRL`; DS local wireless doesn't use
  them).
- **The real "poll layer" is a host→joiner `data/1` stream at ~100 Hz** (body
  `00 80 00 00 e4 09`, seq incrementing). The host only polls MACs it is already
  engaged with; it does not poll unknown MACs. So an unknown injector never gets
  polled and never gets auth'd.
- **The re-join trigger is uncaptured.** Across every clean capture there is a
  ~2.8 s blackout (host stops beaconing / heavy loss) immediately before each
  auth. Whatever the joiner sends to re-engage the host lives in that blackout and
  the single-radio ESP32 sniffer loses it every time.
- Joiner→host `deauth` (reason 3) is what the joiner sends when the user *leaves*
  a room.

**Open blocker:** to join, the C6 must reproduce the DS's beacon-TSF / TDMA slot
sync so the host will engage it — not just replay frames. Capturing the trigger
needs a non-lossy monitor (a real monitor-mode adapter), not the ESP32 sniffer.

## Incremental drawing (unverified)

The former size-based stroke/row interpretation is superseded by the validated
transfer layout above. Offsets stepping by 160 identify byte chunks, not image
rows. Distinguishing live drawing events from sent-message transfers still needs
fresh, labelled captures.

## Still unknown / TODO

- ~~Exact canvas width / origin / bit order~~ — SOLVED: 256x80, 4bpp 8x8 tiles.
- The old image breakup was a decoder offset error, now fixed by validated
  fragment extraction and removal of the 36-byte message header.
- ~~The **join / association handshake**~~ — **CAPTURED** in `join01.pcap` and
  fully decoded (Open-System auth, host-initiated, ~1 ms responses). See the
  "Join / association handshake" section above.
- **TX feasibility**: whether `esp_wifi_80211_tx()` can meet the DS TDMA
  response timing well enough to be accepted as a participant. Untested; this is
  the biggest risk for the "send a message" goal.
- Meaning of the `81 93 32 02` / `9f c5 36 02` fields — appear as the per-event
  hash at **bytes 10–13 of the 114 B poll/sync frames**; still unconfirmed.
