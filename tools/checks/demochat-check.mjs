#!/usr/bin/env node
// 1.0.13: headless, real-pixel proof that the LANDING PAGE DEMO (not just
// the native kernel; tools/checks/chatapp-check.py already covers that)
// really shows a visitor Chat talking to Samantha. terminaldemo-check.mjs
// is the model this follows: an in-process static server over landing/,
// a real headless Chromium, page.evaluate reading window.__jt -- but
// driven deterministically (dock click + emu.keyboard_send_text) instead
// of waiting on the idle tour, which can take a couple of minutes to
// cycle around to Chat and would make this flaky to run twice in CI.
//
// Hermetic: page.route intercepts the demo's own /api/proxy fetch (v86's
// fetch-relay turns the guest's plain-HTTP request into this real,
// same-origin browser fetch -- see embed.js's `cors_proxy` and worker.js's
// isSamanthaChat exception) and answers with a fixed, recognisable reply.
// turing.heyitsmejosh.com is never touched, same reasoning
// chat-samantha-check.py / chatapp-check.py already use for the native
// kernel path. Every OTHER proxied request (geo/weather/map tiles) gets a
// flat 403, so this never depends on this sandbox's real network access.
//
// The idle tour is permanently disabled for this page load via
// page.emulateMedia({reducedMotion: 'reduce'}) -- embed.js reads
// prefers-reduced-motion once at parse time and never arms tourArmed's
// setInterval when it's set -- rather than clicking into the demo for
// real (embed.js's own focusIn()), which would also arm a 15s idle-reset
// reboot that a real (if intercepted) network round trip could lose the
// race against. This also means `focused` never becomes true and the
// tour's own updateHeadline() never runs, so there's nothing to assert
// about the H1 here (see CLAUDE.md's task notes: skip the headline
// assertion when driving directly instead of through the tour).
//
// Proof is on screen, not just on serial: kernel/chat.h's gui_launch_chat_app
// draws the whole console in one of two colours -- CHAT_INK (#1C1C1E) for
// both the echoed question and the assistant's reply, CHAT_DIM (#75726E)
// for the ">>> " prompt glyphs and the status/footer lines -- so counting
// exact CHAT_INK pixels in the band strictly below the question row is a
// real, discriminating signal for "did an answer actually render", not a
// guess: it's nonzero before anything is sent (1.3.0's empty-state
// suggestion rows draw in CHAT_INK there) and, once a message is sent and
// the transcript replaces that empty state, stays 0 if a reply with empty
// content ever rendered (proved live below, temporarily,
// then reverted). VX/VY/VW/VH/QROW_TOP/REPLY_TOP below are the exact same
// geometry tools/checks/chatapp-check.py already derives and proves against
// the real kernel (dock-launched window viewport at logical (78,72), size
// 804x345; T=gui_app_dy()=-32 windowed; question row at local y=76 i.e.
// screen y=72+44=116; reply starts one row (20px) lower).
//
// Usage: node tools/checks/demochat-check.mjs   (after `make kernel.elf`;
// this script does NOT rebuild it itself, so a stale landing/v86/kernel.elf
// stays stale -- same contract terminaldemo-check.mjs already documents)
import { chromium } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../landing');
const kernelElf = path.join(root, 'v86', 'kernel.elf');
if (!fs.existsSync(kernelElf)) {
  console.error('FAIL: ' + kernelElf + ' does not exist -- run `make kernel.elf` first (its Makefile rule copies the build to landing/v86/kernel.elf)');
  process.exit(1);
}

// Real browser binary this sandbox has, not the revision playwright-core's
// own default lookup expects (see the environment's PLAYWRIGHT_BROWSERS_PATH);
// the plain symlink resolves regardless of which exact revision folder it
// happens to point at.
// This sandbox preinstalls a Chromium at /opt/pw-browsers/chromium whose revision
// does not match playwright's pinned one, so it is used only when it exists;
// CI (`npx playwright install --with-deps chromium`) and a laptop use
// Playwright's own download. JT_CHROMIUM overrides either.
const CHROMIUM_PATH = process.env.JT_CHROMIUM || (fs.existsSync('/opt/pw-browsers/chromium') ? '/opt/pw-browsers/chromium' : undefined);
const EMPTY_REPLY = process.env.DEMOCHAT_EMPTY_REPLY === '1'; // discriminating-proof toggle only, see this file's own report

