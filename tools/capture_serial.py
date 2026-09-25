"""Capture independent WROOM/C6 serial logs without one port blocking the other."""
import argparse
import concurrent.futures
import datetime
import pathlib
import threading
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ports', nargs='+', required=True,
                        help='one or more actual serial ports (for example COM5 COM12 or /dev/ttyUSB0)')
    parser.add_argument('--seconds', type=float, default=120)
    parser.add_argument('--label', default='mp-trace')
    parser.add_argument('--reset', action='store_true', help='Reset boards via RTS after opening their ports')
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error('--seconds must be positive')
    if not args.label or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.label):
        parser.error('--label must contain only letters, digits, hyphens or underscores')
    now = datetime.datetime.now()
    root = pathlib.Path('captures_out') / now.strftime('%Y-%m-%d')
    root.mkdir(parents=True, exist_ok=True)
    stamp = now.strftime('%H%M%S-%f')
    ports = []
    stop = threading.Event()
    try:
        for name in args.ports:
            port = serial.Serial(port=None, baudrate=115200, timeout=0.2)
            port.dtr = False
            port.rts = False
            port.port = name
            port.open()
            ports.append(port)
            if args.reset:
                port.rts = True
                port.dtr = port.dtr  # usbser.sys needs a DTR write to propagate RTS.
                time.sleep(0.2)
                port.rts = False
                port.dtr = port.dtr
                time.sleep(0.2)
        end = time.monotonic() + args.seconds

        def record(port):
            path = root / f'{args.label}-{stamp}-{port.port}.log'
            with path.open('x', encoding='utf-8', buffering=1) as log:
                print('RECORDING', port.port, path, flush=True)
                while not stop.is_set() and time.monotonic() < end:
                    raw = port.readline()
                    if not raw:
                        continue
                    line = raw.decode(errors='replace').rstrip()
                    row = datetime.datetime.now().isoformat(timespec='milliseconds') + ' ' + line
                    log.write(row + '\n')
                    if any(s in line for s in ('online:', 'MP ', 'MGMT ', 'connected', 'CLIENT JOINED',
                                               'Error', 'Guru', 'ESP_ERROR_CHECK', 'HOST APP:',
                                               'admission', 'ADMISSION', 'DRAW received',
                                               'DRAW bot', 'DRAW rejected', 'DRAW BEGIN', 'DRAW END')):
                        print(port.port, row, flush=True)
            return str(path)

        with concurrent.futures.ThreadPoolExecutor(max_workers=len(ports)) as pool:
            futures = [pool.submit(record, port) for port in ports]
            try:
                for future in concurrent.futures.as_completed(futures):
                    print('SAVED', future.result(), flush=True)
            finally:
                stop.set()
    finally:
        for port in ports:
            port.close()


if __name__ == '__main__':
    main()
