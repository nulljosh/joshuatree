/* v0.76.11: real fix for the CORS block found root-causing why the
   landing demo's browser-side satellite/map/geo fetches can never work
   (roadmap.md's "dock render report investigated..." entry has the full
   trace): ip-api.com/a.tile.opentopomap.org/mt0.google.com don't send
   Access-Control-Allow-Origin, so the guest kernel's HTTP requests --
   which v86's fetch-relay network mode turns into real browser fetch()
   calls from this page's own origin -- get rejected by the browser
   itself before they ever leave the page, for every real visitor, not
   just a sandboxed one.

   v86's own network adapter (Xb in libv86.js) already has a first-class
   `cors_proxy` option built for exactly this: it prepends the proxy URL
   to the real target and fetches THAT instead. Pointing it at this
   Worker (`/api/proxy?url=<target>`, a same-origin path, see embed.js's
   net_device config) makes the fetch happen server-to-server, which is
   never CORS-limited -- only a browser-to-cross-origin-server fetch is.

   Deliberately NOT a general-purpose open proxy: cors_proxy's own
   contract is "prepend this to any URL the guest asks for," which would
   let anyone hit this Worker with an arbitrary `url=` and have it fetch
   server-side on their behalf (a real SSRF/abuse vector) if left
   unchecked. isAllowedTarget() below is a strict allowlist of exactly
   the real hosts this kernel's own code (drivers/http.c's callers
   in kernel.c: geo_fetch, weather_fetch, wall_fetch) ever asks for,
   nothing else is ever proxied. */

// api.open-meteo.com was missing from this list from the day the proxy
// shipped: geo_fetch (ip-api.com) got through, weather_fetch's forecast
// request got this Worker's own 403 "Host not allowed", the kernel could
// not parse that as weather JSON, and the demo's Weather window and menu
// bar never showed a reading. The real cause of "Weather shows no data".
const ALLOWED_HOSTS = new Set([
  "ip-api.com",
  "api.open-meteo.com",
  "a.tile.opentopomap.org",
  "mt0.google.com",
]);

function isAllowedTarget(url) {
  if (url.protocol !== "http:" && url.protocol !== "https:") return false;
  return ALLOWED_HOSTS.has(url.hostname);
}

// Root cause of "Chat works, then freezes on weather" (owner report on the
// live landing demo): every fetch() in this file except handleStocks's own
// (which already carries AbortSignal.timeout(8000)) had no bound at all.
// v86's `cors_proxy` network mode (embed.js's net_device config) turns the
// guest kernel's one TCP connection into exactly one browser-side fetch()
// through this Worker, and buffers the WHOLE reply before ever handing a
// single byte back to the guest's virtual NIC -- there is no per-segment
// delivery for the guest's own ticks()-driven timeouts (drivers/net.c's
// tcp_get_timeout, WX_REPLY_TIMEOUT_TICKS in kernel.c) to fire against
// until this Worker's fetch() actually settles. An upstream host that
// accepts the TCP connection and then never sends a body (a real,
// ordinary failure mode, not a crafted one) leaves this fetch() pending
// for Cloudflare's own subrequest ceiling (tens of seconds to minutes),
// which the guest experiences as the whole desktop hanging: no reply ever
// reaches the virtual NIC for the guest's own bounded wait to time out
// against. Weather is the one most likely to hit this live (kernel.c's
// gui_run calls weather_fetch() unconditionally on first pass, and again
// every ten minutes, same shared geo_fetch/weather_fetch_inner path the
// menu bar and the Chat "weather" tool both read from -- see chat.h's
// weather tool handler), but the fix belongs here, in the one function
// every proxied request funnels through, not bolted onto weather alone.
const PROXY_FETCH_TIMEOUT_MS = 8000; // matches handleStocks's own bound

