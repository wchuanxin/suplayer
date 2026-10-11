# suplayer

[English](README.md) | 中文

基于 FFmpeg 的 HarmonyOS 媒体播放技术栈。

## 特性

- **国产广播编码格式** — 软解支持 AVS2（GB/T 33475.2，davs2）与 AVS3（AVS3-P2/IEEE.1857.10，
  uavs3d），并包含 FFmpeg 全量软解；H.264/HEVC 走 AVCodec Kit 硬解，
  失败自动回退软解 — Annex-B（TS/HLS）与长度前缀（MP4/MKV，hvcC/avcC
  经比特流滤镜转换）两种封装均支持，HDR10/HLG 硬解流直通显示端做色调映射。
- **直播** — HLS 直播/点播、直播边缘加入、主播放列表变体选择
  （`variantIndex` / `maxBandwidth`）与目标延迟控制（`targetLatencyMs`）；
  HTTP/HTTPS（OpenSSL）与 RTSP（TCP/UDP）；长连接与断线自动重连。
  共享解复用会话（`sharedDemux`）让全部解码器共用一条网络连接，
  令牌过期 / CDN 错误时原地重建会话，并通过 `overflow` / `discontinuity`
  事件与缓冲/码率统计上报队列健康度。
- **广播 TS** — 多节目 TS（MPTS）带 PSI 节目表（PAT/PMT/SDT）、节目锁定播放、
  DVB 位图字幕（仅凭 PMT 声明即可开启）、多音轨零间隙热切换。
- **点播控制** — 运行时 seek（`seekTo`，关键帧锚定、感知硬解码器冲刷）、
  `startTimeMs` 精确起播位置、帧捕获（`captureFrame`，RGBA8888）用于截图。
- **自定义 I/O** — JS 字节源（`registerStreamIO` + `"streamio:<id>"` URL）
  将 SMB 共享、自定义缓存、加密存储接入探测 / 解码 / 转码全链路，无需改动 C++。
- **转码与转封装** — 解码 → 滤镜 → 编码（OpenH264 + AAC）→ MP4/MPEG-TS
  封装，支持流拷贝转封装（`vcodec` / `acodec: 'copy'`）与附加滤镜链
  （`hqdn3d`、`unsharp` 等）。
- **播放器组件** — `cmplayer` 提供音视频同步播放组件（`SupPlayer`），
  支持暂停/恢复、切轨与偏差校正；`sup` 演示应用覆盖内核全部能力。

## 模块

| 模块 | 类型 | 说明 |
|---|---|---|
| [`ffmpeg`](ffmpeg) | HAR（`@suplayer/ffmpeg`） | 基于 FFmpeg 的原生媒体内核：媒体探测（PSI 节目表、章节/标签）、帧提取、带硬解回退的音频/视频/字幕解码、点播 seek 与帧捕获、多节目 TS、DVB 字幕、共享解复用直播、自定义 I/O（streamio）、转码/转封装。[文档](ffmpeg/README.md) |
| [`sup`](sup) | HAP（示例应用） | 演示内核全部能力：流卡片与节目表探测、MPTS 节目播放、抽帧、音频解码与音轨热切换、视频+音频统一播放（暂停/恢复、硬解选择、字幕轨点选）、DVB 字幕解码、转码 |
| [`cmplayer`](cmplayer) | HAR | 基于内核的播放器 UI 组件库：`SupPlayer` 组件 + `SupPlayerController`（音频主时钟、偏差校正、音轨切换）。[文档](cmplayer/README_zh.md) |
| [`tools/ffmpeg-build`](tools/ffmpeg-build) | 构建脚本 | FFmpeg 7.1.1 / OpenH264 2.5.0 / davs2（AVS2）/ uavs3d（AVS3）/ OpenSSL 3.4.1 的 HarmonyOS 交叉编译脚本（`arm64-v8a`、`x86_64`） |

## 安装

```json5
// oh-package.json5
"dependencies": {
  "@suplayer/ffmpeg": "^1.10.0"
}
```

完整 API 文档：
[`ffmpeg/README.md`](ffmpeg/README.md)，版本说明见
[`ffmpeg/CHANGELOG.md`](ffmpeg/CHANGELOG.md)。自 **1.7.0** 起预编译
`libffmpeg.so` 为 GPL-3.0-or-later（新增 AVS2/AVS3 解码），1.6.x 是
最后一个 LGPL 版本（见[许可证](#许可证)）。

## 快速示例

```ts
import { probe, FFVideoDecoder } from '@suplayer/ffmpeg';

// 1. HLS 直播：从边缘加入并维持约 6 秒延迟
const info = await probe('https://example.com/live/index.m3u8');
const live = new FFVideoDecoder();
await live.open('https://example.com/live/index.m3u8', -1, {
  liveStartIndex: -1,    // 从直播边缘开始播放
  targetLatencyMs: 6000  // 持续收敛到约 6 秒延迟
});
live.start(surfaceId);   // 来自 XComponent

// 2. 点播（私有连接）：运行时 seek + 帧捕获
const vod = new FFVideoDecoder();
await vod.open(vodUrl, -1, { hw: 'never' }); // 软解以便截图
vod.start(surfaceId);
await vod.seekTo(90000);              // seek 到 90 秒
const snap = await vod.captureFrame(); // RGBA8888 帧
```

## 构建原生库

`ffmpeg/src/main/cpp/third_party/` 中的 FFmpeg/OpenH264/OpenSSL/davs2/uavs3d
预编译库由 `tools/ffmpeg-build/` 下的脚本生成（MSYS2 + OpenHarmony LLVM 工具链）。
详见 [ffmpeg/README.md](ffmpeg/README.md#building-from-source)。

## 许可证

本项目源代码基于 **LGPL-2.1-or-later** 许可证发布 — 见 [LICENSE](LICENSE)。
`ffmpeg` 模块的预编译 `libffmpeg.so` 静态链接 FFmpeg 7.1.1、OpenH264 2.5.0、
OpenSSL 3.4.1、davs2（AVS2，GPL-2.0-or-later）与 uavs3d（AVS3，BSD-2-Clause），
因此以 **GPL-3.0-or-later** 分发 — 见 [ffmpeg/LICENSE](ffmpeg/LICENSE)。
三方库声明与重链说明见
[ffmpeg/THIRD_PARTY_NOTICES.md](ffmpeg/THIRD_PARTY_NOTICES.md)。
