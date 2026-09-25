# AI PictoChat participant

An `AI BOT` ghost reads PictoChat message images with a vision model and sends
short text replies rendered as native PictoChat bitmap messages. It uses the
existing C6 firmware, Android/PC BLE gateway, and Node relay without modifications.
The bot implements the remote participant endpoint; it replaces one human side.

```text
DS <-> C6 <-> phone BLE gateway <-> Node relay <-> Python AI participant
                                                       |
                                                Chat Completions API
```

## Setup

Run from the repository root with Python 3.11+:

```powershell
python -m pip install -r examples/ai-participant/requirements.txt
python examples/ai-participant/test_bot.py
```

Use the [online relay example](../online-relay/README.md) to start the Node
server, tunnel and one phone gateway. The bot uses the other node's existing
relay credentials from ignored `relay.local.json`.

Set `OPENAI_API_KEY` in your environment. Optional settings, also accepted in
the ignored root `.env.local`, are:

```dotenv
OPENAI_BASE_URL=https://api.openai.com/v1
OPENAI_MODEL=gpt-4.1-mini
```

Environment values take precedence over `.env.local`; `--base-url` and `--model`
override both. The base URL includes `/v1`, not `/chat/completions`. Compatible
providers must support Chat Completions with base64 PNG `image_url` inputs.
Use that provider's key as `OPENAI_API_KEY`. HTTPS is required except for local
loopback testing. Keys remain on the computer running the bot, never in the APK
or C6 firmware. Message bitmaps are sent to the configured AI provider.

