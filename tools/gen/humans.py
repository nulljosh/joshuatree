#!/usr/bin/env python3
"""Draws docs/apps-for-humans.svg: how a new app reaches your Pi, in plain words, for someone who has never read the code
(the store, Samantha, or you), and what the seal means. Built on tools/gen/svgkit.py: the kit's colours, an 8 point
grid, Tree Sans, pixel icons."""
from svgkit import Pic, M, S, R, INK, MUT, RULE, T, PAPER, G

W = 640
p = Pic(W)
p.strip(0)
p.text(M, 56, 'How your Pi gets new apps', 22, weight=700, face='Tree Sans Display')
p.text(M, 80, 'Three ways in. Each one ends with the app open on your Pi, walled off from everything else.', 13, MUT)

def lane(y, icon, title, who, steps):
    """A card: an icon, a title, who it is for, and four numbered steps joined by a rule."""
    h = 136
    p.card(M, y, W - 2 * M, h)
    p.icon(M + S[16], y + S[16], icon)
    p.text(M + S[16] + 24 + S[12], y + S[16] + 17, title, 15, weight=700)
    p.text(W - M - S[16], y + S[16] + 17, who, 11, MUT, anchor='end')
    inner = W - 2 * M - 2 * S[16]
    sw = (inner - 3 * S[16]) / 4
    for i, (a, b) in enumerate(steps):
        x = M + S[16] + i * (sw + S[16])
        last = i == len(steps) - 1
        p.add(f'<circle cx="{x + 8}" cy="{y + 64}" r="8" fill="{T if last else G}"/>')
        p.text(x + 8, y + 68, str(i + 1), 11, PAPER, 700, 'middle')
        if not last: p.add(f'<path d="M{x + 24} {y + 64}H{x + sw + S[16] - 8}" stroke="{RULE}" stroke-width="1.5"/>')
        p.text(x, y + 96, a, 13, weight=600)
        for k, line in enumerate(b): p.text(x, y + 112 + k * 14, line, 11, MUT)
    return y + h + S[16]

y = lane(104, 'bag', 'From the store', 'for anyone', [
    ('Type install dice', ['at the Terminal']),
    ('It asks the store', ["on Joshua's website"]),
    ('It checks the seal', ['before it saves a', 'single byte']),
    ('Dice opens', ['in Spotlight too'])])
y = lane(y, 'talk', 'From Samantha', 'for an admin', [
    ('Ask her', ['"make me a tip', 'calculator app"']),
    ('She writes it', ["in Plank, Joshua's", 'own language']),
    ('The Mac builds it', ['an error goes back', 'and she fixes it']),
    ('It opens on your Pi', ['no seal: hers, so', 'an admin says yes'])])
y = lane(y, 'prompt', 'From you', 'for anyone who codes', [
    ('Write a program', ['one file, in C', 'or in Plank']),
    ('Build it on a Mac', ['tools/mkapp.sh or', 'tools/plankapp.sh']),
    ('Put it on the card', ['in JT/APPS']),
    ('Open it', ['userrun, or find it', 'in Spotlight'])])

y += S[8]
p.text(M, y + 16, 'The seal', 15, weight=700)
p.text(M, y + 40, "Every app can carry Joshua's seal, a signature only his key can make. The Pi reads it first.", 13, MUT)
for i, (ic, c, a, b) in enumerate([('seal', T, 'Sealed', 'anyone can run it.'),
                                   ('ring', G, 'No seal', 'only the person in charge (an admin) can run it.'),
                                   ('broken', MUT, 'Broken seal', 'never. Someone changed it after it was sealed.')]):
    ry = y + 56 + i * S[32]
    p.icon(M, ry, ic, c)
    p.text(M + 24 + S[12], ry + 17, a, 13, weight=600)
    p.text(M + 24 + S[12] + 88, ry + 17, b, 13, MUT)
y += 56 + 3 * S[32] + S[16]
p.text(M, y + 13, 'Next: the Pi builds its own apps, with no Mac in the middle.', 13, T, 600)
p.text(M, y + 37, 'Drawn by tools/gen/humans.py. The full map is architecture.svg.', 11, MUT)
p.save('docs/apps-for-humans.svg', y + 37 + M)
