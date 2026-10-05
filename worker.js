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
    if (targetUrl.pathname === "/api/books") return handleBooks();
    if (targetUrl.pathname === "/api/lexly") return handleLexly(targetUrl);
    if (targetUrl.pathname === "/api/hikko") return handleHikko();
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
    // 2.0.0: the ring-3 Samantha's per-sentence SYS_HTTP_GET /api/speak?t=...
    // goes to the fixed Worker host, so in the demo it arrives HERE (through
    // the browser relay), not at the top-level /api/speak route. Without this
    // it fell through to isAllowedTarget and got the generic 403.
    if (targetUrl.pathname === "/api/speak" && request.method === "GET") return handleSpeakGet(targetUrl, request, env);
    if ((/^\/face(-joshua)?\/(idle|talk)-[0-9]{1,2}\.jpg$/.test(targetUrl.pathname) || targetUrl.pathname === "/face/hd.jpg") && request.method === "GET") {   // face-joshua: the portfolio face
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

// Bookrank, the public ranked shelf from the real app's REST API
// (bookrank.heyitsmejosh.com/api/books, a read-only GET). The kernel gets
// plain text so its parser stays small: first line is the shelf total, then
// one `rank|rating x100|reviews|badge|title|author|notes` row per book. A
// field never holds a pipe, a newline or a non-ASCII byte, whatever the
// upstream sends. Per-account chapter summaries are not public, so the
// notes line is the summary the OS can show.
const BOOKRANK_API = "https://bookrank.heyitsmejosh.com/api/books?section=ranked&limit=12";
const wireText = (v, max) => String(v ?? "").normalize("NFKD").replace(/[\u0300-\u036f]/g, "").replace(/[\u2018\u2019]/g, "'").replace(/[\u201c\u201d]/g, '"')
  .replace(/[^\x20-\x7e]/g, " ").replace(/\|/g, " ").replace(/\s+/g, " ").trim().slice(0, max);
function bookrankWire(data) {
  const list = Array.isArray(data?.results) ? data.results : [];
  const rows = list.filter(b => b && typeof b.title === "string" && wireText(b.title, 60)).slice(0, 12).map((b, i) => {
    const rank = Number.isInteger(b.rank) && b.rank > 0 && b.rank < 1000 ? b.rank : i + 1;
    const rating = typeof b.rating === "number" && b.rating >= 0 && b.rating <= 5 ? Math.round(b.rating * 100) : 0;
    const badge = Array.isArray(b.badges) && typeof b.badges[0] === "string" ? wireText(b.badges[0], 24) : "";
    return [rank, rating, wireText(b.reviewCount, 24), badge, wireText(b.title, 60), wireText(b.author, 40), wireText(b.notes, 140)].join("|");
  });
  const total = Number.isInteger(data?.total) && data.total >= 0 && data.total < 100000 ? data.total : rows.length;
  return [total, ...rows].join("\n") + "\n";
}
async function handleBooks() {
  let res;
  try { res = await fetch(BOOKRANK_API, {signal: AbortSignal.timeout(8000), cf: {cacheTtl: 300, cacheEverything: true}}); }
  catch { return new Response("", {status: 502}); }
  if (!res.ok) return new Response("", {status: 502});
  let data;
  try { data = await res.json(); } catch { return new Response("", {status: 502}); }
  const wire = bookrankWire(data);
  if (wire.split("\n").length < 3) return new Response("", {status: 502}); // no usable row: let the OS keep its samples
  return new Response(wire, {headers: WIRE});
}

// Tonchi (was Lexly), "learn anything": the real app's public course packs.
// The App Store rename is not applied and tonchi.heyitsmejosh.com does not
// exist yet, so the address, the /api/lexly route and the LEXLY_* names stay
// as they are: a URL is not a label. Flip LEXLY_SITE when the new host answers.
// (lexly.heyitsmejosh.com/content/catalog.json and /content/courses/<id>.json,
// static read-only files). Two wires for the Tonchi app:
//   /api/lexly            first line the course count, then `id|name|category`
//   /api/lexly?c=<id>     first line the question count, then
//                         `answer index 0-3|question|choice0|choice1|choice2|choice3`
// The OS font draws ASCII only, so a field is kept only if it survives as
// ASCII (accents are stripped, a few letters transliterated). A question
// with any field that does not, or that holds a pipe, or whose four choices
// are not four distinct strings that include the answer, is dropped, never
// bent into something else. Courses are an allowlist: those whose packs keep
// at least 12 readable questions under these rules (checked against the
// real packs when this shipped, 72 of them). Left out: Japanese, Chinese,
// Korean, Russian, Arabic, Hindi, Greek, Hebrew, Ukrainian, Persian,
// Bulgarian, Macedonian and Yiddish, which are written in scripts the font
// cannot draw, Klingon (two readable questions), and the short math courses
// (arithmetic, algebra, geometry, trigonometry, statistics, linear algebra,
// logic, Pre-Calculus 11), which keep only four to six four-choice questions
// that survive. Accented languages (Polish, Czech, Turkish, Vietnamese and
// the like) are offered with their accents stripped.
const LEXLY_SITE = "https://lexly.heyitsmejosh.com";
const LEXLY_COURSES = [
  "spanish", "french", "german", "italian", "portuguese", "dutch", "turkish", "polish", "swedish",
  "vietnamese", "indonesian", "czech", "danish", "finnish", "norwegian", "hungarian", "romanian", "catalan",
  "croatian", "serbian", "slovak", "lithuanian", "estonian", "icelandic", "galician", "slovenian",
  "javascript", "python", "rust", "cpp", "java", "go", "sql", "computers", "ai", "hardware",
  "reverse_engineering", "ios_internals", "git_terminal", "html_css", "swift", "dsa", "servers", "databases",
  "c_lang", "typescript", "csharp", "kotlin", "ruby", "php", "bash", "assembly", "haskell", "lua",
  "calculus", "discrete_math", "fieldbook", "physics", "chemistry", "biology", "astronomy", "astrophysics",
  "anthropology", "anatomy", "physiology", "precalc12", "anatomy12", "chess", "music_theory",
  "music_history", "world_history", "geography"
];
const LEXLY_MAX_COURSES = 80, LEXLY_MAX_Q = 20, LEXLY_Q_CHARS = 90, LEXLY_C_CHARS = 60, LEXLY_MAX_BYTES = 5800;
const LEXLY_TYPES = new Set(["mathChoice", "translation", "cloze"]);
const LEXLY_FOLD = {"ß": "ss", "æ": "ae", "Æ": "AE", "ø": "o", "Ø": "O", "ł": "l", "Ł": "L", "đ": "d", "Đ": "D", "ı": "i", "œ": "oe", "Œ": "OE", "ð": "d", "þ": "th"};
// Plain ASCII, one line, at most max characters, or null when the text cannot
// survive as it is (non-ASCII left over, a pipe, empty, too long). With cut set,
// too long is cut instead (a course name is a label; a question never is).
function lexlyText(v, max, cut = false) {
  if (typeof v !== "string") return null;
  const s = v.replace(/[ßæÆøØłŁđĐıœŒðþ]/g, c => LEXLY_FOLD[c])
    .normalize("NFKD").replace(/[̀-ͯ]/g, "").replace(/[¿¡]/g, "")
    .replace(/[‘’]/g, "'").replace(/[“”]/g, '"').replace(/[–—]/g, "-").replace(/…/g, "...")
    .replace(/[\s ]+/g, " ").trim();
  if (!s || (s.length > max && !cut) || /[^\x20-\x7e]/.test(s) || s.includes("|")) return null;
  return s.slice(0, max).trim();
}
function lexlyQuestions(pack) {
  const out = [];
  for (const u of Array.isArray(pack?.units) ? pack.units : [])
    for (const l of Array.isArray(u?.lessons) ? u.lessons : [])
      for (const e of Array.isArray(l?.exercises) ? l.exercises : []) {
        if (!e || !LEXLY_TYPES.has(e.type) || !Array.isArray(e.choices) || e.choices.length !== 4) continue;
        const q = lexlyText(e.question, LEXLY_Q_CHARS), a = lexlyText(e.answer, LEXLY_C_CHARS);
        const c = e.choices.map(x => lexlyText(x, LEXLY_C_CHARS));
        if (!q || !a || c.includes(null) || new Set(c).size !== 4) continue;
        const idx = c.indexOf(a);
        if (idx >= 0) out.push([idx, q, ...c].join("|"));
      }
  return out;
}
function lexlyCourseWire(catalog) {
  const seen = new Set(), rows = []; let bytes = 8;
  for (const [cat, group] of Object.entries(catalog?.categories ?? {}))
    for (const s of Array.isArray(group?.subjects) ? group.subjects : []) {
      const id = s?.id, name = lexlyText(s?.name, 40, true), kind = lexlyText(cat, 12);
      if (typeof id !== "string" || !LEXLY_COURSES.includes(id) || seen.has(id) || !name || !kind) continue;
      seen.add(id);
      const row = [id, name, kind].join("|");
      if (rows.length < LEXLY_MAX_COURSES && bytes + row.length + 1 <= LEXLY_MAX_BYTES) { rows.push(row); bytes += row.length + 1; }
    }
  return [rows.length, ...rows].join("\n") + "\n";
}
function lexlyQuestionWire(pack) {
  const all = lexlyQuestions(pack);
  // An even spread through the course, not the first twenty, so every unit is in the drill.
  const pick = all.length <= LEXLY_MAX_Q ? all : Array.from({length: LEXLY_MAX_Q}, (_, i) => all[Math.floor(i * all.length / LEXLY_MAX_Q)]);
  const rows = []; let bytes = 8;
  for (const r of pick) { if (bytes + r.length + 1 > LEXLY_MAX_BYTES) break; rows.push(r); bytes += r.length + 1; }
  return [rows.length, ...rows].join("\n") + "\n";
}
async function handleLexly(url) {
  const course = url.searchParams.get("c");
  if (course !== null && !(/^[a-z0-9_]{1,24}$/.test(course) && LEXLY_COURSES.includes(course))) return new Response("", {status: 404});
  let res;
  try { res = await fetch(LEXLY_SITE + (course === null ? "/content/catalog.json" : `/content/courses/${course}.json`), {signal: AbortSignal.timeout(8000), cf: {cacheTtl: 3600, cacheEverything: true}}); }
  catch { return new Response("", {status: 502}); }
  if (!res.ok) return new Response("", {status: 502});
  let data;
  try { data = await res.json(); } catch { return new Response("", {status: 502}); }
  const wire = course === null ? lexlyCourseWire(data) : lexlyQuestionWire(data);
  if (wire.split("\n").length < (course === null ? 3 : 6)) return new Response("", {status: 502}); // too few usable rows: let the OS keep its Spanish set
  return new Response(wire, {headers: WIRE});
}

// Hikko, the idea forum: the real app's public, read-only post feed
// (hikko.heyitsmejosh.com/api/posts, no login, no token). The list call
// (?limit=12) has the title, score and category; the body and the plan only come
// from the one-post call (?id=), so the top twelve are read in parallel. Wire:
//   first line the row count, then `votes|category|title|text|plan`
// Text is ASCII, one line, cut to a cap with "..." and never holds a pipe. A
// post with no readable title is dropped. A post whose body call fails keeps its
// row with empty text, so the OS still shows the idea. No write path: this
// route never sends a vote, a post or a header from the guest.
const HIKKO_API = "https://hikko.heyitsmejosh.com/api/posts";
const HIKKO_MAX_ROWS = 12, HIKKO_TITLE = 60, HIKKO_CAT = 14, HIKKO_TEXT = 150, HIKKO_PLAN = 200, HIKKO_MAX_BYTES = 5800;
function hikkoText(v, max) {
  if (typeof v !== "string") return "";
  const s = wireText(v.replace(/[\u2013\u2014]/g, "-").replace(/\u2026/g, "..."), 100000);
  if (s.length <= max) return s;
  return s.slice(0, max - 3).replace(/\s+\S*$/, "").trim() + "...";
}
function hikkoWire(list, bodies) {
  const rows = []; let bytes = 8;
  for (const p of Array.isArray(list) ? list : []) {
    if (rows.length >= HIKKO_MAX_ROWS) break;
    const title = hikkoText(p?.title, HIKKO_TITLE);
    if (!title) continue;
    const votes = Number.isInteger(p.score) && p.score > 0 && p.score < 100000 ? p.score : 0;
    const b = (p.id && bodies?.[p.id]) || {};
    const row = [votes, hikkoText(p.category, HIKKO_CAT), title, hikkoText(b.content, HIKKO_TEXT), hikkoText(b.enrichmentPlan, HIKKO_PLAN)].join("|");
    if (bytes + row.length + 1 > HIKKO_MAX_BYTES) break;
    rows.push(row); bytes += row.length + 1;
  }
  return [rows.length, ...rows].join("\n") + "\n";
}
async function hikkoGet(url) {
  const res = await fetch(url, {signal: AbortSignal.timeout(8000), cf: {cacheTtl: 300, cacheEverything: true}});
  if (!res.ok) throw new Error("http " + res.status);
  return res.json();
}
async function handleHikko() {
  let list;
  try { list = (await hikkoGet(`${HIKKO_API}?limit=${HIKKO_MAX_ROWS}`))?.posts; }
  catch { return new Response("", {status: 502}); }
  if (!Array.isArray(list)) return new Response("", {status: 502});
  const top = list.slice(0, HIKKO_MAX_ROWS).filter(p => typeof p?.id === "string" && /^[A-Za-z0-9_-]{1,64}$/.test(p.id));
  const got = await Promise.allSettled(top.map(p => hikkoGet(`${HIKKO_API}?id=${p.id}`)));
  const bodies = {};
  top.forEach((p, i) => { if (got[i].status === "fulfilled" && got[i].value?.post) bodies[p.id] = got[i].value.post; });
  const wire = hikkoWire(top, bodies);
  if (wire.split("\n").length < 3) return new Response("", {status: 502}); // no usable row: let the OS keep its demo ideas
  return new Response(wire, {headers: WIRE});
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
  "The hardware is called Neo, a concept case for the OS. It is not for sale yet. When the dev kit is ready you'll get one email from me, and nothing else in between.",
  "",
  "Joshua",
].join("\n");
const WAITLIST_MAIL_HTML = `<div style="background:#F4EEE3;padding:32px 16px;font-family:-apple-system,BlinkMacSystemFont,'SF Pro Text','Helvetica Neue',Helvetica,Arial,sans-serif;color:#1A1814"><div style="max-width:520px;margin:0 auto"><p style="font-size:28px;font-weight:600;letter-spacing:-0.02em;margin:0 0 16px">You're on the list.</p><p style="font-size:16px;line-height:1.55;margin:0 0 16px">Joshua Tree is an operating system written from scratch. It boots in your browser right now, with a dock full of apps and Samantha, the assistant who lives inside it.</p><p style="margin:0 0 16px"><a href="https://joshuatree.heyitsmejosh.com" style="background:#B9542C;color:#F4EEE3;text-decoration:none;font-weight:600;padding:12px 22px;border-radius:999px;display:inline-block">Try the live demo</a></p><p style="margin:0 0 24px"><a href="https://github.com/nulljosh/joshuatree/releases/download/1.8.8/joshua-tree-ad-v6.mp4" style="color:#B9542C;font-weight:600">Watch the 36 second ad</a></p><p style="font-size:15px;line-height:1.55;color:#6F675C;margin:0 0 16px">The hardware is called Neo, a concept case for the OS. It is not for sale yet. When the dev kit is ready you'll get one email from me, and nothing else in between.</p><p style="font-size:15px;margin:0">Joshua</p></div></div>`;

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

// Ring-3 Samantha speaks through SYS_HTTP_GET (up to 64 KB back, host fixed
// to this one): GET /api/speak?t=<one sentence> forwards to Turing's POST
// /api/speak and returns the raw 8-bit 16 kHz PCM untouched.
async function handleSpeakGet(url, request, env) {
  // Same per-IP guard as handleListen, its own binding (a sentence per call, so a looser limit).
  if (env && env.SPEAK_RATE_LIMITER) {
    const ip = request.headers.get("cf-connecting-ip") || "unknown";
    const { success } = await env.SPEAK_RATE_LIMITER.limit({ key: ip });
    if (!success) return new Response("too many requests", { status: 429 });
  }
  const text = (url.searchParams.get("t") || "").slice(0, 300);
  if (!text) return new Response("missing t", { status: 400 });
  const up = await fetch("https://turing.heyitsmejosh.com/api/speak", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ text, format: "pcm8", voice: url.searchParams.get("v") === "joshua" ? "joshua" : undefined }), // his cloned voice in portfolio mode
  });
  return new Response(up.body, { status: up.status, headers: { "Content-Type": "application/octet-stream", "Access-Control-Allow-Origin": "*" } });
}

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

