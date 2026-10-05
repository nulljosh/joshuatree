#!/usr/bin/env node
// 2.11.1: the landing hero must never be a blank box, and must open on Samantha.
//
// Real headless Chromium against landing/ (in-process static server, hermetic
// /api/proxy that serves only the committed /face/*.jpg frames). It proves, with
// pixels and the kernel's own serial log, not DOM presence:
//   1. Before any click the hero shows a real picture: the poster is visible, its
//      natural aspect ratio equals the rendered box (no stretch or crop), and a
//      screenshot of the stage has the luminance spread and colour of a portrait,
//      not a flat cream or black field.
//   2. Click lifts the poster, the kernel boots to Samantha by default (serial
//      "bootsamantha" then "samopen"), and the live canvas is her face: warm,
//      detailed, and close to the poster still (so the click does not jump to a
//      different picture).
//   3. Esc closes her (serial "SAMANTHA.BIN exited") and the desktop is up.
//   4. ?desktop shows no poster and the kernel gets no "samantha" token.
//   5. Phone width (390) and tablet (820): no horizontal scroll, the right source
//      is used (430x760 portrait on a phone), and a tap lifts the poster.
//   6. Self-test: with the poster images blocked the same blank-pixel assertion
//      FAILS. A check that cannot fail proves nothing; this shows it can.
//
// Usage: node tools/checks/hero-poster-check.mjs   (builds kernel.elf first, like democrisp-check)
import { chromium } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execSync } from 'node:child_process';

