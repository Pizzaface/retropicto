# Drawing receive/relay and PICTOBOT reply

## Multi-client delivery follow-up

After the sender-slot correction, the user reported that two-client operation worked
but was slow, and that both original messages and bot copies could be missing.
The first room adapter sent bot replies only to the original sender. It now retains
one reply body/token and a recipient-generation snapshot until every current member
has an available outbound buffer or has disconnected. Original-user forwarding has
priority over bot copies. The bounded bot queue holds four completed drawings.

The room also owns application sequence counters across recipients; per-client
sessions no longer reuse the same host sequence numbers. Failed TX restores the
counter, while a missing reply advances it for retry. This is consistent with
transmitter-scoped sequencing in the
[reference runner](https://github.com/mjwells2002/foa_dswifi/blob/dev/foa_dswifi/src/runner.rs),
while retaining this library's separate counters per WM port.

`test_room_delivery.c` checks bot delivery to both recipients, a busy recipient,
failure/no-reply retries, shared sequencing, and reconnect isolation. All 14 native
tests and the delivery sanitizer check pass. The updated C6 firmware was built,
flashed and hash-verified. In the 2026-09-24 two-console test, the user confirmed
that both originals and bot copies arrived. The `room-delivery-fixed-160441-150722`
capture records two complete incoming drawings, each forwarded to the other DS,
and both bot copies transmitted to both consoles, with no reported queue drops.
The original forwards completed about 5.5–7.1 seconds after host reception; the
last bot copies completed about 11.7–14.8 seconds after reception. This verifies
the tested exchange, not sustained reliability or acceptable latency at capacity.

> Historical evidence: experimental captures, logs and binary snapshots cited below
> were removed during cleanup. Retained regression data lives in `tests/fixtures/`.

The working roster-target firmware enabled Send, but the user reported that
sending the first message disabled Send again. That firmware relayed identities
only: drawing announcements and 172-byte drawing fragments were rejected. The
new message path is implemented and under test; live messaging is not yet proven.

## Wire format and evidence

`send.pcap` contains a complete 2084-byte message: 36 metadata bytes followed by
a 2048-byte tiled bitmap. Data arrives in 160-byte chunks, with a four-byte final
chunk and duplicate packets. The two small fixtures in `tests/fixtures/` contain
the captured client application packets (each prefixed by a little-endian 16-bit
length) and the independently decoded message body. They were extracted with
`tools/analyze.py` and `python/pictochat/message.py`.

Host-originated first drawing fragments at `perfect01.pcap` parser indexes 12461
and 12464 use header `0200ac000000a00000000000`, sender slot 0, and an 86-word
application packet. The first two message bytes are `0302`; sender MAC bytes at
offset 2 are halfword-swapped. The new PICTOBOT sender uses this fragment layout,
copies the captured announcement descriptor/bitmap metadata, changes the sender
slot and MAC, and assigns a fresh announcement token. Live DS interpretation of
the resulting host message remains to be checked.

## Implementation

- `host_identity.h` now relays bounded identity and drawing application packets:
  client type 0 becomes host type 1; type 2 is copied unchanged. It retains the
  announced transfer size for duplicate final-fragment retries.
- `host_message.h` reassembles one announced drawing by byte offset and coverage.
  It emits once only after full coverage and a final fragment. Conflicting data,
  invalid ranges, and incomplete transfers cannot become complete drawings.
- Supported drawings contain 36 bytes of metadata plus up to 10240 bitmap bytes,
  in whole 1024-byte tile rows. Reassembly buffers are static, not task-stack
  allocations. Fragment/application queues remain bounded.
- A completed drawing is queued for a separate PICTOBOT reply. Host fragments
  carry slot 0 and the host MAC; the bitmap is unchanged. The original client
  fragments are also relayed so the DS can finish its own send operation.
- Radio submission/completion failures restore outgoing state/counters. Reply
  generation uses the existing 2 Mbps and completion-based ACK scheduling.
- A lower-priority task emits complete original message bodies as `DRAW` records
  with byte offsets and FNV-1a checksums. `tools/export_drawings.py` rejects missing
  or corrupt serial dumps and exports `.bin` plus a rendered `.png`.

Run `python tools/export_drawings.py <serial-log> -o captures_out` after a test.
`DRAW outbound TX complete` means driver completion only; it does not prove that the DS
displayed the reply. The live test must check visible PICTOBOT replies and that
Send becomes available again after multiple consecutive drawings.

## Validation so far

`test_host_message.c` passes against the captured drawing, including exact relay
bytes, short/duplicate final fragments, one-time completion, host sender rewrite,
independently checked outgoing offsets/pixels, missing coverage, conflicting
overlap, new-transfer reset, and truncated input. Identity, admission sequencing,
and footer tests also pass. The five dump-export tests cover exact bytes,
missing data/end markers, checksum corruption, and reused IDs after a reboot.

## First live drawing, 2026-09-24

The drawing firmware built successfully and was flashed with a verified image
hash. Its source and binary snapshot are in
`captures_out/2026-09-24/drawing-relay-build/`.

The paired `drawing-relay-134430-936453` logs record admission and both identity
stages, followed by a complete 2084-byte client message at 13:44:56.717. The dump
has FNV-1a checksum `46f98d8b` and sender MAC `00:22:d7:39:bc:a3`.
`tools/export_drawings.py` verified every byte and exported drawing 00/id1; visual
inspection shows a clean “123”. The host completed its separate reply at
13:44:57.119, with rewritten sender metadata and checksum `9cded394`.

This proves live receive and rendering for that drawing. The user confirmed
that Send became enabled again, but no PICTOBOT reply appeared. Driver completion
therefore did not establish application delivery. Later client transfers were
relayed without a completed receive dump; their missing coverage needs diagnosis.

The next diagnostic logs announcement/final-fragment coverage and extends the
independent sniffer beyond its initial 64-packet association window with a
separate 256-frame host application budget. Trace selection tests pass. No
message-format or delivery-timing fix is claimed yet.

The user subsequently confirmed a visible PICTOBOT copy, with shifted pixels
in a “123” message. Delivery is therefore intermittent or was initially missed;
pixel fidelity is not established. Additional verified dumps from this same
firmware render a clean “12345” (id2, 2084 bytes, hash `942a9d1e`) and a full-height
handwritten “Boink” (id3, 10276 bytes, hash `ae0c30d0`). These prove receive
coverage for both short and full-height messages, not correctness of the DS's
displayed bot copies. Radio capture of outgoing application bytes is next.
 
## Delivery diagnostics and retries

The `drawing-wire-135616-914595` capture contains the complete relayed “123”
(`46f98d8b`) but misses the bot's fragment at offset 1440 in the independent
sniffer, with no reported sniffer queue drops. That is evidence of a capture/radio
gap, not proof of exactly what the DS received. A larger incoming drawing reached
its final fragment with only 9636 of 10276 bytes covered.

Repeating bot announcements and fragments three times, as observed in native
host captures, did not resolve reliability: the user still reported only
occasional responses. The repeated-fragment fixture test passes.

The next build (`drawing-reply-confirmed-build`) requires a client reply before
advancing an admitted, granted application poll. It waits up to 5 ms, restores
application state on timeout, keeps advancing the WM sequence per transmission,
and does not send a fallback ACK for missing replies. Ungranted/pre-admission
poll behavior is preserved. This follows the local Rust runner's reply-based
advancement and timeout; it does not establish application-level receipt or
solve attribution of a late reply. ACK gate tests passed for fallback versus
actual reply tracking, reset, and 100 contested cycles. The build and flash
succeeded; `drawing-confirmed-140641-595442` is the pending live test.

## Successful live bidirectional test

The user confirmed that both requested replies displayed correctly and Send
recovered after each with the reply-confirmed firmware. The same session logged
four complete received bodies and four completed bot transmissions:

| Drawing | Bytes | Received FNV-1a | Bot FNV-1a |
|---|---:|---|---|
| 1 | 10276 | 22641c9a | 294dd985 |
| 2 | 2084 | 885dc349 | c2ad4e96 |
| 3 | 10276 | 756fb919 | e20690c6 |
| 4 | 2084 | 942a9d1e | 23f2a321 |

All four serial dumps passed coverage/checksum validation. Recomputing every bot
body from its received body with just the sender MAC rewritten reproduced the
logged outgoing checksums. Visual inspection of the short received messages
shows clean “Boink” and “12345”. The user supplies the evidence of correct DS
display; the independent sniffer dropped records during long transfers and is
not evidence of complete on-air delivery in this run.

The binary for that earlier trial was preserved at
`captures_out/2026-09-24/drawing-reply-confirmed-build/firmware.bin`, SHA-256
`25f98825e827b80e9eb2bd5dee1e3a1a1ac4b14bd679c86c5d4ab7a6c369dc02`.
The paired `drawing-confirmed-140641-595442` logs were saved before cleanup. The session joined
at 14:07:21 and disconnected at 14:08:58. The user confirmed they left room B
themselves, without a connection error. This validates one DS with PICTOBOT, not multiple
simultaneous clients or an extended unattended soak.

## Room delivery baseline before event hooks (2026-09-24)

The later `room-delivery-fixed-160441-150722-COM12.log` recorded two connected
clients and two complete messages: 10276 bytes from AID 2 (FNV-1a `598c97e2`), then
8228 bytes from AID 1 (`beb7bec8`). Each original drawing completed transmission to
the other client, and its host-authored reply completed separately to both AIDs.
The rewritten host bodies had hashes `1ddd551d` and `668a99dd`, respectively.
These were observed protocol completion logs, not independent screen confirmation.
The experimental logs were removed as generated artifacts during the approved cleanup.

The subsequent event refactor routes these same application reactions through
`host_room_event`. `MESSAGE_RECEIVED` occurs once per completed incoming drawing;
`MESSAGE_SENT` occurs per outbound recipient, including forwarded user drawings.
Filter `sender_slot == 0` to react only to host-authored sends. A two-client room can
therefore emit three SENT events per incoming drawing: one forwarded original and
two echo replies. The event code is covered by native tests and the hardware trial below.


## Event callbacks: successful two-DS trial (2026-09-24)

Built and flashed `esp32c6host` to COM12 with upload hash verification. Firmware
SHA-256: `2045f6c8958dc954fd674172ab0b4b5727f79889503b4132192bcbaa8b5adc2e`.
The paired capture is `captures_out/2026-09-24/event-hooks-164013-545435-COM12.log`
and `event-hooks-164013-545435-COM6.log`; verified bitmap exports are in
`captures_out/2026-09-24/event-hooks-rendered/`.

Both DS clients reached READY (AIDs 1 and 2). The user confirmed that PICTOBOT
replies displayed correctly and Send became available again after each drawing
with two DS consoles connected. Three drawings sent by AID 1 were independently
checked against the complete serial dumps:

| Drawing | Bytes | Received FNV-1a | Bot FNV-1a |
| --- | ---: | --- | --- |
| 1 | 8228 | `41aade92` | `c522d413` |
| 2 | 8228 | `c2e7be7c` | `5a7b2bc9` |
| 3 | 8228 | `808826e4` | `74781961` |

Each drawing produced one MESSAGE_RECEIVED callback and exactly three
MESSAGE_SENT callbacks: the unchanged original forwarded to AID 2, followed by
one host-authored echo to each AID. Rewriting only the sender MAC in each received
body reproduced the logged bot checksum. All three incoming callbacks reported zero
drawing drops. This validates callback-driven echo handling and per-recipient sent
notifications with two clients; simultaneous sends, reconnects, and four-client
operation were not covered by these three drawings. The five-minute capture
finished successfully and both serial ports were released.
