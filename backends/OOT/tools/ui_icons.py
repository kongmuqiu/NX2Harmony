#!/usr/bin/env python3
"""The app shell's icons, drawn here as geometry: every icon is one SVG path of polygons on a 24 x 24 grid, filled
white with the nonzero rule (outlines wind one way, cut-outs the other), so ArkUI's Image.fillColor recolours all of
it (fillColor replaces fills only; a stroke would keep its colour). Strokes are made into polygons here: a segment
is a quad, a joint or a cap a circle, an arc an annular band.

    python tools/ui_icons.py [--out entry/src/main/resources/base/media] [--preview icons.png]

Writes ic_<name>.svg for each icon below. --preview renders a contact sheet with PySide6 (to look at them)."""
from __future__ import annotations

import argparse
import math
from pathlib import Path

N = 40   # points per full circle
BOLD = 1.22   # every stroke this much wider: chunky, game-HUD glyphs


# ---------- polygon primitives (screen coordinates, y down; "solid" = positive shoelace area) ----------
def _area(pts):
    return sum(x1 * y2 - x2 * y1 for (x1, y1), (x2, y2) in zip(pts, pts[1:] + pts[:1])) / 2


def poly(pts, hole=False):
    pts = list(pts)
    if (_area(pts) > 0) == hole:
        pts.reverse()
    return pts, hole


def circle(cx, cy, r, hole=False, n=N):
    return poly([(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)], hole)


def ellipse(cx, cy, rx, ry, hole=False, n=N):
    return poly([(cx + rx * math.cos(2 * math.pi * i / n), cy + ry * math.sin(2 * math.pi * i / n)) for i in range(n)], hole)


def rrect(x, y, w, h, r=0.0, hole=False):
    r = min(r, w / 2, h / 2)
    if r <= 0:
        return poly([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], hole)
    pts = []
    for cx, cy, a0 in ((x + w - r, y + r, -90), (x + w - r, y + h - r, 0), (x + r, y + h - r, 90), (x + r, y + r, 180)):
        for i in range(7):
            a = math.radians(a0 + 15 * i)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return poly(pts, hole)


def seg(x1, y1, x2, y2, w):
    """A stroke segment with round caps: its quad and two cap circles."""
    w *= BOLD
    dx, dy = x2 - x1, y2 - y1
    L = math.hypot(dx, dy) or 1
    nx, ny = -dy / L * w / 2, dx / L * w / 2
    return [poly([(x1 + nx, y1 + ny), (x2 + nx, y2 + ny), (x2 - nx, y2 - ny), (x1 - nx, y1 - ny)]),
            circle(x1, y1, w / 2, n=16), circle(x2, y2, w / 2, n=16)]


def line(pts, w):
    out = []
    for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
        out += seg(x1, y1, x2, y2, w)
    return out


def arc(cx, cy, r, a0, a1, w, caps=True):
    """A stroked arc from angle a0 to a1 (degrees, clockwise on screen, 0 = right)."""
    w *= BOLD
    steps = max(4, int(abs(a1 - a0) / 6))
    outer = [(cx + (r + w / 2) * math.cos(math.radians(a0 + (a1 - a0) * i / steps)),
              cy + (r + w / 2) * math.sin(math.radians(a0 + (a1 - a0) * i / steps))) for i in range(steps + 1)]
    inner = [(cx + (r - w / 2) * math.cos(math.radians(a0 + (a1 - a0) * i / steps)),
              cy + (r - w / 2) * math.sin(math.radians(a0 + (a1 - a0) * i / steps))) for i in range(steps + 1)]
    out = [poly(outer + inner[::-1])]
    if caps:
        for a in (a0, a1):
            out.append(circle(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)), w / 2, n=16))
    return out


def ring(cx, cy, r, w):
    w *= BOLD
    return [circle(cx, cy, r + w / 2), circle(cx, cy, r - w / 2, hole=True)]


def at(cx, cy, r, deg):
    return cx + r * math.cos(math.radians(deg)), cy + r * math.sin(math.radians(deg))


def head(tip, deg, size=4.2, w=2.4):
    """An arrow head (two strokes) at tip, pointing along deg."""
    a = at(tip[0], tip[1], size, deg + 180 - 42)
    b = at(tip[0], tip[1], size, deg + 180 + 42)
    return line([a, tip, b], w)


