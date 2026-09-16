#!/usr/bin/env node
// Discriminating regression test for idle-reset behavior (v0.76.26)
// Verifies:
// 1. Continuous small interactions (mousemove every 1s) for 10s -> NO restart
// 2. Genuine 5s inactivity -> restart DOES happen
// 3. Any real interaction (click, keydown, keyup) immediately interrupts tour

import { chromium } from "playwright";

const TEST_URL = "http://localhost:3000"; // Adjust to local test server if needed, or use file:// URL
const LOGICAL_W = 960;
const LOGICAL_H = 540;

async function main() {
  let browser, page;
  try {
    browser = await chromium.launch({ headless: true });
    page = await browser.newPage();

    // Test 1: Continuous interaction should prevent restart
    console.log("Test 1: Continuous mousemove every 1s for 10s should NOT trigger restart...");
    await testContinuousInteractionPreventsRestart(page);
    console.log("✓ Test 1 PASS: Continuous interaction prevented restart");

    // Test 2: Genuine inactivity should trigger restart
    console.log("\nTest 2: 5s of genuine inactivity should trigger restart...");
    await testInactivityTriggersRestart(page);
    console.log("✓ Test 2 PASS: Inactivity triggered restart");

    // Test 3: Keyboard input should prevent restart
    console.log("\nTest 3: Continuous keydown/keyup every 1s for 10s should NOT trigger restart...");
    await testKeyboardInteractionPreventsRestart(page);
    console.log("✓ Test 3 PASS: Keyboard interaction prevented restart");

    console.log("\n✓ All tests passed!");
    process.exit(0);
  } catch (err) {
    console.error("✗ Test failed:", err.message);
    process.exit(1);
  } finally {
    if (browser) await browser.close();
  }
}

async function testContinuousInteractionPreventsRestart(page) {
  await page.goto(TEST_URL, { waitUntil: "networkidle" });

  // Get initial serial log length to detect if kernel reboots (new kmain output)
  const getSerialLog = () => page.evaluate(() => window.__jt?.serial || "");
  const initialSerial = await getSerialLog();
  const initialBootCount = (initialSerial.match(/=== kmain boot start ===/g) || []).length;

  // Click to focus
  await page.click("#v86-embed", { position: { x: LOGICAL_W / 2, y: LOGICAL_H / 2 } });
  await page.waitForTimeout(100);

  // Simulate continuous mousemove every 1 second for 10 seconds
  const startTime = Date.now();
  while (Date.now() - startTime < 10000) {
    const x = LOGICAL_W / 2 + Math.random() * 20 - 10;
    const y = LOGICAL_H / 2 + Math.random() * 20 - 10;
    await page.mouse.move(x, y);
    await page.waitForTimeout(1000);
  }

  // Check if restart happened
  const finalSerial = await getSerialLog();
  const finalBootCount = (finalSerial.match(/=== kmain boot start ===/g) || []).length;

  if (finalBootCount > initialBootCount) {
    throw new Error(`Restart triggered during continuous interaction (boots: ${initialBootCount} -> ${finalBootCount})`);
  }
}

async function testInactivityTriggersRestart(page) {
  await page.goto(TEST_URL, { waitUntil: "networkidle" });

  const getSerialLog = () => page.evaluate(() => window.__jt?.serial || "");
  const initialSerial = await getSerialLog();
  const initialBootCount = (initialSerial.match(/=== kmain boot start ===/g) || []).length;

  // Click to focus
  await page.click("#v86-embed", { position: { x: LOGICAL_W / 2, y: LOGICAL_H / 2 } });
  await page.waitForTimeout(100);

  // Wait 5+ seconds with absolutely NO interaction
  await page.waitForTimeout(5500);

  // Check if restart happened
  const finalSerial = await getSerialLog();
  const finalBootCount = (finalSerial.match(/=== kmain boot start ===/g) || []).length;

  if (finalBootCount <= initialBootCount) {
    throw new Error(`Restart NOT triggered after 5.5s inactivity (boots stayed at ${initialBootCount})`);
  }
}

async function testKeyboardInteractionPreventsRestart(page) {
  await page.goto(TEST_URL, { waitUntil: "networkidle" });

  const getSerialLog = () => page.evaluate(() => window.__jt?.serial || "");
  const initialSerial = await getSerialLog();
  const initialBootCount = (initialSerial.match(/=== kmain boot start ===/g) || []).length;

  // Click to focus
  await page.click("#v86-embed", { position: { x: LOGICAL_W / 2, y: LOGICAL_H / 2 } });
  await page.waitForTimeout(100);

  // Simulate continuous keydown/keyup every 1 second for 10 seconds
  const startTime = Date.now();
  while (Date.now() - startTime < 10000) {
    await page.keyboard.press("a");
    await page.waitForTimeout(1000);
  }

  // Check if restart happened
  const finalSerial = await getSerialLog();
  const finalBootCount = (finalSerial.match(/=== kmain boot start ===/g) || []).length;

  if (finalBootCount > initialBootCount) {
    throw new Error(`Restart triggered during continuous keyboard input (boots: ${initialBootCount} -> ${finalBootCount})`);
  }
}

main().catch(console.error);
