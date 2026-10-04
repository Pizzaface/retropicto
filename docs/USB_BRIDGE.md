# USB PictoChat bridge

The `esp32c6usb` build ("Relay") owns one DS room over its own radio and
exchanges PCTR v2 relay frames with whatever is on the other end of its native
USB Serial/JTAG port. In production that is the RetroPicto Android app
(`../retropicto-android`), which carries frames to other phones over MLS. The
two-PC `tools/usb_bridge.py` setup further down is the bench/dev path only.
No Wi-Fi credentials or router configuration are used in this build.

## Phone cable (production)

The phone opens the C6 as a plain CDC bulk pipe (VID `0x303a`, PID `0x1001`).
It sets DTR|RTS once on open and never toggles them, so the board is not reset;
only the in-app flasher performs the esptool reset dance. All traffic is ASCII
lines terminated by `\n` (`\r` ignored). Both sides prefix every line they send
with `\n` so a half-written line is discarded and the parser resynchronises.
Lines longer than the PCTR maximum are dropped up to the next newline.

### Room selection

The USB build has no MAC → board mapping (that code is LAN-build only,
`online.c` `#if !PICTOCHAT_USB && !PICTOCHAT_BLE`). At boot it reads NVS
namespace `picto`, key `room` (u8, 1..4 = A..D). If absent it uses
`PICTOCHAT_NODE`, which `esp32c6usb` does not define, so **any board, known or
unknown, starts as room A / channel 1 until `@ROOM` is sent**. Channels are
fixed per room: A=1, B=7, C=13, D=7. The app's flasher skips the NVS region
(`0x9000`–`0xF000`), so the saved room survives firmware updates; a full erase
or a merged-image write over that region resets it to A.

### Control lines

| Line | Direction | Effect | Response |
| --- | --- | --- | --- |
| `@INFO?` | phone → C6 | Request status. | `@INFO room=<A-D> channel=<n> local=<ONLINE_LOCAL_SLOTS> ghosts=<GHOST_SLOTS> build=<16 hex>` |
| `@INFO room=…` | C6 → phone | Also sent unprompted once at boot. `build` = first 8 bytes of `app_elf_sha256`, lowercase hex. There is no version field; the app gets the version from the service's `/firmware/esp32c6.json`. | — |
| `@OPEN` | phone → C6 | Advertise the DS room (PictoChat beacon vendor IE) for 10 s from receipt. | none |
| `@CLOSE` | phone → C6 | Stop advertising immediately; removes the IE and deauthenticates every connected DS. | none |
| `@ROOM <A-D>` | phone → C6 | Save room to NVS, then reboot ~200 ms later. Applies on the next boot. | `@ROOM OK` or `@ROOM FAIL` (NVS error, no reboot). Any other letter is silently ignored. |
| `@PCTR <hex>` | both | One PCTR v2 frame, header + payload hex-encoded (either case accepted by C6; C6 emits lowercase). | none; invalid frames are dropped silently |
| anything else | C6 → phone | ESP-IDF log rows. Never forwarded; the app logs them. Rows are bounded (256 B) and may be dropped under load. | — |

Commands must match exactly (`@OPEN`, not `@OPEN ` or `@open`). Matching is on
the whole line after newline/CR stripping.

The room is closed at boot. The app sends `@OPEN` every 3 s while its MLS room
reports `Online`, and `@CLOSE` when it goes offline, is kicked, or the user
disconnects. If the app dies or the cable is pulled, the room closes on its
own within 10 s of the last `@OPEN`. Room visibility does not stop the relay:
the C6 keeps sending state heartbeats over USB either way.

### PCTR frames over the cable

C6 → phone: `from` = local DS slot (0..`ONLINE_LOCAL_SLOTS`-1) for state and
drawings; ACKs are sent with `from` = 0 and `to` = the remote peer id that sent
the drawing. Each local slot sends a state frame every second and on change,
including empty slots (generation 0).

Phone → C6: the app keys each remote participant as
`(sourcePhoneId, remote from)`, assigns peer ids 1, 2, 3… in first-seen order
(never reused within a bridge session), rewrites `from` to that id and `to` to
0, and leaves payload and checksum unchanged. `from` = 0 is rejected by the C6.
A `leave` for an unknown key is not forwarded.

