"""Offline Twemoji lookup and monochrome rasterization (CC BY 4.0 artwork)."""
from functools import lru_cache
import io
from pathlib import Path
import zipfile
from PIL import Image, ImageChops, ImageFilter
import resvg_py


@lru_cache(maxsize=1)
def assets():
    return zipfile.ZipFile(Path(__file__).with_name('emoji') / 'twemoji-svg.zip')


@lru_cache(maxsize=4096)
def emoji_key(cluster):
    # Text presentation is respected; VS16 may be omitted in Twemoji filenames.
    if '\ufe0e' in cluster:
        return None
    points = [format(ord(c), 'x') for c in cluster]
    for key in ('-'.join(points), '-'.join(p for p in points if p != 'fe0f')):
        if key + '.svg' in assets().NameToInfo:
            return key
    return None


@lru_cache(maxsize=512)
def emoji_image(cluster, size=11):
    key = emoji_key(cluster)
    if key is None:
        raise ValueError('Unsupported emoji sequence')
    svg = assets().read(key + '.svg').decode('utf-8')
    png = resvg_py.svg_to_bytes(svg_string=svg, width=size * 4, height=size * 4,
                               skip_system_fonts=True)
    rgba = Image.open(io.BytesIO(png)).convert('RGBA').resize((size, size), Image.Resampling.LANCZOS)
    alpha = rgba.getchannel('A').point(lambda p: 255 if p >= 128 else 0)
    white = Image.new('RGBA', rgba.size, 'white')
    gray = Image.alpha_composite(white, rgba).convert('L')
    dark = gray.point(lambda p: 255 if p < 160 else 0)
    # Outline light-colored faces/shapes so they don't disappear on white.
    padded = Image.new('L', (size + 2, size + 2), 0)
    padded.paste(alpha, (1, 1))
    inner = padded.filter(ImageFilter.MinFilter(3)).crop((1, 1, size + 1, size + 1))
    edge = ImageChops.subtract(alpha, inner)
    ink = ImageChops.lighter(dark, edge)
    return ImageChops.invert(ink).convert('1', dither=Image.Dither.NONE)
