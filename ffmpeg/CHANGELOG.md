# Changelog

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
