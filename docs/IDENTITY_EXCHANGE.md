# Identity exchange: observed gap after 2 Mbps admission

## Multi-client sender-slot correction

The September 24 two-console trial exposed an AID-1 assumption below the room
scheduler: client identity receive, drawing receive, and RequestIdent still used
member slot 1. The independent sniffer captured the second client's announcement
as `000014000200ffff54000000e4513a026db06ee9` (member slot 2). The host advertised
both roster MACs and its own profile, but rejected that announcement and requested
the wrong identity slot. The exact announcement is retained in
`tests/test_host_identity.c`.

Each room session now configures its member slot from the roster/AID on join.
Requests, identity validation, drawing assembly, and bot reply validation use that
slot. Cross-client identity replay and drawing forwarding retain the original
sender slot; slot 0 is reserved for host-originated data. Room tests now feed
distinct slots and identity MACs, including AIDs 9 and 15, instead of cloning
slot-1 packets into every session. Native regression checks cover this correction;
successful display and Send behavior still require the console trial.

> Historical evidence: experimental captures, logs and binary snapshots cited below
> were removed during cleanup. Retained regression data lives in `tests/fixtures/`.

The September 24 `rate-2m-started-125449-822924` capture reached type-6
admission on three joins. The DS displayed PICTOBOT, left Send disabled, and
reported a connection error. The bounded WROOM packet trace contains type-6
and repeated type-3 application packets; its final counters report 1220
body-bearing replies. The counters do not identify the types of packets outside
the bounded trace. No completed client identity transfer is proven by this run.

## Working reference

The following are zero-based packet indexes from `pictochat.capture.parse_pcap()`
on `perfect01.pcap`. Application bytes exclude the host's six-byte MP prefix
and four-byte footer, or the client's two-byte prefix and two-byte footer.
These observations describe one working exchange, not universal field semantics.

| Packet | Direction | Observed operation |
| --- | --- | --- |
| 471294 | client | Type 6 admission |
| 471301 | host | Type 1: `0100140000d0ffff54000000000000005858b351` |
| 471311 | host | Type 2: prefix `02006000002d540100000000`, body starts `0300`, host identity |
| 471312 | client | Type 0: `000014000100ffff54000000290069005e88a0d7` |
| 471318 | host | Same announcement bytes with type changed from 0 to 1 |
| 471328 | client | Type 2, slot byte 1, body starts `0300`, client identity |
| 471333 | host | Byte-identical application payload relayed from 471328 |
| 471340 | client | Type 3 |
| 471344 | host | Type 1: `010014000000ffff540000000c0000007a743d01` |
| 471345 | client | New type 0 announcement, slot 1, descriptor `29006900` |
| 471351 | host | Type 2: prefix `0200600000db540100000000`, body starts `0301`, host identity |
| 471358 | host | Relays announcement from 471345 as type 1 |
| 471365 | client | Type 2, body starts `0301`, client identity |
| 471372 | host | Byte-identical application payload relayed from 471365 |
| 471375 | client | Type 3 |

The host announcement and its following data packet share a footer sequence
in this reference: 471301/471311 use `660f`, 471318/471333 use `6b0f`,
471344/471351 use `700f`, and 471358/471372 use `750f`. Retransmitted or
ungranted copies can use incremented footer sequences. Do not infer all
retransmission rules from these pairs alone.

## Differences found in the 2 Mbps admission baseline

`host_send_id_announce(1)` always emits the slot-1 descriptor `29006900`,
but `host_send_id_data()` sends the host's slot-0 identity. The working reference
uses the slot-1 descriptor when relaying the client's announcement. Our current
announcements also generate a fresh random token on every repeat, while profile
frames retain a constant footer sequence of 1. The host never relays the client's
type-0 announcement or type-2 payload. Finally, it repeats both host identity
stages indefinitely without waiting for or tracking the client's transfer.

The next implementation must distinguish host identity transfers from client
relays, maintain transfer/sequence state, and respond to received application
packets. Type-3 field meanings and the exact condition enabling Send remain
unverified. A displayed name alone is not a completed identity handshake.

## Relay implementation under test

`lib/pictochat/include/pictochat/host_identity.h` now emits a slot-0 generic announcement and stage-0 host
profile, relays the client's type-0 announcement as type 1, and relays the
84-byte type-2 identity payload unchanged. It sends the host's 0x0c announcement
and stage 1 without waiting for client stage 0. It stops advertising identity
after both client stages have been relayed; waiting cycles send heartbeats.
The stage scheduling is a hypothesis to test, not proof that the DS
has enabled Send. The reference places a client type-3 packet between stages;
its semantics are not modeled yet.

