"""Unicode text and bounded, self-contained SVG rendering for PictoChat."""
from functools import lru_cache
from pathlib import Path
import io
import math
import subprocess
import sys
import re
import unicodedata
import xml.etree.ElementTree as ET

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont
import regex
import resvg_py
from emoji_art import emoji_key, emoji_image

FONT_DIR = Path(__file__).with_name('fonts')
FONT_FILES = [FONT_DIR / (name + '-Regular.ttf') for name in
              ('NotoSans', 'NotoSansMath', 'NotoSansSymbols', 'NotoSansSymbols2')]


def graphemes(text):
    return regex.findall(r'\X', text)


class UnicodeFont:
    def __init__(self, size=10):
        self.fonts = [ImageFont.truetype(str(p), size) for p in FONT_FILES]
        self.coverage = []
        for path in FONT_FILES:
            with TTFont(path) as font:
                self.coverage.append(set(font.getBestCmap()))

    @lru_cache(maxsize=2048)
    def runs(self, text):
        runs = []
        for cluster in graphemes(unicodedata.normalize('NFC', text)):
            if emoji_key(cluster):
                runs.append((-1, cluster))
                continue
            # Variation selectors request presentation, not a separate visible glyph.
            cluster = cluster.replace('\ufe0f', '').replace('\ufe0e', '')
            if not cluster:
                continue
            index = next((i for i, codes in enumerate(self.coverage)
                          if all(ord(c) in codes for c in cluster)), None)
            if index is None:
                cluster = ''.join('[U+%04X]' % ord(c) for c in cluster)
                index = 0
            if runs and runs[-1][0] == index:
                runs[-1] = (index, runs[-1][1] + cluster)
            else:
                runs.append((index, cluster))
        return tuple(runs)

    def prepare_text(self, text):
        return ''.join(run for _, run in self.runs(text))

    def getlength(self, text, mode='1'):
        return sum(12 if i == -1 else self.fonts[i].getlength(run, mode='1')
                   for i, run in self.runs(text))

    def getbbox(self, text, mode='1'):
        x, boxes = 0, []
        for i, run in self.runs(text):
            if i == -1:
                boxes.append((x, -10, x + 11, 1))
                x += 12
                continue
            font = self.fonts[i]
            l, t, r, b = font.getbbox(run, mode='1', anchor='ls')
            boxes.append((x + l, t, x + r, b))
            x += font.getlength(run, mode='1')
        if not boxes:
            return (0, 0, 0, 0)
        return (min(b[0] for b in boxes), min(b[1] for b in boxes),
                max(b[2] for b in boxes), max(b[3] for b in boxes))

    def draw(self, image, text, left, top, max_height=11):
        l, t, r, b = self.getbbox(text)
        if r <= l or b <= t:
            return
        # Tall combining marks/symbols stay within the existing line spacing.
        strip = Image.new('1', (math.ceil(max(r, self.getlength(text)) - min(l, 0)), b - t), 1)
        draw = ImageDraw.Draw(strip)
        x = -min(l, 0)
        for i, run in self.runs(text):
            if i == -1:
                strip.paste(emoji_image(run), (round(x), -10 - t))
                x += 12
                continue
            draw.text((x, -t), run, font=self.fonts[i], fill=0, anchor='ls')
            x += self.fonts[i].getlength(run, mode='1')
        if strip.height > max_height:
            strip = strip.resize((strip.width, max_height), Image.Resampling.NEAREST)
        image.paste(strip, (left, top))


@lru_cache(maxsize=1)
def reply_font():
    return UnicodeFont()


