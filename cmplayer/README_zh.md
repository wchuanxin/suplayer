# cmplayer

[English](README.md) | 中文

基于 [`@suplayer/ffmpeg`](../ffmpeg/README.md) 媒体内核的播放器 UI 组件库：
提供音视频同步播放组件（`SupPlayer`）及其控制器（`SupPlayerController`），
可直接嵌入任意页面。

## 特性

- **音视频同步播放** — 视频、音频解码器并行，音频为主时钟；位置取自包
  PTS，周期性偏差校正是"冻结超前一方"而不是丢帧或爆发追帧。
- **真正可暂停的 暂停/恢复** — 内核在暂停时平移 pacing 锚点，恢复后从
  暂停处继续，不会爆发快进；暂停同时冻结 `AudioRenderer` 与两个解码器。
- **硬解自动回退** — `hw: 'auto' | 'force' | 'never'` 透传给内核；
  实际后端（`hw`/`sw`）在上报后显示在界面上。
- **音轨切换** — 轨道列表来自后台 `probe`；`switchTrack` 重路由内核的
  并行音频解码器，零间隙。
- **Surface 就绪处理** — `src` 与 XComponent surface 无论谁先就绪都能正确
  起播；修改 `src` 自动重新打开播放。

## 安装

包尚未发布到 OHPM，先以本地路径引用：

```json5
// oh-package.json5
"dependencies": {
  "cmplayer": "file:../cmplayer"
}
```

`cmplayer` 自身依赖 `@suplayer/ffmpeg`（工作区内为 `file:../ffmpeg` —
外部使用时请改为已发布版本号，如 `^1.7.0`）。

## 快速开始

```ts
import { SupPlayer } from 'cmplayer';

@Entry
@Component
struct Index {
  @State src: string = '';

  build() {
    Column({ space: 12 }) {
      TextInput({ placeholder: '输入媒体文件路径或 HLS/TS URL' })
        .onChange((v: string) => { this.src = v; })
      if (this.src.length > 0) {
        SupPlayer({ src: this.src, hwMode: 'auto' })
      }
    }
    .width('100%').height('100%').padding(16)
  }
}
```

`MainPage` 就是封装了上述流程的现成演示页。

## API

### `SupPlayer`（组件）

| 属性 | 类型 | 说明 |
|---|---|---|
| `src` | `string` | 媒体源（文件路径或 URL）；受监听，变更后自动重新起播 |
| `hwMode` | `SupHwMode` | 视频解码后端选择，默认 `'auto'` |

渲染 220vp 的 `XComponent` 视频画面、播放/暂停/恢复/停止控制、进度显示、
实际解码后端、音轨选择 chips 与错误文本；UI 状态每 500 ms 从控制器同步。

### `SupPlayerController`

```ts
open(src: string, options: SupPlayerOptions): void   // 先停止当前播放，再打开
pause(): void; resume(): void; stop(): void
switchTrack(streamIndex: number): void               // 零间隙音轨切换
release(): void                                      // 停止并释放渲染器/解码器
```

可观察状态：

| 字段 | 说明 |
|---|---|
| `playing` / `paused` | 会话状态 |
| `positionMs` / `durationMs` | 播放位置（主时钟）与时长 |
| `backend` | `'hw'` \| `'sw'` \| `''` — 上报后的实际视频后端 |
| `tracks` / `currentTrack` | 后台 probe 得到的 `SupTrackInfo[]` 与当前轨道 |
| `errorMessage` | 最近一次播放错误（由 `SupPlayer` 显示） |

### `SupPlayerOptions`

| 字段 | 类型 | 说明 |
|---|---|---|
| `surfaceId` | `string` | 视频解码器渲染目标 XComponent surface id |
| `hw` | `SupHwMode?` | 解码后端选择，默认 `'auto'` |
| `net` | `FFNetOptions?` | 网络/直播选项，透传给两个解码器与轨道探测 |

`SupTrackInfo` 含 `index`、`codec`、`language`、`sampleRate`、`channels`。

## 权限

网络源需要在宿主 HAP 的 `module.json5` 中声明 `ohos.permission.INTERNET`
（见 [ffmpeg/README.md](../ffmpeg/README.md#permissions)）。

## 许可证

LGPL-2.1-or-later（项目代码，见 [../LICENSE](../LICENSE)）。其依赖
`@suplayer/ffmpeg` 携带的原生 `libffmpeg.so` 以 GPL-3.0-or-later 分发 — 见
[../ffmpeg/README.md#license](../ffmpeg/README.md#license)。