async function fetchWithTimeout(url, init) {
  try {
    return await fetch(url, { ...init, signal: AbortSignal.timeout(PROXY_FETCH_TIMEOUT_MS) });
  } catch (e) {
    // A timed-out or network-failed upstream still gets the guest a real,
    // fast, well-formed HTTP response instead of leaving the browser-side
    // fetch (and with it, v86's relay and the whole guest desktop) hanging
    // indefinitely. 504 is what a real gateway sends for exactly this.
    return new Response("Upstream timed out", { status: 504 });
  }
}

async function handleProxy(request, env) {
  const requestUrl = new URL(request.url);
  const target = requestUrl.searchParams.get("url");
  if (!target) return new Response("Missing url parameter", { status: 400 });

  let targetUrl;
  try {
    targetUrl = new URL(target);
  } catch (e) {
    return new Response("Invalid url parameter", { status: 400 });
  }

  if (targetUrl.hostname === "joshuatree.heyitsmejosh.com"
      && ["http:", "https:"].includes(targetUrl.protocol) && !targetUrl.port && !targetUrl.username && !targetUrl.password) {
    if (targetUrl.pathname === "/api/stocks") return handleStocks(targetUrl);
    if (targetUrl.pathname === "/api/quotes") return handleQuotes();
    if (targetUrl.pathname === "/api/deals") return handleDeals(request); // the guest's request rides the visitor's own browser fetch, so request.cf is the visitor
    // v1.6.12: kernel/chat_face.h's chat_face_load fetches Samantha's Chat
    // face frames (idle-0..5.jpg, talk-0..11.jpg) over the same plain-HTTP
    // stack every other guest request uses, from facehost=joshuatree.
    // heyitsmejosh.com (embed.js's cmdline). Those files are static assets
    // in this exact deploy (landing/face/*.jpg, wrangler.toml's [assets]
    // binding), so a plain GET, self-fetched over HTTPS, is a real,
    // narrowly-shaped exception -- same idea as /api/stocks etc above, just
    // serving a fixed asset instead of running a handler. Path is
    // constrained to exactly the file names chat_face.h ever builds
    // (face_fetch's "/face/" + kind + "-" + i + ".png"), nothing else on
    // this host is reachable through this branch.
    if (/^\/face(-joshua)?\/(idle|talk)-[0-9]{1,2}\.jpg$/.test(targetUrl.pathname) && request.method === "GET") {   // face-joshua: the portfolio face (kernel/chat_face.h face_fetch)
      // Read the frame from this deploy's own assets: a Worker fetching its
      // own hostname over the network gets Cloudflare's 522, so the guest's
      // face never loaded on the live site.
      const resp = await env.ASSETS.fetch(new Request("https://joshuatree.heyitsmejosh.com" + targetUrl.pathname));
      const headers = new Headers(resp.headers);
      headers.set("Access-Control-Allow-Origin", "*");
      // The kernel counts frames by asking for the next one until it fails,
      // so the first missing frame is the normal end of the clip. A 404 there
      // is a red "Failed to load resource" in every visitor's console; an
      // empty 200 is the same stop (face_fetch: got <= 0) without the noise.
      if (resp.status === 404) return new Response(null, { status: 200, headers });
      return new Response(resp.body, { status: resp.status, statusText: resp.statusText, headers });
    }
  }

  // 1.0.12: a tight, deliberately narrow exception for Chat talking to the
  // Turing project's own Ollama-compatible /api/chat (kernel/chat.h's
  // chat_send). turing.heyitsmejosh.com is NOT added to ALLOWED_HOSTS --
  // every other request to it, or to this exact path with a different
  // method/content-type/size, falls straight through to the ordinary
  // isAllowedTarget() check below and gets the same 403 any other
  // unlisted host gets. This branch only ever matches the one real shape
  // chat_send actually sends (POST, application/json, the guest's typed
  // message plus history, capped by CHAT_SAMANTHA_MAX_BODY well under
  // chat.h's own 6144-byte request buffer), and forwards exactly that:
  // method, body and content-type, nothing else guest-controlled. Every
  // other allowed host keeps the GET-only, no-body behaviour below
  // unchanged -- this is the one and only route that ever forwards a
  // guest-supplied body anywhere.
  //
  // 1.1.0: /api/pick joins /api/chat here, same host, same exact
  // guest-controlled shape (POST, application/json, a small body well
  // under the same cap) -- kernel/chat.h's new chat_pick asks it before
  // every chat_send, so it needs the identical narrow forwarding /api/chat
  // already has, nothing wider. Still not part of ALLOWED_HOSTS/
  // isAllowedTarget: every other path on this host, or either of these
  // two paths with a different method/content-type/size, still falls
  // through to the ordinary 403 below exactly as before this pass.
  //
  // 1.6.12: /api/speak joins the set -- drivers/speak.c's speak_text posts
  // the exact same shape (POST, application/json, {"text","format"}, well
  // under the cap) to the same llm_host chat_send/chat_pick already use.
  // Without this, Chat's speak_text always got the generic 403 below (no
  // ALLOWED_HOSTS entry for turing.heyitsmejosh.com), so speak: status=200
  // never happened on the real deployed page -- audio was silently dead in
  // production even with sb16 wired up correctly on the kernel side. The
  // response here is raw PCM, not JSON, but this block only inspects the
  // REQUEST's content-type/size and passes the response body straight
  // through untouched either way.
  const SAMANTHA_PATHS = new Set(["/api/chat", "/api/pick", "/api/speak"]);
  const isSamanthaChat = targetUrl.hostname === "turing.heyitsmejosh.com" && SAMANTHA_PATHS.has(targetUrl.pathname)
      && ["http:", "https:"].includes(targetUrl.protocol) && !targetUrl.port && !targetUrl.username && !targetUrl.password;
  const CHAT_SAMANTHA_MAX_BODY = 8192; // 8 KB, the task's own stated cap
  if (isSamanthaChat && request.method === "POST") {
    const contentType = (request.headers.get("content-type") || "").toLowerCase();
    if (!contentType.startsWith("application/json")) {
      return new Response("Unsupported content-type", { status: 415 });
    }
    const declaredLength = Number(request.headers.get("content-length") || "0");
    if (declaredLength > CHAT_SAMANTHA_MAX_BODY) {
      return new Response("Body too large", { status: 413 });
    }
    const bodyBuffer = await request.arrayBuffer();
    if (bodyBuffer.byteLength > CHAT_SAMANTHA_MAX_BODY) {
      return new Response("Body too large", { status: 413 });
    }
    const upstreamResponse = await fetchWithTimeout(targetUrl.toString(), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "User-Agent": "JoshuaTree-kernel-demo/1 (+https://joshuatree.heyitsmejosh.com)",
      },
      body: bodyBuffer,
    });
    const headers = new Headers(upstreamResponse.headers);
    headers.set("Access-Control-Allow-Origin", "*");
    headers.delete("content-security-policy");
    headers.delete("set-cookie");
    return new Response(upstreamResponse.body, {
      status: upstreamResponse.status,
      statusText: upstreamResponse.statusText,
      headers,
    });
  }

  if (!isAllowedTarget(targetUrl)) {
    return new Response("Host not allowed", { status: 403 });
  }

  // v0.76.15: real root cause of "stale wallpaper" surviving every
  // earlier fix (the CORS proxy itself, and the Settings-click
  // regression) -- found by reading libv86.js directly rather than
  // guessing again: `if (typeof window !== "undefined" && d.protocol ===
  // "http:" && window.location.protocol === "https:") d.protocol =
  // "https:";`. This page is served over HTTPS, so v86's own network
  // relay silently upgrades the guest's plain-HTTP geo request (real
  // kernel code: `http_get("ip-api.com", "/json/", 80, ...)`, no TLS
  // anywhere in this kernel) to `https://ip-api.com/...` before it ever
  // reaches this Worker -- a real, deliberate mixed-content fix in v86
  // itself, just wrong for this one target. ip-api.com's own real,
  // documented API policy requires a paid key for HTTPS access; the free
  // tier returns a real 403 over HTTPS (confirmed directly: the exact
  // same proxied request that returns 200 with real geo JSON over HTTP
  // returns 403 over HTTPS, live, in production, nothing else differs).
  // geo_fetch never succeeding meant `geo_have` never became true, which
  // meant kernel.c's own automatic `wall_fetch()` call (gated on
  // `geo_have`) never fired for a real browser visitor, ever -- despite
  // the CORS fix being real and despite the Settings-click regression
  // fix being real, this is the actual reason satellite tiles never
  // painted. This Worker fully controls the real outbound scheme
  // regardless of what the browser-relayed URL says, so it corrects it
  // here rather than trying to patch vendored, unowned v86 source.
  if (targetUrl.hostname === "ip-api.com") targetUrl.protocol = "http:";

  // The demo asking "where am I?" gets the visitor's location, not ours:
  // fetched from here, ip-api.com sees Cloudflare's data center (a
  // Vancouver PoP for a Langley visitor), so the weather and map flipped
  // to Vancouver. request.cf is Cloudflare's own lookup of the visitor's
  // connection, the same source /api/deals uses. Same ip-api field names
  // and numeric lat/lon, which is all kernel.c's geo_fetch reads. No cf
  // data (e.g. local dev) falls through to the real lookup below.
  const cf = request.cf;
  if (targetUrl.hostname === "ip-api.com" && targetUrl.pathname.startsWith("/json") && cf && cf.latitude && cf.longitude) {
    const lat = Number(cf.latitude), lon = Number(cf.longitude);
    if (Number.isFinite(lat) && Number.isFinite(lon)) {
      return new Response(JSON.stringify({ status: "success", city: cf.city || "", regionName: cf.region || "", country: cf.country || "", lat, lon, timezone: cf.timezone || "" }), {
        status: 200, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*", "Cache-Control": "no-store" },
      });
    }
  }

  // Only forward a plain GET with no guest-controlled headers/body: every
  // real caller here (geo_fetch/weather_fetch/wall_fetch) only ever issues
  // a bare GET, so there's nothing legitimate to lose by not forwarding
  // arbitrary request headers/methods through to the upstream host. This
  // is deliberately unconditional -- even a POST from the guest lands
  // here as a bare GET with no body, the one exception being the
  // Samantha-chat branch above, which returns before ever reaching this
  // line.
  const upstreamResponse = await fetchWithTimeout(targetUrl.toString(), {
    method: "GET",
    headers: { "User-Agent": "JoshuaTree-kernel-demo/1 (+https://joshuatree.heyitsmejosh.com)" },
  });

  const headers = new Headers(upstreamResponse.headers);
  headers.set("Access-Control-Allow-Origin", "*");
  headers.delete("content-security-policy");
  headers.delete("set-cookie");

  return new Response(upstreamResponse.body, {
    status: upstreamResponse.status,
    statusText: upstreamResponse.statusText,
    headers,
  });
}

