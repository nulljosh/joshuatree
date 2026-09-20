// Regression check for progress.svg's (and its landing-page twin's) text
// layout: no two real <text> element boxes may overlap on screen.
//
// v53 found three real overlap bugs, all only visible by actually
// rendering the chart, not by reading the generator's coordinate math:
//   1. the rotated y-axis title ("Lines of code") landing on top of a
//      numeric tick label -- both used to start at the same x=2, and the
//      title's real rotated footprint (its string length, not its 8px
//      font-size) was never accounted for.
//   2. x-axis date labels crowding together at the right edge -- the old
//      code force-showed the final unique date regardless of how close it
//      landed to whatever label the gap scan had already kept.
//   3. the bottom caption crowding the x-axis date label directly above
//      it, an 8px baseline gap for two stacked font-size-10 rows.
// tools/gen/progress.sh fixed all three (dynamic pad_l with a separate
// title/tick column, replace-not-append for the crowded final date
// label, a wider pad_b). This check is discriminating, not cosmetic:
// verified by hand before landing this check -- temporarily revert any
// one of those three fixes and regenerate, and this fails; restore it
// and this passes again.
//
// Uses real getBoundingClientRect() on the actual rendered <text>
// elements (Chromium's own layout engine, real font metrics, and it
// automatically accounts for the y-axis title's rotate(-90) transform)
// instead of a hand-approximated glyph-width guess, so there is no
// hardcoded pixel model to drift out of sync with the generator, and it
// keeps working as real data changes shape over time, not just against
// today's snapshot.
//
// Also asserts the polyline and its dots never render above the plot's
// own top gridline -- a second real bug found live while verifying the
// text-overlap fix: commit 6c169c0 temporarily added ~53,000 generated
// lines to kernel/icon_art.h before the very next commit shrank it back
// down, and since the chart's y-scale (max_v) was read straight off the
// FINAL sampled point, that intermediate spike rendered off the top of
// the chart entirely. Fixed by excluding icon_art.h the same way
// wallpaper.h/editor_fonts.h/vgafont.h already were (a generated data
// blob, not hand-authored code), but a real render bug like that is
// exactly the kind of thing worth a standing check too.
//
// Usage: node tools/checks/progress-svg-layout-check.mjs
import { chromium } from 'playwright';
import path from 'path';
import { fileURLToPath } from 'url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

