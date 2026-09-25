"""BLE-to-WebSocket PictoChat gateway. USB is not used for relay data."""
import argparse
import asyncio
import json
import time
from pathlib import Path
from urllib.parse import urlsplit
from bleak import BleakClient, BleakScanner
from websockets.asyncio.client import connect
from ble_wire import SERVICE, RX, TX, MAX_PACKET, Assembly, fragments, valid_packet

async def session(node, base):
    print('Scanning for', node['ble_name'], flush=True)
    device = await BleakScanner.find_device_by_filter(
        lambda d, a: a.local_name == node['ble_name'] and SERVICE in a.service_uuids, timeout=15)
    if device is None:
        raise RuntimeError('BLE node not found')
    ended = asyncio.Event()
    async with BleakClient(device, disconnected_callback=lambda _: ended.set(), timeout=20) as client:
        async with connect(base.rstrip('/') + '/relay/' + node['id'],
                           additional_headers={'Authorization': 'Bearer ' + node['token']},
                           max_size=MAX_PACKET, max_queue=4, compression=None,
                           open_timeout=15, ping_interval=15, ping_timeout=15) as ws:
            queue = asyncio.Queue(maxsize=4)
            assembly = Assembly()
            def notification(_, data):
                packet = assembly.feed(data)
                if packet is not None:
                    try:
                        queue.put_nowait(packet)
                    except asyncio.QueueFull:
                        ended.set()  # Reconnect rather than accumulate stale drawings.
            await client.start_notify(TX, notification)
            print('CONNECTED', node['id'], 'BLE MTU', client.mtu_size, flush=True)
            async def uplink():
                last_state = None
                while True:
                    packet = await queue.get()
                    await ws.send(packet)
                    if packet[5] != 1 or packet != last_state:
                        if packet[5] == 1: last_state = packet
                        print('UPLINK', node['id'], 'kind', packet[5], 'bytes', len(packet), flush=True)
            async def downlink():
                last_state = None
                rx = client.services.get_characteristic(RX)
                async for packet in ws:
                    if not isinstance(packet, bytes) or not valid_packet(packet):
                        raise ValueError('Invalid server packet')
                    # A GATT response per fragment can take seconds on Windows
                    # and starve peer heartbeats. Pace write commands instead;
                    # firmware validates/retries the complete drawing end to end.
                    started = time.monotonic()
                    mtu = min(client.mtu_size, rx.max_write_without_response_size + 3)
                    for fragment in fragments(packet, mtu):
                        await client.write_gatt_char(rx, fragment, response=False)
                        await asyncio.sleep(0.015)
                    if packet[5] != 1 or packet != last_state:
                        if packet[5] == 1: last_state = packet
                        print('DOWNLINK', node['id'], 'kind', packet[5], 'bytes', len(packet), 'seconds', round(time.monotonic()-started, 3), 'MTU', mtu, flush=True)
            tasks = [asyncio.create_task(uplink()), asyncio.create_task(downlink()), asyncio.create_task(ended.wait())]
            try:
                done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                for task in done:
                    task.result()
            finally:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)

async def run(args):
    config = json.loads(Path(args.config).read_text(encoding='utf-8-sig'))
    node = next(n for n in config['nodes'] if n['id'] == args.node)
    base = args.url or config['url']
    url = urlsplit(base)
    if url.scheme != 'wss' and not (url.scheme == 'ws' and url.hostname in ('localhost', '127.0.0.1', '::1')):
        raise ValueError('Use wss://, or ws:// for loopback testing only')
    if url.username or url.password or url.query or url.fragment or url.path not in ('', '/'):
        raise ValueError('URL must be a server origin without credentials/path/query')
    while True:
        try:
            await session(node, base)
        except (OSError, RuntimeError, TimeoutError, ValueError) as error:
            print('Reconnect:', type(error).__name__, flush=True)
        except Exception as error:
            # BLE/WinRT and WebSocket errors contain platform-specific details;
            # avoid printing request headers or token-bearing configuration.
            print('Reconnect:', type(error).__name__, flush=True)
        await asyncio.sleep(3)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', default='relay.local.json')
    parser.add_argument('--node', required=True)
    parser.add_argument('--url')
    args = parser.parse_args()
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
