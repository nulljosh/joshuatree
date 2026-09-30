#!/usr/bin/env node
// Landing demo on a phone: a visitor can actually type to it.
//
// Reported: "the Samantha demo on the mobile landing page does not work, I
// can't type anything". The kernel's Samantha, Notes and Terminal screens
// take typed keys, v86 only listens for keydown on window, and a phone has
// no keyboard for it to hear. On phones the page now shows a chat bar under
// the demo (a real text field and a Send button, which is what makes iOS and
// Android raise their keyboard). What is typed is mirrored into the kernel's
// own input line as it changes, and Send is the Enter key.
//
// Real iPhone emulation, real touch taps, the real demo booted from
// landing/v86. Hermetic: the demo's /api/proxy is answered with a 403 and
// the idle tour is off (prefers-reduced-motion), so nothing reaches the
// network and nothing else types.
//
// Asserts, in order:
//   1. a phone shows the chat bar and Send; a desktop viewport does not
//   2. tapping the field focuses it and takes the demo over (focused=true)
//   3. typing puts the letters on Samantha's screen, each sent exactly once
//      (not doubled), a correction sends Backspace (keyCode 8)
//   4. tapping Send sends Enter (keyCode 13) and empties the field
import { chromium, devices } from 'playwright';
import { createServer } from 'http';
import { readFile } from 'fs/promises';
import { extname, join } from 'path';

const ROOT = new URL('../../landing/', import.meta.url).pathname;
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

// CHROMIUM_PATH points at an already-installed Chromium when Playwright's own
// download is not available (sandboxes); CI runs `npx playwright install`.
const browser = await chromium.launch({ headless: true, ...(process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {}) });

async function open(contextOpts) {
  const ctx = await browser.newContext(contextOpts);
  const page = await ctx.newPage();
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.route('**/api/proxy**', r => r.fulfill({ status: 403, body: '' }));
  await page.goto(base, { waitUntil: 'load' });
  return { ctx, page };
}

try {
  // 1b. desktop: no Type button, no field
  {
    const { ctx, page } = await open({ viewport: { width: 1280, height: 900 } });
    const shown = await page.locator('#demo-composer').isVisible();
    ok(!shown, 'desktop viewport does not show the chat bar');
    await ctx.close();
  }

  // phone
  const { ctx, page } = await open({ ...devices['iPhone 13'] });
  await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 120000 });
  await page.waitForTimeout(8000); // let the kernel reach its home screen

  const input = page.locator('#demo-compose-input');
  const send = page.locator('#demo-compose-send');
  ok(await page.locator('#demo-composer').isVisible(), 'phone viewport shows the chat bar');
  ok(await send.isVisible(), 'phone viewport shows the Send button');

  // Open Samantha from the phone home screen with a real tap, then give the
  // kernel a few seconds to draw her empty prompt.
  const rect = await page.evaluate(() => { const r = document.getElementById('screen_canvas').getBoundingClientRect(); return [r.x, r.y, r.width, r.height]; });
  await page.touchscreen.tap(rect[0] + rect[2] * 0.30, rect[1] + rect[3] * 0.23);
  await page.waitForTimeout(7000);
  // The input bar is the strip just above the bottom buttons.
  const bar = { x: rect[0], y: rect[1] + rect[3] * 0.86, width: rect[2], height: rect[3] * 0.06 };
  const before = await page.screenshot({ clip: bar });

  // Record what the page sends into the kernel.
  await page.evaluate(() => {
    const e = window.__jt.emu;
    window.__sent = { text: [], keys: [] };
    const t = e.keyboard_send_text.bind(e), k = e.keyboard_send_keys.bind(e);
    e.keyboard_send_text = (s, d) => { window.__sent.text.push(s); return t(s, d); };
    e.keyboard_send_keys = (c, d) => { window.__sent.keys.push(...c); return k(c, d); };
  });

  // 2. a real touch tap in the field
  await input.tap();
  await page.waitForTimeout(500);
  const afterTap = await page.evaluate(() => ({
    active: document.activeElement && document.activeElement.id,
    focused: window.__jt.focused,
  }));
  ok(afterTap.active === 'demo-compose-input', 'tapping the bar focuses the text field (this is what raises the phone keyboard)');
  ok(afterTap.focused === true, 'tapping the bar takes the demo over');

  // 3. type like a soft keyboard would, then correct one letter
  await page.keyboard.type('hellp');
  await page.waitForTimeout(4500); // the keys are replayed one at a time
  const typed = await page.screenshot({ clip: bar });
  ok(!before.equals(typed), "the typed letters show up in Samantha's input bar");
  await page.keyboard.press('Backspace');
  await page.keyboard.type('o');
  await page.waitForTimeout(2500);

  // 4. Send is Enter and clears the field
  await send.tap();
  await page.waitForTimeout(3000);
  const sent = await page.evaluate(() => window.__sent);
  ok(sent.text.join('') === 'hellpo', `each letter reaches the kernel exactly once (got ${JSON.stringify(sent.text.join(''))})`);
  ok(sent.keys.includes(8), 'a correction reaches the kernel as Backspace (keyCode 8)');
  ok(sent.keys.includes(13), 'Send reaches the kernel as Enter (keyCode 13)');
  ok(await input.inputValue() === '', 'Send empties the field');
  await ctx.close();
} catch (e) {
  failures.push('check crashed: ' + e.message);
  console.log('FAIL check crashed: ' + e.message);
}

await browser.close();
server.close();
if (failures.length) { console.log(`\nFAIL: ${failures.length} problem(s)`); process.exit(1); }
console.log('\nPASS: a phone visitor can type to the demo');
