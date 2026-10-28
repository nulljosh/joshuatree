#!/usr/bin/env python3
"""Draws architecture.svg: every Joshua Tree graph in one picture (where it runs, the stack, talking to Samantha, the Pi
bring-up, how it gets built, memory, apps over time). The apps line is read from progress.svg, so rerun this after
tools/gen/progress.sh. The memory numbers come from docs/memory-map.svg (rerun tools/gen/memory-map.py first). The Pi row is typed in: update it when the board moves."""
import re, os
ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
# Colours come from the brand kit's tokens (docs/brand/tokens.css, the light set), so the map and the kit cannot drift.
TOK = dict(re.findall(r'--([a-z-]+):\s*(#[0-9a-f]{6})', open(os.path.join(ROOT, 'docs', 'brand', 'tokens.css')).read().split('@media')[0]))
PAPER, INK, MUT, RULE, CARD, T = TOK['bg'], TOK['fg'], TOK['sub'], TOK['rule'], TOK['card'], TOK['accent']
G = TOK['window-ink']
W = 640
out, lines = [], []

def box(x, y, w, h, a, b, kind='core'):
    fill = {'core': f'fill="{G}" fill-opacity=".12" stroke="{G}"', 'host': f'fill="{CARD}" stroke="{RULE}"',
            'new': f'fill="{T}" fill-opacity=".12" stroke="{T}"', 'svc': f'fill="{CARD}" stroke="{RULE}"',
            'todo': f'fill="{CARD}" stroke="{MUT}" stroke-dasharray="3 3"'}[kind]
    out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" {fill}/>')
    c, fs = (MUT, 9) if kind in ('svc', 'todo') else (INK, 11)
    out.append(f'<text x="{x + w / 2}" y="{y + h / 2 - 2}" fill="{c}" font-size="{fs}" font-weight="600" text-anchor="middle">{a}</text>')
    if b: out.append(f'<text x="{x + w / 2}" y="{y + h / 2 + 11}" fill="{c}" font-size="{fs - 1}" text-anchor="middle">{b}</text>')

def sec(y, t): out.append(f'<text x="20" y="{y}" fill="{MUT}" font-size="10" font-weight="600" letter-spacing="1">{t.upper()}</text>')

def chain(y, steps, kinds, back=None):
    n = len(steps); sw = 92 if n == 6 else (W - 40 - (n - 1) * 14) / n; gap = (W - 40 - n * sw) / (n - 1)
    for i, ((a, b), k) in enumerate(zip(steps, kinds)):
        x = 20 + i * (sw + gap); box(x, y, sw, 40, a, b, k)
        if i < n - 1: out.append(f'<path d="M{x + sw + 2} {y + 20}h{gap - 6}" stroke="{T}" stroke-width="1.5" marker-end="url(#a)"/>')
    if back: out.append(f'<path d="M{W - 20 - sw / 2} {y + 40}v10H{20 + sw / 2}v-6" fill="none" stroke="{T}" stroke-width="1.2" stroke-dasharray="3 3" marker-end="url(#a)"/>'
                        f'<text x="{W / 2}" y="{y + 62}" fill="{MUT}" font-size="9" text-anchor="middle">{back}</text>')

APPS = re.findall(r'data-apps="(\d+)"', open(os.path.join(ROOT, 'progress.svg')).read())[-1]
out.append(f'<text x="{W / 2}" y="30" fill="{INK}" font-size="18" font-weight="700" text-anchor="middle">Joshua Tree at a glance</text>')
out.append(f'<text x="{W / 2}" y="48" fill="{MUT}" font-size="11" text-anchor="middle">A computer that knows what you want before you ask, and keeps it on your machine.</text>')

sec(78, 'Where it runs')
xs, bw = [20, 175, 330, 485], 135
for x, (a, b, k) in zip(xs, [('Browser', 'the landing page', 'host'), ('QEMU', 'dev and CI', 'host'),
                             ('Raspberry Pi 4', 'the real board', 'new'), ('Real PC', 'from a USB stick', 'host')]):
    box(x, 86, bw, 42, a, b, k)
