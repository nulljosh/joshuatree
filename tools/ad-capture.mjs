// Deterministic capture of landing/ad/index.html: steps window.__ad.render(t) per frame.
// usage: node tools/ad-capture.mjs [outDir] [workers] [only-times-csv-for-stills]
import {chromium} from 'playwright';
import {createServer} from 'node:http';
import {readFileSync,mkdirSync,existsSync} from 'node:fs';
import {join,extname,resolve} from 'node:path';
const root=resolve(new URL('..',import.meta.url).pathname,'landing/..');
const out=process.argv[2]||'frames';const workers=+(process.argv[3]||6);const stillsCsv=process.argv[4];
const types={'.html':'text/html','.js':'text/javascript','.svg':'image/svg+xml','.webp':'image/webp','.jpg':'image/jpeg','.stl':'model/stl','.png':'image/png'};
const srv=createServer((q,r)=>{const p=join(root,decodeURIComponent(q.url.split('?')[0]));
 if(!existsSync(p)){r.writeHead(404);return r.end()}r.writeHead(200,{'content-type':types[extname(p)]||'application/octet-stream'});r.end(readFileSync(p))});
await new Promise(r=>srv.listen(0,r));const url=`http://localhost:${srv.address().port}/landing/ad/index.html?t=0`;
mkdirSync(out,{recursive:true});
const args=['--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist'];
async function worker(frames){
 const b=await chromium.launch({headless:true,args});const pg=await b.newPage({viewport:{width:1920,height:1080}});
 await pg.goto(url);await pg.evaluate(()=>window.__ad.ready);
 for(const [i,t] of frames){await pg.evaluate(t=>window.__ad.render(t),t);
  await pg.screenshot({path:join(out,stillsCsv?`still-${i}.png`:`f${String(i).padStart(4,'0')}.png`)})}
 await b.close();
}
const fps=30,dur=30;let all=[];
if(stillsCsv)all=stillsCsv.split(',').map((t,i)=>[i,+t]);else for(let i=0;i<fps*dur;i++)all.push([i,i/fps]);
const buckets=Array.from({length:workers},()=>[]);all.forEach((f,i)=>buckets[i%workers].push(f));
await Promise.all(buckets.map(worker));srv.close();console.log('frames',all.length);
