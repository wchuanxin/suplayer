# @suplayer/ffmpeg

FFmpeg-powered media kernel for HarmonyOS (compatible with API 24+,
HarmonyOS 6.1.1 and later). Ships as a HAR with prebuilt native libraries for
`arm64-v8a` and `x86_64`.

## Features

- **Media probing** — container/codec/stream metadata for any FFmpeg-supported
  format, including raw MPEG-TS streams with multi-audio/subtitle tracks and
  language tags, plus the PSI program table (PAT/PMT/SDT) of multi-program
  transport streams.
- **Frame extraction** — decode and extract a video frame at a given timestamp
  (RGBA, ready for `PixelMap`).
- **Audio decoding** — software decoding to PCM (F32LE interleaved) at the
  source's sample rate and channel layout (mono through 7.1; exotic or >8ch
  layouts are downmixed to stereo), with realtime pacing, instant hot track
  switching (all audio tracks decoded in parallel, switching re-routes the
  output with zero re-buffering), pause/resume.
- **Video decoding** — software decoding with direct `XComponent`/`OH_NativeWindow`
  Surface rendering. Chinese AVS standards are supported in software: AVS2
  (GB/T 33475.2, via libdavs2) and AVS3 (AVS3-P2/IEEE.1857.10, via libuavs3d),
  as found in CCTV 4K/UHD broadcast TS streams (stream types 0xd2/0xd4). AVS1
  (CAVS, stream type 0x42) decodes only in its Jizhun baseline profile; the
  AVS+ broadcast profile (AVS-P16) is not supported by upstream FFmpeg/davs2.
- **Subtitles** — DVB/broadcast subtitle decoding to composited RGBA bitmaps
  (or UTF-8 text), enabled from the container's declaration (a PMT entry is
  enough, even when the subtitle PID never delivers data).
- **Multi-program TS (MPTS)** — lock playback to a single MPEG-TS program by
  its program number; other programs' streams are discarded at the demuxer.
- **Transcoding / muxing** — full pipeline: decode → filter (`setpts`, `scale`,
  `fps`) → encode (OpenH264 for H.264, native AAC for audio) → MP4/MPEG-TS mux
  with `+faststart`.
- **Network & live streaming** — HTTPS (OpenSSL), HTTP, HLS (live and VOD),
  plain TS over HTTP. `src` accepts URLs anywhere a file path is accepted.

All processing runs in native worker threads; the JS API is promise/callback
based and never blocks the UI thread. Native diagnostics are forwarded to
`hilog` under domain `0xFF00`; entries are app-type logs, so they print with
the leading type marker — filter with `0xff00` (shown as `A0xff00` in raw
output). Tags: `FFmpeg` (libavformat/libavcodec log forward), `FFDemux`
(shared demuxer session), `FFVideo` / `FFVideoHW` (software / hardware video
path), `FFAudioSW` (audio), `FFSub` / `FFSubtitle` (subtitles).

## Installation

```bash
ohpm install @suplayer/ffmpeg
```

Or declare the dependency in `oh-package.json5`:

```json5
// oh-package.json5
"dependencies": {
  "@suplayer/ffmpeg": "^1.7.0"
}
```

