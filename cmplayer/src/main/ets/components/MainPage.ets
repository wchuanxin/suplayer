import { SupPlayer } from '../player/SupPlayer';

@Component
export struct MainPage {
  @State srcInput: string = '';
  @State playSrc: string = '';

  build() {
    Column({ space: 12 }) {
      Text('cmplayer')
        .fontSize(20)
        .fontWeight(FontWeight.Bold)
      TextInput({ placeholder: '输入媒体文件路径或 HLS/TS URL' })
        .onChange((value: string) => {
          this.srcInput = value;
        })
      Button('播放')
        .width('60%')
        .enabled(this.srcInput.length > 0)
        .onClick(() => {
          this.playSrc = this.srcInput;
        })
      if (this.playSrc.length > 0) {
        SupPlayer({ src: this.playSrc, hwMode: 'auto' })
      }
    }
    .width('100%')
    .height('100%')
    .padding(16)
  }
}
