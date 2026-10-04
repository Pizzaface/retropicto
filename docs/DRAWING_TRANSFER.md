# Drawing receive/relay and PICTOBOT reply

## Multi-client delivery follow-up

After the sender-slot correction, two-client operation worked but was slow, and
both original messages and bot copies could be missing. The first room adapter sent bot replies only to the original sender. It now retains
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
tests and the delivery sanitizer check pass. In a two-console test, both originals
and bot copies arrived: two complete incoming drawings (10,276 and 8,228 bytes),
each forwarded to the other DS, and both bot copies transmitted to both consoles,
with no reported queue drops. The original forwards completed about 5.5–7.1 seconds after host reception; the
last bot copies completed about 11.7–14.8 seconds after reception. This verifies
the tested exchange, not sustained reliability or acceptable latency at capacity.

Relaying identities alone enables Send, but sending the first message then
disables Send again: the host must also relay drawing announcements and 172-byte
drawing fragments.

## Wire format and evidence

`send.pcap` contains a complete 2084-byte message: 36 metadata bytes followed by
a 2048-byte tiled bitmap. Data arrives in 160-byte chunks, with a four-byte final
chunk and duplicate packets. The two small fixtures in `tests/fixtures/` contain
the captured client application packets (each prefixed by a little-endian 16-bit
length) and the independently decoded message body. They were extracted with
`tools/analyze.py` and `python/pictochat/message.py`.

In a working two-DS reference capture (`perfect01.pcap`, not in this repository),
host-originated first drawing fragments (parser indexes 12461 and 12464) use header `0200ac000000a00000000000`, sender slot 0, and an 86-word
application packet. The first two message bytes are `0302`; sender MAC bytes at
offset 2 are halfword-swapped. The new PICTOBOT sender uses this fragment layout,
copies the captured announcement descriptor/bitmap metadata, changes the sender
slot and MAC, and assigns a fresh announcement token.

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

## Live results

- Live receive and rendering work for short (2,084-byte) and full-height
  (10,276-byte) drawings; serial dumps pass coverage/checksum validation.
- Driver completion (`DRAW outbound TX complete`) did not establish application
  delivery: early builds re-enabled Send but showed the PICTOBOT reply only
  intermittently, sometimes with shifted pixels. Repeating bot announcements and
  fragments three times did not fix this. The independent sniffer can miss
  fragments during long transfers, so it is not evidence of complete on-air
  delivery.
- The fix was to require a client reply before advancing an admitted, granted
  application poll: wait up to 5 ms, restore application state on timeout, keep
  advancing the WM sequence per transmission, and send no fallback ACK for missing
  replies. Ungranted/pre-admission polls are unchanged. This follows the Rust
  runner's reply-based advancement; it does not establish application-level
  receipt or solve late-reply attribution.
- With that change, PICTOBOT replies displayed correctly on one DS and Send
  recovered after each of four consecutive drawings (two 10,276-byte, two
  2,084-byte). Recomputing each bot body from its received body with only the
  sender MAC rewritten reproduced the logged outgoing checksums. Leaving the room
  disconnected cleanly. This covers one DS, not an extended unattended soak.

## Room delivery baseline before event hooks

A two-client log recorded two connected
clients and two complete messages: 10276 bytes from AID 2 (FNV-1a `598c97e2`), then
8228 bytes from AID 1 (`beb7bec8`). Each original drawing completed transmission to
the other client, and its host-authored reply completed separately to both AIDs.
The rewritten host bodies had hashes `1ddd551d` and `668a99dd`, respectively.
These were observed protocol completion logs, not independent screen confirmation.

The subsequent event refactor routes these same application reactions through
`host_room_event`. `MESSAGE_RECEIVED` occurs once per completed incoming drawing;
`MESSAGE_SENT` occurs per outbound recipient, including forwarded user drawings.
Filter `sender_slot == 0` to react only to host-authored sends. A two-client room can
therefore emit three SENT events per incoming drawing: one forwarded original and
two echo replies. The event code is covered by native tests and the hardware trial below.


## Event callbacks: two-DS trial

With `esp32c6host` and two DS consoles at READY (AIDs 1 and 2), PICTOBOT replies
displayed correctly and Send became available again after each drawing. Three
drawings sent by AID 1 were checked against the complete serial dumps:

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
operation were not covered by these three drawings.
