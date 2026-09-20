#!/usr/bin/env node
// Probe: boot the fresh kernel.elf inside headless v86 with a REAL working
// /api/proxy (Playwright route -> node fetch, the same http-for-ip-api rule
// worker.js applies), let the landing tour run, and sample the desktop's
// brightness next to the kernel's own serial log. Not a CI check (it needs
// the live internet); it is the evidence tool for the black-desktop report.
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { mkdtempSync, cpSync, copyFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('../..', import.meta.url));
const PORT = 8937;
const SECONDS = Number(process.env.PROBE_SECONDS || 150);

const scratch = mkdtempSync(join(tmpdir(), 'jt-neverblack-'));
cpSync(join(ROOT, 'landing'), scratch, { recursive: true });
copyFileSync(join(ROOT, 'kernel.elf'), join(scratch, 'v86', 'kernel.elf'));
const server = spawn('python3', ['-m', 'http.server', String(PORT)], { cwd: scratch, stdio: 'ignore' });
await new Promise((r) => setTimeout(r, 500));

try {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
  await page.route('**/api/proxy**', async (route) => {
    const u = new URL(route.request().url());
    const target = new URL(u.searchParams.get('url'));
    if (target.hostname === 'ip-api.com') target.protocol = 'http:';
    try {
      const r = await fetch(target.toString());
      const buf = Buffer.from(await r.arrayBuffer());
      console.log('proxy', r.status, buf.length, target.toString().slice(0, 90));
      await route.fulfill({ status: r.status, body: buf, headers: { 'content-type': r.headers.get('content-type') || 'application/octet-stream' } });
    } catch (e) { console.log('proxy FAIL', String(e)); await route.fulfill({ status: 502, body: 'bad gateway' }); }
  });
  await page.goto(`http://localhost:${PORT}/index.html`, { waitUntil: 'load' });
  await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 60000 });
  let lastLen = 0;
  for (let s = 0; s < SECONDS; s += 5) {
    await page.waitForTimeout(5000);
    const info = await page.evaluate(() => {
      const c = document.querySelector('canvas');
      let mean = -1;
      try {
        const t = document.createElement('canvas'); t.width = 64; t.height = 36;
        const g = t.getContext('2d'); g.drawImage(c, 0, 0, 64, 36);
        const d = g.getImageData(0, 4, 8, 20).data; let sum = 0; // left edge strip: desktop, never under a window or the dock
        for (let i = 0; i < d.length; i += 4) sum += d[i] + d[i + 1] + d[i + 2];
        mean = sum / (d.length / 4) / 3;
      } catch (e) { mean = -2; }
      return { mean, serial: window.__jt.serial };
    });
    const fresh = info.serial.slice(lastLen); lastLen = info.serial.length;
    const lines = fresh.split('\n').filter((l) => /^(wall|geo=|wx|heap|mw)/.test(l));
    console.log(`t=${s + 5}s desktop-mean=${info.mean.toFixed(1)} ${lines.join(' | ')}`);
    if (process.env.PROBE_SHOT) await page.locator('canvas').first().screenshot({ path: `${process.env.PROBE_SHOT}-${s + 5}.png` });
  }
  await browser.close();
} finally {
  server.kill();
  rmSync(scratch, { recursive: true, force: true });
}
