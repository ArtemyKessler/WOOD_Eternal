"""Простой BSP-нодбилдер для карт формата Doom (ванильные SEGS/SSECTORS/NODES).

Рассчитан на наши сгенерированные карты: сотни линий, без хитростей вроде
самопересечений. BLOCKMAP не строим — PrBoom+ сам строит его при пустом лампе,
REJECT пишем пустым (движок дополняет его нулями).
"""
import math
import struct

EPS = 1e-6


class Seg:
    __slots__ = ("x1", "y1", "x2", "y2", "line", "side", "offset")

    def __init__(self, x1, y1, x2, y2, line, side, offset):
        self.x1, self.y1, self.x2, self.y2 = x1, y1, x2, y2
        self.line, self.side, self.offset = line, side, offset


def _side(px, py, x, y, dx, dy):
    """>0 — справа (front в терминах Doom), <0 — слева, 0 — на линии."""
    v = dy * (px - x) - dx * (py - y)
    if abs(v) < EPS * max(1.0, math.hypot(dx, dy)):
        return 0.0
    return v


def _classify(seg, part):
    x, y, dx, dy = part
    a = _side(seg.x1, seg.y1, x, y, dx, dy)
    b = _side(seg.x2, seg.y2, x, y, dx, dy)
    if a >= 0 and b >= 0:
        if a == 0 and b == 0:
            # коллинеарен: по направлению
            same = (seg.x2 - seg.x1) * dx + (seg.y2 - seg.y1) * dy > 0
            return "front" if same else "back"
        return "front"
    if a <= 0 and b <= 0:
        return "back"
    return "split"


def _is_convex(segs):
    for s in segs:
        part = (s.x1, s.y1, s.x2 - s.x1, s.y2 - s.y1)
        for o in segs:
            if o is not s and _classify(o, part) != "front":
                return False
    return True


class Builder:
    def __init__(self, vertices, linedefs):
        """vertices: [(x, y)]; linedefs: [(v1, v2, right_sidedef, left_sidedef)] (-1 = нет)."""
        self.vertices = [tuple(map(float, v)) for v in vertices]
        self.linedefs = linedefs
        self.out_vertices = [(int(round(x)), int(round(y))) for x, y in self.vertices]
        self.vindex = {v: i for i, v in enumerate(self.out_vertices)}
        self.segs_out = []
        self.ssectors = []
        self.nodes = []

    def _vertex(self, x, y):
        key = (int(round(x)), int(round(y)))
        if key not in self.vindex:
            self.vindex[key] = len(self.out_vertices)
            self.out_vertices.append(key)
        return self.vindex[key]

    def _initial_segs(self):
        segs = []
        for i, (v1, v2, right, left) in enumerate(self.linedefs):
            (x1, y1), (x2, y2) = self.vertices[v1], self.vertices[v2]
            if right >= 0:
                segs.append(Seg(x1, y1, x2, y2, i, 0, 0.0))
            if left >= 0:
                segs.append(Seg(x2, y2, x1, y1, i, 1, 0.0))
        return segs

    def _split(self, seg, part):
        x, y, dx, dy = part
        a = dy * (seg.x1 - x) - dx * (seg.y1 - y)
        b = dy * (seg.x2 - x) - dx * (seg.y2 - y)
        t = a / (a - b)
        mx = seg.x1 + t * (seg.x2 - seg.x1)
        my = seg.y1 + t * (seg.y2 - seg.y1)
        # привязываем точку раздела к целой сетке — так её запишет VERTEXES
        mx, my = round(mx), round(my)
        first = Seg(seg.x1, seg.y1, mx, my, seg.line, seg.side, seg.offset)
        second = Seg(mx, my, seg.x2, seg.y2, seg.line, seg.side,
                     seg.offset + math.hypot(mx - seg.x1, my - seg.y1))
        return (first, second) if a > 0 else (second, first)  # (front, back)

    def _choose(self, segs):
        best, best_score = None, None
        seen = set()
        for s in segs:
            dx, dy = s.x2 - s.x1, s.y2 - s.y1
            key = (round(s.x1), round(s.y1), round(dx), round(dy))
            if key in seen:
                continue
            seen.add(key)
            part = (s.x1, s.y1, dx, dy)
            front = back = splits = 0
            for o in segs:
                c = _classify(o, part)
                if c == "front":
                    front += 1
                elif c == "back":
                    back += 1
                else:
                    splits += 1
            if back + splits == 0 or front + splits == 0:
                continue
            score = splits * 8 + abs(front - back)
            if best_score is None or score < best_score:
                best, best_score = part, score
        return best

    @staticmethod
    def _bbox(segs):
        xs = [s.x1 for s in segs] + [s.x2 for s in segs]
        ys = [s.y1 for s in segs] + [s.y2 for s in segs]
        # top, bottom, left, right
        return (int(math.ceil(max(ys))), int(math.floor(min(ys))),
                int(math.floor(min(xs))), int(math.ceil(max(xs))))

    def _build(self, segs):
        """Возвращает индекс ребёнка для NODES (с флагом 0x8000 для подсектора)."""
        part = None if _is_convex(segs) else self._choose(segs)
        if part is None:
            first = len(self.segs_out)
            self.segs_out.extend(segs)
            self.ssectors.append((len(segs), first))
            return 0x8000 | (len(self.ssectors) - 1)
        front, back = [], []
        for s in segs:
            c = _classify(s, part)
            if c == "front":
                front.append(s)
            elif c == "back":
                back.append(s)
            else:
                f, b = self._split(s, part)
                for piece, dst in ((f, front), (b, back)):
                    if abs(piece.x2 - piece.x1) + abs(piece.y2 - piece.y1) > 0:
                        dst.append(piece)
        rchild = self._build(front)
        lchild = self._build(back)
        x, y, dx, dy = part
        self.nodes.append((int(round(x)), int(round(y)), int(round(dx)), int(round(dy)),
                           self._bbox(front), self._bbox(back), rchild, lchild))
        return len(self.nodes) - 1

    def build(self):
        self._build(self._initial_segs())
        if len(self.out_vertices) > 32767 or len(self.segs_out) > 32767:
            raise ValueError("карта слишком велика для ванильного формата нод")

    # --- сериализация (segs_lump — до vertexes_lump: он может добавить вершины) ---
    def vertexes_lump(self):
        return b"".join(struct.pack("<hh", x, y) for x, y in self.out_vertices)

    def segs_lump(self):
        out = bytearray()
        for s in self.segs_out:
            v1 = self._vertex(s.x1, s.y1)
            v2 = self._vertex(s.x2, s.y2)
            ang = int(round(math.atan2(s.y2 - s.y1, s.x2 - s.x1) / (2 * math.pi) * 65536)) & 0xFFFF
            out += struct.pack("<HHHHhH", v1, v2, ang, s.line, s.side, int(round(s.offset)))
        return bytes(out)

    def ssectors_lump(self):
        return b"".join(struct.pack("<HH", n, f) for n, f in self.ssectors)

    def nodes_lump(self):
        out = bytearray()
        for x, y, dx, dy, rb, lb, rc, lc in self.nodes:
            out += struct.pack("<hhhh", x, y, dx, dy)
            out += struct.pack("<hhhh", *rb) + struct.pack("<hhhh", *lb)
            out += struct.pack("<HH", rc, lc)
        return bytes(out)
