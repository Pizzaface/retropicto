import importlib.util
import pathlib
import socket
import threading
import unittest

spec = importlib.util.spec_from_file_location('usb_bridge', pathlib.Path(__file__).parents[1] / 'tools/usb_bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class RelayTests(unittest.TestCase):
    def test_wire_sizes_and_checksum(self):
        self.assertEqual(bridge.checksum(b'hello'), 0x4f9f2cab)
        for kind, lengths in [(1, [92]), (3, [8]), (4, [0]), (2, [64 + 1024*n for n in range(1, 11)])]:
            for length in lengths:
                body = bytes(i % 251 for i in range(length))
                frame = bridge.encode(kind, 123, body, 5, 0x1234)
                self.assertEqual(bridge.decode(frame), (kind, 123, body, 5, 0x1234))
                if not length:
                    continue
                corrupt = bytearray(frame)
                corrupt[-2] = ord('0') if corrupt[-2] != ord('0') else ord('1')
                self.assertIsNone(bridge.decode(bytes(corrupt)))
                self.assertIsNone(bridge.decode(frame[:-3] + b'\n'))
        for kind, size in [(0, 92), (1, 91), (2, 64), (2, 1089), (2, 11328), (3, 9), (4, 1)]:
            with self.assertRaises(ValueError):
                bridge.encode(kind, 1, bytes(size))

    def test_fragmentation_overflow_and_log_noise(self):
        frame = bridge.encode(1, 0, bytes(92))
        lines = bridge.Lines()
        self.assertEqual(lines.feed(b'x' * (bridge.MAX_LINE + 50)), [])
        self.assertLessEqual(len(lines.buffer), bridge.MAX_LINE + 1)
        self.assertEqual(lines.feed(b'\n'), [])
        got = []
        stream = b'boot log\r\n' + frame + b'warning\n' + frame
        for byte in stream:
            got.extend(lines.feed(bytes([byte])))
        self.assertEqual(sum(bridge.decode(line) is not None for line in got), 2)
        self.assertIsNone(bridge.decode(frame[:12] + b' ' + frame[12:]))

    def test_real_tcp_forwarding_both_directions_without_generated_acks(self):
        host_a, device_a = socket.socketpair()
        host_b, device_b = socket.socketpair()
        endpoints = [bridge.TcpEndpoint(host_a), bridge.TcpEndpoint(host_b)]
        for device in (device_a, device_b):
            device.settimeout(1)
        stop, errors = threading.Event(), []
        workers = []
        try:
            for source, dest in (endpoints, endpoints[::-1]):
                t = threading.Thread(target=bridge.pump, args=(source, dest, stop, lambda *a: None, errors))
                t.start()
                workers.append(t)
            drawing = bridge.encode(2, 77, bytes(1088))
            device_a.sendall(b'normal log\n@PCTR invalid\n' + drawing[:25])
            device_a.sendall(drawing[25:])
            def receive(device, expected):
                data = b''
                while len(data) < len(expected):
                    data += device.recv(4096)
                self.assertEqual(data, expected)
            receive(device_b, b'\n' + bridge.encode(2, 77, bytes(1088), sender=1))
            device_a.settimeout(0.05)
            with self.assertRaises(socket.timeout):
                device_a.recv(1)  # PC must not invent consumption ACKs.
            device_a.settimeout(1)
            ack = bridge.encode(3, 77, (42).to_bytes(4, 'little') + (77).to_bytes(4, 'little'))
            device_b.sendall(ack)
            receive(device_a, b'\n' + bridge.encode(3, 77, (42).to_bytes(4, 'little') + (77).to_bytes(4, 'little'), sender=1))
            self.assertEqual(errors, [])
        finally:
            stop.set()
            for worker in workers:
                worker.join(2)
                self.assertFalse(worker.is_alive())
            for endpoint in endpoints:
                endpoint.close()
            device_a.close()
            device_b.close()


if __name__ == '__main__':
    unittest.main()
