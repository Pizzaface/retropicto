"""Forward validated PictoChat relay frames over USB or a private TCP connection.

Same-PC trial: python tools/usb_bridge.py --ports COM12 COM5
Two PCs: --port COM12 --listen 127.0.0.1:26712 (use SSH forwarding)
         --port COM5 --connect 127.0.0.1:26712
The C6s own message ACKs/retries; this process never acknowledges a drawing.
"""
import argparse
import datetime
import pathlib
import socket
import struct
import threading
import time

MAX_PAYLOAD = 10304
MAX_LINE = 6 + 2 * (16 + MAX_PAYLOAD)
PREFIX = b'@PCTR '


def checksum(data):
    h = 2166136261
    for value in data:
        h = ((h ^ value) * 16777619) & 0xffffffff
    return h


def valid_length(kind, size):
    return ((kind == 1 and size == 92) or (kind == 3 and size == 8) or
            (kind == 2 and 1088 <= size <= MAX_PAYLOAD and (size - 64) % 1024 == 0))


def decode(line):
    """Return (kind, sequence, payload) only for a complete, valid wire frame."""
    line = line.rstrip(b'\r\n')
    if not line.startswith(PREFIX) or len(line) > MAX_LINE:
        return None
    encoded = line[len(PREFIX):]
    if len(encoded) % 2 or any(c not in b'0123456789abcdefABCDEF' for c in encoded):
        return None
    raw = bytes.fromhex(encoded.decode('ascii'))
    if len(raw) < 16:
        return None
    magic, version, kind, size, seq, digest = struct.unpack('<4sBBHII', raw[:16])
    body = raw[16:]
    if magic != b'PCTR' or version != 1 or not valid_length(kind, size):
        return None
    if len(body) != size or checksum(body) != digest:
        return None
    return kind, seq, body


def encode(kind, seq, body):
    if not valid_length(kind, len(body)):
        raise ValueError('Invalid relay payload length')
    header = struct.pack('<4sBBHII', b'PCTR', 1, kind, len(body), seq, checksum(body))
    return PREFIX + (header + body).hex().encode('ascii') + b'\n'


class Lines:
    """Bounded line decoder. Discard overflow through the next newline."""
    def __init__(self):
        self.buffer = bytearray()
        self.overflow = False

    def feed(self, data):
        result = []
        for value in data:
            if value == 10:
                if not self.overflow:
                    result.append(bytes(self.buffer).rstrip(b'\r'))
                self.buffer.clear()
                self.overflow = False
            elif len(self.buffer) < MAX_LINE + 1:
                self.buffer.append(value)
            else:
                self.overflow = True
        return result


class SerialEndpoint:
    def __init__(self, name, reset=False):
        import serial
        self.name = name
        self.device = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=3)
        self.device.dtr = False
        self.device.rts = False
        self.device.port = name
        self.device.open()
        if reset:
            self.device.rts = True
            self.device.dtr = self.device.dtr
            time.sleep(0.2)
            self.device.rts = False
            self.device.dtr = self.device.dtr
            time.sleep(0.2)

    def read(self):
        return self.device.read(min(max(self.device.in_waiting, 1), 4096))

    def send(self, data):
        # Leading newline allows recovery after an interrupted write/reconnection.
        data = b'\n' + data
        while data:
            count = self.device.write(data[:1024])
            if not count:
                raise OSError('Serial write stalled')
            data = data[count:]

    def close(self):
        self.device.close()


class TcpEndpoint:
    def __init__(self, connection):
        self.device = connection
        self.device.settimeout(0.2)
        self.name = 'TCP'

    def read(self):
        try:
            data = self.device.recv(4096)
        except socket.timeout:
            return b''
        if not data:
            raise EOFError('TCP peer disconnected')
        return data

    def send(self, data):
        # Total write deadline; partial writes must never silently lose a frame.
        view = memoryview(b'\n' + data)
        deadline = time.monotonic() + 3
        while view:
            if time.monotonic() >= deadline:
                raise TimeoutError('TCP write stalled')
            try:
                n = self.device.send(view)
            except socket.timeout:
                continue
            if not n:
                raise EOFError('TCP write failed')
            view = view[n:]

    def close(self):
        self.device.close()


