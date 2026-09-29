import { probe, FFNetOptions, FFMediaInfo, FFStreamInfo, FFAudioDecoder, FFVideoDecoder, FFAudioOpenInfo, FFAudioPacket, FFVideoOpenInfo, FFVideoFrameEvent, FFVideoInfoEvent } from '@suplayer/ffmpeg';
import { audio } from '@kit.AudioKit';
import { BusinessError } from '@kit.BasicServicesKit';

export type SupHwMode = 'auto' | 'force' | 'never';

export interface SupPlayerOptions {
  /** XComponent surface id the video decoder renders into */
  surfaceId: string;
  /** Video decode backend selection, forwarded to the kernel (default 'auto') */
  hw?: SupHwMode;
  /** Network/live options forwarded to both decoders and the track probe */
  net?: FFNetOptions;
}

export interface SupTrackInfo {
  index: number;
  codec: string;
  language: string;
  sampleRate: number;
  channels: number;
}

/** AV-sync player on top of the @suplayer/ffmpeg kernel: parallel video and
 * audio decoders, audio as the master clock, position tracked from packet pts,
 * pause/resume that freezes both decoders and the renderer, and periodic drift
 * correction that freezes whichever stream runs ahead. */
export class SupPlayerController {
  private videoDec: FFVideoDecoder | undefined = undefined;
  private audioDec: FFAudioDecoder | undefined = undefined;
  private renderer: audio.AudioRenderer | undefined = undefined;
  private writeChain: Promise<void> = Promise.resolve();
  private syncTimer: number = -1;
  private lastAudioPts: number = -1;
  private lastVideoPts: number = -1;
  private basePts: number = -1;
  private correcting: boolean = false;
  private frames: number = 0;

  playing: boolean = false;
  paused: boolean = false;
  positionMs: number = 0;
  durationMs: number = 0;
  /** 'hw' | 'sw' | '' — actual video backend once reported by the kernel */
  backend: string = '';
  tracks: Array<SupTrackInfo> = [];
  currentTrack: number = -1;
  errorMessage: string = '';

  open(src: string, options: SupPlayerOptions): void {
    this.stop();
    this.playing = true;
    this.paused = false;
    this.positionMs = 0;
    this.durationMs = 0;
    this.backend = '';
    this.errorMessage = '';
    this.frames = 0;
    this.basePts = -1;
    this.lastAudioPts = -1;
    this.lastVideoPts = -1;
    this.correcting = false;
    this.currentTrack = -1;
    this.tracks = [];

    // Audio track list from a background probe; failures just leave it empty
    // (playback itself does not depend on it).
    const netOpts: FFNetOptions | undefined = options.net;
    probe(src, netOpts)
      .then((info: FFMediaInfo): void => {
        const list: Array<SupTrackInfo> = [];
        for (let i = 0; i < info.streams.length; i++) {
          const s: FFStreamInfo = info.streams[i];
          if (s.type === 'audio') {
            list.push({ index: s.index, codec: s.codec, language: s.language, sampleRate: s.sampleRate, channels: s.channels });
          }
        }
        this.tracks = list;
        if (this.currentTrack < 0 && list.length > 0) {
          this.currentTrack = list[0].index;
        }
      })
      .catch((err: BusinessError): void => {
        console.info(`SUPPLAYER_PROBE_FAIL ${err.message}`);
      });

    const openOpts: FFNetOptions = this.mergeNet(netOpts, options.hw);
    const vdec = new FFVideoDecoder();
    this.videoDec = vdec;
    vdec.open(src, -1, openOpts)
      .then((info: FFVideoOpenInfo): void => {
        if (!this.playing || this.videoDec !== vdec) {
          vdec.close();
          return;
        }
        this.durationMs = info.durationMs;
        vdec.on('info', (ev: FFVideoInfoEvent): void => {
          this.backend = ev.backend;
          console.info(`SUPPLAYER_BACKEND ${ev.backend}`);
        });
        vdec.on('frame', (ev: FFVideoFrameEvent): void => {
          this.frames++;
          this.lastVideoPts = ev.ptsMs;
          if (this.lastAudioPts < 0) {
            this.trackPos(ev.ptsMs);
          }
        });
        vdec.on('end', (): void => {
          console.info(`SUPPLAYER_VIDEO_END frames=${this.frames}`);
          if (this.playing) {
            this.stop();
          }
        });
        vdec.on('error', (msg: string): void => {
          console.error(`SUPPLAYER_VIDEO_FAIL ${msg}`);
          if (this.playing) {
            this.errorMessage = `视频失败: ${msg}`;
            this.stop();
          }
        });
        vdec.start(options.surfaceId);
        this.openAudio(src, openOpts);
      })
      .catch((err: Error): void => {
        console.error(`SUPPLAYER_OPEN_FAIL ${err.message}`);
        this.errorMessage = `打开失败: ${err.message}`;
        this.stop();
      });
  }

