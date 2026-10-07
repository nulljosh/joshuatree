#!/usr/bin/env python3
"""Draws architecture.svg: every Joshua Tree graph in one picture (where it runs, the stack, talking to Samantha, the Pi
bring-up, how it gets built, memory, apps over time). The apps line is read from progress.svg, so rerun this after
tools/gen/progress.sh. The memory numbers come from docs/memory-map.svg (rerun tools/gen/memory-map.py first). The Pi row is typed in: update it when the board moves."""
import re, os
ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
G, T, INK, MUT = '#2f6b3a', '#b5502c', '#141413', '#888'
W = 640
out, lines = [], []

def box(x, y, w, h, a, b, kind='core'):
    fill = {'core': f'fill="{G}" fill-opacity=".15" stroke="{G}"', 'host': 'fill="#f5f5f7" stroke="#d1d1d6"',
            'new': f'fill="{T}" fill-opacity=".12" stroke="{T}"', 'svc': 'fill="#f0f0f0" stroke="#ccc"',
            'todo': 'fill="#fff" stroke="#ccc" stroke-dasharray="3 3"'}[kind]
    out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" {fill}/>')
    c, fs = ('#666', 9) if kind in ('svc', 'todo') else ('#333', 11)
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
lines.append('M87 128v16M242 128v16M397 128v16M552 128v16M87 144H552M210 144v14M430 144v14')
box(110, 158, 200, 42, 'Desktop', f'windows, dock, {APPS} apps')
box(330, 158, 200, 42, 'Samantha', 'you type, she runs it')
lines.append('M210 200v14M430 200v14M210 214H430M320 214v14')
box(110, 228, 420, 42, 'Kernel', 'tasks, drivers, fonts, network, disk, on i386 and ARM64')
out.append(f'<text x="{W / 2}" y="290" fill="{MUT}" font-size="10" text-anchor="middle">Calls out to a Cloudflare Worker, Turing (her brain), ElevenLabs (her voice) and Open-Meteo.</text>')

sec(320, 'What happens when you talk to Samantha')
chain(330, [('You type', '"call Mom at 5"'), ('Turing', 'picks the tool'), ('Joshua Tree', 'does it'), ('She answers', 'voice and face')],
      ['host', 'host', 'core', 'core'])

# The Pi, on the real board. done = seen working in a photo of the screen.
sec(400, 'The Pi, on the real board')
pi = [('Screen', '1080p desktop', 1), ('Keyboard', 'USB, hot-plug', 1), ('Wi-Fi scan', '16 networks', 1),
      ('Wi-Fi join', 'WPA2, Shaw', 1), ('Clock', 'from the net', 0), ('Sound', 'headphone jack', 0),
      ('Mouse', 'Bluetooth', 0), ('Mic', 'Yeti, USB', 0)]
pw = (W - 40 - 7 * 8) / 8
for i, (a, b, done) in enumerate(pi):
    box(20 + i * (pw + 8), 410, pw, 40, a, b, 'new' if done else 'todo')
out.append(f'<text x="20" y="468" fill="{MUT}" font-size="9">Wi-Fi: 9 of 10 steps on the real board (joined Shaw with WPA2, 2026-10-07). Solid boxes are seen working on the screen; dashed are next, in order.</text>')

sec(496, 'How it gets built')
chain(506, [('Roadmap', 'the queue'), ('Build it', 'one agent, own branch'), ('Prove it', 'tests, real frames'), ('Ship it', 'merge on green')],
      ['host', 'core', 'core', 'new'], back='next item')

sec(594, 'Memory, PC build')
# Read from docs/memory-map.svg, which tools/gen/memory-map.py draws from the built kernel.elf: never typed in.
mm = open(os.path.join(ROOT, 'docs', 'memory-map.svg')).read()
kb = {n: int(k.replace(',', '')) for n, k in re.findall(r'>([a-z ,()]+): ([0-9,]+) KB', mm)}
mem = [('code', kb['code'], '#2f6b3a'), ('fonts, images, text', kb['fixed data (fonts, images, text)'], '#7fa886'),
       ('starting values', kb['starting values'], '#b5502c'), ('working memory', kb['working memory'], '#c9d9cb'),
       ('programs', kb['where programs load'], '#141413'), ('sound', kb['sound buffer'], '#d97757')]
tot = sum(k for _, k, _ in mem); x = 20.0
for n, k, c in mem:
    w = max(2.0, (W - 40) * k / tot); out.append(f'<rect x="{x:.1f}" y="602" width="{w:.1f}" height="16" fill="{c}"/>'); x += w
for i, (n, k, c) in enumerate(mem):   # three to a row, so the labels never run into each other
    lx, ly = 20 + (i % 3) * 200, 626 + (i // 3) * 14
    out.append(f'<rect x="{lx}" y="{ly}" width="8" height="8" fill="{c}"/><text x="{lx + 11}" y="{ly + 8}" fill="#555" font-size="9">{n} {k:,} KB</text>')

sec(664, 'Apps shipped')
svg = open(os.path.join(ROOT, 'progress.svg')).read()
pts = [tuple(map(int, p.split(','))) for p in re.search(r'<polyline points="([^"]+)"', svg).group(1).split()]
apps = re.findall(r'data-apps="(\d+)"', svg)[-1]
dates = re.findall(r'data-version="([\d-]+)"', svg)
x0, x1 = pts[0][0], pts[-1][0]
sp = ' '.join(f'{20 + (x - x0) * 600 / (x1 - x0):.0f},{674 + (y - 10) * 80 / 140:.0f}' for x, y in pts)
out.append('<path d="M20 674H620M20 714H620M20 754H620" stroke="#ece8df"/>')
out.append(f'<polyline points="{sp}" fill="none" stroke="{INK}" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>')
ey = 674 + (pts[-1][1] - 10) * 80 / 140
out.append(f'<circle cx="620" cy="{ey:.0f}" r="4" fill="#fff" stroke="{T}" stroke-width="2"/>')
out.append(f'<text x="612" y="{ey + 22:.0f}" fill="{INK}" font-size="12" font-weight="600" text-anchor="end">{apps} apps</text>')
out.append(f'<text x="20" y="772" fill="{MUT}" font-size="10">{dates[0]}</text><text x="620" y="772" fill="{MUT}" font-size="10" text-anchor="end">{dates[-1]}</text>')
out.append(f'<text x="{W / 2}" y="798" fill="{MUT}" font-size="10" text-anchor="middle">Terracotta: the newest piece. Drawn by tools/gen/map.py; the detail graphs live in docs/.</text>')

open(os.path.join(ROOT, 'architecture.svg'), 'w').write(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="814" viewBox="0 0 {W} 814" '
    'style="background:#fff;font-family:-apple-system,BlinkMacSystemFont,Helvetica,sans-serif">\n'
    f'<defs><marker id="a" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="6" markerHeight="6" orient="auto"><path d="M0 0L8 4L0 8z" fill="{T}"/></marker></defs>\n'
    f'<rect width="100%" height="100%" fill="#fff"/>\n<path d="{"".join(lines)}" stroke="#d1d1d6" fill="none"/>\n'
    + '\n'.join(out) + '\n</svg>\n')