const LOGICAL_W = 960, LOGICAL_H = 540;
const QUESTION = 'what is the capital of france';
const REPLY_TEXT = EMPTY_REPLY ? '' : 'Paris is the capital of France, and this reply came through the demo proxy.';
const VX = 78, VY = 72, VW = 804, VH = 345; // gui_launch_from_dock's viewport: (win_x+8, win_y+32, win_w-16, win_h-40)
const QROW_TOP = VY + (-32 + 76);   // 116: T=gui_app_dy()=-32 (windowed), first turn drawn at local y=76
const REPLY_TOP = QROW_TOP + 20;    // 136: one user-row's worth (16px text + 4px gap) below the question
const INK = [0x1C, 0x1C, 0x1E];     // CHAT_INK
const INK_TOL = 8;                  // per-channel tolerance, comfortably tighter than CHAT_DIM's own ~90-value gap from CHAT_INK
const REPLY_INK_THRESHOLD = 200;    // same threshold chatapp-check.py already proves discriminating for a similarly-sized reply

// Same arithmetic as embed.js's own dockSlotPos(), not re-derived: GUI_ICON_COUNT
// (11) slots, dock_scale_pct (default 7) percent of height capped by the 740px
// DOCK_BUDGET, gap 6, pad 10, bottom margin 24.
function dockSlotPos(slot) {
  const count = 11, gap = 6, pad = 10, marginBot = 24, budget = 740;
  let icon = Math.floor(LOGICAL_H * 7 / 100);
  const maxByWidth = Math.floor((budget - 2 * pad - (count - 1) * gap) / count);
  if (icon > maxByWidth) icon = maxByWidth;
  if (icon < 16) icon = 16;
  const dockW = count * icon + (count - 1) * gap + 2 * pad;
  const x0 = Math.floor((LOGICAL_W - dockW) / 2) + pad + Math.floor(icon / 2);
  const cy = LOGICAL_H - marginBot - pad - Math.floor(icon / 2);
  return [x0 + slot * (icon + gap), cy];
}
const CHAT_SLOT = 7; // GUI_DOCK_DEFAULT order in kernel.c: Apps,Burrow,Mail,Calendar,Notes,Reminders,Terminal,Samantha,Weather,Stocks,Trash (0-indexed dock tiles)
const [CHAT_X, CHAT_Y] = dockSlotPos(CHAT_SLOT);

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.txt': 'text/plain', '.elf': 'application/octet-stream' };
const server = http.createServer((q, r) => {
  const f = path.join(root, q.url.split('?')[0] === '/' ? 'index.html' : decodeURIComponent(q.url.split('?')[0]));
  if (!f.startsWith(root) || !fs.existsSync(f)) { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(r);
});
await new Promise(res => server.listen(0, res));
const url = `http://localhost:${server.address().port}/index.html`;

const fails = [];
function fail(msg) { fails.push(msg); console.log('  FAIL: ' + msg); }
function ok(msg) { console.log('  ok:   ' + msg); }

const browser = await chromium.launch(CHROMIUM_PATH ? { executablePath: CHROMIUM_PATH } : {});
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });

// Never arms the idle tour for this page load at all (embed.js reads
// prefers-reduced-motion once, at parse time) -- see this file's header
// comment for why that's the real fix instead of clicking into the demo
// (embed.js's own focusIn()) and racing its 15s idle-reset reboot.
await page.emulateMedia({ reducedMotion: 'reduce' });

// 1.2.0: the tour's Chat scene now asks Samantha to run four real local
// tools (kernel/chat.h's chat_run_tool) plus a final open_app -- each one
// a real /api/pick round trip through this same proxy first. Answered here
// the same hermetic way /api/chat already is: a fixed, recognisable
// {"tool","arg"} shape per known question, everything else (including the
// existing "what is the capital of france" case, which must keep falling
// through to chat_send exactly as it did before this pass) gets the flat
// 403 chat_pick already treats as "no tool".
const PICK_REPLIES = {
  'remind me to call mom at 5': { tool: 'new_reminder', arg: 'call mom at 5' },
  'note: pick up dry cleaning': { tool: 'new_note', arg: 'pick up dry cleaning' },
  "what's the weather like": { tool: 'weather', arg: '' },
  "what's on my calendar today": { tool: 'calendar_today', arg: '' },
  'note: book flights for the launch': { tool: 'new_note', arg: 'book flights for the launch' },
};

