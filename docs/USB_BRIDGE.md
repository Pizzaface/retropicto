# USB PictoChat bridge

Each C6 owns its DS radio connection. The computer forwards complete profiles,
drawings and acknowledgment frames over USB, leaving the radio on its room's
working channel. No Wi-Fi credentials or router configuration are needed.

| Board | Port | PictoChat room | Radio channel | Host name |
| --- | --- | --- | --- | --- |
| A | COM5 | A | 1 | RELAY A |
| B | COM12 | B | 7 | RELAY B |

The common `esp32c6usb` image selects A/B from the two bench boards' factory
MACs in `online_wifi_configure()`. Unknown boards refuse startup. This mapping
must be changed for replacement hardware; see the [board assignment instructions](../README.md#1-assign-your-boards-before-flashing). Each board admits
`ONLINE_LOCAL_SLOTS` physical DS consoles; every remote DS is installed as a
virtual participant (ghost) using its actual profile, one ghost slot per remote
peer id, AIDs assigned downward from 15.
The local echo bot is disabled. Drawing attribution is rewritten by the existing
ghost API, without changing the bitmap. USB code reuses the online bridge's room
ownership, bounded queues, generation checks, and duplicate suppression.

## Same computer trial

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

Wait for `USB relay ready` and `USB peer connected` from both boards. Then join
Room A on one DS and Room B on the other. Each room should show RELAY A/B and
the remote DS's name once both identities complete. Send a drawing each way and
verify both attribution and bitmap. Test overlapping sends and leaving/rejoining.
Stopping the PC bridge should remove remote ghosts after about six seconds;
restarting it should restore current remote membership without rebooting either
C6. Messages sent while disconnected can be dropped; there is no offline history.

`Accepted remote drawing` means queued for the local DS. `Peer consumed drawing`
means acknowledged by the receiving bridge. `DRAW outbound TX complete` means
the DS radio transfer completed. Only the DS screen confirms correct display.

## Two computers

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

## Framing and radio isolation

Wire packets use the 20-byte PCTR **v2** header (`PCTR`, version 2, kind,
uint16 length, uint32 sequence, uint32 FNV-1a payload checksum, uint16 `from`,
uint16 `to`) and bounded state/drawing/ACK/leave payloads. Board→bridge, `from`
is the local DS slot and `to` is a remote peer id or 0 for all. Bridge→board,
`from` is a nonzero peer id the bridge assigns per remote participant and `to`
is a local slot or 0. `leave` (kind 4, empty) removes a peer immediately;
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

The old `wifi.local.json` diagnostic settings do not affect this environment.
The temporary changes to `esp32c6ghost` (APSTA, alternate address, Room A, passive
observer) were removed; that environment is once again the local AP-only Room B
GHOST demonstration. Saved diagnostic binaries remain in `captures_out`.

## Validation

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
mapping supersedes the table above until the diagnosis is complete.

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

The final USB trial was confirmed by the user in both directions with correct sender attribution. The subsequent BLE/PC/phone experiment is documented in [BLE_GATEWAY.md](BLE_GATEWAY.md).
