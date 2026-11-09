#!/usr/bin/env python3
"""Собирает урезанный IWAD wood.wad из freedoom2.wad.

Что делает:
  * выкидывает все карты Freedoom и вставляет наши из map-WAD (wad с MAP01 от gen_forest.py);
  * выкидывает музыку, кроме KEEP_MUSIC, и демо-записи;
  * выкидывает спрайты монстров/предметов из DROP_SPRITES;
  * добавляет лампы из каталога wad/lumps/ (WOODCFG и т.п.), заменяя одноимённые.

Использование: build_iwad.py <freedoom2.wad> <wad/lumps> <map01.wad> <out/wood.wad>
"""
import os
import re
import struct
import sys

KEEP_MUSIC = {"D_RUNNIN"}
# Монстры, которых в WOOD точно не будет (на этапе спайка — грубо, уточним по дизайну).
DROP_SPRITES = {
    "CYBR",  # кибердемон
    "SPID",  # паук-мастермайнд
    "BSPI",  # арахнотрон
    "VILE",  # арчвайл
    "FATT",  # манкубус
    "PAIN",  # элементаль боли
    "SSWV",  # эсэсовец
    "KEEN",
    "SKEL",  # ревенант
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


def strip(lumps):
    out = []
    in_sprites = False
    skipping_map = False
    for name, data in lumps:
        if re.fullmatch(r"MAP\d\d", name):
            skipping_map = True
            continue
        elif skipping_map and name in MAP_LUMPS:
            continue
        else:
            skipping_map = False

        if name.startswith("D_") and name not in KEEP_MUSIC:
            continue
        if re.fullmatch(r"DEMO\d", name):  # демо-записи ссылаются на выкинутые карты
            continue
        if name in ("S_START", "SS_START"):
            in_sprites = True
        elif name in ("S_END", "SS_END"):
            in_sprites = False
        elif in_sprites and name[:4] in DROP_SPRITES:
            continue
        out.append((name, data))
    return out


def add_custom(lumps, lumps_dir):
    custom = {}
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
    stripped = add_custom(strip(lumps), lumps_dir) + maps
    size = write_wad(dst, ident, stripped)
    print(f"wood.wad: {len(lumps)} -> {len(stripped)} lumps, "
          f"{os.path.getsize(src) / 1e6:.1f} MB -> {size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
