# Changelog

## 1.8.0 (unreleased)

In development — entries accumulate here as features land. The interim
1.7.6–1.7.10 builds below are folded into this release.

## 1.7.6 – 1.7.10 (2026-10-04 – 2026-10-05, internal iteration builds)

Audio varispeed, raw subtitle PES pump, and live-connection watchdogs. These
versions were internal iteration builds consumed directly by the app; their
changes ship in 1.8.0.

- New `FFAudioDecoder.setRate(rate)`: audio varispeed playback. The decode
  worker rebuilds each track's `SwrContext` with the resampler output rate
  scaled to `outRate / rate` (rate clamped to [0.5, 2.0]), so the renderer
  keeps its fixed sample rate and buffer format — no `AudioRenderer`
  reconfiguration. The rate is applied lazily by the worker at packet
  boundaries (threadsafe, callable during playback); a failed reconfig keeps
  the previous rate.
- New `FFSubtitleDecoder.setRawPump(true)` + `'pes'` event: raw PES pump
  mode. Skips avcodec subtitle decoding entirely and forwards each demuxed
  subtitle packet as-is (payload, pts, stream index) for app-side DVB
  parsing. Must be set before `start()`; `open()` runs unchanged (the codec
  context is opened but unused in raw mode).
- Live read-stall watchdog (shared demux, live sources): a live connection
  that has not delivered a single packet for 30s (`LIVE_READ_STALL_MS`) is
  dead — a server that closed silently or a network black hole, where
  `av_read_frame` blocks forever because the interrupt callback only aborts
  on stop. The watchdog fires the interrupt so the blocked read fails, the
  demux thread exits, and the app's recovery machinery can rebuild the
  session on the next open.
- Live slow-starve watchdog (shared demux, live sources): a server-throttled
  connection trickles packets (the no-packet watchdog never fires) while
  playback crawls at a fraction of realtime. The demuxer now measures the
  video stream's media-time progress per 10s wall window (per-packet
  duration, immune to prestart bursts and pts jumps): progress below 15% of
  realtime sustained for 15s aborts the session (read ends, EOF to all
  subscribers) instead of hanging on a dead-but-connected stream.

## 1.7.5 (2026-10-04)

Live A/V desync recovery + zombie session cleanup.

- New `FFVideoDecoder.flushToKeyframe()` / `FFAudioDecoder.flushBacklog()`:
  live A/V resync primitives. Drops the shared-queue backlog and resumes at
  the newest queued keyframe (video) or the next routed packet (audio),
  re-anchoring a lagging track to the live edge. The still-running worker
  re-anchors its pacing on the forward pts jump, exactly like a native
  live-overflow skip. No-op on private (non-shared) connections.
- New `detach()` on all three decoders: leak-path zombie cleanup. When the
  app deliberately skips `stop()`/`close()` to avoid a main-thread join
  deadlock, the leaked decoder's queue stayed subscribed to the shared demux
  session — the demux thread kept routing packets into the orphaned queue,
  producing an endless "dropped 300 packets" high-water storm (observed for
  30+ minutes) plus a wasted network connection. `detach()` unsubscribes
  without joining anything; the still-running worker sees the detached queue
  fail fast, feeds EOS and exits on its own, and the session reference is
  released on a helper thread so a possible last-reference shutdown (interrupt
  + join demux thread + close connection) never runs on the JS thread. All
  decode workers now hold their own session reference for their lifetime,
  keeping the borrowed `AVFormatContext` valid while they drain.
- `HwVideoDecoder::RenderLoop` pacing now re-anchors on a forward pts jump
  (`waitMs >= 1000`) instead of bursting frames at full speed — same
  semantics as the software path; a burst desynced from the (re-anchored)
  audio pacing after live skips.

## 1.7.4 (2026-10-03)

Adaptive live backpressure grace + PCM headroom.

- Shared-queue live catch-up now anchors its backpressure grace window to the
  consumer's LAST POP instead of a fixed 500ms fill wait: a healthy or even
  heavily CPU-starved consumer (app-start contention, system-side stalls)
  keeps the demux pacing behind it with stable latency and no backlog drop;
  only a consumer with no pop for `LIVE_STUCK_MS` (3s, paused/hung) falls
  back to the immediate stuck drop. Eliminates startup mis-drops under CPU
  contention.
- Audio PCM threadsafe queue cap raised 32 -> 96 (~3s headroom) so
  sub-second JS-thread stalls never reach the native drop path.

