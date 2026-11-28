#!/usr/bin/env python3
"""Draws docs/architecture.svg: Joshua Tree at a glance, three panels (where it runs, the layers, talking to Samantha). 13.21.3 cut
it from seven panels and 2092 px to these, so the README stays short. The app count is read from landing/progress.svg. Colours, spacing, radii
and type come from the brand kit's tokens (docs/brand/tokens.css) through tools/gen/svgkit.py, so the map cannot drift."""
import re, os
from svgkit import Pic, ROOT, M, S, R, PAPER, INK, MUT, RULE, CARD, T, G

W = 640
p = Pic(W)
IW = W - 2 * M                                    # inner width

def box(x, y, w, h, a, b, kind='core'):
    fill = {'core': f'fill="{G}" fill-opacity=".08" stroke="{G}" stroke-opacity=".5"', 'host': f'fill="{CARD}" stroke="{RULE}"',
            'new': f'fill="{T}" fill-opacity=".1" stroke="{T}"', 'todo': f'fill="{PAPER}" stroke="{MUT}" stroke-dasharray="3 3"'}[kind]
    p.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{R[8]}" {fill}/>')
    c = MUT if kind == 'todo' else INK
    p.text(x + w / 2, y + h / 2 - 2 if b else y + h / 2 + 4, a, 11, c, 600, 'middle')
    if b: p.text(x + w / 2, y + h / 2 + 12, b, 11, MUT, anchor='middle')

def sec(y, icon, t):
    """A section head: its pixel icon and its name. Returns where the section's body starts."""
    p.icon(M, y, icon)
    p.text(M + 24 + S[12], y + 17, t, 15, weight=700)
    return y + 24 + S[16]

def chain(y, steps, kinds, back=None):
    n = len(steps); gap = S[16]; sw = (IW - (n - 1) * gap) / n
    for i, ((a, b), k) in enumerate(zip(steps, kinds)):
        x = M + i * (sw + gap); box(x, y, sw, 48, a, b, k)
        if i < n - 1: p.add(f'<path d="M{x + sw + 2} {y + 24}h{gap - 4}" stroke="{T}" stroke-width="1.5" marker-end="url(#a)"/>')
    y += 48
    if back:
        p.add(f'<path d="M{W - M - sw / 2} {y}v{S[12]}H{M + sw / 2}v-{S[8]}" fill="none" stroke="{T}" stroke-width="1.2" stroke-dasharray="3 3" marker-end="url(#a)"/>')
        p.text(W / 2, y + S[12] + 16, back, 11, MUT, anchor='middle'); y += S[32]
    return y

def note(y, s): p.text(M, y + 12, s, 11, MUT); return y + 16

svg = open(os.path.join(ROOT, 'landing', 'progress.svg')).read()
APPS = re.findall(r'data-apps="(\d+)"', svg)[-1]

p.strip(0)
p.text(M, 56, 'Joshua Tree at a glance', 22, weight=700, face='Tree Sans Display')
p.text(M, 80, 'A computer that knows what you want before you ask, and keeps it on your machine.', 13, MUT)

y = sec(112, 'screen', 'Where it runs')
bw = (IW - 3 * S[16]) / 4
for i, (a, b, k) in enumerate([('Browser', 'the landing page', 'host'), ('QEMU', 'dev and CI', 'host'),
                               ('Raspberry Pi 4', 'the real board', 'new'), ('Real PC', 'from a USB stick', 'host')]):
    box(M + i * (bw + S[16]), y, bw, 48, a, b, k)
y += 48 + S[32]

y = sec(y, 'stack', 'The layers')
y = note(y, 'The same in all four places. Each layer only talks to the one under it.') + S[8]
stack = [('Apps', f'{APPS} apps, each walled off, so one crash stays one crash', 'asks by system call'),
         ('Desktop', 'windows, dock, menu bar, Spotlight, Settings, Samantha', 'draws off-screen, then shows it'),
         ('Kernel', 'four cores, 10 ms turns, threads, pipes, sockets, files, network', 'drives the parts'),
         ('Drivers', 'screen, keyboard, mouse, mic, Wi-Fi, Bluetooth, SD card, USB stick, sound', ''),
         ('The machine', 'a Raspberry Pi 4, a PC, or QEMU pretending to be one', '')]
for i, (a, b, hand) in enumerate(stack):
    p.add(f'<rect x="{M}" y="{y}" width="{IW}" height="40" rx="{R[8]}" ' + (f'fill="{CARD}" stroke="{RULE}"/>' if i == 4 else f'fill="{G}" fill-opacity=".08" stroke="{G}" stroke-opacity=".5"/>'))
    p.text(M + S[16], y + 25, a, 13, weight=600)
    p.text(M + 184, y + 25, b, 11, MUT)
    y += 40
    if hand:
        p.add(f'<path d="M{M + 40} {y + 1}v{S[12] - 2}" stroke="{T}" stroke-width="1.5" marker-end="url(#a)"/>')
        p.text(M + 52, y + 12, hand, 11, MUT)
    y += S[16]
y = note(y, 'Out on the internet: Turing (her brain), ElevenLabs (her voice), a Cloudflare Worker and Open-Meteo.') + S[32]

y = sec(y, 'talk', 'Talking to Samantha')
y = chain(y, [('You type or say', '"call Mom at 5"'), ('Turing', 'picks the tool'), ('Joshua Tree', 'does it'), ('She answers', 'voice and face')],
          ['host', 'host', 'core', 'core']) + S[32]

y -= S[16]
p.text(M, y + 4, 'Drawn by tools/gen/map.py. More in docs/ARCHITECTURE.md.', 11, MUT)
p.save('docs/architecture.svg', y + 4 + M)