// A small, bounded wire format keeps market JSON parsing out of the kernel.
const STOCK_SYMBOLS = ["AAPL", "MSFT", "GOOGL", "AMZN", "TSLA", "NVDA", "META", "NFLX"];
const STOCK_RANGES = [["1d", "5m"], ["5d", "30m"], ["1mo", "1d"], ["3mo", "1d"], ["1y", "1wk"]];
function stockWire(data, symbol) {
  const r = data?.chart?.result?.[0], m = r?.meta;
  const cents = v => typeof v === "number" && Number.isFinite(v) && v > 0 && v <= 100000 ? Math.round(v * 100) : 0;
  if (m?.symbol !== symbol || m.currency !== "USD") throw Error("Invalid quote");
  const price = cents(m.regularMarketPrice), prev = cents(m.previousClose ?? m.chartPreviousClose);
  const time = m.regularMarketTime;
  const raw = (r.indicators?.quote?.[0]?.close ?? []).map(cents).filter(Boolean);
  if (!price || !prev || !Number.isInteger(time) || time < 1 || time > 2147483647 || !raw.length) throw Error("Missing quote");
  const n = Math.min(raw.length, 64);
  const points = Array.from({length: n}, (_, i) => raw[n === 1 ? 0 : Math.round(i * (raw.length - 1) / (n - 1))]);
  return [price, prev, time, n, ...points].join(" ");
}
async function handleStocks(url) {
  const range = url.searchParams.get("range") ?? "0";
  if (!/^[0-4]$/.test(range)) return new Response("Invalid range", {status: 400});
  const [period, interval] = STOCK_RANGES[Number(range)];
  const lines = await Promise.all(STOCK_SYMBOLS.map(async symbol => {
    try {
      const response = await fetch(`https://query1.finance.yahoo.com/v8/finance/chart/${symbol}?range=${period}&interval=${interval}`, {
        headers: {"User-Agent": "Mozilla/5.0"},
        signal: AbortSignal.timeout(8000),
        cf: {cacheTtl: 60, cacheEverything: true},
      });
      if (!response.ok) throw Error("Quote unavailable");
      return stockWire(await response.json(), symbol);
    } catch { return "0"; }
  }));
  return new Response(lines.join("\n") + "\n", {headers: {
    "Content-Type": "text/plain", "Access-Control-Allow-Origin": "*", "Cache-Control": "no-store",
  }});
}

