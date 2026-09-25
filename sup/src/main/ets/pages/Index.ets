import { probe, extractFrame, transcode, FFAudioDecoder, FFVideoDecoder, FFSubtitleDecoder, FFMediaInfo, FFStreamInfo, FFProgramInfo, FFFrame, FFAudioOpenInfo, FFAudioPacket, FFVideoOpenInfo, FFVideoFrameEvent, FFVideoInfoEvent, FFSubtitleOpenInfo, FFSubtitleEvent, FFTranscodeResult, FFNetOptions } from '@suplayer/ffmpeg';
import { common } from '@kit.AbilityKit';
import { image } from '@kit.ImageKit';
import { audio } from '@kit.AudioKit';
import { BusinessError } from '@kit.BasicServicesKit';

@Entry
@Component
struct Index {
  @State srcPath: string = '';
  @State probing: boolean = false;
  @State errMsg: string = '';
  @State info: FFMediaInfo | undefined = undefined;
  @State framePm: image.PixelMap | undefined = undefined;
  @State extracting: boolean = false;
  @State frameMsg: string = '';
  @State decoding: boolean = false;
  @State audioMsg: string = '';
  @State videoMsg: string = '';
  @State playing: boolean = false;
  @State trackMsg: string = '';
  @State currentTrack: number = -1;
  @State transcoding: boolean = false;
  @State transcodeMsg: string = '';
  @State mptsPlaying: boolean = false;
  @State mptsProgram: number = -1;
  @State mptsMsg: string = '';
  @State subOn: boolean = false;
  @State subMsg: string = '';
  @State subEvents: number = 0;
  @State subOverlay: image.PixelMap | undefined = undefined;
  @State subBox: number[] = [0, 0, 0, 0];
  @State subCanvas: number[] = [0, 0];
  @State subText: string = '';
  @State videoBoxW: number = 0;
  @State videoBoxH: number = 0;

  private probedPath: string = '';
  private renderer: audio.AudioRenderer | undefined = undefined;
  private audioDec: FFAudioDecoder | undefined = undefined;
  private writeChain: Promise<void> = Promise.resolve();
  private xcc: XComponentController = new XComponentController();
  private mptsVideoDec: FFVideoDecoder | undefined = undefined;
  private mptsAudioDec: FFAudioDecoder | undefined = undefined;
  private mptsRenderer: audio.AudioRenderer | undefined = undefined;
  private mptsWriteChain: Promise<void> = Promise.resolve();
  private mptsFrames: number = 0;
  private mptsAudioPackets: number = 0;
  private programNums: number[] = [];
  private subDec: FFSubtitleDecoder | undefined = undefined;
  private videoW: number = 0;
  private videoH: number = 0;

  private programLabel(s: FFStreamInfo): string {
    if (s.programIndex < 0 || s.programIndex >= this.programNums.length) {
      return '';
    }
    return `    节目: ${this.programNums[s.programIndex]}`;
  }

  private programNumsOf(result: FFMediaInfo): number[] {
    const nums: number[] = [];
    for (let i = 0; i < result.programs.length; i++) {
      nums.push(result.programs[i].programNum);
    }
    return nums;
  }

  // Generic options for user-provided URLs: permissive extension matching and a
  // capped analysis window (needed by streams whose segment URLs are disguised).
  private netOptionsFor(src: string): FFNetOptions {
    if (src.startsWith('http://') || src.startsWith('https://')) {
      return { analyzeMs: 5000, allowedExtensions: 'ALL' };
    }
    return {};
  }

  // Maps an output channel count to the AudioRenderer enum; unsupported counts
  // (0, >8) fall back to stereo.
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

  // Maps the decoder's FFmpeg-native channel mask (bit-identical to OHOS
  // AudioChannelLayout values) to the SDK enum; undefined when unknown.
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

  // Renderer setup follows the decoder's locked output format (rate, channel
  // count and, when recognized, the channel layout).
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

  // stop() may reject when the renderer is already stopped/invalid; release()
  // must still run. Errors are logged and swallowed so callers can chain freely
  // and repeated teardown stays safe.
  private releaseRenderer(r: audio.AudioRenderer): Promise<void> {
    return r.stop()
      .catch((e: BusinessError): void => {
        console.error(`FFRENDERER_STOP_FAIL ${e.message}`);
      })
      .then(async (): Promise<void> => {
        await r.release();
      })
      .catch((e: BusinessError): void => {
        console.error(`FFRENDERER_RELEASE_FAIL ${e.message}`);
      });
  }