def star(cx, cy, ro, ri, points, rot=-90):
    pts = []
    for i in range(points * 2):
        r = ro if i % 2 == 0 else ri
        pts.append(at(cx, cy, r, rot + 180 * i / points))
    return [poly(pts)]


# ---------- the icons ----------
W = 2.2   # the default stroke width


def icons() -> dict:
    I = {}
    I["play"] = [poly([(8, 4.5), (19.5, 12), (8, 19.5)])]
    gear = [circle(12, 12, 7.2)]
    for k in range(8):
        a = math.radians(22.5 + 45 * k)
        c, s = math.cos(a), math.sin(a)
        hw = 1.8
        p = [(12 + c * 6 - s * hw, 12 + s * 6 + c * hw), (12 + c * 10.2 - s * hw, 12 + s * 10.2 + c * hw),
             (12 + c * 10.2 + s * hw, 12 + s * 10.2 - c * hw), (12 + c * 6 + s * hw, 12 + s * 6 - c * hw)]
        gear.append(poly(p))
    gear.append(circle(12, 12, 3, hole=True))
    I["settings"] = gear
    I["save"] = [rrect(4, 3.5, 16, 17, 3), rrect(6.2, 5.7, 11.6, 12.6, 1.2, hole=True), rrect(8.5, 5.7, 7, 4.5, 0.8),
                 rrect(8.5, 13.5, 7, 4.8, 0.8)]
    I["info"] = ring(12, 12, 8.6, 2) + [circle(12, 7.8, 1.35), rrect(10.95, 10.3, 2.1, 7, 1)]
    I["back"] = line([(15, 4.8), (7.8, 12), (15, 19.2)], 2.6)
    I["forward"] = line([(9, 4.8), (16.2, 12), (9, 19.2)], 2.6)
    I["close"] = line([(6, 6), (18, 18)], 2.4) + line([(18, 6), (6, 18)], 2.4)
    I["check"] = line([(4.8, 12.6), (9.8, 17.6), (19.4, 7)], 2.6)
    I["warning"] = [poly([(12, 2.8), (22.2, 20.6), (1.8, 20.6)]), poly([(12, 7.2), (18.6, 18.6), (5.4, 18.6)], hole=True),
                    rrect(11, 9.8, 2, 5.2, 1), circle(12, 16.6, 1.2)]
    I["export"] = line([(4.5, 14), (4.5, 19.5), (19.5, 19.5), (19.5, 14)], W) + line([(12, 15.5), (12, 4.5)], W) + \
        head((12, 4.5), -90)
    I["import"] = line([(4.5, 14), (4.5, 19.5), (19.5, 19.5), (19.5, 14)], W) + line([(12, 4.5), (12, 15)], W) + \
        head((12, 15), 90)
    I["restore"] = arc(12, 12.5, 7.6, -150, 160, W) + head(at(12, 12.5, 7.6, -150), -60, 4.2, W)
    pad_body = [rrect(2.6, 7.2, 18.8, 9.4, 4.7), circle(6.9, 15.8, 3.9), circle(17.1, 15.8, 3.9)]
    cross = [(5.85, 9.7), (7.55, 9.7), (7.55, 11.15), (9.0, 11.15), (9.0, 12.85), (7.55, 12.85), (7.55, 14.3), (5.85, 14.3),
             (5.85, 12.85), (4.4, 12.85), (4.4, 11.15), (5.85, 11.15)]
    I["controller"] = pad_body + [poly(cross, hole=True), circle(16.4, 10.6, 1.25, hole=True), circle(18.7, 12.9, 1.25, hole=True),
                                  circle(14.1, 12.9, 1.25, hole=True), circle(16.4, 15.2, 1.25, hole=True)]
    I["touch"] = [circle(12, 15, 2.8)] + arc(12, 15, 6.6, 205, 335, 1.9) + arc(12, 15, 10.4, 215, 325, 1.9)
    I["look"] = ring(12, 12, 4.2, 2.1) + [circle(12, 12, 1.6)] + line([(6.2, 8.2), (2.6, 12), (6.2, 15.8)], 2.1) + \
        line([(17.8, 8.2), (21.4, 12), (17.8, 15.8)], 2.1)
    I["sticks"] = ring(7, 13.5, 4.3, 2) + ring(17, 13.5, 4.3, 2) + [circle(7, 13.5, 1.7), circle(17, 13.5, 1.7)]
    I["gauge"] = arc(12, 13.5, 8.2, 150, 390, W) + line([(12, 13.5), (16.6, 8.6)], W) + [circle(12, 13.5, 2)]
    I["layers"] = [poly([(12, 3.2), (20.8, 7.8), (12, 12.4), (3.2, 7.8)])] + line([(3.6, 12), (12, 16.4), (20.4, 12)], 2) + \
        line([(3.6, 16.2), (12, 20.6), (20.4, 16.2)], 2)
    I["sparkle"] = star(10.5, 13, 9, 2.6, 4) + star(18.8, 5.2, 3.6, 1.1, 4)
    I["display"] = [rrect(2.8, 4, 18.4, 12.6, 2.2), rrect(4.9, 6.1, 14.2, 8.4, 0.8, hole=True), rrect(11, 16, 2, 3, 0),
                    rrect(7.2, 18.8, 9.6, 2.1, 1)]
    I["more"] = [circle(5.5, 12, 1.9), circle(12, 12, 1.9), circle(18.5, 12, 1.9)]
    I["menu"] = [rrect(4, 5.6, 16, 2.2, 1.1), rrect(4, 10.9, 16, 2.2, 1.1), rrect(4, 16.2, 16, 2.2, 1.1)]
    I["home"] = [poly([(12, 2.8), (21.6, 11.2), (19, 11.2), (19, 20.4), (5, 20.4), (5, 11.2), (2.4, 11.2)]),
                 rrect(10, 13.8, 4, 6.6, 0.8, hole=True)]
    I["data"] = [rrect(3.2, 6, 17.6, 12, 2.6), rrect(5.3, 8.1, 13.4, 7.8, 1, hole=True), rrect(7.3, 11.1, 6, 1.8, 0.9),
                 circle(16.2, 12, 1.25)]
    hexo = [at(12, 12, 9.6, -90 + 60 * k) for k in range(6)]
    hexi = [at(12, 12, 7.4, -90 + 60 * k) for k in range(6)]
    I["shader"] = [poly(hexo), poly(hexi, hole=True)] + line([(12, 12), (12, 19.4)], 1.9) + \
        line([(12, 12), at(12, 12, 7.4, -30)], 1.9) + line([(12, 12), at(12, 12, 7.4, 210)], 1.9)
    I["clock"] = ring(12, 12, 8.6, 2) + line([(12, 7.4), (12, 12), (15.4, 14)], 2)
    I["slot"] = [poly([(5.6, 3.2), (18.4, 3.2), (18.4, 20.8), (12, 16.6), (5.6, 20.8)]),
                 poly([(7.7, 5.3), (16.3, 5.3), (16.3, 16.9), (12, 14.1), (7.7, 16.9)], hole=True)]
    I["power"] = arc(12, 13, 7.6, -55, 235, W) + line([(12, 3.4), (12, 11)], W)
    I["plus"] = [rrect(10.9, 4.5, 2.2, 15, 1.1), rrect(4.5, 10.9, 15, 2.2, 1.1)]
    I["minus"] = [rrect(4.5, 10.9, 15, 2.2, 1.1)]
    I["dpad_up"] = [poly([(12, 5), (19, 15.5), (5, 15.5)])]
    I["dpad_down"] = [poly([(12, 19), (19, 8.5), (5, 8.5)])]
    I["dpad_left"] = [poly([(5, 12), (15.5, 5), (15.5, 19)])]
    I["dpad_right"] = [poly([(19, 12), (8.5, 5), (8.5, 19)])]
    I["eye"] = [poly([at(12, 21, 14.2, a) for a in range(-130, -49, 5)] + [at(12, 3, 14.2, a) for a in range(50, 131, 5)])] + \
        [circle(12, 12, 4.1, hole=True), circle(12, 12, 2.2)]
    I["eye_off"] = I["eye"] + line([(4, 4), (20, 20)], 2.2)
    I["language"] = ring(12, 12, 8.6, 1.9) + [ellipse(12, 12, 4.2, 9.55), ellipse(12, 12, 2.3, 7.65, hole=True)] + \
        [rrect(3.2, 11.05, 17.6, 1.9, 0)]
    I["volume"] = [poly([(3.5, 9), (7.5, 9), (12.5, 4.5), (12.5, 19.5), (7.5, 15), (3.5, 15)])] + \
        arc(12.5, 12, 4.4, -50, 50, 1.9) + arc(12.5, 12, 8, -55, 55, 1.9)
    # Tears of the Kingdom's actions (on-screen keys)
    def shield_shape(top, side, shoulder, bottom, inset):
        """A heater shield: flat shoulders, sides curving to a point (quadratic curves sampled)."""
        l, r = 12 - side + inset, 12 + side - inset
        pts = [(12, top + inset), (r, top + 2.6 + inset * 0.6), (r, shoulder)]
        for i in range(1, 11):   # right side down to the point
            t = i / 10
            c = (r, bottom - 4.5)
            p0, p2 = (r, shoulder), (12, bottom - inset * 1.4)
            pts.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t * t * p2[0],
                        (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t * t * p2[1]))
        for i in range(1, 10):   # left side back up
            t = i / 10
            c = (l, bottom - 4.5)
            p0, p2 = (12, bottom - inset * 1.4), (l, shoulder)
            pts.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t * t * p2[0],
                        (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t * t * p2[1]))
        pts += [(l, shoulder), (l, top + 2.6 + inset * 0.6)]
        return pts
    I["shield"] = [poly(shield_shape(2.4, 8.4, 10.5, 21.8, 0)), poly(shield_shape(2.4, 8.4, 10.8, 21.8, 2.1), hole=True),
                   rrect(11.05, 6.6, 1.9, 11.6, 0.95), rrect(7.2, 10.2, 9.6, 1.9, 0.95)]
    I["pouch"] = [circle(12, 14.6, 6.6), rrect(9.4, 6.4, 5.2, 3.6, 0.6), poly([(7.6, 3.2), (16.4, 3.2), (14.6, 6.8), (9.4, 6.8)])]
    I["note"] = [ellipse(7.2, 17.4, 2.9, 2.3), ellipse(17, 15.4, 2.9, 2.3), rrect(8.7, 5.4, 1.8, 12, 0), rrect(18.5, 3.4, 1.8, 12, 0),
                 poly([(8.7, 5.4), (20.3, 3.2), (20.3, 6.4), (8.7, 8.6)])]
    I["sword"] = [poly([(20.6, 3.4), (19.6, 7.4), (10.6, 16.4), (7.6, 13.4), (16.6, 4.4)])] + line([(5.4, 12.6), (11.4, 18.6)], 2.4) + \
        line([(8.2, 15.8), (4.4, 19.6)], 2.4) + [circle(3.7, 20.3, 1.6)]
    I["crouch"] = line([(6, 5.6), (12, 11.4), (18, 5.6)], 2.4) + line([(6, 12.4), (12, 18.2), (18, 12.4)], 2.4)
    I["scope"] = ring(7.4, 15, 4.1, 2.2) + ring(16.6, 15, 4.1, 2.2) + [rrect(4, 5.4, 5.4, 6.6, 1.4), rrect(14.6, 5.4, 5.4, 6.6, 1.4),
                                                                      rrect(9.6, 10.6, 4.8, 3, 1)]
    I["ability"] = ring(12, 12, 8.8, 1.8) + star(12, 12, 6.2, 1.9, 4, rot=-90)
    I["throw"] = [circle(5, 16.6, 2.5)] + arc(12.5, 18, 8.4, 200, 320, 2.1) + head(at(12.5, 18, 8.4, 320), 50, 4, 2.1)
    bx = 4.2
    bow_top, bow_bot = at(bx, 12, 11, -56), at(bx, 12, 11, 56)
    I["bow"] = arc(bx, 12, 11, -56, 56, 2.2) + line([bow_top, bow_bot], 1.3) + line([(5, 12), (19.5, 12)], 1.9) + \
        [poly([(22, 12), (17.6, 9.2), (17.6, 14.8)])]
    I["jump"] = line([(12, 19.4), (12, 5.6)], 2.6) + line([(5.8, 11), (12, 4.8), (18.2, 11)], 2.6) + [rrect(5.5, 20.3, 13, 1.9, 0.95)]
    I["attack"] = star(12, 12, 10, 4.4, 8, rot=-90)
    I["target"] = ring(12, 12, 6.8, 2) + line([(12, 1.8), (12, 5.6)], 2) + line([(12, 18.4), (12, 22.2)], 2) + \
        line([(1.8, 12), (5.6, 12)], 2) + line([(18.4, 12), (22.2, 12)], 2) + [circle(12, 12, 1.9)]
    I["dash"] = [poly([(13.8, 2.4), (4.6, 13.6), (10.8, 13.6), (9.4, 21.6), (19.4, 9.6), (13.2, 9.6)])]
    I["undo"] = arc(13, 13.5, 6.8, 180, 450, W, caps=False) + [circle(*at(13, 13.5, 6.8, 450), W / 2, n=16)] + \
        head(at(13, 13.5, 6.8, 180), -90, 4, W)
    I["map"] = line([(3, 6.2), (9, 4.2), (15, 6.2), (21, 4.2), (21, 17.8), (15, 19.8), (9, 17.8), (3, 19.8), (3, 6.2)], 2) + \
        line([(9, 4.6), (9, 17.6)], 1.8) + line([(15, 6.4), (15, 19.4)], 1.8)
    I["blocks"] = [rrect(3.4, 3.4, 8.6, 8.6, 2), rrect(12.6, 12.6, 8, 8, 2), rrect(14.2, 3.4, 6.4, 6.4, 1.6, hole=False),
                   rrect(3.4, 14.2, 6.4, 6.4, 1.6)]
    I["swap"] = line([(4, 8.4), (19, 8.4)], 2.1) + head((19.4, 8.4), 0, 3.8, 2.1) + line([(20, 15.6), (5, 15.6)], 2.1) + \
        head((4.6, 15.6), 180, 3.8, 2.1)
    return I