const WIRE = {"Content-Type": "text/plain; charset=utf-8", "Access-Control-Allow-Origin": "*", "Cache-Control": "no-store"};

// Epiphany's watchlist: price and previous close only, one line per symbol,
// so the kernel side stays a 40-line text parse. Kept under Cloudflare's
// 50-subrequest cap for one incoming request.
const QUOTE_SYMBOLS = ["AAPL", "MSFT", "GOOGL", "AMZN", "TSLA", "NVDA", "META", "NFLX", "AMD", "DIS", "JPM", "COIN", "SHOP", "UBER",
  "INTC", "CRM", "ORCL", "ADBE", "PYPL", "SQ", "ABNB", "SNOW", "PLTR", "BA", "GS", "BAC", "V", "MA", "WMT", "COST",
  "KO", "PEP", "NKE", "SBUX", "MCD", "XOM", "CVX", "PFE", "JNJ", "UNH"];
async function handleQuotes() {
  const lines = await Promise.all(QUOTE_SYMBOLS.map(async symbol => {
    try {
      const response = await fetch(`https://query1.finance.yahoo.com/v8/finance/chart/${symbol}?range=1d&interval=1d`, {
        headers: {"User-Agent": "Mozilla/5.0"}, signal: AbortSignal.timeout(8000), cf: {cacheTtl: 300, cacheEverything: true},
      });
      const m = (await response.json())?.chart?.result?.[0]?.meta;
      const cents = v => typeof v === "number" && v > 0 && v <= 100000 ? Math.round(v * 100) : 0;
      const price = cents(m?.regularMarketPrice), prev = cents(m?.previousClose ?? m?.chartPreviousClose);
      if (!price || !prev) throw Error("Missing quote");
      return `${symbol} ${price} ${prev}`;
    } catch { return `${symbol} 0 0`; }
  }));
  return new Response(lines.join("\n") + "\n", {headers: WIRE});
}

