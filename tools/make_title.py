#!/usr/bin/env python3
"""Рисует графику WOOD: титульный экран TITLEPIC, логотип меню M_DOOM и лесное небо RSKY1.

Картинки рисуются Pillow, приводятся к палитре PLAYPAL из freedoom2.wad и сохраняются
в формате Doom picture. Шрифт — DejaVu Sans Bold (лицензия позволяет встраивать
растровые результаты).

Использование: make_title.py <freedoom2.wad> <out_dir>
(превью в PNG кладутся рядом, в <out_dir>-preview)
"""
import math
import os
import random
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    "/Library/Fonts/Arial Bold.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
]

WOOD_LIGHT = (196, 140, 76)
WOOD_DARK = (92, 56, 24)
OUTLINE = (20, 12, 4)


def font(size):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def read_playpal(wad_path):
    data = open(wad_path, "rb").read()
    _, count, diroff = struct.unpack("<4sii", data[:12])
    for i in range(count):
        off, size, name = struct.unpack("<ii8s", data[diroff + 16 * i: diroff + 16 * i + 16])
        if name.rstrip(b"\0") == b"PLAYPAL":
            raw = data[off:off + 768]
            return [tuple(raw[j:j + 3]) for j in range(0, 768, 3)]
    sys.exit("PLAYPAL не найден")


def to_palette(img, pal):
    """RGBA -> список строк индексов палитры (None — прозрачный пиксель)."""
    cache = {}
    w, h = img.size
    px = img.load()
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 128:
                row.append(None)
                continue
            key = (r, g, b)
            if key not in cache:
                cache[key] = min(range(256), key=lambda i: (pal[i][0] - r) ** 2 * 3
                                 + (pal[i][1] - g) ** 2 * 4 + (pal[i][2] - b) ** 2 * 2)
            row.append(cache[key])
        rows.append(row)
    return rows


def doom_picture(rows, left=0, top=0):
    h, w = len(rows), len(rows[0])
    columns = []
    for x in range(w):
        col = bytearray()
        y = 0
        while y < h:
            if rows[y][x] is None:
                y += 1
                continue
            start = y
            pixels = bytearray()
            while y < h and rows[y][x] is not None and len(pixels) < 128:
                pixels.append(rows[y][x])
                y += 1
            col += bytes([start, len(pixels), 0]) + pixels + b"\0"
        col += b"\xff"
        columns.append(bytes(col))
    header = struct.pack("<hhhh", w, h, left, top)
    offset = len(header) + 4 * w
    offsets = []
    for c in columns:
        offsets.append(offset)
        offset += len(c)
    return header + struct.pack(f"<{w}i", *offsets) + b"".join(columns)


def wood_text(size, text, grain_seed=1):
    """Надпись «деревянными» буквами с тёмной обводкой, RGBA."""
    f = font(size)
    bbox = f.getbbox(text)
    w, h = bbox[2] - bbox[0] + 12, bbox[3] - bbox[1] + 12
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).text((6 - bbox[0], 6 - bbox[1]), text, font=f, fill=255)

    # текстура «древесины»: горизонтальные волокна
    rnd = random.Random(grain_seed)
    grain = Image.new("RGB", (w, h))
    gp = grain.load()
    phase = [rnd.uniform(0, 6.28) for _ in range(4)]
    for y in range(h):
        for x in range(w):
            v = math.sin(y * 0.55 + math.sin(x * 0.045 + phase[0]) * 2.2 + phase[1]) * 0.5 + 0.5
            v = v * 0.7 + 0.3 * (math.sin(x * 0.013 + y * 0.09 + phase[2]) * 0.5 + 0.5)
            gp[x, y] = tuple(int(WOOD_DARK[i] + (WOOD_LIGHT[i] - WOOD_DARK[i]) * v) for i in range(3))

    outline = mask.filter(ImageFilter.MaxFilter(5))
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(Image.new("RGBA", (w, h), OUTLINE + (255,)), (0, 0), outline)
    out.paste(grain.convert("RGBA"), (0, 0), mask)
    return out


def forest_background(w, h, seed=7):
    rnd = random.Random(seed)
    img = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):  # сумеречное небо -> тёмная земля
        t = y / h
        c = (int(18 + 30 * (1 - t)), int(34 + 40 * (1 - t)), int(28 + 22 * (1 - t)))
        d.line([(0, y), (w, y)], fill=c + (255,))
    for layer, (shade, count) in enumerate([((22, 44, 26), 26), ((12, 26, 14), 18)]):
        for _ in range(count):  # силуэты елей в два слоя
            x = rnd.randint(-10, w + 10)
            base = h - rnd.randint(0, 20) - layer * 4
            height = rnd.randint(70, 140) + layer * 20
            half = height // 4
            d.polygon([(x, base - height), (x - half, base), (x + half, base)], fill=shade + (255,))
            d.rectangle([x - 2, base, x + 2, h], fill=(16, 10, 6, 255))
    return img


