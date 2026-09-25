# Multi-Hop bundle validation — September 24, 2026

All six final firmware environments compiled and linked with the pinned
pioarduino 55.03.39 / ESP-IDF 5.5.4 toolchain:

- `multihop_c6_a`, `multihop_c6_b`
- `multihop_s3_a`, `multihop_s3_b`
- `multihop_esp32_a`, `multihop_esp32_b`

The original ESP32 build needed room-state allocation from internal heap to
fit its static DRAM window. C6/S3 retain static room storage. C6-only private
driver diagnostic layouts are excluded from S3/ESP32; the shared TX-completion
frame helper remains available on all three chips.

Software checks passed:

- Android debug APK assembly, unit tests, and lint.
- 28 existing Python tests and two release-packaging tests.
- Three Node relay tests, including authenticated drawing and ACK forwarding.
- Packaging tests for credential/config exclusions, checksums, executable Gradle
  wrapper permissions, correct chip offsets, and final PlatformIO image selection.

Rebuild firmware from the project root with PlatformIO Core:

```powershell
pio run --disable-auto-clean -e multihop_c6_a -e multihop_c6_b -e multihop_s3_a -e multihop_s3_b -e multihop_esp32_a -e multihop_esp32_b
```

These are build and software results, not a hardware certification. No boards
were flashed during packaging. The prior two-phone/two-DS confirmation used C6
with the older MAC-selected image. The new explicit-role C6 images need a smoke
test; S3 and original ESP32/WROOM need full DS admission, drawing, reconnect,
and runtime-memory validation. Follow [BOARD_SUPPORT.md](BOARD_SUPPORT.md).
