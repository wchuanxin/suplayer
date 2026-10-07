import ffmpeg from 'libffmpeg.so';
import { FFAudioDecoder, FFVideoDecoder, FFSubtitleDecoder } from 'libffmpeg.so';

export interface FFStreamInfo {
  index: number;
  type: string;
  codec: string;
  codecLongName: string;
  width: number;
  height: number;
  sampleRate: number;
  channels: number;
  language: string;
  title: string;
  duration: number;
  bitRate: number;
  /** MPEG-TS packet id (PID); -1 for containers without PSI */
  pid: number;
  /** Index into FFMediaInfo.programs, -1 when the stream is not part of any program */
  programIndex: number;
}

export interface FFProgramInfo {
  index: number;
  /** DVB service number from the PAT */
  programNum: number;
  /** PMT packet id, -1 when unknown */
  pmtPid: number;
  /** PCR packet id, -1 when unknown */
  pcrPid: number;
  /** PMT version_number field (0-31), -1 when unknown */
  pmtVersion: number;
  /** Program flags; bit 0 (1) = AV_PROGRAM_RUNNING */
  flags: number;
  /** SDT service name, empty when absent */
  serviceName: string;
  /** SDT service provider, empty when absent */
  serviceProvider: string;
  /** Stream indexes (into FFMediaInfo.streams) that belong to this program */
  streamIndexes: Array<number>;
}

export interface FFMediaInfo {
  format: string;
  formatLongName: string;
  title: string;
  duration: number;
  bitRate: number;
  streams: Array<FFStreamInfo>;
  /** MPEG-TS programs (PAT/PMT view); empty for containers without PSI */
  programs: Array<FFProgramInfo>;
}

export interface FFFrame {
  width: number;
  height: number;
  data: ArrayBuffer;
}

export interface FFNetOptions {
  /** HTTP User-Agent (applied to playlist and all segment downloads, HLS included) */
  userAgent?: string;
  /** Extra HTTP headers, CRLF-separated, e.g. "Referer: https://x\r\nCookie: y=z" */
  headers?: string;
  /** HLS segment extension whitelist, comma-separated; pass 'ALL' to disable.
   *  Setting this also disables strict extension/format matching. */
  allowedExtensions?: string;
  /** HLS live: segment index to start at, negative counts back from live edge (default -3) */
  liveStartIndex?: number;
  /** Cap stream-info analysis duration in ms; lower = faster live startup */
  analyzeMs?: number;
  /** Video decode backend: 'auto' (default) tries hardware decode first for h264/hevc
   *  Annex-B streams and falls back to software on failure; 'force' fails on hardware
   *  errors; 'never' always decodes in software. */
  hw?: 'auto' | 'force' | 'never';
  /** MPEG-TS program number to lock playback to (multi-program TS). When set,
   *  only streams of that program are demuxed; automatic stream selection stays
   *  inside the program and explicit stream indexes are validated against it. */
  program?: number;
  /** Reuse one HTTP connection across HLS playlist reloads and segment
   *  downloads instead of a fresh TCP+TLS handshake per segment (default true). */
  httpPersistent?: boolean;
  /** Auto-retry transient network failures: connection errors, live-stream
   *  disconnects and HTTP 4xx/5xx segment responses are retried with a capped
   *  backoff before surfacing an error (default true). */
  reconnect?: boolean;
  /** Audio track decode mode (FFAudioDecoder): 'all' (default) decodes every
   *  audio track in parallel so switchTrack is a zero-gap re-route; 'first'
   *  decodes only the selected track and opens the target track lazily on
   *  switchTrack — lower CPU/power, with a brief splice on switch. */
  audioTracks?: 'all' | 'first';
  /** Minimum interval in ms between FFVideoDecoder 'frame' events
   *  (0 = every frame, default). Throttles the NAPI bridge traffic for apps
   *  that only need periodic PTS updates (A/V sync, progress). The first
   *  frame is always emitted. */
  frameEventMs?: number;
  /** VOD start position in ms: the decoder seeks to the keyframe at/before
   *  the target and drops pre-target frames so playback starts precisely at
   *  the requested position (resume/seek support). 0 (default) = from head.
   *  Ignored when the source is not seekable (live streams). */
  startTimeMs?: number;
  /** Shared demuxer (B1): when true, every decoder (video / audio / subtitle)
   *  opened for the same source + matching demux options shares ONE network
   *  connection through a process-wide demux session, instead of one
   *  connection per decoder. false (default) keeps per-decoder private
   *  connections. Attachers start at the session's current demux position and
   *  compensate with their own startTimeMs pre-target drop. */
  sharedDemux?: boolean;
  /** Source is a live stream. Live video subscribers use a catch-up overflow
   *  policy: when their queue is full the backlog is dropped and routing
   *  resumes at the next keyframe, instead of blocking the shared demux
   *  thread (which would starve the audio subscribers). Default false. */
  live?: boolean;
}