out.append(f'<text x="{W / 2}" y="146" fill="{MUT}" font-size="10" text-anchor="middle">The same layers in all four places. Each one only talks to the one under it:</text>')
# The stack, top to bottom, with what each layer does and how it asks the next one. Right column: the word for that hand-off.
stack = [('Apps', f'{APPS} apps, each in its own walled-off memory, so one crash stays one crash', 'core', 'asks by system call'),
         ('Desktop and Samantha', 'windows, dock and menu bar; Samantha runs what you type or say', 'core', 'draws off-screen, then shows it'),
         ('Kernel', 'takes turns between apps, stops one frozen for 3 s, files, network', 'core', 'drives the parts'),
         ('Drivers', 'screen, USB keyboard, mouse and mic, Wi-Fi, Bluetooth, SD card, sound', 'core', ''),
         ('The machine', 'a Raspberry Pi 4, a PC, or QEMU pretending to be one', 'host', '')]
for i, (a, b, k, hand) in enumerate(stack):
    y = 154 + i * 50
    box(20, y, 600, 38, a, b, k)
    if hand:
        out.append(f'<path d="M80 {y + 39}v9" stroke="{T}" stroke-width="1.5" marker-end="url(#a)"/>')
        out.append(f'<text x="90" y="{y + 47}" fill="{MUT}" font-size="9">{hand}</text>')
out.append(f'<text x="{W / 2}" y="420" fill="{MUT}" font-size="10" text-anchor="middle">Out on the internet: Turing (her brain), ElevenLabs (her voice), a Cloudflare Worker and Open-Meteo.</text>')

out.append('<g transform="translate(0,130)">')
sec(320, 'What happens when you talk to Samantha')
chain(330, [('You type or say', '"call Mom at 5"'), ('Turing', 'picks the tool'), ('Joshua Tree', 'does it'), ('She answers', 'voice and face')],
      ['host', 'host', 'core', 'core'])

# 8.0 (6.51.0 to 6.59.0): an app no longer has to be built into the kernel. Six steps, from whoever writes it to the Pi.
sec(400, 'How an app gets onto the Pi (8.0)')
chain(410, [('Write it', 'you or Samantha'), ('Build it', 'C or Plank'), ('Seal it', "Joshua's key"),
            ('The store', 'on the website'), ('install', 'seal checked first'), ('It runs', 'walled off')],
      ['host', 'host', 'new', 'host', 'core', 'core'])
out.append(f'<text x="{W / 2}" y="470" fill="{MUT}" font-size="9" text-anchor="middle">Sealed apps run for anyone; unsealed ones (yours, or Samantha\'s) only for an admin; a broken seal never.</text>')
out.append('<g transform="translate(0,100)">')

# The Pi, on the real board. done = seen working on the board (docs/ROADMAP.md, docs/ROADMAP.md); the rest is proven in QEMU only.
sec(400, 'The Pi, on the real board')
pi = [('Screen', '1080p desktop', 1), ('Keyboard', 'USB, hot-plug', 1), ('Wi-Fi', 'WPA2, joined', 1),
      ('Internet', 'DHCP and DNS', 1), ('Clock', 'from the net', 1), ('Terminal', 'zsh style, sam', 1),
      ('Samantha', 'chat on the Pi', 1), ('Pi actions', 'note, green light', 1), ('HTTPS', 'Browser, 37 roots', 1),
      ('Browser', 'window, links', 1), ('Books', 'plain text', 1), ('Music', 'wav, mp3, covers', 1),
      ('Notes', 'saved to the card', 1), ('Calendar', 'four views', 1), ('Bluetooth', 'mouse pairs', 1),
      ('Sound', 'headphone jack', 0), ('Many windows', 'apps side by side', 0),
      ('Walled memory', 'one app, one arena', 0), ('Watchdog', 'frozen app stopped', 0), ('Mic', 'Yeti, USB', 0)]
