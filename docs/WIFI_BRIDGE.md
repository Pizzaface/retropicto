# Two C6s on one Wi-Fi network

This is the first direct-Wi-Fi bridge experiment. Each ESP32-C6 hosts one physical
DS and represents the other DS as a ghost. The two C6s discover each other and
exchange profiles and completed drawings over the LAN. USB is used for flashing
and logs; the computer does not forward messages.

| Board | Environment | Port | Local PictoChat room | Host name |
| --- | --- | --- | --- | --- |
| A | esp32c6online | COM12 | A | RELAY A |
| B | esp32c6online | COM5 | B | RELAY B |

The two roles have distinct host MACs. Each allows one physical association and
uses ghost AID 15 for the remote DS. Open the indicated room on each console.
The remote user's actual profile supplies the ghost name, Bio, and sender MAC.
The echo bot is disabled in these builds.

## Configuration and build

Copy `wifi.example.json` to `wifi.local.json`, then fill in the shared 2.4 GHz
SSID and WPA2 password. Set node_a_mac and node_b_mac to the boards' factory
Wi-Fi MAC addresses (shown as the USB serial IDs). One firmware image selects
its room from this mapping at startup; an unassigned board refuses to start. The local file and generated credentials header are
ignored by Git. The pre-build hook does not put the password in command-line
flags or logs. Firmware binaries contain the configured credentials; keep them
local too. Both boards use this same local configuration and firmware image for the first test.
Run PlatformIO operations sequentially; its automatic cleanup can otherwise
invalidate another environment's active build. The commands below disable auto-clean.

```sh
pio run --disable-auto-clean -e esp32c6online
pio run --disable-auto-clean -e esp32c6online -t upload --upload-port COM12
pio run --disable-auto-clean -e esp32c6online -t upload --upload-port COM5
python tools/capture_serial.py --ports COM12 COM5 --seconds 180 --label wifi-bridge
```

The router must allow clients to communicate and LAN broadcasts to reach both
boards. Discovery uses UDP port 26711; board A listens on TCP port 26711 and board
B connects to it. There is no computer-hosted server or
port forwarding in this LAN experiment. Discovery binds to the station IPv4 address
and sends to its subnet broadcast address. If broadcasts do not reach the peer,
set optional `node_a_ip` in `wifi.local.json` to board A's LAN IPv4 address;
board B also attempts a direct connection there. Omit it or use `0.0.0.0` for
discovery only. A DHCP address can change; update this setting if needed.

## Radio coexistence

Both station and PictoChat SoftAP modes share a radio. The station's home channel
takes priority, so the host's beacon reports the channel supplied by Wi-Fi events.
Ordinary frames receive a standard three-byte DS-parameter element; the hidden
PictoChat beacon retains the existing SSID surgery. Both the strong builder
symbol and linker wrapper lead to that implementation so SDK-internal calls
can also use it. The host stays at 802.11b with 2 Mbps
raw injection, and modem sleep is disabled.

