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

async function handleProxy(request) {
  const requestUrl = new URL(request.url);
  const target = requestUrl.searchParams.get("url");
  if (!target) return new Response("Missing url parameter", { status: 400 });

  let targetUrl;
  try {
    targetUrl = new URL(target);
  } catch (e) {
    return new Response("Invalid url parameter", { status: 400 });
  }

  if (targetUrl.hostname === "joshuatree.heyitsmejosh.com" && targetUrl.pathname === "/api/stocks"
      && ["http:", "https:"].includes(targetUrl.protocol) && !targetUrl.port && !targetUrl.username && !targetUrl.password)
    return handleStocks(targetUrl);

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
  const isSamanthaChat = targetUrl.hostname === "turing.heyitsmejosh.com" && targetUrl.pathname === "/api/chat"
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
    const upstreamResponse = await fetch(targetUrl.toString(), {
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

  // Only forward a plain GET with no guest-controlled headers/body: every
  // real caller here (geo_fetch/weather_fetch/wall_fetch) only ever issues
  // a bare GET, so there's nothing legitimate to lose by not forwarding
  // arbitrary request headers/methods through to the upstream host. This
  // is deliberately unconditional -- even a POST from the guest lands
  // here as a bare GET with no body, the one exception being the
  // Samantha-chat branch above, which returns before ever reaching this
  // line.
  const upstreamResponse = await fetch(targetUrl.toString(), {
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

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/api/stocks") return handleStocks(url);
    if (url.pathname === "/api/proxy") {
      return handleProxy(request);
    }
    return env.ASSETS.fetch(request);
  },
};

// Exported for tools/checks/worker-proxy-check.mjs, a real Cloudflare
// Worker isn't spun up in this CI; the allowlist logic and request/
// response handling are still real, plain JS testable directly under Node.
export { isAllowedTarget, handleProxy, ALLOWED_HOSTS };

export { stockWire, handleStocks };