The host task owns application state. The RX callback queues bounded identity
packets tagged with a join generation so stale replies cannot initialize a new
session. Application output uses separate increasing WM footer counters for
ports 13 and 14. Failed TX restores the pending packet/state/counters for retry.
Message bitmap transfers are not handled by this identity-only state machine.

`tests/test_host_identity.c` compares application bytes against captured fixtures
from the packet indexes above. It covers both stages and relay payloads,
malformed/unannounced input, pending-packet preservation, rejoin reset, and
state restoration for retry. `test_host_sequence.c` verifies the admission gate
and roster-to-application handoff. Both passed using `zig cc -std=c11 -Wall
-Wextra -Werror`; local MinGW `gcc` could not launch its compiler subprocess.
These tests do not prove radio reliability, WM footer semantics, or Send state.

The first relay trial (`identity-relay-131348-705333` paired logs) displayed
PICTOBOT with Send disabled. It sent only the first host profile stage and then
waited. The bounded trace showed admission followed by two-byte WM acknowledgments;
no client type-0/type-2 identity was queued or relayed. The DS remained associated
for over a minute in the first observed join; that is not proof of a completed
session or a permanent disconnect fix. The next revision removes that wait and
publishes both host stages, matching the Rust reference's own-profile burst.
The golden application-byte tests still pass, with an added no-client-reply case.

The two-stage trial (`identity-both-stages-131800-405889`) placed PICTOBOT in
the DS top bar, but the user confirmed Send remained disabled. Both stages were
sent, yet no client identity announcement/data reached the relay. The next
isolated addition is the Rust reference's `RequestIdent(1)` packet after the host
profile: `010014000100ffff5400000000000000b778d529`. This packet's role is taken
from the reference implementation, not established by the working capture;
its ability to solicit a DS identity is under hardware test.

The request trial (`identity-request-132026-936086`) did not enable Send. The
request was transmitted, and the DS responded with a type-3 packet rather than
a type-0 identity announcement. The sampled application response begins
`030014000169ffff0000000000213302af933202`. Neither type-0 nor type-2 client
identity was observed in the bounded trace or queued by the host. This refutes
the assumption that this request alone starts the missing client upload in the
current session. Keep the packet semantics provisional; application type 3 is
not treated as identity completion. The capture was stopped after the user
reported Send disabled, and the firmware snapshot is `identity-request-build/`.

## Verified admission and Send-enabled session

The next correction changed the type-4/type-5 footer's client-target mask from
constant zero to the poll grant mask after admission. Before admission it remains
zero, preserving the successful startup behavior. In 488 post-admission roster
and heartbeat packets from `perfect01.pcap` (parser indexes 471295–472499),
the footer mask matched the grant mask: 156 granted packets used 2 and 332
ungranted packets used 0. The local `runner.rs` transmitter independently names
and fills this footer field `client_target_mask` from the poll mask.

`roster-target-mask-132416-595966` then reached both client identity relay stages
and `identity_phase=6` at 13:24:35.272. The user confirmed **Send enabled**.
The session lasted approximately 102 seconds, and the user explicitly confirmed
leaving the room themselves; the disconnect was not a reported connection error.
C6 reported 6652 CMD and ACK completions each, zero TX failures/rejections/trace
drops, and the identity relay logged zero RX queue drops. The firmware/source
snapshot is `captures_out/2026-09-24/roster-target-build/`.

At the time (September 24) this was the working handshake baseline; drawing
reception, reassembly, and host-originated transmission were implemented and
hardware-validated afterwards (see [DRAWING_TRANSFER.md](DRAWING_TRANSFER.md)). Captured
drawings in `send.pcap` use 160-byte chunks (172-byte application packets), with
a four-byte final chunk in the complete 2084-byte message example.

## Configurable host Bio

`firmware/esp32/main.c` configures `host_profile_bio` as a UTF-16 string literal.
The portable setter is in `lib/pictochat/include/pictochat/host_profile.h`.
Change it, build `esp32c6host`, flash, and rejoin the room to load the new profile.
The example text is `Hi from PICTOBOT!`. This is a build-time setting.

The 84-byte identity has a 52-byte Bio field at offset 28, following the
20-byte nickname. The captured golden profile contains `Can you` at this
location, and the local Rust ConsoleId serializer independently uses the same
field ordering. The setter writes UTF-16LE, clears unused field bytes, rejects
more than 26 code units, and preserves the MAC, nickname, color, and birthday.
Both identity stages use the configured profile.

`tests/test_host_profile.c` checks the captured offset, little-endian non-ASCII
encoding, clearing, exact capacity, overflow rejection without mutation, and
both identity stages. These tests pass. Live DS Bio display is pending.