let recordedBody = null, recordedMethod = null, proxyPostSeen = false;
await page.route('**/api/proxy**', async (route) => {
  const req = route.request();
  let target = '';
  try { target = new URL(req.url()).searchParams.get('url') || ''; } catch (e) { /* fall through to 403 below */ }
  let targetUrl = null;
  try { targetUrl = new URL(target); } catch (e) { /* not a valid absolute url -> 403 below */ }
  const isSamanthaChat = targetUrl && targetUrl.hostname === 'turing.heyitsmejosh.com' && targetUrl.pathname === '/api/chat';
  const isSamanthaPick = targetUrl && targetUrl.hostname === 'turing.heyitsmejosh.com' && targetUrl.pathname === '/api/pick';
  if (isSamanthaChat && req.method() === 'POST') {
    recordedBody = req.postData();
    recordedMethod = req.method();
    proxyPostSeen = true;
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      headers: { 'Access-Control-Allow-Origin': '*' },
      body: JSON.stringify({ model: 'samantha', message: { role: 'assistant', content: REPLY_TEXT }, done: true }),
    });
  } else if (isSamanthaPick && req.method() === 'POST') {
    let q = '';
    try { q = JSON.parse(req.postData() || '{}').q || ''; } catch (e) { /* malformed body -> no match below, 403 */ }
    const picked = PICK_REPLIES[q];
    if (picked) {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        headers: { 'Access-Control-Allow-Origin': '*' },
        body: JSON.stringify({ tool: picked.tool, arg: picked.arg }),
      });
    } else {
      // The capital-of-france question, and anything else unrecognized:
      // no tool, same as a real host that decided nothing matched -- falls
      // straight through to chat_send.
      await route.fulfill({ status: 403, body: '' });
    }
  } else {
    // Hermetic by design: every other proxied request (geo/weather/map
    // tiles, or a malformed url= this route couldn't even parse) gets a
    // flat 403 -- real internet access is never needed for this check.
    await route.fulfill({ status: 403, body: '' });
  }
});

await page.goto(url, { waitUntil: 'load' });

// Exact-INK pixel count over the whole screen, sampled per logical pixel. The
// desktop photo carries some near-INK pixels, so callers compare against a
// baseline taken with her window open and still empty.
async function ink() {
  return await page.evaluate(([ink, tol]) => {
    const c = document.querySelector('#screen_canvas');
    if (!c || !c.width || !c.height) return null;
    const scale = c.width / 960;
    const data = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    let total = 0;
    for (let y = 0; y < 540; y++) for (let x = 0; x < 960; x++) {
      const i = (Math.min(c.height - 1, Math.round(y * scale)) * c.width + Math.min(c.width - 1, Math.round(x * scale))) * 4;
      if (Math.abs(data[i] - ink[0]) <= tol && Math.abs(data[i + 1] - ink[1]) <= tol && Math.abs(data[i + 2] - ink[2]) <= tol) total++;
    }
    return { total, scale, canvasW: c.width, canvasH: c.height };
  }, [INK, INK_TOL]);
}

