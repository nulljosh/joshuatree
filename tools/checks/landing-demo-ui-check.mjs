#!/usr/bin/env node
// Landing page QA fixes that a visitor (and a screen reader) would notice:
//   - one <main id="main"> that the skip link lands in, and the hero eyebrow
//     is a paragraph, not a heading that reads "v1.9.11"
//   - a canonical link, og:url and og:image:alt
//   - no keyboard trap (WCAG 2.1.2): Tab walks past the live demo without
//     taking the keyboard over, the demo shows a focus ring, a key press or a
//     click takes it over, and Shift+Escape gives it back
//   - on a phone the Full screen button is top right of the demo, not over
//     the kernel's own bottom input line, and the Full screen and Send
//     buttons are icons
//   - a phone on its side gets a real demo, not a 128x72 strip
//   - an iPad (touch, no keyboard) gets the chat bar too
//   - on a touch screen the waitlist fields are at least 44px tall
//
// LANDING_DIR points the check at another copy of landing/ (used to prove it
// fails on the old page). CHROMIUM_PATH points at an installed Chromium when
// Playwright's own download is not available; CI runs `npx playwright install`.
import { chromium, devices } from 'playwright';
import { createServer } from 'http';
import { readFile } from 'fs/promises';
import { extname, join } from 'path';

const ROOT = process.env.LANDING_DIR || new URL('../../landing/', import.meta.url).pathname;
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.svg': 'image/svg+xml',
                '.wasm': 'application/wasm', '.png': 'image/png', '.jpg': 'image/jpeg',
                '.webp': 'image/webp', '.txt': 'text/plain', '.gz': 'application/gzip' };
