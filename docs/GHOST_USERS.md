# Ghost users and an online relay

## Local proof of concept

`esp32c6ghost` hosts the normal room B with PICTOBOT and a virtual participant
named GHOST. GHOST has MAC `00:09:bf:c6:c6:c7`, roster/AID position 15, and the
Bio `Local ghost; online next`. A completed local drawing is echoed under that
identity. The host remains at position zero. This build reserves the fourth room
storage slot for GHOST, leaving capacity for three physical DS clients.

The September 24 local trial succeeded: the user confirmed that GHOST appears,
Send enables, and the drawing echo is attributed to GHOST. Both identity stages
use the captured client announcement descriptor with a substituted roster
position and profile. This proves the tested local exchange, not sustained
reliability, capacity, or an internet relay.

```sh
pio run -e esp32c6ghost
pio run -e esp32c6ghost -t upload --upload-port COM12
python tools/capture_serial.py --seconds 120 --label ghost-local
```

The capture tool defaults to COM6 and COM12; use `--ports COM12` when only the C6
is connected. Start the capture before joining the room. Then:

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

## Path to online PictoChat

The proposed topology is:

```text
DS A <-> C6 A <-> USB bridge A <-> internet relay <-> USB bridge B <-> C6 B <-> DS B
```

Each C6 would keep its local association, polling, acknowledgments and transfer
retries. The internet layer would exchange participant profiles, membership
changes and completed drawing bodies. Remote users would be installed as local
ghosts; a remote drawing would use ghost_send after validation and normalization.
AIDs are local room positions and must be mapped, not copied between rooms.

The bridge still needs framed bidirectional serial transport, bounded queues,
remote identity/generation mapping, message IDs and duplicate suppression, room
membership/disconnection handling, and a relay service. Received remote messages
must not be exported back to their origin. Preserve the drawing bitmap while
mapping sender metadata to the destination room's virtual identity.

USB is the initial transport proposal. Espressif documents that the C6 shares a
single Wi-Fi channel between station and SoftAP modes, with the station's channel
taking priority. Combining the custom PictoChat host with a router connection
would therefore need separate channel and timing validation; it is not established
by this PoC. See the [Espressif Wi-Fi API documentation](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32c6/api-reference/network/esp_wifi.html).

## September 24, 2026 validation

All 15 Windows-compatible native C tests passed with Zig (`-std=c11 -Wall
-Wextra -Werror`); the optional POSIX pthread ACK test was excluded. The
`esp32c6ghost` and standard `esp32c6host` firmware builds passed. The ghost
firmware was flashed to COM12 with esptool hash verification and remains on the C6.

In the user-confirmed successful trial, the host received an 8,228-byte drawing at
17:06:29.243 and completed its ghost echo at 17:06:33.112. The original body hash
was `de3af333`; the outgoing body hash was `a59868cf`. Rewriting only the six sender
MAC bytes in the exported original produces the outgoing hash. The serial DRAW
export passed coverage and checksum validation.

Evidence is under `captures_out/2026-09-24/` (ignored generated output):

- `ghost-local-170345-835629-COM12.log` and `-COM6.log`: initial join/identity trial.
- `ghost-drawing-170602-736245-COM12.log` and `-COM6.log`: successful drawing trial.
- `ghost-local-verified/`: validated original drawing body and rendered bitmap.

The drawing trial's bounded independent sniffer sample contains 267 host CMDs:
7 grants with mask zero and 260 with mask 2 (real AID 1). None grants ghost AID 15.
The sample includes both ghost profile stages but lacks enough drawing fragments
to reconstruct the echo independently; screen confirmation and C6 completion
logs are the evidence for the successful echo.

The first session logged a wait for CMD TX completion, and the later session has
nonzero TX failures. A reconnect preceded the confirmed drawing. This PoC does
not resolve the adapter's existing timing/reliability questions.
