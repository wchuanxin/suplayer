#!/bin/bash
# Cross-compile OpenSSL 3.4.1 (static) for HarmonyOS (aarch64 + x86_64)
# Usage: ./build-openssl.sh [arch...]   (default: arm64-v8a x86_64)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/openssl-3.4.1"
TC="$SCRIPT_DIR/ohos-toolchain"
NDK=/c/ohos-ndk
OUT="$SCRIPT_DIR/prebuilt"

ARCHS=("$@")
if [ ${#ARCHS[@]} -eq 0 ]; then
  ARCHS=(arm64-v8a x86_64)
fi

for ARCH in "${ARCHS[@]}"; do
  case "$ARCH" in
    arm64-v8a) TARGET=linux-aarch64 ; CC="$TC/aarch64-linux-ohos-clang" ;;
    x86_64)    TARGET=linux-x86_64  ; CC="$TC/x86_64-linux-ohos-clang" ;;
    *) echo "unsupported arch: $ARCH" >&2; exit 1 ;;
  esac

  BUILD="$SCRIPT_DIR/build/openssl-$ARCH"
  echo ">>> building openssl $ARCH"
  rm -rf "$BUILD"
  mkdir -p "$BUILD"
  cd "$BUILD"

  # no-apps/no-tests/no-docs: only libcrypto.a + libssl.a are needed
  "$SRC/Configure" "$TARGET" \
    --prefix="$BUILD/install" \
    --openssldir="/system/etc/ssl" \
    no-shared no-apps no-tests no-docs \
    CC="$CC" \
    AR="$NDK/llvm/bin/llvm-ar" \
    RANLIB="$NDK/llvm/bin/llvm-ranlib" \
    -Oz -ffunction-sections -fdata-sections

  make -j"$(nproc)" -s
  make -j"$(nproc)" -s install_dev

  # publish into prebuilt tree: libssl.a/libcrypto.a per arch + shared include
  # (x86_64 target installs into lib64)
  LIBDIR=lib
  [ -d "$BUILD/install/lib64" ] && LIBDIR=lib64
  mkdir -p "$OUT/$ARCH"
  cp -f "$BUILD/install/$LIBDIR/libssl.a" "$BUILD/install/$LIBDIR/libcrypto.a" "$OUT/$ARCH/"
  mkdir -p "$OUT/include/openssl"
  cp -f "$BUILD/install/include/openssl/"*.h "$OUT/include/openssl/"

  cd - > /dev/null
done

echo ""
echo "=== DONE openssl ==="
for a in "${ARCHS[@]}"; do
  echo "--- $a"
  ls -lh "$OUT/$a/libssl.a" "$OUT/$a/libcrypto.a"
done
