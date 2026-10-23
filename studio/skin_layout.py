"""
THE HYBRID CORE — faceplate layout (single source of truth).

Every live control's position on the faceplate art, in ORIGINAL IMAGE pixels
(skin_original.jpg is 1438 x 2576). This script:
  1. cuts the painted knobs, meters, screens and readouts out of the art
     (fills them with the surrounding panel colour) -> assets/skin.jpg
  2. writes skin_layout.hpp so the C++ app places its real controls on the
     exact same spots.
Run:  python3 studio/skin_layout.py
"""
from PIL import Image, ImageDraw, ImageFilter
import os, statistics

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "assets", "skin_original.jpg")
DST = os.path.join(HERE, "assets", "skin.jpg")
HPP = os.path.join(HERE, "skin_layout.hpp")

# ---- knobs: (name, cx, cy, radius_of_cutout) --------------------------------
KNOBS = [
    ("dsThreshold", 303, 1623, 46), ("dsBleed", 476, 1623, 46), ("dsDecay", 597, 1623, 46),
    ("rsSensitivity", 838, 1623, 46), ("rsFrequency", 958, 1623, 46), ("rsBandwidth", 1078, 1623, 46),
    ("rsReduction", 1198, 1623, 46), ("rsCompensation", 1316, 1623, 46),
    ("trAttack", 117, 2052, 54), ("trSustain", 265, 2052, 54), ("trGain", 462, 2052, 60), ("trMix", 600, 2052, 54),
    ("msCeiling", 1228, 1848, 42), ("msThreshold", 1330, 1848, 42),
    ("msRelease", 1228, 2072, 45), ("msWidth", 1330, 2072, 45),
]
# ---- rectangles: (name, x0, y0, x1, y1) -------------------------------------
RECTS = [
    # transport
    ("timeDisplay", 420, 578, 756, 627), ("tempoDisplay", 768, 578, 874, 627), ("levelMeter", 893, 579, 1150, 627),
    # arrange
    ("ruler", 322, 655, 1362, 686), ("lanes", 322, 688, 1350, 1245), ("names", 104, 688, 238, 1245),
    # header selectors (text boxes)
    ("dsHeaderBox", 509, 1324, 608, 1352), ("rsHeaderBox", 1162, 1324, 1314, 1352), ("msHeaderBox", 1164, 1750, 1310, 1774),
    # screens
    ("dsScreen", 82, 1375, 650, 1571), ("rsScreen", 794, 1376, 1358, 1568),
    ("trAttackScreen", 76, 1796, 354, 1978), ("trSustainScreen", 366, 1796, 651, 1978),
    # destroyer resolution selector box, bypass toggle
    ("dsResBox", 123, 1614, 186, 1646), ("dsToggle", 375, 1612, 405, 1648),
    # transient punch/speed switch + LED
    ("trSwitch", 348, 2036, 378, 2068), ("trLed", 596, 1746, 646, 1770),
    # mastering meters + readouts
    ("msMeters", 792, 1824, 1010, 2100),
    ("msIntegrated", 1022, 1832, 1146, 1868), ("msShortTerm", 1022, 1905, 1146, 1941),
    ("msPsr", 1022, 1984, 1146, 2020), ("msPlr", 1022, 2057, 1146, 2093),
    ("msPlrLabel", 1052, 2031, 1118, 2051), ("msFooterLabel", 1052, 2101, 1118, 2122),
    ("msModeBox", 1192, 1935, 1268, 1961), ("msTargetBox", 1296, 1935, 1368, 1961), ("msTargetLabel", 1286, 1986, 1376, 2007),
]
# ---- clickable regions that keep their painted look -------------------------
HOTSPOTS = [
    ("btnMenu", 76, 145, 131, 200), ("btnRewind", 141, 145, 198, 200), ("btnPlay", 208, 145, 265, 200),
    ("btnStop", 274, 145, 331, 200), ("btnRecord", 341, 145, 398, 200),
    ("btnAnalyze", 1163, 145, 1233, 200), ("btnSettings", 1243, 145, 1298, 200), ("btnView", 1306, 145, 1360, 200),
    ("btnTopLeft", 80, 96, 116, 126), ("btnTopRight", 1322, 96, 1358, 126),
    ("dsPrev", 467, 1322, 497, 1354), ("dsNext", 620, 1322, 652, 1354), ("dsTitle", 100, 1316, 440, 1356),
    ("rsPrev", 1120, 1322, 1148, 1354), ("rsNext", 1326, 1322, 1356, 1354), ("rsTitle", 798, 1316, 1060, 1356),
    ("dsResPrev", 82, 1613, 110, 1647), ("dsResNext", 198, 1613, 226, 1647),
    ("trTitle", 90, 1740, 340, 1780),
    ("msPrev", 1122, 1748, 1150, 1776), ("msNext", 1332, 1748, 1362, 1776), ("msTitle", 798, 1740, 1060, 1780),
]
# transport hotspots above are in "top crop" coordinates; shift into image space
HOTSPOTS = [(n, x0, y0 + 430, x1, y1 + 430) if n.startswith("btn") else (n, x0, y0, x1, y1)
            for (n, x0, y0, x1, y1) in HOTSPOTS]
