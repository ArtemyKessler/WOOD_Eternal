#!/usr/bin/env python3
"""Генератор лесной карты WOOD (MAP01).

Карта описывается как набор многоугольных зон (старт, тропы, поляны). Каждая зона — свой
сектор. Общие рёбра соседних зон становятся двусторонними линиями, остальные — стеной леса.
Над всем открытое небо. Поляны получают теги 1..N — по ним директор волн будет их находить.

Использование: gen_forest.py <out/map01.wad> [seed]
"""
import math
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nodebuild import Builder  # noqa: E402

# --- оформление (всё из Freedoom) ---
FOREST_WALL = os.environ.get("WOOD_FOREST_WALL", "A-VINES")   # стена «чащи»: берёзовые заросли
FLOOR_PATH = "GRASS2"
FLOOR_CLEARING = "GRASS1"
CEILING = "F_SKY1"
FLOOR_H, CEIL_H = 0, 256

# Типы вещей Doom II
THING_PLAYER1 = 1
THING_TREE_BIG = 54      # TRE2, большое дерево (радиус 32)
THING_TREE_BURNT = 43    # TRE1, обгоревшее дерево (радиус 16)
THING_STUMP = 47         # SMIT, пень (радиус 16)
SKILL_ALL = 0x0007

PATH_HALF_WIDTH = 128


def octagon(cx, cy, r):
    """Восьмиугольник с плоскими сторонами по осям, вершины по часовой стрелке."""
    a = int(round(r * math.cos(math.pi / 8)))   # до плоской стороны
    b = int(round(r * math.sin(math.pi / 8)))   # половина плоской стороны
    return [(cx - b, cy + a), (cx + b, cy + a), (cx + a, cy + b), (cx + a, cy - b),
            (cx + b, cy - a), (cx - b, cy - a), (cx - a, cy - b), (cx - a, cy + b)]


def rect(x0, y0, x1, y1):
    x0, x1 = sorted((x0, x1))
    y0, y1 = sorted((y0, y1))
    return [(x0, y1), (x1, y1), (x1, y0), (x0, y0)]


def flat_extent(r):
    return int(round(r * math.cos(math.pi / 8)))


class Zone:
    def __init__(self, kind, poly, tag=0, light=160, center=None, radius=0):
        self.kind, self.poly, self.tag, self.light = kind, poly, tag, light
        self.center, self.radius = center, radius


def layout():
    """Лес v1: старт → тропа на север → поляна 1 → тропа на восток → поляна 2 → тропа на север → поляна 3."""
    zones = []
    start = (0, 0, 320)
    c1 = (0, 2304, 896)
    c2 = (3072, 2304, 896)
    c3 = (3072, 4864, 1024)

    zones.append(Zone("start", octagon(*start), light=160, center=start[:2], radius=start[2]))
    zones.append(Zone("path", rect(-PATH_HALF_WIDTH, start[1] + flat_extent(start[2]),
                                   PATH_HALF_WIDTH, c1[1] - flat_extent(c1[2])), light=144))
    zones.append(Zone("clearing", octagon(*c1), tag=1, light=176, center=c1[:2], radius=c1[2]))
    zones.append(Zone("path", rect(c1[0] + flat_extent(c1[2]), c1[1] - PATH_HALF_WIDTH,
                                   c2[0] - flat_extent(c2[2]), c1[1] + PATH_HALF_WIDTH), light=144))
    zones.append(Zone("clearing", octagon(*c2), tag=2, light=176, center=c2[:2], radius=c2[2]))
    zones.append(Zone("path", rect(c2[0] - PATH_HALF_WIDTH, c2[1] + flat_extent(c2[2]),
                                   c2[0] + PATH_HALF_WIDTH, c3[1] - flat_extent(c3[2])), light=144))
    zones.append(Zone("clearing", octagon(*c3), tag=3, light=176, center=c3[:2], radius=c3[2]))
    return zones