def pump(source, destination, stop, report, errors):
    lines = Lines()
    last_state = None
    try:
        while not stop.is_set():
            for line in lines.feed(source.read()):
                packet = decode(line)
                if packet is None:
                    if line and not line.startswith(PREFIX):
                        report(source.name, line.decode('utf-8', errors='replace'), False)
                    continue
                destination.send(encode(*packet))
                kind, seq, body = packet
                if kind != 1 or body != last_state:
                    report(source.name, f'FORWARD kind={kind} seq={seq} bytes={len(body)}', True)
                    if kind == 1:
                        last_state = body
    except (OSError, EOFError) as error:
        if not stop.is_set():
            errors.append(error)
            report(source.name, str(error), True)
            stop.set()


def address(value):
    host, port = value.rsplit(':', 1)
    number = int(port)
    if not host or not 1 <= number <= 65535:
        raise ValueError('Expected host:port')
    return host, number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--ports', nargs=2, metavar=('LEFT', 'RIGHT'))
    group.add_argument('--port')
    network = parser.add_mutually_exclusive_group()
    network.add_argument('--listen', type=address)
    network.add_argument('--connect', type=address)
    parser.add_argument('--reset', action='store_true')
    parser.add_argument('--seconds', type=float, default=0, help='0 runs until Ctrl-C')
    args = parser.parse_args()
    if bool(args.port) != bool(args.listen or args.connect):
        parser.error('--port requires exactly one of --listen or --connect; --ports uses neither')
    if args.ports and args.ports[0].upper() == args.ports[1].upper():
        parser.error('Use two distinct serial ports')
    if args.seconds < 0:
        parser.error('--seconds cannot be negative')
    root = pathlib.Path('captures_out') / datetime.date.today().isoformat()
    root.mkdir(parents=True, exist_ok=True)
    path = root / ('usb-bridge-' + datetime.datetime.now().strftime('%H%M%S-%f') + '.log')
    stop = threading.Event()
    errors, endpoints, workers = [], [], []
    log_lock = threading.Lock()
    with path.open('x', encoding='utf-8', buffering=1) as log:
        def report(name, message, important):
            row = datetime.datetime.now().isoformat(timespec='milliseconds') + f' {name} {message}'
            with log_lock:
                log.write(row + '\n')
                if important or any(s in message for s in ('USB relay', 'USB peer', 'Remote DS',
                        'Accepted remote', 'DRAW received', 'DRAW outbound', 'ROOM ready',
                        'dropped', 'Guru', 'ESP_ERROR', 'abort')):
                    print(row, flush=True)
        try:
            if args.port:
                if args.listen:
                    with socket.create_server(args.listen) as listener:
                        print(f'Waiting at {args.listen}; use a private link or SSH tunnel.', flush=True)
                        connection, _ = listener.accept()
                else:
                    connection = socket.create_connection(args.connect, timeout=10)
                endpoints.append(TcpEndpoint(connection))
                endpoints.insert(0, SerialEndpoint(args.port, args.reset))
            else:
                for name in args.ports:
                    endpoints.append(SerialEndpoint(name, args.reset))
            print(f'BRIDGING {endpoints[0].name} <-> {endpoints[1].name}; LOG {path}', flush=True)
            for source, destination in (endpoints, endpoints[::-1]):
                thread = threading.Thread(target=pump, args=(source, destination, stop, report, errors))
                thread.start()
                workers.append(thread)
            deadline = time.monotonic() + args.seconds if args.seconds else None
            while not stop.wait(0.1):
                if deadline is not None and time.monotonic() >= deadline:
                    break
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
            for worker in workers:
                worker.join(timeout=4)
            for endpoint in endpoints:
                endpoint.close()
        print('SAVED', path, flush=True)
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