# The colourful menu badges: (tile colour, its darker base). A badge is the glyph in white on a rounded tile with a
# darker base under it (depth) and a soft highlight on its top half, as a game's menu draws its icons; it keeps its
# colours (no fillColor). 48 x 48.
BADGES = {
    "play": ("#35D07F", "#1B7A49"), "settings": ("#5B8CFF", "#2B47A8"), "save": ("#FFB443", "#B36A12"),
    "slot": ("#FFB443", "#B36A12"), "restore": ("#FFB443", "#B36A12"), "export": ("#FF9F43", "#B35C12"),
    "import": ("#FF9F43", "#B35C12"), "info": ("#9B7BFF", "#5638B3"), "gauge": ("#FF6B5B", "#A8302A"),
    "layers": ("#38C8F0", "#16769A"), "sparkle": ("#FF6BD6", "#A82E87"), "display": ("#2FD3B5", "#147A68"),
    "controller": ("#FF8A3D", "#A8481A"), "data": ("#38C8F0", "#16769A"), "shader": ("#9B7BFF", "#5638B3"),
    "touch": ("#2FD3B5", "#147A68"), "home": ("#5B8CFF", "#2B47A8"), "eye": ("#2FD3B5", "#147A68"),
    "eye_off": ("#8A94A6", "#4A5263"), "look": ("#38C8F0", "#16769A"), "sticks": ("#38C8F0", "#16769A"),
    "clock": ("#8A94A6", "#4A5263"), "warning": ("#FFC53D", "#A87A12"), "check": ("#35D07F", "#1B7A49"),
    "close": ("#FF5D6C", "#A82A36"), "language": ("#5B8CFF", "#2B47A8"), "volume": ("#FF6BD6", "#A82E87"),
    "power": ("#FF5D6C", "#A82A36"),
}


