# suplayer

[English](README.md) | 中文

基于 FFmpeg 的 HarmonyOS 媒体播放技术栈。

## 模块

| 模块 | 类型 | 说明 |
|---|---|---|
| [`ffmpeg`](ffmpeg) | HAR（`@suplayer/ffmpeg`） | 基于 FFmpeg 的原生媒体内核：媒体探测、帧提取、音频/视频/字幕解码、多节目 TS、转码。[文档](ffmpeg/README.md) |
| [`sup`](sup) | HAP（示例应用） | 演示内核全部能力的示例应用（HLS 直播、多节目 TS、多音轨无缝切换、DVB 字幕、硬件解码、转码） |
| [`cmplayer`](cmplayer) | HAR（开发中） | 基于内核的播放器 UI 组件库 |
| [`tools/ffmpeg-build`](tools/ffmpeg-build) | 构建脚本 | FFmpeg 7.1.1 / OpenH264 2.5.0 / OpenSSL 3.4.1 的 HarmonyOS 交叉编译脚本（`arm64-v8a`、`x86_64`） |

## 安装

```json5
// oh-package.json5
"dependencies": {
  "@suplayer/ffmpeg": "^1.6.0"
}
```

完整 API 文档：
[`ffmpeg/README.md`](ffmpeg/README.md)。

## 快速示例

```ts
import { probe, FFVideoDecoder } from '@suplayer/ffmpeg';

const info = await probe('https://example.com/live/index.m3u8');
const vdec = new FFVideoDecoder();
await vdec.open('https://example.com/live/index.m3u8', -1, {
  liveStartIndex: -1   // 从直播边缘开始播放
});
vdec.start(surfaceId); // 来自 XComponent
```

## 构建原生库

`ffmpeg/src/main/cpp/third_party/` 中的 FFmpeg/OpenH264/OpenSSL 预编译库由
`tools/ffmpeg-build/` 下的脚本生成（MSYS2 + OpenHarmony LLVM 工具链）。
详见 [ffmpeg/README.md](ffmpeg/README.md#building-from-source)。

## 许可证

本项目基于 **LGPL-2.1-or-later** 许可证发布 — 见 [LICENSE](LICENSE)。
`ffmpeg` 模块静态链接 FFmpeg 7.1.1、OpenH264 2.5.0 与 OpenSSL 3.4.1；
三方库声明与 LGPL 重链说明见
[ffmpeg/THIRD_PARTY_NOTICES.md](ffmpeg/THIRD_PARTY_NOTICES.md)。
