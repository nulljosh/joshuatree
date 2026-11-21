#!/usr/bin/env python3
"""The app wall never flashes blank on scroll (13.22.0): each clip's still is its figure's background, a clip starts
loading 600 px before it scrolls in, and it stays invisible (class wait) until its first `playing` event.
Reading the page's own source, so it fails if any of the three is taken out.
Usage: tools/checks/landing-clip-still-check.py   (from anywhere)"""
import os, sys
html = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "landing", "index.html")).read()
need = {
    "the still behind each clip": "v.parentNode.style.backgroundImage = 'url(' + v.poster + ')'",
    "loading before it scrolls in": "rootMargin: '600px 0px'",
    "invisible until it plays": "video.clip.wait { opacity: 0; }",
    "shown on its first frame": "v.addEventListener('playing', function () { v.classList.remove('wait'); }",
}
miss = [k for k, v in need.items() if v not in html]
if miss: print("FAIL: app wall clips can flash blank: missing " + ", ".join(miss)); sys.exit(1)
print("PASS: app wall clips load early, sit over their stills and fade in only when playing")