const server = createServer(async (req, res) => {
  const path = join(ROOT, decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  try {
    const body = await readFile(path);
    res.writeHead(200, { 'content-type': TYPES[extname(path)] || 'application/octet-stream' });
    res.end(body);
  } catch { res.writeHead(404).end('nope'); }
});
await new Promise(r => server.listen(0, r));
const base = `http://localhost:${server.address().port}/`;

const failures = [];
const ok = (cond, msg) => { if (!cond) failures.push(msg); console.log((cond ? 'PASS ' : 'FAIL ') + msg); };
const browser = await chromium.launch({ headless: true, ...(process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {}) });

async function open(opts) {
  const ctx = await browser.newContext(opts);
  const page = await ctx.newPage();
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.route('**/api/proxy**', r => r.fulfill({ status: 403, body: '' }));
  await page.goto(base, { waitUntil: 'load' });
  return { ctx, page };
}

try {
  // ---- desktop: landmarks, meta, keyboard ----
  {
    const { ctx, page } = await open({ viewport: { width: 1280, height: 900 } });
    const s = await page.evaluate(() => ({
      mains: document.querySelectorAll('main').length,
      skipTarget: (() => { const a = document.querySelector('.skip-link'); const t = a && document.querySelector(a.getAttribute('href')); return t ? t.tagName : null; })(),
      eyebrow: (document.querySelector('.eyebrow') || {}).tagName,
      canonical: !!document.querySelector('link[rel="canonical"]'),
      ogUrl: !!document.querySelector('meta[property="og:url"]'),
      ogAlt: !!document.querySelector('meta[property="og:image:alt"]'),
    }));
    ok(s.mains === 1, 'exactly one <main> landmark');
    ok(s.skipTarget === 'MAIN', `the skip link lands in <main> (got ${s.skipTarget})`);
    ok(s.eyebrow === 'P', `the hero eyebrow is a paragraph, not a heading (got ${s.eyebrow})`);
    ok(s.canonical && s.ogUrl && s.ogAlt, 'canonical link, og:url and og:image:alt are present');

    await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 120000 });
    // Walk to the demo with Tab, the way a keyboard user would.
    await page.evaluate(() => document.querySelector('.skip-link').focus());
    let onDemo = false;
    for (let i = 0; i < 6 && !onDemo; i++) {
      await page.keyboard.press('Tab');
      onDemo = await page.evaluate(() => document.activeElement && document.activeElement.id === 'v86-embed');
    }
    ok(onDemo, 'Tab reaches the demo');
    const ring = await page.evaluate(() => { const cs = getComputedStyle(document.getElementById('v86-embed')); return parseFloat(cs.outlineWidth) > 0 && cs.outlineStyle !== 'none'; });
    ok(ring, 'the focused demo shows a visible focus ring');
    await page.keyboard.press('Tab');
    const left = await page.evaluate(() => ({ active: document.activeElement && document.activeElement.id, taken: window.__jt.focused }));
    ok(left.active !== 'v86-embed' && left.taken === false, 'Tab walks past the demo without taking the keyboard over');
    await page.evaluate(() => document.getElementById('v86-embed').focus());
    await page.keyboard.press('Enter');
    ok(await page.evaluate(() => window.__jt.focused) === true, 'Enter takes the demo over');
    await page.keyboard.press('Shift+Escape');
    const released = await page.evaluate(() => ({ taken: window.__jt.focused, kb: window.__jt.emu.keyboard_adapter.emu_enabled }));
    ok(released.taken === false && released.kb === false, 'Shift+Escape gives the keyboard back (no keyboard trap)');
    await ctx.close();
  }

  // ---- phone: Full screen button top right ----
  {
    const { ctx, page } = await open({ ...devices['iPhone 13'] });
    await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 120000 });
    const g = await page.evaluate(() => {
      const b = document.getElementById('demo-exit').getBoundingClientRect();
      const w = document.getElementById('stage-wrap').getBoundingClientRect();
      return { top: b.top - w.top, right: w.right - b.right, bottom: w.bottom - b.bottom };
    });
    ok(g.top >= 0 && g.top < 30 && g.right >= 0 && g.right < 24, `on a phone Full screen sits top right of the demo (top ${g.top.toFixed(0)}px, right ${g.right.toFixed(0)}px)`);
    await ctx.close();
  }
  // ---- phone on its side: the demo is not a strip ----
  {
    const { ctx, page } = await open({ ...devices['iPhone 13 landscape'] });
    const h = await page.evaluate(() => document.getElementById('stage-wrap').getBoundingClientRect().height);
    ok(h >= 300, `a phone on its side gets a usable demo (${h.toFixed(0)}px tall)`);
    await ctx.close();
  }

  // ---- iPad: touch, no keyboard ----
  {
    const { ctx, page } = await open({ ...devices['iPad Mini'] });
    await page.locator('#hero-poster').tap();
    await page.locator('#demo-composer').waitFor({ state: 'visible', timeout: 120000 });
    ok(await page.locator('#demo-composer').isVisible(), 'an iPad shows the chat bar after boot');
    await ctx.close();
  }

  // ---- touch screen: icon buttons, big enough targets ----
  {
    const { ctx, page } = await open({ ...devices['iPhone 13'] });
    await page.waitForTimeout(1500);
    const t = await page.evaluate(() => {
      const h = sel => { const e = document.querySelector(sel); return e ? Math.round(e.getBoundingClientRect().height) : 0; };
      const exit = document.getElementById('demo-exit');
      const send = document.getElementById('demo-compose-send');
      return {
        exitIcon: parseFloat(getComputedStyle(exit).fontSize) === 0 && exit.getBoundingClientRect().width <= 44,
        exitName: exit.getAttribute('aria-label') || exit.textContent.trim(),
        sendIcon: !!send.querySelector('svg') && send.textContent.trim() === '',
        sendName: send.getAttribute('aria-label'),
        emailField: h('.waitlist-form input[type="email"]'), sendButton: h('.waitlist-form button'),
      };
    });
    ok(t.exitIcon && !!t.exitName, `Full screen is an icon with an accessible name (${JSON.stringify(t.exitName)})`);
    ok(t.sendIcon && t.sendName === 'Send', 'Send is an icon with an accessible name');
    ok(t.emailField >= 44 && t.sendButton >= 44, `the waitlist fields are big enough to tap (email ${t.emailField}px, button ${t.sendButton}px)`);
    await ctx.close();
  }
} catch (e) {
  failures.push('check crashed: ' + e.message);
  console.log('FAIL check crashed: ' + e.message);
}

await browser.close();
server.close();
if (failures.length) { console.log(`\nFAIL: ${failures.length} problem(s)`); process.exit(1); }
console.log('\nPASS: landing demo is keyboard-safe, landmarked and laid out for phones');
