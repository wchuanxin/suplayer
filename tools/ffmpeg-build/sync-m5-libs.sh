#!/bin/bash
set -e
cd /d/DevEcoStudioProjects/suplayer
P=tools/ffmpeg-build/prebuilt
D=ffmpeg/src/main/cpp/third_party/ffmpeg/libs
for abi in arm64-v8a x86_64; do
  cp -f $P/$abi/libavcodec.a $P/$abi/libavformat.a $P/$abi/libavutil.a \
        $P/$abi/libavfilter.a $P/$abi/libswscale.a $P/$abi/libswresample.a \
        $P/$abi/libopenh264.a $P/$abi/libssl.a $P/$abi/libcrypto.a $D/$abi/
done
# libc++_shared.so: link-time copy in third_party + packaging copy in HAR libs/
T=aarch64-linux-ohos
U=x86_64-linux-ohos
NDK=/c/ohos-ndk/llvm/lib
cp -f $NDK/$T/libc++_shared.so $D/arm64-v8a/
cp -f $NDK/$U/libc++_shared.so $D/x86_64/
mkdir -p ffmpeg/libs/arm64-v8a ffmpeg/libs/x86_64
cp -f $NDK/$T/libc++_shared.so ffmpeg/libs/arm64-v8a/
cp -f $NDK/$U/libc++_shared.so ffmpeg/libs/x86_64/
ls -la $D/arm64-v8a/ $D/x86_64/ ffmpeg/libs/arm64-v8a/ ffmpeg/libs/x86_64/
