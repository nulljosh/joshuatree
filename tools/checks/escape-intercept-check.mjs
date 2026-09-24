#!/usr/bin/env node
// MANUAL: needs Playwright's Chromium browser binaries plus a local dev server at localhost:3000 (landing/), neither installed in CI (.github/workflows/check.yml has no npm/playwright step). Run by hand after npm install && npx playwright install.
// Discriminating regression test for Escape-key interception (v0.76.26 fix attempt)
// Verifies that pressing Escape in the demo does NOT drop into the kernel's raw text shell.
//
// The bug: v86 attaches its own global keyboard listener to `window`, and an earlier
// fix attached an Escape interceptor to `container` in capture phase. But capture phase
// runs outer-to-inner (window first, then container), so v86's listener fires and
// forwards Escape to the kernel before our interceptor ever runs. The fix is to move
// the Escape listener from `container` to `window` and register it BEFORE v86's own
// listener (both in capture phase on window -- registration order FIFO).
//
// This test verifies:
// 1. Demo boots and enters graphical mode
// 2. Click to focus the demo
// 3. Press Escape key
// 4. Verify the demo is STILL in graphical mode (not dropped to shell)
// 5. Verify serial log does NOT contain any shell prompt marker

import { chromium } from 'playwright';

const url = process.argv[2] || 'https://joshuatree.heyitsmejosh.com';
const LOGICAL_W = 960;
const LOGICAL_H = 540;

async function main() {
  let browser, page;
  try {
    browser = await chromium.launch({ headless: true });
    page = await browser.newPage({ viewport: { width: 1400, height: 900 } });

    console.log('Loading page:', url);
    await page.goto(url, { waitUntil: 'load' });

    // Wait for canvas to be visible and graphical mode to start
    console.log('Waiting for graphical mode...');
    const canvas = page.locator('#screen_canvas');
    await canvas.waitFor({ state: 'visible', timeout: 60000 });
    await page.waitForFunction(() => {
      const c = document.getElementById('screen_canvas');
      return c && c.width >= 640 && getComputedStyle(c).display !== 'none';
    }, null, { timeout: 60000 });

    // Wait a bit for full boot
    await page.waitForTimeout(2000);

    console.log('Clicking into demo to focus it...');
    await page.click('#v86-embed', { position: { x: LOGICAL_W / 2, y: LOGICAL_H / 2 } });
    await page.waitForTimeout(100);

    // Get state before Escape
    const serialBefore = await page.evaluate(() => window.__jt?.serial || '');
    const graphicalBefore = await page.evaluate(() => {
      const vga = window.__jt?.emu?.v86?.cpu?.devices?.vga;
      return vga ? !!vga.graphical_mode : false;
    });

    console.log('Before Escape - graphical mode:', graphicalBefore);
    console.log('Serial log length before:', serialBefore.length);

    // Press Escape
    console.log('Pressing Escape...');
    await page.keyboard.press('Escape');

    // Wait a moment for any state change
    await page.waitForTimeout(500);

    // Get state after Escape
    const serialAfter = await page.evaluate(() => window.__jt?.serial || '');
    const graphicalAfter = await page.evaluate(() => {
      const vga = window.__jt?.emu?.v86?.cpu?.devices?.vga;
      return vga ? !!vga.graphical_mode : false;
    });

    console.log('After Escape - graphical mode:', graphicalAfter);
    console.log('Serial log length after:', serialAfter.length);

    // Check what new output was added to serial
    const newSerial = serialAfter.substring(serialBefore.length);
    console.log('New serial output after Escape:', newSerial.slice(0, 200));

    // The fix works if:
    // 1. Demo is still in graphical mode (not dropped to text)
    if (!graphicalAfter) {
      throw new Error('FAIL: Demo dropped from graphical mode to text shell after Escape');
    }

    // 2. No shell prompt appeared in the new serial output
    // Common shell markers: '=>', '$', '#', '> ' (various shell prompts)
    if (/^=>|^\$|^#|^> /.test(newSerial.trim())) {
      throw new Error(`FAIL: Shell prompt detected in serial output after Escape: ${newSerial.slice(0, 50)}`);
    }

    // If we reach here, the test passed
    console.log('PASS: Escape key was successfully intercepted, demo remains in graphical mode');
    process.exit(0);
  } catch (err) {
    console.error('✗ Test failed:', err.message);
    process.exit(1);
  } finally {
    if (browser) await browser.close();
  }
}

main();
