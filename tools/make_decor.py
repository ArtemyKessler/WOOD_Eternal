#!/usr/bin/env python3
"""Рисует лесной декор WOOD: валуны, камни, кусты, траву, цветы, грибы.

В Freedoom всего три «природных» спрайта (TRE1, TRE2, SMIT), поэтому недостающее рисуем сами:
пиксели берём из лесных текстур Freedoom (листва A-CAMO*, мшистый камень SP_ROCK1), форму и
объём задаём процедурно, а затенение делаем через COLORMAP — так цвета остаются в палитре.

Новые картинки подменяют спрайты декораций Doom II, которые в WOOD не используются
(номера вещей — doomednum, их ставит gen_forest.py):
    30 COL1 -> валун            (твёрдый)
    31 COL2 -> камень           (твёрдый)
    32 COL3 -> большой куст     (твёрдый)
    33 COL4 -> малый куст       (твёрдый)
    79 POB1 -> пучок травы      (проходимый)
    80 POB2 -> цветы            (проходимый)
    81 BRS1 -> грибы            (проходимый)
    48 ELEC -> золотое дерево   (твёрдое; директор ставит его вместо обычного, см. golden_tree)

Использование: make_decor.py <freedoom2.wad> <out_dir>
(лампы <имя>.lmp кладутся в out_dir, превью PNG — в <out_dir>-preview)
"""
import math
import os
import random
import struct
import sys

from PIL import Image

# --- чтение WAD ---


def read_wad(path):
    data = open(path, "rb").read()
    _, count, diroff = struct.unpack("<4sii", data[:12])
    lumps = {}
    for i in range(count):
        off, size, name = struct.unpack("<ii8s", data[diroff + 16 * i: diroff + 16 * i + 16])
        lumps.setdefault(name.rstrip(b"\0").decode("ascii").upper(), data[off:off + size])
    return lumps


def patch_pixels(data):
    """Doom picture -> (w, h, {(x, y): индекс})."""
    w, h = struct.unpack("<hh", data[:4])
    px = {}
    for x in range(w):
        off = struct.unpack("<i", data[8 + 4 * x: 12 + 4 * x])[0]
        while data[off] != 255:
            top, n = data[off], data[off + 1]
            for j in range(n):
                px[(x, top + j)] = data[off + 3 + j]
            off += n + 4
    return w, h, px


def texture_grid(lumps, name):
    """Составная текстура -> двумерный список индексов палитры."""
    pn = lumps["PNAMES"]
    pnames = [pn[4 + 8 * i: 12 + 8 * i].rstrip(b"\0").decode().upper()
              for i in range(struct.unpack("<i", pn[:4])[0])]
    for tl in ("TEXTURE1", "TEXTURE2"):
        t = lumps.get(tl)
        if not t:
            continue
        cnt = struct.unpack("<i", t[:4])[0]
        for o in struct.unpack(f"<{cnt}i", t[4:4 + 4 * cnt]):
            if t[o:o + 8].rstrip(b"\0").decode().upper() != name:
                continue
            w, h = struct.unpack("<hh", t[o + 12:o + 16])
            grid = [[0] * w for _ in range(h)]
            for k in range(struct.unpack("<h", t[o + 20:o + 22])[0]):
                ox, oy, pi = struct.unpack("<hhh", t[o + 22 + 10 * k:o + 28 + 10 * k])
                _, _, px = patch_pixels(lumps[pnames[pi]])
                for (x, y), c in px.items():
                    if 0 <= ox + x < w and 0 <= oy + y < h:
                        grid[oy + y][ox + x] = c
            return grid
    sys.exit(f"текстура {name} не найдена")


# --- запись ---


def doom_picture(rows, left, top):
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


