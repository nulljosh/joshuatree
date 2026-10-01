#!/usr/bin/env python3
"""Post a release to X, so a new version announces itself.

Usage: tools/release-tweet.py "<version>" "<title>" [url, unused]
       tools/release-tweet.py --avatar landing/brand/x-avatar.png   (set the profile picture)
Needs X_API_KEY, X_API_SECRET, X_ACCESS_TOKEN, X_ACCESS_SECRET (an X app
with read and write, OAuth 1.0a user keys). Without them it prints the post
and exits 0: a dry run, so releases never fail over a missing key.
Stdlib only: OAuth 1.0a signing is a few lines of HMAC-SHA1.
"""
import base64, hashlib, hmac, json, os, re, secrets, sys, time, urllib.parse, urllib.request

AVATAR = sys.argv[2] if sys.argv[1:2] == ["--avatar"] else None
v, title = (None, None) if AVATAR else sys.argv[1:3]
# No link in the post: X charges $0.20 for a post with a URL and $0.015
# without (pay-per-use, 2026-09). The profile bio links the site.
# Changelog note only, like @ClaudeCodeLog: no tagline repeated on every post,
# and only the first clause of the title (the rest is usually dev notes).
note = re.split(r";\s*", title or "")[0].strip()
text = "" if AVATAR else f"Joshua Tree {v}: {note}"[:280]
keys = [os.environ.get(k, "") for k in ("X_API_KEY", "X_API_SECRET", "X_ACCESS_TOKEN", "X_ACCESS_SECRET")]
if not all(keys):
    print("dry run (no X keys), would " + (f"set avatar {AVATAR}" if AVATAR else "post:\n" + text))
    sys.exit(0)
ck, cs, tk, ts = keys
if AVATAR:   # v1.1 profile image: form-encoded, so the image field is signed too
    endpoint = "https://api.twitter.com/1.1/account/update_profile_image.json"
    form = {"image": base64.b64encode(open(AVATAR, "rb").read()).decode()}
else:
    endpoint, form = "https://api.twitter.com/2/tweets", {}
q = lambda s: urllib.parse.quote(s, safe="")
oauth = {"oauth_consumer_key": ck, "oauth_nonce": secrets.token_hex(16), "oauth_signature_method": "HMAC-SHA1",
         "oauth_timestamp": str(int(time.time())), "oauth_token": tk, "oauth_version": "1.0"}
params = {**oauth, **form}
base = "&".join(["POST", q(endpoint), q("&".join(f"{q(k)}={q(params[k])}" for k in sorted(params)))])
oauth["oauth_signature"] = base64.b64encode(hmac.new(f"{q(cs)}&{q(ts)}".encode(), base.encode(), hashlib.sha1).digest()).decode()
auth = "OAuth " + ", ".join(f'{q(k)}="{q(val)}"' for k, val in sorted(oauth.items()))
if AVATAR:
    req = urllib.request.Request(endpoint, data=urllib.parse.urlencode(form).encode(), method="POST",
                                 headers={"Authorization": auth, "Content-Type": "application/x-www-form-urlencoded"})
else:
    req = urllib.request.Request(endpoint, data=json.dumps({"text": text}).encode(), method="POST",
                                 headers={"Authorization": auth, "Content-Type": "application/json"})
try:
    r = json.load(urllib.request.urlopen(req, timeout=30))
    print("avatar set" if AVATAR else "posted: " + r["data"]["id"])
except Exception as e:   # never fail a release over a post
    print("post failed:", e)
