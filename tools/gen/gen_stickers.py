#!/usr/bin/env python3
"""Draws the Joshua Tree sticker sheet into docs/brand/stickers/: three 3 inch stickers as SVG, fonts and art embedded,
ready for a print shop (Sticker Mule takes SVG or PNG and cuts around the shape). The die-cut tree needs no file of its own:
upload docs/brand/tree.png, the painting on nothing, and the shop cuts around it. Colours are the brand's: paper
#f4f3ec, bark ink #261a13, terracotta #b5502c, the park's sun gold #ffd98a.

  park      circle: the park at dusk, the name in its sky
  prompt    rounded bar: the Terminal prompt, joshua@tree %, with a terracotta caret
  scratch   rounded square: the glyph and "A computer, from scratch."

Run: python3 tools/gen/gen_stickers.py. The PNGs beside them are rendered from these SVGs at 1200 px."""
import base64, os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'docs', 'brand', 'stickers')
PAPER, INK, TERRA, GOLD = '#f4f3ec', '#261a13', '#b5502c', '#ffd98a'

def b64(path): return base64.b64encode(open(os.path.join(ROOT, path), 'rb').read()).decode()

def fonts(*faces):
    return ''.join(f"@font-face{{font-family:'{fam}';font-weight:{wt};src:url(data:font/woff2;base64,{b64('landing/fonts/' + f + '.woff2')}) format('woff2')}}"
                   for f, fam, wt in faces)

DISPLAY = ('TreeSansDisplay-Bold', 'Tree Sans Display', 700)
SEMI = ('TreeSans-SemiBold', 'Tree Sans', 600)

def svg(w, h, body, css=''):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
            f'<defs><style>{css}</style></defs>{body}</svg>\n')

S = 900  # 3 inches at 300 dpi

def tree():
    # The painted tree, white border all round so the cut line has room (Sticker Mule cuts around the alpha).
    pad = 60
    img = f'data:image/png;base64,{b64("docs/brand/tree.png")}'
    return svg(S, S, f'<defs><filter id="o" x="-10%" y="-10%" width="120%" height="120%"><feMorphology in="SourceAlpha" operator="dilate" radius="22"/>'
                     f'<feFlood flood-color="#fff"/><feComposite in2="SourceAlpha" operator="in" result="w"/>'
                     f'<feMorphology in="SourceAlpha" operator="dilate" radius="22" result="d"/><feFlood flood-color="#fff"/><feComposite in2="d" operator="in"/>'
                     f'<feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>'
                     f'<image href="{img}" x="{pad}" y="{pad}" width="{S - 2 * pad}" height="{S - 2 * pad}" filter="url(#o)"/>')

def park():
    img = f'data:image/png;base64,{b64("docs/brand/park-wall.png")}'
    r = S / 2
    return svg(S, S, f'<defs><clipPath id="c"><circle cx="{r}" cy="{r}" r="{r - 18}"/></clipPath></defs>'
                     f'<circle cx="{r}" cy="{r}" r="{r}" fill="#fff"/>'
                     f'<g clip-path="url(#c)"><image href="{img}" x="-420" y="-60" width="{S * 16 / 9 + 120}" height="{S + 60}" preserveAspectRatio="xMidYMax slice" style="image-rendering:pixelated"/></g>'
                     f'<text x="{r}" y="340" text-anchor="middle" font-family="Tree Sans Display" font-weight="700" font-size="128" letter-spacing="-4" fill="#fff">Joshua Tree</text>',
               fonts(DISPLAY, SEMI))

def prompt():
    w, h = S, 300
    return svg(w, h, f'<rect width="{w}" height="{h}" rx="72" fill="#fff"/><rect x="16" y="16" width="{w - 32}" height="{h - 32}" rx="58" fill="{INK}"/>'
                     f'<circle cx="92" cy="92" r="14" fill="#ff5f57"/><circle cx="134" cy="92" r="14" fill="#febc2e"/><circle cx="176" cy="92" r="14" fill="#28c840"/>'
                     f'<text x="84" y="208" font-family="Tree Sans" font-weight="600" font-size="84" letter-spacing="-1" fill="{PAPER}">joshua@tree %</text>'
                     f'<rect x="700" y="140" width="44" height="80" rx="6" fill="{TERRA}"/>',
               fonts(SEMI))

def scratch():
    glyph = f'data:image/png;base64,{b64("docs/brand/glyph.png")}'
    return svg(S, S, f'<rect width="{S}" height="{S}" rx="150" fill="#fff"/><rect x="18" y="18" width="{S - 36}" height="{S - 36}" rx="134" fill="{PAPER}"/>'
                     f'<defs><mask id="m" style="mask-type:alpha"><image href="{glyph}" x="330" y="120" width="240" height="240"/></mask></defs>'
                     f'<rect x="330" y="120" width="240" height="240" fill="{INK}" mask="url(#m)"/>'
                     f'<text x="{S / 2}" y="520" text-anchor="middle" font-family="Tree Sans Display" font-weight="700" font-size="112" letter-spacing="-4" fill="{INK}">A computer,</text>'
                     f'<text x="{S / 2}" y="640" text-anchor="middle" font-family="Tree Sans Display" font-weight="700" font-size="112" letter-spacing="-4" fill="{TERRA}">from scratch.</text>'
                     f'<text x="{S / 2}" y="760" text-anchor="middle" font-family="Tree Sans" font-weight="600" font-size="38" letter-spacing="6" fill="{INK}" opacity=".6">JOSHUA TREE</text>',
               fonts(DISPLAY, SEMI))

if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    for name, f in (('park', park), ('prompt', prompt), ('scratch', scratch)):
        p = os.path.join(OUT, name + '.svg')
        open(p, 'w').write(f())
        print('wrote', os.path.relpath(p, ROOT))