  private doProbe(): void {
    if (this.srcPath.length === 0 || this.probing) {
      return;
    }
    this.probing = true;
    this.errMsg = '';
    this.info = undefined;
    probe(this.srcPath, this.netOptionsFor(this.srcPath))
      .then((result: FFMediaInfo) => {
        this.info = result;
        this.probedPath = this.srcPath;
        this.programNums = this.programNumsOf(result);
        this.probing = false;
      })
      .catch((err: Error) => {
        this.errMsg = err.message;
        this.probing = false;
      });
  }

  private doExtract(): void {
    if (this.srcPath.length === 0 || this.extracting) {
      return;
    }
    this.extracting = true;
    this.frameMsg = '抽帧中...';
    extractFrame(this.srcPath, 5000)
      .then(async (f: FFFrame): Promise<void> => {
        const opts: image.InitializationOptions = {
          size: { width: f.width, height: f.height },
          pixelFormat: image.PixelMapFormat.RGBA_8888,
          srcPixelFormat: image.PixelMapFormat.RGBA_8888
        };
        const pm: image.PixelMap = await image.createPixelMap(f.data, opts);
        this.framePm = pm;
        this.extracting = false;
        this.frameMsg = `${f.width}x${f.height}`;
        console.info(`FFEXTRACT_OK w=${f.width} h=${f.height} bytes=${f.data.byteLength}`);
      })
      .catch((err: Error) => {
        this.extracting = false;
        this.frameMsg = `失败: ${err.message}`;
        console.error(`FFEXTRACT_FAIL ${err.message}`);
      });
  }

  private async playPcm(chunks: Array<ArrayBuffer>, info: FFAudioOpenInfo): Promise<void> {
    const renderer: audio.AudioRenderer = await audio.createAudioRenderer(this.rendererOptionsOf(info));
    this.renderer = renderer;
    try {
      await renderer.start();
      for (let i = 0; i < chunks.length; i++) {
        await renderer.write(chunks[i]);
      }
    } finally {
      this.renderer = undefined;
      await this.releaseRenderer(renderer);
    }
  }

  private doDecodeAudio(): void {
    if (this.srcPath.length === 0 || this.decoding) {
      return;
    }
    this.decoding = true;
    this.audioMsg = '解码中...';

    const dec = new FFAudioDecoder();
    const chunks: Array<ArrayBuffer> = [];
    let totalBytes = 0;
    let packets = 0;
    let openInfo: FFAudioOpenInfo | undefined = undefined;

    dec.open(this.srcPath, -1, this.netOptionsFor(this.srcPath))
      .then((info: FFAudioOpenInfo): void => {
        openInfo = info;
        this.audioMsg = `codec=${info.codec} rate=${info.sampleRate} ch=${info.channels}`;
        dec.on('pcm', (packet: FFAudioPacket): void => {
          chunks.push(packet.data);
          totalBytes += packet.data.byteLength;
          packets++;
        });
        dec.on('end', (): void => {
          console.info(`FFAUDIO_OK packets=${packets} bytes=${totalBytes}`);
          this.audioMsg = `解码完成: ${packets} 包, ${totalBytes} 字节, 开始播放`;
          if (openInfo !== undefined) {
            this.playPcm(chunks, openInfo)
              .then((): void => {
                this.decoding = false;
                this.audioMsg = '播放完成';
                console.info('FFAUDIO_PLAY_DONE');
              })
              .catch((err: Error): void => {
                this.decoding = false;
                this.audioMsg = `播放失败: ${err.message}`;
                console.error(`FFAUDIO_PLAY_FAIL ${err.message}`);
              });
          }
        });
        dec.on('error', (msg: string): void => {
          this.decoding = false;
          this.audioMsg = `解码失败: ${msg}`;
          console.error(`FFAUDIO_FAIL ${msg}`);
        });
        dec.start();
      })
      .catch((err: Error): void => {
        this.decoding = false;
        this.audioMsg = `打开失败: ${err.message}`;
        console.error(`FFAUDIO_OPEN_FAIL ${err.message}`);
      });
  }