def image_tiles(image):
    if image.size != (256, 80):
        raise ValueError('Expected 256x80 canvas')
    image = image.convert('1', dither=Image.Dither.NONE)
    tiles = bytearray(10240)
    for y in range(80):
        for x in range(256):
            if not image.getpixel((x, y)):
                at = ((y // 8) * 32 + x // 8) * 32 + (y % 8) * 4 + (x % 8) // 2
                tiles[at] |= 1 << (4 * (x % 2))
    return bytes(tiles)


TAGS = {'svg', 'g', 'path', 'rect', 'circle', 'ellipse', 'line', 'polyline', 'polygon', 'text', 'tspan', 'title', 'desc'}
ATTRS = {'viewBox', 'width', 'height', 'x', 'y', 'x1', 'y1', 'x2', 'y2', 'cx', 'cy', 'r', 'rx', 'ry',
         'd', 'points', 'fill', 'stroke', 'stroke-width', 'stroke-linecap', 'stroke-linejoin',
         'stroke-miterlimit', 'stroke-dasharray', 'stroke-dashoffset', 'fill-rule', 'opacity',
         'fill-opacity', 'stroke-opacity', 'transform', 'font-size', 'font-family',
         'font-weight', 'text-anchor', 'dx', 'dy', 'preserveAspectRatio'}


def clean_svg(svg):
    if not isinstance(svg, str) or len(svg.encode('utf-8')) > 32768:
        raise ValueError('SVG must fit in 32 KB')
    if '<!' in svg or '<?' in svg:
        raise ValueError('SVG declarations and entities are not supported')
    try:
        root = ET.fromstring(svg)
    except ET.ParseError as error:
        raise ValueError('Malformed SVG') from error
    def tag_name(tag):
        return tag.removeprefix('{http://www.w3.org/2000/svg}')
    if tag_name(root.tag) != 'svg':
        raise ValueError('Expected SVG root')
    elements = list(root.iter())
    if len(elements) > 256:
        raise ValueError('Too many SVG elements')
    def visit(node, depth):
        if depth > 16 or tag_name(node.tag) not in TAGS:
            raise ValueError('Unsupported SVG element or nesting')
        for name, value in node.attrib.items():
            if name not in ATTRS or len(value) > 8192:
                raise ValueError('Unsupported SVG attribute')
            if name not in ('fill', 'stroke', 'font-family', 'text-anchor', 'preserveAspectRatio'):
                for number in re.findall(r'[-+]?(?:\d*\.\d+|\d+)(?:[eE][-+]?\d+)?', value):
                    if not math.isfinite(float(number)) or abs(float(number)) > 10000:
                        raise ValueError('SVG numeric value is too large')
            if re.search(r'url\s*\(|https?:|file:|data:|@', value, re.I):
                raise ValueError('SVG resources are not supported')
        for child in node:
            visit(child, depth + 1)
    visit(root, 0)
    if root.get('viewBox') != '0 0 226 58':
        raise ValueError('Use viewBox="0 0 226 58"')
    # Fixed output allocation; document dimensions never control raster size.
    root.set('width', '226'); root.set('height', '58')
    for node in elements:
        if tag_name(node.tag) in ('text', 'tspan'):
            node.set('font-family', 'Noto Sans')
    return ET.tostring(root, encoding='unicode')


def _raster_svg(cleaned):
    png = resvg_py.svg_to_bytes(svg_string=cleaned, width=904, height=232,
        background='white', skip_system_fonts=True, font_files=[str(p) for p in FONT_FILES])
    drawing = Image.open(io.BytesIO(png)).convert('L').resize((226, 58), Image.Resampling.LANCZOS)
    drawing = drawing.point(lambda p: 255 if p >= 160 else 0, mode='1')
    if drawing.getextrema() == (255, 255):
        raise ValueError('SVG has no visible ink')
    canvas = Image.new('1', (256, 80), 1)
    canvas.paste(drawing, (24, 20))
    return image_tiles(canvas)


def render_svg(svg):
    cleaned = clean_svg(svg)
    try:
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--raster-svg'],
            input=cleaned.encode('utf-8'), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            timeout=8, check=True,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except (subprocess.SubprocessError, OSError) as error:
        raise ValueError('SVG rendering failed or exceeded its time limit') from error
    if len(result.stdout) != 10240:
        raise ValueError('Invalid SVG raster size')
    return result.stdout


if __name__ == '__main__':
    if sys.argv[1:] != ['--raster-svg']:
        raise SystemExit(2)
    sys.stdout.buffer.write(_raster_svg(sys.stdin.buffer.read(32769).decode('utf-8')))
