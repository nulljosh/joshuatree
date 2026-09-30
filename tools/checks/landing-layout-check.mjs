// Landing grade fixes (LOOP-HANDOFF "Landing fixes from the grade"), held in
// place. At a phone width (390, 360) and a desktop width (1280) it loads
// landing/index.html with no scrolling and asserts:
//   1. The "Fast on nothing" benchmark values and labels never overlap each
//      other, and no value spills out of its own cell.
//   2. Every app count the page states (the facts row, the closing line,
//      the progress lede, the progress chart's caption and aria-label) is
//      the same number, and it equals the real apps in kernel/kernel.c's
//      APPS[] table (everything except Apps, Trash and Mail Compose).
//   3. The "Boot straight into Samantha" link has a real theme color, not
//      the browser's default link blue or the body text color.
//   4. The key sections are visible (opacity > 0) on load. They used to sit
//      at opacity 0 until a scroll observer fired.
//   5. The "Want one?" section carries the ad video and the Strata render.
// Headless Chromium only, local static server.
import { chromium } from 'playwright';
import { createServer } from 'http';
import { readFile } from 'fs/promises';
import { extname, join } from 'path';

const ROOT = new URL('../../landing/', import.meta.url).pathname;
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.svg': 'image/svg+xml',
                '.wasm': 'application/wasm', '.png': 'image/png', '.jpg': 'image/jpeg',
                '.webp': 'image/webp', '.txt': 'text/plain' };