  private doTranscode(): void {
    if (this.srcPath.length === 0) {
      return;
    }
    this.transcoding = true;
    this.transcodeMsg = '';
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const dst = `${ctx.filesDir}/out.mp4`;
    const t0 = Date.now();
    transcode(this.srcPath, { dst: dst, width: 160, height: 90, videoBitrate: 400000, audioBitrate: 64000 })
      .then((result: FFTranscodeResult): Promise<FFMediaInfo> => {
        const costMs = Date.now() - t0;
        console.info(`FFTRANSCODE_OK size=${result.outputSize} dur=${result.durationMs.toFixed(0)}ms frames=${result.frames} cost=${costMs}ms`);
        this.transcodeMsg = `完成: ${Math.round(result.outputSize / 1024)}KB, ${result.frames} 帧, ${result.durationMs.toFixed(1)}s, 耗时 ${costMs}ms`;
        return probe(dst);
      })
      .then((outInfo: FFMediaInfo): void => {
        const parts: string[] = [];
        for (let i = 0; i < outInfo.streams.length; i++) {
          const s: FFStreamInfo = outInfo.streams[i];
          parts.push(`#${s.index} ${s.type}:${s.codec}${s.type === 'video' ? ` ${s.width}x${s.height}` : ''}`);
          console.info(`FFTRANSCODE_PROBE #${s.index} type=${s.type} codec=${s.codec} ${s.width}x${s.height} rate=${s.sampleRate}`);
        }
        this.transcodeMsg += ` | 输出: ${outInfo.format} [${parts.join(', ')}]`;
      })
      .catch((err: Error): void => {
        console.error(`FFTRANSCODE_FAIL ${err.message}`);
        this.transcodeMsg = `失败: ${err.message}`;
      })
      .finally((): void => {
        this.transcoding = false;
      });
  }

  private doPlayVideo(): void {
    if (this.srcPath.length === 0 || this.playing) {
      return;
    }
    this.playing = true;
    this.videoMsg = '打开中...';

    const dec = new FFVideoDecoder();
    let frames = 0;
    dec.open(this.srcPath, -1, this.netOptionsFor(this.srcPath))
      .then((info: FFVideoOpenInfo): void => {
        this.videoMsg = `${info.width}x${info.height} ${info.fps.toFixed(0)}fps ${info.codec}`;
        this.xcc.setXComponentSurfaceSize({ surfaceWidth: info.width, surfaceHeight: info.height });
        const surfaceId = this.xcc.getXComponentSurfaceId();
        dec.on('frame', (ev: FFVideoFrameEvent): void => {
          frames++;
        });
        dec.on('end', (): void => {
          this.playing = false;
          this.videoMsg = `播放完成 frames=${frames}`;
          console.info(`FFVIDEO_OK frames=${frames} dur=${info.durationMs}`);
        });
        dec.on('error', (msg: string): void => {
          this.playing = false;
          this.videoMsg = `失败: ${msg}`;
          console.error(`FFVIDEO_FAIL ${msg}`);
        });
        dec.start(surfaceId);
      })
      .catch((err: Error): void => {
        this.playing = false;
        this.videoMsg = `打开失败: ${err.message}`;
        console.error(`FFVIDEO_OPEN_FAIL ${err.message}`);
      });
  }

  private async createTrackRenderer(info: FFAudioOpenInfo): Promise<audio.AudioRenderer> {
    const renderer = await audio.createAudioRenderer(this.rendererOptionsOf(info));
    await renderer.start();
    return renderer;
  }

