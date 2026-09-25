"""A vision chat participant using the existing authenticated PictoChat relay."""
import argparse
from dataclasses import dataclass
import asyncio
import base64
import io
import json
import os
from pathlib import Path
import secrets
import struct
import sys
import threading
import unicodedata
import urllib.request
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'python')]
from usb_bridge import encode, decode
from pictochat.canvas import detile
from PIL import Image, ImageDraw, ImageFont
from websockets.asyncio.client import connect
from rendering import reply_font, graphemes, image_tiles, render_svg
from emoji_art import emoji_key, emoji_image

MAC = bytes.fromhex('020000504943')
SWAPPED_MAC = bytes(MAC[i ^ 1] for i in range(6))
# Calibrated from the user's maximum-size DS capture; sender replaced below.
MAX_CANVAS_HEADER = bytes.fromhex(
    '03020000000000000005000000000306080d080d121b0000000000000000000000000000')
REPLY_LINES = ((84, 3), (24, 20), (24, 32), (24, 44), (24, 56), (24, 68))

PROMPT = ('You are AI BOT, a friendly AI participant in Nintendo DS PictoChat. '
          'Read the typed or handwritten message in the image and reply to it. '
          'If unreadable, ask the user to write it again. Use Unicode punctuation, accents and '
          'symbols and emoji naturally. Emoji (including flags and joined sequences) are '
          'converted locally to monochrome artwork; do not spell them as Unicode codes. '
          'An emoji-only reply is displayed as larger artwork. Keep text replies concise (about 200 characters); use line breaks '
          'for separate points. No Markdown. Answer the message rather than describing it. '
          'When asked to draw, use draw_svg. The DS message canvas is 256x80 pixels, but your '
          'SVG drawing area is exactly 226x58 pixels (wide and shallow), with one SVG unit '
          'equal to one final display pixel. Received images are enlarged 3x for reading; '
          'that is not extra drawing resolution. Make a simple bold drawing for a very small '
          'monochrome screen; do not paste SVG into the text reply. Each new image is the next '
          'message in this conversation. Use previous messages and drawings for follow-up requests. '
          'Follow the latest user directions precisely, including requested content, count, format, '
          'language and exclusions. Do not add greetings, explanations, captions, confirmations, '
          'or follow-up questions unless requested or needed to resolve a genuinely unclear message. '
          'Treat words such as only, exactly, and no text as strict output constraints. '
          'For emoji-only requests, output the actual requested Unicode emoji and nothing else, '
          'never its name, description, U+ code, or an SVG substitute. '
          'For drawing requests, call draw_svg rather than describing what you would draw. '
          'After a successful drawing tool call, return empty text unless the user requested '
          'a caption or explanation. The drawing itself is the response. '
          'Apply follow-up edits to the previous requested result, preserving everything the user '
          'did not ask to change. If the message is unreadable, ask briefly for clarification '
          'instead of guessing. These directions govern response format and do not override safety rules.')


SVG_TOOL = {'type': 'function', 'function': {
    'name': 'draw_svg', 'description': (
        'Draw an SVG in exactly 226x58 final pixels. Set width="226" height="58" '
        'viewBox="0 0 226 58". One SVG unit equals one final DS pixel. '
        'The converter places it at (24,20) on the 256x80 message canvas; use LOCAL '
        'SVG coordinates and do not add that offset yourself. Keep all visible ink, '
        'including half the stroke width, inside x=2..224 and y=2..56. Fit the entire '
        'subject in this wide, shallow area without clipping or stretching its proportions. '
        'Black ink on white, no gray-dependent details; stroke-width 1.5 to 2 or larger. '
        'Tiny details will disappear at 1-bit resolution. Text labels use Noto Sans at '
        '10-12 pixels minimum; keep them short, avoid overlap, and put longer captions '
        'in the normal text response instead. Supported: svg, g, path, '
        'rect, circle, ellipse, line, polyline, polygon, text, tspan. Inline presentation '
        'attributes only. No style, scripts, images, href, use, filters, resources or entities. '
        'Keep markup below 32 KB. Use text sparingly (Noto Sans, at least 10px).'),
    'strict': True,
    'parameters': {'type': 'object', 'properties': {'svg': {'type': 'string', 'description': 'Self-contained SVG: width=226, height=58, viewBox=0 0 226 58. Local coordinates; keep strokes within the 2px inset.'}},
                   'required': ['svg'], 'additionalProperties': False}}}


