#!/usr/bin/env python3
"""Генератор лесных карт WOOD.

Делает пул случайных раскладок MAP01..MAPnn; движок на каждый забег выбирает одну из них
(см. WOOD_PickRunMap). Раскладка — дерево полян на сетке с развилками и иногда петлёй.

Каждая зона (старт, тропа, поляна) — свой сектор. Общие рёбра соседних зон становятся
двусторонними линиями, остальные — стеной леса. Над всем открытое небо. Поляны получают
теги 1..N — по ним директор волн их находит. Самая дальняя тупиковая поляна — логово босса:
она больше, темнее, с тегом BOSS_TAG; директор открывает её, когда зачищены все остальные.

Использование: gen_forest.py <out/maps.wad> [число_карт=16] [seed=1]
"""
import math
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nodebuild import Builder  # noqa: E402

# --- оформление (всё из Freedoom, кроме декора из make_decor.py) ---
CEILING = "F_SKY1"
FLOOR_H, CEIL_H = 0, 256

# Типы полян: стены чащи (на каждый отрезок стены — случайная из списка), земля, свет,
# сколько «особенностей» (скал/прудов) и какой декор чаще.
THEMES = {
    "березняк": dict(walls=["A-VINES"], floor="GRASS1", light=(168, 184),
                     rocks=(0, 1), ponds=(0, 1), decor="grass"),
    "чаща": dict(walls=["A-CAMO1", "A-CAMO4"], floor="GRASS2", light=(144, 160),
                 rocks=(0, 1), ponds=(0, 0), decor="bush"),
    "камни": dict(walls=["A-MOSRK2", "SP_ROCK1"], floor="RROCK19", light=(176, 192),
                  rocks=(2, 3), ponds=(0, 0), decor="stone"),
    "болотце": dict(walls=["A-CAMO1", "A-VINES"], floor="RROCK20", light=(144, 160),
                    rocks=(0, 0), ponds=(1, 3), decor="mushroom"),
}
START_THEME = "березняк"
PATH_WALLS = ["A-VINES", "A-CAMO1", "A-CAMO4"]
PATH_FLOORS = ["FLAT10", "RROCK16"]          # грунтовые тропинки
PATH_LIGHT = (136, 160)

# Особенности полян: скала (поднятый непроходимый сектор) и пруд (вода ниже уровня земли,
# тоже непроходимый — иначе директор волн поставил бы барьер и по берегу).
ROCK_WALL, ROCK_TOP = "SP_ROCK1", "RROCK18"
ROCK_HEIGHTS = (48, 96)
POND_BANK, POND_FLOOR, POND_DEPTH = "A-MUD", "FWATER1", 8   # мелко: гладь видна издалека

# Типы вещей Doom II
THING_PLAYER1 = 1
THING_TREE_BIG = 54      # TRE2, большое дерево (радиус 32)
THING_TREE_BURNT = 43    # TRE1, обгоревшее дерево (радиус 16)
THING_STUMP = 47         # SMIT: в Doom пень, во Freedoom маленькое деревце (радиус 16)
# декор из make_decor.py (подменяет неиспользуемые декорации Doom II)
THING_BOULDER = 30       # COL1, валун (твёрдый)
THING_STONE = 31         # COL2, камень (твёрдый)
THING_BUSH_BIG = 32      # COL3, большой куст (твёрдый)
THING_BUSH_SMALL = 33    # COL4, малый куст (твёрдый)
THING_GRASS = 79         # POB1, трава (проходимая)
THING_FLOWERS = 80       # POB2, цветы (проходимые)
THING_MUSHROOMS = 81     # BRS1, грибы (проходимые)
SOLID_THINGS = {THING_TREE_BIG, THING_TREE_BURNT, THING_STUMP, THING_BOULDER, THING_STONE,
                THING_BUSH_BIG, THING_BUSH_SMALL}
SKILL_ALL = 0x0007

PATH_HALF_WIDTH = 128

