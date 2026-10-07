#!/usr/bin/env bash
# Host-side unit tests for the native sources that build without the OHOS
# SDK (PacketQueue today). Requires a host g++ (MSYS2/MinGW on Windows, gcc
# on Linux/macOS); override with CXX=<compiler> if it is not on PATH.
#
# Usage: tools/host-tests.sh
set -eu

# A proper mingw environment is required: in a plain MSYS login shell (no
# MSYSTEM, or MSYSTEM=MSYS) the compiler spawns cc1plus, which fails
# SILENTLY with exit 1. Re-exec under UCRT64 when not already in a mingw
# environment.
case "${MSYSTEM:-}" in
    MINGW32|MINGW64|UCRT64|CLANG64) ;;
    *) MSYSTEM=UCRT64 exec bash -l "$0" "$@" ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CPP="$ROOT/ffmpeg/src/main/cpp"
OUT="$ROOT/build/host_tests"
mkdir -p "$OUT"

CXX="${CXX:-g++}"

# g++ not on PATH: try the common MSYS2 locations.
if ! command -v "$CXX" >/dev/null 2>&1; then
    for candidate in /mingw64/bin/g++.exe /ucrt64/bin/g++.exe /mingw32/bin/g++.exe; do
        # Resolve inside an MSYS2 root when running from Windows shells.
        for base in "C:/msys64" "/c/msys64" ""; do
            if [ -n "$base" ] && [ -x "$base$candidate" ]; then
                CXX="$base$candidate"
                break 2
            fi
        done
    done
fi

echo "compiler: $CXX"
"$CXX" --version | head -n 1 || true

# The compiler's bin dir must be on PATH: g++ spawns cc1plus/as from a
# different directory, and those load their DLLs (libgmp, libwinpthread, ...)
# from the bin dir — without it the spawn fails silently with exit 1.
CXX_BIN="$(dirname "$CXX")"
export PATH="$CXX_BIN:/usr/bin:$PATH"

# -static: the test exe must run without the mingw runtime DLLs on PATH
# (libstdc++/libwinpthread), e.g. straight from a CI job or Explorer.
"$CXX" -std=c++17 -O1 -pthread -static -DFFMPEG_HOST_TEST \
    -I"$CPP/napi/host_tests/stub" \
    -I"$CPP/napi" \
    "$CPP/napi/packet_queue.cpp" \
    "$CPP/napi/host_tests/test_packet_queue.cpp" \
    -o "$OUT/test_packet_queue"

"$OUT/test_packet_queue"
