#!/bin/bash
# Builds MiSTer Moonlight CRT for the MiSTer's ARM, runs the test with the fake core
# (with qemu-arm, if present) and packs the zip to copy to the SD.
# Usage: scripts/build.sh [work folder]   (default: ./work)
set -euo pipefail
CLIENT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(realpath -m "${1:-$CLIENT/work}")
DEPS=$WORK/deps
bash "$CLIENT/scripts/build-deps.sh" "$WORK/src" "$DEPS"
export PKG_CONFIG_LIBDIR=$DEPS/lib/pkgconfig PKG_CONFIG_PATH=
cmake -S "$CLIENT" -B "$WORK/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$CLIENT/cmake/arm-mister.cmake" \
  -DDEPS_PREFIX="$DEPS" -DMG_TESTS=ON >/dev/null
ninja -C "$WORK/build"

if command -v qemu-arm >/dev/null && command -v ffmpeg >/dev/null; then
  CLIP=$WORK/test_640x480.h264
  [ -f "$CLIP" ] || ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x480:rate=60 -t 2 -c:v libx264 \
    -profile:v high -bf 0 -g 60 -pix_fmt yuv420p -x264-params slices=4 -b:v 4M "$CLIP"
  qemu-arm "$WORK/build/test_core" "$CLIP"
else
  echo "qemu-arm or ffmpeg missing: skipping the test with the fake core"
fi

VER=$(sed -n 's/^#define MG_VERSION "\(.*\)"/\1/p' "$CLIENT/src/main.c")
PKG=$WORK/pkg
rm -rf "$PKG" && mkdir -p "$PKG/Scripts" "$PKG/MiSTer_moonlight_crt/core"
cp "$CLIENT"/dist/Scripts/*.sh "$PKG/Scripts/"
cp "$CLIENT"/dist/MiSTer_moonlight_crt/* "$PKG/MiSTer_moonlight_crt/"
cp "$WORK/build/mister_moonlight_crt" "$PKG/MiSTer_moonlight_crt/"
cp "$CLIENT/../README.md" "$PKG/MiSTer_moonlight_crt/" 2>/dev/null || true
# the H264 core goes in MiSTer_moonlight_crt/core (from the core build: H264_<date>.rbf)
touch "$PKG/MiSTer_moonlight_crt/core/.keep"
(cd "$PKG" && rm -f "$WORK/MiSTer_Moonlight_CRT-$VER.zip" && zip -qr "$WORK/MiSTer_Moonlight_CRT-$VER.zip" .)
echo "Ready: $WORK/MiSTer_Moonlight_CRT-$VER.zip"
