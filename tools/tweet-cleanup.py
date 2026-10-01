#!/usr/bin/env python3
"""Delete old release posts that carry the repeated tagline.

Run by hand from the "Clean up X posts" workflow (the keys live only in
GitHub secrets). Lists the account's recent posts and deletes every one
containing TAGLINE. Without keys it exits 0 as a dry run.
"""
import base64, hashlib, hmac, json, os, secrets, sys, time, urllib.parse, urllib.request

TAGLINE = "A computer built from scratch. Try it in your browser"
keys = [os.environ.get(k, "") for k in ("X_API_KEY", "X_API_SECRET", "X_ACCESS_TOKEN", "X_ACCESS_SECRET")]
if not all(keys):
    print("dry run (no X keys)"); sys.exit(0)
ck, cs, tk, ts = keys
q = lambda s: urllib.parse.quote(s, safe="")

def call(method, url, query=None):
    query = query or {}
    oauth = {"oauth_consumer_key": ck, "oauth_nonce": secrets.token_hex(16), "oauth_signature_method": "HMAC-SHA1",
             "oauth_timestamp": str(int(time.time())), "oauth_token": tk, "oauth_version": "1.0"}
    params = {**oauth, **query}
    base = "&".join([method, q(url), q("&".join(f"{q(k)}={q(params[k])}" for k in sorted(params)))])
    oauth["oauth_signature"] = base64.b64encode(hmac.new(f"{q(cs)}&{q(ts)}".encode(), base.encode(), hashlib.sha1).digest()).decode()
    auth = "OAuth " + ", ".join(f'{q(k)}="{q(v)}"' for k, v in sorted(oauth.items()))
    full = url + ("?" + urllib.parse.urlencode(query) if query else "")
    return json.load(urllib.request.urlopen(urllib.request.Request(full, method=method, headers={"Authorization": auth}), timeout=30))

me = call("GET", "https://api.twitter.com/2/users/me")["data"]["id"]
posts = call("GET", f"https://api.twitter.com/2/users/{me}/tweets", {"max_results": "100"}).get("data", [])
spam = [p for p in posts if TAGLINE in p["text"]]
print(f"{len(posts)} recent posts, {len(spam)} with the tagline")
for p in spam:
    try:
        call("DELETE", f"https://api.twitter.com/2/tweets/{p['id']}")
        print("deleted", p["id"], p["text"].splitlines()[0])
    except Exception as e:
        print("failed", p["id"], e)
