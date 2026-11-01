"""Draws the Phantom Co. icon (the classic ghost of Web_Ui/ghosts.js on a dark rounded square)
and writes phantom_co.ico (16-256 px, PNG inside) and icon_preview.png next to this file.
Needs Pillow:  python -X utf8 make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
BIG = 1024


def bezier(p0, p1, p2, p3=None, n=40):
    pts = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        if p3 is None:  # quadratic
            x = u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0]
            y = u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]
        else:
            x = u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0]
            y = u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]
        pts.append((x, y))
    return pts


def ghost_outline():
    """Same path as BODY in ghosts.js (120 x 140 box)."""
    pts = [(18, 74)]
    pts += bezier((18, 74), (18, 38), (36, 14), (60, 14))
    pts += bezier((60, 14), (84, 14), (102, 38), (102, 74))
    pts += [(102, 114)]
    pts += bezier((102, 114), (92, 132), (82, 114))
    pts += bezier((82, 114), (71, 98), (60, 114))
    pts += bezier((60, 114), (49, 130), (38, 114))
    pts += bezier((38, 114), (28, 98), (18, 114))
    return pts


def lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def render():
    s = BIG
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # dark rounded square with a soft vertical gradient
    bg = Image.new("RGBA", (s, s))
    d = ImageDraw.Draw(bg)
    for y in range(s):
        d.line([(0, y), (s, y)], fill=lerp((38, 28, 74), (14, 11, 26), y / s) + (255,))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.22), fill=255)
    img.paste(bg, (0, 0), mask)

    # ghost: scale the 120 x 140 design box so the body fills ~78 % of the height
    k = s * 0.78 / 118
    ox = s / 2 - 60 * k
    oy = s / 2 - 73 * k + s * 0.01
    tr = lambda p: (ox + p[0] * k, oy + p[1] * k)

    glow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(glow).polygon([tr(p) for p in ghost_outline()], fill=(182, 156, 255, 120))
    from PIL import ImageFilter
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(s * 0.035)))

    # body gradient (light top -> lavender -> darker bottom), clipped to the outline
    body = Image.new("RGBA", (s, s))
    bd = ImageDraw.Draw(body)
    top, mid, bot = (222, 208, 255), (182, 156, 255), (150, 126, 214)
    y0, y1 = tr((0, 14))[1], tr((0, 132))[1]
    for y in range(s):
        t = min(1, max(0, (y - y0) / (y1 - y0)))
        c = lerp(top, mid, t / 0.55) if t < 0.55 else lerp(mid, bot, (t - 0.55) / 0.45)
        bd.line([(0, y), (s, y)], fill=c + (255,))
    bmask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(bmask).polygon([tr(p) for p in ghost_outline()], fill=255)
    # little arms
    arms = ImageDraw.Draw(bmask)
    for cx, cy, rot in ((13, 86, 35), (107, 86, -35)):
        arm = Image.new("L", (s, s), 0)
        ImageDraw.Draw(arm).ellipse([tr((cx - 10, cy - 6.5)), tr((cx + 10, cy + 6.5))], fill=255)
        arm = arm.rotate(-rot, center=tr((cx, cy)), resample=Image.BICUBIC)
        bmask.paste(255, (0, 0), arm)
    img.paste(body, (0, 0), bmask)

    # highlight, blush, eyes, mouth
    o = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    od = ImageDraw.Draw(o)
    od.ellipse([tr((30, 32)), tr((58, 48))], fill=(255, 255, 255, 60))
    for cx in (35, 85):
        od.ellipse([tr((cx - 6, 76.4)), tr((cx + 6, 83.6))], fill=(255, 122, 168, 85))
    for cx in (44, 76):
        od.ellipse([tr((cx - 6.5, 51)), tr((cx + 6.5, 71))], fill=(27, 22, 48, 255))
        od.ellipse([tr((cx - 2.6, 54)), tr((cx + 1.2, 58.2))], fill=(255, 255, 255, 235))
    od.arc([tr((52, 70)), tr((68, 86))], 20, 160, fill=(27, 22, 48, 255), width=int(k * 2.6))
    img.alpha_composite(o)
    return img


def main():
    big = render()
    big.save(HERE / "icon_preview.png")
    sizes = [256, 128, 64, 48, 32, 24, 16]
    frames = [big.resize((n, n), Image.LANCZOS) for n in sizes]
    frames[0].save(HERE / "phantom_co.ico", format="ICO", sizes=[(n, n) for n in sizes], append_images=frames[1:])
    print("written:", HERE / "phantom_co.ico")


if __name__ == "__main__":
    main()
