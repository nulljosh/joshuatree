// Landing "Tech specs" accordion and footer directory, held in place.
// The developer specs (benchmarks, growth chart, documented %, architecture,
// build) live in one <section id="specs"> of native <details>, collapsed by
// default, so the page above the fold reads in plain words. The footer is a
// four-column directory like apple.com. At 390, 768 and 1280 wide, in light
// and dark, this asserts:
//   1. #specs exists with 5+ <details>, all collapsed on load. Nothing
//      developer-flavoured (ns/op, MB/s, tools/bench.sh, the chart, the
//      Documented %) sits outside #specs, and all of it is hidden while
//      collapsed and visible once opened.
//   2. Every benchmark value and label, the growth chart with its lede, the
//      documented % span and the ring-3 count are still in the DOM.
//   3. The plain lines and the footer link to #specs, and every in-page
//      anchor on the page resolves to a real id.
//   4. The footer has four columns of links (2x2 on phones, 4 across on
//      wider screens), every docs/ or LICENSE link points at a file in this
//      repo, and every relative link points at a file in landing/.
//   5. No horizontal overflow, closed or with every accordion open.
//   6. Footer text keeps 4.5:1 contrast against its real background.
//   7. Summaries are 44px tall, show a focus ring, and toggle with Enter
//      and Space from the keyboard.
// LANDING_DIR points the check at another copy of landing/ (to prove it
// fails on the old page); CHROMIUM_PATH at an installed Chromium.
import { chromium } from 'playwright';
import { createServer } from 'http';
import { readFile, stat } from 'fs/promises';
import { extname, join } from 'path';

const ROOT = process.env.LANDING_DIR || new URL('../../landing/', import.meta.url).pathname;
const REPO = new URL('../../', import.meta.url).pathname;
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

let failed = 0;
const fail = (msg) => { console.log('FAIL: ' + msg); failed++; process.exitCode = 1; };
const exists = async (p) => { try { await stat(p); return true; } catch { return false; } };

const BENCH = [['Boot to shell', 'ms'], ['Alloc + free', 'ns/op'], ['memcpy', 'MB/s'], ['Context switch', 'ns/switch'], ['Disk read', 'KB/s']];
const DEV_WORDS = ['ns/op', 'MB/s', 'ns/switch', 'KB/s', 'tools/bench.sh'];
const REPO_URL = 'https://github.com/nulljosh/joshuatree/';

