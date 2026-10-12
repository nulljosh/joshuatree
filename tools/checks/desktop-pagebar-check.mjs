#!/usr/bin/env node
// On a phone the landing page shows exactly ONE "Message Samantha" box: the
// real input under the demo (#demo-compose-input, with Send). The kernel's own
// bar drawn over her face is hidden on phones (user/samantha.c, uface_phone).
// The DOM half is checked here: one visible text input with that name, and it
// takes focus and typing. The canvas half is covered by mobile-type-check.mjs
// (it boots the real kernel on a phone). CHROMIUM_PATH: an installed Chromium.
import { chromium, devices } from 'playwright';
import { createServer } from 'http';
import { readFile } from 'fs/promises';
import { extname, join } from 'path';

const ROOT = new URL('../../landing/', import.meta.url).pathname;
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.svg': 'image/svg+xml', '.wasm': 'application/wasm', '.png': 'image/png', '.jpg': 'image/jpeg', '.webp': 'image/webp', '.txt': 'text/plain', '.gz': 'application/gzip' };
const server = createServer(async (req, res) => {
  const path = join(ROOT, decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  try { res.writeHead(200, { 'content-type': TYPES[extname(path)] || 'application/octet-stream' }); res.end(await readFile(path)); }
  catch { res.writeHead(404).end('nope'); }
});
await new Promise(r => server.listen(0, r));
const browser = await chromium.launch({ headless: true, ...(process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {}) });
let bad = '';
// On a desktop (2.48.1) her input is the page's own field too, floating over her face, shown only while she is open:
// the kernel's bar was a bitmap font scaled up with the screen and read pixelated. One visible "Message Samantha"
// box, it takes typing, and the kernel got `pagebar` so it draws none of its own. SHOT=<png> saves the frame.
try {
  const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, deviceScaleFactor: 2 });
  const page = await ctx.newPage();
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.route('**/api/proxy**', r => r.fulfill({ status: 403, body: '' }));
  await page.goto(`http://localhost:${server.address().port}/`, { waitUntil: 'load' });
  await page.locator('#hero-poster').click();
  await page.locator('#demo-composer').waitFor({ state: 'visible', timeout: 240000 });
  if (!await page.evaluate(() => document.getElementById('demo-frame').classList.contains('pagebar'))) bad = 'the frame never took the pagebar class';
  const n = await page.evaluate(() => [...document.querySelectorAll('input,textarea')].filter(e => e.offsetParent && /Message Samantha/.test((e.getAttribute('aria-label') || '') + (e.placeholder || ''))).length);
  if (!bad && n !== 1) bad = `${n} visible inputs named "Message Samantha" (want 1)`;
  const input = page.getByRole('textbox', { name: 'Message Samantha' });
  if (!bad) {
    await input.focus();
    await page.keyboard.type('hi');
    if (await input.inputValue() !== 'hi') bad = 'typing did not reach the input';
  }
  if (process.env.SHOT) await page.locator('#demo-frame').screenshot({ path: process.env.SHOT });
} catch (e) { bad = 'crashed: ' + e.message; }
await browser.close(); server.close();
console.log(bad ? 'FAIL ' + bad : 'PASS one sharp Message Samantha input over her face on a desktop, focusable and typeable');
process.exit(bad ? 1 : 0);
