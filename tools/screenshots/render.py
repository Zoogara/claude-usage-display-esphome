#!/usr/bin/env python3
"""Redraw claude-usage-display.yaml's display lambda with FICTIONAL data.

This is a layout mock-up, not a capture of the real firmware: coordinates,
colours and thresholds mirror the lambda, drawn with Pillow. Keep it in sync
when the lambda changes. Needs Pillow; Roboto is used if found (CLAUDE_FONT
can point at a Roboto .ttf), otherwise a system sans-serif.

    python3 tools/screenshots/render.py [out.png]
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H, SCALE = 480, 320, 2

FONT_CANDIDATES = [
    os.environ.get("CLAUDE_FONT", ""),
    "/Applications/Android Studio.app/Contents/plugins/design-tools/resources/layoutlib/data/fonts/Roboto-Regular.ttf",
    "/usr/share/fonts/truetype/roboto/Roboto-Regular.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]


def load_font(size, bold):
    for p in FONT_CANDIDATES:
        if p and os.path.exists(p):
            f = ImageFont.truetype(p, size)
            if bold:
                try:  # variable Roboto: pick the named Bold instance
                    f.set_variation_by_name("Bold")
                except Exception:
                    pass  # static font: keep as is
            return f
    return ImageFont.load_default()


FONT_BIG, FONT_MED, FONT_SMALL = load_font(30, True), load_font(20, True), load_font(14, False)

# --- fictional demo data ---
DATA = {
    "plan": "Max",
    "session": (37, "2h 05m"),
    "weekly": (72, "Fri 09:00"),
    "rows": [("Claude Code", 42), ("Chats", 18), ("Cowork", 36), ("Other", 4)],
    "status": "OK",
}

WHITE, GREY, DIM = (255, 255, 255), (150, 150, 150), (70, 70, 70)


def render():
    im = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(im)

    def text(x, y, s, font, color, align="l"):
        d.text((x, y), s, font=font, fill=color, anchor=align + "a")

    def rect(x, y, w, h, color):  # ESPHome rectangle(x, y, w, h)
        d.rectangle([x, y, x + w - 1, y + h - 1], outline=color)

    def fill(x, y, w, h, color):
        d.rectangle([x, y, x + w - 1, y + h - 1], fill=color)

    text(16, 4, "CLAUDE", FONT_BIG, WHITE)
    text(464, 12, DATA["plan"], FONT_MED, GREY, "r")
    d.line([(16, 44), (464, 44)], fill=DIM)

    def block(y, label, accent, pct, detail):
        bar = (80, 200, 120) if pct < 60 else (255, 180, 60) if pct < 85 else (255, 90, 90)
        text(16, y + 2, label, FONT_MED, accent)
        text(464, y - 2, f"{pct:.0f}%", FONT_BIG, WHITE, "r")
        rect(16, y + 38, 448, 14, DIM)
        w = int(min(pct, 100)) * 444 // 100
        if w > 0:
            fill(18, y + 40, w, 10, bar)
        text(16, y + 58, detail, FONT_SMALL, GREY)

    block(52, "Session (5h)", (255, 180, 60), DATA["session"][0], "resets in " + DATA["session"][1])
    d.line([(16, 136), (464, 136)], fill=(40, 40, 40))
    block(142, "Weekly (7d)", (90, 140, 255), DATA["weekly"][0], "resets " + DATA["weekly"][1])
    d.line([(16, 226), (464, 226)], fill=(40, 40, 40))

    text(16, 234, "Weekly by source", FONT_MED, (190, 130, 255))
    rows = DATA["rows"]
    total = sum(p for _, p in rows)
    pal = [(217, 119, 87), (90, 140, 255), (80, 200, 120), (150, 150, 150)]
    rect(16, 270, 448, 14, DIM)
    cum, lx = 0.0, 16
    for i, (name, p) in enumerate(rows):
        c = pal[i % 4]
        if p <= 0:
            continue
        x0 = 18 + int(cum * 444 / total)
        cum += p
        x1 = 18 + int(cum * 444 / total)
        fill(x0, 272, x1 - x0, 10, c)
        t = f"{name} {p:.0f}%"
        fill(lx, 296, 8, 8, c)
        text(lx + 12, 292, t, FONT_SMALL, (200, 200, 200))
        lx += int(FONT_SMALL.getlength(t)) + 28

    if DATA["status"] not in ("OK", "Signed in"):
        text(464, 4, DATA["status"], FONT_SMALL, (255, 90, 90), "r")
    return im.resize((W * SCALE, H * SCALE), Image.NEAREST)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(__file__), "..", "..", "docs", "screenshot.png")
    render().save(out, optimize=True)
    print("wrote", os.path.abspath(out))
