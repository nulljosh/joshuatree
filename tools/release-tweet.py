#!/usr/bin/env python3
"""Post a release to X, so a new version announces itself.

Usage: tools/release-tweet.py "<version>" "<title>" "<url>"
Needs X_API_KEY, X_API_SECRET, X_ACCESS_TOKEN, X_ACCESS_SECRET (an X app
with read and write, OAuth 1.0a user keys). Without them it prints the post
and exits 0: a dry run, so releases never fail over a missing key.
Stdlib only: OAuth 1.0a signing is a few lines of HMAC-SHA1.
"""
import base64, hashlib, hmac, json, os, secrets, sys, time, urllib.parse, urllib.request

v, title, url = sys.argv[1:4]
text = f"Joshua Tree {v}: {title}\n\nA computer built from scratch, running in your browser.\n{url}"
if len(text) > 280:
    text = f"Joshua Tree {v}: {title[:280 - len(v) - len(url) - 20]}...\n{url}"
keys = [os.environ.get(k, "") for k in ("X_API_KEY", "X_API_SECRET", "X_ACCESS_TOKEN", "X_ACCESS_SECRET")]
if not all(keys):
    print("dry run (no X keys), would post:\n" + text)
    sys.exit(0)
ck, cs, tk, ts = keys
endpoint = "https://api.twitter.com/2/tweets"
q = lambda s: urllib.parse.quote(s, safe="")
oauth = {"oauth_consumer_key": ck, "oauth_nonce": secrets.token_hex(16), "oauth_signature_method": "HMAC-SHA1",
         "oauth_timestamp": str(int(time.time())), "oauth_token": tk, "oauth_version": "1.0"}
base = "&".join(["POST", q(endpoint), q("&".join(f"{q(k)}={q(oauth[k])}" for k in sorted(oauth)))])
oauth["oauth_signature"] = base64.b64encode(hmac.new(f"{q(cs)}&{q(ts)}".encode(), base.encode(), hashlib.sha1).digest()).decode()
auth = "OAuth " + ", ".join(f'{q(k)}="{q(val)}"' for k, val in sorted(oauth.items()))
req = urllib.request.Request(endpoint, data=json.dumps({"text": text}).encode(), method="POST",
                             headers={"Authorization": auth, "Content-Type": "application/json"})
try:
    print("posted:", json.load(urllib.request.urlopen(req, timeout=20))["data"]["id"])
except Exception as e:   # never fail a release over a post
    print("post failed:", e)
