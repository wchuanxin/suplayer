import { SupPlayerController, SupTrackInfo, SupHwMode } from './SupPlayerController';

let supPlayerSeq: number = 0;

@Component
export struct SupPlayer {
  @Prop @Watch('onSrcChanged') src: string = '';
  @Prop hwMode: SupHwMode = 'auto';
  @State isPlaying: boolean = false;
  @State isPaused: boolean = false;
  @State posMs: number = 0;
  @State durMs: number = 0;
  @State backendLabel: string = '';
  @State errText: string = '';
  @State trackList: SupTrackInfo[] = [];
  @State curTrack: number = -1;
  private ctrl: SupPlayerController = new SupPlayerController();
  private xcc: XComponentController = new XComponentController();
  private uiTimer: number = -1;
  private surfaceReady: boolean = false;
  private compId: string = '';

  private formatClock(ms: number): string {
    const total: number = Math.max(0, Math.floor(ms / 1000));
    const h: number = Math.floor(total / 3600);
    const m: number = Math.floor((total % 3600) / 60);
    const s: number = total % 60;
    if (h > 0) {
      return `${h}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
    }
    return `${m}:${s.toString().padStart(2, '0')}`;
  }

  private syncFromCtrl(): void {
    this.isPlaying = this.ctrl.playing;
    this.isPaused = this.ctrl.paused;
    this.posMs = this.ctrl.positionMs;
    this.durMs = this.ctrl.durationMs;
    this.backendLabel = this.ctrl.backend;
    this.errText = this.ctrl.errorMessage;
    this.trackList = this.ctrl.tracks.slice();
    this.curTrack = this.ctrl.currentTrack;
  }

  private openSrc(src: string): void {
    this.ctrl.open(src, { surfaceId: this.xcc.getXComponentSurfaceId(), hw: this.hwMode });
    this.syncFromCtrl();
  }

  onSrcChanged(): void {
    if (this.surfaceReady && this.src.length > 0) {
      this.openSrc(this.src);
    }
  }

  aboutToAppear(): void {
    this.compId = `supplayer_${supPlayerSeq}`;
    supPlayerSeq++;
    this.uiTimer = setInterval((): void => {
      this.syncFromCtrl();
    }, 500);
  }

  aboutToDisappear(): void {
    if (this.uiTimer >= 0) {
      clearInterval(this.uiTimer);
      this.uiTimer = -1;
    }
    this.ctrl.release();
  }

  build() {
    Column({ space: 8 }) {
      Stack() {
        XComponent({ id: this.compId, type: XComponentType.SURFACE, controller: this.xcc })
          .onLoad(() => {
            this.surfaceReady = true;
            if (this.src.length > 0 && !this.ctrl.playing) {
              this.openSrc(this.src);
            }
          })
          .width('100%')
          .height('100%')
        if (!this.isPlaying) {
          Text(this.errText.length > 0 ? this.errText : '输入媒体地址后播放')
            .fontSize(14)
            .fontColor('#868E96')
            .textAlign(TextAlign.Center)
            .padding(12)
        }
      }
      .width('100%')
      .height(220)
      .borderRadius(8)
      .backgroundColor('#000000')

      Row({ space: 8 }) {
        Button(this.isPlaying ? (this.isPaused ? '恢复' : '暂停') : '播放')
          .layoutWeight(1)
          .enabled(this.src.length > 0)
          .onClick(() => {
            if (!this.ctrl.playing) {
              this.openSrc(this.src);
            } else if (this.ctrl.paused) {
              this.ctrl.resume();
            } else {
              this.ctrl.pause();
            }
            this.syncFromCtrl();
          })
        Button('停止')
          .layoutWeight(1)
          .enabled(this.isPlaying)
          .onClick(() => {
            this.ctrl.stop();
            this.syncFromCtrl();
          })
      }
      .width('100%')

      Text(`${this.formatClock(this.posMs)} / ${this.durMs > 0 ? this.formatClock(this.durMs) : '直播'}`)
        .fontSize(13)
        .fontColor('#495057')
        .width('100%')
      if (this.durMs > 0 && this.isPlaying) {
        Progress({ value: Math.min(this.posMs, this.durMs), total: this.durMs, type: ProgressType.Linear })
          .width('100%')
          .height(5)
          .color('#1971C2')
      }
      if (this.backendLabel.length > 0) {
        Text(`解码后端: ${this.backendLabel === 'hw' ? '硬解 (AVCodec)' : '软解 (FFmpeg)'}`)
          .fontSize(12)
          .fontColor('#2F9E44')
          .width('100%')
      }

      if (this.trackList.length > 1) {
        Scroll() {
          Row({ space: 8 }) {
            ForEach(this.trackList, (t: SupTrackInfo) => {
              Text(`${t.language.length > 0 ? t.language : `轨道 ${t.index}`} · ${t.codec}`)
                .fontSize(12)
                .fontColor(this.curTrack === t.index ? Color.White : '#495057')
                .backgroundColor(this.curTrack === t.index ? '#1971C2' : '#F1F3F5')
                .padding({ left: 10, right: 10, top: 6, bottom: 6 })
                .borderRadius(12)
                .onClick(() => {
                  this.ctrl.switchTrack(t.index);
                  this.syncFromCtrl();
                })
            }, (t: SupTrackInfo) => t.index.toString())
          }
        }
        .scrollable(ScrollDirection.Horizontal)
        .scrollBar(BarState.Off)
        .width('100%')
      }
    }
    .width('100%')
  }
}