## 1.7.3 (2026-10-02)

Live periodic-skip elimination.

- Demux live queue overflow switched from immediate backlog drop to bounded
  backpressure-first (grace wait before dropping); audio and software video
  pacing loops re-anchor on a forward pts jump (waitMs >= 1000) instead of
  bursting after a skip.

## 1.7.2 (2026-10-01)

Emit deadlock fix.

- All decoder `Emit` calls are now non-blocking (`napi_tsfn_nonblocking`):
  realtime payloads drop on a full queue, critical events retry briefly.
  A blocking send with a full threadsafe queue deadlocked against the JS
  thread joining the emitting worker in `stop()`/`close()` (6s appfreeze).

## 1.7.1 (2026-09-30)

Software-render crash fix (out-of-bounds write).

- Fixed a heap-corruption crash in the software video render path
  (`FFVideoDecoder::RenderFrame`): `SET_BUFFER_GEOMETRY` is negotiated
  asynchronously with the surface consumer, so the first requested buffers can
  still carry the old, smaller surface geometry — `sws_scale` then wrote a
  full-size RGBA frame (e.g. 1920x1080) into a smaller buffer, corrupting the
  heap and crashing inside jemalloc (`je_memalign`/`posix_memalign`) on the
  next `av_malloc`, typically a fraction of a second into playback.
- Every requested buffer's actual `width`/`height`/`stride` is now validated
  before writing. When the buffer covers the frame, frames are converted in
  place as before (no extra copy); when it is smaller, the frame is converted
  into an exact-size scratch buffer and copied row-by-row, clamped to the
  buffer bounds — worst case a cropped render instead of a crash.
- Tolerates devices that report `BufferHandle::stride` in pixels instead of
  bytes (re-derives the byte pitch).
- `SET_BUFFER_GEOMETRY`/`SET_FORMAT`/`SET_USAGE` return codes are now logged
  instead of silently discarded, and an `OH_NativeBuffer_Unmap` leak on the
  sws-context allocation failure path was fixed.
- New `FFNetOptions.audioTracks` (`'all'` default / `'first'`): `'first'`
  decodes only the selected audio track instead of every track in parallel —
  lower CPU and power on multi-track sources. `switchTrack` opens the target
  track lazily on demand; the switch has a brief splice instead of the
  parallel path's zero-gap re-route.
- New `FFNetOptions.frameEventMs` (default `0` = every frame): throttles the
  `FFVideoDecoder` `'frame'` event to at most one per interval, cutting the
  NAPI bridge traffic for apps that only need periodic PTS updates. The first
  frame is always emitted.
- New `FFNetOptions.startTimeMs` (default `0` = from head): VOD start position.
  The decoder seeks to the keyframe at/before the target and drops pre-target
  output (video frames are decoded but not rendered, audio frames are not
  resampled or emitted), so playback starts precisely at the requested
  position — enables resume-from-position and app-level seek via reopen.
  Non-seekable sources (live) fall back to normal startup; a drop-guard cap
  protects streams with broken timestamps.
- New `FFNetOptions.sharedDemux` (default `false`): shared demuxer. Every
  decoder (video / audio / subtitle) opened for the same source with matching
  demux options now shares ONE network connection through a process-wide
  `DemuxSession` registry, instead of one connection per decoder. A single
  demux thread routes packets to per-subscriber bounded queues (300 packets /
  64 MB high-water backpressure preserves the self-pacing semantics of
  private connections). Concurrent first openers race and the loser attaches
  to the winner, keeping startup parallelism while the steady state holds a
  single connection. Attachers requesting a different `startTimeMs` compensate
  with their own pre-target drop filter; `probe()` reuses a live session's
  context for metadata reads without a new connection. Also fixed the
  hw-eligibility peek losing the first video packet on non-Annex-B streams
  (the peeked keyframe is now kept for the software path).
