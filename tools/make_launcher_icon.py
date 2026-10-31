"""Draws the launcher's icon, src/launcher/ervr.ico: the emblem the window draws in its header (ui.cpp's
App::emblem) - a gold ring holding two lenses (an image for each eye) and the sword between them.

Each size is drawn at 8x and scaled down, so the thin lines stay smooth. Needs Pillow:
    python tools/make_launcher_icon.py
"""
import math
import pathlib

from PIL import Image, ImageDraw

GROUND = (32, 29, 24, 255)
GOLD = (201, 164, 90, 255)
GOLD_LIFT = (230, 196, 122, 255)
GOLD_DEEP = (150, 117, 58, 255)
SIZES = (16, 24, 32, 48, 64, 128, 256)
OUT = pathlib.Path(__file__).resolve().parent.parent / "src" / "launcher" / "ervr.ico"


def emblem(size):
    s = size * 8
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(image)
    c = s / 2
    r = s * 0.48
    # Small sizes get heavier lines, or the ring and lenses vanish.
    line = max(r * (0.11 if size <= 32 else 0.075), 8)

    def circle(cx, cy, radius, fill=None, outline=None, width=0):
        d.ellipse((cx - radius, cy - radius, cx + radius, cy + radius), fill=fill, outline=outline, width=int(width))

    circle(c, c, r, fill=GROUND)
    circle(c, c, r, outline=GOLD, width=line)
    if size >= 48:
        circle(c, c, r * 0.8, outline=GOLD_DEEP, width=max(line * 0.45, 4))
    lens, dx, ly = r * 0.24, r * 0.4, c + r * 0.04
    circle(c - dx, ly, lens, outline=GOLD_LIFT, width=line)
    circle(c + dx, ly, lens, outline=GOLD_LIFT, width=line)
    top, guard, tip, blade = c - r * 0.62, c - r * 0.36, c + r * 0.64, max(r * 0.075, line * 0.6)
    circle(c, top, line * 0.9, fill=GOLD)
    d.line((c, top, c, guard), fill=GOLD, width=int(line))
    d.line((c - r * 0.2, guard, c + r * 0.2, guard), fill=GOLD, width=int(line * 1.1))
    shoulder = tip - r * 0.14
    d.polygon([(c - blade, guard), (c + blade, guard), (c + blade * 0.6, shoulder), (c, tip), (c - blade * 0.6, shoulder)],
              fill=GOLD_LIFT)
    return image.resize((size, size), Image.LANCZOS)


def main():
    images = [emblem(size) for size in SIZES]
    images[-1].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    print(f"wrote {OUT} ({', '.join(str(s) for s in SIZES)} px)")


if __name__ == "__main__":
    main()