# Плотность леса. Деревья можно спилить бензопилой — см. saw_trees в WOODCFG.
CLEARING_TREES = 16      # отдельных деревьев внутри поляны
EDGE_ROW2_STEP = 176     # второй ряд опушки на полянах: шаг вдоль стены
EDGE_ROW2_INSET = 120    # и отступ от стены


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
        # оформление (assign_themes / add_features)
        self.theme = None
        self.walls, self.floor = ["A-VINES"], "GRASS1"
        self.floor_h, self.ceil_h = FLOOR_H, CEIL_H
        self.parent = None       # для скал и прудов — индекс поляны, внутри которой они стоят
        self.bank = "-"          # нижняя текстура на границе с родителем


GRID = 6                 # сетка GRID x GRID ячеек под поляны
CELL = 2560              # шаг сетки, юнитов
START_RADIUS = 384
CLEARING_RADII = (704, 768, 832, 896, 960)
JITTER = 48              # сдвиг центров полян, чтобы раскладка не была идеальной решёткой
MIN_CLEARINGS, MAX_CLEARINGS = 11, 14   # обычные поляны, без логова босса
BOSS_TAG = 100           # должен совпадать с WOOD_BOSS_TAG в engine/src/wood_director.c
BOSS_RADIUS = 1216
FLOOR_BOSS = os.environ.get("WOOD_BOSS_FLOOR", "RROCK16")
BOSS_THEME, BOSS_LIGHT = "чаща", 128


def flat_half(r):
    return int(round(r * math.sin(math.pi / 8)))


def layout(rng):
    """Случайный лес: дерево полян на сетке (развилки — узлы с 3–4 тропами) плюс иногда петля.

    Возвращает зоны и стартовую точку (x, y, угол).
    """
    want = rng.randint(MIN_CLEARINGS, MAX_CLEARINGS) + 1   # +1 — логово босса
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

    # логово босса — самый дальний от старта тупик дерева: туда ведёт ровно одна тропа
    depth, queue = {start: 0}, [start]
    while queue:
        c = queue.pop(0)
        for n in neighbours(c):
            if n not in depth and frozenset((c, n)) in edges:
                depth[n] = depth[c] + 1
                queue.append(n)
    leaves = [c for c in cells if c != start and sum(1 for e in edges if c in e) == 1]
    boss = max(leaves, key=lambda c: (depth[c], rng.random()))

    # иногда добавляем петлю: две соседние поляны без прямой тропы (не через старт и не в логово)
    if rng.random() < 0.6:
        extra = [frozenset((a, b)) for a in cells for b in neighbours(a)
                 if b in cells and frozenset((a, b)) not in edges and start not in (a, b)
                 and boss not in (a, b)]
        if extra:
            edges.add(rng.choice(extra))

    # центры и радиусы
    nodes = {}
    for c in cells:
        cx, cy = c[0] * CELL, c[1] * CELL
        if c == start:
            nodes[c] = (cx, cy, START_RADIUS)
        elif c == boss:
            nodes[c] = (cx, cy, BOSS_RADIUS)
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
        elif c == boss:
            zones.append(Zone("boss", octagon(x, y, r), tag=BOSS_TAG, light=128, center=(x, y), radius=r))
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


def assign_themes(zones, rng):
    """Каждой поляне — свой тип леса (соседние по возможности разные), тропам — грунт."""
    names = list(THEMES)
    last = None
    for z in zones:
        if z.kind == "path":
            z.walls = [rng.choice(PATH_WALLS)]
            z.floor = rng.choice(PATH_FLOORS)
            z.light = rng.randint(*PATH_LIGHT) // 8 * 8
            continue
        if z.kind == "boss":
            # логово: тёмная чаща на голой земле, без скал и прудов (add_features его пропускает)
            z.theme = BOSS_THEME
            z.walls, z.floor, z.light = THEMES[BOSS_THEME]["walls"], FLOOR_BOSS, BOSS_LIGHT
            continue
        name = START_THEME if z.kind == "start" else rng.choice([n for n in names if n != last])
        last = name
        t = THEMES[name]
        z.theme = name
        z.walls, z.floor = t["walls"], t["floor"]
        z.light = rng.randint(*t["light"]) // 8 * 8


