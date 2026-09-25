# Setup and flashing

This guide uses two ESP32-C6 boards and the local companion repositories. It
assumes you have two Nintendo DS consoles with PictoChat. Board A hosts DS Room
A; board B hosts DS Room B. Keep each board near its console.

## 1. Install prerequisites

Install Python 3.11+ and Git. The PC BLE gateway needs Python 3.11; the
standalone capture decoder supports Python 3.10+. Use USB data cables, not
charge-only cables.
From the parent folder containing all three local repos, install esptool and
the BLE gateway dependencies:

```powershell
cd retropicto
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip esptool
python -m pip install -r tools/requirements-ble.txt
```

On macOS/Linux, use `python3 -m venv .venv`, activate with
`source .venv/bin/activate`, and use `python` in place of `py`.

## 2. Flash each board

Connect one board at a time, then list ports and preview its image:

```powershell
python tools/flash_multihop.py --list-ports
python tools/flash_multihop.py --board c6 --role a
```

Preview verifies the bundled manifest, each SHA-256, image size and flash
offsets. It does not open a serial port or touch hardware. When ready to write,
specify the port and `--flash`:

```powershell
python tools/flash_multihop.py --board c6 --role a --port COM5 --flash
python tools/flash_multihop.py --board c6 --role b --port COM12 --flash
```

Substitute the ports shown on your computer. The helper first asks esptool to
identify the selected chip and flash capacity. It refuses the write if the chip
does not match or if the capacity is below the profile requirement. It then
writes the bootloader, partition table, and application at the manifest offsets.
Flashing replaces existing firmware. Close monitors and other serial programs
before running it.

For USB bridge experiments without Wi-Fi/BLE relay, or to build for another
supported board, see [`docs/BOARD_SUPPORT.md`](docs/BOARD_SUPPORT.md).

## 3. Start the relay

Open the sibling `retropicto-relay` repo and follow its README to create the
local `relay.local.json`, start the server, and optionally expose it through a
secure tunnel. The setup script creates distinct random tokens locally. Do not
share that file. The gateway `url` must be an origin only, for example
`ws://127.0.0.1:8787` or `wss://your-tunnel-host`; it appends `/relay/a` or
`/relay/b` automatically.

Keep the server and tunnel terminals running. A local PC gateway can connect
directly to the server on loopback. A phone on another network needs the secure
tunnel origin.

## 4. Connect PC gateways

With both repos next to each other, use the shared relay config for each PC
gateway. Open one terminal per side:

```powershell
cd ../retropicto
python tools/ble_gateway.py --config ../retropicto-relay/relay.local.json --node a
```

In a second terminal, run node `b`. If a gateway is already using a node,
disconnect it before starting another. Install `pyserial` as well if using the
USB bridge scripts; those are separate same-computer experiments.

For Android, follow `retropicto-android/README.md` and import the node's URL,
role, and token from the sibling relay repo's generated app configuration.
Run `npm run provision-android -- a --url wss://YOUR-TUNNEL-HOST` in
`retropicto-relay` to create `android-a.local.json`; use `b` for the other
phone. The exporter will not overwrite an existing file or print tokens. Keep
the relay token private. Do not configure Android and PC gateways for the same
node at the same time.

## 5. Join PictoChat and check the link

1. Power both boards and start their matching gateways.
2. On DS A, enter Room A. On DS B, enter Room B.
3. Wait for the remote participant to appear, then send a drawing each way.
4. If a drawing does not arrive, check the gateway connection, node role, relay
   room and opposite side, then review the firmware and gateway logs.

The relay has no offline history. A packet ACK means the receiving adapter
consumed it; it does not prove the DS displayed it. C6 firmware has not yet had
a fresh hardware smoke test for this extracted repo. S3 and WROOM images are
experimental, build-checked only, and need DS radio validation.

## Troubleshooting

| Problem | Check |
| --- | --- |
| No serial port | Use a data cable, install the board's USB driver if needed, and close serial monitors. |
| Helper rejects the chip | Confirm the selected board family matches the connected device. |
| Helper rejects flash capacity | This image requires 8 MiB for C6/S3 or 4 MiB for WROOM; choose/build a profile for the board. |
| Hash mismatch | Re-extract or restore the image and manifest from the same repository snapshot. |
| BLE device missing | Power the board, enable Bluetooth permissions, move closer, and disconnect another central. |
| Relay rejects gateway | Match `--node`, token, room, and opposite side; do not add `/relay/a` or `/relay/b` to URL. |
| Remote DS participant missing | Verify both consoles joined the expected room, both gateways are connected, and A/B board roles match. |
