# Portable library and Python port contract

The library provides a per-client session engine and a four-client room scheduler.
Wi-Fi association, channel selection, radio timing and a complete Python host remain
adapter responsibilities. Multi-client wire behavior is experimental until verified
with physical consoles. Captured constants are not claims that every DS variant uses
the same protocol.

## Multi-client rooms

Allocate `pictochat_room_t` in static/heap storage (roughly 100 KB). Its four storage
slots are independent of association IDs: accept AIDs 1..15 and pass the actual AID
to `pictochat_room_join`, with a nonzero generation changed on each new association.
Duplicate joins preserve state. Set the peer's `admitted` flag only after validating
its admission reply. All room calls and peer-state updates belong to one owner.

Feed input through `pictochat_room_receive(slot, generation, ...)`; retain input on
`-2` (backpressure) and discard stale generations. The room caches both identity
stages, replays them to other members, and forwards complete drawings while preserving
the source member slot, sender bytes and transaction token. Slot zero belongs to
the host; it is not a generic outgoing transfer channel. Existing admitted members are the drawing's
recipients; late joiners receive identities, not drawing history. Each source retains
one completed drawing until it has been copied into each recipient's outbound buffer.
Recipient buffers retry independently; a slow recipient does not stop radio polling
of other members. Bounded adapter queues can still fill if a member stops replying;
the adapter owns any timeout/disconnection policy.

`pictochat_room_prepare` selects one connected slot round-robin. Encode its application
using `pictochat_frame_target_app` and `1u << peer.aid`. For roster/heartbeat frames,
use `pictochat_room_members` and `pictochat_frame_room_members`: roster positions are
indexed by AID, with the host at position zero. Only one reply slot is granted per
cycle; this does not implement simultaneous multi-slot TDMA. Resolve each output with
`pictochat_room_finish`. Only a reply from the selected MAC/current generation counts
as delivery. Missing replies retry that recipient's payload. Membership changes must
wait until the outstanding output is resolved.

Application sequence counters belong to the room (one per WM port), not individual
recipients. They survive joins/leaves; failed radio transmission rolls back the
counter, while a transmitted poll with no reply advances it for the retry.

To send a host-authored reply to the whole room, capture recipients and one shared
token with `pictochat_room_delivery`, then call `pictochat_room_reply` with a validated
completed drawing. Retain the delivery ticket and body until it returns true.
Busy recipients remain pending; replacement association generations are removed.
Pending original-user drawings take priority over host replies. Completion here
means copied into each recipient's outbound buffer, not displayed on its screen.

`pictochat_room_leave` removes pending recipient bits, clears cached identity delivery
for the vacated slot, and refreshes the remaining members' rosters. Other sessions
survive. A drawing already copied into an outbound buffer remains deliverable even
if its original sender leaves. The original single-client frame helpers remain
available with their AID-1 defaults.

## Application events

Register `pictochat_room_set_handler(room, handler, context)` after room reset.
The registration survives individual joins/leaves; reset clears it. A NULL handler
unregisters notifications. Registration changes are refused during an outstanding
radio cycle or a running handler.

```c
static void on_event(void *context, const pictochat_event_t *event) {
    if (event->type == PICTOCHAT_MESSAGE_RECEIVED) {
        // Copy event->body[0..length) into your application's work queue.
    } else if (event->type == PICTOCHAT_MESSAGE_SENT) {
        // One recipient (event->aid/generation) has completed this transfer.
        // event->token identifies the drawing; sender_slot == 0 means host-authored.
    }
}
// Immediately after pictochat_room_reset(&room, profile):
// pictochat_room_set_handler(&room, on_event, application_context);
```

| Event | When it fires |
| --- | --- |
| `PICTOCHAT_PEER_JOINED` | a new MAC/AID/generation is installed; duplicate joins do not notify |
| `PICTOCHAT_PEER_LEFT` | an existing peer is removed, carrying its old identity/generation |
| `PICTOCHAT_PEER_READY` | the identity exchange reaches READY after successful transmission |
| `PICTOCHAT_MESSAGE_RECEIVED` | complete validated drawing reassembly, once despite duplicate fragments |
| `PICTOCHAT_MESSAGE_SENT` | final outgoing copy succeeds for one recipient, including required reply |

Queuing a room reply does **not** fire SENT. Failed transmission or a missing reply
on the final copy does not fire it either. Broadcast replies produce one SENT event
per actual recipient; they do not produce an aggregate all-members-delivered event.
A disconnected recipient never gets a success event for an unfinished transfer.
Forwarded user drawings also produce SENT; `sender_slot` preserves their author.
These events establish protocol completion, not proof of pixels displayed on a DS.

Handlers run synchronously on the room owner's thread after state commits. The room
has no outstanding cycle while a handler runs. Do not reset or mutate the engine
inside a handler. Other mutating room calls reject reentrancy; reset and direct
state writes are forbidden by this contract. Read-only membership or
recipient snapshots are allowed. Keep handlers bounded and copy borrowed C message
bytes before returning if another task needs them. A handler may enqueue application
work; the protocol library never launches processes, threads, or I/O on its own.
`firmware/esp32/main.c:host_room_event` is the working echo/logging integration.

The Python decoder accepts the corresponding receive hook:

```python
from pictochat import Decoder

def on_message(message):
    print(message.sender.hex(":"), len(message.body))

decoder = Decoder(on_message=on_message)
# decoder.feed(frame), or decoder.feed_application(source, application_bytes)
```

`decode(frames, on_message=handler)` also supports this hook. Python messages own
immutable bytes and can be retained. Handler exceptions propagate; the completed
transfer remains marked emitted, so duplicate packets do not invoke it again.
The passive Python decoder has no outgoing-radio SENT event; that belongs to the
active host engine when it is ported.