// Curbfind, local to the visitor: Cloudflare's own geo-IP city, slugged the
// way Craigslist names its areas, against Curbfind's real deal-ranked search
// (curbfind/worker). Unknown area falls back to Vancouver. Wire format:
// first line is the city, then `score|price|neighbourhood|title` rows.
const CURBFIND_API = "https://curbside-api.trommatic.workers.dev/api/search?sort=deal&city=";
async function handleDeals(request) {
  const clean = v => String(v ?? "").replace(/[^\x20-\x7e]/g, "").replace(/\|/g, " ").trim();
  let city = clean(request.cf?.city) || "Vancouver";
  const fetchCity = c => fetch(CURBFIND_API + encodeURIComponent(c.toLowerCase().replace(/[^a-z]/g, "")), {signal: AbortSignal.timeout(8000)});
  let res = await fetchCity(city);
  if (!res.ok) { city = "Vancouver"; res = await fetchCity(city); }
  if (!res.ok) return new Response("", {status: 502});
  const items = (await res.json()).items || [];
  const rows = items.filter(i => typeof i.price === "number" && i.price >= 25 && i.title).slice(0, 14).map(i =>
    `${Math.max(0, Math.min(10, Math.round(i.dealScore * 10)))}|$${i.price}|${clean(i.location) || city}|${clean(i.title).slice(0, 60)}`);
  return new Response([city, ...rows].join("\n") + "\n", {headers: WIRE});
}