execSync('make -s kernel.elf', { stdio: 'ignore' }); // landing/v86/kernel.elf is a build artifact the page boots
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../landing');
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.webp': 'image/webp', '.jpg': 'image/jpeg', '.png': 'image/png', '.txt': 'text/plain' };
const server = http.createServer((q, r) => {
  const rel = q.url.split('?')[0];
  const f = path.join(root, rel === '/' ? 'index.html' : decodeURIComponent(rel));
  if (!f.startsWith(root) || !fs.existsSync(f)) { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(r);
});
await new Promise(res => server.listen(0, res));
const base = `http://localhost:${server.address().port}/`;
const CHROMIUM_PATH = process.env.JT_CHROMIUM || (fs.existsSync('/opt/pw-browsers/chromium') ? '/opt/pw-browsers/chromium' : undefined);
const browser = await chromium.launch(CHROMIUM_PATH ? { executablePath: CHROMIUM_PATH } : {});
const fails = [];
const ok = m => console.log('  ok:   ' + m);
const fail = m => { console.log('  FAIL: ' + m); fails.push(m); };
const DEADLINE = 90000; // v86 boots slowly; every wait is a condition with this ceiling

async function newPage(viewport, { mobile = false, blockPoster = false } = {}) {
  const ctx = await browser.newContext({ viewport, hasTouch: mobile, isMobile: mobile, deviceScaleFactor: mobile ? 2 : 1 });
  const page = await ctx.newPage();
  await page.route('**/api/proxy**', async route => {
    let u = null;
    try { u = new URL(new URL(route.request().url()).searchParams.get('url') || ''); } catch (e) { /* 403 */ }
    if (u && /^\/face\/(idle|talk)-\d{1,2}\.jpg$|^\/face\/hd\.jpg$/.test(u.pathname)) {
      const file = path.join(root, 'face', path.basename(u.pathname));
      if (fs.existsSync(file)) return route.fulfill({ status: 200, contentType: 'image/jpeg', headers: { 'Access-Control-Allow-Origin': '*' }, body: fs.readFileSync(file) });
    }
    return route.fulfill({ status: 403, body: '' });
  });
  if (blockPoster) await page.route('**/hero/*.webp', route => route.abort());
  return page;
}

// Luminance spread, warm-colour share and a 32x18 luminance thumbnail of a PNG (bytes) or a canvas.
const STATS_FN = `(async (src) => {
  let w, h, data;
  const draw = (img, W, H) => { const c = document.createElement('canvas'); c.width = W; c.height = H; const x = c.getContext('2d'); x.drawImage(img, 0, 0, W, H); return x.getImageData(0, 0, W, H).data; };
  const img = new Image(); img.src = src; await img.decode();
  w = 320; h = Math.round(320 * img.naturalHeight / img.naturalWidth);
  data = draw(img, w, h);
  let sum = 0, sum2 = 0, warm = 0; const n = w * h;
  for (let i = 0; i < data.length; i += 4) { const l = 0.299 * data[i] + 0.587 * data[i + 1] + 0.114 * data[i + 2]; sum += l; sum2 += l * l; if (data[i] > data[i + 2] + 25) warm++; }
  const mean = sum / n, sd = Math.sqrt(Math.max(0, sum2 / n - mean * mean));
  const tw = 32, th = 18, t = draw(img, tw, th), thumb = [];
  for (let i = 0; i < t.length; i += 4) thumb.push(0.299 * t[i] + 0.587 * t[i + 1] + 0.114 * t[i + 2]);
  return { mean, sd, warm: warm / n, thumb };
})`;
const statsOf = (page, dataUrl) => page.evaluate(`${STATS_FN}(${JSON.stringify(dataUrl)})`);
const pngUrl = buf => 'data:image/png;base64,' + buf.toString('base64');

// A portrait is not a flat field: real spread of light and dark, and mostly warm tones.
// A blank cream/black box has sd ~ 0 and no warm share. Thresholds are far from both.
const SD_MIN = 25, WARM_MIN = 0.5;
async function assertPosterPixels(page, tag, report) {
  const poster = await page.evaluate(() => {
    const e = document.getElementById('hero-poster');
    const cs = e ? getComputedStyle(e) : null;
    const img = e && e.querySelector('img');
    const r = e ? e.getBoundingClientRect() : { width: 0, height: 0 };
    return { shown: !!e && !e.hidden && cs.display !== 'none' && r.width > 0, w: r.width, h: r.height, nw: img ? img.currentSrc && img.naturalWidth : 0, nh: img ? img.naturalHeight : 0, fit: img ? getComputedStyle(img).objectFit : '', src: img ? img.currentSrc.split('/').pop() : '' };
  });
  const problems = [];
  if (!poster.shown) problems.push('poster is not visible before the click');
  if (!poster.nw) problems.push('poster image did not load (naturalWidth 0)');
  if (poster.nw && poster.nh) {
    const boxR = poster.w / poster.h, imgR = poster.nw / poster.nh;
    if (Math.abs(boxR / imgR - 1) > 0.01) problems.push(`poster box ${poster.w.toFixed(0)}x${poster.h.toFixed(0)} (${boxR.toFixed(3)}) does not match the image ${poster.nw}x${poster.nh} (${imgR.toFixed(3)}): stretched or cropped`);
  }
  const shot = await page.locator('#stage-wrap').screenshot();
  const st = await statsOf(page, pngUrl(shot));
  if (st.sd < SD_MIN) problems.push(`stage looks flat (luminance sd ${st.sd.toFixed(1)} < ${SD_MIN}): blank box`);
  if (st.warm < WARM_MIN) problems.push(`stage is not a warm portrait (warm share ${(st.warm * 100).toFixed(0)}% < ${WARM_MIN * 100}%)`);
  if (report) console.log(`  [${tag}] poster ${poster.src} ${poster.nw}x${poster.nh} in ${poster.w.toFixed(0)}x${poster.h.toFixed(0)}, sd=${st.sd.toFixed(1)} warm=${(st.warm * 100).toFixed(0)}%`);
  return { problems, st, poster };
}
const serial = page => page.evaluate(() => window.__jt && window.__jt.serial || '');

// ---- 1-3: desktop 1440x900, default boot --------------------------------------------------
{
  const page = await newPage({ width: 1440, height: 900 });
  await page.goto(base);
  await page.waitForSelector('#hero-poster', { state: 'visible', timeout: 15000 }).catch(() => {});
  await page.waitForFunction(() => { const i = document.querySelector('#hero-poster img'); return i && i.complete && i.naturalWidth > 0; }, null, { timeout: 15000 }).catch(() => {}); // a missing poster is reported by assertPosterPixels, not thrown here
  const before = await assertPosterPixels(page, 'desktop before click', true);
  if (before.problems.length) before.problems.forEach(p => fail('desktop before click: ' + p));
  else ok('desktop before click: poster is a real, undistorted portrait (not a blank box)');
  const sx = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
  if (sx > 0) fail(`desktop: horizontal scroll of ${sx}px`); else ok('desktop: no horizontal scroll');
  const cta = await page.evaluate(() => { const c = document.querySelector('.poster-cta-desktop'); return c && getComputedStyle(c).display !== 'none' ? c.textContent.trim() : ''; });
  if (cta !== 'Click to boot') fail(`desktop: affordance reads "${cta}", want "Click to boot"`); else ok('desktop: "Click to boot" affordance visible');
  const dl = await page.evaluate(() => { const a = document.getElementById('desktop-boot-link'); return a ? { t: a.textContent.trim(), h: a.getAttribute('href'), v: a.offsetParent !== null } : null; });
  if (!dl || dl.t !== 'See the desktop' || dl.h !== '?desktop' || !dl.v) fail('desktop: "See the desktop" link missing or wrong ' + JSON.stringify(dl)); else ok('desktop: "See the desktop" links to ?desktop');
  if (await page.evaluate(() => !!document.getElementById('samantha-boot-link'))) fail('the old "Boot straight into Samantha" link is still there (duplicate)'); else ok('no duplicate "Boot straight into Samantha" link');

  const poster = before.st.thumb;
  await page.waitForFunction(() => /samopen/.test(window.__jt.serial), null, { timeout: DEADLINE });
  ok('kernel booted to Samantha by default (serial samopen, no click needed)');
  if (!/bootsamantha/.test(await serial(page))) fail('serial has no "bootsamantha": the default cmdline did not carry the samantha token'); else ok('default cmdline carried the samantha token (serial bootsamantha)');
  await page.click('#hero-poster');
  await page.waitForFunction(() => document.getElementById('hero-poster').hidden, null, { timeout: 5000 });
  ok('click lifts the poster');
  await page.waitForFunction(() => window.__jt.focused, null, { timeout: 5000 });
  ok('click takes the keyboard (focused)');
  await page.waitForTimeout(8000); // her idle frames and the glass bar settle
  const canvasUrl = await page.evaluate(() => document.getElementById('screen_canvas').toDataURL('image/png'));
  const live = await statsOf(page, canvasUrl);
  let diff = 0; for (let i = 0; i < poster.length; i++) diff += Math.abs(poster[i] - live.thumb[i]); diff /= poster.length;
  console.log(`  [desktop after click] canvas sd=${live.sd.toFixed(1)} warm=${(live.warm * 100).toFixed(0)}% thumb diff vs poster=${diff.toFixed(1)}/255`);
  if (live.sd < SD_MIN || live.warm < WARM_MIN) fail('after the click the live canvas is not her face (flat or not warm)'); else ok('after the click the live canvas is her face (detailed, warm)');
  if (diff > 18) fail(`live canvas differs from the poster still by ${diff.toFixed(1)}/255 mean luminance: the click jumps to a different picture`); else ok(`live canvas matches the poster still (mean luminance diff ${diff.toFixed(1)}/255)`);
  await page.keyboard.press('Escape');
  await page.waitForFunction(() => /ring3app: SAMANTHA\.BIN exited/.test(window.__jt.serial), null, { timeout: 15000 }).then(() => ok('Esc closes her (SAMANTHA.BIN exited)'), () => fail('Esc did not close Samantha'));
  await page.waitForTimeout(2500);
  const desk = await statsOf(page, await page.evaluate(() => document.getElementById('screen_canvas').toDataURL('image/png')));
  if (desk.warm > 0.35 || desk.sd < SD_MIN) fail(`after Esc the canvas is not the desktop (warm ${(desk.warm * 100).toFixed(0)}%, sd ${desk.sd.toFixed(1)})`); else ok('after Esc the desktop is showing');
  await page.context().close();
}

// ---- 4: ?desktop ---------------------------------------------------------------------------
{
  const page = await newPage({ width: 1440, height: 900 });
  await page.goto(base + '?desktop');
  const shown = await page.evaluate(() => { const e = document.getElementById('hero-poster'); return !!e && getComputedStyle(e).display !== 'none' && !e.hidden; });
  if (shown) fail('?desktop still shows the poster'); else ok('?desktop shows no poster');
  const linkHidden = await page.evaluate(() => { const a = document.getElementById('desktop-boot-link'); return !a || a.offsetParent === null; });
  if (!linkHidden) fail('?desktop still offers "See the desktop"'); else ok('?desktop hides the "See the desktop" link (already there)');
  await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: DEADLINE });
  await page.waitForFunction(() => /menubarredraw|fullrepaint/.test(window.__jt.serial), null, { timeout: DEADLINE });
  const s = await serial(page);
  if (/bootsamantha|samopen/.test(s)) fail('?desktop booted into Samantha'); else ok('?desktop boots the desktop (no samantha token, no samopen)');
  await page.context().close();
}