The default model supports image input and Chat Completions; see the official
[model documentation](https://developers.openai.com/api/docs/models/gpt-4.1-mini)
and [vision guide](https://developers.openai.com/api/docs/guides/images-vision).

```powershell
# Small real API request with a synthetic blank image; no DS needed.
python examples/ai-participant/bot.py --check-api

# Keep Android A connected. Disconnect Android/PC B first.
python examples/ai-participant/bot.py --node b
```

Join Room A and send a short typed or handwritten question. Look for `AI BOT`
and a reply with that sender. Start with a clear message such as `What is 2+2?`.
For Room B instead, keep Android B and run the bot as node `a` after disconnecting
Android A. Do not run two gateways for the same node.

Stop with Ctrl-C. To restore the two-human relay, reconnect the phone gateway
for the node the bot occupied. No reflashing or server reconfiguration is needed.

## Behavior and limits

- The current DS session retains up to eight completed conversation turns
  (bounded to about 512 KB), including input images, replies, SVG tool calls and
  results. Whole oldest turns are dropped together. Memory is in-process only
  and clears when the DS leaves, its session changes, the peer times out, or the
  bot restarts. In-flight old responses cannot restore cleared history.
- Replies use full 256x80 pages, even after short incoming messages. Word wrapping
  measures pixel widths; the first line starts at x=84, y=3 and later lines at
  x=24, y=20/32/44/56/68. These margins follow the user's maximum-size native
  message capture. Six readable lines fit per page, with a six-pixel right margin.
  Explicit line breaks are preserved and additional pages wait for individual ACKs.
  Model output is bounded by 1,536 tokens to allow SVG markup; truncated API
  responses are rejected rather than rendering incomplete output.
  Full-height drawing metadata uses the captured 80-pixel template with the bot's
  sender identity, and each announcement declares the new body length.
- Heartbeats run independently of model latency. One request runs at a time;
  at most two further drawings are queued. Duplicates are acknowledged without
  triggering repeated model calls. A full queue leaves the drawing unacknowledged
  so C6 firmware can retry.
- Outgoing pages use the relay ACK and retry contract. ACK means consumed by
  the C6, not proof of pixels displayed. Four attempts, twelve seconds apart,
  bound delivery retries. Connection/delivery failure exits; rerun to reconnect.
- Membership changes discard queued work and late model answers. HTTP requests
  already in flight may finish but cannot reply into a replacement session.
- API errors produce a short in-room failure message. Logs omit credentials,
  user message text, and provider error bodies. TLS redirects are not followed.

The separate tests cover image encoding, all ten canvas heights, the compatible
HTTP request shape, relay validation, duplicate suppression, backpressure,
ACK identity, heartbeats during slow inference, and stale-session suppression.
Real DS display/readability needs a physical trial; software tests alone cannot
establish it.

Validation on September 24, 2026: six example tests and all 28 existing Python
tests passed. A live image-input request using the existing `OPENAI_API_KEY`
succeeded. The bot connected to the relay as node B; physical DS reply display
is pending user confirmation.

For local layout diagnostics, `--capture-dir captures_out/ai-layout` saves the
latest incoming wire payload and PNG. This opt-in capture contains message
content; the `captures_out` directory is Git-ignored. The confirmed maximum-size
input has a 10,276-byte body (36 metadata bytes plus 10,240 bitmap bytes).

## Unicode and model drawing tool

Text retains Unicode with NFC normalization (including composed accents). Bundled
Noto Sans, Math and Symbols fonts cover accented Latin, Greek/Cyrillic, smart
quotes, currency, common math symbols, arrows and many pictographs. Wrapping
keeps grapheme clusters together and measures the same monochrome glyphs that
are drawn. Tall marks are fitted within the line height. Unsupported glyphs are
shown as explicit `[U+XXXX]` labels instead of being silently changed to `?`;
this font set does not cover all scripts. Emoji use the separate artwork fallback below.

The model receives the `draw_svg({svg: string})` function tool through Chat
Completions. When asked to draw, it can supply SVG with `viewBox="0 0 226 58"`.
The tool renders on white, thresholds to black/white, and inserts the drawing at
(24,20) on a full 256x80 canvas, preserving the name area and six-pixel right
margin. Drawings are sent first, followed by any text, through the same ordered
ACK/retry queue. The API key remains in the gateway process.

Supported SVG elements: svg, g, path, rect, circle, ellipse, line, polyline,
polygon, text, tspan, title and desc. Use inline presentation attributes and
bold strokes. Scripts, stylesheets, external resources, images, use/filters and
entities are rejected. Input is bounded to 32 KB, 256 elements and 16 levels;
rasterization runs in a separate process with an eight-second timeout and fixed
output size. An invalid SVG gets one model repair opportunity. Successful tool
results say converted/queued, never claim that pixels were displayed.

The implementation follows the official [function-calling contract](https://developers.openai.com/api/docs/guides/function-calling)
and uses [resvg-py](https://pypi.org/project/resvg-py/) for local rasterization.
A compatible endpoint must support function tools as well as image inputs.

Try `Draw a cat` or `Reply with accented letters and math symbols` in Room B.
Validation: 12 offline tests pass; a live API request generated and converted an
SVG cat. The local Unicode and drawing previews were inspected. DS display of
these additions still requires a physical trial.

The model is explicitly told that SVG units are final DS pixels, not the 3x
input preview scale: a 226x58 local viewport, inserted at (24,20) by the converter.
It should keep ink within a two-pixel inset, use strokes of at least 1.5-2 pixels,
and labels at least 10-12 pixels, putting longer captions in the text response.

## Emoji conversion

Emoji text now uses 4,009 locally bundled Twemoji v17.0.1 SVG assets. Whole
Unicode grapheme sequences are looked up before font fallback, including skin
tones, regional flags, keycaps and joined emoji such as the woman astronaut.
Variation-selector spellings are normalized for asset lookup; explicit text
presentation stays in the font path. Unavailable sequences retain the visible
Unicode-code fallback.

Mixed text uses 11-pixel monochrome emoji with a 12-pixel advance, measured as
one unbroken unit when wrapping. Emoji-only replies use 48-pixel artwork,
four per full-height message. Colors are converted to black/white, with outlines
added to keep light faces and shapes visible; color-only distinctions and fine
inline details cannot survive the DS's monochrome resolution.

No artwork is fetched at runtime. Twemoji graphics are by Twitter and the Twemoji
contributors, licensed under CC BY 4.0. See [artwork attribution and license](emoji/README.md).
The modified rendering consists of scaling, monochrome conversion and outlines.

Validation: 15 example tests pass, including faces, skin tones, flags, keycaps,
joined sequences, wrapping and two-page emoji output. Large and inline previews
were visually inspected. Try asking the bot to reply with emoji only.
