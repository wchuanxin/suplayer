#!/bin/bash
# Cross-compile libuavs3d (static, AVS3-P2/IEEE.1857.10 video decoder) for HarmonyOS (aarch64 + x86_64)
# Source: https://github.com/uavs3/uavs3d (tarball snapshot in ./uavs3d, see README)
# License: BSD-2-Clause (LGPL-compatible)
# Usage: ./build-uavs3d.sh [arch...]   (default: arm64-v8a x86_64)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/uavs3d"
CMAKE=/c/ohos-ndk/build-tools/cmake/bin/cmake.exe
TOOLCHAIN=/c/ohos-ndk/build/cmake/ohos.toolchain.cmake
OUT="$SCRIPT_DIR/prebuilt"

ARCHS=("$@")
if [ ${#ARCHS[@]} -eq 0 ]; then
  ARCHS=(arm64-v8a x86_64)
fi

# The tarball snapshot has no .git history; give version.sh a stable fallback
# (FFmpeg requires uavs3d >= 1.1.41 via pkg-config).
if [ ! -d "$SRC/.git" ]; then
  sed -i 's|^VER_R=.*|VER_R=100|; s|^VER_L=.*|VER_L=100|; s|^VER_SHA1=.*|VER_SHA1=tarball-snapshot|' "$SRC/version.sh"
fi

for ARCH in "${ARCHS[@]}"; do
  case "$ARCH" in
    arm64-v8a) OHARCH=aarch64 ;;
    x86_64)    OHARCH=x86_64 ;;
    *) echo "unsupported arch: $ARCH" >&2; exit 1 ;;
  esac

  BUILD="$SCRIPT_DIR/build/uavs3d-$ARCH"
  echo ">>> building uavs3d $ARCH"
  rm -rf "$BUILD"
  mkdir -p "$BUILD"
  cd "$BUILD"

  "$CMAKE" -G "Unix Makefiles" \
    -DOHOS_ARCH="$ARCH" \
    -DOHOS_PLATFORM=OHOS \
    -DOHOS_STL=c++_shared \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    "$SRC"

  "$CMAKE" --build . --target uavs3d -j"$(nproc)"

  mkdir -p "$OUT/$ARCH" "$OUT/include"
  cp -f source/libuavs3d.a "$OUT/$ARCH/"
  cp -f "$SRC/source/decoder/uavs3d.h" "$OUT/include/"

  cd - > /dev/null
done

echo ""
echo "=== DONE uavs3d ==="
for a in "${ARCHS[@]}"; do
  echo "--- $a"
  ls -lh "$OUT/$a/libuavs3d.a"
done