# track rows: centre y of row i = ROW0 + i * ROW_STEP
ROW0, ROW_STEP, ROW_H = 707.0, 42.83, 40.0
ROW_M = (243, 271)   # x range of the painted M box
ROW_S = (276, 305)   # x range of the painted S box
VIEWS = {"full": (0, 0, 1438, 2576), "console": (0, 455, 1438, 2215)}


def ring_median(im, cx, cy, r):
    px = im.load(); w, h = im.size; vals = []
    for rr in (r + 4, r + 7, r + 10):
        for a in range(0, 360, 6):
            import math
            x = int(cx + rr * math.cos(math.radians(a))); y = int(cy + rr * math.sin(math.radians(a)))
            if 0 <= x < w and 0 <= y < h: vals.append(px[x, y])
    return tuple(int(statistics.median(c[i] for c in vals)) for i in range(3))


def border_median(im, x0, y0, x1, y1):
    px = im.load(); w, h = im.size; vals = []
    for d in (2, 4):
        for x in range(x0, x1, 3):
            for y in (y0 - d, y1 + d):
                if 0 <= y < h: vals.append(px[x, y])
        for y in range(y0, y1, 3):
            for x in (x0 - d, x1 + d):
                if 0 <= x < w: vals.append(px[x, y])
    return tuple(int(statistics.median(c[i] for c in vals)) for i in range(3))


SCREEN_LIKE = {"dsScreen", "rsScreen", "trAttackScreen", "trSustainScreen", "msMeters", "lanes", "ruler",
               "timeDisplay", "tempoDisplay", "levelMeter", "msIntegrated", "msShortTerm", "msPsr", "msPlr",
               "msModeBox", "msTargetBox", "dsHeaderBox", "rsHeaderBox", "msHeaderBox", "dsResBox"}


def build_skin():
    im = Image.open(SRC).convert("RGB")
    mask = Image.new("L", im.size, 0); md = ImageDraw.Draw(mask)
    fill = Image.new("RGB", im.size); fd = ImageDraw.Draw(fill)
    for name, cx, cy, r in KNOBS:
        col = ring_median(im, cx, cy, r)
        md.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
        fd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)
    for name, x0, y0, x1, y1 in RECTS:
        col = (7, 11, 15) if name in SCREEN_LIKE else border_median(im, x0, y0, x1, y1)
        md.rectangle([x0, y0, x1, y1], fill=255)
        fd.rectangle([x0, y0, x1, y1], fill=col)
    mask = mask.filter(ImageFilter.GaussianBlur(1.5))
    out = Image.composite(fill, im, mask)
    out.save(DST, quality=93, subsampling=0)
    return out


def write_header():
    L = ["// GENERATED by studio/skin_layout.py — do not edit by hand.",
         "// Positions of live controls on the faceplate art, in original image pixels.",
         "#pragma once", "namespace hc::studio::layout {",
         "struct Knob { float cx, cy, r; };", "struct Box { float x0, y0, x1, y1; };",
         "inline constexpr float kImageW = 1438.0f, kImageH = 2576.0f;"]
    for n, cx, cy, r in KNOBS:
        L.append(f"inline constexpr Knob {n}{{{cx}.0f, {cy}.0f, {r}.0f}};")
    for n, x0, y0, x1, y1 in RECTS + HOTSPOTS:
        L.append(f"inline constexpr Box {n}{{{x0}.0f, {y0}.0f, {x1}.0f, {y1}.0f}};")
    L.append(f"inline constexpr float kRow0 = {ROW0}f, kRowStep = {ROW_STEP}f, kRowH = {ROW_H}f;")
    L.append(f"inline constexpr Box rowM{{{ROW_M[0]}.0f, 0.0f, {ROW_M[1]}.0f, 0.0f}}, rowS{{{ROW_S[0]}.0f, 0.0f, {ROW_S[1]}.0f, 0.0f}};")
    for n, (x0, y0, x1, y1) in VIEWS.items():
        L.append(f"inline constexpr Box view_{n}{{{x0}.0f, {y0}.0f, {x1}.0f, {y1}.0f}};")
    L.append("} // namespace hc::studio::layout")
    open(HPP, "w").write("\n".join(L) + "\n")


if __name__ == "__main__":
    build_skin()
    write_header()
    print("wrote", DST, "and", HPP)
