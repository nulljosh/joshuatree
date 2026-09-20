// Regression check: the landing tour's Terminal demo must type real shell
// commands into the real Terminal app, not garbage.
//
// Root cause it guards (Sep 2026): embed.js dockSlotPos() hardcoded a
// 10-tile dock after Stocks became the 11th pinned tile, so every tour
// click landed about half a tile off. The tour opened the neighbouring
// app and typed each script into the wrong window (Reminders text ended
// up in the Terminal prompt as "ashi p the demo tour rework").
//
// Read-back: the terminal's scrollback (term_buf in kernel.c) is plain
// ASCII in guest RAM, so this scans the emulator's memory for it instead
// of squinting at pixels. The idle tour is left completely alone (nobody
// focuses the page), headless Chromium only.
//
// Usage: node tools/checks/terminaldemo-check.mjs   (serves ./landing itself)
import { chromium } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../landing');
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.txt': 'text/plain' };
const server = http.createServer((q, r) => {
  const f = path.join(root, q.url.split('?')[0] === '/' ? 'index.html' : decodeURIComponent(q.url.split('?')[0]));
  if (!f.startsWith(root) || !fs.existsSync(f)) { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(r);
});
await new Promise(res => server.listen(0, res));
const url = `http://localhost:${server.address().port}/index.html`;

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
await page.goto(url, { waitUntil: 'load' });

async function terminalBuffer() {
  return await page.evaluate(() => {
    const e = window.__jt && window.__jt.emu;
    if (!e || !e.v86 || !e.v86.cpu || !e.v86.cpu.mem8) return '';
    const txt = new TextDecoder('latin1').decode(e.v86.cpu.mem8.subarray(0, 16 * 1024 * 1024));
    const i = txt.lastIndexOf('Joshua Tree terminal. Type help.');
    return i < 0 ? '' : txt.slice(i, i + 1500).split('\0')[0];
  });
}

const want = [
  '> ls\n',
  '> echo hello from joshua tree\nhello from joshua tree\n',
  '> uptime\n'
];
let buf = '', ok = false;
const t0 = Date.now();
while (Date.now() - t0 < 240000) {
  await page.waitForTimeout(3000);
  buf = await terminalBuffer();
  if (want.every(w => buf.includes(w)) && /> uptime\n\d+s\n/.test(buf)) { ok = true; break; }
}
await browser.close();
server.close();
console.log('terminal scrollback:\n' + buf);
// Every prompt line in the scrollback must be one of the demo commands.
const prompts = buf.split('\n').filter(l => l.startsWith('> ')).map(l => l.slice(2));
const allowed = new Set(['ls', 'echo hello from joshua tree', 'uptime']);
const junk = prompts.filter(p => !allowed.has(p));
if (!ok || junk.length) {
  console.error(`FAIL: ok=${ok} unexpected prompt lines=${JSON.stringify(junk)}`);
  process.exit(1);
}
console.log('PASS: Terminal demo typed ls / echo / uptime correctly');