export interface FFAudioOpenInfo {
  durationMs: number;
  sampleRate: number;
  channels: number;
  /** Output channel mask (FFmpeg AV_CH_* bits, identical to OHOS AudioChannelLayout
   *  values; 0x3 = stereo). Locked to the initial track across hot switches. */
  channelLayout: number;
  codec: string;
}

export interface FFAudioPacket {
  data: ArrayBuffer;
  ptsMs: number;
}

export interface FFVideoOpenInfo {
  width: number;
  height: number;
  fps: number;
  durationMs: number;
  codec: string;
  /** Color primaries of the video, ffprobe naming (e.g. 'bt709', 'bt2020'); empty when unspecified */
  colorPrimaries?: string;
  /** Transfer characteristics, ffprobe naming (e.g. 'bt709', 'smpte2084' = HDR10 PQ,
   *  'arib-std-b67' = HLG); empty when unspecified */
  colorTransfer?: string;
  /** Color range: 'tv' (limited), 'pc' (full); empty when unspecified */
  colorRange?: string;
}

export interface FFVideoFrameEvent {
  ptsMs: number;
}

export interface FFVideoInfoEvent {
  /** Actual decode backend in use: 'hw' (OH_AVCodec hardware) or 'sw' (FFmpeg software) */
  backend: 'hw' | 'sw';
}

export interface FFSubtitleOpenInfo {
  codec: string;
  language: string;
  /** Subtitle composition size (0 when the stream has not declared one yet) */
  width: number;
  height: number;
  /** Selected subtitle stream index in the container */
  streamIndex: number;
  durationMs: number;
  /** True when the stream is declared by the container (e.g. a DVB subtitle
   *  entry in the PMT) — decoding is enabled even if the PID carries no data. */
  declared: boolean;
}

export interface FFSubtitleEvent {
  /** True when the subtitle should be cleared (empty display set) */
  clear: boolean;
  ptsMs: number;
  durationMs: number;
  /** Subtitle composition size for scaling; falls back to the video size */
  canvasWidth: number;
  canvasHeight: number;
  /** Union bounding box of this display set, in composition coordinates */
  x: number;
  y: number;
  width: number;
  height: number;
  /** Number of bitmap regions composited into this event */
  regionCount: number;
  /** RGBA8888 bitmap of the bounding box (width * height * 4 bytes, empty when clear) */
  rgba: ArrayBuffer;
  /** Plain UTF-8 text for text-based subtitle streams (empty for bitmap ones) */
  text: string;
}

export interface FFTranscodeOptions {
  dst: string;
  width?: number;
  height?: number;
  fps?: number;
  videoBitrate?: number;
  audioBitrate?: number;
  startTimeMs?: number;
  durationMs?: number;
}

export interface FFTranscodeResult {
  outputSize: number;
  durationMs: number;
  frames: number;
}

/** Shared-queue overflow episode: the live catch-up valve dropped queued
 *  packets to keep the session near the live edge. */
export interface FFOverflowEvent {
  /** Packets dropped in this episode */
  dropped: number;
}

/** Stream discontinuity raised by an explicit backlog resync (flushToKeyframe
 *  / flushBacklog); more reasons (variant switch, reconnect) may follow. */
export interface FFDiscontinuityEvent {
  reason: string;
}

/** Network/queue health of a decoder fed by the shared demux session.
 *  Values are 0 on a private (non-shared) connection where not applicable. */
export interface FFBufferStats {
  /** Media duration of the stats-stream packets currently queued (ms) */
  bufferedAheadMs: number;
  /** Total demuxed bytes read by this decoder (packets it consumed) */
  readBytes: number;
  /** EWMA bitrate over 5s windows (bits per second) */
  measuredBitrateBps: number;
  /** Live edge latency (ms); -1 until playlist-level timing is wired */
  liveLatencyMs: number;
  /** Number of overflow episodes (see FFOverflowEvent) */
  overflowCount: number;
  /** Automatic reconnects (token self-healing); 0 until wired */
  reconnectCount: number;
}

/** Decode-side playback statistics of a decoder. */
export interface FFPlaybackStats {
  /** Frames/packets decoded and emitted to JS */
  decodedFrames: number;
  /** Frames decoded but dropped (pre-target seek skip, render failures) */
  droppedFrames: number;
  overflowCount: number;
  reconnectCount: number;
  /** Stream discontinuities observed (resyncs so far) */
  discontinuityCount: number;
}

export function probe(src: string, options?: FFNetOptions): Promise<FFMediaInfo> {
  return ffmpeg.probe(src, options);
}

export function extractFrame(src: string, timeMs: number): Promise<FFFrame> {
  return ffmpeg.extractFrame(src, timeMs);
}

export function transcode(src: string, options: FFTranscodeOptions): Promise<FFTranscodeResult> {
  return ffmpeg.transcode(src, options);
}

export { FFAudioDecoder, FFVideoDecoder, FFSubtitleDecoder };
