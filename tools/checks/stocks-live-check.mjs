import assert from 'node:assert/strict';
import {readFileSync, writeFileSync, mkdtempSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {execFileSync} from 'node:child_process';
import worker, {stockWire, handleStocks, handleProxy} from '../../worker.js';
const fixture = symbol => ({chart:{result:[{meta:{symbol,currency:'USD',regularMarketPrice:337.02,previousClose:339.75,regularMarketTime:1790193601},indicators:{quote:[{close:[339.1,null,338,337.02]}]}}]}});
assert.equal(stockWire(fixture('AAPL'), 'AAPL'), '33702 33975 1790193601 3 33910 33800 33702');
assert.throws(() => stockWire(fixture('MSFT'), 'AAPL'));
const invalid = fixture('AAPL'); invalid.chart.result[0].meta.regularMarketPrice = null;
assert.throws(() => stockWire(invalid, 'AAPL'));
const long = fixture('AAPL'); long.chart.result[0].indicators.quote[0].close = Array.from({length:390},(_,i)=>100+i);
const sampled = stockWire(long,'AAPL').split(' ').map(Number);
assert.equal(sampled[3],64); assert.equal(sampled[4],10000); assert.equal(sampled.at(-1),48900);
const original = globalThis.fetch;
let calls = 0;
globalThis.fetch = async (url) => {
  calls++;
  assert.match(url, /^https:\/\/query1.finance.yahoo.com\/v8\/finance\/chart\/[A-Z]+\?range=/);
  const symbol = new URL(url).pathname.split('/').at(-1);
  return symbol === 'MSFT' ? new Response('limited',{status:429}) : Response.json(fixture(symbol));
};
let wire;
try {
  assert.equal((await handleStocks(new URL('https://example.com/api/stocks?range=evil'))).status,400);
  assert.equal(calls,0);
  const target='http://joshuatree.heyitsmejosh.com/api/stocks?range=0';
  const response=await handleProxy(new Request('https://joshuatree.heyitsmejosh.com/api/proxy?url='+encodeURIComponent(target)));
  wire=await response.text();
  assert.equal(calls,8); assert.equal(wire.trim().split('\n')[1],'0');
  assert.equal(response.headers.get('cache-control'),'no-store');
  const direct=await worker.fetch(new Request(target),{});
  assert.equal(await direct.text(),wire);
  for (const url of ['http://evil.example/api/stocks','http://joshuatree.heyitsmejosh.com/other','http://joshuatree.heyitsmejosh.com:123/api/stocks']) {
    assert.equal((await handleProxy(new Request('https://example.com/api/proxy?url='+encodeURIComponent(url)))).status,403);
  }
} finally { globalThis.fetch=original; }
// Compile the actual parser and fetch routine, not a handwritten copy.
const source=readFileSync(new URL('../../kernel/stocks.h',import.meta.url),'utf8').split('static void stocks_format_price')[0];
const dir=mkdtempSync(tmpdir()+'/jt-stocks-');
try {
writeFileSync(dir+'/test.c',`#include <assert.h>
#include <string.h>
static const char *reply; static int status=200, online=1;
static int net_init(unsigned ip){assert(ip==0x0A00020F);return online;}
static unsigned int ticks(void){return 7000;}
static int http_last_status(void){return status;}
static int http_get_timeout(const char*h,const char*p,int port,char*b,unsigned cap,unsigned timeout){
 assert(online); assert(!strcmp(h,"joshuatree.heyitsmejosh.com")); assert(!strcmp(p,"/api/stocks?range=0")); assert(port==80 && timeout==1000);
 unsigned n=strlen(reply); assert(n<cap); memcpy(b,reply,n); return n;
}
${source}
int main(void){
 reply=${JSON.stringify(wire)}; stocks_fetch(0);
 assert(stocks_entries[0].price_x100==33702 && stocks_entries[0].change_x100==-273);
 assert(stx_data[0][0].n==3 && !stx_data[0][0].stale);
 assert(stocks_entries[1].price_x100==0 && stx_data[0][1].stale);
 assert(stx_data[0][7].points[2]==33702);
 const char *bad[]={"", "1", "1 2 3 0\\n", "1 2 3 65 1\\n", "2147483648 2 3 1 1\\n", "1 2 3 1 0\\n", "1 2 3 1 -1\\n", "1 2 3 2 1\\n", "1 2 3 1 1 2\\n", "1 2 3 1 1"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){assert(!stx_parse_row(bad[i],0,0));assert(stocks_entries[0].price_x100==33702);}
 reply="0\\n"; stocks_fetch(0); assert(stx_data[0][0].stale && stocks_entries[0].price_x100==33702);
 status=503; stocks_fetch(0); assert(stx_data[0][0].stale);
 online=0; status=200; stocks_fetch(0); assert(stx_data[0][0].stale);
 assert(stx_refresh_tick==7000);
}
`);
execFileSync('clang',['-fsanitize=address,undefined',dir+'/test.c','-o',dir+'/test']);
execFileSync(dir+'/test');
} finally {rmSync(dir,{recursive:true,force:true});}
assert.match(readFileSync(new URL('../../kernel/stocks.h',import.meta.url),'utf8'),/get_key_or_click_until\(stx_refresh_tick \+ 6000\)/);
console.log('PASS: real Worker routes, quotes/history, partial failures, strict kernel parser, stale retention and refresh');
