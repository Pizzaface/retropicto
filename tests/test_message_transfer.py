import pathlib
import struct
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'python'))
from pictochat.message import CMD_GROUP, REPLY_GROUP, Decoder, decode
from pictochat import capture as analyze


def frame(app, source=b'client', sequence=0):
    return (source, b'host00', REPLY_GROUP, 2, 1,
            b'\x56\x8e' + app + struct.pack('<H', sequence))


def announcement(total, token=1):
    return struct.pack('<HHBBHH', 0, 20, 1, 0, 0xffff, total) + bytes(6) + struct.pack('<I', token)


def fragment(offset, data, final=False):
    return struct.pack('<HHBBBBH2s', 2, 12 + len(data), 1, 4, len(data), final, offset, b'\0\0') + data


class TransferTests(unittest.TestCase):
    def setUp(self):
        self.body = b'\x03\x02' + bytes.fromhex('220039d7a3bc') + bytes(28) + bytes(range(256)) * 4

    def packets(self, source=b'client'):
        yield frame(announcement(len(self.body)), source)
        for offset in range(0, len(self.body), 160):
            data = self.body[offset:offset + 160]
            yield frame(fragment(offset, data, offset + len(data) == len(self.body)), source, 0xaaaa)

    def test_exact_pixels_including_fragment_boundaries_and_short_tail(self):
        messages = list(decode(self.packets()))
        self.assertEqual(len(messages), 1)
        self.assertEqual(messages[0].body, self.body)
        self.assertEqual(messages[0].bitmap, bytes(range(256)) * 4)
        self.assertEqual(messages[0].sender.hex(':'), '00:22:d7:39:bc:a3')

    def test_missing_fragment_is_not_filled_from_another_client(self):
        packets = list(self.packets())
        packets[3] = list(self.packets(b'other!'))[3]
        self.assertEqual(list(decode(packets)), [])

    def test_host_prefix_and_footer_are_not_message_data(self):
        host_packets = []
        for src, dst, bssid, kind, subtype, payload in self.packets():
            app = payload[2:-2]
            host_packets.append((b'host00', CMD_GROUP, b'host00', 2, 2,
                                 b'\xe6\x03\x02\x00\x56\x9e' + app + b'\xaa\xaa\x00\x00'))
        self.assertEqual(list(decode(host_packets))[0].body, self.body)

    def test_early_final_flag_does_not_claim_completion(self):
        packets = list(self.packets())
        packets[1] = frame(fragment(0, self.body[:160], final=True))
        self.assertEqual(list(decode(packets)), [])

    def test_final_fragment_can_arrive_before_gap_is_filled(self):
        packets = list(self.packets())
        packets.append(packets.pop(3))
        self.assertEqual(list(decode(packets))[0].body, self.body)

    def test_retransmissions_do_not_emit_duplicate_messages(self):
        packets = list(self.packets())
        doubled = [p for packet in packets for p in (packet, packet)]
        self.assertEqual(len(list(decode(doubled))), 1)

    def test_conflicting_overlap_rejects_transfer(self):
        packets = list(self.packets())
        packets.insert(2, frame(fragment(0, b'bad data')))
        self.assertEqual(list(decode(packets)), [])

    def test_new_announcement_does_not_inherit_old_fragments(self):
        packets = list(self.packets())
        packets.insert(3, frame(announcement(len(self.body), token=2)))
        self.assertEqual(list(decode(packets)), [])

    def test_truncated_fragment_rejected(self):
        packets = list(self.packets())
        packet = list(packets[3])
        packet[-1] = packet[-1][:-1]
        packets[3] = tuple(packet)
        self.assertEqual(list(decode(packets)), [])

    def test_application_api_matches_shared_c_fixture(self):
        data = (ROOT / 'tests/fixtures/send-client-apps.bin').read_bytes()
        decoder = Decoder()
        messages = []
        pos = 0
        while pos < len(data):
            length = int.from_bytes(data[pos:pos + 2], 'little')
            pos += 2
            message = decoder.feed_application(b'client', data[pos:pos + length])
            if message is not None:
                messages.append(message)
            pos += length
        self.assertEqual(len(messages), 1)
        self.assertEqual(messages[0].body, (ROOT / 'tests/fixtures/send-message.bin').read_bytes())
        self.assertIsNone(decoder.feed_application(b'client', b''))
        self.assertEqual(decoder.rejected, 1)

    def test_message_handler_runs_once_after_complete_reassembly(self):
        events = []
        packets = list(self.packets())
        repeated = [frame for packet in packets for frame in (packet, packet)]
        messages = list(decode(repeated, on_message=events.append))
        self.assertEqual(events, messages)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0].body, self.body)
        partial_events = []
        list(decode(packets[:-1], on_message=partial_events.append))
        self.assertEqual(partial_events, [])

    def test_handler_exception_propagates_without_reemitting(self):
        def fail(message):
            raise RuntimeError('application failure')
        decoder = Decoder(on_message=fail)
        packets = list(self.packets())
        for packet in packets[:-1]:
            decoder.feed(packet)
        with self.assertRaisesRegex(RuntimeError, 'application failure'):
            decoder.feed(packets[-1])
        self.assertIsNone(decoder.feed(packets[-1]))

    def test_real_send_capture(self):
        messages = list(decode(analyze.parse_pcap(ROOT / 'tests/fixtures/send.pcap')))
        self.assertEqual(len(messages), 1)
        self.assertEqual(len(messages[0].body), 2084)
        self.assertEqual(len(messages[0].bitmap), 2048)
        self.assertEqual(messages[0].sender.hex(':'), '00:22:d7:39:bc:a3')

    def test_real_incomplete_corners_is_not_a_complete_bitmap(self):
        self.assertEqual(list(decode(analyze.parse_pcap(ROOT / 'tests/fixtures/corners.pcap'))), [])


if __name__ == '__main__':
    unittest.main()
