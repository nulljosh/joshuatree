from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parent.parent.parent
pixels = bytearray()
glyphs = []
for family in ('DejaVuSans', 'DejaVuSerif', 'DejaVuSansMono'):
    for weight in ('', '-Bold'):
        for size in (16, 20, 24, 28):
            font = ImageFont.truetype(str(root / 'tools/fonts' / f'{family}{weight}.ttf'), size)
            for code in range(32, 127):
                character = chr(code)
                left, top, right, bottom = font.getbbox(character)
                width, height = right - left, bottom - top
                canvas = Image.new('L', (width, height))
                ImageDraw.Draw(canvas).text((-left, -top), character, font=font, fill=255)
                glyphs.append((len(pixels), width, height, left, top, round(font.getlength(character))))
                pixels.extend(canvas.tobytes())
lines = ['static const unsigned char editor_pixels[] = {']
lines.extend(','.join(str(value) for value in pixels[start:start + 32]) + ',' for start in range(0, len(pixels), 32))
lines += ['};', 'static const struct editor_glyph { unsigned int offset; int width, height, left, top, advance; } editor_glyphs[] = {']
lines.extend('{' + ','.join(map(str, glyph)) + '},' for glyph in glyphs)
lines += ['};', '']
(root / 'drivers/editor_fonts.h').write_text('\n'.join(lines))