pw = (W - 40 - 3 * 14) / 4
for i, (a, b, done) in enumerate(pi):
    box(20 + (i % 4) * (pw + 14), 410 + (i // 4) * 54, pw, 40, a, b, 'new' if done else 'todo')
rows = (len(pi) + 3) // 4
out.append(f'<text x="20" y="{410 + rows * 54 + 8}" fill="{MUT}" font-size="9">Solid boxes are seen working on the real board (2026-10-12, v3); dashed are built and checked in QEMU, not yet seen there.</text>')
out.append(f'<g transform="translate(0,{410 + rows * 54 + 30 - 486})">')   # everything below moves down past the Pi grid

sec(496, 'How it gets built')
chain(506, [('Roadmap', 'the queue'), ('Build it', 'one agent, own branch'), ('Prove it', 'tests, real frames'), ('Ship it', 'merge on green')],
      ['host', 'core', 'core', 'new'], back='next item')

sec(594, 'Memory, PC build')
# Read from docs/memory-map.svg, which tools/gen/memory-map.py draws from the built kernel.elf: never typed in.
mm = open(os.path.join(ROOT, 'docs', 'memory-map.svg')).read()
kb = {n: int(k.replace(',', '')) for n, k in re.findall(r'>([a-z ,()]+): ([0-9,]+) KB', mm)}
mem = [('code', kb['code'], '#1c1c1e'), ('fonts, images, text', kb['fixed data (fonts, images, text)'], '#e3b7a4'),
       ('starting values', kb['starting values'], '#b5502c'), ('working memory', kb['working memory'], '#f0d9ce'),
       ('programs', kb['where programs load'], '#141413'), ('sound', kb['sound buffer'], '#d97757')]
tot = sum(k for _, k, _ in mem); x = 20.0
for n, k, c in mem:
    w = max(2.0, (W - 40) * k / tot); out.append(f'<rect x="{x:.1f}" y="602" width="{w:.1f}" height="16" fill="{c}"/>'); x += w
for i, (n, k, c) in enumerate(mem):   # three to a row, so the labels never run into each other
    lx, ly = 20 + (i % 3) * 200, 626 + (i // 3) * 14
    out.append(f'<rect x="{lx}" y="{ly}" width="8" height="8" fill="{c}"/><text x="{lx + 11}" y="{ly + 8}" fill="{MUT}" font-size="9">{n} {k:,} KB</text>')

out.append('<g transform="translate(0,28)">')
sec(664, 'Apps shipped')
svg = open(os.path.join(ROOT, 'progress.svg')).read()
pts = [tuple(map(int, p.split(','))) for p in re.search(r'<polyline points="([^"]+)"', svg).group(1).split()]
apps = re.findall(r'data-apps="(\d+)"', svg)[-1]
dates = re.findall(r'data-version="([\d-]+)"', svg)
x0, x1 = pts[0][0], pts[-1][0]
sp = ' '.join(f'{20 + (x - x0) * 600 / (x1 - x0):.0f},{674 + (y - 10) * 80 / 140:.0f}' for x, y in pts)
out.append(f'<path d="M20 674H620M20 714H620M20 754H620" stroke="{RULE}"/>')
out.append(f'<polyline points="{sp}" fill="none" stroke="{INK}" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>')
ey = 674 + (pts[-1][1] - 10) * 80 / 140
out.append(f'<circle cx="620" cy="{ey:.0f}" r="4" fill="{PAPER}" stroke="{T}" stroke-width="2"/>')
out.append(f'<text x="612" y="{ey + 22:.0f}" fill="{INK}" font-size="12" font-weight="600" text-anchor="end">{apps} apps</text>')
out.append(f'<text x="20" y="772" fill="{MUT}" font-size="10">{dates[0]}</text><text x="620" y="772" fill="{MUT}" font-size="10" text-anchor="end">{dates[-1]}</text>')
out.append(f'<text x="{W / 2}" y="798" fill="{MUT}" font-size="10" text-anchor="middle">Terracotta: the newest piece. Drawn by tools/gen/map.py; the detail graphs live in docs/.</text>')

out.append('</g>')
out.append('</g>')
out.append('</g>')
out.append('</g>')
H = 1062 + 130 + 100 + (rows * 54 + 30 - 220) - 40
open(os.path.join(ROOT, 'architecture.svg'), 'w').write(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
    f'style="background:{PAPER};font-family:Tree Sans,-apple-system,BlinkMacSystemFont,Helvetica,sans-serif">\n'
    f'<defs><marker id="a" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="6" markerHeight="6" orient="auto"><path d="M0 0L8 4L0 8z" fill="{T}"/></marker></defs>\n'
    f'<rect width="100%" height="100%" fill="{PAPER}"/>\n<path d="{"".join(lines)}" stroke="{RULE}" fill="none"/>\n'
    + '\n'.join(out) + '\n</svg>\n')
