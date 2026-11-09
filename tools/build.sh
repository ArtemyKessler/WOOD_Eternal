#!/usr/bin/env bash
# Полная сборка WOOD для браузера. Результат: dist/ (готово для GitHub Pages / itch.io).
# Требует: emsdk в PATH (emcmake), cmake, gcc, libsdl2-dev, python3, curl, unzip.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="$ROOT/build/cache"
FREEDOOM_VER="0.13.0"
FREEDOOM_SHA256="3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59"
PRBOOMX_SHA1="c3946ca57137c974e474abf8201994dcc00d463d"
JOBS="$(nproc)"

mkdir -p "$CACHE"

# 1. Freedoom (BSD-3) — источник ресурсов.
FD_ZIP="$CACHE/freedoom-$FREEDOOM_VER.zip"
if [ ! -f "$FD_ZIP" ]; then
  curl -fsSL -o "$FD_ZIP" \
    "https://github.com/freedoom/freedoom/releases/download/v$FREEDOOM_VER/freedoom-$FREEDOOM_VER.zip"
fi
echo "$FREEDOOM_SHA256  $FD_ZIP" | sha256sum -c -
unzip -o -q -j "$FD_ZIP" "freedoom-$FREEDOOM_VER/freedoom2.wad" -d "$CACHE"

# 2. Карта (генерируется скриптом, ноды строит наш nodebuild.py) и урезанный IWAD.
python3 "$ROOT/tools/mapgen/gen_forest.py" "$CACHE/map01.wad"
python3 "$ROOT/tools/make_title.py" "$CACHE/freedoom2.wad" "$CACHE/gfx"
python3 -I "$ROOT/tools/build_iwad.py" "$CACHE/freedoom2.wad" "$ROOT/wad/lumps:$CACHE/gfx" "$CACHE/map01.wad" "$CACHE/wood.wad"

# 3. prboomx.wad — служебный WAD движка, собирается нативной сборкой.
if [ ! -f "$CACHE/prboomx.wad" ]; then
  cmake -S "$ROOT/engine" -B "$ROOT/build/native" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_GL=OFF -DWITH_IMAGE=OFF -DWITH_MIXER=OFF -DWITH_NET=OFF -DWITH_PCRE=OFF \
    -DWITH_ZLIB=OFF -DWITH_MAD=OFF -DWITH_FLUIDSYNTH=OFF -DWITH_DUMB=OFF \
    -DWITH_VORBISFILE=OFF -DWITH_PORTMIDI=OFF -DWITH_ALSA=OFF
  cmake --build "$ROOT/build/native" --target prboomwad -j"$JOBS"
  cp "$ROOT/build/native/prboomx.wad" "$CACHE/prboomx.wad"
fi
echo "$PRBOOMX_SHA1  $CACHE/prboomx.wad" | sha1sum -c -

# 4. Файлы, которые упаковываются в index.data (имена только в нижнем регистре).
FS="$ROOT/engine/wasm/fs"
cp "$CACHE/wood.wad" "$CACHE/prboomx.wad" "$FS/"

# 5. WebAssembly-сборка (программный рендерер, без GL4ES).
#    Каталог сборки обязан быть engine/build: CMake движка ссылается на ../../wasm/fs.
emcmake cmake -S "$ROOT/engine" -B "$ROOT/engine/build" -DCMAKE_BUILD_TYPE=Release
rm -f "$ROOT/engine/build/index.html"   # заставляем перелинковку, чтобы обновить index.data
cmake --build "$ROOT/engine/build" -j"$JOBS"

# 6. dist/
rm -rf "$ROOT/dist"
mkdir -p "$ROOT/dist/licenses"
cp "$ROOT"/engine/build/index.{html,js,wasm,data} "$ROOT/dist/"
cp "$ROOT"/LICENSES/* "$ROOT/dist/licenses/"
ls -la "$ROOT/dist"