Peer lifetime on the C6: a peer gets a remote entry on its first valid state
frame and expires 6 s after its last state frame, or immediately on `leave`
(kind 4, empty payload). When the app stops it sends `@CLOSE` to its own C6 and
`leave` for slots 0–3 to the other phones. Drawings are resent every 4 s with
the same sequence until every live remote entry has ACKed, then dropped after
30 s. Neither the app nor the bench tool invents ACKs.

### Slots and ghosts

`esp32c6usb` builds with `PICTOCHAT_ROOM_CLIENTS=6` and `ONLINE_LOCAL_SLOTS=2`
(`platformio.ini`), so each Relay hosts 2 physical DS consoles and
`GHOST_SLOTS` = 4 remote entries. Physical consoles take radio AIDs counting up
from 1; ghosts take AIDs counting down from 15. A remote DS is shown as a ghost
using its real profile, and drawing attribution is rewritten via the ghost API
without changing the bitmap. A remote entry is consumed by every remote peer id
that sends state, including empty remote slots (generation 0), so 4 entries
cover two remote Relays, not four remote consoles. A 5th peer is refused
(`Remote <id> refused: no free ghost slot`). Each room slot costs about 24 KB
static RAM; the local echo bot is disabled in this build.

Useful log rows: `USB relay ready`, `Remote <id> connected`, `Remote <id>
installed as ghost`, `Accepted remote <id> drawing` (queued for the local DS),
`Remote <id> consumed drawing` (ACK received), `DRAW outbound TX complete`
(DS radio transfer done), `room open` / `room closed`. Only the DS screen
confirms correct display.

## Framing and radio isolation

Wire packets use the 20-byte PCTR **v2** header (`PCTR`, version 2, kind,
uint16 length, uint32 sequence, uint32 FNV-1a payload checksum, uint16 `from`,
uint16 `to`) and bounded state/drawing/ACK/leave payloads. Board→bridge, `from`
is the local DS slot and `to` is a remote peer id or 0 for all. Bridge→board,
`from` is a nonzero peer id the bridge assigns per remote participant and `to`
is a local slot or 0 (the C6 ignores `to`). `leave` (kind 4, empty) removes a peer immediately;
otherwise a peer expires six seconds after its last state heartbeat. A drawing
is retried every four seconds until every live peer has ACKed it, and dropped
after 30 seconds. `usb_bridge.py` is single-peer: it stamps the far board as
peer 1. USB carries one hex-encoded packet per line
prefixed `@PCTR `. Diagnostic lines are logged locally, never forwarded. Both
parsers bound input size and reject invalid framing. A leading newline permits
resynchronization after an interrupted write. Checksums detect corruption; they
do not authenticate peers. The PC never generates delivery ACKs.

The native USB Serial/JTAG driver handles USB input/output. A low-priority task
owns serial writes and reads, with a separate bounded log queue so verbose radio
logs cannot block protocol timing. Firmware log rows may be truncated/dropped
under load. Drawing queues are bounded (two outgoing, one incoming), and retries
use the same sequence number. Each room slot costs about 24 KB of static RAM
(`PICTOCHAT_ROOM_CLIENTS`, set per environment in `platformio.ini`); the C6 ceiling
for local consoles plus ghosts must be found on hardware. Incoming state heartbeats refresh peer liveness.
Room mutation happens only in the existing radio-owner task.

## Bench setup: PC bridge (dev only)

> **Limited.** `tools/usb_bridge.py` sends `@OPEN` every 3 s, but stamps every
> forwarded frame `from=1`, so with `ONLINE_LOCAL_SLOTS=2` both far-side slots
> overwrite one remote entry: use one DS per board. Both boards start as room A
> until `@ROOM B` is sent to one of them. Not hardware-tested since the `@OPEN`
> change.

### Same computer trial

Run PlatformIO operations sequentially, with automatic cleanup disabled:

```powershell
pio run --disable-auto-clean -e esp32c6usb
pio run --disable-auto-clean -e esp32c6usb -t upload --upload-port COM12
pio run --disable-auto-clean -e esp32c6usb -t upload --upload-port COM5
python tools/usb_bridge.py --ports COM12 COM5 --reset
```

Install the serial dependency with `python -m pip install pyserial`. Close serial monitors first: each port must have one owner. `--reset` captures a
clean startup; omit it to attach without resetting the current DS sessions.
Ctrl-C stops the bridge. `--seconds 240` limits a recording. The tool saves
firmware logs and forwarding summaries under `captures_out/<date>/usb-bridge-*`.