  pause(): void {
    if (!this.playing || this.paused) {
      return;
    }
    this.paused = true;
    this.videoDec?.pause();
    this.audioDec?.pause();
    if (this.renderer !== undefined) {
      this.renderer.pause().catch((e: BusinessError): void => {
        console.error(`SUPPLAYER_RENDERER_PAUSE_FAIL ${e.message}`);
      });
    }
  }

  resume(): void {
    if (!this.playing || !this.paused) {
      return;
    }
    this.paused = false;
    if (this.renderer !== undefined) {
      this.renderer.start().catch((e: BusinessError): void => {
        console.error(`SUPPLAYER_RENDERER_RESUME_FAIL ${e.message}`);
      });
    }
    this.videoDec?.resume();
    this.audioDec?.resume();
  }

  stop(): void {
    this.stopSyncTimer();
    if (this.videoDec !== undefined) {
      this.videoDec.stop();
      this.videoDec.close();
      this.videoDec = undefined;
    }
    if (this.audioDec !== undefined) {
      this.audioDec.stop();
      this.audioDec.close();
      this.audioDec = undefined;
    }
    if (this.renderer !== undefined) {
      const r = this.renderer;
      this.renderer = undefined;
      this.writeChain
        .catch((e: BusinessError): void => {
          console.error(`SUPPLAYER_WRITE_FAIL ${e.message}`);
        })
        .then(async (): Promise<void> => {
          await this.releaseRenderer(r);
        });
    }
    this.playing = false;
    this.paused = false;
    this.correcting = false;
  }

  /** Releases all decoders and the renderer; call when the host component is destroyed. */
  release(): void {
    this.stop();
  }

  /** Instant hot switch to another audio track of the current source. */
  switchTrack(streamIndex: number): void {
    if (this.audioDec !== undefined && streamIndex !== this.currentTrack) {
      this.currentTrack = streamIndex;
      console.info(`SUPPLAYER_SWITCH track=${streamIndex}`);
      this.audioDec.switchTrack(streamIndex);
    }
  }

  private mergeNet(net: FFNetOptions | undefined, hw: SupHwMode | undefined): FFNetOptions {
    const opts: FFNetOptions = {};
    if (net !== undefined) {
      opts.userAgent = net.userAgent;
      opts.headers = net.headers;
      opts.allowedExtensions = net.allowedExtensions;
      opts.liveStartIndex = net.liveStartIndex;
      opts.analyzeMs = net.analyzeMs;
      opts.hw = net.hw;
      opts.program = net.program;
      opts.httpPersistent = net.httpPersistent;
      opts.reconnect = net.reconnect;
    }
    if (hw !== undefined) {
      opts.hw = hw;
    }
    return opts;
  }