  private startTrackPlayback(dec: FFAudioDecoder, info: FFAudioOpenInfo): void {
    this.createTrackRenderer(info)
      .then((renderer: audio.AudioRenderer): void => {
        this.renderer = renderer;
        let packets = 0;
        let finished = false;
        // Single guarded teardown: safe against end+error both firing and
        // against repeated calls (e.g. page dispose during playback).
        const finish = (): void => {
          if (finished) {
            return;
          }
          finished = true;
          this.audioDec = undefined;
          this.currentTrack = -1;
          this.renderer = undefined;
          this.writeChain
            .catch((e: BusinessError): void => {
              console.error(`FFAUDIO_WRITE_FAIL ${e.message}`);
            })
            .then(async (): Promise<void> => {
              await this.releaseRenderer(renderer);
            });
        };
        dec.on('pcm', (packet: FFAudioPacket): void => {
          packets++;
          this.writeChain = this.writeChain
            .then(async (): Promise<void> => {
              await renderer.write(packet.data);
            })
            .catch((e: BusinessError): void => {
              console.error(`FFAUDIO_WRITE_FAIL ${e.message}`);
            });
        });
        dec.on('switched', (sw: FFAudioOpenInfo): void => {
          this.trackMsg = `已切换到轨道 #${this.currentTrack} (${sw.sampleRate}Hz)`;
          console.info(`FFSWITCH_OK to=${this.currentTrack} rate=${sw.sampleRate}`);
        });
        dec.on('end', (): void => {
          console.info(`FFAUDIO_OK track=${this.currentTrack} packets=${packets}`);
          this.trackMsg = `播放完成 (轨道 #${this.currentTrack}, ${packets} 包)`;
          finish();
        });
        dec.on('error', (msg: string): void => {
          console.error(`FFAUDIO_FAIL ${msg}`);
          this.trackMsg = `失败: ${msg}`;
          finish();
        });
        dec.start();
      })
      .catch((err: Error): void => {
        console.error(`FFAUDIO_RENDERER_FAIL ${err.message}`);
        this.trackMsg = `创建播放器失败: ${err.message}`;
        this.audioDec = undefined;
        this.currentTrack = -1;
      });
  }

  private playTrack(index: number): void {
    if (this.probedPath.length === 0) {
      return;
    }
    if (this.currentTrack === index && this.audioDec !== undefined) {
      return;
    }
    if (this.audioDec === undefined) {
      const dec = new FFAudioDecoder();
      this.audioDec = dec;
      this.currentTrack = index;
      this.trackMsg = `正在播放轨道 #${index}...`;
      console.info(`FFAUDIO_TRACK_START track=${index}`);
      dec.open(this.probedPath, index)
        .then((info: FFAudioOpenInfo): void => {
          this.trackMsg = `轨道 #${index}: ${info.codec} ${info.sampleRate}Hz`;
          this.startTrackPlayback(dec, info);
        })
        .catch((err: Error): void => {
          console.error(`FFAUDIO_TRACK_OPEN_FAIL track=${index} ${err.message}`);
          this.trackMsg = `轨道 #${index} 打开失败: ${err.message}`;
          this.audioDec = undefined;
          this.currentTrack = -1;
        });
    } else {
      this.currentTrack = index;
      console.info(`FFSWITCH to=${index}`);
      this.trackMsg = `切换到轨道 #${index}...`;
      this.audioDec.switchTrack(index);
    }
  }

  private lenToNumber(v: Length): number {
    if (typeof v === 'number') {
      return v;
    }
    if (typeof v === 'string') {
      const n = parseFloat(v);
      return Number.isFinite(n) ? n : 0;
    }
    return 0;
  }

  private clearSubtitle(): void {
    if (this.subOverlay !== undefined) {
      this.subOverlay.release();
      this.subOverlay = undefined;
    }
    this.subText = '';
  }

  private applySubtitle(ev: FFSubtitleEvent): void {
    this.subEvents++;
    console.info(`FFSUBEV n=${this.subEvents} clear=${ev.clear} box=${ev.x},${ev.y} ${ev.width}x${ev.height} regions=${ev.regionCount} canvas=${ev.canvasWidth}x${ev.canvasHeight} textLen=${ev.text.length} bytes=${ev.rgba.byteLength}`);
    if (ev.canvasWidth > 0 && ev.canvasHeight > 0) {
      this.subCanvas = [ev.canvasWidth, ev.canvasHeight];
    } else if (this.videoW > 0 && this.videoH > 0) {
      this.subCanvas = [this.videoW, this.videoH];
    }
    if (ev.clear || ev.rgba.byteLength === 0) {
      this.clearSubtitle();
      if (ev.text.length > 0) {
        this.subText = ev.text;
      }
      return;
    }
    const opts: image.InitializationOptions = {
      size: { width: ev.width, height: ev.height },
      pixelFormat: image.PixelMapFormat.RGBA_8888,
      srcPixelFormat: image.PixelMapFormat.RGBA_8888
    };
    image.createPixelMap(ev.rgba, opts)
      .then((pm: image.PixelMap): void => {
        if (this.subOverlay !== undefined) {
          this.subOverlay.release();
        }
        this.subOverlay = pm;
        this.subBox = [ev.x, ev.y, ev.width, ev.height];
        this.subText = '';
        this.subMsg = `字幕事件 ${this.subEvents} (${ev.regionCount} 区域)`;
      })
      .catch((err: Error): void => {
        console.error(`FFSUB_BITMAP_FAIL ${err.message}`);
      });
  }