def make_titlepic(pal):
    img = forest_background(320, 200)
    logo = wood_text(70, "WOOD")
    img.alpha_composite(logo, ((320 - logo.width) // 2, 30))
    sub = font(11)
    d = ImageDraw.Draw(img)
    for text, y, color in (("A ROGUELIKE IN THE WOODS", 132, (220, 200, 150)),
                           ("(now read it upside down)", 150, (150, 160, 130)),
                           ("powered by Freedoom", 184, (120, 130, 110))):
        tw = d.textlength(text, font=sub)
        d.text(((320 - tw) // 2 + 1, y + 1), text, font=sub, fill=(0, 0, 0, 255))
        d.text(((320 - tw) // 2, y), text, font=sub, fill=color + (255,))
    return doom_picture(to_palette(img, pal)), img


SKY_RAMP = [11, 10, 9, 159, 158, 157, 156, 155, 154, 153, 152]  # тёмно-оливковый -> светлая дымка
BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def make_sky(pal, seed=11):
    """Лесное небо 256x128 (патч RSKY1 текстуры SKY1), бесшовное по горизонтали.

    Рисуется прямо индексами палитры: оливковая вечерняя дымка (градиент с упорядоченным
    дизерингом, иначе Freedoom-палитра превращает её в серое), облака и два слоя елей.
    Нижний край текстуры совпадает с горизонтом, поэтому деревья стоят «на земле».
    """
    w, h = 256, 128
    rnd = random.Random(seed)
    last = len(SKY_RAMP) - 1

    def ramp(pos, x, y):
        pos = max(0.0, min(last, pos))
        base = int(pos)
        frac = pos - base
        step = 1 if frac * 16 > BAYER4[y % 4][x % 4] + 0.5 else 0
        return SKY_RAMP[min(last, base + step)]

    rows = [[ramp(last * (y / (h - 1)) ** 1.3, x, y) for x in range(w)] for y in range(h)]

    # облака: вытянутые эллипсы на 2 ступени светлее неба вокруг
    for _ in range(9):
        cx, cy = rnd.randint(0, w - 1), rnd.randint(8, 56)
        rw, rh = rnd.randint(20, 46), rnd.randint(3, 7)
        for y in range(cy - rh, cy + rh + 1):
            for dx in range(-rw, rw + 1):
                if (dx / rw) ** 2 + ((y - cy) / rh) ** 2 <= 1:
                    x = (cx + dx) % w
                    rows[y][x] = ramp(last * (y / (h - 1)) ** 1.3 + 2.5, x, y)

    # два слоя елей: дальний — серо-зелёный, ближний — почти чёрный
    layers = []
    for color, count, hmin, hmax in ((157, 34, 22, 40), (12, 24, 34, 62)):
        mask = Image.new("L", (w * 3, h), 0)
        d = ImageDraw.Draw(mask)
        for _ in range(count):
            x = rnd.randint(0, w - 1) + w
            height = rnd.randint(hmin, hmax)
            half = max(4, height // 4)
            for k in range(3):  # ярусы ели
                top = h - height + k * height // 4
                base = h - height // 3 + k * height // 6
                hw = half * (k + 2) // 3
                for dx in (-w, 0, w):
                    d.polygon([(x + dx, top), (x + dx - hw, base), (x + dx + hw, base)], fill=255)
            for dx in (-w, 0, w):
                d.rectangle([x + dx - half, h - height // 3, x + dx + half, h], fill=255)
        layers.append((color, mask.crop((w, 0, 2 * w, h)).load()))
    for color, m in layers:
        for y in range(h):
            for x in range(w):
                if m[x, y]:
                    rows[y][x] = color

    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = pal[rows[y][x]]
    return doom_picture(rows), img


def make_menu_logo(pal):
    logo = wood_text(48, "WOOD")
    # меню рисует M_DOOM по центру сверху; смещение 0,0 как у оригинала
    return doom_picture(to_palette(logo, pal)), logo


def main():
    src, out_dir = sys.argv[1:3]
    os.makedirs(out_dir, exist_ok=True)
    pal = read_playpal(src)
    title, title_img = make_titlepic(pal)
    menu, menu_img = make_menu_logo(pal)
    open(os.path.join(out_dir, "titlepic.lmp"), "wb").write(title)
    open(os.path.join(out_dir, "m_doom.lmp"), "wb").write(menu)
    sky, sky_img = make_sky(pal)
    open(os.path.join(out_dir, "rsky1.lmp"), "wb").write(sky)
    preview_dir = out_dir.rstrip("/") + "-preview"
    os.makedirs(preview_dir, exist_ok=True)
    title_img.save(os.path.join(preview_dir, "titlepic.png"))
    menu_img.save(os.path.join(preview_dir, "m_doom.png"))
    sky_img.resize((1024, 512), Image.NEAREST).save(os.path.join(preview_dir, "rsky1.png"))
    print(f"title: TITLEPIC {len(title)} B, M_DOOM {menu_img.size}")


if __name__ == "__main__":
    main()