@dataclass
class Reply:
    text: str
    drawings: tuple = ()


def packet(kind, seq, body):
    return bytes.fromhex(encode(kind, seq, body)[6:].decode().strip())


def unpack(raw):
    if not isinstance(raw, bytes) or len(raw) > 10320:
        raise ValueError('Invalid relay packet')
    result = decode(b'@PCTR ' + raw.hex().encode())
    if result is None:
        raise ValueError('Invalid relay packet')
    return result


def profile():
    p = bytearray(84)
    p[:8] = b'\x03\x00' + SWAPPED_MAC
    name = 'AI BOT'.encode('utf-16le')
    p[8:8 + len(name)] = name
    bio = 'AI PictoChat participant'.encode('utf-16le')
    p[28:28 + len(bio)] = bio
    p[80:] = bytes.fromhex('0b000710')
    return bytes(p)


def drawing_valid(payload):
    if len(payload) < 1088 or len(payload) > 10304 or (len(payload) - 64) % 1024:
        return False
    a, body = payload[8:28], payload[28:]
    return (a[:4] == b'\x00\x00\x14\x00' and 1 <= a[4] <= 15
            and struct.unpack_from('<H', a, 8)[0] == len(body)
            and a[10:12] == b'\0\0' and body[:2] == b'\x03\x02')


def png_data(bitmap):
    grid = detile(bitmap)
    img = Image.frombytes('L', (256, len(grid)), bytes(v for row in grid for v in row))
    output = io.BytesIO()
    img.resize((768, len(grid) * 3), Image.Resampling.NEAREST).save(output, 'PNG')
    return 'data:image/png;base64,' + base64.b64encode(output.getvalue()).decode()


def wrap_reply(text, font, width):
    """Wrap at word boundaries by pixel width, preserving explicit line breaks."""
    text = unicodedata.normalize('NFC', text)
    if hasattr(font, 'prepare_text'):
        text = '\n'.join(font.prepare_text(line) for line in text.replace('\r\n', '\n').replace('\r', '\n').split('\n'))
    text = text.replace('\r\n', '\n').replace('\r', '\n').strip() or 'Please try again.'
    lines = []
    def fits(value):
        box = font.getbbox(value, mode='1')
        limit = width(len(lines)) if callable(width) else width
        return max(font.getlength(value, mode='1'), box[2]) - min(box[0], 0) <= limit
    for paragraph in text.split('\n'):
        line = ''
        words = paragraph.split()
        if not words:
            lines.append('')
            continue
        for word in words:
            candidate = line + (' ' if line else '') + word
            if fits(candidate):
                line = candidate
                continue
            if line:
                lines.append(line)
                line = ''
            # Only split a word if that word cannot fit on an otherwise empty line.
            for char in graphemes(word):
                if line and not fits(line + char):
                    lines.append(line)
                    line = ''
                line += char
        lines.append(line)
    return lines


def reply_bodies(text, template):
    """Render full-height pages using the calibrated native message margins."""
    height = (len(template) - 36) // 128
    if height not in range(8, 81, 8) or template[:2] != b'\x03\x02':
        raise ValueError('Invalid drawing template')
    clusters = [g for g in graphemes(text.strip()) if not g.isspace()]
    if clusters and all(emoji_key(g) for g in clusters):
        # Emoji-only replies get readable 48px artwork, four per page.
        for start in range(0, len(clusters), 4):
            image = Image.new('1', (256, 80), 1)
            for i, cluster in enumerate(clusters[start:start + 4]):
                image.paste(emoji_image(cluster, 48), (24 + i * 56, 22))
            yield bitmap_body(image_tiles(image))
        return
    # Always use a calibrated full-height page, even for a short incoming question.
    height = 80
    font = reply_font()
    per_page = len(REPLY_LINES)
    lines = wrap_reply(text, font, lambda i: 250 - REPLY_LINES[i % per_page][0])
    for start in range(0, len(lines), per_page):
        image = Image.new('1', (256, height), 1)
        draw = ImageDraw.Draw(image)
        for row, value in enumerate(lines[start:start + per_page]):
            left, y = REPLY_LINES[row]
            font.draw(image, value, left, y)
        yield bitmap_body(image_tiles(image))