  private openAudio(src: string, openOpts: FFNetOptions): void {
    const adec = new FFAudioDecoder();
    this.audioDec = adec;
    adec.open(src, -1, openOpts)
      .then((info: FFAudioOpenInfo): void => {
        if (!this.playing || this.audioDec !== adec) {
          adec.close();
          return;
        }
        if (this.durationMs <= 0) {
          this.durationMs = info.durationMs;
        }
        if (this.currentTrack < 0 && this.tracks.length > 0) {
          this.currentTrack = this.tracks[0].index;
        }
        console.info(`SUPPLAYER_AUDIO_OPEN ${info.codec} ${info.sampleRate}Hz ch=${info.channels}`);
        this.createRenderer(info)
          .then((r: audio.AudioRenderer): void => {
            if (!this.playing) {
              this.releaseRenderer(r);
              return;
            }
            this.renderer = r;
            adec.on('pcm', (packet: FFAudioPacket): void => {
              this.lastAudioPts = packet.ptsMs;
              this.trackPos(packet.ptsMs);
              this.writeChain = this.writeChain
                .then(async (): Promise<void> => {
                  await r.write(packet.data);
                })
                .catch((e: BusinessError): void => {
                  console.error(`SUPPLAYER_WRITE_FAIL ${e.message}`);
                });
            });
            adec.on('end', (): void => {
              // Audio EOF: fall back to the video clock as master.
              this.lastAudioPts = -1;
            });
            adec.on('error', (msg: string): void => {
              console.error(`SUPPLAYER_AUDIO_FAIL ${msg}`);
              this.lastAudioPts = -1;
            });
            adec.start(true);
            this.startSyncTimer();
          })
          .catch((err: Error): void => {
            console.error(`SUPPLAYER_RENDERER_FAIL ${err.message}`);
            this.startSyncTimer();
          });
      })
      .catch((err: Error): void => {
        // Video-only source: keep playing without an audio master clock.
        console.info(`SUPPLAYER_NO_AUDIO ${err.message}`);
        this.audioDec = undefined;
        this.startSyncTimer();
      });
  }

  private trackPos(ptsMs: number): void {
    if (this.basePts < 0) {
      this.basePts = ptsMs;
    }
    this.positionMs = Math.max(0, ptsMs - this.basePts);
  }

  private startSyncTimer(): void {
    if (this.syncTimer >= 0) {
      return;
    }
    this.syncTimer = setInterval((): void => {
      this.runSyncTick();
    }, 1000);
  }

  private stopSyncTimer(): void {
    if (this.syncTimer >= 0) {
      clearInterval(this.syncTimer);
      this.syncTimer = -1;
    }
  }

  // Coarse drift correction, ticked once per second: freeze whichever stream
  // runs ahead of the audio master clock; its native pacing anchor shifts by
  // the paused span, delaying it until the two clocks re-align.
  private runSyncTick(): void {
    if (!this.playing || this.paused || this.correcting) {
      return;
    }
    if (this.lastAudioPts < 0 || this.lastVideoPts < 0) {
      return;
    }
    const skew = this.lastVideoPts - this.lastAudioPts;
    if (Math.abs(skew) > 3000) {
      return;
    }
    if (skew > 150) {
      const span = Math.min(skew, 500);
      this.correcting = true;
      this.videoDec?.pause();
      setTimeout((): void => {
        if (this.videoDec !== undefined && this.playing && !this.paused) {
          this.videoDec.resume();
        }
        this.correcting = false;
      }, span);
    } else if (skew < -150) {
      const span = Math.min(-skew, 500);
      this.correcting = true;
      this.audioDec?.pause();
      setTimeout((): void => {
        if (this.audioDec !== undefined && this.playing && !this.paused) {
          this.audioDec.resume();
        }
        this.correcting = false;
      }, span);
    }
  }