// POST /api/mail/send: the Mail app's outgoing path (user/mail.c). The kernel has no TLS, so it posts
// plain HTTP here and this relays through Resend, same request shape as sendWaitlistEmail above.
// Guards, in order: POST only; a bearer token (MAIL_SEND_TOKEN secret, the kernel adds it from
// Settings, so the public demo cannot relay mail); a per-IP limit (MAIL_RATE_LIMITER, and with no
// binding it refuses rather than running unlimited); one validated recipient; capped text.
// Fixed sender, no reply-to, so this can never be used to impersonate anyone.
const MAIL_SEND_FROM = "Samantha <samantha@epiphany.heyitsmejosh.com>";
const MAIL_MAX_BODY_BYTES = 8192;
const MAIL_ADDR_RE = /^[A-Za-z0-9._%+-]{1,64}@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$/;

function tokenEquals(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}
const mailErr = (status, error) => new Response(JSON.stringify({ error }), { status, headers: JSON_CORS });
// Strip control characters (CR/LF included) so nothing typed can break a header or forge lines.
const oneLine = (v, max) => String(v == null ? "" : v).replace(/[\u0000-\u001f\u007f]+/g, " ").trim().slice(0, max);

async function handleMailSend(request, env) {
  if (request.method !== "POST") return mailErr(405, "method not allowed");
  if (!env.MAIL_SEND_TOKEN || !env.RESEND_API_KEY) return mailErr(503, "mail is not configured");
  const auth = request.headers.get("authorization") || "";
  if (!auth.startsWith("Bearer ") || !tokenEquals(auth.slice(7), env.MAIL_SEND_TOKEN)) return mailErr(401, "missing or wrong mail token");
  if (!env.MAIL_RATE_LIMITER) return mailErr(503, "mail rate limit is not configured");
  const ip = request.headers.get("cf-connecting-ip") || "unknown";
  const { success } = await env.MAIL_RATE_LIMITER.limit({ key: ip });
  if (!success) return mailErr(429, "too many messages, try again later");

  const declared = Number(request.headers.get("content-length") || "0");
  if (declared > MAIL_MAX_BODY_BYTES) return mailErr(413, "message too large");
  let data;
  try { data = JSON.parse(await request.text()); } catch (e) { return mailErr(400, "bad json"); }
  if (!data || typeof data !== "object") return mailErr(400, "bad json");

  const to = oneLine(data.to, 254);
  if (!MAIL_ADDR_RE.test(to)) return mailErr(400, "to must be one email address");
  const subject = oneLine(data.subject, 120) || "(no subject)";
  const fromName = oneLine(data.from_name, 40) || "someone";
  const body = String(data.body == null ? "" : data.body).replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, "").slice(0, 2000);
  if (!body.trim()) return mailErr(400, "body is empty");

  try {
    const res = await fetch("https://api.resend.com/emails", {
      method: "POST",
      headers: { Authorization: `Bearer ${env.RESEND_API_KEY}`, "Content-Type": "application/json" },
      body: JSON.stringify({ from: MAIL_SEND_FROM, to: [to], subject, text: `${body}\n\n--\nSent by ${fromName} from Joshua Tree (joshuatree.heyitsmejosh.com).` }),
    });
    if (!res.ok) { console.warn("mail send failed", res.status, await res.text()); return mailErr(502, "the mail service refused the message"); }
  } catch (e) { console.warn("mail send error", String(e)); return mailErr(502, "the mail service is unreachable"); }
  return new Response(JSON.stringify({ ok: true }), { status: 200, headers: JSON_CORS });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/api/stocks") return handleStocks(url);
    if (url.pathname === "/api/quotes") return handleQuotes();
    if (url.pathname === "/api/books") return handleBooks();
    if (url.pathname === "/api/lexly") return handleLexly(url);
    if (url.pathname === "/api/hikko") return handleHikko();
    if (url.pathname === "/api/deals") return handleDeals(request);
    if (url.pathname === "/api/listen") return handleListen(request, env);
    if (url.pathname === "/api/mail/send") return handleMailSend(request, env);
    if (url.pathname === "/api/speak" && request.method === "GET") return handleSpeakGet(url, request, env);
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
export { bookrankWire, handleBooks };
export { hikkoWire, hikkoText, handleHikko };
export { lexlyQuestions, lexlyText, lexlyCourseWire, lexlyQuestionWire, handleLexly, LEXLY_COURSES };
export { handleWaitlistPost, handleWaitlistCount };
export { handleMailSend, MAIL_SEND_FROM };
export { handleListen, wrapPcmAsWav, LISTEN_MAX_BYTES };