// Dev-kit waitlist: one email, one timestamp, key = email so a repeat
// signup just overwrites its own row instead of growing the namespace.
// WAITLIST_MAX_BODY guards against someone posting a huge JSON blob;
// WAITLIST_MAX_EMAIL is RFC 5321's own address-length ceiling, generous
// for any real address.
const WAITLIST_MAX_BODY = 1024; // 1 KB
const WAITLIST_MAX_EMAIL = 254;
// A deliberately simple shape check, not full RFC 5322: one @, something
// on both sides, no whitespace, a dot in the domain part. Real validation
// (does the mailbox exist) only happens when the dev kit actually ships
// and someone emails the list -- this just keeps obvious garbage out.
const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;
const WAITLIST_JSON = { "Content-Type": "application/json", "Cache-Control": "no-store" };

async function handleWaitlistPost(request, env) {
  if (!env.WAITLIST) return new Response("Waitlist not configured", { status: 500 });
  const declaredLength = Number(request.headers.get("content-length") || "0");
  if (declaredLength > WAITLIST_MAX_BODY) return new Response("Body too large", { status: 413 });
  const bodyBuffer = await request.arrayBuffer();
  if (bodyBuffer.byteLength > WAITLIST_MAX_BODY) return new Response("Body too large", { status: 413 });

  let data;
  try {
    data = JSON.parse(new TextDecoder().decode(bodyBuffer));
  } catch {
    return new Response(JSON.stringify({ error: "Invalid JSON" }), { status: 400, headers: WAITLIST_JSON });
  }

  const email = typeof data?.email === "string" ? data.email.trim().toLowerCase() : "";
  if (!email || email.length > WAITLIST_MAX_EMAIL || !EMAIL_RE.test(email)) {
    return new Response(JSON.stringify({ error: "Invalid email" }), { status: 400, headers: WAITLIST_JSON });
  }

  // Only a first-time signup sends mail, so a repeat post can't be used to spam an address.
  const isNew = (await env.WAITLIST.get(email)) === null;
  await env.WAITLIST.put(email, new Date().toISOString());
  if (isNew) await sendWaitlistEmail(env, email);
  return new Response(JSON.stringify({ ok: true }), { status: 200, headers: WAITLIST_JSON });
}

const WAITLIST_MAIL_TEXT = [
  "You're on the list.",
  "",
  "Joshua Tree is an operating system written from scratch. It boots in your browser right now, with a dock full of apps and Samantha, the assistant who lives inside it.",
  "",
  "Try the live demo: https://joshuatree.heyitsmejosh.com",
  "Watch the 36 second ad: https://github.com/nulljosh/joshuatree/releases/download/1.8.8/joshua-tree-ad-v6.mp4",
  "",
  "The hardware is called Strata, a concept case for the OS. It is not for sale yet. When the dev kit is ready you'll get one email from me, and nothing else in between.",
  "",
  "Joshua",
].join("\n");
const WAITLIST_MAIL_HTML = `<div style="background:#F4EEE3;padding:32px 16px;font-family:-apple-system,BlinkMacSystemFont,'SF Pro Text','Helvetica Neue',Helvetica,Arial,sans-serif;color:#1A1814"><div style="max-width:520px;margin:0 auto"><p style="font-size:28px;font-weight:600;letter-spacing:-0.02em;margin:0 0 16px">You're on the list.</p><p style="font-size:16px;line-height:1.55;margin:0 0 16px">Joshua Tree is an operating system written from scratch. It boots in your browser right now, with a dock full of apps and Samantha, the assistant who lives inside it.</p><p style="margin:0 0 16px"><a href="https://joshuatree.heyitsmejosh.com" style="background:#B9542C;color:#F4EEE3;text-decoration:none;font-weight:600;padding:12px 22px;border-radius:999px;display:inline-block">Try the live demo</a></p><p style="margin:0 0 24px"><a href="https://github.com/nulljosh/joshuatree/releases/download/1.8.8/joshua-tree-ad-v6.mp4" style="color:#B9542C;font-weight:600">Watch the 36 second ad</a></p><p style="font-size:15px;line-height:1.55;color:#6F675C;margin:0 0 16px">The hardware is called Strata, a concept case for the OS. It is not for sale yet. When the dev kit is ready you'll get one email from me, and nothing else in between.</p><p style="font-size:15px;margin:0">Joshua</p></div></div>`;

