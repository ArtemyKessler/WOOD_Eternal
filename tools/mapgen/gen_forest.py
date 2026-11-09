#!/usr/bin/env python3
"""Генератор лесных карт WOOD.

Делает пул случайных раскладок MAP01..MAPnn; движок на каждый забег выбирает одну из них
(см. WOOD_PickRunMap). Раскладка — дерево полян на сетке с развилками и иногда петлёй.

Каждая зона (старт, тропа, поляна) — свой сектор. Общие рёбра соседних зон становятся
двусторонними линиями, остальные — стеной леса. Над всем открытое небо. Поляны получают
теги 1..N — по ним директор волн их находит.

Использование: gen_forest.py <out/maps.wad> [число_карт=16] [seed=1]
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


GRID = 5                 # сетка GRID x GRID ячеек под поляны
CELL = 2560              # шаг сетки, юнитов
START_RADIUS = 384
CLEARING_RADII = (704, 768, 832, 896, 960)
JITTER = 48              # сдвиг центров полян, чтобы раскладка не была идеальной решёткой
MIN_CLEARINGS, MAX_CLEARINGS = 7, 10


def flat_half(r):
    return int(round(r * math.sin(math.pi / 8)))


def layout(rng):
    """Случайный лес: дерево полян на сетке (развилки — узлы с 3–4 тропами) плюс иногда петля.

    Возвращает зоны и стартовую точку (x, y, угол).
    """
    want = rng.randint(MIN_CLEARINGS, MAX_CLEARINGS)
    start = (rng.randrange(GRID), rng.randrange(GRID))
    cells = [start]
    edges = set()
    frontier = []

    def neighbours(c):
        x, y = c
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if 0 <= x + dx < GRID and 0 <= y + dy < GRID:
                yield (x + dx, y + dy)

    for n in neighbours(start):
        frontier.append((start, n))
    while len(cells) < want + 1 and frontier:
        a, b = frontier.pop(rng.randrange(len(frontier)))
        if b in cells:
            continue
        cells.append(b)
        edges.add(frozenset((a, b)))
        for n in neighbours(b):
            if n not in cells:
                frontier.append((b, n))

    # иногда добавляем петлю: две соседние поляны без прямой тропы
    if rng.random() < 0.6:
        extra = [frozenset((a, b)) for a in cells for b in neighbours(a)
                 if b in cells and frozenset((a, b)) not in edges and start not in (a, b)]
        if extra:
            edges.add(rng.choice(extra))

    # центры и радиусы
    nodes = {}
    for c in cells:
        cx, cy = c[0] * CELL, c[1] * CELL
        if c == start:
            nodes[c] = (cx, cy, START_RADIUS)
        else:
            nodes[c] = (cx + rng.randint(-JITTER, JITTER), cy + rng.randint(-JITTER, JITTER),
                        rng.choice(CLEARING_RADII))

    zones = []
    # поляны нумеруем по удалённости от старта (BFS) — так теги идут «вглубь леса»
    order, seen, queue = [], {start}, [start]
    while queue:
        c = queue.pop(0)
        order.append(c)
        for n in neighbours(c):
            if n not in seen and frozenset((c, n)) in edges:
                seen.add(n)
                queue.append(n)
    tag = 0
    for c in order:
        x, y, r = nodes[c]
        if c == start:
            zones.append(Zone("start", octagon(x, y, r), light=160, center=(x, y), radius=r))
        else:
            tag += 1
            zones.append(Zone("clearing", octagon(x, y, r), tag=tag, light=176, center=(x, y), radius=r))

    for e in edges:
        a, b = sorted(e)
        (ax, ay, ar), (bx, by, br) = nodes[a], nodes[b]
        if a[1] == b[1]:   # горизонтальная тропа: a слева
            lo = max(ay - (flat_half(ar) - PATH_HALF_WIDTH), by - (flat_half(br) - PATH_HALF_WIDTH))
            hi = min(ay + (flat_half(ar) - PATH_HALF_WIDTH), by + (flat_half(br) - PATH_HALF_WIDTH))
            pc = (lo + hi) // 2
            zones.append(Zone("path", rect(ax + flat_extent(ar), pc - PATH_HALF_WIDTH,
                                           bx - flat_extent(br), pc + PATH_HALF_WIDTH), light=144))
        else:              # вертикальная тропа: a снизу
            lo = max(ax - (flat_half(ar) - PATH_HALF_WIDTH), bx - (flat_half(br) - PATH_HALF_WIDTH))
            hi = min(ax + (flat_half(ar) - PATH_HALF_WIDTH), bx + (flat_half(br) - PATH_HALF_WIDTH))
            pc = (lo + hi) // 2
            zones.append(Zone("path", rect(pc - PATH_HALF_WIDTH, ay + flat_extent(ar),
                                           pc + PATH_HALF_WIDTH, by - flat_extent(br)), light=144))

    # игрок смотрит в сторону первой тропы от старта
    first = next(n for n in neighbours(start) if frozenset((start, n)) in edges)
    angle = {(1, 0): 0, (0, 1): 90, (-1, 0): 180, (0, -1): 270}[(first[0] - start[0], first[1] - start[1])]
    sx, sy, _ = nodes[start]
    return zones, (sx, sy, angle)


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


def place_things(zones, rng, player_start):
    things = [(player_start[0], player_start[1], player_start[2], THING_PLAYER1, SKILL_ALL)]
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


def map_lumps(mapname, vertices, sectors, sidedefs, linedefs, things):
    b = Builder(vertices, [(l[0], l[1], l[5], l[6]) for l in linedefs])
    b.build()
    segs = b.segs_lump()   # до VERTEXES: может добавить вершины
    lumps = [
        (mapname, b""),
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
    print(f"{mapname}: {sum(1 for x in sectors if x[6])} полян, {len(sectors)} секторов, "
          f"{len(linedefs)} линий, {len(things)} вещей, {len(b.nodes)} нод")
    return lumps


def write_wad(path, lumps):
    body, directory, pos = bytearray(), bytearray(), 12
    for name, data in lumps:
        directory += struct.pack("<ii", pos, len(data)) + pack_name(name)
        body += data
        pos += len(data)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sii", b"PWAD", len(lumps), pos) + bytes(body) + bytes(directory))


def main():
    out = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    base_seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    lumps = []
    for i in range(count):
        rng = random.Random(base_seed * 1000 + i)
        zones, player_start = layout(rng)
        insert_tjunctions(zones)
        vertices, sectors, sidedefs, linedefs = build_geometry(zones)
        things = place_things(zones, rng, player_start) + \
            edge_trees(zones, vertices, sidedefs, linedefs, rng)
        lumps += map_lumps(f"MAP{i + 1:02d}", vertices, sectors, sidedefs, linedefs, things)
    write_wad(out, lumps)


if __name__ == "__main__":
    main()