def insert_tjunctions(zones):
    """Добавляет в рёбра зон чужие вершины, лежащие на этих рёбрах (стыки тропа/поляна)."""
    allv = {v for z in zones for v in z.poly}
    for z in zones:
        out = []
        n = len(z.poly)
        for i in range(n):
            a, b = z.poly[i], z.poly[(i + 1) % n]
            out.append(a)
            between = []
            for v in allv:
                if v in (a, b):
                    continue
                cross = (b[0] - a[0]) * (v[1] - a[1]) - (b[1] - a[1]) * (v[0] - a[0])
                if cross != 0:
                    continue
                t = ((v[0] - a[0]) * (b[0] - a[0]) + (v[1] - a[1]) * (b[1] - a[1])) / \
                    ((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2)
                if 0 < t < 1:
                    between.append((t, v))
            out.extend(v for _, v in sorted(between))
        z.poly = out


def build_geometry(zones):
    vertices, vindex = [], {}

    def vid(v):
        if v not in vindex:
            vindex[v] = len(vertices)
            vertices.append(v)
        return vindex[v]

    sectors, sidedefs, linedefs = [], [], []
    edges = {}  # (va, vb) -> (zone index)
    for zi, z in enumerate(zones):
        floor = FLOOR_CLEARING if z.kind == "clearing" else FLOOR_PATH
        sectors.append((FLOOR_H, CEIL_H, floor, CEILING, z.light, 0, z.tag))
        n = len(z.poly)
        for i in range(n):
            edges[(vid(z.poly[i]), vid(z.poly[(i + 1) % n]))] = zi

    done = set()
    for (a, b), zi in edges.items():
        if (a, b) in done:
            continue
        done.add((a, b))
        other = edges.get((b, a))
        if other is not None:
            done.add((b, a))
            right = len(sidedefs)
            sidedefs.append((0, 0, "-", "-", "-", zi))
            left = len(sidedefs)
            sidedefs.append((0, 0, "-", "-", "-", other))
            linedefs.append((a, b, 0x0004, 0, 0, right, left))  # двусторонняя
        else:
            right = len(sidedefs)
            sidedefs.append((0, 0, "-", "-", FOREST_WALL, zi))
            linedefs.append((a, b, 0x0001, 0, 0, right, -1))     # непроходимая стена
    return vertices, sectors, sidedefs, linedefs


def edge_trees(zones, vertices, sidedefs, linedefs, rng):
    """Деревья вдоль стены чащи: стена становится опушкой, а не «коридором»."""
    things = []
    for v1, v2, flags, _, _, right, left in linedefs:
        if left != -1:
            continue
        zone = zones[sidedefs[right][5]]
        (ax, ay), (bx, by) = vertices[v1], vertices[v2]
        length = math.hypot(bx - ax, by - ay)
        nx, ny = (by - ay) / length, -(bx - ax) / length   # внутрь сектора (правая сторона)
        on_path = zone.kind == "path"
        step = 112 if on_path else 96
        inset = 28 if on_path else 44
        n = int(length // step)
        for i in range(n):
            t = (i + 0.5 + rng.uniform(-0.25, 0.25)) / max(n, 1)
            d = inset + rng.uniform(0, 12)
            x, y = ax + (bx - ax) * t + nx * d, ay + (by - ay) * t + ny * d
            if on_path:
                kind = rng.choice([THING_TREE_BURNT, THING_TREE_BURNT, THING_STUMP])
            else:
                kind = rng.choice([THING_TREE_BIG, THING_TREE_BIG, THING_TREE_BURNT])
            things.append((int(x), int(y), rng.randrange(0, 360, 45), kind, SKILL_ALL))
    return things


def place_things(zones, rng):
    things = [(0, -160, 90, THING_PLAYER1, SKILL_ALL)]
    for z in zones:
        if z.kind != "clearing":
            continue
        cx, cy = z.center
        placed = []
        attempts = 0
        while len(placed) < 10 and attempts < 500:
            attempts += 1
            ang = rng.uniform(0, 2 * math.pi)
            # не загораживаем входы/выходы троп (оси N/E/S/W)
            if min(abs(((ang - k * math.pi / 2) + math.pi) % (2 * math.pi) - math.pi)
                   for k in range(4)) < 0.35:
                continue
            dist = rng.uniform(0.45, 0.78) * z.radius
            x, y = cx + dist * math.cos(ang), cy + dist * math.sin(ang)
            if any(math.hypot(x - px, y - py) < 160 for px, py in placed):
                continue
            placed.append((x, y))
            kind = rng.choice([THING_TREE_BIG, THING_TREE_BIG, THING_TREE_BURNT, THING_STUMP])
            things.append((int(x), int(y), rng.randrange(0, 360, 45), kind, SKILL_ALL))
    return things


def pack_name(s):
    return s.encode("ascii").ljust(8, b"\0")


def write_map(path, vertices, sectors, sidedefs, linedefs, things):
    b = Builder(vertices, [(l[0], l[1], l[5], l[6]) for l in linedefs])
    b.build()
    segs = b.segs_lump()   # до VERTEXES: может добавить вершины
    lumps = [
        ("MAP01", b""),
        ("THINGS", b"".join(struct.pack("<hhhhh", *t) for t in things)),
        ("LINEDEFS", b"".join(struct.pack("<HHhhhHH", l[0], l[1], l[2], l[3], l[4],
                                          l[5], l[6] & 0xFFFF) for l in linedefs)),
        ("SIDEDEFS", b"".join(struct.pack("<hh", s[0], s[1]) + pack_name(s[2]) + pack_name(s[3])
                              + pack_name(s[4]) + struct.pack("<h", s[5]) for s in sidedefs)),
        ("VERTEXES", b.vertexes_lump()),
        ("SEGS", segs),
        ("SSECTORS", b.ssectors_lump()),
        ("NODES", b.nodes_lump()),
        ("SECTORS", b"".join(struct.pack("<hh", s[0], s[1]) + pack_name(s[2]) + pack_name(s[3])
                             + struct.pack("<hhh", s[4], s[5], s[6]) for s in sectors)),
        ("REJECT", b""),
        ("BLOCKMAP", b""),
    ]
    body, directory, pos = bytearray(), bytearray(), 12
    for name, data in lumps:
        directory += struct.pack("<ii", pos, len(data)) + pack_name(name)
        body += data
        pos += len(data)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sii", b"PWAD", len(lumps), pos) + bytes(body) + bytes(directory))
    print(f"map01: {len(sectors)} секторов, {len(linedefs)} линий, {len(things)} вещей, "
          f"{len(b.nodes)} нод, {len(b.ssectors)} подсекторов")


def main():
    out = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    zones = layout()
    insert_tjunctions(zones)
    vertices, sectors, sidedefs, linedefs = build_geometry(zones)
    rng = random.Random(seed)
    things = place_things(zones, rng) + edge_trees(zones, vertices, sidedefs, linedefs, rng)
    write_map(out, vertices, sectors, sidedefs, linedefs, things)


if __name__ == "__main__":
    main()
