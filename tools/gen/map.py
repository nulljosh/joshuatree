#!/usr/bin/env python3
"""Draws architecture.svg: every Joshua Tree graph in one picture (where it runs, the stack, talking to Samantha, how an
app gets onto the Pi, the compiler building itself, the Pi bring-up, apps over time). The apps line is read from progress.svg, so
rerun this after tools/gen/progress.sh. The Pi row is typed in: update it when the board moves. Colours, spacing, radii
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

svg = open(os.path.join(ROOT, 'progress.svg')).read()
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
         ('Desktop and Samantha', 'windows, dock, menu bar; Samantha runs what you type or say', 'draws off-screen, then shows it'),
         ('Kernel', 'takes turns between apps, stops a frozen one, files, network', 'drives the parts'),
         ('Drivers', 'screen, keyboard, mouse, mic, Wi-Fi, Bluetooth, SD card, sound', ''),
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

# 8.0 (6.51.0 to 6.59.0): an app no longer has to be built into the kernel. Six steps, from whoever writes it to the Pi.
y = sec(y, 'box', 'How an app gets onto the Pi (8.0)')
y = chain(y, [('Write it', 'you, or Sam'), ('Build it', 'C or Plank'), ('Seal it', "Joshua's key"),
              ('The store', 'on the website'), ('install', 'seal checked'), ('It runs', 'walled off')],
          ['host', 'host', 'new', 'host', 'core', 'core'])
y = note(y + S[8], "Sealed apps run for anyone; unsealed ones (yours, or Samantha's) only for an admin; a broken seal never.") + S[32]

# 8.0 (6.60.0 to 6.71.0, then 6.72.0 on): plankc, the compiler written in Plank, rebuilds itself on the Pi.
y = sec(y, 'loop', 'It builds itself (8.0)')
y = chain(y, [('plankc', 'Plank, in Plank'), ('On the Pi', 'compiles itself'), ('A new plankc', 'no clang, no linker'),
              ('Same bytes', 'every time')], ['host', 'core', 'core', 'new'])
y = note(y + S[8], 'Checked on every pull request. The kernel is still C, built on a Mac.') + S[32]

# The Pi, on the real board. Solid = seen working on the board (docs/ROADMAP.md); dashed = proven in QEMU only.
y = sec(y, 'chip', 'The Pi, on the real board')
pi = [('Screen', '1080p desktop', 1), ('Keyboard', 'USB, hot-plug', 1), ('Wi-Fi', 'WPA2, joined', 1),
      ('Internet', 'DHCP and DNS', 1), ('Clock', 'from the net', 1), ('Terminal', 'zsh style, sam', 1),
      ('Samantha', 'chat on the Pi', 1), ('Pi actions', 'note, green light', 1), ('HTTPS', 'Browser, 37 roots', 1),
      ('Browser', 'window, links', 1), ('Books', 'plain text', 1), ('Music', 'wav, mp3, covers', 1),
      ('Notes', 'saved to the card', 1), ('Calendar', 'four views', 1), ('Bluetooth', 'mouse pairs', 1),
      ('Sound', 'headphone jack', 0), ('Many windows', 'apps side by side', 0),
      ('Walled memory', 'one app, one arena', 0), ('Watchdog', 'frozen app stopped', 0), ('Mic', 'Yeti, USB', 0),
      ('Wi-Fi switch', 'no restart', 0), ('Mail', 'IMAP, reply', 0), ('CalDAV', 'syncs when idle', 0),
      ('BT keyboard', 'LE, passkey', 0)]
pw = (IW - 3 * S[16]) / 4
for i, (a, b, done) in enumerate(pi):
    box(M + (i % 4) * (pw + S[16]), y + (i // 4) * (48 + S[8]), pw, 48, a, b, 'new' if done else 'todo')
y += ((len(pi) + 3) // 4) * (48 + S[8])
y = note(y, 'Solid: seen working on the real board (2026-10-12, v3). Dashed: built and checked in QEMU, not yet seen there.') + S[32]

y = sec(y, 'bars', 'Apps shipped')
pts = [tuple(map(int, q.split(','))) for q in re.search(r'<polyline points="([^"]+)"', svg).group(1).split()]
dates = re.findall(r'data-version="([\d-]+)"', svg)
x0, x1 = pts[0][0], pts[-1][0]
gy = lambda v: y + (v - 10) * 96 / 140          # progress.svg's plot runs from y 10 (its peak) to 150 (none)
PEAK = max(map(int, re.findall(r'data-apps="(\d+)"', svg)))
CX = M + S[32]                                   # the chart starts past its scale
for k, v in enumerate((PEAK, PEAK // 2, 0)):
    p.add(f'<path d="M{CX} {y + k * 48}H{W - M}" stroke="{RULE}"/>')
    p.text(CX - S[8], y + k * 48 + 4, str(v), 11, MUT, anchor='end')
p.add('<polyline points="' + ' '.join(f'{CX + (x - x0) * (W - M - CX) / (x1 - x0):.0f},{gy(v):.0f}' for x, v in pts)
      + f'" fill="none" stroke="{INK}" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>')
p.add(f'<circle cx="{W - M}" cy="{gy(pts[-1][1]):.0f}" r="4" fill="{PAPER}" stroke="{T}" stroke-width="2"/>')
p.text(W - M - S[8], gy(pts[-1][1]) + 40, f'{APPS} apps', 13, weight=600, anchor='end')
y += 96 + S[16]
p.text(CX, y + 4, dates[0], 11, MUT); p.text(W - M, y + 4, dates[-1], 11, MUT, anchor='end')
y += S[32]
p.text(M, y + 4, 'Drawn by tools/gen/map.py. The detail graphs live in docs/.', 11, MUT)
p.save('architecture.svg', y + 4 + M)
