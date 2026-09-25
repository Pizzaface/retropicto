# Regression inputs

These are intentional test data, not generated output.

- `send.pcap`: complete DS drawing transfer for framing and reassembly tests.
- `corners.pcap`: incomplete capture which must not produce a complete image.
- `send-client-apps.bin`: applications extracted from send.pcap, each prefixed by
  a two-byte little-endian length; includes native retransmissions.
- `send-message.bin`: independently decoded expected 2084-byte message body.

C and Python share these inputs to preserve wire behavior. Other experimental
captures, logs and firmware snapshots were removed. Historical documents retain
the observations but do not imply those files still exist.
