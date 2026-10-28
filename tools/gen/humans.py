#!/usr/bin/env python3
"""Draws docs/apps-for-humans.svg: how a new app reaches your Pi, in plain words, for someone who has never read the code
(the store, Samantha, or you), and what the seal means. Colours from the brand kit's tokens, like tools/gen/map.py."""
import re, os
ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
TOK = dict(re.findall(r'--([a-z-]+):\s*(#[0-9a-f]{6})', open(os.path.join(ROOT, 'docs', 'brand', 'tokens.css')).read().split('@media')[0]))
PAPER, INK, MUT, RULE, CARD, T = TOK['bg'], TOK['fg'], TOK['sub'], TOK['rule'], TOK['card'], TOK['accent']
W, out = 640, []

def text(x, y, s, size=11, color=None, weight=400, anchor='start'):
    out.append(f'<text x="{x}" y="{y}" fill="{color or INK}" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}">{s}</text>')

def lane(y, title, who, steps):
    """A rounded card: a title, who it is for, and four numbered steps joined by arrows."""
    out.append(f'<rect x="20" y="{y}" width="{W - 40}" height="124" rx="14" fill="{CARD}" stroke="{RULE}"/>')
    text(36, y + 26, title, 14, weight=700)
    text(W - 36, y + 26, who, 10, MUT, anchor='end')
    sw, gap = 128, 14
    for i, (a, b) in enumerate(steps):
        x = 36 + i * (sw + gap)
        out.append(f'<circle cx="{x + 10}" cy="{y + 52}" r="9" fill="{T if i == len(steps) - 1 else INK}"/>')
        text(x + 10, y + 56, str(i + 1), 10, PAPER, 700, 'middle')
        text(x, y + 80, a, 11, weight=600)
        for k, line in enumerate(b): text(x, y + 96 + k * 13, line, 10, MUT)
        if i < len(steps) - 1: out.append(f'<path d="M{x + 24} {y + 52}H{x + sw + gap - 4}" stroke="{RULE}" stroke-width="1.5" marker-end="url(#a)"/>')

text(W / 2, 34, 'How your Pi gets new apps', 20, weight=700, anchor='middle')
text(W / 2, 54, 'Three ways in. Every one ends with the app open on your Pi, walled off from everything else.', 11, MUT, anchor='middle')
lane(74, 'From the store', 'for anyone', [
    ('Type install dice', ['at the Terminal']),
    ('It asks the store', ['on Joshua\'s website']),
    ('It checks the seal', ['before it saves a', 'single byte']),
    ('Dice opens', ['in Spotlight too'])])
lane(212, 'From Samantha', 'for an admin', [
    ('Ask her', ['"make me a tip', 'calculator app"']),
    ('She writes it', ['in Plank, Joshua\'s', 'own language']),
    ('The Mac builds it', ['an error goes back', 'and she fixes it']),
    ('It opens on your Pi', ['no seal: hers, so', 'an admin says yes'])])
lane(350, 'From you', 'for anyone who codes', [
    ('Write a program', ['one file, in C', 'or in Plank']),
    ('Build it on a Mac', ['tools/mkapp.sh or', 'tools/plankapp.sh']),
    ('Put it on the card', ['in JT/APPS']),
    ('Open it', ['userrun, or find it', 'in Spotlight'])])

y = 494
text(36, y + 4, 'The seal', 14, weight=700)
text(36, y + 22, 'Every app can carry Joshua\'s seal, a signature only his key can make. The Pi reads it before it runs anything.', 10, MUT)
rows = [(T, 'Sealed', 'anyone can run it.'), (INK, 'No seal', 'only the person in charge (an admin) can run it.'),
        (MUT, 'Broken seal', 'never, not even the admin. Someone changed it after it was sealed.')]
for i, (c, a, b) in enumerate(rows):
    ry = y + 44 + i * 24
    out.append(f'<rect x="36" y="{ry - 10}" width="12" height="12" rx="3" fill="{c}"/>')
    text(56, ry, a, 11, weight=600)
    text(140, ry, b, 11, MUT)
text(W / 2, 636, 'Next: the Pi builds its own apps, with no Mac in the middle.', 11, T, 600, 'middle')
text(W / 2, 656, 'Drawn by tools/gen/humans.py. The full map is architecture.svg.', 9, MUT, anchor='middle')

H = 676
open(os.path.join(ROOT, 'docs', 'apps-for-humans.svg'), 'w').write(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
    f'style="background:{PAPER};font-family:Tree Sans,-apple-system,BlinkMacSystemFont,Helvetica,sans-serif">\n'
    f'<defs><marker id="a" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="6" markerHeight="6" orient="auto"><path d="M0 0L8 4L0 8z" fill="{RULE}"/></marker></defs>\n'
    f'<rect width="100%" height="100%" fill="{PAPER}"/>\n' + '\n'.join(out) + '\n</svg>\n')
