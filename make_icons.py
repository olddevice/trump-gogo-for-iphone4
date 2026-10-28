#!/usr/bin/env python3
"""Build the iOS launcher icons (57x57 and 114x114) from the sprite atlas.

iOS 7 masks the corners itself, so the plate is full-bleed and opaque:
a green gradient with a soft sun glow, the front-view runner standing on
a shadow near the bottom edge.
"""
import os
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.abspath(__file__))
ATLAS = os.path.join(ROOT, "assets", "trumprun.png")
OUT1 = os.path.join(ROOT, "assets", "icon.png")        # 57x57  @1x
OUT2 = os.path.join(ROOT, "assets", "icon@2x.png")     # 114x114 @2x
PREVIEW = os.path.join(ROOT, "build", "icon_preview.png")

S = 228                     # work at 2x of the 114 pt icon, ship 114 and 57
FW, FH = 106, 74
ROW_FRONT, FRAME = 3, 2     # symmetric mid-stride front pose

im = Image.open(ATLAS).convert("RGBA")
cell = im.crop((FRAME * FW, ROW_FRONT * FH, (FRAME + 1) * FW, (ROW_FRONT + 1) * FH))
alpha = cell.getchannel("A").point(lambda v: 255 if v > 30 else 0)
hero = cell.crop(alpha.getbbox())

# ---- plate: full-bleed vertical gradient ---------------------------------
plate = Image.new("RGB", (S, S))
d = ImageDraw.Draw(plate)
for y in range(S):
    t = y / float(S - 1)
    r = int(0x7A + (0x2F - 0x7A) * t)
    g = int(0xC8 + (0x6E - 0xC8) * t)
    b = int(0x5C + (0x2A - 0x5C) * t)
    d.line([(0, y), (S, y)], fill=(r, g, b))

# ---- sun glow behind the character ---------------------------------------
glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(glow).ellipse([S * 0.10, S * 0.02, S * 0.90, S * 0.82],
                             fill=(255, 255, 255, 34))
glow = glow.filter(ImageFilter.GaussianBlur(S * 0.06))
plate = Image.alpha_composite(plate.convert("RGBA"), glow)

# ---- character -----------------------------------------------------------
TH = int(S * 0.80)
tw = int(hero.width * TH / hero.height)
hero = hero.resize((tw, TH), Image.LANCZOS)
fx, fy = (S - tw) // 2, S - int(S * 0.10) - TH

sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(sh).ellipse([fx + tw * 0.16, fy + TH - 4,
                            fx + tw * 0.84, fy + TH + 10],
                           fill=(0, 0, 0, 96))
sh = sh.filter(ImageFilter.GaussianBlur(S * 0.012))
plate = Image.alpha_composite(plate, sh)
layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
layer.paste(hero, (fx, fy))
plate = Image.alpha_composite(plate, layer)

plate.convert("RGB").resize((114, 114), Image.LANCZOS).save(OUT2)
plate.convert("RGB").resize((57, 57), Image.LANCZOS).save(OUT1)
print("icons:", OUT1, OUT2)

# preview: 57 / 114 / 114-with-mask, on a dark launcher background
prev = Image.new("RGB", (57 + 114 + 114 + 60, 140), (34, 34, 40))
p57 = Image.open(OUT1).convert("RGB")
p114 = Image.open(OUT2).convert("RGB")
prev.paste(p57, (12, 42))
prev.paste(p114, (86, 13))
mask = Image.new("L", (114, 114), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, 113, 113], radius=22, fill=255)
masked = Image.new("RGB", (114, 114), (34, 34, 40))
masked.paste(p114, (0, 0), mask)
prev.paste(masked, (216, 13))
os.makedirs(os.path.dirname(PREVIEW), exist_ok=True)
prev.save(PREVIEW)
print("preview:", PREVIEW)
