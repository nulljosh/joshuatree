#!/usr/bin/env python3
"""Where the Launchpad (Apps folder) is, for every check that clicks into it or looks for its red dot.

One place, so a layout change touches this file and kernel/apps_geom.h, not fifty checks.
  - folder_geometry(lw, lh): window, viewport and grid for a screen, from kernel/apps_geom.h's own numbers.
  - from_log(path): the same, as the kernel measured it (the "appsgeom" serial line), when a run has one.
  - FOLDER_CLOSE_X/Y: the folder window's red dot on the default 960x540 screen.
  - tile_center(g, k, scroll): screen point of grid slot k.
"""
import os, re

_HDR = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "kernel", "apps_geom.h")).read()
_D = {m[0]: int(m[1]) for m in re.findall(r"#define (APPS_\w+)\s+(\d+)", _HDR)}
MENU_H = 26


def _panel_h(rows): return _D["APPS_GRID_TOP"] + (rows - 1) * _D["APPS_CELL_H"] + _D["APPS_LABEL_H"] + _D["APPS_PAD"]


def folder_geometry(lw=960, lh=540):
    """Default dock (11 tiles, 7 percent scale), as in kernel/dock_geom.c."""
    icon = max(16, min(lh * 7 // 100, (min(740, lw - 40) - 2 * 10 - 10 * 6) // 11))
    bot = lh - icon - 2 * 10 - 24; top = MENU_H
    rows = 3
    while rows > 1:
        py = (top + bot - _panel_h(rows)) // 2
        if py - _D["APPS_MARGIN"] - 32 >= top + 2 and py + _panel_h(rows) + _D["APPS_MARGIN"] + 8 <= bot - 8: break
        rows -= 1
    cw = _D["APPS_CELL_W"]; pw = 5 * cw + 2 * _D["APPS_PADX"]; ph = _panel_h(rows)
    w = pw + 2 * _D["APPS_MARGIN"] + _D["APPS_FRAME_W"]; h = ph + 2 * _D["APPS_MARGIN"] + _D["APPS_FRAME_H"]
    x = (lw - w) // 2; y = (top + bot - ph) // 2 - _D["APPS_MARGIN"] - 32
    vx, vy = x + 8, y + 32
    px = (w - 16 - pw) // 2
    return {"vis": rows, "px": px, "py": _D["APPS_MARGIN"], "pw": pw, "ph": ph, "x0": px + _D["APPS_PADX"],
            "y0": _D["APPS_MARGIN"] + _D["APPS_GRID_TOP"], "cw": cw, "ch": _D["APPS_CELL_H"], "tile": _D["APPS_TILE"],
            "vx": vx, "vy": vy, "wx": x, "wy": y, "ww": w, "wh": h}


def from_log(path):
    line = [l for l in open(path, errors="replace").read().splitlines() if l.startswith("appsgeom")][-1]
    return {k: int(v) for k, v in (kv.split("=") for kv in line.split()[1:])}


def close_dot(g): return (g["vx"] - 8 + 24, g["vy"] - 32 + 16)  # window x+24, y+16


def tile_center(g, k, scroll=0):
    """Screen point inside grid slot k (row/col from index k, minus scroll rows)."""
    row, col = k // 5 - scroll, k % 5
    return (g["vx"] + g["x0"] + col * g["cw"] + g["cw"] // 2, g["vy"] + g["y0"] + row * g["ch"] + g["tile"] // 2)


_DEF = folder_geometry()
FOLDER_CLOSE_X, FOLDER_CLOSE_Y = close_dot(_DEF)
