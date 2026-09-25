#!/bin/bash
# Cross-compile libopenh264 (static) for HarmonyOS (aarch64 + x86_64)
# Usage: ./build-openh264.sh [arch...]   (default: arm64-v8a x86_64)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/openh264-2.5.0"
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

  BUILD="$SCRIPT_DIR/build/openh264-$ARCH"
  echo ">>> building openh264 $ARCH"
  rm -rf "$BUILD"
  mkdir -p "$BUILD"
  cd "$BUILD"

  make -f "$SRC/Makefile" \
    OS=linux ARCH=$OHARCH BUILDTYPE=Release ENABLEPIC=Yes \
    CC="$TC/$OHARCH-linux-ohos-clang" \
    CXX="$TC/$OHARCH-linux-ohos-clang++" \
    AR="$NDK/llvm/bin/llvm-ar" \
    STRIP="$NDK/llvm/bin/llvm-strip" \
    -j"$(nproc)" libopenh264.a

  mkdir -p "$OUT/$ARCH"
  cp -f libopenh264.a "$OUT/$ARCH/"
  mkdir -p "$OUT/include/wels"
  cp -f "$SRC"/codec/api/wels/*.h "$OUT/include/wels/"

  cd - > /dev/null
done

echo ""
echo "=== DONE openh264 ==="
for a in "${ARCHS[@]}"; do
  echo "--- $a"
  ls -lh "$OUT/$a/libopenh264.a"
done
