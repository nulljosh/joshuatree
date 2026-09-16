// Idle-tour QA: load the demo, touch NOTHING, and confirm the tour opens an app on its own.
import { chromium } from 'playwright';
const b = await chromium.launch();
const p = await (await b.newContext({viewport:{width:1280,height:900}})).newPage();
await p.goto(process.argv[2] || 'http://localhost:8899', {waitUntil:'load'});
await p.waitForFunction(() => { const c=document.getElementById('screen_canvas'); return c && c.width>=640; }, null, {timeout:60000});
const fp = () => p.evaluate(() => { const c=document.getElementById('screen_canvas'); const d=c.getContext('2d').getImageData(0,0,c.width,c.height).data; let l=0; for(let i=0;i<d.length;i+=4*29) if(d[i]>200&&d[i+1]>200&&d[i+2]>200) l++; return l; });
await p.waitForTimeout(4000);
const desktop = await fp();
// tour arms 6s after graphical mode, first app opens ~1.5s later; sample for 14s and keep the extreme
let opened = false, seen = [];
for (let t = 0; t < 14; t++) { await p.waitForTimeout(1000); const v = await fp(); seen.push(v); if (v > desktop*2+50 || v < desktop/2) opened = true; }
console.log('desktop bright px:', desktop, ' samples:', seen.join(','));
console.log(opened ? 'PASS: idle tour opened an app with no interaction' : 'FAIL: nothing opened on its own');
await p.screenshot({path:'/tmp/tour.png'});
await b.close(); process.exit(opened?0:1);