// ---- 5: phone 390 and tablet 820 -----------------------------------------------------------
for (const [tag, vp, mobile, wantSrc, wantR] of [['phone 390', { width: 390, height: 844 }, true, 'samantha-phone.webp', 430 / 760], ['tablet 820', { width: 820, height: 1180 }, false, 'samantha.webp', 16 / 9]]) {
  const page = await newPage(vp, { mobile });
  await page.goto(base);
  await page.waitForFunction(() => { const i = document.querySelector('#hero-poster img'); return i && i.complete && i.naturalWidth > 0; }, null, { timeout: 15000 }).catch(() => {}); // a missing poster is reported by assertPosterPixels, not thrown here
  const r = await assertPosterPixels(page, tag, true);
  if (r.problems.length) r.problems.forEach(p => fail(`${tag}: ${p}`)); else ok(`${tag}: poster is a real, undistorted portrait`);
  if (r.poster.src !== wantSrc) fail(`${tag}: poster source ${r.poster.src}, want ${wantSrc}`); else ok(`${tag}: poster source is ${wantSrc}`);
  if (Math.abs(r.poster.w / r.poster.h / wantR - 1) > 0.01) fail(`${tag}: poster box is not ${wantR.toFixed(3)}`);
  const sx = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
  if (sx > 0) fail(`${tag}: horizontal scroll of ${sx}px`); else ok(`${tag}: no horizontal scroll`);
  if (mobile) await page.tap('#hero-poster'); else await page.click('#hero-poster');
  await page.waitForFunction(() => document.getElementById('hero-poster').hidden, null, { timeout: 5000 }).then(() => ok(`${tag}: ${mobile ? 'tap' : 'click'} lifts the poster`), () => fail(`${tag}: poster did not lift`));
  await page.context().close();
}

// ---- 6: self-test, the assertion must FAIL when the poster is gone --------------------------
{
  const page = await newPage({ width: 1440, height: 900 }, { blockPoster: true });
  await page.goto(base);
  await page.waitForTimeout(2000);
  const r = await assertPosterPixels(page, 'self-test, poster blocked', true);
  if (r.problems.length === 0) fail('self-test: the poster assertion PASSED with the poster images blocked, so it proves nothing'); else ok(`self-test: with the poster removed the assertion fails (${r.problems.length} problem${r.problems.length > 1 ? 's' : ''}: ${r.problems[0]})`);
  await page.context().close();
}

await browser.close();
server.close();
if (fails.length) { console.log('FAIL:'); for (const m of fails) console.log('  - ' + m); process.exit(1); }
console.log('PASS: the hero shows a real portrait before any click, boots into Samantha by default, Esc reaches the desktop, ?desktop opts out, and phone/tablet hold');