function overlaps(a, b) {
  return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

async function textRects(page, scopeSelector) {
  return page.evaluate((sel) => {
    const root = sel ? document.querySelector(sel) : document;
    if (!root) return null;
    return Array.from(root.querySelectorAll('text'))
      .map((el) => {
        const r = el.getBoundingClientRect();
        const content = (el.textContent || '').trim();
        return { content, left: r.left, top: r.top, right: r.right, bottom: r.bottom };
      })
      .filter((t) => t.content.length > 0 && t.right > t.left && t.bottom > t.top);
  }, scopeSelector);
}

function findOverlaps(rects) {
  const found = [];
  for (let i = 0; i < rects.length; i++) {
    for (let j = i + 1; j < rects.length; j++) {
      if (overlaps(rects[i], rects[j])) found.push([rects[i].content, rects[j].content]);
    }
  }
  return found;
}

// The vertical left axis line's own real height IS the plot's real
// top/bottom bound (pad_t to pad_t+plot_h) -- reading it straight off
// the rendered axis, rather than re-deriving pad_t/plot_h by hand, means
// this stays correct if those constants ever change.
async function plotBounds(page, scopeSelector) {
  return page.evaluate((sel) => {
    const root = sel ? document.querySelector(sel) : document;
    if (!root) return null;
    const axisLine = Array.from(root.querySelectorAll('line')).find(
      (l) => l.getAttribute('x1') === l.getAttribute('x2')
    );
    if (!axisLine) return null;
    const r = axisLine.getBoundingClientRect();
    return { top: r.top, bottom: r.bottom };
  }, scopeSelector);
}

async function findOutOfBounds(page, scopeSelector, bounds) {
  const TOLERANCE = 8; // real stroke/marker overhang, not a rendering bug
  return page.evaluate(({ sel, bounds, tolerance }) => {
    const root = sel ? document.querySelector(sel) : document;
    if (!root) return [];
    // Only the real value marks -- the interactive version's invisible,
    // deliberately oversized hit-target circles (progress-hit) are meant
    // to stick out past the plot near the top/bottom points and aren't a
    // rendering defect.
    const marks = Array.from(root.querySelectorAll('polyline, circle:not(.progress-hit)'));
    const bad = [];
    for (const el of marks) {
      const r = el.getBoundingClientRect();
      if (r.top < bounds.top - tolerance || r.bottom > bounds.bottom + tolerance) {
        bad.push(`${el.tagName} top=${Math.round(r.top)} bottom=${Math.round(r.bottom)} (plot is ${Math.round(bounds.top)}-${Math.round(bounds.bottom)})`);
      }
    }
    return bad;
  }, { sel: scopeSelector, bounds, tolerance: TOLERANCE });
}

const browser = await chromium.launch();
const problems = [];

// 1. The plain static file README.md embeds -- load it directly as its
//    own document, same as an <img src="progress.svg"> would render it.
{
  const page = await browser.newPage({ viewport: { width: 900, height: 400 } });
  await page.goto('file://' + path.join(ROOT, 'progress.svg'));
  const rects = await textRects(page, null);
  if (!rects || rects.length === 0) {
    problems.push('progress.svg: no <text> elements found (missing or empty?)');
  } else {
    for (const [a, b] of findOverlaps(rects)) {
      problems.push(`progress.svg: ${JSON.stringify(a)} overlaps ${JSON.stringify(b)}`);
    }
  }
  const bounds = await plotBounds(page, null);
  if (!bounds) {
    problems.push('progress.svg: could not find the y-axis line to measure plot bounds');
  } else {
    for (const bad of await findOutOfBounds(page, null, bounds)) {
      problems.push(`progress.svg: a data mark renders outside the plot area: ${bad}`);
    }
  }
  await page.close();
}

// 2. The real interactive version inlined into landing/index.html --
//    load the actual page (exercises the exact markup a visitor gets,
//    including the #progress-chart-live scoped CSS) and scope the query
//    to the chart itself.
{
  const page = await browser.newPage({ viewport: { width: 900, height: 1400 } });
  page.on('pageerror', () => {}); // the v86 emulator assets 404 under file://, unrelated to this check
  await page.goto('file://' + path.join(ROOT, 'landing/index.html'));
  await page.waitForTimeout(300);
  const chart = await page.$('#progress-chart-live');
  if (!chart) {
    problems.push('landing/index.html: #progress-chart-live not found -- run tools/gen/progress.sh');
  } else {
    const rects = await textRects(page, '#progress-chart-live');
    if (!rects || rects.length === 0) {
      problems.push('landing/index.html: interactive chart has no <text> elements');
    } else {
      for (const [a, b] of findOverlaps(rects)) {
        problems.push(`landing/index.html (interactive chart): ${JSON.stringify(a)} overlaps ${JSON.stringify(b)}`);
      }
    }
    const bounds = await plotBounds(page, '#progress-chart-live');
    if (!bounds) {
      problems.push('landing/index.html: could not find the y-axis line to measure plot bounds');
    } else {
      for (const bad of await findOutOfBounds(page, '#progress-chart-live', bounds)) {
        problems.push(`landing/index.html (interactive chart): a data mark renders outside the plot area: ${bad}`);
      }
    }
  }
  await page.close();
}

await browser.close();

if (problems.length) {
  console.log('FAIL:');
  for (const p of problems) console.log('  ' + p);
  process.exit(1);
}
console.log('PASS: no overlapping <text> elements and no data mark renders outside the plot area, in progress.svg or its landing-page twin');
