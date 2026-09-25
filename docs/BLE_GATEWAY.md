# BLE PC gateway trial

Status: BLE relay transport has delivered drawings between real DS consoles in
both directions with PC and Android gateways. The current setup path is
[`../INSTALL.md`](../INSTALL.md); Android details live in the sibling
`retropicto-android` repository.

## Topology

```text
DS <-> ESP32-C6 PictoChat Wi-Fi <-> BLE <-> PC gateway <-> WSS <-> Node server
```

Each C6 stays in AP-only mode, without router credentials. The explicit-role
images advertise `PICTO-A` or `PICTO-B` and host Room A/channel 1 or Room
B/channel 7. Use each board's actual serial port; board roles do not depend on
COM port names. USB remains available for power, flashing and diagnostics.

## Build and run

Use Python 3.11+ for the PC gateway. Install its dependencies:

```powershell
python -m pip install -r tools/requirements-ble.txt
```

Use the verified prebuilt image helper or build and upload to the actual port:

```powershell
python tools/flash_multihop.py --board c6 --role a --port YOUR_PORT --flash
pio run -e multihop_c6_a -t upload --upload-port YOUR_PORT
```

Use the sibling `retropicto-relay` repository to create the ignored
`relay.local.json`, start its server, and optionally expose it through a secure
tunnel. Its local setup generates separate random tokens. Run:

```powershell
cd ../retropicto-relay
npm run setup
npm start
```

Set its URL to the secure tunnel origin if connecting from another network.
Then in separate terminals:

```powershell
cd ../retropicto
python tools/ble_gateway.py --config ../retropicto-relay/relay.local.json --node a
python tools/ble_gateway.py --config ../retropicto-relay/relay.local.json --node b
```

For a local-only gateway on the relay computer, use the `ws://127.0.0.1:8787`
origin in its node config. The gateway requires TLS for other hosts, verifies certificates, reconnects after transport failure,
and never creates drawing ACKs. The server authenticates `Authorization: Bearer`
headers, reserves one live connection per node, and pairs configured room sides.
It validates every packet and stores no offline drawings or profiles. The public
`/health` route exposes only an OK status. Stop with Ctrl-C.

The BLE bench link currently has no pairing/encryption requirement: one nearby
central can connect and write the service. Keep this as a controlled prototype;
a phone/product version needs pairing or per-device BLE authentication before
use around untrusted nearby devices. Server tokens are held by the gateway,
not sent over BLE. The C6 does not directly open the server URL.

## Phone-compatible GATT contract

- Service: `50494354-0000-0080-5049-505100000001`
- Gateway writes RX: `50494354-0000-0080-5049-505100000002` (write commands; writes with response also accepted).
- Gateway subscribes TX: `50494354-0000-0080-5049-505100000003` (notifications).
- Each GATT value starts with uint16 little-endian offset and total packet length,
  followed by up to `min(ATT_MTU - 7, 240)` bytes. Offset zero starts a new packet.
- Require consecutive offsets, consistent total, and a maximum of 10,320 bytes.
  Discard incomplete, oversized or checksum-invalid packets. Reconnect resets
  assembly. The firmware prefers MTU 247 but the gateway honors the negotiated MTU.
- The assembled value is the existing binary PCTR header plus payload, as defined
  in `lib/pictochat/include/pictochat/relay_wire.h`. WebSockets carry that same
  binary packet, one message per packet; no USB hex or log text goes to the server.
- Heartbeats advertise current membership; six seconds without the remote state
  removes its ghost. Drawing ACK/retry and deduplication stay in C6 firmware.

A phone implementation needs a native BLE central (GATT client) plus an
authenticated WebSocket client. It can reuse this contract; screen-off/background
operation must be tested separately on Android/iOS.

## Validation

`python tests/test_ble_wire.py` covers all drawing sizes, MTUs 23 through 517,
missing fragments, corruption, bounds and restart recovery. `npm test --prefix
server` covers authentication, duplicate nodes, room isolation, maximum drawings,
ACK forwarding, reconnects, and absence of offline replay/generated ACKs.

The relay's own test suite covers authentication, duplicate nodes, room isolation,
maximum drawings, ACK forwarding, reconnects, and absence of offline replay.

On this bench the Realtek Bluetooth 5.0 USB adapter scans successfully. The Intel
adapter is disabled. The authenticated Cloudflare WSS path passed maximum-size
binary drawing forwarding in both directions using software clients.

First BLE firmware build passed: 155,624 bytes static RAM, 1,255,184-byte image.
The BLE environment uses `partitions.ble.csv` (application size 0x1f0000), because
the old 1 MB slot is too small. Both uploads were hash-verified. Both boards
booted with approximately 78 KB free heap and connected to the Realtek adapter
with MTU 247. At 20:08 on September 24, each board logged `BLE peer connected`:
state packets crossed C6 -> BLE -> PC -> Cloudflare WSS -> server -> PC -> BLE ->
other C6. Serial capture: `captures_out/2026-09-24/ble-first-200712-840225-COM*.log`.
The subsequent real-DS drawing trial passed in both directions. Rollback USB images and hashes
are saved in `captures_out/2026-09-24/usb-before-ble/`.

The first drawing trial exposed remote-ghost timeout/rejoin cycles while both
BLE connections stayed up. Sequential write-with-response fragments delayed
membership heartbeats past six seconds (drawings took several seconds). Added
GATT write-command support and changed the PC to paced 15 ms writes without
response, bounded by the characteristic's actual maximum write size. Existing
whole-packet checksums and C6 ACK/retry remain responsible for delivery.

The paced-write hardware ingress test used three maximum-size packets with
zero membership generation (discarded and acknowledged, never displayed on a
DS). Submission took 1.25-1.31 seconds, full Cloudflare/BLE acknowledgment took
4.67-4.88 seconds, and no peer timeout occurred during those transfers. This also
showed the four-second drawing retry was too short: BLE now retries after twelve
seconds, while the six-second remote heartbeat expiry remains unchanged.