try {
  console.log('serving ' + url);
  await page.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 60000 });
  ok('window.__jt.ready');

  // Same explicit enable the tour's own runSoloApp already relies on
  // (libv86.js gates programmatic keyboard_send_text/mouse sends on these
  // exact flags, independent of embed.js's own `focused`).
  await page.evaluate(() => {
    const e = window.__jt.emu;
    e.mouse_adapter.emu_enabled = true;
    e.keyboard_adapter.emu_enabled = true;
  });

  await page.evaluate(([x, y]) => window.__jt.moveTo(x, y), [CHAT_X, CHAT_Y]);
  await page.waitForTimeout(300);
  await page.evaluate(() => window.__jt.click());
  ok(`clicked dock slot ${CHAT_SLOT} (Chat) at (${CHAT_X},${CHAT_Y})`);

  // 2.0: Samantha is a ring-3 window (user/samantha.c). Its own markers say
  // the window opened and the input bar is drawn and focused; nothing else
  // gates typing, so the scene types straight into the bar, no hotkey.
  await page.waitForFunction(() => window.__jt.serial.includes('samopen'), null, { timeout: 30000 });
  await page.waitForFunction(() => window.__jt.serial.includes('samfocus'), null, { timeout: 30000 });
  ok('samopen/samfocus markers seen: Samantha opened with her input bar focused');
  await page.waitForTimeout(800); // first frame (and her face fetch's 403) settle before typing
  const before = await ink();
  if (!before) fail('could not read the v86 canvas at all');
  else console.log(`ink before sending: ${before.total} (canvas ${before.canvasW}x${before.canvasH})`);

  // The tour's exact delivery: the sentence plus Enter, typed straight in at
  // 55ms a key. A stray leading 'n' (the old windowed console's hotkey)
  // would corrupt the sentence, which the body assertions below catch.
  await page.evaluate(async (q) => { await window.__jt.emu.keyboard_send_text(q, 55); }, QUESTION + '\n');
  ok(`typed "${QUESTION}" + Enter straight into her input bar through emu.keyboard_send_text`);

  const t0 = Date.now();
  while (Date.now() - t0 < 30000 && !proxyPostSeen) await page.waitForTimeout(200);
  if (!proxyPostSeen) fail('the demo never made the intercepted POST to turing.heyitsmejosh.com/api/chat through /api/proxy');
  else ok('intercepted the guest\'s real POST through /api/proxy (v86\'s fetch relay)');

  await page.waitForFunction(() => window.__jt.serial.includes('chatreply='), null, { timeout: 30000 }).catch(() => fail('no chatreply= marker within 30s of the intercepted reply'));
  await page.waitForTimeout(600); // let the redraw after chat_send() returns actually paint

  // Her bubble draws in INK; the face band above it never reaches this much of it.
  const after = await ink();
  console.log(`ink after the reply: ${after ? after.total : '(no canvas)'}`);
  if (!after || !before) fail('could not read the v86 canvas after the reply');
  else if (after.total - before.total < REPLY_INK_THRESHOLD) fail(`reply did not render on screen: only +${after.total - before.total} CHAT_INK px over the empty window (need >= ${REPLY_INK_THRESHOLD})`);
  else ok(`reply rendered on screen: +${after.total - before.total} CHAT_INK px over the empty window`);

  console.log('recorded proxy request: method=' + recordedMethod + ' body=' + (recordedBody || '').slice(0, 300));
  try {
    const j = JSON.parse(recordedBody || '{}');
    if (j.model !== 'samantha') fail(`recorded request model != samantha: ${JSON.stringify(j.model)}`);
    else ok('recorded request has model=samantha');
    const lastUser = (j.messages || []).filter(m => m.role === 'user').slice(-1)[0];
    if (!lastUser || lastUser.content !== QUESTION) fail(`recorded request's newest user message is wrong: ${JSON.stringify(lastUser)}`);
    else ok('recorded request\'s newest user message is the typed question');
  } catch (e) {
    fail('recorded proxy request body is not Ollama-shaped JSON: ' + e.message);
  }

  // 1.2.0: the tour's Chat scene now runs four real local tools plus a
  // final open_app before it ends (see embed.js's TOUR_APPS Chat entry and
  // this file's own PICK_REPLIES above). Proven here the same way the
  // capital-of-france exchange above already is: type the exact
  // sentence the tour itself sends, straight into her bar, wait for chat_run_tool's own
  // discriminating `chattool=<tool>:...` serial marker (kernel/chat.h).
  const TOOL_SCENES = [
    { q: 'remind me to call mom at 5', marker: 'chattool=new_reminder:' },
    { q: 'note: pick up dry cleaning', marker: 'chattool=new_note:' },
    { q: "what's the weather like", marker: 'chattool=weather:' },
    { q: "what's on my calendar today", marker: 'chattool=calendar_today:' },
    { q: 'note: book flights for the launch', marker: 'chattool=new_note:' }, // lap 2's second line; the tour no longer ends on open calculator, Escape closes her
  ];
  const seenMarker = {};
  for (const scene of TOOL_SCENES) {
    seenMarker[scene.marker] = (seenMarker[scene.marker] || 0) + 1; // new_note: fires twice, so wait for the Nth occurrence
    const toolName = scene.marker.replace(/^chattool=|:$/g, '');
    await page.evaluate(async (q) => { await window.__jt.emu.keyboard_send_text(q, 55); }, scene.q + '\n');
    await page.waitForFunction(([t, n]) => (window.__jt.toolCounts[t] || 0) >= n, [toolName, seenMarker[scene.marker]], { timeout: 15000 })
      .then(() => ok(`tool scene ran: "${scene.q}" -> ${scene.marker}`))
      .catch(() => fail(`tool scene never fired: "${scene.q}" -> ${scene.marker}`));
    await page.waitForTimeout(300);
  }

  const outPath = '/tmp/jt-demochat-after.png';
  await page.locator('#screen_canvas').screenshot({ path: outPath });
  console.log('saved ' + outPath);

  // Real bug fixed by fix/phone-samantha-picker: on a phone-sized viewport
  // embed.js's cmdline adds "phone samantha " (kernel.c's boot_to_phone +
  // boot_to_samantha), which lands straight in Chat's full-screen avatar
  // (kernel/chat.h's chat_boot_samantha_open) -- an already-open input box,
  // not the windowed console's 'n'-to-compose flow every scene above just
  // drove. phoneSamanthaIntro (embed.js) types directly into it, no
  // leading 'n'. Proven here the same discriminating way the desktop tool
  // scenes above are: the real /api/pick body must be the clean sentence
  // (not "nremind me to call mom at 5", the exact corruption this bug
  // produced), and kernel/chat.h's own chattool=new_reminder: marker --
  // not a decline line -- must be the result.
  console.log('--- phone path ---');
  const phonePage = await browser.newPage({ viewport: { width: 390, height: 844 } });
  await phonePage.emulateMedia({ reducedMotion: 'reduce' });
  let phonePickBody = null, phonePickSeen = false;
  await phonePage.route('**/api/proxy**', async (route) => {
    const req = route.request();
    let target = '';
    try { target = new URL(req.url()).searchParams.get('url') || ''; } catch (e) { /* fall through to 403 below */ }
    let targetUrl = null;
    try { targetUrl = new URL(target); } catch (e) { /* not a valid absolute url -> 403 below */ }
    const isPick = targetUrl && targetUrl.hostname === 'turing.heyitsmejosh.com' && targetUrl.pathname === '/api/pick';
    if (isPick && req.method() === 'POST') {
      phonePickBody = req.postData();
      phonePickSeen = true;
      let q = '';
      try { q = JSON.parse(phonePickBody || '{}').q || ''; } catch (e) { /* malformed -> 403 below */ }
      if (q === 'remind me to call mom at 5') {
        await route.fulfill({ status: 200, contentType: 'application/json', headers: { 'Access-Control-Allow-Origin': '*' }, body: JSON.stringify({ tool: 'new_reminder', arg: 'call mom at 5' }) });
      } else {
        await route.fulfill({ status: 403, body: '' }); // any other/garbled q (e.g. the pre-fix "nremind me...") falls through to /api/chat below, same as a real unmatched pick
      }
    } else {
      // /api/chat and everything else: the decline line is what a real LLM
      // says to a garbled sentence, so a 403 here (falls through to
      // chat_send's own error text) is enough to prove the FIX path never
      // needs it -- this route should never see a POST at all once the
      // pick body is clean.
      await route.fulfill({ status: 403, body: '' });
    }
  });
  await phonePage.goto(url, { waitUntil: 'load' });
  try {
    await phonePage.waitForFunction(() => window.__jt && window.__jt.ready, null, { timeout: 60000 });
    await phonePage.evaluate(() => {
      const e = window.__jt.emu;
      e.mouse_adapter.emu_enabled = true;
      e.keyboard_adapter.emu_enabled = true;
    });
    await phonePage.waitForFunction(() => window.__jt.serial.includes('samopen'), null, { timeout: 20000 });
    ok('phone: booted straight into the full-screen avatar (samopen)');
    await phonePage.waitForFunction(() => window.__jt.serial.includes('samfocus'), null, { timeout: 20000 });
    ok('phone: avatar input box is focused (samfocus)');

    // Same exact input phoneSamanthaIntro (embed.js) drives: typed
    // straight into the already-open avatar box, no leading 'n' -- this is
    // what the fix changed (before, runSoloApp's shared script prepended
    // 'n', corrupting the message into "nremind me to call mom at 5").
    await phonePage.evaluate(async (q) => { await window.__jt.emu.keyboard_send_text(q, 55); }, 'remind me to call mom at 5\n');
    ok('phone: typed the reminder sentence directly into the avatar, no leading n');

    const t0 = Date.now();
    while (Date.now() - t0 < 15000 && !phonePickSeen) await phonePage.waitForTimeout(200);
    if (!phonePickSeen) fail('phone: the demo never made the intercepted POST to /api/pick through /api/proxy');
    else {
      let q = '';
      try { q = JSON.parse(phonePickBody || '{}').q || ''; } catch (e) { /* logged raw below */ }
      if (q === 'remind me to call mom at 5') ok(`phone: /api/pick got the clean sentence (q="${q}")`);
      else fail(`phone: /api/pick got a corrupted q: ${JSON.stringify(phonePickBody)}`);
    }

    await phonePage.waitForFunction(() => window.__jt.serial.includes('chattool=new_reminder:'), null, { timeout: 15000 })
      .then(() => ok('phone: chattool=new_reminder: fired -- the reminder was really created, not the decline line'))
      .catch(() => fail('phone: chattool=new_reminder: never fired within 15s'));
  } catch (e) {
    fail('phone path setup failed: ' + e.message);
  }
} finally {
  await browser.close();
  server.close();
}

if (fails.length) {
  console.log('FAIL:');
  for (const m of fails) console.log('  - ' + m);
  process.exit(1);
}
console.log('PASS: the landing demo\'s real Chat app asked Samantha a question through the proxied fetch and rendered her reply on screen');