  private stopSubtitle(): void {
    if (this.subDec !== undefined) {
      this.subDec.stop();
      this.subDec.close();
      this.subDec = undefined;
    }
    this.clearSubtitle();
    this.subOn = false;
  }

  // Enables DVB/broadcast subtitles for the given source. As soon as the
  // container declares a subtitle stream (a PMT entry is enough) the decoder
  // opens — even when its PID never carries data.
  private startSubtitle(src: string, opts: FFNetOptions): void {
    if (this.subDec !== undefined) {
      return;
    }
    const dec = new FFSubtitleDecoder();
    this.subDec = dec;
    dec.open(src, -1, opts)
      .then((info: FFSubtitleOpenInfo): void => {
        console.info(`FFSUB_OPEN codec=${info.codec} lang=${info.language} canvas=${info.width}x${info.height} declared=${info.declared} idx=${info.streamIndex}`);
        this.subOn = true;
        this.subMsg = `字幕: ${info.codec} ${info.language} declared=${info.declared}`;
        dec.on('subtitle', (ev: FFSubtitleEvent): void => {
          this.applySubtitle(ev);
        });
        dec.on('error', (msg: string): void => {
          console.error(`FFSUB_FAIL ${msg}`);
          this.subMsg = `字幕失败: ${msg}`;
        });
        dec.start(true);
      })
      .catch((err: Error): void => {
        console.info(`FFSUB_OPEN_FAIL ${err.message}`);
        this.subDec = undefined;
        this.subOn = false;
        this.subMsg = `无字幕: ${err.message}`;
      });
  }

  // Self-check for the "PMT declares subtitles" rule: probes a stream whose PMT
  // carries a DVB subtitle entry but whose PID never delivers a single packet,
  // then opens the subtitle decoder straight from that declaration.
  private toggleSubtitle(): void {
    if (this.subDec !== undefined) {
      const n = this.subEvents;
      this.stopSubtitle();
      this.subMsg = `字幕已停止 (${n} 事件)`;
      console.info(`FFSUB_STOP events=${n}`);
      return;
    }
    this.subEvents = 0;
    this.startSubtitle(this.probedPath, this.netOptionsFor(this.probedPath));
  }

  private stopMptsPlayback(): void {
    if (this.mptsVideoDec !== undefined) {
      this.mptsVideoDec.stop();
      this.mptsVideoDec.close();
      this.mptsVideoDec = undefined;
    }
    if (this.mptsAudioDec !== undefined) {
      this.mptsAudioDec.stop();
      this.mptsAudioDec.close();
      this.mptsAudioDec = undefined;
    }
    if (this.mptsRenderer !== undefined) {
      const r = this.mptsRenderer;
      this.mptsRenderer = undefined;
      this.mptsWriteChain
        .catch((e: BusinessError): void => {
          console.error(`FFAUDIO_WRITE_FAIL ${e.message}`);
        })
        .then(async (): Promise<void> => {
          await this.releaseRenderer(r);
        });
    }
    this.mptsPlaying = false;
  }

