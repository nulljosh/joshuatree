#!/usr/bin/env python3
"""Keyrate's page, served by the `keyrate` command on :8080, is one offline file.

13.24.0 embeds keyrate 2.5.0 bundled by keyrate's scripts/single.mjs. The kernel had served a
7 KB October copy whose scripts and styles pointed at files the Pi never had. This decodes
drivers/app_keyrate.h and fails if the page is that small again, or if it loads anything by
src, href, import, CSS url() or @import, fetch() or a Worker other than a data: URI or an in-page #anchor.
"""
import os, re, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
s = open(os.path.join(ROOT, "drivers/app_keyrate.h")).read()
n = int(re.search(r"app_keyrate_len = (\d+);", s).group(1))
body = re.search(r"app_keyrate_html\[\] = \{(.*?)\};", s, re.S).group(1)
page = bytes(int(x) for x in re.findall(r"\d+", body))
fails = []
if len(page) != n: fails.append(f"app_keyrate_len says {n}, the array holds {len(page)}")
if n < 30000: fails.append(f"the page is {n} bytes: the stale copy is back (2.5.0 bundled is about 42 KB)")
html = page.decode("utf-8", "replace")
refs = re.findall(r"""\b(?:src|href)\s*=\s*["']([^"']*)["']""", html)
refs += re.findall(r"""\bimport\s*(?:[\w{}*,\s]+from\s*)?["']([^"']+)["']""", html)
refs += re.findall(r"""\bimport\(\s*["']([^"']+)["']""", html)
refs += re.findall(r"""\burl\(\s*["']?([^"')]+)["']?\s*\)""", html)            # CSS url(...), fonts and images
refs += re.findall(r"""@import\s+["']([^"']+)["']""", html)                    # CSS @import "x.css"
refs += re.findall(r"""\b(?:fetch|Worker|SharedWorker|importScripts)\(\s*["'`]([^"'`]+)["'`]""", html)
bad = [r for r in refs if r and not r.startswith(("data:", "#"))]
if bad: fails.append(f"the page loads files the Pi does not serve: {bad[:5]}")
if "<script" not in html: fails.append("no inline script: the test is missing")
if fails:
    print("FAIL:"); [print("  - " + f) for f in fails]; sys.exit(1)
print(f"PASS: Keyrate is one offline page ({n} bytes), nothing loaded from outside it")
