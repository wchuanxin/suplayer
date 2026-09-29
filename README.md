# suplayer

English | [中文](README_zh.md)

HarmonyOS media playback stack built on FFmpeg.

## Highlights

- **Chinese broadcast codecs** — software decoding for AVS2 (GB/T 33475.2,
  via davs2) and AVS3 (AVS3-P2/IEEE.1857.10, via uavs3d) alongside the full
  FFmpeg decoder set; H.264/HEVC hardware decoding (AVCodec Kit) with
  automatic software fallback.
- **Live streaming** — HLS live/VOD with live-edge joining, HTTP/HTTPS
  (OpenSSL), persistent connections and auto-reconnect.
- **Broadcast TS** — multi-program TS (MPTS) with the PSI program table
  (PAT/PMT/SDT), program-locked playback, DVB bitmap subtitles (enable from
  the PMT declaration alone) and multi-track audio with zero-gap hot
  switching.
- **Transcoding** — one-shot decode → filter → encode (OpenH264 + AAC) →
  MP4/MPEG-TS muxing.
- **Player components** — `cmplayer` ships AV-sync player UI (`SupPlayer`)
  with pause/resume, track switching and drift correction; the `sup` demo
  app exercises every kernel capability.

## Modules

| Module | Type | Description |
|---|---|---|
| [`ffmpeg`](ffmpeg) | HAR (`@suplayer/ffmpeg`) | FFmpeg-powered native media kernel: probe, frame extraction, audio/video/subtitle decoding, multi-program TS, transcoding. [Docs](ffmpeg/README.md) |
| [`sup`](sup) | HAP (demo) | Demo app exercising every feature of the kernel: probe with stream cards and program table, MPTS program playback, frame extraction, audio decode with hot track switching, unified video+audio playback (pause/resume, hw selector, live subtitle track selection), DVB subtitle decoding, transcoding |
| [`cmplayer`](cmplayer) | HAR | Player UI component library on top of the kernel: `SupPlayer` component + `SupPlayerController` (audio master clock, drift correction, audio track switching). [Docs](cmplayer/README.md) |
| [`tools/ffmpeg-build`](tools/ffmpeg-build) | build scripts | Cross-compilation of FFmpeg 7.1.1 / OpenH264 2.5.0 / davs2 (AVS2) / uavs3d (AVS3) / OpenSSL 3.4.1 for HarmonyOS (`arm64-v8a`, `x86_64`) |

## Install

```json5
// oh-package.json5
"dependencies": {
  "@suplayer/ffmpeg": "^1.7.0"
}
```

Published on [OHPM](https://ohpm.openharmony.cn). Full API documentation:
[`ffmpeg/README.md`](ffmpeg/README.md). **1.7.0** adds AVS2/AVS3 decoding;
from this version the prebuilt `libffmpeg.so` is GPL-3.0-or-later, 1.6.x is
the last LGPL build (see [License](#license)).

## Quick example

```ts
import { probe, FFVideoDecoder } from '@suplayer/ffmpeg';

const info = await probe('https://example.com/live/index.m3u8');
const vdec = new FFVideoDecoder();
await vdec.open('https://example.com/live/index.m3u8', -1, {
  liveStartIndex: -1   // join at the live edge
});
vdec.start(surfaceId); // from XComponent
```

## Building the native libraries

The prebuilt FFmpeg/OpenH264/OpenSSL/davs2/uavs3d libraries in `ffmpeg/src/main/cpp/third_party/`
are produced by the scripts in `tools/ffmpeg-build/` (MSYS2 + OpenHarmony LLVM
toolchain). See [ffmpeg/README.md](ffmpeg/README.md#building-from-source).

## License

The project source code is licensed under the **LGPL-2.1-or-later** — see
[LICENSE](LICENSE). The `ffmpeg` module's prebuilt `libffmpeg.so` statically
links FFmpeg 7.1.1, OpenH264 2.5.0, OpenSSL 3.4.1, davs2 (AVS2, GPL-2.0-or-later)
and uavs3d (AVS3, BSD-2-Clause), and is therefore distributed under the
**GPL-3.0-or-later** — see [ffmpeg/LICENSE](ffmpeg/LICENSE). Third-party
notices and relinking instructions are in
[ffmpeg/THIRD_PARTY_NOTICES.md](ffmpeg/THIRD_PARTY_NOTICES.md).
