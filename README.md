# suplayer

English | [中文](README_zh.md)

HarmonyOS media playback stack built on FFmpeg.

## Highlights

- **Chinese broadcast codecs** — software decoding for AVS2 (GB/T 33475.2,
  via davs2) and AVS3 (AVS3-P2/IEEE.1857.10, via uavs3d) alongside the full
  FFmpeg decoder set; H.264/HEVC hardware decoding (AVCodec Kit) with
  automatic software fallback — Annex-B (TS/HLS) and length-prefixed
  (MP4/MKV, hvcC/avcC via bitstream filters) framing alike, with HDR10/HLG
  hardware streams routed to the display for tone-mapping.
- **Live streaming** — HLS live/VOD with live-edge joining, master-playlist
  variant selection (`variantIndex` / `maxBandwidth`) and target-latency
  control (`targetLatencyMs`); HTTP/HTTPS (OpenSSL) and RTSP (TCP/UDP);
  persistent connections and auto-reconnect. A shared demux session
  (`sharedDemux`) feeds every decoder over one connection, rebuilds itself
  in place on token expiry / CDN errors, and reports queue health through
  `overflow` / `discontinuity` events plus buffer/bitrate statistics.
- **Broadcast TS** — multi-program TS (MPTS) with the PSI program table
  (PAT/PMT/SDT), program-locked playback, DVB bitmap subtitles (enable from
  the PMT declaration alone) and multi-track audio with zero-gap hot
  switching.
- **VOD control** — runtime seek (`seekTo`, keyframe-anchored, hardware
  flush aware), precise `startTimeMs` start offsets and frame capture
  (`captureFrame`, RGBA8888) for snapshots.
- **Custom I/O** — JS-backed byte sources (`registerStreamIO` +
  `"streamio:<id>"` URLs) bridge SMB shares, custom caches and encrypted
  stores into probe / decode / transcode without touching C++.
- **Transcoding & remuxing** — decode → filter → encode (OpenH264 + AAC) →
  MP4/MPEG-TS muxing, plus stream-copy remux (`vcodec` / `acodec: 'copy'`)
  and extra filter chains (`hqdn3d`, `unsharp`, ...).
- **Player components** — `cmplayer` ships AV-sync player UI (`SupPlayer`)
  with pause/resume, track switching and drift correction; the `sup` demo
  app exercises every kernel capability.

## Modules

| Module | Type | Description |
|---|---|---|
| [`ffmpeg`](ffmpeg) | HAR (`@suplayer/ffmpeg`) | FFmpeg-powered native media kernel: probe (PSI program table, chapters/tags), frame extraction, audio/video/subtitle decoding with hardware fallback, VOD seek & frame capture, multi-program TS, DVB subtitles, shared-demux live streaming, custom I/O (streamio), transcoding/remux. [Docs](ffmpeg/README.md) |
| [`sup`](sup) | HAP (demo) | Demo app exercising every feature of the kernel: probe with stream cards and program table, MPTS program playback, frame extraction, audio decode with hot track switching, unified video+audio playback (pause/resume, hw selector, subtitle track selection), DVB subtitle decoding, transcoding |
| [`cmplayer`](cmplayer) | HAR | Player UI component library on top of the kernel: `SupPlayer` component + `SupPlayerController` (audio master clock, drift correction, audio track switching). [Docs](cmplayer/README.md) |
| [`tools/ffmpeg-build`](tools/ffmpeg-build) | build scripts | Cross-compilation of FFmpeg 7.1.1 / OpenH264 2.5.0 / davs2 (AVS2) / uavs3d (AVS3) / OpenSSL 3.4.1 for HarmonyOS (`arm64-v8a`, `x86_64`) |

## Install

```json5
// oh-package.json5
"dependencies": {
  "@suplayer/ffmpeg": "^1.10.0"
}
```

Published on [OHPM](https://ohpm.openharmony.cn). Full API documentation:
[`ffmpeg/README.md`](ffmpeg/README.md), release notes in
[`ffmpeg/CHANGELOG.md`](ffmpeg/CHANGELOG.md). Since **1.7.0** the prebuilt
`libffmpeg.so` is GPL-3.0-or-later (adds AVS2/AVS3 decoding); 1.6.x is the
last LGPL build (see [License](#license)).

## Quick example

```ts
import { probe, FFVideoDecoder } from '@suplayer/ffmpeg';

// 1. HLS live: join at the edge and hold a ~6 s latency
const info = await probe('https://example.com/live/index.m3u8');
const live = new FFVideoDecoder();
await live.open('https://example.com/live/index.m3u8', -1, {
  liveStartIndex: -1,    // join at the live edge
  targetLatencyMs: 6000  // keep converging on ~6 s
});
live.start(surfaceId);   // from XComponent

// 2. VOD (private connection): runtime seek + frame capture
const vod = new FFVideoDecoder();
await vod.open(vodUrl, -1, { hw: 'never' }); // sw decode for snapshots
vod.start(surfaceId);
await vod.seekTo(90000);              // runtime seek to 90 s
const snap = await vod.captureFrame(); // RGBA8888 frame
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
