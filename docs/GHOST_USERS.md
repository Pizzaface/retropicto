# Ghost users

## Local proof of concept

`esp32c6ghost` hosts the normal room B with PICTOBOT and a virtual participant
named GHOST. GHOST has MAC `00:09:bf:c6:c6:c7`, roster/AID position 15, and the
Bio `Local ghost; online next`. A completed local drawing is echoed under that
identity. The host remains at position zero. This build reserves the fourth room
storage slot for GHOST, leaving capacity for three physical DS clients.

In a local trial GHOST appeared, Send enabled, and the drawing echo was
attributed to GHOST. Both identity stages
use the captured client announcement descriptor with a substituted roster
position and profile. This proves the tested local exchange, not sustained
reliability, capacity, or an internet relay.

```sh
pio run -e esp32c6ghost
pio run -e esp32c6ghost -t upload --upload-port <port>
python tools/capture_serial.py --ports <port> --seconds 120 --label ghost-local
```

`--ports` is required (no default); list every board you want recorded. Start the capture before joining the room. Then:

1. Join room B from one DS and check for PICTOBOT and GHOST as distinct users.
2. Open GHOST's profile and check its name and Bio. Wait for Send to enable.
3. Send a small drawing. Check that an intact copy arrives from GHOST and Send
   becomes available again. Repeat with a different drawing.
4. Leave and rejoin; repeat the send. Optionally join a second DS and check that
   both consoles see GHOST and receive its echoes.

The firmware logs `GHOST demo` at startup. Successful ghost transfers produce
`DRAW outbound TX complete` with `sender=15` and the real recipient's AID.
That log reports protocol completion; only the DS display verifies attribution.
A valid trial should also retain an independent COM6 trace, when available, to
check that no poll grants AID 15 a reply slot.

Rebuild/upload `esp32c6host` to return to the standard PICTOBOT echo application.
The ghost build does not include an internet connection or serial injection API.

## Library behavior

`pictochat_room_ghost_join` installs a virtual member, its two identity stages,
and generation in an ordinary room storage slot. It shares MAC/AID collision
checks with physical members and appears in `pictochat_room_members`. It is
excluded from polling, physical drawing recipients, and host delivery tickets.
Physical receive input addressed to a ghost is rejected.

`pictochat_room_ghost_send` accepts a complete drawing with the same validated
input contract as `pictochat_session_reply`. It copies the drawing, rewrites its
sender MAC/roster position and token, and snapshots the currently admitted real
recipients. Return 0 means the source copy was accepted, -2 means retain/retry
because the source is busy, and -1 means invalid input or a stale generation.

Each destination receives both ghost identity stages before the drawing. Existing
per-recipient retry and sequence behavior applies. Late joiners receive identity
but no earlier drawings. Disconnect removes pending recipient bits, so a reused
slot does not receive an old queued source drawing. A drawing already copied into
a destination's outbound buffer can complete after its ghost author leaves.

Ghost injection does not fire MESSAGE_RECEIVED, avoiding an application echo loop.
MESSAGE_SENT still reports each real destination with the ghost's sender slot.
PEER_JOINED/LEFT also describe ghost membership; ghost creation has no physical
handshake and does not emit PEER_READY. All mutations belong to the room owner
outside event callbacks and outstanding radio cycles.

The demo uses the existing bounded drawing queue. Its recipient snapshot occurs
when ghost_send accepts that queued drawing, which can be later than receipt of
the original drawing. There is no unbounded offline history.

`tests/test_room_ghost.c` covers roster encoding inputs, identity ordering, virtual
poll exclusion, malformed input, retries, backpressure, two-recipient delivery,
late join, reconnect isolation, leave/rejoin, and outgoing author bytes.

## Online rooms

The online design shipped: `esp32c6usb` installs remote MLS room members as local
ghosts over USB. See [USB_BRIDGE.md](USB_BRIDGE.md).

## Validation

In the local trial, rewriting only the six sender MAC bytes of the received
8,228-byte drawing reproduced the outgoing ghost body hash, and the serial DRAW
export passed coverage and checksum validation. An independent sniffer sample of
267 host CMDs had 7 grants with mask zero and 260 with mask 2 (real AID 1); none
granted ghost AID 15. The trial also logged CMD TX completion waits, nonzero TX
failures, and a reconnect, so this PoC does not resolve the adapter's existing
timing/reliability questions.
