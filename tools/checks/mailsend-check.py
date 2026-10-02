#!/usr/bin/env python3
"""Mail Send check, no QEMU. Two halves.

1. The Worker route, run for real under node: worker.js's handleMailSend with a stubbed Resend
   (a fake fetch that records the outgoing request) and fake env bindings. Asserts the refusals
   (wrong method, no secret, no/wrong bearer, no rate-limit binding, rate limited, bad or multiple
   recipients, empty body) and that a good request reaches Resend with the fixed sender, one
   recipient, no reply_to, capped subject and body, and answers {"ok":true}.
2. The wiring, from source: user/mail.c posts {from_name,to,subject,body} to /api/mail/send with
   JT_POST_WORKER, shows "Sent." or a reason, files the record with the s/x flag; the kernel adds
   the bearer only for that path inside SYS_HTTP_POST and the token is never exposed to ring 3;
   SYSCALL-ABI.md documents it.

The "Sent" state in a booted Mail window is not driven here (that needs QEMU); it is covered by
the source assertions plus the Worker half.
"""
import json, os, py_compile, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
def rd(p): return open(os.path.join(ROOT, p), encoding="utf-8").read()
fails = []
def check(name, ok):
    print(("ok   " if ok else "FAIL ") + name)
    if not ok: fails.append(name)

NODE = r'''
import { handleMailSend } from "%s/worker.js";
const out = [];
let sent = [];
globalThis.fetch = async (url, init) => { sent.push({ url, init }); return new Response("{}", { status: 200 }); };
const req = (body, headers = {}, method = "POST") => new Request("http://x/api/mail/send", { method, headers: { "content-type": "application/json", ...headers }, body: method === "GET" ? undefined : (typeof body === "string" ? body : JSON.stringify(body)) });
const good = { from_name: "Josh", to: "a@example.com", subject: "hi", body: "hello" };
const env = (over = {}) => ({ RESEND_API_KEY: "rk", MAIL_SEND_TOKEN: "tok", MAIL_RATE_LIMITER: { limit: async () => ({ success: true }) }, ...over });
const auth = { authorization: "Bearer tok" };
async function t(name, r, wantStatus) { const res = await r; out.push({ name, status: res.status, want: wantStatus, text: await res.text() }); }
await t("get refused", handleMailSend(req(null, auth, "GET"), env()), 405);
await t("no token secret", handleMailSend(req(good, auth), env({ MAIL_SEND_TOKEN: undefined })), 503);
await t("no bearer", handleMailSend(req(good), env()), 401);
await t("wrong bearer", handleMailSend(req(good, { authorization: "Bearer nope" }), env()), 401);
await t("no rate binding refuses", handleMailSend(req(good, auth), env({ MAIL_RATE_LIMITER: undefined })), 503);
await t("rate limited", handleMailSend(req(good, auth), env({ MAIL_RATE_LIMITER: { limit: async () => ({ success: false }) } })), 429);
await t("two recipients", handleMailSend(req({ ...good, to: "a@example.com,b@example.com" }, auth), env()), 400);
await t("header injection", handleMailSend(req({ ...good, to: "a@example.com\r\nbcc: b@example.com" }, auth), env()), 400);
await t("no at", handleMailSend(req({ ...good, to: "nobody" }, auth), env()), 400);
await t("empty body", handleMailSend(req({ ...good, body: "  " }, auth), env()), 400);
await t("bad json", handleMailSend(req("{nope", auth), env()), 400);
if (sent.length) out.push({ name: "refusals never reached Resend", status: 0, want: 0, text: "", bad: true });
await t("good send", handleMailSend(req({ ...good, subject: "s".repeat(300), body: "b".repeat(5000), reply_to: "x@y.com" }, auth), env()), 200);
console.log(JSON.stringify({ out, sent: sent.map(s => ({ url: s.url, auth: s.init.headers.Authorization, body: JSON.parse(s.init.body) })) }));
''' % ROOT.replace("\\", "/")

with tempfile.NamedTemporaryFile("w", suffix=".mjs", delete=False) as f:
    f.write(NODE); path = f.name
r = subprocess.run(["node", path], capture_output=True, text=True, timeout=60)
os.unlink(path)
if r.returncode != 0:
    print(r.stderr); check("worker harness ran", False)
else:
    res = json.loads(r.stdout.strip().splitlines()[-1])
    for o in res["out"]:
        if o.get("bad"): check(o["name"], False); continue
        check("worker: " + o["name"] + " -> " + str(o["want"]), o["status"] == o["want"])
    check("worker: exactly one Resend call (the good one)", len(res["sent"]) == 1)
    if res["sent"]:
        s = res["sent"][0]; b = s["body"]
        check("worker: Resend url", s["url"] == "https://api.resend.com/emails")
        check("worker: Resend bearer is the API key", s["auth"] == "Bearer rk")
        check("worker: fixed sender", b["from"].startswith("Samantha <samantha@"))
        check("worker: one recipient", b["to"] == ["a@example.com"])
        check("worker: no reply_to", "reply_to" not in b and "replyTo" not in b)
        check("worker: subject capped at 120", len(b["subject"]) == 120)
        check("worker: body capped near 2000", 2000 <= len(b["text"]) < 2200)

mail = rd("user/mail.c")
check("mail.c posts to /api/mail/send", '"/api/mail/send"' in mail)
check("mail.c uses JT_POST_WORKER", "jt_http_post_ex" in mail and "JT_POST_WORKER" in mail)
for k in ("from_name", "to", "subject", "body"):
    check("mail.c body key " + k, '"%s"' % k in mail)
check("mail.c shows Sent.", '"Sent."' in mail)
check("mail.c shows a reason when unsent", "Not sent:" in mail)
check("mail.c files s/x flag", '"s|"' in mail and '"x|"' in mail)
check("mail.h reads s/x as read", "!= 'u'" in rd("kernel/mail.h"))
sc = rd("kernel/syscall.c")
check("syscall: bearer only for /api/mail/send on the Worker", "path_is_mail_send" in sc and "http_post_set_bearer(mail_token_get())" in sc and '"/api/mail/send"' in sc)
check("syscall: bearer disarmed after the call", "http_post_set_bearer(0)" in sc)
check("no syscall returns the token", "mail_token" not in rd("kernel/syscall.h").replace("mail_token_get", "") and "mail_token" not in rd("user/jtsys.h"))
check("settings persist mailtoken", "mailtoken=" in rd("kernel/kernel.c") and "is_mailtoken" in rd("kernel/kernel.c"))
check("settings row exists", "Mail token" in rd("kernel/settings_ui.h"))
check("http.c adds Authorization: Bearer", "Authorization: Bearer" in rd("drivers/http.c"))
abi = rd("SYSCALL-ABI.md") if os.path.exists(os.path.join(ROOT, "SYSCALL-ABI.md")) else rd("docs/SYSCALL-ABI.md")
check("SYSCALL-ABI documents /api/mail/send", "/api/mail/send" in abi and "never" in abi)
wr = rd("wrangler.toml")
check("wrangler MAIL_RATE_LIMITER", 'name = "MAIL_RATE_LIMITER"' in wr)

py_compile.compile(__file__, doraise=True)
print("FAILED: " + ", ".join(fails) if fails else "mailsend-check: all ok")
sys.exit(1 if fails else 0)
