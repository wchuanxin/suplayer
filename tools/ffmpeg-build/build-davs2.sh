#!/bin/bash
# Cross-compile libdavs2 (static, AVS2-P2/IEEE.1857.4 video decoder) for HarmonyOS (aarch64 + x86_64)
# Source: https://github.com/pkuvcl/davs2 (tarball snapshot in ./davs2, see README)
# NOTE: davs2 is GPL-2.0. Linking it into libffmpeg.so makes the combined work GPL-2.0.
# Usage: ./build-davs2.sh [arch...]   (default: arm64-v8a x86_64)
set -e

export PATH="/ucrt64/bin:$PATH"   # host binutils (strings) for configure checks

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/davs2"
TC="$SCRIPT_DIR/ohos-toolchain"
NDK=/c/ohos-ndk
OUT="$SCRIPT_DIR/prebuilt"

ARCHS=("$@")
if [ ${#ARCHS[@]} -eq 0 ]; then
  ARCHS=(arm64-v8a x86_64)
fi

for ARCH in "${ARCHS[@]}"; do
  case "$ARCH" in
    arm64-v8a) OHARCH=aarch64 ;;
    x86_64)    OHARCH=x86_64 ;;
    *) echo "unsupported arch: $ARCH" >&2; exit 1 ;;
  esac

  # davs2's configure hardcodes `cd ../../source`, so it must run inside the
  # source tree -> work on a per-arch copy instead of the pristine checkout.
  BUILD="$SCRIPT_DIR/build/davs2-$ARCH"
  echo ">>> building davs2 $ARCH"
  rm -rf "$BUILD"
  cp -r "$SRC" "$BUILD"
  cd "$BUILD/build/linux"

  # --disable-asm: upstream never published the common/aarch64/*.S sources the
  # Makefile references (dir missing on master), so asm builds fail on aarch64.
  CC="$TC/$OHARCH-linux-ohos-clang++" \
  AR="$NDK/llvm/bin/llvm-ar" \
  RANLIB="$NDK/llvm/bin/llvm-ranlib" \
  STRIP="$NDK/llvm/bin/llvm-strip" \
  ./configure \
    --host="$OHARCH-linux" \
    --disable-cli \
    --disable-asm \
    --enable-pic \
    --bit-depth=8

  make -j"$(nproc)" lib-static

  mkdir -p "$OUT/$ARCH" "$OUT/include"
  cp -f libdavs2.a "$OUT/$ARCH/"
  cp -f "$BUILD/source/davs2.h" "$OUT/include/"

  cd - > /dev/null
done

echo ""
echo "=== DONE davs2 ==="
for a in "${ARCHS[@]}"; do
  echo "--- $a"
  ls -lh "$OUT/$a/libdavs2.a"
done
