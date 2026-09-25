# Two-room relay example

This guide connects two DS consoles through ESP32-C6 BLE adapters, two
gateways, and a WebSocket relay. It summarizes the working topology; current
installation commands live in [`../../INSTALL.md`](../../INSTALL.md).

```text
DS Room A <-> C6 A <-> BLE <-> gateway A <-> WSS <-> relay
DS Room B <-> C6 B <-> BLE <-> gateway B <-> WSS <-> relay
```

Firmware is in [`../../firmware/esp32`](../../firmware/esp32), the PC BLE
gateway is [`../../tools/ble_gateway.py`](../../tools/ble_gateway.py), and the
portable protocol implementation is [`../../lib/pictochat`](../../lib/pictochat).
The sibling `retropicto-android` repository contains the phone gateway. The
sibling `retropicto-relay` repository contains the server and local relay
configuration. Keep its generated tokens private.

The relay pairs node sides `a` and `b` in one configured room. Use a secure WSS
origin for gateways outside the relay computer; the gateway appends its node
route. Each ESP32 advertises `PICTO-A` or `PICTO-B` and hosts DS Room A or Room B.
Only one gateway may own each node at a time.

The C6 BLE path has delivered drawings between real DS consoles in both
directions. Prebuilt C6 images in this repository still need a fresh smoke
test. S3 and original ESP32/WROOM builds are experimental and need hardware
validation before treating them as supported.