## Boundary and ownership

`pictochat/session.h` is the application engine. Allocate one `pictochat_session_t`
per client (or use the room wrapper). It owns profile, identity, reassembly and transmit buffers. Use
static/heap storage on an MCU (roughly 24 KB). It uses no platform callbacks, global
mutable state, threads or allocation. Optional room event handlers belong to the
application and are called synchronously. One owner calls it serially. Public structures
are inspectable C values, not a stable binary ABI.

The adapter owns admission validation, RX queues, radio callbacks, ACK ownership,
clocks, deadlines and logging. The ESP32 example also owns its echo-bot policy and
drawing-export queues. Discard queued input from previous association generations.
On association changes, reset only the affected client's session and poll schedule.

1. `pictochat_session_reset(state, profile, token0, token1)` copies an 84-byte
   profile and resets protocol state. The caller supplies random tokens.
2. Feed WM application bytes with `pictochat_session_receive`. Remove MAC/WM
   headers and sequence footers first; call only for the admitted current client.
   Results: `-2` busy (retain/retry input), `-1` rejected, `0` accepted without a
   completed drawing, `1` one complete drawing. Copy `state.received` if it must
   persist beyond the next accepted announcement.
3. `pictochat_session_reply` queues a validated drawing with rewritten sender
   metadata. It refuses invalid sizes, a busy sender, or an unfinished identity
   exchange. Echoing is optional adapter policy.
4. `pictochat_session_prepare(state, admitted, output)` prepares one poll: empty,
   roster, heartbeat or application plus its WM sequence. Exactly one output may
   be outstanding. Receive/reply calls are blocked until it is resolved. State
   inspection during this interval reflects tentative changes.
5. Encode through `pictochat/frame.h`, transmit, then call `pictochat_session_finish`.
   Every successful prepare needs one finish, unless reset cancels the session.

| Transmission result | Payload state | Poll state | Application sequence |
| --- | --- | --- | --- |
| `PICTOCHAT_TX_FAILED` | restore | restore | restore |
| `PICTOCHAT_TX_NO_REPLY` | restore | advance | advance |
| `PICTOCHAT_TX_DELIVERED` | advance | advance | advance |

A missing TX callback remains an outstanding operation; wait for its actual result.
Radio TX completion alone is not a client reply. The ESP32 adapter requires a reply
for granted admitted polls and keeps ACK timing in the radio layer. Poll-field
cadence and the member-frame counter are separate adapter-owned state, matching
the original firmware's retry behavior.

## Bytes and bounds

Frame encoders return raw 802.11 without radiotap/FCS, or zero for invalid input or
capacity. App buffers hold at most 268 bytes; WM encoding requires even application
lengths. Integers are explicitly little-endian. Never serialize C structs or rely
on native packing, byte order or enum size. Encoder input/output must not overlap.

Drawing bodies contain 36 metadata bytes plus 1..10 tile rows of 1024 bytes, at most
10276 total. Fragments carry offset/length/final flags. Missing coverage or conflicting
overlap cannot complete a drawing. Duplicate final packets must not emit twice.
Identity bodies are 84 bytes; Bio accepts at most 26 UTF-16 code units.

## Building host payloads

Do not copy captured magic bytes into adapters. `host_profile.h` builds the 84-byte
identity (`host_profile_init` for type/MAC/colour/birthday, then `set_name`, `set_bio`,
`set_colour`); `host_message_body_full` writes the full-height 36-byte message header
plus a 10240-byte bitmap. Python mirrors both in `pictochat.drawing` (`profile`,
`message_body`, `announcement`, `state_payload`, `drawing_payload`, `drawing_bitmap`)
and `pictochat.canvas` converts between tile rows and palette-index grids (`tile`,
`detile_indices`; `write_png(..., palette=PALETTE)` keeps colour). Both languages
compare their header against `tests/fixtures/drawing-header-full.bin` and the captured
identity vector, so a layout change in one language fails the other's test. Header
bytes 8..35 of the full-height message are still undecoded: smaller canvases must pass
a captured template.

Smaller header modules are independently usable: identity, message, sequence,
profile, poll fields, admission/reply filtering and tracing. `ack_gate.h` is an
optional C11 atomic helper for concurrent adapters; the engine uses no atomics.
Low-level message-reply helpers assume validated sizes. Prefer the checked
`pictochat_session_reply` entry point for external data.

## Porting to Python

Port frame encoding, then sequence/identity transitions, drawing sender/reassembly,
and session prepare/finish. Use dataclasses for state, bytes for immutable packets
and bytearrays for bounded reassembly. Explicitly wrap 16-bit sequences and 32-bit
tokens/hash arithmetic. The C structs are not a ctypes ABI.

The existing package exposes `Decoder.feed(frame)` and
`Decoder.feed_application(source, app, route=())`. Each returns a complete Message
once or None. Route distinguishes separate transport paths for the same source.
This passive multi-stream decoder is not the active single-client send/retry engine.

Both languages use `tests/fixtures/send-client-apps.bin`: each application packet
has a two-byte little-endian length prefix. `send-message.bin` is the expected
reassembled body. `test_session.c` checks failure, missing reply, backpressure,
readiness, retransmission and reconnect. `test_frame.c` checks byte order, wire
offsets, groups, masks and bounds; identity tests check captured payload vectors.

Before adding a Python transport, reproduce the session tests and byte-level vectors.
The transport supplies the same events as C and separately proves radio timing on
hardware. The decoder tests alone do not establish host conformance.