[Espressif's home-channel documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/wifi-driver/overview.html)
explains the shared-channel constraint. Compilation alone cannot establish that
the custom host remains compatible with the router or reliable under simultaneous
network and PictoChat traffic. Router channel changes/reconnection can interrupt
the local DS session.

## Protocol and ownership

The radio task exclusively owns the room. A lower-priority task owns TCP/UDP,
snapshots local/remote profiles under a short lock, and uses bounded queues for
complete drawings. No socket calls run in a room event handler.

The TCP stream uses a 16-byte header: `PCTR`, version 1, kind (state/drawing/ACK),
16-bit payload length, 32-bit sequence, and 32-bit FNV-1a payload checksum. All
integers are little endian. State contains boot ID, association generation, and
an 84-byte profile. Drawings contain boot ID, association generation, the original
20-byte announcement, and 1..10 tile rows with 36 metadata bytes. Maximum payload
is 10,304 bytes. ACKs contain the sender boot ID and message sequence.

There are at most two queued outgoing drawings and one queued incoming drawing.
An outgoing message waits for an ACK; retransmissions reuse its sequence. The
receiving owner remembers the last accepted sequence for the source boot ID so
an ACK lost across reconnection does not inject the same drawing twice. A fresh
remote profile is installed before its drawing. Ghost injection never emits a
physical receive event, preventing forwarding loops.

ACK means consumed by the bridge (accepted, duplicate, or discarded as stale or
invalid), not successful DS display. `DRAW outbound TX complete` is the separate
radio completion log. Full queues and drawings sent while the peer link is absent
are explicitly dropped with warnings; this PoC has no offline history. Membership
loss removes the remote ghost; drawings queued for an old local association are
discarded. Existing room generation and retry semantics apply after acceptance.

This discovery/TCP protocol is unauthenticated and intended for the private LAN
trial. It is not an internet-facing relay. Connecting separate home networks will
need authenticated transport and a reachable relay or tunnel; LAN broadcasts do
not cross routers.

## Hardware trial

1. Wait for `Wi-Fi ready` and `Peer TCP connected` on both boards.
2. Join room A on one DS and room B on the other. Wait for Send to enable and
   each remote name to appear alongside RELAY A/B.
3. Send different small drawings in each direction; verify the source names and
   clean bitmaps. Repeat with overlapping sends.
4. Leave/rejoin one DS. Verify the other room removes/reinstalls its ghost and
   does not display a queued drawing as a replacement user.
5. Reset one C6 and test rediscovery, then reconnect its DS. Note any duplicate,
   dropped, or delayed messages alongside the logs.

The previous local GHOST experiment is verified. This direct-Wi-Fi bridge must
be assessed separately; see the current trial results before claiming it works.

## Validation notes (2026-09-24)

The relay codec native test and host transmit-source filter check passed. The
online firmware built and flashed successfully to COM12 and COM5. Both obtained
LAN addresses on 2.4 GHz (A: 192.168.1.224, B: 192.168.1.222). An initial Wi-Fi
failure retried successfully. The router exposes multiple access points on
channels 6 and 11. UDP discovery from the PC and a TCP state packet from A were
verified, but automatic board discovery did not establish a peer connection.
The current local trial therefore configures `node_a_ip` explicitly.

`tools/capture_serial.py --reset` can capture a fresh boot. It includes the
Windows USB serial DTR workaround needed to propagate RTS changes.

With the explicit peer address, both C6s logged `Peer TCP connected` at
18:10:37 and continued exchanging state. Capture pair:
`captures_out/2026-09-24/wifi-direct-peer-181014-054927-COM12.log` and
`wifi-direct-peer-181014-054927-COM5.log`. This establishes the Wi-Fi transport;
remote DS membership and drawing display still require the two-console trial.

The first DS trial failed: the user reported missing rooms. Logs recorded brief
associations followed by departure, zero MP replies, and no identity exchange.
A follow-up build restores a strong `ieee80211_add_dsparams` override alongside
the wrapper to cover SDK-internal references. Both boards were flashed; room
visibility remains pending a screen check. Capture prefix: `wifi-beacon-fix-181442-815000`.

## Current diagnostic state

The strong-symbol beacon change did not restore visible rooms (user confirmed
neither). COM12 then captured COM5's actual channel-11 beacon in
`captures_out/2026-09-24/beacon-channel11-181845-331796-COM12.log`: no SSID IE,
four supported rates, DS parameter 11, Nintendo vendor IE with room B and one
occupant. This rules out the missing SSID surgery as a sufficient explanation.
It does not establish DS compatibility on that channel or during APSTA operation.

COM12 is currently isolated on channel 7, with its router connection disabled,
to compare room visibility. COM5 retains its networked firmware. The local config
currently has `diagnostic_channel: 7`; **set it to 0 or omit it before building
normal bridge firmware**. Nonzero values (1..13) skip station connection/network
task startup and select that radio channel. This is a room diagnostic, not a bridge.
Capture: `captures_out/2026-09-24/local-radio7-182012-491644-COM12.log`.

Optional `wifi_channel` (0..13, default 0) provides a station scan starting-channel
preference; it does not override the connected router's channel. The local trial
uses 11 to make it easier for one board to observe the other. The online RX
callback logs only the first three received relay beacons for that diagnosis.

The channel-7 / station-disconnected diagnostic also failed to show Room A
(user confirmed). Disabling station association alone therefore did not restore
the working behavior. It still used APSTA mode, the online host MAC, room A,
and the online beacon path, so those differences remain unisolated. Do not
attribute the failure solely to router channels or assume direct Wi-Fi is viable.

The user is away and explicitly authorized flashing without further DS tests.
COM12 has been restored to `esp32c6ghost` (Room B, PICTOBOT + GHOST, AP-only,
channel 7); COM5 retains the experimental online image. The two-board bridge is
not operational in this diagnostic arrangement. The next manual check is whether
the restored local GHOST demo is visible and echoes as it did earlier.

Restoration completed at 18:31 on September 24. Esptool independently verified
all 874,288 application bytes against `.pio/build/esp32c6ghost/firmware.bin`
(digest matched). Fresh boot confirms BSSID `00:09:bf:c6:c6:c6`, room B,
channel 7, and `GHOST demo: virtual member aid=15`. No DS test was performed
because the user is away. Boot log:
`captures_out/2026-09-24/ghost-restored-183125-924412-COM12.log`.
A recovery copy of bootloader, partitions, application, flash offsets, and SHA-256
checksums is saved in `captures_out/2026-09-24/ghost-restored-firmware/`.
The online `diagnostic_channel: 7` setting remains in the local configuration;
it does not affect this offline GHOST environment.

At 18:58 the user confirmed the restored Room B baseline shows both names,
enables Send, and echoes drawings as GHOST. The next isolated trial changes
only the ghost build radio mode to APSTA, leaving STA unconfigured/disconnected,
with the same host MAC, room B, channel 7, GHOST identity, and protocol.
This temporary ghost-mode change must be reverted after the comparison.

APSTA-idle trial passed after a delay: the DS joined at 19:00:04, exchanged
GHOST identities at 19:00:44, and completed a 10,276-byte ghost drawing at
19:01:18 (`sender=15`, hash `781c9a89`). The user confirmed GHOST appeared
and worked. APSTA mode alone is therefore not sufficient to explain failure.
Capture: `ghost-apsta-idle-185938-699766-COM12.log`.

Next isolation trial retains that setup but changes the host MAC from c6 to d1
(the bridge A address), including the MAC embedded in the host profile. Room B,
channel 7, GHOST and disconnected STA are retained. These temporary ghost-build
changes remain experimental; the saved recovery image retains the working AP-only baseline.

The bridge-address trial passed: COM12 advertised d1, joined at 19:04:52,
reached ROOM ready at 19:05:04, received 8,228 bytes at 19:06:00 and completed
a GHOST echo at 19:06:04 (`sender=15`, hash `1ea33405`). Capture:
`ghost-bridge-mac-190430-082534-COM12.log`. The next isolated change is
room B to A; other parameters remain the same. The temporary ghost build
now uses APSTA idle, d1, room A, channel 7.

Room-A trial: COM12 boot log reports A, but the user still saw B. COM5 was
also still advertising B, so attribution is unresolved. COM5 has now been parked
in its ROM/stub bootloader using `esptool --after no-reset chip-id`; its firmware
is intact and its radio is silent until reset. The next screen check isolates COM12.
GBATEK independently places the room byte at Nintendo IE data offset 0x1c
(30 including tag/length), matching the code; that does not substitute for an
over-the-air check: https://problemkaputt.de/gbatek-ds-wifi-nintendo-beacons.htm

A passive COM5 witness captured COM12 advertising room byte 0, users 2,
channel 7, and no SSID IE (`witness-room-a-191109-633153-COM5.log`).
Room A remained invisible with COM5 silent. A historical first-hand capture
reports A/channel 1 and B/channel 7: https://gbadev.net/forum-archive/thread/20/6972.html
The next trial changes the ghost Room A radio to channel 1. The prior assumption
that the room number could change independently of the channel was unverified.
COM5 now runs a passive observer in the ghost diagnostic image, selected by its
factory MAC; it advertises no room. Its currently flashed observer listens on 7.

Changing only the Room A radio channel from 7 to 1 immediately restored DS
admission: the fresh boot capture `ghost-room-a-channel1-191233-348835-COM12.log`
shows type-6 admission at 19:12:34 and GHOST identity exchange at 19:12:37.
This is direct evidence that the Room A/channel 7 diagnostic was invalid.
The earlier online A/B build followed router channels 6 or 11, neither of which
matched the tested A/channel 1 or B/channel 7 combinations. Direct-Wi-Fi bridge
planning must respect that constraint; separate room letters on one Wi-Fi
channel cannot be assumed valid. Screen and echo confirmation for A/1 pending.

The user confirmed Room A/channel 1 works, including GHOST and drawing echo.
A verified-build copy is saved in `captures_out/2026-09-24/ghost-room-a-channel1-verified/`.
Current hardware: COM12 hosts this working local diagnostic; COM5 is a passive
channel-7 observer, not an online bridge. Next step: a 2.4 GHz AP on channel 1
for the first direct-Wi-Fi Room A trial. Awaiting the user's router configuration
capability; do not follow channels 6/11 while claiming Room A compatibility.
For two nearby C6s on the same channel, room selection also needs separate
validation: the original assumption that A and B could share the router channel
was wrong. A remote deployment can use the same room letter at both sites.

## Superseded bench path

The user cannot change the router channel and approved the USB alternative.
See [USB_BRIDGE.md](USB_BRIDGE.md). The temporary ghost/observer source edits
have been removed. USB hosts A on channel 1 and B on channel 7; the PC carries
relay traffic, so the router no longer determines PictoChat radio channels.