def bitmap_body(tiles):
    if len(tiles) != 10240:
        raise ValueError('Expected full-height drawing')
    header = bytearray(MAX_CANVAS_HEADER)
    header[2:8] = SWAPPED_MAC
    return bytes(header) + tiles


def render_reply(result, template):
    pages = [bitmap_body(drawing) for drawing in result.drawings]
    if result.text.strip():
        pages.extend(reply_bodies(result.text, template))
    return pages


def load_env(path):
    """Read explicit local settings without executing shell syntax or printing secrets."""
    if path.exists():
        for line in path.read_text(encoding='utf-8-sig').splitlines():
            key, sep, value = line.strip().partition('=')
            if sep and key in ('OPENAI_API_KEY', 'OPENAI_BASE_URL', 'OPENAI_MODEL'):
                os.environ.setdefault(key, value.strip().strip('\"\''))


def validate_url(url, websocket=False):
    u = urlsplit(url)
    secure, local = ('wss', 'ws') if websocket else ('https', 'http')
    if (not u.hostname or u.username or u.password or u.query or u.fragment
            or (u.scheme != secure and not (u.scheme == local and u.hostname in ('127.0.0.1', 'localhost', '::1')))
            or (websocket and u.path not in ('', '/'))):
        raise ValueError('Use a TLS URL (plain HTTP/WS is allowed only on loopback)')


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None  # Do not forward API credentials to a redirect target.


class Chat:
    def __init__(self, base, model, key):
        validate_url(base)
        self.url, self.model, self.key = base.rstrip('/') + '/chat/completions', model, key
        self.history = []
        self.history_epoch = 0
        self.history_lock = threading.Lock()

    def reset(self):
        with self.history_lock:
            self.history.clear()
            self.history_epoch += 1

    def remember(self, epoch, turn):
        with self.history_lock:
            if epoch != self.history_epoch:
                return
            self.history.append(turn)
            while len(self.history) > 8 or (len(self.history) > 1 and
                    len(json.dumps(self.history).encode('utf-8')) > 524288):
                self.history.pop(0)

    def completion(self, messages, tools=True):
        data = {'model': self.model, 'stream': False, 'max_tokens': 1536,
                'messages': messages}
        if self.model == 'gpt-5.6-luna':
            data['max_completion_tokens'] = data.pop('max_tokens')
            data['reasoning_effort'] = 'none'
        if tools:
            data.update(tools=[SVG_TOOL], tool_choice='auto', parallel_tool_calls=False)
        request = urllib.request.Request(self.url, data=json.dumps(data).encode(), headers={
            'Authorization': 'Bearer ' + self.key, 'Content-Type': 'application/json'})
        with urllib.request.build_opener(NoRedirect).open(request, timeout=60) as response:
            raw = response.read(1048577)
        if len(raw) > 1048576:
            raise ValueError('API response too large')
        choice = json.loads(raw)['choices'][0]
        print('API completion; token limit reached:', choice.get('finish_reason') == 'length', flush=True)
        if choice.get('finish_reason') == 'length':
            raise ValueError('API reply was truncated')
        return choice['message']

    def request(self, bitmap):
        with self.history_lock:
            epoch = self.history_epoch
            history = [message for turn in self.history for message in turn]
        messages = [{'role': 'system', 'content': PROMPT}] + history
        start = len(messages)
        messages.append({'role': 'user', 'content': [
            {'type': 'text', 'text': 'Reply to this PictoChat message.'},
            {'type': 'image_url', 'image_url': {'url': png_data(bitmap)}}]})
        drawings = []
        # One repair opportunity for invalid SVG, then a final text-only response.
        for turn in range(3):
            message = self.completion(messages, tools=turn < 2 and not drawings)
            calls = message.get('tool_calls') or []
            text = message.get('content') or ''
            if not isinstance(text, str):
                raise ValueError('Invalid API text')
            if not calls:
                if not text.strip() and not drawings:
                    raise ValueError('API returned no reply')
                messages.append({'role': 'assistant', 'content': text})
                self.remember(epoch, messages[start:])
                return Reply(text, tuple(drawings)) if drawings else text
            if turn == 2 or drawings or len(calls) != 1:
                raise ValueError('Unexpected tool calls')
            call = calls[0]
            if call.get('type') != 'function' or call['function']['name'] != 'draw_svg':
                raise ValueError('Unknown model tool')
            messages.append({'role': 'assistant', 'content': text or None, 'tool_calls': calls})
            try:
                args = json.loads(call['function']['arguments'])
                if not isinstance(args, dict) or set(args) != {'svg'}:
                    raise ValueError('draw_svg expects only svg')
                drawings.append(render_svg(args['svg']))
                result = {'ok': True, 'status': 'Converted to monochrome and queued; not yet sent or displayed.'}
                print('SVG converted to monochrome', flush=True)
            except (ValueError, TypeError) as error:
                result = {'ok': False, 'error': str(error)}
                print('SVG rejected; asking model to repair', flush=True)
            messages.append({'role': 'tool', 'tool_call_id': call['id'], 'content': json.dumps(result)})
        raise ValueError('Tool round limit')

    async def reply(self, bitmap):
        return await asyncio.to_thread(self.request, bitmap)


