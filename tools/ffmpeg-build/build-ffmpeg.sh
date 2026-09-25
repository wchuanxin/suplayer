#!/bin/bash
# Cross-compile FFmpeg for HarmonyOS (aarch64 + x86_64)
# Usage (from MSYS2): ./build-ffmpeg.sh [minimal|full] [arch...]   (default arch: arm64-v8a x86_64)
set -e

export PATH="/ucrt64/bin:$PATH"   # mingw gcc as host cc for tablegen tools

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/ffmpeg-7.1.1"
TC="$SCRIPT_DIR/ohos-toolchain"
NDK=/c/ohos-ndk
MODE="${1:-full}"
shift || true
ARCHS=("$@")
if [ ${#ARCHS[@]} -eq 0 ]; then
  ARCHS=(arm64-v8a x86_64)
fi

COMMON_FLAGS=(
  --enable-cross-compile
  --target-os=linux
  --enable-pic
  --enable-static
  --disable-shared
  --disable-programs
  --disable-doc
  --disable-htmlpages
  --disable-manpages
  --disable-podpages
  --disable-txtpages
  --disable-avdevice
  --disable-postproc
  --disable-debug
  --disable-symver
  --disable-vulkan
  --disable-vaapi
  --disable-vdpau
  --enable-small
  --extra-cflags="-ffunction-sections -fdata-sections -Oz"
  --extra-ldflags="-Wl,--gc-sections"
)

if [ "$MODE" = "minimal" ]; then
  # toolchain validation config: smallest possible feature set
  CONFIG_FLAGS=(
    --disable-everything
    --disable-network
    --enable-demuxer=mov,mpegts
    --enable-decoder=h264,aac
    --enable-protocol=file
  )
elif [ "$MODE" = "m5" ] || [ "$MODE" = "m7" ]; then
  # M5: decode-side full config + openh264/aac encoders + mp4/mpegts muxers
  # M7: M5 + OpenSSL (https/tls protocols)
  CONFIG_FLAGS=(
    --disable-everything
    --enable-network
    --enable-protocols
    --enable-demuxers
    --enable-parsers
    --enable-bsfs
    --enable-decoders
    --enable-filters
    --enable-swscale
    --enable-swresample
    --enable-avformat
    --enable-avcodec
    --enable-avfilter
    --enable-zlib
    --enable-libopenh264
    --enable-encoder=libopenh264,aac
    --enable-muxer=mp4,mpegts
  )
  if [ "$MODE" = "m7" ]; then
    CONFIG_FLAGS+=(--enable-openssl)
  fi
else
  # full decode-side config (M1-M4 scope)
  CONFIG_FLAGS=(
    --disable-everything
    --enable-network
    --enable-protocols
    --enable-demuxers
    --enable-parsers
    --enable-bsfs
    --enable-decoders
    --enable-filters
    --enable-swscale
    --enable-swresample
    --enable-avformat
    --enable-avcodec
    --enable-avfilter
    --enable-zlib
    --disable-encoders
    --disable-muxers
  )
fi

build_arch() {
  local ARCH="$1"
  local BUILD="$SCRIPT_DIR/build/$ARCH-$MODE"
  local OUT="$SCRIPT_DIR/prebuilt/$ARCH"
  local ARCH_FLAGS=()

  case "$ARCH" in
    arm64-v8a)
      ARCH_FLAGS=(--arch=aarch64 --cpu=armv8-a
        --cc="$TC/aarch64-linux-ohos-clang"
        --cxx="$TC/aarch64-linux-ohos-clang++")
      ;;
    x86_64)
      ARCH_FLAGS=(--arch=x86_64 --cpu=x86-64
        --cc="$TC/x86_64-linux-ohos-clang"
        --cxx="$TC/x86_64-linux-ohos-clang++")
      ;;
    *)
      echo "unsupported arch: $ARCH" >&2
      exit 1
      ;;
  esac

  echo ">>> building $ARCH ($MODE)"
  rm -rf "$BUILD"
  mkdir -p "$BUILD"
  cd "$BUILD"

  if [ "$MODE" = "m5" ] || [ "$MODE" = "m7" ]; then
    # per-arch pkg-config for static libopenh264 (+ libc++_shared for its C++ runtime)
    local TRIPLE
    case "$ARCH" in
      arm64-v8a) TRIPLE=aarch64-linux-ohos ;;
      x86_64)    TRIPLE=x86_64-linux-ohos ;;
    esac
    mkdir -p pkgconfig
    cat > pkgconfig/openh264.pc <<EOF
Name: openh264
Description: OpenH264 (static)
Version: 2.5.0
Libs: -L$SCRIPT_DIR/prebuilt/$ARCH -lopenh264 -L/c/ohos-ndk/llvm/lib/$TRIPLE -lc++_shared
Cflags: -I$SCRIPT_DIR/prebuilt/include
EOF
    if [ "$MODE" = "m7" ]; then
      cat > pkgconfig/openssl.pc <<EOF
Name: openssl
Description: OpenSSL (static)
Version: 3.4.1
Requires: libssl libcrypto
Cflags: -I$SCRIPT_DIR/prebuilt/include
EOF
      cat > pkgconfig/libssl.pc <<EOF
Name: libssl
Description: OpenSSL SSL library (static)
Version: 3.4.1
Libs: -L$SCRIPT_DIR/prebuilt/$ARCH -lssl -lcrypto
Cflags: -I$SCRIPT_DIR/prebuilt/include
EOF
      cat > pkgconfig/libcrypto.pc <<EOF
Name: libcrypto
Description: OpenSSL crypto library (static)
Version: 3.4.1
Libs: -L$SCRIPT_DIR/prebuilt/$ARCH -lcrypto
Cflags: -I$SCRIPT_DIR/prebuilt/include
EOF
    fi
    export PKG_CONFIG_PATH="$BUILD/pkgconfig"
  fi

  "$SRC/configure" "${COMMON_FLAGS[@]}" "${ARCH_FLAGS[@]}" "${CONFIG_FLAGS[@]}"
  make -j"$(nproc)"

  mkdir -p "$OUT"
  for lib in libavcodec libavformat libavutil libswresample libswscale libavfilter; do
    cp -f "$lib/$lib.a" "$OUT/"
  done
  # generated public headers (avconfig.h / ffversion.h), once per arch
  mkdir -p "$SCRIPT_DIR/prebuilt/include/libavutil"
  cp -f libavutil/avconfig.h "$SCRIPT_DIR/prebuilt/include/libavutil/"
  cp -f libavutil/ffversion.h "$SCRIPT_DIR/prebuilt/include/libavutil/"

  cd - > /dev/null
}

for a in "${ARCHS[@]}"; do
  build_arch "$a"
done

echo ""
echo "=== DONE ($MODE) ==="
for a in "${ARCHS[@]}"; do
  echo "--- $a"
  ls -lh "$SCRIPT_DIR/prebuilt/$a" | tail -n +2
done