// Resend confirmation. Never fails the signup: the address is already saved, a mail hiccup just logs.
async function sendWaitlistEmail(env, email) {
  if (!env.RESEND_API_KEY) { console.warn("waitlist mail skipped: RESEND_API_KEY not set"); return; }
  try {
    const res = await fetch("https://api.resend.com/emails", {
      method: "POST",
      headers: { Authorization: `Bearer ${env.RESEND_API_KEY}`, "Content-Type": "application/json" },
      body: JSON.stringify({
        from: env.MAIL_FROM || "Joshua Tree <noreply@epiphany.heyitsmejosh.com>",
        to: [email],
        subject: "You're on the Joshua Tree list",
        text: WAITLIST_MAIL_TEXT,
        html: WAITLIST_MAIL_HTML,
      }),
    });
    if (!res.ok) console.warn("waitlist mail failed", res.status, await res.text());
  } catch (e) { console.warn("waitlist mail error", String(e)); }
}

// list() pages at 1000 keys per call and this only ever needs a count, so
// this is fine up to a few thousand signups (a handful of calls, worst
// case). Ponytail: past ~50k rows, switch to a maintained counter (a
// single KV key incremented on put) instead of listing every key here.
async function handleWaitlistCount(env) {
  if (!env.WAITLIST) return new Response("Waitlist not configured", { status: 500 });
  let count = 0;
  let cursor;
  do {
    const page = await env.WAITLIST.list({ cursor });
    count += page.keys.length;
    cursor = page.list_complete ? undefined : page.cursor;
  } while (cursor);
  return new Response(JSON.stringify({ count }), { status: 200, headers: WAITLIST_JSON });
}

// v1.6.23: speech-to-text for Chat's push-to-talk (kernel/chat.h's
// chat_ptt_record posts here directly, over the same plain-HTTP path
// stocks.h/curbfind.h's http_get_timeout already reaches this exact host
// with -- no /api/proxy involved, this endpoint lives on this Worker
// itself). Runs Cloudflare Workers AI's Whisper (`@cf/openai/whisper`,
// free tier) over the posted clip and returns {"text":"..."}. 503 with no
// [ai] binding (a real, honest failure, not a hang) rather than crashing
// on env.AI.run of an undefined binding.
const LISTEN_MAX_BYTES = 1024 * 1024; // 1MB, the task's own stated cap

// Whisper's input contract wants a real audio container (wav/mp3/flac),
// not a bare PCM stream -- the kernel's sb16_record output has no header
// at all, so a plain byte array of raw samples fed straight to Whisper
// would be misread as garbage. This wraps it in the smallest real WAV
// header (44 bytes, PCM, mono, 8-bit, matching drivers/sb16.c's record
// format) when the caller didn't already send one (sniffed by "RIFF").
function wrapPcmAsWav(bytes, sampleRate, bitsPerSample, channels) {
  const blockAlign = (channels * bitsPerSample) / 8;
  const byteRate = sampleRate * blockAlign;
  const buf = new ArrayBuffer(44 + bytes.length);
  const view = new DataView(buf);
  const str = (off, s) => { for (let i = 0; i < s.length; i++) view.setUint8(off + i, s.charCodeAt(i)); };
  str(0, "RIFF"); view.setUint32(4, 36 + bytes.length, true); str(8, "WAVE");
  str(12, "fmt "); view.setUint32(16, 16, true); view.setUint16(20, 1, true);
  view.setUint16(22, channels, true); view.setUint32(24, sampleRate, true);
  view.setUint32(28, byteRate, true); view.setUint16(32, blockAlign, true);
  view.setUint16(34, bitsPerSample, true);
  str(36, "data"); view.setUint32(40, bytes.length, true);
  new Uint8Array(buf, 44).set(bytes);
  return new Uint8Array(buf);
}

