"""Generated blocks inside docs/ARCHITECTURE.md. A block sits between <!-- NAME:start --> and <!-- NAME:end -->; generators rewrite only that."""
import os, re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PATH = os.path.join(ROOT, "docs", "ARCHITECTURE.md")


def spliced(name, body, text=None):
    """The doc text with block NAME holding body."""
    if text is None:
        text = open(PATH, encoding="utf-8").read()
    start, end = "<!-- %s:start -->" % name, "<!-- %s:end -->" % name
    pat = re.compile(re.escape(start) + r".*?" + re.escape(end), re.S)
    if not pat.search(text):
        raise SystemExit("docs/ARCHITECTURE.md has no %s block" % name)
    return pat.sub(lambda m: start + "\n" + body.strip("\n") + "\n" + end, text, count=1)


def current(name):
    text = open(PATH, encoding="utf-8").read()
    m = re.search(r"<!-- %s:start -->\n(.*?)\n<!-- %s:end -->" % (name, name), text, re.S)
    return m.group(1) if m else None


def write(name, body):
    text = spliced(name, body)
    open(PATH, "w", encoding="utf-8").write(text)