  private doPlayProgram(p: FFProgramInfo): void {
    if (this.mptsPlaying && this.mptsProgram === p.programNum) {
      const frames = this.mptsFrames;
      const packets = this.mptsAudioPackets;
      this.stopMptsPlayback();
      this.mptsMsg = `已停止节目 ${p.programNum} (视频 ${frames} 帧, 音频 ${packets} 包)`;
      console.info(`FFMPTS_STOP program=${p.programNum} frames=${frames} audioPackets=${packets}`);
      return;
    }
    this.stopMptsPlayback();
    this.mptsPlaying = true;
    this.mptsProgram = p.programNum;
    this.mptsFrames = 0;
    this.mptsAudioPackets = 0;
    this.mptsMsg = `节目 ${p.programNum} 打开中...`;
    const opts: FFNetOptions = { program: p.programNum };

    const vdec = new FFVideoDecoder();
    this.mptsVideoDec = vdec;
    vdec.open(this.probedPath, -1, opts)
      .then((info: FFVideoOpenInfo): void => {
        console.info(`FFMPTS_VIDEO_OPEN program=${p.programNum} ${info.width}x${info.height} fps=${info.fps} codec=${info.codec}`);
        this.mptsMsg = `节目 ${p.programNum} (${p.serviceName}): ${info.width}x${info.height} ${info.codec}`;
        this.xcc.setXComponentSurfaceSize({ surfaceWidth: info.width, surfaceHeight: info.height });
        const surfaceId = this.xcc.getXComponentSurfaceId();
        vdec.on('frame', (ev: FFVideoFrameEvent): void => {
          this.mptsFrames++;
        });
        vdec.on('end', (): void => {
          console.info(`FFMPTS_VIDEO_END program=${p.programNum} frames=${this.mptsFrames}`);
          if (this.mptsPlaying) {
            const frames = this.mptsFrames;
            const packets = this.mptsAudioPackets;
            this.stopMptsPlayback();
            this.mptsMsg = `节目 ${p.programNum} 播放完成 (视频 ${frames} 帧, 音频 ${packets} 包)`;
          }
        });
        vdec.on('error', (msg: string): void => {
          console.error(`FFMPTS_VIDEO_FAIL program=${p.programNum} ${msg}`);
          this.stopMptsPlayback();
          this.mptsMsg = `节目 ${p.programNum} 视频失败: ${msg}`;
        });
        vdec.start(surfaceId);

        const adec = new FFAudioDecoder();
        this.mptsAudioDec = adec;
        adec.open(this.probedPath, -1, opts)
          .then((ainfo: FFAudioOpenInfo): void => {
            console.info(`FFMPTS_AUDIO_OPEN program=${p.programNum} ${ainfo.codec} ${ainfo.sampleRate}Hz ch=${ainfo.channels}`);
            this.createTrackRenderer(ainfo)
              .then((renderer: audio.AudioRenderer): void => {
                this.mptsRenderer = renderer;
                adec.on('pcm', (packet: FFAudioPacket): void => {
                  this.mptsAudioPackets++;
                  this.mptsWriteChain = this.mptsWriteChain
                    .then(async (): Promise<void> => {
                      await renderer.write(packet.data);
                    })
                    .catch((e: BusinessError): void => {
                      console.error(`FFAUDIO_WRITE_FAIL ${e.message}`);
                    });
                });
                adec.on('error', (msg: string): void => {
                  console.error(`FFMPTS_AUDIO_FAIL program=${p.programNum} ${msg}`);
                });
                adec.start(true);
              })
              .catch((err: Error): void => {
                console.error(`FFMPTS_RENDERER_FAIL ${err.message}`);
              });
          })
          .catch((err: Error): void => {
            console.error(`FFMPTS_AUDIO_OPEN_FAIL program=${p.programNum} ${err.message}`);
          });
      })
      .catch((err: Error): void => {
        console.error(`FFMPTS_VIDEO_OPEN_FAIL program=${p.programNum} ${err.message}`);
        this.stopMptsPlayback();
        this.mptsMsg = `节目 ${p.programNum} 打开失败: ${err.message}`;
      });
  }

  private formatStreamIndexes(indexes: Array<number>): string {
    const parts: string[] = [];
    for (let i = 0; i < indexes.length; i++) {
      parts.push(`#${indexes[i]}`);
    }
    return parts.join(', ');
  }

  aboutToDisappear(): void {
    this.stopMptsPlayback();
    this.stopSubtitle();
    if (this.audioDec !== undefined) {
      this.audioDec.stop();
      this.audioDec.close();
      this.audioDec = undefined;
    }
    if (this.renderer !== undefined) {
      const r = this.renderer;
      this.renderer = undefined;
      this.releaseRenderer(r);
    }
    if (this.framePm !== undefined) {
      this.framePm.release();
      this.framePm = undefined;
    }
    if (this.subOverlay !== undefined) {
      this.subOverlay.release();
      this.subOverlay = undefined;
    }
  }