Wait for `USB relay ready` and `Remote 1 connected` from both boards. Then join
Room A on one DS and Room B on the other. Each room should show RELAY A/B and
the remote DS's name once both identities complete. Send a drawing each way and
verify both attribution and bitmap. Test overlapping sends and leaving/rejoining.
Stopping the PC bridge should remove remote ghosts after about six seconds;
restarting it should restore current remote membership without rebooting either
C6. Messages sent while disconnected can be dropped; there is no offline history.

Log row meanings are listed under [Slots and ghosts](#slots-and-ghosts).

### Two computers

The same tool can forward between one USB port and a TCP connection:

```powershell
# Computer A: listener is loopback-only in this example.
python tools/usb_bridge.py --port COM12 --listen 127.0.0.1:26712
# Computer B: connect via an already-established SSH port forward to A.
python tools/usb_bridge.py --port COM5 --connect 127.0.0.1:26712
```

For a private LAN, use A's LAN address in both `--listen` and `--connect` as
appropriate. This TCP stream has no built-in authentication or encryption; use
an authenticated SSH tunnel for separate networks, not an exposed public port.
Setting up that tunnel is separate from the local bench test. A socket/serial
failure stops the process and lets firmware heartbeat expiry remove the peer;
restart the tool after reconnecting. It does not reconnect automatically.

## History

The log below records the September 2026 two-PC bring-up. Board/port roles,
the MAC-based A/B selection and `USB peer connected` it mentions are obsolete;
see [Room selection](#room-selection).

The old `wifi.local.json` diagnostic settings do not affect this environment.
The temporary changes to `esp32c6ghost` (APSTA, alternate address, Room A, passive
observer) were removed; that environment is once again the local AP-only Room B
GHOST demonstration. Saved diagnostic binaries remain in `captures_out`.

### Validation

`python tests/test_usb_bridge.py` passes wire-size/checksum rejection, fragmented
and oversized stream recovery, and bidirectional socket forwarding without
invented acknowledgments. C framing uses the existing tested `relay_wire.h`.
The USB firmware build passed (185,216 bytes static RAM; 889,152 bytes code)
and both COM12 and COM5 flashed with verified hashes. At 19:27:21 on September
24 both boards logged `USB peer connected`; startup confirmed A/channel 1 and
B/channel 7. Capture: `captures_out/2026-09-24/usb-bridge-192720-034338.log`.
The two-DS display/drawing trial is in progress.

Initial two-console trial: Room A works, but the user reports Send disabled and
no users in Room B. COM5 associates with DS `00:22:d7:39:bc:a3` but receives no
MP replies and remains unadmitted. COM12 admits `64:b5:c6:9c:60:a0`. USB forwards
A's profile and COM5 installs its remote ghost, so profile transport is functioning
while B's local radio admission fails. A console-swap test is pending to separate
a console-specific issue from the COM5/Room B path. No bidirectional delivery
claim is made. Firmware remains unchanged for the swap.

Console-swap result: only A still works; COM12 admitted the second console
(`00:22:d7:39:bc:a3`) at 19:31:01. Next test swaps board roles in the common
USB image: **COM12 = B/channel 7/d2, COM5 = A/channel 1/d1**. This temporary
mapping was used for the rest of the trial.

The swapped-role trial enabled Send on both DS consoles. The user sent drawings
in both directions but neither appeared remotely. Capture
`captures_out/2026-09-24/usb-bridge-193447-027934.log` shows both physical peers
READY and both remote ghosts installed. Local drawing fragment echoes are present,
but no completed drawing was queued or forwarded over USB. Added per-second
`DRAW RX` assembly status (coverage, total, final, invalid, complete, rejected)
to distinguish missing fragments from validation failures on the next trial.
The role mapping remains COM5=A and COM12=B.

The isolated A-to-B drawing trial completed assembly at 19:41:44 (10,276 bytes,
full coverage, no validation failures) and queued sequence 1. No drawing frame
reached the PC. The installed ESP-IDF USB driver uses one atomic ring-buffer
submission per write call; the firmware submitted a 20,648-byte drawing line to
its 4,096-byte TX ring. Changed `usb_write` to submit at most 1,024 bytes per call,
retaining partial-write handling. PC bridge tests pass (3 tests). Display delivery
still requires the next physical trial. Diagnostic capture:
`captures_out/2026-09-24/usb-bridge-193855-211554.log`.

The final USB trial was confirmed by the user in both directions with correct sender attribution. The subsequent BLE/PC/phone experiment has been removed from the tree (see git history).
