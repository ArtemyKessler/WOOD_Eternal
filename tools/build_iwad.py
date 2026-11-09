#!/usr/bin/env python3
"""Собирает урезанный IWAD wood.wad из freedoom2.wad.

Что делает:
  * выкидывает все карты Freedoom и вставляет наши из map-WAD (MAP01 от gen_forest.py);
  * оставляет только текстуры, которые использует наша карта (+ служебные), пересобирает
    TEXTURE1/PNAMES и выкидывает неиспользуемые патчи;
  * оставляет только флэты, которые использует карта (+ служебные);
  * оставляет только спрайты из KEEP_SPRITES (игрок, оружие, наши враги, предметы, деревья);
  * выкидывает музыку, кроме перечисленной в WOODCFG, демо-записи, звуки PC-спикера и звуки
    выкинутых монстров;
  * добавляет лампы из каталога wad/lumps/ (WOODCFG, TITLEPIC и т.п.), заменяя одноимённые.

Использование: build_iwad.py <freedoom2.wad> <каталоги_лампов> <map01.wad> <out/wood.wad>
(каталоги лампов — через os.pathsep, например wad/lumps:build/cache/gfx)
"""
import os
import re
import struct
import sys

# Музыка: всё, что перечислено в music_calm/music_battle в WOODCFG (см. woodcfg_music()).

# Спрайты (первые 4 буквы имени), которые нужны игре. Всё остальное выкидывается.
# Если директор начнёт спавнить что-то новое — добавить сюда, иначе движок упадёт.
KEEP_SPRITES = {
    # игрок и эффекты
    "PLAY", "PUFF", "BLUD", "TFOG", "IFOG", "POL5",
    # оружие в руках и вспышки (кулак и пистолет — на случай автопереключения)
    "PUNG", "PISG", "PISF", "SAWG", "SHTG", "SHTF", "SHT2", "CHGG", "CHGF",
    "MISG", "MISF", "PLSG", "PLSF", "BFGG", "BFGF",
    # снаряды и взрывы
    "MISL", "PLSS", "PLSE", "BFS1", "BFE1", "BFE2", "BAL1", "BAL2", "BAL7",
    # враги из WOODCFG
    "POSS", "SPOS", "CPOS", "TROO", "SARG", "SKUL", "HEAD", "BOSS", "BOS2",
    # оружие и патроны на земле
    "CSAW", "SHOT", "MGUN", "SGN2", "LAUN", "PLAS", "BFUG",
    "CLIP", "AMMO", "SHEL", "SBOX", "ROCK", "BROK", "CELL", "CELP", "BPAK",
    # здоровье и броня
    "STIM", "MEDI", "SOUL", "MEGA", "ARM1", "ARM2", "BON1", "BON2",
    # лес и барьер
    "TRE1", "TRE2", "SMIT", "TRED",
}

# Текстуры и флэты, нужные движку помимо карты.
KEEP_TEXTURES = {"AASHITTY", "SKY1"}  # первая текстура — «нулевая», SKY1 — небо MAP01
SKY_ALIASES = ("SKY2", "SKY3")       # движок ищет их по имени; делаем копиями SKY1
KEEP_FLATS = {"F_SKY1", "GRNROCK", "FLOOR7_2"}  # небо и фон рамки экрана

# Звуки выкинутых монстров (DS-префикс).
DROP_SOUNDS = {
    "CYBSIT", "CYBDTH", "HOOF", "METAL", "SPISIT", "SPIDTH",
    "VILSIT", "VILATK", "VIPAIN", "VILDTH", "VILACT", "FLAME", "FLAMST",
    "MANSIT", "MNPAIN", "MANATK", "MANDTH", "PESIT", "PEPAIN", "PEDTH",
    "SKESIT", "SKEATK", "SKEDTH", "SKEACT", "SKESWG", "SKEPCH",
    "BSPSIT", "BSPDTH", "BSPACT", "BSPWLK", "SSSIT", "SSDTH",
    "KEENPN", "KEENDT", "BOSSIT", "BOSPIT", "BOSPN", "BOSDTH", "BOSCUB",
}

