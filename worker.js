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
   the three real hosts this kernel's own code (drivers/http.c's callers
   in kernel.c: geo_fetch, weather_fetch, wall_fetch) ever asks for,
   nothing else is ever proxied. */

const ALLOWED_HOSTS = new Set([
  "ip-api.com",
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

  if (!isAllowedTarget(targetUrl)) {
    return new Response("Host not allowed", { status: 403 });
  }

  // Only forward a plain GET with no guest-controlled headers/body: every
  // real caller here (geo_fetch/weather_fetch/wall_fetch) only ever issues
  // a bare GET, so there's nothing legitimate to lose by not forwarding
  // arbitrary request headers/methods through to the upstream host.
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

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
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