const server = createServer(async (req, res) => {
  const path = join(ROOT, decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  try {
    const body = await readFile(path);
    res.writeHead(200, { 'content-type': TYPES[extname(path)] || 'application/octet-stream' });
    res.end(body);
  } catch { res.writeHead(404).end('nope'); }
});
await new Promise(r => server.listen(0, r));
const url = `http://127.0.0.1:${server.address().port}/index.html`;

const fail = (msg) => { console.log('FAIL: ' + msg); process.exitCode = 1; };

// The real app count, straight from the kernel's own table.
const kernel = await readFile(new URL('../../kernel/kernel.c', import.meta.url), 'utf8');
const table = kernel.match(/struct app APPS\[GUI_APP_COUNT\]\s*=\s*\{([\s\S]*?)\n\};/);
if (!table) { console.log('FAIL: could not find the APPS table in kernel/kernel.c'); process.exit(1); }
const names = [...table[1].matchAll(/\{"([^"]*)",/g)].map(m => m[1]);
const realApps = names.filter(n => !['Apps', 'Trash', 'Compose'].includes(n)).length;
console.log(`APPS[] has ${realApps} real apps`);

const overlap = (a, b) => a.x < b.x + b.w - 0.5 && b.x < a.x + a.w - 0.5 && a.y < b.y + b.h - 0.5 && b.y < a.y + a.h - 0.5;
const KEY = ['Joshua Tree by the numbers', 'How fast', 'Meet Samantha', 'Apps built in', 'And so much more', 'Dev kit waitlist'];

const browser = await chromium.launch();
try {
  for (const w of [390, 360, 1280]) {
    const page = await browser.newPage({ viewport: { width: w, height: 844 } });
    await page.goto(url, { waitUntil: 'load' });
    await page.waitForTimeout(700);
    const d = await page.evaluate((KEY) => {
      const box = (e) => { const r = e.getBoundingClientRect(); return { x: r.x, y: r.y + scrollY, w: r.width, h: r.height }; };
      const facts = [...document.querySelectorAll('[aria-label="How fast"] .fact')].map(f => ({
        cell: box(f),
        parts: [...f.querySelectorAll('strong, strong > .label, :scope > .label')].map(e => ({ t: e.textContent.trim(), b: box(e) })),
        spill: f.scrollWidth > f.clientWidth + 1,
      }));
      const link = document.getElementById('samantha-boot-link');
      const ls = link && getComputedStyle(link);
      const body = getComputedStyle(document.body).color;
      const text = document.body.innerText + ' ' + document.documentElement.innerHTML;
      const counts = [];
      document.querySelectorAll('[data-fact="apps"]').forEach(e => counts.push(['fact span', e.textContent.trim()]));
      const lede = document.querySelector('.progress-section .sub');
      const lm = lede && lede.textContent.match(/(\d+) apps today/);
      if (lm) counts.push(['progress lede', lm[1]]);
      const chart = document.getElementById('progress-chart-live');
      if (chart) {
        const am = (chart.getAttribute('aria-label') || '').match(/(\d+) apps/);
        if (am) counts.push(['chart aria-label', am[1]]);
        const cap = [...chart.querySelectorAll('text')].map(t => t.textContent.match(/^(\d+) apps\s*\u00b7\s*\d+% documented/)).find(Boolean);
        if (cap) counts.push(['chart caption', cap[1]]);
      }
      const sections = KEY.map(k => {
        const s = document.querySelector(`section[aria-label="${k}"]`);
        return [k, s ? +getComputedStyle(s).opacity : null];
      });
      const want = document.querySelector('section[aria-label="Dev kit waitlist"]');
      return {
        facts, counts, sections,
        linkColor: ls && ls.color, bodyColor: body,
        ad: !!(want && want.querySelector('video source[src*="joshua-tree-ad-v6.mp4"]')),
        strata: !!(want && want.querySelector('img[src="strata-hero.jpg"]')),
        scrolled: scrollY,
      };
    }, KEY);
    const tag = `${w}px`;
    console.log(`${tag}: ${d.facts.length} bench facts, counts ${JSON.stringify(d.counts)}, link ${d.linkColor}`);

    if (d.scrolled !== 0) fail(`${tag}: page scrolled on load`);
    // 1. benchmark boxes
    if (d.facts.length < 5) fail(`${tag}: expected 5 benchmark facts, found ${d.facts.length}`);
    const all = d.facts.flatMap((f, i) => f.parts.map(p => ({ i, ...p })));
    for (let a = 0; a < all.length; a++) for (let b = a + 1; b < all.length; b++) {
      // A value and its own unit label nest by design; only compare across facts,
      // or a fact's value against its own caption.
      const same = all[a].i === all[b].i;
      const nested = all[b].b.x >= all[a].b.x - 1 && all[b].b.x + all[b].b.w <= all[a].b.x + all[a].b.w + 1 &&
                     all[b].b.y >= all[a].b.y - 1 && all[b].b.y + all[b].b.h <= all[a].b.y + all[a].b.h + 1;
      if (same && nested) continue;
      if (overlap(all[a].b, all[b].b)) fail(`${tag}: benchmark "${all[a].t}" overlaps "${all[b].t}"`);
    }
    d.facts.forEach((f, i) => {
      if (f.spill) fail(`${tag}: benchmark fact ${i} spills out of its cell`);
    });
    // 2. app counts
    if (d.counts.length < 4) fail(`${tag}: expected 4+ app-count mentions, found ${d.counts.length}`);
    for (const [where, n] of d.counts) if (+n !== realApps) fail(`${tag}: ${where} says ${n} apps, APPS[] has ${realApps}`);
    // 3. link styling
    const rgb = d.linkColor;
    if (!rgb || rgb === 'rgb(0, 0, 238)' || rgb === 'rgb(0, 102, 204)' || rgb === d.bodyColor) fail(`${tag}: Samantha link is unstyled (color ${rgb}, body ${d.bodyColor})`);
    // 4. visibility without scrolling
    for (const [k, o] of d.sections) {
      if (o === null) fail(`${tag}: section "${k}" missing`);
      else if (!(o > 0)) fail(`${tag}: section "${k}" is invisible on load (opacity ${o})`);
    }
    // 5. Want one
    if (!d.ad) fail(`${tag}: "Want one?" has no joshua-tree-ad-v6.mp4 video`);
    if (!d.strata) fail(`${tag}: "Want one?" has no Strata render`);
    await page.close();
  }
  if (!process.exitCode) console.log('PASS: benchmark labels clear, one app count, Samantha link styled, sections visible on load');
} finally {
  await browser.close();
  server.close();
}