> Version note: **1.7.0** adds AVS2/AVS3 decoding; from this version on the
> prebuilt `libffmpeg.so` is GPL-3.0-or-later (see [License](#license)).
> 1.6.x is the last LGPL-2.1-or-later build (no AVS2 support).

## Permissions

Network sources (HTTP/HTTPS/HLS) require the Internet permission in the
consuming HAP's `module.json5`:

```json5
"requestPermissions": [
  { "name": "ohos.permission.INTERNET" }
]
```

Local file playback needs no permission when reading files inside the app's
own sandbox; other paths require the usual file access permissions.

## Quick start

```ts
import {
  probe, extractFrame, transcode,
  FFAudioDecoder, FFVideoDecoder,
  FFMediaInfo, FFFrame, FFTranscodeOptions, FFTranscodeResult
} from '@suplayer/ffmpeg';

// 1. Probe a media file
const info: FFMediaInfo = await probe('/data/app/el2/100/base/.../files/input.ts');
console.info(info.format, info.duration, info.streams.length);

// 2. Extract a frame (RGBA)
const frame: FFFrame = await extractFrame(path, 5000); // ms

// 3. Decode + play audio through AudioRenderer
const dec = new FFAudioDecoder();
const info = await dec.open(path, 1); // stream #1
dec.on('pcm', (pkt) => renderer.write(pkt.data));
dec.on('end', () => renderer.stop());
dec.start(); // paced=true (realtime); pass false for max-speed decoding

// Switch audio track while playing (multi-track TS)
dec.switchTrack(2);

// 4. Render video onto an XComponent surface
const vdec = new FFVideoDecoder();
const vinfo = await vdec.open(path);
vdec.on('frame', (ev) => console.info(`frame @${ev.ptsMs}ms`));
vdec.start(surfaceId); // from XComponent

// 5. Transcode: TS -> MP4 with scaling
const result: FFTranscodeResult = await transcode(srcPath, {
  dst: outPath,
  width: 160,
  height: 90,
  videoBitrate: 400000,
  audioBitrate: 64000
});

// 6. Play an HLS live stream (HTTPS), starting at the live edge
const live = new FFVideoDecoder();
await live.open('https://example.com/live/index.m3u8', -1, {
  liveStartIndex: -1,        // first segment from the end = lowest latency
  analyzeMs: 5000,           // cap stream-info analysis for faster startup
  allowedExtensions: 'ALL'   // for playlists with disguised segment URLs
});
live.start(surfaceId);

// 7. Multi-program TS: probe the program table, then play one program
const tsInfo: FFMediaInfo = await probe('https://example.com/mpts.ts');
for (const p of tsInfo.programs) {
  console.info(`program ${p.programNum} "${p.serviceName}" PMT ${p.pmtPid}`);
}
const vdec2 = new FFVideoDecoder();
await vdec2.open('https://example.com/mpts.ts', -1, { program: 101 });
vdec2.start(surfaceId); // only program 101's streams are demuxed
```

## API

### `probe(src: string, options?: FFNetOptions): Promise<FFMediaInfo>`

Returns container metadata and the full stream list. Each `FFStreamInfo`
carries `index`, `type` (`video`/`audio`/`subtitle`/...), codec name, dimensions
or sample rate/channels, `language`, duration and bitrate. For raw MPEG-TS
inputs each stream also carries `pid` (its transport PID) and `programIndex`
(index into `programs`); other containers report `pid: -1` because their
stream ids are track/playlist indexes rather than PIDs.

`FFMediaInfo.programs` lists the PSI program table (PAT/PMT/SDT) of
multi-program transport streams: `programNum`, `pmtPid`, `pcrPid`,
`pmtVersion` (PMT version_number, `-1` when unknown), `flags` (bit 0 =
`AV_PROGRAM_RUNNING`), SDT `serviceName`/`serviceProvider` and the
`streamIndexes` belonging to each program. `pmtPid`/`pcrPid`/`pmtVersion` are
`-1` when the input carries no PSI. HLS exposes one synthetic program per
variant (`programNum` 0, no SDT name, no PIDs) while MP4 reports none.

`FFMediaInfo.chapters` lists container chapters (mp4/MKV chapter tracks) as
`{ id, startMs, endMs, title }`, and `FFMediaInfo.tags` carries all container
metadata tags (`Record<string, string>`: title, artist, encoder,
service_name, ...). Each `FFStreamInfo` additionally reports
`dispositionDefault` / `dispositionForced` (container disposition bits, for
default track selection and forced narratives) and `rotation` (clockwise
degrees from the display matrix, 0/90/180/270).

### `extractFrame(src: string, timeMs: number): Promise<FFFrame>`

Decodes the nearest video frame at `timeMs` and returns RGBA pixels
(`width`, `height`, `data: ArrayBuffer`).

### `transcode(src: string, options: FFTranscodeOptions): Promise<FFTranscodeResult>`

One-shot transcode on a background worker. `FFTranscodeOptions`:

| Field | Default | Description |
|---|---|---|
| `dst` | (required) | Output path; container inferred from extension (`.mp4`, `.ts`) |
| `width` / `height` | source size | Scale filter target |
| `fps` | source fps | fps filter target |
| `videoBitrate` | 800000 | H.264 (OpenH264) target bitrate, bps |
| `audioBitrate` | 96000 | AAC target bitrate, bps; `0` disables the audio track |
| `startTimeMs` | 0 | Seek into the source before encoding |
| `durationMs` | 0 (to EOF) | Stop encoding after this span |
| `vcodec` | `'h264'` | Video codec: `'h264'` (libopenh264), `'mpeg2'` (mpeg2video) or `'copy'` — remux the video track without re-encoding (h264/hevc inputs are converted to Annex-B when the output is mpegts) |
| `acodec` | `'aac'` | Audio codec: `'aac'`, `'ac3'` or `'copy'` — remux the audio track without re-encoding |
| `filter` | none | Extra libavfilter video chain inserted before scale/fps (e.g. `'hqdn3d=4:3:6:4.5'`, `'unsharp=5:5:1.0'`) |

`FFTranscodeResult` reports `outputSize` (bytes), `durationMs` of the produced
file and encoded video `frames`. Output timestamps are normalized to start at
zero.

### `FFNetOptions`

Optional network/live options accepted by `probe` and both decoders' `open`:

| Field | Default | Description |
|---|---|---|
| `userAgent` | `Lavf/7.1.1` | HTTP `User-Agent`; also applied to all HLS segment/key downloads |
| `headers` | none | Extra HTTP headers, CRLF-separated (e.g. `"Referer: https://x\r\nCookie: y=z"`) |
| `allowedExtensions` | FFmpeg whitelist | HLS segment extension whitelist, comma-separated. Pass `'ALL'` (and setting this at all) disables strict extension/format matching for streams whose playlist/segment URLs use disguised extensions (e.g. `.php` playlists, `.jpg`-named TS segments) |
| `liveStartIndex` | `-3` | HLS live: segment index to start at; negative counts back from the live edge. `-1` starts at the newest segment (lowest latency, may join mid-GOP) |
| `analyzeMs` | FFmpeg default | Cap `avformat_find_stream_info` analysis duration in ms — lower values speed up live startup |
| `hw` | `'auto'` | Video decode backend: `'auto'` prefers the hardware decoder and falls back to software on failure, `'force'` requires hardware, `'never'` always uses software (FFVideoDecoder only) |
| `program` | unset | MPEG-TS program number to lock playback to. When set, only streams of that program are demuxed; automatic stream selection stays inside the program and explicit `streamIndex` values are validated against it. Unknown program numbers fail the `open` |
| `httpPersistent` | `true` | Keep one HTTP/TCP connection alive across HLS playlist refreshes and segment downloads (`http_persistent=1`) instead of reconnecting for every request — cuts live-edge latency and TLS handshakes |
| `reconnect` | `true` | Auto-retry failed network requests: `reconnect=1`, `reconnect_streamed=1`, `reconnect_on_network_error=1`, `reconnect_on_http_error=4xx,5xx`, `reconnect_delay_max=5` — survives CDN edge-misses (momentary 404/410 on freshly announced segments) and brief network drops |
| `variantIndex` | unset | HLS master-playlist variant selection: pick the variant at this 0-based index. Takes precedence over `maxBandwidth` |
| `maxBandwidth` | `0` | HLS variant bandwidth cap: pick the highest-bandwidth variant whose `BANDWIDTH <= maxBandwidth` (all exceeding → the lowest). `0` = no cap. The master playlist is fetched once before the open and the chosen variant URL replaces the source; any fetch/parse failure falls back to the original URL |
| `targetLatencyMs` | `0` | Live target latency: derive `liveStartIndex` from the media playlist's `TARGETDURATION` (`-ceil(target/segDur)`, clamped 1..30) when no explicit `liveStartIndex` is set, and keep converging on it — the shared session drops subscriber backlogs exceeding `target + max(2s, target/2)` every ~5s. `0` = off |
| `autoReconnect` | auto | In-place session rebuild after 401/403/404/429 read errors (token expiry, sliding-window miss, rate limit): the shared session re-resolves the source (fresh CDN token) and reopens, verifying the stream layout; subscribers get `'reconnected'` + `discontinuity('reconnect')`. `true` = always on, `false` = always off, unset = auto (live on, VOD off) |
| `reconnectMaxRetries` | `3` | Rebuild attempts before the session gives up (EOF); exponential backoff 1s..8s (2s..16s for HTTP 429) |
| `dvrSeek` | `false` | Reserved (DVR window seek over the shared connection); no effect yet |
| `audioTracks` | `'all'` | `FFAudioDecoder` track decode mode: `'all'` decodes every audio track in parallel (zero-gap hot switching); `'first'` decodes only the selected track and opens the target track lazily on `switchTrack` — lower CPU/power on multi-track sources, with a brief splice instead of a seamless switch |
| `frameEventMs` | `0` | Minimum interval in ms between `FFVideoDecoder` `'frame'` events (`0` = every frame). The first frame is always emitted; larger values throttle the NAPI bridge traffic for apps that only need periodic PTS updates (A/V sync, progress) |
| `startTimeMs` | `0` | VOD start position in ms: the decoder seeks to the keyframe at/before the target and drops pre-target frames (video: decode but no render/emit, audio: no resample/emit), so playback starts precisely at the requested position. `0` = from head. Ignored when the source is not seekable (live streams fall back to normal startup) |
| `sharedDemux` | `false` | Shared demuxer: every decoder (video / audio / subtitle) opened for the same source with matching demux options (`userAgent`/`headers`/`allowedExtensions`/`liveStartIndex`/`analyzeMs`/`program`/`httpPersistent`/`reconnect`/`variantIndex`/`maxBandwidth`/`targetLatencyMs`/`autoReconnect`/`reconnectMaxRetries`) shares ONE network connection through a process-wide demux session, instead of one connection per decoder. A single demux thread routes packets to per-decoder bounded queues; queue high-water backpressure keeps the self-pacing semantics of private connections (a paused decoder stalls the shared read exactly like it used to stall its own). Concurrent first openers race; the loser attaches to the winner (startup stays parallel, steady state is one connection). Attachers compensate a different `startTimeMs` with their own pre-target drop. `probe()` reuses a live session's context for metadata reads without opening a new connection |
| `rtspTransport` | FFmpeg default | RTSP transport for rtsp:// sources: `'tcp'` (interleaved in the control connection) or `'udp'` |
| `bufferSize` | `0` | Low-level socket buffer size in bytes (FFmpeg `buffer_size`); raise it for high-bitrate RTSP/multicast feeds |
| `rwTimeoutMs` | `0` | I/O read/write timeout in ms (FFmpeg `rw_timeout`); guards against a stalled socket on non-HLS protocols. `0` = no timeout |

For live streams `durationMs` is `0` and the `'end'` event never fires; use
`stop()` to end playback. `stop()` also aborts in-flight network reads
immediately.

### Live queue events, recovery & statistics

All three decoders expose the live health of their shared demux
subscription:

- `'overflow'` (`{ dropped }`): the queue's live catch-up valve dropped a
  backlog episode — packets dropped in this episode. Also fires while
  paused (worker-side counter polling), which is exactly when a live
  backlog overflows.
- `'discontinuity'` (`{ reason }`): stream discontinuity. `'resync'` after
  an explicit `flushToKeyframe()` / `flushBacklog()` or a queue backlog
  drop; `'reconnect'` after the session rebuilt its connection in place.
- `'reconnected'` (video/audio only, no payload): the shared session
  recovered from a 401/403/404/429 by re-resolving the source and
  reopening; stream indexes stay valid. Followed by
  `discontinuity('reconnect')`.
- `getBufferStats()` (video/audio): `{ bufferedAheadMs, readBytes,
  measuredBitrateBps, liveLatencyMs, overflowCount, reconnectCount }` —
  queue depth, consumed bytes, EWMA bitrate (5s windows) and the
  overflow/reconnect counters.
- `getStats()` (video/audio): `{ decodedFrames, droppedFrames,
  overflowCount, reconnectCount, discontinuityCount }`.

`FFMediaInfo.programs[].bandwidth` carries the HLS variant's `BANDWIDTH`
(`0` for plain MPEG-TS programs), so apps can rank/pick variants by real
bandwidth via `probe()`.

### `FFAudioDecoder`

```ts
open(src: string, streamIndex?: number, options?: FFNetOptions): Promise<FFAudioOpenInfo>
on(event: 'pcm', cb: (packet: FFAudioPacket) => void): void   // F32LE interleaved
on(event: 'end', cb: () => void): void
on(event: 'error', cb: (msg: string) => void): void
on(event: 'switched', cb: (info: FFAudioOpenInfo) => void): void
on(event: 'overflow', cb: (ev: FFOverflowEvent) => void): void
on(event: 'discontinuity', cb: (ev: FFDiscontinuityEvent) => void): void
on(event: 'reconnected', cb: () => void): void
start(paced?: boolean): void      // default true = realtime pacing
switchTrack(streamIndex: number): void  // instant hot switch, mid-playback
flushBacklog(): void              // live A/V resync: drop queue backlog
detach(): void                    // leak-path cleanup, no thread joins
setRate(rate: number): void       // varispeed [0.98, 1.02] for A/V sync
seekTo(targetMs: number): Promise<void>  // runtime VOD seek (private connections)
getBufferStats(): FFBufferStats
getStats(): FFPlaybackStats
pause(): void; resume(): void
stop(): void; close(): void
```

All audio tracks of the source (within the locked `program`, if any) are
decoded in parallel from the start, so `switchTrack` only re-routes the output:
no decoder re-open, no re-buffering, no gap. Every track is resampled to the
initial track's sample rate and channel layout, so the output format (and your
`AudioRenderer` configuration) stays constant across switches.

`FFAudioOpenInfo.channels` reports the output channel count and
`FFAudioOpenInfo.channelLayout` the output channel mask (FFmpeg `AV_CH_*`
bits, numerically identical to OHOS `audio.AudioChannelLayout` values; `0x3`
= stereo) — pass them straight to `AudioRendererOptions.streamInfo`.

### `FFVideoDecoder`

```ts
open(src: string, streamIndex?: number, options?: FFNetOptions): Promise<FFVideoOpenInfo>
on(event: 'frame', cb: (ev: FFVideoFrameEvent) => void): void
on(event: 'end', cb: () => void): void
on(event: 'error', cb: (msg: string) => void): void
on(event: 'info', cb: (ev: FFVideoInfoEvent) => void): void  // { backend: 'hw' | 'sw' }
on(event: 'overflow', cb: (ev: FFOverflowEvent) => void): void
on(event: 'discontinuity', cb: (ev: FFDiscontinuityEvent) => void): void
on(event: 'reconnected', cb: () => void): void
start(surfaceId: string): void    // renders into XComponent surface
flushToKeyframe(): void            // live A/V resync: drop backlog to newest keyframe
detach(): void                     // leak-path cleanup, no thread joins
seekTo(targetMs: number): Promise<void>   // runtime VOD seek (private connections)
captureFrame(): Promise<FFCapturedFrame>  // next decoded frame as RGBA8888 (sw decode)
getBufferStats(): FFBufferStats
getStats(): FFPlaybackStats
pause(): void; resume(): void
stop(): void; close(): void
```

#### Runtime seek & frame capture

`seekTo(targetMs)` works on private connections (`sharedDemux: false`): it
seeks to the keyframe at/before the target, flushes the decoder (the
hardware path is flushed through `OH_VideoDecoder_Flush`), re-anchors the
pacing clock and drops pre-target frames, so playback resumes precisely at
the target. It rejects on live/shared connections, while another seek is in
flight, or after a 15 s timeout. `captureFrame()` resolves the next decoded
frame as `{ width, height, data: ArrayBuffer }` (RGBA8888, native size) —
software decode only, so open with `hw: 'never'` when snapshots are needed;
it is safe to call while playback is running or paused.

#### Hardware decoding

With `hw: 'auto'` (default), H.264/HEVC streams in Annex-B framing (MPEG-TS,
HLS-TS) are decoded by the AVCodec Kit hardware decoder (`OH_VideoDecoder`,
surface mode) and rendered straight onto the XComponent surface — including
10-bit HEVC (verified with 4K 3840x2160@50 HEVC Main10 live input at realtime
frame rates). MP4-style length-prefixed streams are detected automatically and
stay on the software path. The active backend is reported once via the
`'info'` event (`{ backend: 'hw' | 'sw' }`) after `start`.

Before the first packet is fed to the hardware decoder, an AVCodec Kit
capability pre-check (`OH_AVCodec_GetCapabilityByCategory`) validates that a
hardware decoder exists for the codec and supports the stream's resolution and
profile. The check is conservative: unknown or unmappable profiles and unknown
dimensions never block decode, and the container's frame-rate metadata is
advisory only (a mismatch logs a warning). On failure, `hw: 'force'` reports
the reason through the `'error'` event, while `hw: 'auto'` silently falls back
to software.

#### HDR (HDR10 / HLG)

`FFVideoOpenInfo` reports the stream's `colorPrimaries`, `colorTransfer` and
`colorRange` (ffprobe naming, e.g. `bt2020`, `smpte2084`, `arib-std-b67`,
`tv`). On the hardware path, BT.2020 streams with SMPTE2084 (HDR10) or
ARIB-STD-B67 (HLG) transfer are further routed to the display by setting the
surface color gamut (`NATIVEBUFFER_COLOR_GAMUT_BT2100_PQ` / `_HLG`), so the
compositor tone-maps them correctly. The software path is left untouched: it
renders RGBA, which cannot reproduce HDR anyway.

### `FFSubtitleDecoder`

```ts
open(src: string, streamIndex?: number, options?: FFNetOptions): Promise<FFSubtitleOpenInfo>
on(event: 'subtitle', cb: (ev: FFSubtitleEvent) => void): void
on(event: 'pes', cb: (payload: ArrayBuffer, pts: number) => void): void  // raw PES pump
on(event: 'end', cb: () => void): void
on(event: 'error', cb: (msg: string) => void): void
on(event: 'overflow', cb: (ev: FFOverflowEvent) => void): void
on(event: 'discontinuity', cb: (ev: FFDiscontinuityEvent) => void): void
setRawPump(enable: boolean): void  // before start(): forward PES as-is
start(paced?: boolean): void      // default true = presentation-time pacing
detach(): void                    // leak-path cleanup, no thread joins
pause(): void; resume(): void
stop(): void; close(): void
```

`open` accepts a stream that is merely *declared* by the container — a DVB
subtitle entry in the PMT is enough: it succeeds and stays enabled even when
the subtitle PID never carries a single packet (`declared: true` in the open
info), so subtitles can be turned on straight from the program table.
`streamIndex < 0` picks the first declared subtitle stream (inside the locked
`program`, when `program` is set).

Each `'subtitle'` event carries one decoded display set:

| Field | Description |
|---|---|
| `clear` | `true` for an empty display set — clear what is on screen |
| `ptsMs` / `durationMs` | Presentation time and display duration |
| `canvasWidth` / `canvasHeight` | Composition size to scale against (falls back to the video size) |
| `x` / `y` / `width` / `height` | Bounding box of the visible subtitle content, in composition coordinates |
| `rgba` | RGBA8888 bitmap of that box (`width * height * 4` bytes), empty when `clear` |
| `regionCount` | Bitmap regions composited into this box |
| `text` | UTF-8 text for text-based subtitle streams (bitmap streams leave it empty) |

Bitmap regions (DVB, DVD) are composited into a single box, cropped to the
non-transparent area — a full-screen DVB page shrinks to a small text strip, so
only a few dozen kilobytes cross the bridge per display set.

```ts
const subs = new FFSubtitleDecoder();
const info = await subs.open(liveUrl, -1, { liveStartIndex: -1 }); // PMT-declared is enough
subs.on('subtitle', async (ev) => {
  if (ev.clear || ev.rgba.byteLength === 0) { overlay = undefined; return; }
  overlay = await image.createPixelMap(ev.rgba, {      // draw at (ev.x, ev.y) scaled by
    size: { width: ev.width, height: ev.height },      // boxWidth / ev.canvasWidth
    pixelFormat: image.PixelMapFormat.RGBA_8888,
    srcPixelFormat: image.PixelMapFormat.RGBA_8888
  });
});
subs.start(true);
```

`durationMs` announces each display set's on-screen lifetime: schedule a clear
for it (as the demo app does), so a subtitle does not linger on screen when a
live stream drops the terminating empty display set.

Pause/resume is pacing-aware: the presentation clock freezes while paused and
resumes where it left off, instead of bursting through the backlog.

### Custom I/O — `registerStreamIO` / `pushStreamIO` / `closeStreamIO`

Bridge arbitrary byte sources (SMB shares, custom caches, encrypted stores)
into the whole pipeline without touching C++: register a JS-backed source
and address it as `"streamio:<id>"` anywhere a URL is accepted (`probe`,
`extractFrame`, `transcode`, all three decoders).

```ts
import { registerStreamIO, pushStreamIO, closeStreamIO } from '@suplayer/ffmpeg';

registerStreamIO(1, {
  read: (seq, offset, length) => {
    mySource.read(offset, length).then((buf: ArrayBuffer) => {
      pushStreamIO(1, seq, buf.byteLength, buf); // byteCount 0 = EOF, < 0 = error
    });
  },
  seek: (seq, offset, whence) => {   // optional; omit = non-seekable
    // whence 0 = absolute, 2 = report total size, 3 = relative to end
    pushStreamIO(1, seq, whence === 2 ? mySource.size : mySource.seek(offset, whence));
  },
  close: () => mySource.close()
});

const info = await probe('streamio:1');           // metadata over the JS bridge
await transcode('streamio:1', { dst: 'out.mp4', vcodec: 'copy', acodec: 'aac' });
closeStreamIO(1);                                  // release the callbacks
```

Each read delivers up to 64 KiB and must complete within 15 s via
`pushStreamIO`, otherwise the source fails permanently (subsequent reads
return EOF immediately). Without a `seek` callback the source is
non-seekable — moov-at-end mp4 files will not open. A seekable source lets
`seekTo`, `startTimeMs` and the mp4 demuxer work exactly like on a real
file.

## Supported ABIs

- `arm64-v8a` (devices)
- `x86_64` (emulator)

`libc++_shared.so` is bundled automatically.

## Building from source

The prebuilt native libraries are produced by the scripts in
`tools/ffmpeg-build/` of the project repository (MSYS2 + OpenHarmony LLVM
toolchain):

```bash
./build-openssl.sh          # OpenSSL 3.4.1 -> prebuilt/<abi>/libssl.a, libcrypto.a
./build-openh264.sh         # OpenH264 2.5.0 -> prebuilt/<abi>/libopenh264.a
./build-davs2.sh            # davs2 (AVS2)  -> prebuilt/<abi>/libdavs2.a
./build-uavs3d.sh           # uavs3d (AVS3) -> prebuilt/<abi>/libuavs3d.a
./build-ffmpeg.sh m7        # FFmpeg 7.1.1  -> prebuilt/<abi>/libav*.a etc.
```

Copy the results into `ffmpeg/src/main/cpp/third_party/ffmpeg/libs/<abi>/`
(`tools/ffmpeg-build/sync-m5-libs.sh` does this) and rebuild the HAR. This
also documents how to relink the library against a modified FFmpeg, as
required for GPL/LGPL compliance.

## License

`libffmpeg.so` is distributed under the **GPL-3.0-or-later** — see
[LICENSE](LICENSE). The build enables `--enable-gpl --enable-version3`
because of libdavs2 (GPL-2.0-or-later, the AVS2 decoder) and OpenSSL 3.x
(Apache-2.0). Remove `--enable-gpl --enable-version3 --enable-libdavs2` in
`tools/ffmpeg-build/build-ffmpeg.sh` and rebuild to get an LGPL-2.1-or-later
library without AVS2 support. Full third-party license texts and
static-linkage/relinking details are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Third-party components

| Component | Version | License |
|---|---|---|
| FFmpeg | 7.1.1 | LGPL-2.1-or-later (built with GPL-3.0 options) |
| OpenH264 | 2.5.0 | BSD-2-Clause |
| davs2 | 1.6.0 (master snapshot) | GPL-2.0-or-later |
| uavs3d | 1.2 (master snapshot) | BSD-2-Clause |
| OpenSSL | 3.4.1 | Apache-2.0 |
| LLVM libc++ (`libc++_shared.so`) | NDK bundled | Apache-2.0 WITH LLVM-exception |
| zlib | system provided | Zlib |

License texts, exact FFmpeg build configuration and GPL relinking
instructions: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