MAP_LUMPS = {"THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS", "SSECTORS",
             "NODES", "SECTORS", "REJECT", "BLOCKMAP", "BEHAVIOR"}


def read_wad(path):
    data = open(path, "rb").read()
    ident, count, diroff = struct.unpack("<4sii", data[:12])
    lumps = []
    for i in range(count):
        off, size, name = struct.unpack("<ii8s", data[diroff + 16 * i: diroff + 16 * i + 16])
        lumps.append((name.rstrip(b"\0").decode("ascii"), data[off:off + size]))
    return ident, lumps


def write_wad(path, ident, lumps):
    body = bytearray()
    directory = bytearray()
    pos = 12
    for name, data in lumps:
        directory += struct.pack("<ii8s", pos, len(data), name.encode("ascii"))
        body += data
        pos += len(data)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sii", ident, len(lumps), pos))
        f.write(body)
        f.write(directory)
    return pos + len(directory)


def name8(raw):
    return raw.split(b"\0", 1)[0].decode("ascii").upper()


def pack8(name):
    return name.encode("ascii").ljust(8, b"\0")


def woodcfg_music(lumps_dirs):
    tracks = set()
    for lumps_dir in lumps_dirs.split(os.pathsep):
        path = os.path.join(lumps_dir, "woodcfg.txt")
        if not os.path.exists(path):
            continue
        for line in open(path, encoding="utf-8"):
            words = line.split("#", 1)[0].split()
            if words and words[0] in ("music_calm", "music_battle"):
                tracks.update(w.upper() for w in words[1:])
    return tracks


# --- что использует карта ---

def map_usage(maps):
    textures, flats = set(), set()
    for name, data in maps:
        if name == "SIDEDEFS":
            for i in range(0, len(data), 30):
                for j in (4, 12, 20):
                    t = name8(data[i + j:i + j + 8])
                    if t and t != "-":
                        textures.add(t)
        elif name == "SECTORS":
            for i in range(0, len(data), 26):
                flats.add(name8(data[i + 4:i + 12]))
                flats.add(name8(data[i + 12:i + 20]))
    return textures, flats


# --- TEXTURE1 / PNAMES ---

def parse_textures(data):
    count = struct.unpack_from("<i", data, 0)[0]
    offsets = struct.unpack_from(f"<{count}i", data, 4)
    textures = []
    for off in offsets:
        name = name8(data[off:off + 8])
        masked, width, height, _coldir, npatches = struct.unpack_from("<ihhih", data, off + 8)
        patches = [struct.unpack_from("<hhhhh", data, off + 22 + 10 * k) for k in range(npatches)]
        textures.append([name, masked, width, height, patches])
    return textures


def build_textures(textures):
    header = struct.pack("<i", len(textures))
    offsets, body = [], bytearray()
    base = 4 + 4 * len(textures)
    for name, masked, width, height, patches in textures:
        offsets.append(base + len(body))
        body += pack8(name) + struct.pack("<ihhih", masked, width, height, 0, len(patches))
        for p in patches:
            body += struct.pack("<hhhhh", *p)
    return header + struct.pack(f"<{len(offsets)}i", *offsets) + bytes(body)