- Fixed a shared-demux main-thread freeze (appfreeze `THREAD_BLOCK_3S`):
  - The hw-eligibility peek runs synchronously on the JS main thread
    (`start()`); with a shared queue whose demux thread was still fetching the
    first segment or backpressured by another subscriber, the peek could
    block its full 3s deadline and trip the watchdog. The shared-path peek
    deadline is now 500ms (falls back to software decode instead).
  - `PacketQueue::Pop` polled its abort flag only when woken, and setting the
    decoder stop flag never woke the queue — `stop()`/`close()` on the main
    thread could hang forever joining a worker blocked on an empty, stalled
    queue. Pop now re-checks abort on a 100ms `wait_for` interval, and
    `StopInternal` calls the new `PacketQueue::Wake()`.
  - Startup grace: a subscriber that has subscribed but not started consuming
    yet (the app's video/audio open rendezvous gates the audio start) no
    longer backpressures the shared read at the normal 300-packet watermark —
    it buffers up to 4000 packets until its first pop, so one not-yet-started
    decoder cannot starve the others' first frames.
- Fixed slow-consumer audio starvation on live streams (shared demux): a
  persistently slow video subscriber (e.g. software-decoded 1080p50 on a
  forced-software live setting) filled its 300-packet queue and blocked the
  demux thread in `Route()` indefinitely — audio subscribers starved
  (silence, PCM queue-full drops) and the HLS reader fell off the live edge
  (whole-segment skips, PTS jumps of minutes). New `FFNetOptions.live`
  (default `false`): the video subscriber queue switches to a catch-up
  overflow policy — on a full queue the backlog is dropped and routing
  resumes at the next keyframe (clean IDR restart), so the demux thread never
  blocks on a slow live consumer and audio keeps flowing in real time.
- Extended the live catch-up overflow policy to audio queues: an audio
  subscriber whose consumer never drains (e.g. the app-side AudioRenderer
  `start()` IPC hangs while the app is backgrounded/locked) filled its queue
  and blocked `Route()` forever — the whole shared session deadlocked (video
  frozen too, stall watchdog recovery looped into a zombie state). Audio
  queues now flush their backlog on overflow and resume at the next packet
  (audio frames are independently decodable, no keyframe wait); the demux
  thread can no longer deadlock on any live subscriber. VOD playback and
  subtitle queues keep the blocking backpressure semantics.

## 1.7.0 (2026-09-29)

Chinese broadcast codec support (AVS2/AVS3) and true pause/resume. **License
change:** the prebuilt `libffmpeg.so` is now **GPL-3.0-or-later** — the build
enables `--enable-gpl --enable-version3` for libdavs2 (AVS2, GPL-2.0-or-later)
and OpenSSL 3.x (Apache-2.0). 1.6.x remains the last LGPL-2.1-or-later build.

- AVS2 (AVS2-P2/IEEE.1857.4, GB/T 33475.2) software decoding via the
  cross-compiled davs2 library — CCTV 4K/UHD broadcast TS streams (stream
  type 0xd2) now decode end to end (`codec: avs2`).
- AVS3 (AVS3-P2/IEEE.1857.10) software decoding via the cross-compiled
  uavs3d library (BSD-2-Clause), with NEON assembly on arm64 — stream
  type 0xd4 (`codec: avs3`).
- AVS1 (CAVS) continues to decode in its Jizhun baseline profile only; the
  AVS+ broadcast profile (AVS-P16) is not supported by upstream FFmpeg/davs2.
  AVS streams always use the software path (no hardware MIME exists).
- Native decoders shift their pacing anchors on `pause()`: `resume()`
  continues from the paused position instead of bursting through the backlog
  (video software renderer, hardware path and audio decoder).
- New build scripts `tools/ffmpeg-build/build-davs2.sh` and
  `build-uavs3d.sh`; FFmpeg rebuilt with
  `--enable-gpl --enable-version3 --enable-libdavs2 --enable-libuavs3d`.

## 1.6.1 (2026-09-29)

Package metadata fix (no code changes).

- Corrected the `repository`/`homepage`/`bugs`/`author.url` URLs in
  `oh-package.json5`: the previous GitHub address was unreachable (wrong
  owner), all links now point to the accessible Gitee mirror
  `https://gitee.com/wchuanxin/suplayer`.

## 1.6.0 (2026-09-26)

Hardware capability pre-check, HDR display adaptation, and live-networking
hardening (validated against a 4K HDR10 HLS live source: 3840x2160@50 HEVC
Main10, dual AAC, DVB subtitles, disguised `.php`/`.jpg` URLs).

- Hardware decode capability pre-check: before the decoder is created the
  chip's ability is queried via `OH_AVCodec_GetCapabilityByCategory`
  (HARDWARE) — no hardware decoder for the mime type, or a size/profile
  outside the reported range (e.g. HEVC Main10 on a Main-only decoder),
  skips the whole hardware trial and starts straight on software. With
  `hw: 'force'` the pre-check failure surfaces as a `start` error with the
  reason. Stream profiles are mapped from FFmpeg `AV_PROFILE_*` (H.264
  Baseline/Main/High, HEVC Main/Main10/MainStill); unmapped/unknown profiles
  never block. Frame-rate metadata is advisory (logged, not blocking). The
  matched hardware codec name is logged (`FFVideoHW`).
- HDR10/HLG display: on the hardware path, BT.2020 content with an HDR
  transfer (SMPTE 2084 / ARIB std B67) sets the surface color gamut to the
  matching BT.2100 variant (`SET_COLOR_GAMUT`) right after start, so the
  system compositor tone-maps 10-bit NV12 frames correctly. New
  `FFVideoOpenInfo.colorPrimaries` / `colorTransfer` / `colorRange` fields
  (ffprobe naming) let apps detect HDR content.
- New `FFNetOptions.httpPersistent` (default `true`): HLS playlist reloads
  and segment downloads reuse one HTTP connection instead of paying a fresh
  TCP+TLS handshake per segment.
- New `FFNetOptions.reconnect` (default `true`): transient network failures
  — connection errors, live-stream disconnects, HTTP 4xx/5xx segment/
  playlist responses — are auto-retried with a capped backoff (max 5s)
  before the error surfaces. Both options are no-ops for non-HTTP sources.
- `FFSubtitleDecoder` pause is now pacing-aware: the presentation anchor is
  shifted by the paused span on resume, so subtitles no longer burst out to
  catch up with the wall clock.
- Palette-less PAL8 subtitle regions (rare, malformed DVB sets) now fall
  back to fully transparent instead of painting a solid white block.

## 1.5.0 (2026-09-25)

Multi-channel audio output: the decoder no longer downmixes to stereo.

- `FFAudioDecoder` output now follows the source's channel layout (mono
  through 7.1, native order). Unspecified layouts use the default layout of
  that channel count; custom/ambisonic layouts and anything above 8 channels
  are downmixed to stereo. Output stays F32LE interleaved.
