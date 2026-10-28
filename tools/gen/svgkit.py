"""The house kit for the drawn pictures (architecture.svg, docs/apps-for-humans.svg): the brand's colours, an 8 point grid,
one radius set, Tree Sans embedded (only the letters a picture uses), and pixel icons on terracotta tiles with dither and
scan lines. Colours, spacing and radii come from docs/brand/tokens.css, so a picture cannot drift from the kit."""
import base64, os, re, subprocess, tempfile
ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
_css = open(os.path.join(ROOT, 'docs', 'brand', 'tokens.css')).read().split('@media')[0]
TOK = dict(re.findall(r'--([a-z-]+):\s*(#[0-9a-f]{6})', _css))
PAPER, INK, MUT, RULE, CARD, T = TOK['bg'], TOK['fg'], TOK['sub'], TOK['rule'], TOK['card'], TOK['accent']
G = TOK['window-ink']
S = {int(k): int(v) for k, v in re.findall(r'--s(\d+):\s*(\d+)px', _css)}    # spacing: 4, 8, 12, 16, 24, 32, 48 ...
R = {int(k): int(v) for k, v in re.findall(r'--r(\d+):\s*(\d+)px', _css)}    # radii: 18 cards, 12 chips, 8, 4
M = S[24]                                                                   # page margin
FACES = [('Tree Sans', 400, 'TreeSans-Regular'), ('Tree Sans', 600, 'TreeSans-SemiBold'),
         ('Tree Sans', 700, 'TreeSans-Bold'), ('Tree Sans Display', 700, 'TreeSansDisplay-Bold')]

# 8 by 8 glyphs, drawn 2 px a dot in white on a 24 px tile.
ICONS = {
    'bag':    ['..####..', '.#....#.', '########', '#......#', '#.#..#.#', '#......#', '#......#', '########'],
    'talk':   ['.######.', '########', '##.##.##', '########', '.######.', '..##....', '.##.....', '........'],
    'prompt': ['........', '.#......', '..#.....', '...#....', '..#.....', '.#..###.', '........', '........'],
    'seal':   ['..####..', '.######.', '######.#', '#.###.##', '##.#.###', '###.####', '.######.', '..####..'],
    'ring':   ['..####..', '.#....#.', '#......#', '#......#', '#......#', '#......#', '.#....#.', '..####..'],
    'broken': ['..####..', '.#....#.', '#.#..#.#', '#..##..#', '#..##..#', '#.#..#.#', '.#....#.', '..####..'],
    'screen': ['########', '#......#', '#......#', '#......#', '########', '...##...', '..####..', '........'],
    'stack':  ['.######.', '........', '########', '........', '.######.', '........', '########', '........'],
    'chip':   ['.#.##.#.', '########', '##....##', '.#....#.', '.#....#.', '##....##', '########', '.#.##.#.'],
    'box':    ['...##...', '...##...', '.######.', '..####..', '...##...', '#......#', '#......#', '########'],
    'loop':   ['..####..', '.#....##', '#....###', '#......#', '#......#', '###....#', '##....#.', '..####..'],
    'bars':   ['......##', '......##', '...##.##', '...##.##', '##.##.##', '##.##.##', '##.##.##', '########'],
}

class Pic:
    def __init__(self, w):
        self.w, self.out, self.chars = w, [], set()

    def add(self, s): self.out.append(s)

    def text(self, x, y, s, size=13, color=None, weight=400, anchor='start', face='Tree Sans'):
        self.chars |= set(s)
        s = s.replace('&', '&amp;').replace('<', '&lt;')
        fam = '' if face == 'Tree Sans' else f' font-family="{face}"'
        self.add(f'<text x="{x}" y="{y}" fill="{color or INK}" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}"{fam}>{s}</text>')

    def card(self, x, y, w, h, r=R[18], fill=CARD, stroke=RULE, extra=''):
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}" stroke="{stroke}"{extra}/>')

    def icon(self, x, y, name, color=T):
        """A 24 px tile: the colour, a dither, scan lines, then the glyph in paper."""
        self.add(f'<g transform="translate({x},{y})"><rect width="24" height="24" rx="{R[8] - 2}" fill="{color}"/>'
                 f'<rect width="24" height="24" rx="{R[8] - 2}" fill="url(#dither)"/><rect width="24" height="24" rx="{R[8] - 2}" fill="url(#scan)"/>'
                 + ''.join(f'<rect x="{4 + 2 * c}" y="{4 + 2 * r}" width="2" height="2"/>' for r, row in enumerate(ICONS[name])
                           for c, ch in enumerate(row) if ch == '#').replace('<rect x="', f'<rect fill="{PAPER}" x="') + '</g>')

    def strip(self, y, h=S[8]):
        """A dithered terracotta band, the pictures' header rule."""
        self.add(f'<rect x="0" y="{y}" width="{self.w}" height="{h}" fill="{T}"/><rect x="0" y="{y}" width="{self.w}" height="{h}" fill="url(#dither)"/>')

    def fonts(self):
        """@font-face rules with each face cut down to this picture's letters, as base64 woff2. Needs uv (fonttools, brotli)."""
        text = ''.join(sorted(self.chars | set('0123456789')))
        rules = []
        with tempfile.TemporaryDirectory() as d:
            for fam, wt, f in FACES:
                o = os.path.join(d, f + '.woff2')
                subprocess.run(['uvx', '-q', '--with', 'brotli', '--from', 'fonttools', 'pyftsubset',
                                os.path.join(ROOT, 'landing', 'fonts', f + '.woff2'), '--text=' + text,
                                '--flavor=woff2', '--output-file=' + o], check=True)
                b = base64.b64encode(open(o, 'rb').read()).decode()
                rules.append(f"@font-face{{font-family:'{fam}';font-weight:{wt};src:url(data:font/woff2;base64,{b}) format('woff2')}}")
        return ''.join(rules)

    def save(self, path, h):
        open(os.path.join(ROOT, path), 'w').write(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{h}" viewBox="0 0 {self.w} {h}" '
            f'style="background:{PAPER};font-family:\'Tree Sans\',-apple-system,BlinkMacSystemFont,Helvetica,sans-serif">\n'
            f'<defs><style>{self.fonts()}</style>'
            f'<pattern id="dither" width="2" height="2" patternUnits="userSpaceOnUse"><rect width="1" height="1" fill="#fff" fill-opacity=".22"/>'
            f'<rect x="1" y="1" width="1" height="1" fill="#fff" fill-opacity=".22"/></pattern>'
            f'<pattern id="scan" width="3" height="3" patternUnits="userSpaceOnUse"><rect width="3" height="1" fill="#000" fill-opacity=".14"/></pattern>'
            f'<marker id="a" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="6" markerHeight="6" orient="auto"><path d="M0 0L8 4L0 8z" fill="{T}"/></marker></defs>\n'
            f'<rect width="100%" height="100%" fill="{PAPER}"/>\n' + '\n'.join(self.out) + '\n</svg>\n')