def trim_textures(lumps, used):
    by_name = dict(lumps)
    pnames_data = by_name["PNAMES"]
    pcount = struct.unpack_from("<i", pnames_data, 0)[0]
    pnames = [name8(pnames_data[4 + 8 * i:12 + 8 * i]) for i in range(pcount)]

    textures = parse_textures(by_name["TEXTURE1"])
    want = KEEP_TEXTURES | used
    kept = [t for t in textures if t[0] in want]
    first = next(t for t in textures if t[0] == "AASHITTY")
    kept.remove(first)
    kept.insert(0, first)
    sky = next(t for t in kept if t[0] == "SKY1")
    kept += [[alias] + sky[1:] for alias in SKY_ALIASES]
    missing = want - {t[0] for t in kept}
    if missing:
        sys.exit(f"нет текстур в Freedoom: {sorted(missing)}")

    new_pnames, remap = [], {}
    for t in kept:
        new_patches = []
        for ox, oy, idx, step, cmap in t[4]:
            pname = pnames[idx]
            if pname not in remap:
                remap[pname] = len(new_pnames)
                new_pnames.append(pname)
            new_patches.append((ox, oy, remap[pname], step, cmap))
        t[4] = new_patches

    new_pnames_data = struct.pack("<i", len(new_pnames)) + b"".join(pack8(n) for n in new_pnames)
    return build_textures(kept), new_pnames_data, set(new_pnames), len(textures), len(kept)


# --- основной проход ---

def strip(lumps, keep_patches, keep_flats, keep_music):
    out = []
    section = None
    skipping_map = False
    for name, data in lumps:
        if re.fullmatch(r"MAP\d\d", name):
            skipping_map = True
            continue
        elif skipping_map and name in MAP_LUMPS:
            continue
        else:
            skipping_map = False

        if name in ("S_START", "SS_START"):
            section = "sprites"
        elif name in ("F_START", "FF_START"):
            section = "flats"
        elif name in ("P_START", "PP_START"):
            section = "patches"
        elif name in ("S_END", "SS_END", "F_END", "FF_END", "P_END", "PP_END"):
            section = None
        elif section == "sprites":
            if name[:4] not in KEEP_SPRITES:
                continue
        elif section == "flats":
            if not name.endswith("_START") and not name.endswith("_END") and name not in keep_flats:
                continue
        elif section == "patches":
            if not name.endswith("_START") and not name.endswith("_END") and name not in keep_patches:
                continue
        else:
            if name.startswith("D_") and name not in keep_music:
                continue
            if re.fullmatch(r"DEMO\d", name):  # демо-записи ссылаются на выкинутые карты
                continue
            if name.startswith("DP"):        # звуки PC-спикера движок не использует
                continue
            if name.startswith("DS") and name[2:] in DROP_SOUNDS:
                continue
        out.append((name, data))
    return out


def add_custom(lumps, lumps_dirs):
    """lumps_dirs — каталоги через os.pathsep; более поздние перекрывают ранние."""
    custom = {}
    for lumps_dir in lumps_dirs.split(os.pathsep):
        for fn in sorted(os.listdir(lumps_dir)):
            name = os.path.splitext(fn)[0].upper()[:8]
            custom[name] = open(os.path.join(lumps_dir, fn), "rb").read()
    out = [(n, d) for n, d in lumps if n not in custom]
    out.extend(custom.items())
    return out


def main():
    src, lumps_dir, map_wad, dst = sys.argv[1:5]
    ident, lumps = read_wad(src)
    if ident != b"IWAD":
        sys.exit(f"{src}: not an IWAD")
    _, maps = read_wad(map_wad)

    used_textures, used_flats = map_usage(maps)
    tex1, pnames, keep_patches, ntex_before, ntex_after = trim_textures(lumps, used_textures)
    lumps = [(n, tex1 if n == "TEXTURE1" else pnames if n == "PNAMES" else d)
             for n, d in lumps if n != "TEXTURE2"]

    keep_music = woodcfg_music(lumps_dir)
    stripped = strip(lumps, keep_patches, KEEP_FLATS | used_flats, keep_music)
    stripped = add_custom(stripped, lumps_dir) + maps
    size = write_wad(dst, ident, stripped)
    print(f"wood.wad: {len(lumps)} -> {len(stripped)} lumps, текстур {ntex_before} -> {ntex_after}, "
          f"патчей {len(keep_patches)}, "
          f"{os.path.getsize(src) / 1e6:.1f} MB -> {size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
