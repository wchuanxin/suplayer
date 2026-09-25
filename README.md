# suplayer

HarmonyOS media playback stack built on FFmpeg.

## Modules

| Module | Type | Description |
|---|---|---|
| [`ffmpeg`](ffmpeg) | HAR (`@suplayer/ffmpeg`) | FFmpeg-powered native media kernel: probe, frame extraction, audio/video/subtitle decoding, multi-program TS, transcoding. [Docs](ffmpeg/README.md) |
| [`sup`](sup) | HAP (demo) | Demo app exercising every feature of the ffmpeg kernel (HLS live, MPTS, multi-track audio hot switching, DVB subtitles, hardware decode, transcode) |
| [`cmplayer`](cmplayer) | HAR (WIP) | Player UI component library on top of the kernel |
| [`tools/ffmpeg-build`](tools/ffmpeg-build) | build scripts | Cross-compilation of FFmpeg 7.1.1 / OpenH264 2.5.0 / OpenSSL 3.4.1 for HarmonyOS (`arm64-v8a`, `x86_64`) |

## Install

```json5
// oh-package.json5
"dependencies": {
  "@suplayer/ffmpeg": "^1.5.0"
}
```

Published on [OHPM](https://ohpm.openharmony.cn). Full API documentation:
[`ffmpeg/README.md`](ffmpeg/README.md).

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

The prebuilt FFmpeg/OpenH264/OpenSSL libraries in `ffmpeg/src/main/cpp/third_party/`
are produced by the scripts in `tools/ffmpeg-build/` (MSYS2 + OpenHarmony LLVM
toolchain). See [ffmpeg/README.md](ffmpeg/README.md#building-from-source).

## License

The project is licensed under the **LGPL-2.1-or-later** — see [LICENSE](LICENSE).
The `ffmpeg` module statically links FFmpeg 7.1.1, OpenH264 2.5.0 and
OpenSSL 3.4.1; third-party notices and LGPL relinking instructions are in
[ffmpeg/THIRD_PARTY_NOTICES.md](ffmpeg/THIRD_PARTY_NOTICES.md).