def _transformed(shapes, scale, dx, dy):
    flat = []
    for s in shapes:
        flat += s if isinstance(s, list) else [s]
    return [([(x * scale + dx, y * scale + dy) for x, y in pts], h) for pts, h in flat]


def _d(flat) -> str:
    return " ".join("M" + " L".join(f"{x:.2f} {y:.2f}" for x, y in pts) + " Z" for pts, _h in flat)


def badge_svg(shapes, color, dark) -> str:
    tile, base = rrect(2, 2, 44, 41, 13), rrect(2, 5, 44, 41, 13)
    gloss = rrect(5, 4.5, 38, 17, 10)
    glyph = _transformed(shapes, 1.3, 24 - 12 * 1.3, 23 - 12 * 1.3)
    shadow = _transformed(shapes, 1.3, 24 - 12 * 1.3, 24.7 - 12 * 1.3)
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48" width="48" height="48">'
            f'<path fill="{dark}" d="{_d([base])}"/>'
            f'<path fill="{color}" d="{_d([tile])}"/>'
            f'<path fill="#FFFFFF" fill-opacity="0.22" d="{_d([gloss])}"/>'
            f'<path fill="#000000" fill-opacity="0.22" fill-rule="nonzero" d="{_d(shadow)}"/>'
            f'<path fill="#FFFFFF" fill-rule="nonzero" d="{_d(glyph)}"/></svg>' + "\n")