def _clockwise(poly):
    area = sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1]
               for i in range(len(poly)))
    return poly if area < 0 else poly[::-1]


def point_in_poly(x, y, poly):
    inside = False
    n = len(poly)
    for i in range(n):
        (ax, ay), (bx, by) = poly[i], poly[(i + 1) % n]
        if (ay > y) != (by > y) and x < ax + (y - ay) * (bx - ax) / (by - ay):
            inside = not inside
    return inside


def dist_to_poly(x, y, poly):
    best = float("inf")
    n = len(poly)
    for i in range(n):
        (ax, ay), (bx, by) = poly[i], poly[(i + 1) % n]
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy or 1)))
        best = min(best, math.hypot(x - ax - t * dx, y - ay - t * dy))
    return best


def add_features(zones, rng):
    """Скалы и пруды у края полян, между выходами троп.

    Держим их в кольце 0.76..0.90 радиуса: директор ставит врагов не дальше ~0.69 радиуса
    (плюс радиус самого врага), так что враг не появится внутри скалы, а входы троп (оси N/E/S/W) остаются свободными.
    """
    features = []
    for zi, z in enumerate(zones):
        if z.kind != "clearing":
            continue
        t = THEMES[z.theme]
        kinds = ["rock"] * rng.randint(*t["rocks"]) + ["pond"] * rng.randint(*t["ponds"])
        quadrants = rng.sample(range(4), min(4, len(kinds)))
        cx, cy = z.center
        for kind, q in zip(kinds, quadrants):
            span = rng.uniform(0.35, 0.7)
            mid = q * math.pi / 2 + math.pi / 4 + rng.uniform(-0.05, 0.05)
            r_in, r_out = 0.76 * z.radius, 0.90 * z.radius
            n = 5 if span < 0.5 else 7
            outer, inner = [], []
            for k in range(n):
                a = mid - span / 2 + span * k / (n - 1)
                edge = k in (0, n - 1)
                ro = r_out - rng.uniform(0, 16) - (24 if edge else 0)
                ri = r_in + rng.uniform(0, 24) + (24 if edge else 0)
                outer.append((int(cx + ro * math.cos(a)), int(cy + ro * math.sin(a))))
                inner.append((int(cx + ri * math.cos(a)), int(cy + ri * math.sin(a))))
            poly = []
            for v in outer + inner[::-1]:
                if v not in poly:
                    poly.append(v)
            poly = _clockwise(poly)
            f = Zone(kind, poly, light=z.light)
            f.parent = zi
            if kind == "rock":
                f.walls, f.floor = [ROCK_WALL], ROCK_TOP
                f.floor_h = FLOOR_H + rng.randrange(ROCK_HEIGHTS[0], ROCK_HEIGHTS[1] + 1, 8)
                f.bank = ROCK_WALL
            else:
                f.walls, f.floor = [POND_BANK], POND_FLOOR
                f.floor_h = FLOOR_H - POND_DEPTH
                f.bank = POND_BANK
            features.append(f)
    zones.extend(features)


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
    rng = random.Random(len(zones) * 7919 + len(vertices))
    for zi, z in enumerate(zones):
        sectors.append((z.floor_h, z.ceil_h, z.floor, CEILING, z.light, 0, z.tag))
        n = len(z.poly)
        for i in range(n):
            edges[(vid(z.poly[i]), vid(z.poly[(i + 1) % n]))] = zi
            if z.parent is not None:   # скала/пруд — «дырка» в поляне-родителе
                edges[(vid(z.poly[(i + 1) % n]), vid(z.poly[i]))] = z.parent

    done = set()
    for (a, b), zi in edges.items():
        if (a, b) in done:
            continue
        done.add((a, b))
        other = edges.get((b, a))
        if other is not None:
            done.add((b, a))
            feature = next((zones[k] for k in (zi, other) if zones[k].parent is not None), None)
            lower = feature.bank if feature else "-"
            flags = 0x0004 | (0x0001 if feature else 0)   # к скале/пруду не пройти
            right = len(sidedefs)
            sidedefs.append((0, 0, "-", lower, "-", zi))
            left = len(sidedefs)
            sidedefs.append((0, 0, "-", lower, "-", other))
            linedefs.append((a, b, flags, 0, 0, right, left))  # двусторонняя
        else:
            right = len(sidedefs)
            sidedefs.append((0, 0, "-", "-", rng.choice(zones[zi].walls), zi))
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
        step = 80 if on_path else 72
        inset = 28 if on_path else 44
        n = int(length // step)
        for i in range(n):
            t = (i + 0.5 + rng.uniform(-0.25, 0.25)) / max(n, 1)
            d = inset + rng.uniform(0, 12)
            x, y = ax + (bx - ax) * t + nx * d, ay + (by - ay) * t + ny * d
            if on_path:
                kind = rng.choice([THING_TREE_BURNT, THING_TREE_BURNT, THING_TREE_BURNT, THING_STUMP])
            else:
                kind = rng.choice([THING_TREE_BIG, THING_TREE_BIG, THING_TREE_BURNT])
            things.append((int(x), int(y), rng.randrange(0, 360, 45), kind, SKILL_ALL))
        if on_path or zone.kind == "start":
            continue
        # второй, редкий ряд на полянах: опушка гуще, а деревьев для пилы больше
        n2 = int(length // EDGE_ROW2_STEP)
        for i in range(n2):
            t = (i + 0.5 + rng.uniform(-0.3, 0.3)) / max(n2, 1)
            if not 0.15 < t < 0.85:
                continue
            d = EDGE_ROW2_INSET + rng.uniform(-16, 24)
            x, y = ax + (bx - ax) * t + nx * d, ay + (by - ay) * t + ny * d
            kind = rng.choice([THING_TREE_BIG, THING_TREE_BURNT, THING_TREE_BURNT])
            things.append((int(x), int(y), rng.randrange(0, 360, 45), kind, SKILL_ALL))
    return things


def place_things(zones, rng, player_start):
    things = [(player_start[0], player_start[1], player_start[2], THING_PLAYER1, SKILL_ALL)]
    for z in zones:
        if z.kind not in ("clearing", "boss"):
            continue
        cx, cy = z.center
        boss = z.kind == "boss"
        # в логове середина свободна: боссу нужен простор (паук-мастермайнд шириной 256)
        lo, hi, want = (0.72, 0.86, 14) if boss else (0.4, 0.78, CLEARING_TREES)
        placed = []
        attempts = 0
        while len(placed) < want and attempts < 800:
            attempts += 1
            ang = rng.uniform(0, 2 * math.pi)
            # не загораживаем входы/выходы троп (оси N/E/S/W)
            if min(abs(((ang - k * math.pi / 2) + math.pi) % (2 * math.pi) - math.pi)
                   for k in range(4)) < 0.35:
                continue
            dist = rng.uniform(lo, hi) * z.radius
            x, y = cx + dist * math.cos(ang), cy + dist * math.sin(ang)
            if any(math.hypot(x - px, y - py) < 144 for px, py in placed):
                continue
            placed.append((x, y))
            if boss:
                kind = rng.choice([THING_TREE_BURNT, THING_TREE_BURNT, THING_STUMP])
            else:
                kind = rng.choice([THING_TREE_BIG, THING_TREE_BIG, THING_TREE_BURNT, THING_STUMP])
            things.append((int(x), int(y), rng.randrange(0, 360, 45), kind, SKILL_ALL))
    return things


DECOR = {   # тип декора поляны -> (твёрдые у края, мелочь по траве)
    "grass": ([THING_BUSH_SMALL, THING_BUSH_BIG, THING_STONE],
              [THING_GRASS, THING_GRASS, THING_FLOWERS, THING_FLOWERS, THING_MUSHROOMS]),
    "bush": ([THING_BUSH_BIG, THING_BUSH_BIG, THING_BUSH_SMALL],
             [THING_GRASS, THING_GRASS, THING_MUSHROOMS]),
    "stone": ([THING_BOULDER, THING_STONE, THING_STONE, THING_BUSH_SMALL],
              [THING_GRASS, THING_FLOWERS]),
    "mushroom": ([THING_BUSH_SMALL, THING_STONE],
                 [THING_MUSHROOMS, THING_MUSHROOMS, THING_GRASS, THING_GRASS]),
}


def place_decor(zones, vertices, linedefs, sidedefs, rng):
    """Кусты и камни вдоль опушки, трава, цветы и грибы по земле."""
    things = []
    for v1, v2, flags, _, _, right, left in linedefs:
        if left != -1:
            continue
        zone = zones[sidedefs[right][5]]
        (ax, ay), (bx, by) = vertices[v1], vertices[v2]
        length = math.hypot(bx - ax, by - ay)
        nx, ny = (by - ay) / length, -(bx - ax) / length
        solid = DECOR[THEMES[zone.theme]["decor"]][0] if zone.theme else \
            [THING_BUSH_SMALL, THING_BUSH_SMALL, THING_STONE]
        on_path = zone.kind == "path"
        step = 200 if on_path else 150
        for i in range(int(length // step)):
            if rng.random() < 0.35:
                continue
            t = (i + rng.uniform(0.2, 0.8)) * step / length
            d = rng.uniform(34, 46) if on_path else rng.uniform(56, 90)
            things.append((int(ax + (bx - ax) * t + nx * d), int(ay + (by - ay) * t + ny * d),
                           0, rng.choice(solid), SKILL_ALL))
    for z in zones:
        if z.parent is not None:
            continue
        xs, ys = [v[0] for v in z.poly], [v[1] for v in z.poly]
        small = DECOR[THEMES[z.theme]["decor"]][1] if z.theme else [THING_GRASS, THING_GRASS,
                                                                       THING_FLOWERS]
        area = (max(xs) - min(xs)) * (max(ys) - min(ys))
        for _ in range(int(area / (300 * 300))):
            x, y = rng.uniform(min(xs), max(xs)), rng.uniform(min(ys), max(ys))
            if point_in_poly(x, y, z.poly) and dist_to_poly(x, y, z.poly) > 24:
                things.append((int(x), int(y), 0, rng.choice(small), SKILL_ALL))
    return things


def clear_features(things, zones):
    """Убирает вещи, попавшие на скалы и в пруды (и вплотную к ним, чтобы не застревали)."""
    feats = [z.poly for z in zones if z.parent is not None]
    out = []
    for t in things:
        x, y, kind = t[0], t[1], t[3]
        margin = 40 if kind in SOLID_THINGS else 8
        if any(point_in_poly(x, y, p) or dist_to_poly(x, y, p) < margin for p in feats):
            continue
        out.append(t)
    return out


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
    print(f"{mapname}: {sum(1 for x in sectors if x[6])} полян (с логовом), {len(sectors)} секторов, "
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
        deco_rng = random.Random(base_seed * 7777 + i)   # отдельный ГСЧ: раскладка не меняется
        assign_themes(zones, deco_rng)
        add_features(zones, deco_rng)
        insert_tjunctions(zones)
        vertices, sectors, sidedefs, linedefs = build_geometry(zones)
        things = place_things(zones, rng, player_start) + \
            edge_trees(zones, vertices, sidedefs, linedefs, rng) + \
            place_decor(zones, vertices, linedefs, sidedefs, deco_rng)
        things = things[:1] + clear_features(things[1:], zones)
        lumps += map_lumps(f"MAP{i + 1:02d}", vertices, sectors, sidedefs, linedefs, things)
    write_wad(out, lumps)


if __name__ == "__main__":
    main()