  private audioChannelOf(n: number): audio.AudioChannel {
    switch (n) {
      case 1: return audio.AudioChannel.CHANNEL_1;
      case 2: return audio.AudioChannel.CHANNEL_2;
      case 3: return audio.AudioChannel.CHANNEL_3;
      case 4: return audio.AudioChannel.CHANNEL_4;
      case 5: return audio.AudioChannel.CHANNEL_5;
      case 6: return audio.AudioChannel.CHANNEL_6;
      case 7: return audio.AudioChannel.CHANNEL_7;
      case 8: return audio.AudioChannel.CHANNEL_8;
      default: return audio.AudioChannel.CHANNEL_2;
    }
  }

  private audioLayoutOf(mask: number): audio.AudioChannelLayout | undefined {
    switch (mask) {
      case 0x04: return audio.AudioChannelLayout.CH_LAYOUT_MONO;
      case 0x03: return audio.AudioChannelLayout.CH_LAYOUT_STEREO;
      case 0x0B: return audio.AudioChannelLayout.CH_LAYOUT_2POINT1;
      case 0x103: return audio.AudioChannelLayout.CH_LAYOUT_3POINT0;
      case 0x07: return audio.AudioChannelLayout.CH_LAYOUT_SURROUND;
      case 0x0F: return audio.AudioChannelLayout.CH_LAYOUT_3POINT1;
      case 0x107: return audio.AudioChannelLayout.CH_LAYOUT_4POINT0;
      case 0x33: return audio.AudioChannelLayout.CH_LAYOUT_QUAD;
      case 0x603: return audio.AudioChannelLayout.CH_LAYOUT_QUAD_SIDE;
      case 0x10F: return audio.AudioChannelLayout.CH_LAYOUT_4POINT1;
      case 0x607: return audio.AudioChannelLayout.CH_LAYOUT_5POINT0;
      case 0x37: return audio.AudioChannelLayout.CH_LAYOUT_5POINT0_BACK;
      case 0x60F: return audio.AudioChannelLayout.CH_LAYOUT_5POINT1;
      case 0x3F: return audio.AudioChannelLayout.CH_LAYOUT_5POINT1_BACK;
      case 0x637: return audio.AudioChannelLayout.CH_LAYOUT_7POINT0;
      case 0x6C7: return audio.AudioChannelLayout.CH_LAYOUT_7POINT0_FRONT;
      case 0x63F: return audio.AudioChannelLayout.CH_LAYOUT_7POINT1;
      default: return undefined;
    }
  }

  private rendererOptionsOf(info: FFAudioOpenInfo): audio.AudioRendererOptions {
    const streamInfo: audio.AudioStreamInfo = {
      samplingRate: info.sampleRate,
      encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW,
      sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_F32LE,
      channels: this.audioChannelOf(info.channels)
    };
    const layout: audio.AudioChannelLayout | undefined = this.audioLayoutOf(info.channelLayout);
    if (layout !== undefined) {
      streamInfo.channelLayout = layout;
    }
    const rendererInfo: audio.AudioRendererInfo = {
      usage: audio.StreamUsage.STREAM_USAGE_MUSIC,
      rendererFlags: 0
    };
    return { streamInfo: streamInfo, rendererInfo: rendererInfo };
  }

  private async createRenderer(info: FFAudioOpenInfo): Promise<audio.AudioRenderer> {
    const r = await audio.createAudioRenderer(this.rendererOptionsOf(info));
    await r.start();
    return r;
  }

  // stop() may reject when the renderer is already stopped/invalid; release()
  // must still run. Errors are logged and swallowed so callers can chain freely
  // and repeated teardown stays safe.
  private releaseRenderer(r: audio.AudioRenderer): Promise<void> {
    return r.stop()
      .catch((e: BusinessError): void => {
        console.error(`SUPPLAYER_RENDERER_STOP_FAIL ${e.message}`);
      })
      .then(async (): Promise<void> => {
        await r.release();
      })
      .catch((e: BusinessError): void => {
        console.error(`SUPPLAYER_RENDERER_RELEASE_FAIL ${e.message}`);
      });
  }
}