- New `FFAudioOpenInfo.channelLayout` (and in the `switched` event): the
  output channel mask in FFmpeg `AV_CH_*` bits, numerically identical to the
  OHOS `audio.AudioChannelLayout` values, so it can be mapped straight to
  `AudioRendererOptions.streamInfo.channelLayout`.
- Hot track switching keeps its zero-re-buffering behavior: every track is
  resampled to the initial track's sample rate **and** channel layout, so the
  output format (and the app-side `AudioRenderer`) stays constant across
  switches. `channels`/`channelLayout` in `switched` report the locked format.
- `channels` in open/switched info now reports the real output channel count
  (was hardcoded `2` in the `switched` event).

## 1.4.0 (2026-09-24)

DVB subtitle decoding, enabled from the container's declaration alone.

- New `FFSubtitleDecoder`: opens a subtitle stream and emits decoded display
  sets as events. Bitmap subtitles (DVB/DVD) are composited into a single
  RGBA8888 bitmap — the union bounding box of all regions, cropped to the
  visible (non-transparent) area, which shrinks typical DVB pages from
  full-screen to a small text strip. Text subtitles are passed through as
  UTF-8 text, and empty display sets are reported as `clear`.
- A stream is accepted as soon as the container *declares* it — a DVB subtitle
  entry in the PMT is enough: `open` succeeds and the decoder stays enabled even
  when the subtitle PID never carries a single packet, so the feature can be
  turned on straight from the program table. `FFSubtitleOpenInfo.declared`
  reports this, and the composition size falls back to the paired video size
  until a display definition segment arrives.
- Same options as the other decoders (`FFNetOptions`, including `program`),
  realtime pacing (`start(paced)`), `pause`/`resume`/`stop`/`close`.
- Verified on-device with a live DVB stream (1920x1080 @50, `dvb_subtitle`,
  language `mul`, composition 1280x720): a display set decoded to a
  377x73 box at (452,544), delivered as an 110 KB RGBA bitmap, followed by a
  `clear`; and with a synthetic TS whose PMT declares a subtitle PID that never
  carries data (`declared=true`, no events, no error).
- `probe` also surfaces FFmpeg's EIT-backed `epg` data stream when a stream
  carries an event information table.

## 1.3.0 (2026-09-24)

MPEG-TS program information (PAT/PMT) and multi-program (MPTS) playback.

