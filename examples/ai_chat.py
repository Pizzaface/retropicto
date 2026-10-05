"""Speak with an AI over PictoChat.

A Relay board (esp32c6usb firmware) on USB hosts the DS room. This script joins
that room as a remote participant named "AI": every drawing a DS sends is shown
to a vision model, and its answer is written back into the room as a drawing.

    python3 -m venv .venv && .venv/bin/pip install pyserial pillow
    OPENAI_API_KEY=... .venv/bin/python examples/ai_chat.py --port /dev/cu.usbmodem1101

Join the Relay's room on a DS, wait for "AI" to appear, then draw or write.
"""
import argparse
import base64
import json
import os
import pathlib
import queue
import random
import sys
import tempfile
import threading
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'python'), str(ROOT / 'tools')]
from pictochat import canvas, drawing  # noqa: E402
from usb_bridge import Lines, SerialEndpoint, decode, encode  # noqa: E402

STATE, DRAWING, ACK, LEAVE = 1, 2, 3, 4
PEER = 1  # our remote peer id; the C6 rejects 0
MAC = bytes.fromhex('024149000001')  # locally administered, must not collide with a DS
PROMPT = ('You are chatting inside Nintendo DS PictoChat. Each image is a hand-drawn '
          'message from a person. Reply in plain text, at most 120 characters, no emoji.')


def ask(history, png, model):
    """Send the drawing (plus recent turns) to OpenAI; return its text reply."""
    url = 'data:image/png;base64,' + base64.b64encode(png).decode()
    history.append({'role': 'user', 'content': [{'type': 'image_url', 'image_url': {'url': url}}]})
    request = urllib.request.Request(
        'https://api.openai.com/v1/chat/completions',
        data=json.dumps({'model': model, 'max_completion_tokens': 1000,
                         'messages': [{'role': 'system', 'content': PROMPT}] + history}).encode(),
        headers={'Authorization': 'Bearer ' + os.environ['OPENAI_API_KEY'],
                 'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=60) as response:
        text = json.load(response)['choices'][0]['message']['content'].strip()
    history.append({'role': 'assistant', 'content': text})
    del history[:-8]  # ponytail: last 4 exchanges only; images are token-heavy
    return text


# Only part of the bitmap is visible; the name tag covers the first ruled line.
LEFT, VISIBLE = canvas.VISIBLE_LEFT, canvas.VISIBLE_WIDTH


def wrap(text, font, width):
    """Greedy word wrap by measured pixel width; words wider than a line are split."""
    lines = []
    for word in text.split():
        if lines and font.getlength(lines[-1] + ' ' + word) <= width:
            lines[-1] += ' ' + word
            continue
        lines.append('')
        for char in word:
            if lines[-1] and font.getlength(lines[-1] + char) > width:
                lines.append('')
            lines[-1] += char
    return lines


def render(text):
    """Text -> 256x80 grid of palette indices inside the visible message box."""
    from PIL import Image, ImageDraw, ImageFont
    image = Image.new('1', (canvas.WIDTH, 80))
    pen, font = ImageDraw.Draw(image), ImageFont.load_default(11)
    for row, line in enumerate(wrap(text, font, VISIBLE - 12)[:4], 1):
        pen.text((LEFT + 4, row * 16 + 2), line, fill=1, font=font)
    return [[1 if image.getpixel((x, y)) else 0 for x in range(canvas.WIDTH)] for y in range(80)]


def png_of(bitmap):
    """Visible part of a DS drawing as a PNG, for the model."""
    grid = [row[LEFT:LEFT + VISIBLE] for row in canvas.detile_indices(bitmap)]
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / 'drawing.png'
        canvas.write_png(path, grid, scale=3, palette=canvas.PALETTE)
        return path.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', required=True, help='Relay serial port')
    parser.add_argument('--model', default='gpt-6-luna')
    parser.add_argument('--name', default='AI')
    args = parser.parse_args()
    if 'OPENAI_API_KEY' not in os.environ:
        parser.error('set OPENAI_API_KEY')

    board = SerialEndpoint(args.port)
    boot, generation = random.randint(1, 0xffffffff), 1
    state = drawing.state_payload(boot, generation, drawing.profile(MAC, args.name, 'Draw to me!'))
    inbox, replies, history, seen = queue.Queue(), queue.Queue(), [], set()
    outgoing, seq, last_sent, next_tick, lines = None, 0, 0.0, 0.0, Lines()

    def answer():  # one worker: replies stay in order and history is single-owner
        while True:
            try:
                text = ask(history, png_of(inbox.get()), args.model)
            except Exception as error:  # keep the room alive; tell the DS what happened
                text = f'(error: {error})'
            print('AI:', text, flush=True)
            replies.put(text)

    threading.Thread(target=answer, daemon=True).start()

    print(f'Joining room on {args.port} as {args.name!r}; Ctrl-C to leave.', flush=True)
    try:
        while True:
            now = time.monotonic()
            if now >= next_tick:  # heartbeat: peer expires after 6 s, room closes after 10 s
                board.send(b'@OPEN\n')
                board.send(encode(STATE, 0, state, sender=PEER))
                next_tick = now + 1
            if outgoing is None and not replies.empty():
                seq += 1
                body = drawing.message_body(canvas.tile(render(replies.get())), MAC)
                outgoing = (seq, drawing.drawing_payload(boot, generation, body, token=random.getrandbits(32)), now)
                last_sent = 0.0
            if outgoing and now - last_sent > 4:  # firmware ACKs once queued for the DS
                if now - outgoing[2] > 30:
                    print('Reply dropped: no ACK from Relay', flush=True)
                    outgoing = None
                else:
                    board.send(encode(DRAWING, outgoing[0], outgoing[1], sender=PEER))
                    last_sent = now
            for line in lines.feed(board.read()):
                packet = decode(line)
                if packet is None:
                    continue
                kind, packet_seq, payload, sender, recipient = packet
                if kind == ACK and recipient == PEER and outgoing and packet_seq == outgoing[0]:
                    outgoing = None
                elif kind == DRAWING:
                    # ACK every copy so the Relay stops retrying; answer each drawing once.
                    board.send(encode(ACK, packet_seq, payload[:4] + packet_seq.to_bytes(4, 'little'),
                                      sender=PEER, recipient=sender))
                    bitmap = drawing.drawing_bitmap(payload)
                    if bitmap and (payload[:4], packet_seq) not in seen:
                        seen.add((payload[:4], packet_seq))
                        print(f'DS slot {sender} drew {len(bitmap)} bytes; asking {args.model}', flush=True)
                        inbox.put(bitmap)
    except KeyboardInterrupt:
        pass
    finally:
        board.send(encode(LEAVE, 0, b'', sender=PEER))
        board.close()


if __name__ == '__main__':
    main()
