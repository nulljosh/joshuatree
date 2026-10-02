// shots.mjs: headless boot screenshots for the portfolio face QA.
// node shots.mjs <outdir>   (run from ~/Documents/Code/joshuatree so playwright resolves)
import { chromium, devices } from 'playwright';
const out = process.argv[2] || '.';
const cases = [
  ['desktop-portfolio', 'https://joshuatree.heyitsmejosh.com/?full&portfolio&samantha', { viewport: { width: 1400, height: 900 } }],
  ['desktop-samantha-control', 'https://joshuatree.heyitsmejosh.com/?samantha', { viewport: { width: 1400, height: 900 } }],
  ['phone-portfolio', 'https://heyitsmejosh.com/', { ...devices['iPhone 14'] }],
];
const browser = await chromium.launch();
for (const [name, url, ctxOpts] of cases) {
  const ctx = await browser.newContext(ctxOpts);
  const page = await ctx.newPage();
  await page.goto(url, { waitUntil: 'load' });
  for (const t of [45, 80]) {
    await page.waitForTimeout(t === 45 ? 45000 : 35000);
    await page.screenshot({ path: `${out}/${name}-${t}s.png` });
  }
  await ctx.close();
}
await browser.close();
console.log('shots done');