- `probe` now exposes the PSI program table: new `FFProgramInfo` entries in
  `FFMediaInfo.programs` with `programNum`, `pmtPid`, `pcrPid`, `pmtVersion`
  (PMT version_number), `flags` (bit 0 = `AV_PROGRAM_RUNNING`), SDT
  `serviceName`/`serviceProvider` and the `streamIndexes` belonging to each
  program. `pmtPid`/`pcrPid`/`pmtVersion` are `-1` when the input carries no
  PSI; empty for containers without a program table (MP4).
- `FFAudioDecoder` now decodes all audio tracks of the source (within the
  locked `program`, if any) in parallel and `switchTrack` is an instant hot
  switch: it just re-routes the output, with no decoder re-open and no
  re-buffering. Every track resamples to the initial track's sample rate, so
  the output rate is constant across switches. Streams outside a locked
  program are still rejected.
- Each `FFStreamInfo` now carries `pid` (MPEG-TS transport PID, `-1` for
  containers whose stream ids are not PIDs) and `programIndex` (index into
  `programs`, `-1` when not in any program).
- New `program` option in `FFNetOptions` (accepted by both decoders' `open`):
  locks demuxing to a single MPEG-TS program by its program number. Streams of
  other programs are discarded at the demuxer level, automatic stream
  selection stays inside the program, explicit `streamIndex` values are
  validated against it, and `FFAudioDecoder.switchTrack` rejects streams
  outside the locked program. Verified on-device with a two-program TS
  (480x270/44.1kHz vs 640x360/22.05kHz programs) played back independently,
  hardware decode active for both.

## 1.2.0 (2026-09-24)

Hardware video decoding (surface mode) via AVCodec Kit.

- New `hw` option in `FFNetOptions` (accepted by `FFVideoDecoder.open`):
  `'auto'` (default) prefers the hardware decoder and silently falls back to
  software on failure, `'force'` requires hardware (start fails otherwise),
  `'never'` always uses software.
- Hardware path decodes H.264/HEVC Annex-B streams (MPEG-TS, HLS-TS) with
  `OH_VideoDecoder` in surface mode, rendering directly onto the XComponent
  surface with wall-clock pacing. Verified on-device with 4K HEVC Main10
  3840x2160@50 (NV12 10-bit output, HDR BT.2020 metadata) and 1080p50 H.264
  live streams at realtime frame rates.
- MP4-style length-prefixed streams are detected by peeking the first video
  packet (Annex-B start code check) and stay on the software path; the peeked
  packet is preserved so neither path loses the first keyframe.
- New `FFVideoDecoder.on('info', ...)` event reporting the active backend
  (`{ backend: 'hw' | 'sw' }`) once the decoder starts.

## 1.1.0 (2026-09-24)

Network and live-streaming support.

- HTTPS/TLS via bundled OpenSSL 3.4.1 (Apache-2.0); `src` now accepts URLs
  (`https://`, `http://`, HLS `.m3u8`, plain TS over HTTP) everywhere a file
  path is accepted.
- HLS live playback: `liveStartIndex` (join at the live edge), realtime
  paced video/audio, seamless `switchTrack` across live audio tracks,
  immediate `stop()` via I/O interrupt callback (no waiting on network reads).
- New `FFNetOptions` on `probe` and both decoders' `open`: `userAgent`,
  `headers`, `allowedExtensions`, `liveStartIndex`, `analyzeMs`.
  `allowedExtensions` also disables strict extension/format matching for
  streams with disguised playlist/segment URLs (`.php` playlists,
  `.jpg`-named TS segments).
- FFmpeg diagnostics forwarded to `hilog` (domain `0xFF00`, tag `FFmpeg`).

## 1.0.0 (2026-09-23)

Initial release.

- `probe`: container/codec/stream metadata probing for all FFmpeg-supported
  formats, including raw MPEG-TS with multi-audio/subtitle tracks and
  language tags.
- `extractFrame`: single-frame extraction to RGBA at a given timestamp.
- `FFAudioDecoder`: software audio decoding to F32LE stereo with realtime
  pacing, seamless `switchTrack` on multi-track inputs, pause/resume.
- `FFVideoDecoder`: software video decoding with OH_NativeWindow/XComponent
  Surface rendering.
- `transcode`: one-shot background transcode/mux pipeline
  (decode -> avfilter setpts/scale/fps -> OpenH264/AAC -> MP4/MPEG-TS).
- Native libraries for `arm64-v8a` and `x86_64`; bundled `libc++_shared.so`.
- Based on FFmpeg 7.1.1 (LGPL-2.1-or-later build, no GPL components) and
  OpenH264 2.5.0.
