import asyncio
import base64
import io
import json
from pathlib import Path
import struct
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import bot


def source(seq=1):
    body = (bot.ROOT / 'tests/fixtures/send-message.bin').read_bytes()
    a = bytearray(20)
    a[:4] = b'\0\0\x14\0'; a[4] = 1
    struct.pack_into('<H', a, 8, len(body))
    struct.pack_into('<I', a, 16, seq)
    payload = struct.pack('<II', 20, 30) + a + body
    profile = bytearray(bot.profile()); profile[2:8] = body[2:8]
    state = struct.pack('<II', 20, 30) + profile
    return bytes(payload), bytes(state)


class Socket:
    def __init__(self):
        self.input = asyncio.Queue()
        self.sent = []

    def __aiter__(self):
        return self

    async def __anext__(self):
        return await self.input.get()

    async def send(self, data):
        self.sent.append(bot.unpack(data))


class CodecTests(unittest.TestCase):
    def test_render_all_heights_and_validate_wire(self):
        payload, _ = source()
        for rows in range(1, 11):
            template = payload[28:64] + bytes(1024 * rows)
            pages = list(bot.reply_bodies('A readable reply with a longword' * 7, template))
            self.assertTrue(pages)
            for body in pages:
                self.assertEqual(len(body), 10276)
                self.assertEqual(body[2:8], bot.SWAPPED_MAC)
                grid = bot.detile(body[36:])
                self.assertTrue(any(0 in row for row in grid))
                self.assertTrue(all(all(v == 255 for v in row[:84]) for row in grid[:16]))
                self.assertTrue(all(all(v == 255 for v in row[:24]) for row in grid[16:]))
                if rows >= 2:
                    self.assertTrue(all(all(v == 255 for v in row) for row in grid[:3]))
                image = bot.Image.open(io.BytesIO(base64.b64decode(bot.png_data(body[36:]).split(',')[1])))
                self.assertEqual(image.size, (768, 240))
                a = bytearray(payload[8:28]); struct.pack_into('<H', a, 8, len(body))
                outgoing = payload[:8] + a + body
                self.assertTrue(bot.drawing_valid(outgoing))
                self.assertEqual(bot.unpack(bot.packet(2, 1, outgoing)), (2, 1, outgoing))

    def test_word_wrap_newlines_and_no_truncation(self):
        font = bot.reply_font()
        text = 'alpha beta gamma delta epsilon zeta'
        lines = bot.wrap_reply(text, font, 65)
        self.assertEqual(' '.join(lines), text)
        self.assertTrue(all(font.getlength(line, mode='1') <= 65 for line in lines))
        self.assertEqual(bot.wrap_reply('first\nsecond\n\nthird', font, 168),
                         ['first', 'second', '', 'third'])
        word = 'W' * 100
        lines = bot.wrap_reply(word, font, 40)
        self.assertEqual(''.join(lines), word)
        self.assertTrue(all(font.getlength(line, mode='1') <= 40 for line in lines))
        long_reply = ('A complete reply must keep every word. ' * 15).strip()
        self.assertEqual(' '.join(bot.wrap_reply(long_reply, font, 168)), long_reply)
        payload, _ = source()
        pages = list(bot.reply_bodies(long_reply, payload[28:]))
        lines = bot.wrap_reply(long_reply, font, lambda i: 250 - bot.REPLY_LINES[i % 6][0])
        self.assertEqual(len(pages), (len(lines) + 5) // 6)
        self.assertGreater(len(pages), 1)
        self.assertTrue(any(len(line) > len(lines[0]) for line in lines[1:6]))

    def test_oversized_word_splits_across_lines_and_pages(self):
        font = bot.reply_font()
        word = 'Supercalifragilisticexpialidocious' * 30
        width = lambda i: 250 - bot.REPLY_LINES[i % len(bot.REPLY_LINES)][0]
        lines = bot.wrap_reply(word, font, width)
        self.assertEqual(''.join(lines), word)
        self.assertGreater(len(lines), len(bot.REPLY_LINES))
        for i, line in enumerate(lines):
            self.assertLessEqual(max(font.getlength(line, mode='1'), font.getbbox(line, mode='1')[2]), width(i))
        payload, _ = source()
        pages = list(bot.reply_bodies(word, payload[28:]))
        self.assertEqual(len(pages), (len(lines) + 5) // 6)
        for body in pages:
            grid = bot.detile(body[36:])
            self.assertTrue(all(all(pixel == 255 for pixel in row[250:]) for row in grid))

    def test_unicode_preserved_and_rendered_with_fallback_fonts(self):
        text = 'Caf\u00e9 na\u00efve \u00a3\u20ac \u201cHello\u201d \u2014 \u00d7 \u00f7 \u2264 \u2265 \u03c0 \u2192 \u2665 \u2605'
        font = bot.reply_font()
        self.assertEqual(font.prepare_text(text), text)
        lines = bot.wrap_reply(text, font, 166)
        self.assertEqual(' '.join(lines), text)
        self.assertEqual(bot.wrap_reply('Cafe\u0301', font, 166), ['Caf\u00e9'])
        self.assertIn('[U+', font.prepare_text('\U0010ffff'))
        payload, _ = source()
        for body in bot.reply_bodies(text, payload[28:]):
            grid = bot.detile(body[36:])
            self.assertTrue(any(0 in row for row in grid))
            self.assertTrue(all(all(v == 255 for v in row[250:]) for row in grid))

    def test_svg_monochrome_bounds_and_rejected_resources(self):
        svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 226 58"><rect width="226" height="58" fill="black"/></svg>'
        tiles = bot.render_svg(svg)
        self.assertEqual(len(tiles), 10240)
        grid = bot.detile(tiles)
        self.assertEqual(sum(v == 0 for row in grid for v in row), 226 * 58)
        self.assertTrue(all(v == 255 for row in grid[:20] for v in row))
        self.assertTrue(all(row[23] == 255 and row[250] == 255 for row in grid))
        for markup in [
            '<svg viewBox="0 0 226 58"><script>alert(1)</script></svg>',
            '<svg viewBox="0 0 226 58"><image href="file:///secret"/></svg>',
            '<svg viewBox="0 0 226 58"><path fill="url(https://example.com)"/></svg>',
            '<!DOCTYPE svg [<!ENTITY x "bad">]><svg/>',
            '<svg viewBox="0 0 99999 99999"/>',
            '<svg viewBox="0 0 226 58"><circle r="1e100"/></svg>',
            '<svg viewBox="0 0 226 58">' + '<g/>' * 257 + '</svg>',
        ]:
            with self.assertRaises(ValueError): bot.render_svg(markup)

    def test_model_svg_tool_repairs_then_returns_text_and_image(self):
        class Model(bot.Chat):
            def __init__(self):
                super().__init__('http://localhost/v1', 'test', 'test')
                self.requests = []
            def completion(self, messages, tools=True):
                self.requests.append((list(messages), tools))
                turn = len(self.requests)
                if turn == 3:
                    return {'content': 'A circle: \u25cb'}
                svg = '<svg><script/></svg>' if turn == 1 else '<svg viewBox="0 0 226 58"><circle cx="113" cy="29" r="20"/></svg>'
                return {'content': None, 'tool_calls': [{'id': 'call' + str(turn), 'type': 'function',
                    'function': {'name': 'draw_svg', 'arguments': json.dumps({'svg': svg})}}]}
        model = Model()
        reply = model.request(bytes(1024))
        self.assertIsInstance(reply, bot.Reply)
        self.assertEqual(reply.text, 'A circle: \u25cb')
        self.assertEqual(len(reply.drawings), 1)
        self.assertFalse(json.loads(model.requests[1][0][-1]['content'])['ok'])
        self.assertTrue(json.loads(model.requests[2][0][-1]['content'])['ok'])
        self.assertFalse(model.requests[2][1])
        self.assertEqual([m['role'] for m in model.history[0]],
                         ['user', 'assistant', 'tool', 'assistant', 'tool', 'assistant'])

    def test_conversation_keeps_bounded_complete_turns(self):
        class Model(bot.Chat):
            def completion(self, messages, tools=True):
                self.last_messages = list(messages)
                return {'content': 'Remember this answer.'}
        model = Model('http://localhost/v1', 'test', 'test')
        model.request(bytes(1024))
        model.request(bytes(2048))
        self.assertEqual([m['role'] for m in model.last_messages],
                         ['system', 'user', 'assistant', 'user'])
        self.assertEqual(model.last_messages[2]['content'], 'Remember this answer.')
        for _ in range(10): model.request(bytes(1024))
        self.assertEqual(len(model.history), 8)
        self.assertTrue(all([m['role'] for m in turn] == ['user', 'assistant'] for turn in model.history))
        model.reset()
        model.request(bytes(1024))
        self.assertEqual([m['role'] for m in model.last_messages], ['system', 'user'])

    def test_reset_during_inference_cannot_restore_old_history(self):
        started, finish = threading.Event(), threading.Event()
        class Model(bot.Chat):
            def completion(self, messages, tools=True):
                started.set()
                if not finish.wait(3): raise TimeoutError('test gate')
                return {'content': 'Stale answer'}
        model = Model('http://localhost/v1', 'test', 'test')
        worker = threading.Thread(target=model.request, args=(bytes(1024),))
        worker.start()
        try:
            self.assertTrue(started.wait(2))
            model.reset()
        finally:
            finish.set(); worker.join(3)
        self.assertFalse(worker.is_alive())
        self.assertEqual(model.history, [])

    def test_emoji_sequences_inline_and_large_pages(self):
        clusters = ['\U0001f600', '\U0001f44d\U0001f3fd', '\U0001f1fa\U0001f1f8',
                    '\U0001f469\u200d\U0001f680', '1\ufe0f\u20e3']
        font = bot.reply_font()
        for cluster in clusters:
            self.assertIsNotNone(bot.emoji_key(cluster))
            self.assertEqual(font.prepare_text(cluster), cluster)
            self.assertEqual(bot.graphemes(cluster), [cluster])
            self.assertEqual(font.getlength(cluster), 12)
            image = bot.emoji_image(cluster, 48)
            self.assertEqual(image.mode, '1')
            self.assertEqual(image.size, (48, 48))
            self.assertEqual(set(image.get_flattened_data()), {0, 255})
        text = 'Hi ' + ' '.join(clusters) + ' bye'
        lines = bot.wrap_reply(text, font, 40)
        self.assertEqual(' '.join(lines), text)
        payload, _ = source()
        for body in bot.reply_bodies(text, payload[28:]):
            grid = bot.detile(body[36:])
            self.assertTrue(all(v == 255 for row in grid for v in row[250:]))
        pages = list(bot.reply_bodies(' '.join(clusters), payload[28:]))
        self.assertEqual(len(pages), 2)
        for body in pages:
            grid = bot.detile(body[36:])
            self.assertTrue(all(v == 255 for row in grid[:20] for v in row))
            self.assertTrue(all(v == 255 for row in grid for v in row[250:]))
            self.assertTrue(any(0 in row for row in grid[22:70]))

    def test_malformed_inputs_and_urls(self):
        payload, _ = source()
        self.assertFalse(bot.drawing_valid(payload[:28] + b'\0' + payload[29:]))
        raw = bytearray(bot.packet(2, 1, payload)); raw[-1] ^= 1
        with self.assertRaises(ValueError): bot.unpack(bytes(raw))
        for url in ('http://example.com/v1', 'https://user:secret@example.com/v1', 'https://example.com/v1?k=secret'):
            with self.assertRaises(ValueError): bot.validate_url(url)

    def test_compatible_http_contract(self):
        seen = []
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                seen.append((self.path, self.headers['Authorization'], request))
                reply = json.dumps({'choices': [{'message': {'content': 'Hello DS!'}}]}).encode()
                self.send_response(200); self.end_headers(); self.wfile.write(reply)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try:
            chat = bot.Chat(f'http://127.0.0.1:{server.server_port}/v1', 'test-vision', 'test-only')
            self.assertEqual(chat.request(bytes(1024)), 'Hello DS!')
            path, auth, request = seen[0]
            self.assertEqual(path, '/v1/chat/completions')
            self.assertEqual(auth, 'Bearer test-only')
            self.assertEqual(request['model'], 'test-vision')
            self.assertFalse(request['stream'])
            self.assertEqual(request['tools'][0]['function']['name'], 'draw_svg')
            self.assertFalse(request['parallel_tool_calls'])
            self.assertTrue(request['messages'][1]['content'][1]['image_url']['url'].startswith('data:image/png;base64,'))
        finally:
            server.shutdown(); server.server_close(); thread.join()


class SessionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.ws = Socket()
        self.bot = bot.Participant(self.ws, None)
        self.task = asyncio.create_task(self.bot.receive())
        self.payload, self.state = source()

    async def asyncTearDown(self):
        self.task.cancel()
        await asyncio.gather(self.task, return_exceptions=True)

    async def feed(self, kind, seq, body):
        self.ws.input.put_nowait(bot.packet(kind, seq, body))
        await asyncio.sleep(0)

    async def test_dedupe_backpressure_and_session_reset(self):
        await self.feed(1, 0, self.state)
        for seq in (1, 1, 2, 3):
            await self.feed(2, seq, self.payload)
        self.assertEqual(self.bot.queue.qsize(), 2)
        self.assertEqual([seq for kind, seq, _ in self.ws.sent if kind == 3], [1, 1, 2])
        self.bot.queue.get_nowait()
        await self.feed(2, 3, self.payload)
        self.assertEqual(self.bot.high_seq, 3)
        await self.feed(1, 0, struct.pack('<II', 20, 0) + self.state[8:])
        self.assertTrue(self.bot.queue.empty())
        self.assertIsNone(self.bot.peer)

    async def test_ack_requires_matching_boot_and_sequence(self):
        await self.feed(1, 0, self.state)
        body = next(bot.reply_bodies('Hi!', self.payload[28:]))
        sending = asyncio.create_task(self.bot.send_reply(self.bot.revision, self.payload, body))
        await asyncio.sleep(0)
        await self.feed(3, 1, struct.pack('<II', self.bot.boot ^ 1, 1))
        self.assertFalse(sending.done())
        await self.feed(3, 1, struct.pack('<II', self.bot.boot, 1))
        self.assertTrue(await asyncio.wait_for(sending, 1))
        self.assertEqual(len([p for p in self.ws.sent if p[0] == 2]), 1)

    async def test_multi_page_reply_waits_for_each_ack(self):
        text = '\n'.join('Line ' + str(i) for i in range(13))
        class Chat:
            async def reply(self, bitmap): return text
        self.bot.chat = Chat()
        await self.feed(1, 0, self.state)
        await self.feed(2, 1, self.payload)
        worker = asyncio.create_task(self.bot.work())
        try:
            expected = list(bot.reply_bodies(text, self.payload[28:]))
            self.assertEqual(len(expected), 3)
            for i, body in enumerate(expected, 1):
                for _ in range(50):
                    drawings = [p for p in self.ws.sent if p[0] == 2]
                    if len(drawings) >= i: break
                    await asyncio.sleep(0.01)
                self.assertEqual(len(drawings), i)
                self.assertEqual(drawings[-1][1], i)
                self.assertEqual(drawings[-1][2][28:], body)
                self.assertTrue(bot.drawing_valid(drawings[-1][2]))
                await asyncio.sleep(0.02)
                self.assertEqual(len([p for p in self.ws.sent if p[0] == 2]), i)
                await self.feed(3, i, struct.pack('<II', self.bot.boot, i))
            await asyncio.sleep(0.02)
            self.assertIsNone(self.bot.pending)
        finally:
            worker.cancel()
            await asyncio.gather(worker, return_exceptions=True)

    async def test_slow_model_keeps_heartbeat_and_drops_stale_answer(self):
        gate = asyncio.Event()
        class SlowChat:
            async def reply(self, bitmap):
                await gate.wait()
                return 'This reply belongs to the old session.'
        self.bot.chat = SlowChat()
        await self.feed(1, 0, self.state)
        await self.feed(2, 1, self.payload)
        worker = asyncio.create_task(self.bot.work())
        heartbeat = asyncio.create_task(self.bot.heartbeat())
        try:
            await asyncio.sleep(1.05)
            self.assertGreaterEqual(len([p for p in self.ws.sent if p[0] == 1]), 2)
            await self.feed(1, 0, struct.pack('<II', 21, 30) + self.state[8:])
            gate.set(); await asyncio.sleep(0.05)
            self.assertFalse(any(p[0] == 2 for p in self.ws.sent))
        finally:
            worker.cancel(); heartbeat.cancel()
            await asyncio.gather(worker, heartbeat, return_exceptions=True)


if __name__ == '__main__':
    unittest.main()
