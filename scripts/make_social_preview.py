"""Builds docs/social-preview.png (1280x640) for the GitHub repository card."""

import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 1280, 640
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAINTING = os.path.join(ROOT, "assets", "rivage-waterlilies.jpg")
SHOT = os.path.join(ROOT, "docs", "rivage-ui.png")
OUT = os.path.join(ROOT, "docs", "social-preview.jpg")

INK = (17, 46, 49)
MUTED = (64, 91, 94)
ACCENT = (0, 104, 100)
PAPER = (238, 249, 247)


def font(name, size):
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)


def cover(image, width, height):
    scale = max(width / image.width, height / image.height)
    resized = image.resize((int(image.width * scale + 0.5), int(image.height * scale + 0.5)),
                           Image.LANCZOS)
    left = (resized.width - width) // 2
    top = (resized.height - height) // 2
    return resized.crop((left, top, left + width, top + height))


def tracked(draw, xy, text, fnt, fill, tracking=0.0):
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=fnt, fill=fill)
        x += draw.textlength(ch, font=fnt) + tracking
    return x


def rounded(image, radius):
    mask = Image.new("L", image.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, image.width - 1, image.height - 1),
                                           radius=radius, fill=255)
    out = image.convert("RGBA")
    out.putalpha(mask)
    return out


def main():
    base = cover(Image.open(PAINTING).convert("RGB"), W, H)

    # A paper wash that is opaque on the left (text) and clears towards the painting.
    wash = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    wash_draw = ImageDraw.Draw(wash)
    for x in range(W):
        t = x / (W - 1)
        alpha = int(247 * max(0.0, 1.0 - t / 0.78) ** 1.25)
        wash_draw.line([(x, 0), (x, H)], fill=PAPER + (alpha,))
    canvas = Image.alpha_composite(base.convert("RGBA"), wash)

    # Soft white paper card behind the screenshot.
    shot = Image.open(SHOT).convert("RGB")
    target_w = 720
    shot = shot.resize((target_w, int(shot.height * target_w / shot.width)), Image.LANCZOS)
    card_pad = 14
    card = Image.new("RGBA", (shot.width + card_pad * 2, shot.height + card_pad * 2),
                     (255, 255, 255, 236))
    card = rounded(card, 22)
    card.paste(rounded(shot, 14), (card_pad, card_pad), rounded(shot, 14))

    shadow = Image.new("RGBA", (card.width + 80, card.height + 80), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((40, 46, shadow.width - 40, shadow.height - 30),
                                             radius=26, fill=(20, 60, 62, 78))
    shadow = shadow.filter(ImageFilter.GaussianBlur(18))

    shot_x = W - card.width - 58
    shot_y = (H - card.height) // 2 + 6
    canvas.alpha_composite(shadow, (shot_x - 40, shot_y - 40))
    canvas.alpha_composite(card, (shot_x, shot_y))

    draw = ImageDraw.Draw(canvas)

    # Brand block.
    x = 76
    tracked(draw, (x, 84), "RIVAGE", font("georgia.ttf", 76), INK, tracking=5.0)
    rule_y = 186
    draw.line([(x + 2, rule_y), (x + 104, rule_y)], fill=(*INK, 150), width=2)
    tracked(draw, (x + 122, rule_y - 11), "REVERB", font("segoeui.ttf", 20), MUTED, tracking=6.0)

    draw.text((x, 226), "Six-machine algorithmic", font=font("georgia.ttf", 33), fill=INK)
    draw.text((x, 268), "reverb for electric guitar", font=font("georgia.ttf", 33), fill=INK)

    draw.text((x, 330),
              "ROOM   PLATE   HALL   SPRING   SHIMMER   CLOUD",
              font=font("consola.ttf", 14), fill=ACCENT)

    draw.text((x, 378), "JUCE  /  VST3  ·  Windows x64  ·  v0.1.0", font=font("segoeui.ttf", 17), fill=MUTED)

    draw.line([(x, 448), (x + 104, 448)], fill=(*INK, 90), width=2)
    draw.text((x, 466), "WTdusk", font=font("segoeui.ttf", 19), fill=INK)
    draw.text((x, 494), "AGPL-3.0-or-later", font=font("segoeui.ttf", 14), fill=MUTED)

    canvas.convert("RGB").save(OUT, quality=92, optimize=True, progressive=True)
    print("written", OUT, canvas.size)


if __name__ == "__main__":
    main()