class Participant:
    def __init__(self, ws, chat, capture_dir=None):
        self.ws, self.chat = ws, chat
        self.capture_dir = Path(capture_dir) if capture_dir else None
        self.boot = secrets.randbelow(0xffffffff) + 1
        self.generation = 1
        self.peer = None
        self.last_seen = 0
        self.high_seq = 0
        self.sequence = 0
        self.revision = 0
        self.queue = asyncio.Queue(maxsize=2)
        self.pending = None

    def reset_peer(self, peer):
        self.peer = peer
        if hasattr(self.chat, 'reset'):
            self.chat.reset()
        self.revision += 1
        self.high_seq = 0
        while not self.queue.empty():
            self.queue.get_nowait()
        if self.pending:
            self.pending[1].set()

    async def heartbeat(self):
        while True:
            now = asyncio.get_running_loop().time()
            if self.peer and now - self.last_seen > 6:
                self.reset_peer(None)
            await self.ws.send(packet(1, 0, struct.pack('<II', self.boot, self.generation) + profile()))
            await asyncio.sleep(1)

    async def receive(self):
        async for raw in self.ws:
            kind, seq, payload = unpack(raw)
            if kind == 1:
                boot, generation = struct.unpack_from('<II', payload)
                if not boot or (generation and (payload[8] != 3 or payload[9] > 1)):
                    raise ValueError('Invalid peer state')
                peer = payload if generation else None
                if peer != self.peer:
                    self.reset_peer(peer)
                    print('DS ready' if peer else 'Waiting for DS', flush=True)
                self.last_seen = asyncio.get_running_loop().time()
            elif kind == 3:
                if (self.pending and seq == self.pending[0]
                        and payload == struct.pack('<II', self.boot, seq)):
                    self.pending[1].set()
            elif kind == 2 and seq:
                if not self.peer or payload[:8] != self.peer[:8]:
                    continue  # Wait for a current state; do not consume unknown sessions.
                if not drawing_valid(payload) or payload[30:36] != self.peer[10:16]:
                    raise ValueError('Invalid peer drawing')
                if seq > self.high_seq:
                    if self.queue.full():
                        continue  # No ACK: firmware will retry once we have capacity.
                    self.queue.put_nowait((self.revision, payload))
                    self.high_seq = seq
                # ACK means accepted into bounded application work, not AI/display success.
                await self.ws.send(packet(3, seq, payload[:4] + struct.pack('<I', seq)))

    async def send_reply(self, revision, source, body):
        self.sequence += 1
        if self.sequence > 0xffffffff:
            raise RuntimeError('Sequence exhausted; reconnect')
        seq = self.sequence
        announcement = bytearray(source[8:28])
        struct.pack_into('<H', announcement, 8, len(body))
        struct.pack_into('<I', announcement, 16, seq)
        wire = packet(2, seq, struct.pack('<II', self.boot, self.generation) + announcement + body)
        event = asyncio.Event()
        self.pending = (seq, event)
        try:
            for _ in range(4):
                if revision != self.revision:
                    return False
                await self.ws.send(wire)
                try:
                    async with asyncio.timeout(12):
                        await event.wait()
                    return revision == self.revision
                except asyncio.TimeoutError:
                    pass
            raise TimeoutError('C6 did not consume reply')
        finally:
            self.pending = None

    async def work(self):
        while True:
            revision, payload = await self.queue.get()
            if revision != self.revision:
                continue
            height = (len(payload) - 64) // 128
            print('Reading message:', len(payload) - 28, 'body bytes; canvas 256 x', height, flush=True)
            if self.capture_dir:
                self.capture_dir.mkdir(parents=True, exist_ok=True)
                (self.capture_dir / 'last-message.bin').write_bytes(payload)
                png = base64.b64decode(png_data(payload[64:]).split(',', 1)[1])
                (self.capture_dir / 'last-message.png').write_bytes(png)
                print('Saved local message capture', flush=True)
            try:
                answer = await self.chat.reply(payload[64:])
            except Exception as error:
                print('AI request failed:', type(error).__name__, flush=True)
                answer = 'The AI service could not reply. Please try again.'
            if revision != self.revision:
                continue
            result = answer if isinstance(answer, Reply) else Reply(answer)
            answer = result.text
            pages = await asyncio.to_thread(render_reply, result, payload[28:])
            if revision != self.revision:
                continue
            print('Reply layout:', len(answer), 'characters;', len(pages),
                  'pages; body bytes:', ','.join(str(len(body)) for body in pages), flush=True)
            if self.capture_dir:
                record = {'text': answer, 'characters': len(answer), 'pages': len(pages),
                          'canvas': [256, 80], 'body_bytes': [len(body) for body in pages]}
                (self.capture_dir / 'last-reply.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
                png = base64.b64decode(png_data(pages[0][36:]).split(',', 1)[1])
                (self.capture_dir / 'last-reply.png').write_bytes(png)
            consumed = 0
            for body in pages:
                if not await self.send_reply(revision, payload, body):
                    break
                consumed += 1
            print('Reply pages consumed:', consumed, '/', len(pages), '(display not verified)', flush=True)

    async def run(self):
        tasks = [asyncio.create_task(f()) for f in (self.heartbeat, self.receive, self.work)]
        try:
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)