class Canvas:
    def __init__(self, w, h, colormap):
        self.w, self.h = w, h
        self.px = [[None] * w for _ in range(h)]
        self.cm = colormap

    def put(self, x, y, idx, shade=0):
        """shade: 0 — как есть, 31 — почти чёрный (уровни COLORMAP)."""
        if 0 <= x < self.w and 0 <= y < self.h:
            s = max(0, min(31, int(shade)))
            self.px[y][x] = self.cm[s * 256 + idx]

    def lump(self, sink=0):
        """Спрайт стоит на полу: центр по X, низ на уровне пола (sink — утопить в землю)."""
        return doom_picture(self.px, self.w // 2, self.h - sink)

    def image(self, pal):
        img = Image.new("RGBA", (self.w, self.h), (0, 0, 0, 0))
        for y, row in enumerate(self.px):
            for x, c in enumerate(row):
                if c is not None:
                    img.putpixel((x, y), pal[c] + (255,))
        return img


def brightness(pal, i):
    r, g, b = pal[i]
    return 0.3 * r + 0.59 * g + 0.11 * b


# --- формы ---


def blob(cv, src, rng, circles, light=(-0.6, -0.8), base_shade=0, rim=10, edge_noise=0.0,
         flat_bottom=True):
    """Объёмное «облако» из кругов, заполненное пикселями текстуры src.

    circles: [(cx, cy, r)] или [(cx, cy, rx, ry)] — эллипсы. Затенение — по нормали
    псевдосферы относительно света light.
    """
    circles = [c if len(c) == 4 else (c[0], c[1], c[2], c[2]) for c in circles]
    th, tw = len(src), len(src[0])
    ox, oy = rng.randrange(tw), rng.randrange(th)
    lx, ly = light
    ln = math.hypot(lx, ly, 1.0)
    for y in range(cv.h):
        for x in range(cv.w):
            best = None
            for cx, cy, rx, ry in circles:
                d = math.hypot((x + 0.5 - cx) / rx, (y + 0.5 - cy) / ry)
                if d < 1 and (best is None or d < best[0]):
                    best = (d, cx, cy, rx, ry)
            if best is None:
                continue
            d, cx, cy, rx, ry = best
            if edge_noise and d > 1 - edge_noise and rng.random() < (d - 1 + edge_noise) / edge_noise:
                continue
            nx, ny = (x + 0.5 - cx) / rx, (y + 0.5 - cy) / ry
            nz = math.sqrt(max(0.0, 1 - nx * nx - ny * ny))
            lam = (nx * lx + ny * ly + nz) / ln          # 1 — к свету, <0 — в тени
            shade = base_shade + (1 - lam) * rim
            if flat_bottom:
                shade += max(0, (y - cv.h * 0.6) / (cv.h * 0.4)) * 8   # низ темнее (тень у земли)
            cv.put(x, y, src[(y + oy) % th][(x + ox) % tw], shade)


def boulder(cm, rock, moss, rng, w, h, lumps_of_moss=True):
    cv = Canvas(w, h, cm)
    circles = []
    n = rng.randint(3, 4)
    for i in range(n):
        t = (i + 0.5) / n
        k = 1 - 0.45 * abs(t - 0.5) * 2              # к краям ниже
        ry = (h - 1) * rng.uniform(0.5, 0.56) * k
        rx = min(w * rng.uniform(0.26, 0.34), w * 0.5)
        cx = min(max(w * (0.2 + 0.6 * t), rx), w - rx)
        circles.append((cx, h - ry * 0.9, rx, ry))
    blob(cv, rock, rng, circles, base_shade=1, rim=18)
    if lumps_of_moss:   # мох на макушке
        th, tw = len(moss), len(moss[0])
        for x in range(w):
            top = next((y for y in range(h) if cv.px[y][x] is not None), None)
            if top is None:
                continue
            depth = int(rng.uniform(1, 4) + 3 * math.sin(x * 0.35 + rng.random()))
            for y in range(top, min(h, top + max(0, depth))):
                cv.put(x, y, moss[y % th][x % tw], 2 + (y - top))
    return cv


def bush(cm, leaves, rng, w, h):
    cv = Canvas(w, h, cm)
    circles = []
    for _ in range(rng.randint(8, 11)):
        r = rng.uniform(0.17, 0.26) * w
        r = min(r, h * 0.45)
        cx = rng.uniform(r, w - r)
        # купол: в середине выше, по краям ниже
        top = r + (h - 2 * r) * 0.55 * (abs(cx / w - 0.5) * 2) ** 1.5
        cy = rng.uniform(top, max(top, h - r * 0.6))
        circles.append((cx, cy, r))
    blob(cv, leaves, rng, circles, base_shade=0, rim=16, edge_noise=0.22)
    return cv


def grass_tuft(cm, greens, rng, w, h, flowers=None):
    cv = Canvas(w, h, cm)
    for _ in range(rng.randint(18, 26)):
        bx = rng.uniform(w * 0.15, w * 0.85)
        length = rng.uniform(h * 0.45, h * 0.98)
        lean = rng.uniform(-0.5, 0.5) + (bx - w / 2) / w
        col = rng.choice(greens)
        steps = int(length)
        for s in range(steps):
            t = s / max(1, steps)
            x = int(bx + lean * length * t * t)
            y = h - 1 - s
            cv.put(x, y, col, 10 * (1 - t) + 2)
    if flowers:
        for _ in range(rng.randint(4, 6)):
            fx = int(rng.uniform(w * 0.2, w * 0.8))
            fy = int(rng.uniform(h * 0.05, h * 0.45))
            col = rng.choice(flowers)
            for dx, dy in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
                cv.put(fx + dx, fy + dy, col, 0 if (dx, dy) == (0, 0) else 3)
            for s in range(1, h - fy):   # стебель
                cv.put(fx, fy + 1 + s, greens[0], 8)
    return cv


def mushrooms(cm, cap, dots, stem, rng, w, h):
    cv = Canvas(w, h, cm)
    for i in range(3):
        mx = w * (0.22 + 0.28 * i) + rng.uniform(-2, 2)
        size = rng.uniform(0.6, 1.0) * (1.0 if i == 1 else 0.75)
        sh = int(h * 0.55 * size)
        cr = w * 0.17 * size + 2
        cy = h - sh
        for y in range(cy, h):                       # ножка
            for x in range(int(mx - 1.5 * size - 1), int(mx + 1.5 * size + 1) + 1):
                cv.put(x, y, stem, 4 + (x > mx) * 4)
        for y in range(int(cy - cr * 0.8), cy + 1):  # шляпка — полуэллипс
            for x in range(int(mx - cr), int(mx + cr) + 1):
                nx, ny = (x - mx) / cr, (y - cy) / (cr * 0.8)
                if nx * nx + ny * ny <= 1:
                    cv.put(x, y, cap, 2 + max(0, nx) * 10 + (ny > -0.2) * 4)
        for _ in range(int(3 + 3 * size)):           # белые точки
            a = rng.uniform(math.pi, 2 * math.pi)
            r = rng.uniform(0.2, 0.75)
            cv.put(int(mx + math.cos(a) * r * cr), int(cy + math.sin(a) * r * cr * 0.8), dots, 0)
    return cv


def golden_tree(lumps, pal):
    """Большое дерево TRE2, перекрашенное в золото: яркость пикселя -> оттенок золота."""
    w, h, px = patch_pixels(lumps["TRE2A0"])
    left, top = struct.unpack("<hh", lumps["TRE2A0"][4:8])
    gold = {}
    rows = [[None] * w for _ in range(h)]
    for (x, y), c in px.items():
        if c not in gold:
            v = min(1.0, brightness(pal, c) / 105) ** 0.8
            gold[c] = nearest(pal, (int(110 + 145 * v), int(75 + 165 * v), int(10 + 120 * v ** 3)))
        rows[y][x] = gold[c]
    return rows, left, top


# --- палитра ---


def nearest(pal, rgb):
    r, g, b = rgb
    return min(range(256), key=lambda i: (pal[i][0] - r) ** 2 * 3 + (pal[i][1] - g) ** 2 * 4
               + (pal[i][2] - b) ** 2 * 2)


def main():
    src, out_dir = sys.argv[1:3]
    lumps = read_wad(src)
    raw = lumps["PLAYPAL"][:768]
    pal = [tuple(raw[i:i + 3]) for i in range(0, 768, 3)]
    cm = lumps["COLORMAP"]
    rng = random.Random(2026)

    rock = texture_grid(lumps, "SP_ROCK1")
    moss = texture_grid(lumps, "A-CAMO1")
    leaves_bright = texture_grid(lumps, "A-CAMO4")
    leaves_dark = texture_grid(lumps, "A-CAMO1")
    greens = sorted({c for row in leaves_bright for c in row}, key=lambda i: brightness(pal, i))
    greens = greens[len(greens) // 3:]                      # без самых тёмных
    flowers = [nearest(pal, c) for c in ((240, 220, 60), (250, 250, 250), (200, 120, 220))]
    cap, dots, stem = nearest(pal, (200, 30, 20)), nearest(pal, (250, 250, 240)), nearest(pal, (220, 200, 160))

    sprites = {
        "COL1A0": (boulder(cm, rock, moss, rng, 76, 46), 3),
        "COL2A0": (boulder(cm, rock, moss, rng, 40, 22, lumps_of_moss=False), 2),
        "COL3A0": (bush(cm, leaves_bright, rng, 84, 58), 2),
        "COL4A0": (bush(cm, leaves_dark, rng, 56, 38), 2),
        "POB1A0": (grass_tuft(cm, greens, rng, 34, 22), 0),
        "POB2A0": (grass_tuft(cm, greens, rng, 34, 20, flowers=flowers), 0),
        "BRS1A0": (mushrooms(cm, cap, dots, stem, rng, 30, 18), 0),
    }
    os.makedirs(out_dir, exist_ok=True)
    preview_dir = out_dir.rstrip("/") + "-preview"
    os.makedirs(preview_dir, exist_ok=True)
    total = 0
    rows, left, top = golden_tree(lumps, pal)
    gold_cv = Canvas(len(rows[0]), len(rows), cm)
    gold_cv.px = rows
    sprites["ELECA0"] = (gold_cv, (left, top))
    for name, (cv, sink) in sprites.items():
        if isinstance(sink, tuple):          # готовые смещения (как у исходного спрайта)
            data = doom_picture(cv.px, *sink)
        else:
            data = cv.lump(sink)
        total += len(data)
        open(os.path.join(out_dir, name.lower() + ".lmp"), "wb").write(data)
        img = cv.image(pal)
        img.resize((img.width * 4, img.height * 4), Image.NEAREST).save(
            os.path.join(preview_dir, name.lower() + ".png"))
    print(f"decor: {len(sprites)} спрайтов, {total / 1024:.1f} KB")


if __name__ == "__main__":
    main()