const launch = process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {};
const browser = await chromium.launch(launch);
try {
  for (const scheme of ['light', 'dark']) for (const w of [390, 768, 1280]) {
    const tag = `${w}px ${scheme}`;
    const ctx = await browser.newContext({ viewport: { width: w, height: 900 }, colorScheme: scheme });
    const page = await ctx.newPage();
    await page.goto(url, { waitUntil: 'load' });
    await page.waitForTimeout(500);

    // 1. structure and collapsed-by-default
    const s = await page.evaluate(({ DEV_WORDS }) => {
      const specs = document.getElementById('specs');
      const ds = specs ? [...specs.querySelectorAll(':scope details')] : [];
      const outside = (() => {
        const clone = document.body.cloneNode(true);
        clone.querySelectorAll('#specs, script, style, #demo-frame').forEach(e => e.remove());
        return clone.textContent;
      })();
      const visible = (e) => { if (!e) return false; const r = e.getBoundingClientRect(); return r.width > 0 && r.height > 0 && e.checkVisibility({ visibilityProperty: true }); };
      return {
        has: !!specs,
        count: ds.length,
        open: ds.filter(d => d.open).length,
        devOutside: DEV_WORDS.filter(x => outside.includes(x)),
        chartOutside: !!(document.getElementById('progress-chart-live') && !(specs && specs.contains(document.getElementById('progress-chart-live')))),
        docOutside: [...document.querySelectorAll('[data-fact="documented"]')].some(e => !specs || !specs.contains(e)),
        chartHiddenClosed: !visible(document.getElementById('progress-chart-live')),
      };
    }, { DEV_WORDS });
    if (!s.has) { fail(`${tag}: no <section id="specs">`); await ctx.close(); continue; }
    if (s.count < 5) fail(`${tag}: #specs has ${s.count} <details>, expected 5+`);
    if (s.open !== 0) fail(`${tag}: ${s.open} <details> open on load, expected all collapsed`);
    if (s.devOutside.length) fail(`${tag}: developer specs outside #specs: ${s.devOutside.join(', ')}`);
    if (s.chartOutside) fail(`${tag}: the growth chart sits outside #specs`);
    if (s.docOutside) fail(`${tag}: the Documented % sits outside #specs`);
    if (!s.chartHiddenClosed) fail(`${tag}: the growth chart is visible while its accordion is collapsed`);

    // 5a. overflow, closed
    const ov = () => page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
    if (await ov() > 0) fail(`${tag}: horizontal overflow with specs closed (${await ov()}px)`);

    // 2. content still in the DOM, hidden closed, visible open
    const before = await page.evaluate((BENCH) => BENCH.map(([label, unit]) => {
      const fact = [...document.querySelectorAll('#specs .fact')].find(f => f.querySelector(':scope > .label')?.textContent.trim() === label);
      const v = fact && fact.querySelector('strong');
      const r = fact && fact.getBoundingClientRect(); const cv = !!fact && fact.checkVisibility();
      return { label, unit, found: !!fact, num: v ? /^\d+/.test(v.textContent.trim()) : false, unitOk: v ? v.textContent.includes(unit) : false, shown: !!r && r.width > 0 && r.height > 0 && cv };
    }), BENCH);
    for (const b of before) {
      if (!b.found) fail(`${tag}: benchmark "${b.label}" missing from #specs`);
      else {
        if (!b.num) fail(`${tag}: benchmark "${b.label}" has no number`);
        if (!b.unitOk) fail(`${tag}: benchmark "${b.label}" lost its unit ${b.unit}`);
        if (b.shown) fail(`${tag}: benchmark "${b.label}" is visible while collapsed`);
      }
    }
    // keyboard: focus the first summary, Enter opens, Space closes
    const first = page.locator('#specs details > summary').first();
    await first.focus();
    await page.keyboard.press('Enter');
    let st = await page.evaluate(() => document.querySelector('#specs details').open);
    if (!st) fail(`${tag}: Enter on a summary did not open it`);
    await page.keyboard.press('Space');
    st = await page.evaluate(() => document.querySelector('#specs details').open);
    if (st) fail(`${tag}: Space on a summary did not close it`);
    // 7. summary size and focus ring (focused by keyboard above)
    const sm = await page.evaluate(() => [...document.querySelectorAll('#specs details > summary')].map(e => {
      const r = e.getBoundingClientRect(); return { h: r.height, t: e.textContent.trim() }; }));
    for (const m of sm) if (m.h < 44) fail(`${tag}: summary "${m.t}" is ${m.h}px tall, under 44px`);
    await first.focus(); await page.keyboard.press('Tab'); await page.keyboard.press('Shift+Tab');
    const ring = await page.evaluate(() => { const cs = getComputedStyle(document.activeElement); return { tag: document.activeElement.tagName, w: parseFloat(cs.outlineWidth), st: cs.outlineStyle }; });
    if (ring.tag !== 'SUMMARY' || ring.st === 'none' || !(ring.w >= 2)) fail(`${tag}: focused summary shows no focus ring (${JSON.stringify(ring)})`);

    // open everything
    await page.evaluate(() => document.querySelectorAll('#specs details').forEach(d => { d.open = true; }));
    await page.waitForTimeout(150);
    const after = await page.evaluate((BENCH) => {
      const vis = (e) => { if (!e) return false; const r = e.getBoundingClientRect(); return r.width > 0 && r.height > 0 && e.checkVisibility(); };
      const chart = document.getElementById('progress-chart-live');
      const lede = document.querySelector('#specs .progress-section .sub');
      const doc = document.querySelector('#specs [data-fact="documented"]');
      const ring3 = document.querySelector('#specs [data-fact="ring3"]');
      return {
        bench: BENCH.map(([label]) => { const f = [...document.querySelectorAll('#specs .fact')].find(f => f.querySelector(':scope > .label')?.textContent.trim() === label); return [label, vis(f)]; }),
        chart: vis(chart), chartDots: chart ? chart.querySelectorAll('.progress-pt').length : 0,
        lede: vis(lede) && /apps today/.test(lede.textContent), doc: vis(doc), ring3: vis(ring3),
        markers: ['bench:start', 'bench:end', 'progress-lede:start', 'progress-lede:end', 'progress-chart:start', 'progress-chart:end', 'roadmap-summary:start'].filter(m => !document.documentElement.innerHTML.includes('<!-- ' + m + ' -->')),
      };
    }, BENCH);
    for (const [label, ok] of after.bench) if (!ok) fail(`${tag}: benchmark "${label}" not visible once opened`);
    if (!after.chart || after.chartDots < 3) fail(`${tag}: growth chart missing or empty once opened`);
    if (!after.lede) fail(`${tag}: growth lede missing once opened`);
    if (!after.doc) fail(`${tag}: Documented % not visible once opened`);
    if (!after.ring3) fail(`${tag}: ring-3 count not visible once opened`);
    if (after.markers.length) fail(`${tag}: generator markers lost: ${after.markers.join(', ')}`);
    // 5b. overflow, open
    if (await ov() > 0) fail(`${tag}: horizontal overflow with specs open (${await ov()}px)`);
    await page.evaluate(() => document.querySelectorAll('#specs details').forEach(d => { d.open = false; }));

    // 3 + 4. links and footer
    const f = await page.evaluate(() => {
      const ids = new Set([...document.querySelectorAll('[id]')].map(e => e.id));
      const anchors = [...document.querySelectorAll('a[href^="#"]')].map(a => a.getAttribute('href')).filter(h => h.length > 1);
      const nav = document.querySelector('footer nav[aria-label="Footer"]');
      const cols = nav ? [...nav.querySelectorAll('.foot-col')] : [];
      const hrefs = nav ? [...nav.querySelectorAll('a')].map(a => a.getAttribute('href')) : [];
      const allHrefs = [...document.querySelectorAll('a[href]')].map(a => a.getAttribute('href'));
      return {
        anchors, missing: anchors.filter(h => !ids.has(decodeURIComponent(h.slice(1)))),
        specsLinks: anchors.filter(h => h === '#specs').length,
        specsFooter: nav ? [...nav.querySelectorAll('a')].some(a => a.getAttribute('href') === '#specs') : false,
        hasNav: !!nav, hasRule: !!document.querySelector('footer .sheet-rule'),
        cols: cols.map(c => ({ h: c.querySelector('h3, h2, h4')?.textContent.trim(), n: c.querySelectorAll('a').length, x: Math.round(c.getBoundingClientRect().x), y: Math.round(c.getBoundingClientRect().y) })),
        hrefs, allHrefs,
        bar: document.querySelector('footer .foot-bar')?.textContent.replace(/\s+/g, ' ').trim() || '',
        barCta: !!document.querySelector('footer .foot-bar a[href="#waitlist"]'),
      };
    });
    if (!f.hasRule) fail(`${tag}: footer lost its .sheet-rule line`);
    if (!f.hasNav) fail(`${tag}: no <nav aria-label="Footer">`);
    if (f.cols.length !== 4) fail(`${tag}: footer has ${f.cols.length} columns, expected 4`);
    for (const c of f.cols) { if (!c.h) fail(`${tag}: a footer column has no heading`); if (c.n < 3) fail(`${tag}: footer column "${c.h}" has only ${c.n} links`); }
    if (f.cols.length === 4) {
      const xs = new Set(f.cols.map(c => c.x)), ys = new Set(f.cols.map(c => c.y));
      const want = w <= 700 ? [2, 2] : [4, 1];
      if (xs.size !== want[0] || ys.size !== want[1]) fail(`${tag}: footer columns form ${xs.size}x${ys.size}, expected ${want[0]}x${want[1]}`);
    }
    if (f.missing.length) fail(`${tag}: in-page anchors with no target: ${f.missing.join(', ')}`);
    if (f.specsLinks < 2) fail(`${tag}: only ${f.specsLinks} links to #specs (plain lines + footer expected)`);
    if (!f.specsFooter) fail(`${tag}: footer has no Tech specs link`);
    if (!/Joshua Tree v\d+\.\d+\.\d+ .*Apache License 2\.0 .*Joshua Trommel .*Vancouver, BC/.test(f.bar)) fail(`${tag}: footer bar text is wrong: "${f.bar}"`);
    if (!f.barCta) fail(`${tag}: footer bar has no waitlist link`);
    if (w === 1280 && scheme === 'light') {
      for (const h of f.allHrefs) {
        if (h.startsWith(REPO_URL + 'blob/main/') || h.startsWith(REPO_URL + 'tree/main/')) {
          const p = h.replace(REPO_URL, '').replace(/^(blob|tree)\/main\//, '');
          if (!await exists(join(REPO, p))) fail(`link ${h} points at ${p}, which is not in the repo`);
        } else if (!/^(https?:|#|mailto:|\?)/.test(h)) {
          if (!await exists(join(ROOT, h.split('#')[0]))) fail(`relative link ${h} points at a file not in landing/`);
        }
      }
      if (!f.allHrefs.some(h => h.includes('docs/'))) fail('footer has no docs/ links at all');
    }

    // 6. footer contrast
    const bad = await page.evaluate(() => {
      const parse = (c) => { const m = c.match(/rgba?\(([^)]+)\)/); const p = m[1].split(/[ ,\/]+/).map(Number); return { r: p[0], g: p[1], b: p[2], a: p.length > 3 ? p[3] : 1 }; };
      const lum = ({ r, g, b }) => { const f = (v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); }; return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b); };
      const bg = (e) => { for (; e; e = e.parentElement) { const c = parse(getComputedStyle(e).backgroundColor); if (c.a > 0.99) return c; } return { r: 255, g: 255, b: 255, a: 1 }; };
      const out = [];
      document.querySelectorAll('footer .foot-col h3, footer .foot-col a, footer .foot-meta, footer .foot-meta a').forEach(e => {
        const fg = parse(getComputedStyle(e).color), b = bg(e);
        const [hi, lo] = [lum(fg), lum(b)].sort((x, y) => y - x);
        const ratio = (hi + 0.05) / (lo + 0.05);
        if (ratio < 4.5) out.push(`${e.textContent.trim().slice(0, 30)} ${ratio.toFixed(2)}`);
      });
      return out;
    });
    if (bad.length) fail(`${tag}: footer text under 4.5:1 contrast: ${bad.join('; ')}`);

    console.log(`${tag}: ${s.count} accordions, footer ${f.cols.map(c => c.n).join('/')} links, ${f.anchors.length} anchors ok`);
    await ctx.close();
  }
  if (!failed) console.log('PASS: specs collapsed and complete, footer directory has four columns of real links, keyboard and contrast hold');
} finally {
  await browser.close();
  server.close();
}
