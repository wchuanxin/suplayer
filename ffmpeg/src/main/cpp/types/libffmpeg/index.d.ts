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

export const probe: (src: string, options?: FFNetOptions) => Promise<FFMediaInfo>;

export const extractFrame: (src: string, timeMs: number) => Promise<FFFrame>;

export const transcode: (src: string, options: FFTranscodeOptions) => Promise<FFTranscodeResult>;

export class FFAudioDecoder {
  open(src: string, streamIndex?: number, options?: FFNetOptions): Promise<FFAudioOpenInfo>;
  on(event: 'pcm', callback: (packet: FFAudioPacket) => void): void;
  on(event: 'end', callback: () => void): void;
  on(event: 'error', callback: (msg: string) => void): void;
  on(event: 'switched', callback: (info: FFAudioOpenInfo) => void): void;
  start(paced?: boolean): void;
  /** Instant hot switch: all audio tracks of the source (within the locked
   *  program) are decoded in parallel, so switching just re-routes the output —
   *  no decoder re-open, no re-buffering. Output sample rate and channel
   *  layout stay locked to the initial track's. */
  switchTrack(streamIndex: number): void;
  pause(): void;
  resume(): void;
  stop(): void;
  close(): void;
}

export class FFVideoDecoder {
  open(src: string, streamIndex?: number, options?: FFNetOptions): Promise<FFVideoOpenInfo>;
  on(event: 'frame', callback: (info: FFVideoFrameEvent) => void): void;
  on(event: 'info', callback: (info: FFVideoInfoEvent) => void): void;
  on(event: 'end', callback: () => void): void;
  on(event: 'error', callback: (msg: string) => void): void;
  start(surfaceId: string): void;
  pause(): void;
  resume(): void;
  stop(): void;
  close(): void;
}

export class FFSubtitleDecoder {
  /** Opens a subtitle stream. Streams merely declared by the container (DVB
   *  subtitle entries in the PMT, even with a silent PID) are accepted, so the
   *  feature can be enabled from the program table alone.
   *  streamIndex < 0 selects the first subtitle stream. */
  open(src: string, streamIndex?: number, options?: FFNetOptions): Promise<FFSubtitleOpenInfo>;
  on(event: 'subtitle', callback: (ev: FFSubtitleEvent) => void): void;
  on(event: 'end', callback: () => void): void;
  on(event: 'error', callback: (msg: string) => void): void;
  /** paced = true (default) displays each display set at its presentation time */
  start(paced?: boolean): void;
  pause(): void;
  resume(): void;
  stop(): void;
  close(): void;
}