  @Builder
  streamRow(s: FFStreamInfo)
  {
    Column({ space: 4 }) {
      Row({ space: 6 }) {
        Text(`#${s.index}  [${s.type}]  ${s.codec}`)
          .fontSize(15)
          .fontWeight(FontWeight.Medium)
          .layoutWeight(1)
        if (s.type === 'audio' && this.currentTrack === s.index) {
          Text('播放中')
            .fontSize(12)
            .fontColor('#1971C2')
        }
      }
      .width('100%')
      if (s.type === 'video') {
        Text(`${s.width}x${s.height}   ${Math.round(s.bitRate / 1000)} kbps`)
          .fontSize(13)
          .fontColor('#666666')
      }
      if (s.type === 'audio') {
        Text(`${s.sampleRate} Hz   ${s.channels} ch   ${s.language}   点击播放/切换`)
          .fontSize(13)
          .fontColor(this.currentTrack === s.index ? '#1971C2' : '#666666')
      }
      if (s.type === 'subtitle') {
        Text(`lang: ${s.language}`)
          .fontSize(13)
          .fontColor('#666666')
      }
      if (s.pid >= 0) {
        Text(`PID: ${s.pid}${this.programLabel(s)}`)
          .fontSize(12)
          .fontColor('#868E96')
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding(12)
    .borderRadius(8)
    .backgroundColor(this.currentTrack === s.index && s.type === 'audio' ? '#D0EBFF' : '#F1F3F5')
    .onClick(() => {
      if (s.type === 'audio') {
        this.playTrack(s.index);
      }
    })
  }

  build() {
    Scroll() {
      Column({ space: 12 }) {
        Text('FFmpeg Demo')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)

      TextInput({ placeholder: '输入媒体文件绝对路径', text: this.srcPath })
        .onChange((value: string) => {
          this.srcPath = value;
        })

      Button(this.probing ? '探测中...' : '探测媒体信息')
        .width('60%')
        .enabled(!this.probing && this.srcPath.length > 0)
        .onClick(() => {
          this.doProbe();
        })

      if (this.errMsg.length > 0) {
        Text(this.errMsg)
          .fontSize(13)
          .fontColor('#D9480F')
          .width('100%')
      }

      if (this.info !== undefined) {
        Column({ space: 8 }) {
            Text(`格式: ${this.info.format} (${this.info.formatLongName})`)
              .fontSize(14)
            Text(`时长: ${this.info.duration.toFixed(2)} s    码率: ${Math.round(this.info.bitRate / 1000)} kbps`)
              .fontSize(14)
            Text(`流数量: ${this.info.streams.length}`)
              .fontSize(14)
            if (this.info.programs.length > 0) {
              Text(`节目数量: ${this.info.programs.length} (MPTS)`)
                .fontSize(14)
                .fontColor('#2F9E44')
              ForEach(this.info.programs, (p: FFProgramInfo) => {
                Column({ space: 4 }) {
                  Row({ space: 6 }) {
                    Text(`节目 ${p.programNum}  ${p.serviceName}`)
                      .fontSize(15)
                      .fontWeight(FontWeight.Medium)
                      .layoutWeight(1)
                    Button(this.mptsPlaying && this.mptsProgram === p.programNum ? '停止' : '播放')
                      .fontSize(12)
                      .height(28)
                      .enabled(this.probedPath.length > 0)
                      .onClick(() => {
                        this.doPlayProgram(p);
                      })
                  }
                  .width('100%')
                  Text(`PMT PID: ${p.pmtPid} (v${p.pmtVersion})    PCR PID: ${p.pcrPid}    提供者: ${p.serviceProvider}${(p.flags & 1) === 1 ? '    [RUNNING]' : ''}`)
                    .fontSize(13)
                    .fontColor('#666666')
                  Text(`流: ${this.formatStreamIndexes(p.streamIndexes)}`)
                    .fontSize(13)
                    .fontColor('#666666')
                }
                .alignItems(HorizontalAlign.Start)
                .width('100%')
                .padding(12)
                .borderRadius(8)
                .backgroundColor(this.mptsPlaying && this.mptsProgram === p.programNum ? '#D3F9D8' : '#F1F3F5')
              }, (p: FFProgramInfo) => p.programNum.toString())
            }
            ForEach(this.info.streams, (s: FFStreamInfo) => {
              this.streamRow(s)
            }, (s: FFStreamInfo) => s.index.toString())
          }
          .alignItems(HorizontalAlign.Start)
          .width('100%')
      }

      Divider()

      Row({ space: 12 }) {
        Button(this.extracting ? '抽帧中...' : '抽取帧 @5s')
          .layoutWeight(1)
          .enabled(!this.extracting && this.srcPath.length > 0)
          .onClick(() => {
            this.doExtract();
          })
        Button(this.decoding ? '解码中...' : '音频解码播放')
          .layoutWeight(1)
          .enabled(!this.decoding && this.srcPath.length > 0)
          .onClick(() => {
            this.doDecodeAudio();
          })
      }
      .width('100%')

      if (this.frameMsg.length > 0) {
        Text(`抽帧: ${this.frameMsg}`)
          .fontSize(13)
          .fontColor('#495057')
      }
      if (this.audioMsg.length > 0) {
        Text(`音频: ${this.audioMsg}`)
          .fontSize(13)
          .fontColor('#495057')
      }
      if (this.framePm !== undefined) {
        Image(this.framePm)
          .objectFit(ImageFit.Contain)
          .width('100%')
          .height(180)
          .borderRadius(8)
      }

      Divider()

      Button(this.playing ? '播放中...' : '视频软解播放')
        .width('60%')
          .enabled(!this.playing && this.srcPath.length > 0)
        .onClick(() => {
          this.doPlayVideo();
        })

      Stack() {
        XComponent({ id: 'ffvideo', type: XComponentType.SURFACE, controller: this.xcc })
          .width('100%')
          .height(200)
        if (this.subOverlay !== undefined && this.subCanvas[0] > 0 && this.videoBoxW > 0) {
          Image(this.subOverlay)
            .width(Math.max(1, Math.round(this.subBox[2] * this.videoBoxW / this.subCanvas[0])))
            .height(Math.max(1, Math.round(this.subBox[3] * this.videoBoxH / this.subCanvas[1])))
            .position({
              x: Math.round(this.subBox[0] * this.videoBoxW / this.subCanvas[0]),
              y: Math.round(this.subBox[1] * this.videoBoxH / this.subCanvas[1])
            })
        }
        if (this.subText.length > 0) {
          Text(this.subText)
            .fontSize(16)
            .fontColor(Color.White)
            .backgroundColor('#99000000')
            .textAlign(TextAlign.Center)
            .width('100%')
            .position({ x: 0, y: 150 })
        }
      }
      .width('100%')
      .height(200)
      .borderRadius(8)
      .backgroundColor('#000000')
      .onAreaChange((oldArea: Area, newArea: Area): void => {
        this.videoBoxW = this.lenToNumber(newArea.width);
        this.videoBoxH = this.lenToNumber(newArea.height);
      })

      if (this.videoMsg.length > 0) {
        Text(`视频: ${this.videoMsg}`)
          .fontSize(13)
          .fontColor('#495057')
      }
      Row({ space: 12 }) {
        Button(this.subOn ? '停止字幕' : '字幕解码')
          .layoutWeight(1)
          .enabled(this.probedPath.length > 0)
          .onClick(() => {
            this.toggleSubtitle();
          })
      }
      .width('100%')
      if (this.subMsg.length > 0) {
        Text(`字幕: ${this.subMsg}`)
          .fontSize(13)
          .fontColor('#7048E8')
      }
      if (this.trackMsg.length > 0) {
        Text(`轨道: ${this.trackMsg}`)
          .fontSize(13)
          .fontColor('#1971C2')
      }
      if (this.mptsMsg.length > 0) {
        Text(`节目播放: ${this.mptsMsg}`)
          .fontSize(13)
          .fontColor('#2F9E44')
      }

      Divider()

      Button(this.transcoding ? '转码中...' : '转码 TS → MP4 (160x90)')
        .width('60%')
          .enabled(!this.transcoding && this.srcPath.length > 0)
        .onClick(() => {
          this.doTranscode();
        })

      if (this.transcodeMsg.length > 0) {
        Text(`转码: ${this.transcodeMsg}`)
          .fontSize(13)
          .fontColor('#495057')
      }
    }
    .width('100%')
    .padding(16)
    .alignItems(HorizontalAlign.Center)
    }
    .width('100%')
    .height('100%')
    .align(Alignment.Top)
    .scrollBar(BarState.Auto)
  }
}