const JSON_CORS = { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*", "Cache-Control": "no-store" };

async function handleListen(request, env) {
  if (request.method !== "POST") return new Response("Method not allowed", { status: 405 });
  if (!env.AI) return new Response(JSON.stringify({ error: "speech recognition is not configured" }), { status: 503, headers: JSON_CORS });

  // Per-IP rate limit: the Workers "ratelimit" binding (wrangler.toml's
  // LISTEN_RATE_LIMITER) when it exists; this repo had none before this
  // pass, and dev/CI environments never define it, so this degrades to
  // "no limit locally" rather than crashing on a missing binding.
  if (env.LISTEN_RATE_LIMITER) {
    const ip = request.headers.get("cf-connecting-ip") || "unknown";
    const { success } = await env.LISTEN_RATE_LIMITER.limit({ key: ip });
    if (!success) return new Response(JSON.stringify({ error: "too many requests" }), { status: 429, headers: JSON_CORS });
  }

  const declaredLength = Number(request.headers.get("content-length") || "0");
  if (declaredLength > LISTEN_MAX_BYTES) return new Response(JSON.stringify({ error: "audio too large" }), { status: 413, headers: JSON_CORS });
  const bodyBuffer = await request.arrayBuffer();
  if (bodyBuffer.byteLength === 0) return new Response(JSON.stringify({ error: "empty audio" }), { status: 400, headers: JSON_CORS });
  if (bodyBuffer.byteLength > LISTEN_MAX_BYTES) return new Response(JSON.stringify({ error: "audio too large" }), { status: 413, headers: JSON_CORS });

  let bytes = new Uint8Array(bodyBuffer);
  const isWav = bytes.length > 4 && bytes[0] === 0x52 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x46; // "RIFF"
  if (!isWav) bytes = wrapPcmAsWav(bytes, 16000, 8, 1); // drivers/sb16.c's sb16_record format: 8-bit unsigned mono, 16kHz

  try {
    const result = await env.AI.run("@cf/openai/whisper", { audio: Array.from(bytes) });
    const text = result && typeof result.text === "string" ? result.text.slice(0, 2000) : "";
    return new Response(JSON.stringify({ text }), { status: 200, headers: JSON_CORS });
  } catch (e) {
    return new Response(JSON.stringify({ error: "speech recognition failed" }), { status: 502, headers: JSON_CORS });
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/api/stocks") return handleStocks(url);
    if (url.pathname === "/api/quotes") return handleQuotes();
    if (url.pathname === "/api/deals") return handleDeals(request);
    if (url.pathname === "/api/listen") return handleListen(request, env);
    if (url.pathname === "/api/proxy") {
      return handleProxy(request, env);
    }
    if (url.pathname === "/api/waitlist" && request.method === "POST") {
      return handleWaitlistPost(request, env);
    }
    if (url.pathname === "/api/waitlist/count" && request.method === "GET") {
      return handleWaitlistCount(env);
    }
    return env.ASSETS.fetch(request);
  },
};

// Exported for tools/checks/worker-proxy-check.mjs, a real Cloudflare
// Worker isn't spun up in this CI; the allowlist logic and request/
// response handling are still real, plain JS testable directly under Node.
export { isAllowedTarget, handleProxy, ALLOWED_HOSTS };

export { stockWire, handleStocks };
export { handleWaitlistPost, handleWaitlistCount };
export { handleListen, wrapPcmAsWav, LISTEN_MAX_BYTES };