def svg(shapes) -> str:
    """One path, nonzero fill, white."""
    flat = []
    for s in shapes:
        flat += s if isinstance(s, list) else [s]
    d = []
    for pts, _hole in flat:
        d.append("M" + " L".join(f"{x:.2f} {y:.2f}" for x, y in pts) + " Z")
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24">'
            f'<path fill="#FFFFFF" fill-rule="nonzero" d="{" ".join(d)}"/></svg>\n')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="entry/src/main/resources/base/media")
    ap.add_argument("--preview", help="a PNG contact sheet of every icon (needs PySide6)")
    ap.add_argument("--ets", help="also write the ArkTS lookup (icon(name): Resource), e.g. entry/src/main/ets/common/Icons.ets")
    ap.add_argument("--game-icon", help="also write the default game_icon.png (the game's own replaces it at build)")
    ap.add_argument("--accent", default="#43D3C3", help="the default game icon's colour")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    all_icons = icons()
    for name, shapes in all_icons.items():
        (out / f"ic_{name}.svg").write_bytes(svg(shapes).encode("utf-8"))
    for name, (color, dark) in BADGES.items():
        (out / f"badge_{name}.svg").write_bytes(badge_svg(all_icons[name], color, dark).encode("utf-8"))
    print(f"{len(all_icons)} icons, {len(BADGES)} badges -> {out}")
    if a.ets:
        lines = ["// Generated by tools/ui_icons.py: the shell's icons by name (ArkTS takes $r only with a literal name).",
                 "export function icon(name: string): Resource {", "  switch (name) {"]
        lines += [f"    case '{n}': return $r('app.media.ic_{n}');" for n in all_icons]
        lines += ["    default: return $r('app.media.ic_more');", "  }", "}", "",
                  "// The colourful menu badges (they keep their colours).", "export function badge(name: string): Resource {",
                  "  switch (name) {"]
        lines += [f"    case '{n}': return $r('app.media.badge_{n}');" for n in BADGES]
        lines += ["    default: return $r('app.media.badge_info');", "  }", "}", ""]
        Path(a.ets).parent.mkdir(parents=True, exist_ok=True)
        Path(a.ets).write_bytes("\n".join(lines).encode("utf-8"))
        print("ets", a.ets)
    if a.game_icon:
        from PySide6.QtCore import QByteArray, QRectF, QPointF
        from PySide6.QtGui import QColor, QGuiApplication, QImage, QLinearGradient, QPainter, QPainterPath
        from PySide6.QtSvg import QSvgRenderer
        app = QGuiApplication.instance() or QGuiApplication([])
        img = QImage(512, 512, QImage.Format_ARGB32)
        img.fill(QColor(0, 0, 0, 0))
        p = QPainter(img)
        p.setRenderHint(QPainter.Antialiasing)
        dark = QColor(a.accent).darker(190).name()
        QSvgRenderer(QByteArray(badge_svg(all_icons["controller"], a.accent, dark).encode())).render(p, QRectF(0, 0, 512, 512))
        p.end()
        img.save(a.game_icon)
        print("game icon", a.game_icon)
        del app
    if a.preview:
        from PySide6.QtCore import QByteArray, QRectF, Qt
        from PySide6.QtGui import QColor, QFont, QGuiApplication, QImage, QPainter
        from PySide6.QtSvg import QSvgRenderer
        app = QGuiApplication.instance() or QGuiApplication([])
        cols, cell, size = 10, 110, 60
        names = list(all_icons) + ["badge:" + n for n in BADGES]
        rows = (len(names) + cols - 1) // cols
        img = QImage(cols * cell, rows * cell, QImage.Format_ARGB32)
        img.fill(QColor("#10141b"))
        p = QPainter(img)
        p.setRenderHint(QPainter.Antialiasing)
        p.setFont(QFont("Microsoft YaHei", 9))
        for i, name in enumerate(names):
            x, y = (i % cols) * cell, (i // cols) * cell
            src = badge_svg(all_icons[name[6:]], *BADGES[name[6:]]) if name.startswith("badge:") else svg(all_icons[name])
            QSvgRenderer(QByteArray(src.encode())).render(p, QRectF(x + (cell - size) / 2, y + 12, size, size))
            p.setPen(QColor("#9aa4b5"))
            p.drawText(QRectF(x, y + cell - 30, cell, 20), Qt.AlignCenter, name)
        p.end()
        img.save(a.preview)
        print("preview", a.preview)
        del app
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