async def run(args):
    load_env(Path(args.env_file))
    key = os.environ.get('OPENAI_API_KEY', '')
    if not key:
        raise ValueError('Set OPENAI_API_KEY in the environment or ignored .env.local')
    chat = Chat(args.base_url or os.environ.get('OPENAI_BASE_URL', 'https://api.openai.com/v1'),
                args.model or os.environ.get('OPENAI_MODEL', 'gpt-4.1-mini'), key)
    if args.check_api:
        await chat.reply(bytes(2048))
        print('Vision API request succeeded; credentials were not logged.')
        return
    config = json.loads(Path(args.config).read_text(encoding='utf-8-sig'))
    node = next(n for n in config['nodes'] if n['id'] == args.node)
    url = args.url or config['url']
    validate_url(url, websocket=True)
    async with connect(url.rstrip('/') + '/relay/' + node['id'],
                       additional_headers={'Authorization': 'Bearer ' + node['token']},
                       max_size=10320, max_queue=4, compression=None,
                       open_timeout=15, ping_interval=15, ping_timeout=15) as ws:
        print('AI BOT connected as node', args.node, flush=True)
        await Participant(ws, chat, args.capture_dir).run()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', default=str(ROOT / 'relay.local.json'))
    parser.add_argument('--env-file', default=str(ROOT / '.env.local'))
    parser.add_argument('--node', default='b')
    parser.add_argument('--url')
    parser.add_argument('--base-url')
    parser.add_argument('--model')
    parser.add_argument('--check-api', action='store_true')
    parser.add_argument('--capture-dir', help='Opt-in local capture of the latest received message')
    args = parser.parse_args()
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    except Exception as error:
        # Third-party exceptions may contain HTTP headers or returned input.
        print('Stopped:', type(error).__name__, '(check local settings and node availability)', file=sys.stderr)
        sys.exit(1)
