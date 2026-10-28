#!/usr/bin/env python3
"""Bake the runtime assets for the iPhone 4 build.

The device renderer is a plain C rasteriser (src/tgraster.c), so it wants raw
premultiplied pixels and an 8-bit coverage font rather than PNGs it would have
to decode with ImageIO.  Both files are little-endian and map straight into
memory, which also means the host test harness reads exactly the same bytes as
the phone.

  assets/atlas.bin   "TGAT" + u16 w, h + w*h premultiplied ARGB words
  assets/font.bin    "TGFT" + u16 cw, ch, first, count
                     + cw*count*ch coverage bytes + count advance bytes
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_ATLAS = os.path.join(ROOT, "assets", "trumprun.png")
OUT_ATLAS = os.path.join(ROOT, "assets", "atlas.bin")
OUT_FONT = os.path.join(ROOT, "assets", "font.bin")

FONT_CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
]
FONT_EM = 24          # cap height lands around 17 px -> 12 units on screen
CELL_W, CELL_H = 34, 32
BASELINE = 19         # cap top sits 2 px below the cell top, like a 12 pt line
FIRST, COUNT = 32, 95


def bake_atlas():
    img = Image.open(SRC_ATLAS).convert("RGBA")
    w, h = img.size
    data = img.tobytes()                      # RGBA, straight alpha
    out = bytearray()
    out += b"TGAT" + struct.pack("<HH", w, h)
    # premultiply and reorder RGBA -> BGRA (0xAARRGGBB in little endian)
    for i in range(0, len(data), 4):
        r, g, b, a = data[i], data[i + 1], data[i + 2], data[i + 3]
        if a != 255:
            r = (r * a + 127) // 255
            g = (g * a + 127) // 255
            b = (b * a + 127) // 255
        out += bytes((b, g, r, a))
    with open(OUT_ATLAS, "wb") as fp:
        fp.write(out)
    print("atlas.bin  %dx%d  %d bytes" % (w, h, len(out)))


def pick_font():
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            return p
    raise SystemExit("no usable system font found")


def bake_font():
    path = pick_font()
    font = ImageFont.truetype(path, FONT_EM)
    aw = CELL_W * COUNT
    img = Image.new("L", (aw, CELL_H), 0)
    draw = ImageDraw.Draw(img)
    adv = bytearray()
    for i in range(COUNT):
        ch = chr(FIRST + i)
        x = i * CELL_W
        if ch != " ":
            try:
                draw.text((x, BASELINE), ch, font=font, fill=255, anchor="ls")
            except ValueError:                 # very old Pillow
                draw.text((x, BASELINE - FONT_EM), ch, font=font, fill=255)
        try:
            a = int(round(font.getlength(ch)))
        except AttributeError:
            a = int(round(font.getsize(ch)[0]))
        adv.append(max(1, min(255, a)))

    out = bytearray()
    out += b"TGFT" + struct.pack("<HHHH", CELL_W, CELL_H, FIRST, COUNT)
    out += img.tobytes()
    out += adv
    with open(OUT_FONT, "wb") as fp:
        fp.write(out)

    # a quick look at the result, handy when tuning sizes
    prev = os.path.join(ROOT, "build", "font_preview.png")
    os.makedirs(os.path.dirname(prev), exist_ok=True)
    img.save(prev)
    print("font.bin   %dx%d cells  em=%d  %d bytes  (%s)"
          % (CELL_W, CELL_H, FONT_EM, len(out), os.path.basename(path)))
    print("           sample advance for '0' =", adv[ord("0") - FIRST],
          " 'W' =", adv[ord("W") - FIRST])


if __name__ == "__main__":
    if not os.path.exists(SRC_ATLAS):
        sys.exit("missing %s" % SRC_ATLAS)
    bake_atlas()
    bake_font()
