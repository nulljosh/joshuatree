#!/usr/bin/env python3
"""Dock drift guard: static, no boot, under a second.

Every check that clicks a dock tile hardcodes that tile's slot number. When the
dock changes (Stocks pinned at 9 pushed Trash to 10) those numbers go stale and
unrelated checks fail far downstream. This reads the one real source, kernel.c's
GUI_DOCK_DEFAULT + GUI_LABELS, and fails naming every file that disagrees.
"""
import glob, os, re, sys
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
k = open("kernel/kernel.c").read()
labels = re.findall(r'"([^"]+)"', re.search(r"GUI_LABELS\[GUI_APP_COUNT\] = \{(.*?)\};", k, re.S).group(1))
sym = {"GUI_APPS_FOLDER": labels.index("Apps"), "GUI_TRASH": labels.index("Trash")}
order = [labels[sym[t] if t in sym else int(t)]
         for t in re.search(r"GUI_DOCK_DEFAULT\[GUI_ICON_COUNT\] = \{(.*?)\};", k).group(1).replace(" ", "").split(",")]
slot = {n.upper(): i for i, n in enumerate(order)}
bad = []
for f in sorted(glob.glob("tools/checks/*")) + ["landing/v86/embed.js"]:
    if f.endswith("dockslots-check.py") or not os.path.isfile(f): continue
    s = open(f, errors="replace").read()
    for names, vals in re.findall(r"^\s*(?:const |var )?((?:[A-Z]+_SLOT(?:, )?)+) = ([\d, ]+)", s, re.M):
        for n, v in zip(names.split(", "), vals.split(",")):
            app = n[:-5]
            if app in slot and v.strip() and int(v) != slot[app]:
                bad.append(f"{f}: {n} = {int(v)}, the dock has {app.title()} at {slot[app]}")
    for m in re.findall(r'^SLOTS = \[(.*?)\]', s, re.M):
        got = re.findall(r'"([^"]+)"', m)
        if got != order: bad.append(f"{f}: SLOTS list {got} != dock order {order}")
    for m in re.findall(r"\bSLOTS = (?:[\d, ]+, )?(\d+)$", s, re.M):
        if int(m) != len(order): bad.append(f"{f}: SLOTS = {m}, the dock has {len(order)} tiles")
    if f.endswith("embed.js"):
        m = re.search(r"var count = (\d+), gap", s)
        if m and int(m.group(1)) != len(order): bad.append(f"{f}: dockSlotPos count = {m.group(1)}, the dock has {len(order)} tiles")
print("dock order:", order)
if bad:
    print("\n".join("STALE  " + b for b in bad)); print(f"FAIL: {len(bad)} stale dock slot constant(s)"); sys.exit(1)
print("PASS: every hardcoded dock slot agrees with kernel.c")
