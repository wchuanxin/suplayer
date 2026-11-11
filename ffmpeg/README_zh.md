# @suplayer/ffmpeg

[English](README.md) | 中文

基于 FFmpeg 的 HarmonyOS 媒体内核（兼容 API 24+，HarmonyOS 6.1.1 及以上）。
以 HAR 形式发布，内置 `arm64-v8a` 与 `x86_64` 预编译原生库。

## 特性

- **媒体探测** — 任意 FFmpeg 支持格式的容器/编解码/流元数据，包括带多音轨/
  字幕轨与语言标签的裸 MPEG-TS 流，以及多节目传输流的 PSI 节目表
  （PAT/PMT/SDT）。
- **帧提取** — 解码并提取指定时间戳处的视频帧（RGBA，可直接喂给
  `PixelMap`）。
- **音频解码** — 软解为 PCM（F32LE 交织），采样率与声道布局跟随源（单声道
  到 7.1；特殊或 >8 声道布局下混为立体声），带实时步调、音轨瞬时热切换
  （全部音轨并行解码，切换只是重路由输出，零重缓冲）、暂停/恢复。
- **视频解码** — 软解并直接渲染到 `XComponent`/`OH_NativeWindow` Surface：
  符合条件的 SDR YUV 帧以 NV12 直写 surface（省去整次 RGB 转换），其余内容
  经 RGBA 渲染。H.264/HEVC 硬解（AVCodec Kit）与常备的软解并行存在，
  `hw: 'auto'` 下播放中途的硬解失败会改道软解而不是终止播放。软件解码支持
  中国 AVS 标准：AVS2（GB/T 33475.2，经 libdavs2）与 AVS3
  （AVS3-P2/IEEE.1857.10，经 libuavs3d），见于 CCTV 4K/UHD 广播 TS 流
  （流类型 0xd2/0xd4）。AVS1（CAVS，流类型 0x42）仅支持基准（Jizhun）档次
  解码；AVS+ 广播档次（AVS-P16）上游 FFmpeg/davs2 不支持。
- **倍速播放** — 音频/视频/字幕三路解码器的用户级 0.5x–2.0x 倍速
  （`setSpeed`）：音频经 FFmpeg `atempo` 滤镜保音高，视频/字幕按内容时间线
  缩放缓步调，音画同步天然保持（两路都留在内容时钟上）。
- **字幕** — DVB/广播字幕解码为合成后的 RGBA 位图（或 UTF-8 文本），凭容器
  声明即可开启（一条 PMT 表项就够，即使该字幕 PID 从不来数据）。
- **多节目 TS（MPTS）** — 按节目号把播放锁定到单个 MPEG-TS 节目；其余
  节目的流在解复用器处被丢弃。
- **转码/转封装** — 完整管线：解码 → 滤镜（`setpts`、`scale`、`fps`）→
  编码（视频用 OpenH264，音频用原生 AAC）→ MP4/MPEG-TS 封装，支持
  `+faststart`。
- **自定义与描述符 I/O** — JS 字节源（`registerStreamIO`、`"streamio:<id>"`
  URL）与 POSIX 描述符源（`"fd://<n>"` URL）在一切接受文件路径的地方均可用。
- **网络与直播** — HTTPS（OpenSSL）、HTTP、HLS（直播与点播）、HTTP 裸 TS。
  凡接受文件路径之处 `src` 均可传 URL。

全部处理运行在原生工作线程；JS API 基于 Promise/回调，绝不阻塞 UI 线程。
原生诊断转发到 `hilog` 的 `0xFF00` 域；条目为 app 类型日志，带前导类型
标记输出 —— 用 `0xff00` 过滤（原始输出中显示为 `A0xff00`）。标签：
`FFmpeg`（libavformat/libavcodec 日志转发）、`FFDemux`（共享解复用会话）、
`FFVideo` / `FFVideoHW`（软件/硬件视频路径）、`FFAudioSW`（音频）、
`FFSub` / `FFSubtitle`（字幕）。

## 安装

```bash
ohpm install @suplayer/ffmpeg
```

或在 `oh-package.json5` 中声明依赖：

```json5
// oh-package.json5
"dependencies": {
  "@suplayer/ffmpeg": "^1.16.0"
}
```

> 版本说明：**1.7.0** 新增 AVS2/AVS3 解码；自该版本起预编译 `libffmpeg.so`
> 为 GPL-3.0-or-later（见[许可证](#许可证)）。1.6.x 是最后一个
> LGPL-2.1-or-later 构建（不含 AVS2 支持）。

## 权限

网络源（HTTP/HTTPS/HLS）需要在宿主 HAP 的 `module.json5` 中声明网络权限：

```json5
"requestPermissions": [
  { "name": "ohos.permission.INTERNET" }
]
```

读取应用自身沙箱内的文件无需权限；其余路径需要常规的文件访问权限。

## 快速上手

```ts
import {
  probe, extractFrame, transcode,
  FFAudioDecoder, FFVideoDecoder,
  FFMediaInfo, FFFrame, FFTranscodeOptions, FFTranscodeResult
} from '@suplayer/ffmpeg';

// 1. 探测媒体文件
const info: FFMediaInfo = await probe('/data/app/el2/100/base/.../files/input.ts');
console.info(info.format, info.duration, info.streams.length);

// 2. 提取一帧（RGBA）
const frame: FFFrame = await extractFrame(path, 5000); // ms

// 3. 解码并经 AudioRenderer 播放音频
const dec = new FFAudioDecoder();
const info = await dec.open(path, 1); // 流 #1
dec.on('pcm', (pkt) => renderer.write(pkt.data));
dec.on('end', () => renderer.stop());
dec.start(); // paced=true（实时步调）；传 false 为全速解码

// 播放中切换音轨（多轨 TS）
dec.switchTrack(2);

// 4. 把视频渲染到 XComponent surface
const vdec = new FFVideoDecoder();
const vinfo = await vdec.open(path);
vdec.on('frame', (ev) => console.info(`frame @${ev.ptsMs}ms`));
vdec.start(surfaceId); // 来自 XComponent

// 5. 转码：TS -> MP4 并缩放
const result: FFTranscodeResult = await transcode(srcPath, {
  dst: outPath,
  width: 160,
  height: 90,
  videoBitrate: 400000,
  audioBitrate: 64000
});

// 6. 播放 HLS 直播（HTTPS），从直播边缘开始
const live = new FFVideoDecoder();
await live.open('https://example.com/live/index.m3u8', -1, {
  liveStartIndex: -1,        // 末尾第一个分片 = 最低延迟
  analyzeMs: 5000,           // 限制流信息分析时长，加快起播
  allowedExtensions: 'ALL'   // 分片 URL 伪装过扩展名的播放列表
});
live.start(surfaceId);

// 7. 多节目 TS：先探测节目表，再播放其中一个节目
const tsInfo: FFMediaInfo = await probe('https://example.com/mpts.ts');
for (const p of tsInfo.programs) {
  console.info(`program ${p.programNum} "${p.serviceName}" PMT ${p.pmtPid}`);
}
const vdec2 = new FFVideoDecoder();
await vdec2.open('https://example.com/mpts.ts', -1, { program: 101 });
vdec2.start(surfaceId); // 只解复用节目 101 的流
```

## API

### `probe(src: string, options?: FFNetOptions): Promise<FFMediaInfo>`

返回容器元数据与完整流列表。每条 `FFStreamInfo` 携带 `index`、`type`
（`video`/`audio`/`subtitle`/...）、编解码器名、尺寸或采样率/声道数、
`language`、时长与码率。裸 MPEG-TS 输入的每条流还携带 `pid`（其传输 PID）
与 `programIndex`（指向 `programs` 的索引）；其他容器上报 `pid: -1`，因为
其流 id 是轨道/播放列表索引而非 PID。

`FFMediaInfo.programs` 列出多节目传输流的 PSI 节目表（PAT/PMT/SDT）：
`programNum`、`pmtPid`、`pcrPid`、`pmtVersion`（PMT version_number，未知为
`-1`）、`flags`（bit 0 = `AV_PROGRAM_RUNNING`）、SDT 的
`serviceName`/`serviceProvider` 以及归属各节目的 `streamIndexes`。输入不含
PSI 时 `pmtPid`/`pcrPid`/`pmtVersion` 为 `-1`。HLS 为每个变体暴露一个合成
节目（`programNum` 0，无 SDT 名称、无 PID），MP4 则一个也不报。

`FFMediaInfo.chapters` 列出容器章节（mp4/MKV 章节轨），形如
`{ id, startMs, endMs, title }`；`FFMediaInfo.tags` 携带全部容器元数据标签
（`Record<string, string>`：title、artist、encoder、service_name、...）。
每条 `FFStreamInfo` 额外上报 `dispositionDefault` / `dispositionForced`
（容器 disposition 位，用于默认轨选择与强制叙事）和 `rotation`（来自
display matrix 的顺时针角度，0/90/180/270）。

### `extractFrame(src: string, timeMs: number): Promise<FFFrame>`

解码 `timeMs` 处最近的关键帧并返回 RGBA 像素（`width`、`height`、
`data: ArrayBuffer`）。

### `transcode(src: string, options: FFTranscodeOptions): Promise<FFTranscodeResult>`

后台线程上的一次性转码。`FFTranscodeOptions`：

| 字段 | 默认值 | 说明 |
|---|---|---|
| `dst` | （必填） | 输出路径；容器由扩展名推断（`.mp4`、`.ts`） |
| `width` / `height` | 源尺寸 | scale 滤镜目标 |
| `fps` | 源帧率 | fps 滤镜目标 |
| `videoBitrate` | 800000 | H.264（OpenH264）目标码率，bps |
| `audioBitrate` | 96000 | AAC 目标码率，bps；`0` 禁用音轨 |
| `startTimeMs` | 0 | 编码前先 seek 进源 |
| `durationMs` | 0（到 EOF） | 编码该时长后停止 |
| `vcodec` | `'h264'` | 视频编解码器：`'h264'`（libopenh264）、`'mpeg2'`（mpeg2video）或 `'copy'` — 不重编码直接转封装视频轨（h264/hevc 输入在输出为 mpegts 时转换为 Annex-B） |
| `acodec` | `'aac'` | 音频编解码器：`'aac'`、`'ac3'` 或 `'copy'` — 不重编码直接转封装音频轨 |
| `filter` | 无 | 插在 scale/fps 之前的附加 libavfilter 视频链（如 `'hqdn3d=4:3:6:4.5'`、`'unsharp=5:5:1.0'`） |

`FFTranscodeResult` 上报产出文件的 `outputSize`（字节）、`durationMs` 与编码
视频 `frames`。输出时间戳归一化为从零开始。

### `FFNetOptions`

`probe` 与两个解码器 `open` 均接受的可选网络/直播选项：

| 字段 | 默认值 | 说明 |
|---|---|---|
| `userAgent` | `Lavf/7.1.1` | HTTP `User-Agent`；同时应用于全部 HLS 分片与密钥下载 |
| `headers` | 无 | 附加 HTTP 头，CRLF 分隔（如 `"Referer: https://x\r\nCookie: y=z"`） |
| `allowedExtensions` | FFmpeg 白名单 | HLS 分片扩展名白名单，逗号分隔。传入 `'ALL'`（乃至设置该字段本身）即为分片 URL 伪装过扩展名的流禁用严格的扩展名/格式匹配（如 `.php` 播放列表、以 `.jpg` 命名的 TS 分片）。注意：FFmpeg 7.1 的 `hls_probe` 还会拒绝路径不含 `.m3u8` 且响应 MIME 不是 `mpegurl` 的播放列表 URL —— 这种拒绝 `allowedExtensions` 救不回来，因为它只在解复用器选定之后放宽分片检查。当源解析器为变体/延迟选择（`variantIndex` / `maxBandwidth` / `targetLatencyMs`）抓取并校验播放列表文本（`#EXTM3U` + 结构标签）时，HLS 输入格式会被预设到上下文上，打开时完全跳过探测 |
| `liveStartIndex` | `-3` | HLS 直播：起始分片索引；负值从直播边缘倒数。`-1` 从最新分片开始（延迟最低，可能从 GOP 中间加入） |
| `analyzeMs` | FFmpeg 默认 | 限制 `avformat_find_stream_info` 的分析时长（ms）—— 调低可加快直播起播 |
| `hw` | `'auto'` | 视频解码后端：`'auto'` 优先硬解、失败回退软解；`'force'` 强制硬解；`'never'` 始终软解（仅 FFVideoDecoder） |
| `program` | 未设置 | MPEG-TS 节目号锁定。设定后仅解复用该节目的流；自动选轨被限制在节目内，显式 `streamIndex` 也会对照其校验。未知节目号使 `open` 失败 |
| `httpPersistent` | `true` | 在 HLS 播放列表刷新与分片下载之间复用一条 HTTP/TCP 连接（`http_persistent=1`），而非每次请求都重连 —— 降低直播边缘延迟与 TLS 握手开销 |
| `reconnect` | `true` | 网络请求失败自动重试：`reconnect=1`、`reconnect_streamed=1`、`reconnect_on_network_error=1`、`reconnect_on_http_error=4xx,5xx`、`reconnect_delay_max=5` —— 可扛住 CDN 边缘未命中（新宣告分片的瞬时 404/410）与短暂断网 |
| `variantIndex` | 未设置 | HLS 主播放列表变体选择：选取该 0 基索引处的变体。优先于 `maxBandwidth`。设置 `abr` 时为钉选（统计仍可用，切换禁用） |
| `maxBandwidth` | `0` | HLS 变体带宽上限：选取 `BANDWIDTH <= maxBandwidth` 的最高带宽变体（全部超限时取最低）。`0` = 不限。主播放列表在打开前抓取一次，选中的变体 URL 替换源；任何抓取/解析失败回退到原始 URL。设置 `abr` 时同时作为切换梯子的上限 |
| `abr` | `false` | 直播 HLS 主播放列表自适应码率：共享会话按 10s 窗口测量视频流的媒体时间推进；持续饥饿（低于实时的 15% 且持续约 8s）触发原地重建降一档变体（先于 15s 饥饿中止），持续健康（≥ 实时的 90%）按指数增长的驻留窗口（30s → 失败探测后至 300s；持续健康 60s 重置）探测升回一档。未显式设置 `variantIndex` 时 open 从最高带宽档起步。订阅者收到 `'reconnected'` + `discontinuity('reconnect')`；`getBufferStats()` 暴露当前变体（`variantIndex` / `variantBandwidthBps` / `variantSwitchCount`）。仅在 `live` 且主播放列表含 ≥ 2 个已知带宽变体时生效 |
| `targetLatencyMs` | `0` | 直播目标延迟：未显式设置 `liveStartIndex` 时，由媒体播放列表的 `TARGETDURATION` 推导 `liveStartIndex`（`-ceil(target/segDur)`，钳制 1..30）并持续收敛 —— 共享会话每 ~5s 丢弃超过 `target + max(2s, target/2)` 的订阅者积压。`0` = 关闭 |
| `autoReconnect` | 自动 | 401/403/404/429 读错误（令牌过期、滑动窗口未命中、限流）后的原地会话重建：共享会话重新解析源（新鲜 CDN 令牌）并重开、校验流布局；订阅者收到 `'reconnected'` + `discontinuity('reconnect')`。`true` = 始终开启，`false` = 始终关闭，未设置 = 自动（直播开、点播关） |
| `reconnectMaxRetries` | `3` | 会话放弃（EOF）前的重建尝试次数；指数退避 1s..8s（HTTP 429 为 2s..16s） |
| `dvrSeek` | `false` | 预留（共享连接上的 DVR 窗口 seek）；暂无效果 |
| `audioTracks` | `'all'` | `FFAudioDecoder` 音轨解码模式：`'all'` 并行解码全部音轨（零间隙热切换）；`'first'` 只解码选中轨道，`switchTrack` 时再惰性打开目标轨道 —— 多轨源上 CPU/功耗更低，切换处有短暂拼接而非无缝 |
| `frameEventMs` | `0` | `FFVideoDecoder` `'frame'` 事件之间的最小间隔（ms）（`0` = 每帧）。首帧必定上报；更大的值可削减 NAPI 桥流量，适合只需要周期性 PTS 更新（音画同步、进度）的应用 |
| `startTimeMs` | `0` | 点播起始位置（ms）：解码器 seek 到目标处或之前的关键帧并丢弃目标前帧（视频：解码但不渲染/上报；音频：不重采样/上报），起播精确落在请求位置。`0` = 从头。源不可 seek 时忽略（直播回退为正常起播） |
| `sharedDemux` | `false` | 共享解复用器：对同一源以匹配的解复用选项（`userAgent`/`headers`/`allowedExtensions`/`liveStartIndex`/`analyzeMs`/`program`/`httpPersistent`/`reconnect`/`variantIndex`/`maxBandwidth`/`abr`/`targetLatencyMs`/`autoReconnect`/`reconnectMaxRetries`）打开的每个解码器（视频/音频/字幕）通过进程级解复用会话共享**一条**网络连接，而非每个解码器一条。单个解复用线程把包路由到各解码器的有界队列；队列高水位反压保留了私有连接的自节流语义（暂停的解码器会像从前停摆自己的读取那样停摆共享读取）。并发的首次打开者相互竞争，落败者挂接到胜者（启动保持并行，稳态一条连接）。挂接者用各自的目标前丢弃来补偿不同的 `startTimeMs`。`probe()` 复用活跃会话的上下文读取元数据，不新开连接 |
| `rtspTransport` | FFmpeg 默认 | rtsp:// 源的 RTSP 传输：`'tcp'`（在控制连接内交织）或 `'udp'` |
| `bufferSize` | `0` | 底层 socket 缓冲区大小（字节）（FFmpeg `buffer_size`）；高码率 RTSP/组播流可调大 |
| `rwTimeoutMs` | `0` | I/O 读写超时（ms）（FFmpeg `rw_timeout`）；防护非 HLS 协议上停摆的 socket。`0` = 无超时 |
| `swThreads` | `1` | 软件视频解码线程数：`1` = 单线程（最低延迟）；`0` = 自动（FFmpeg 按 CPU 数取）；`N` = `N` 个解码线程（帧级 + 切片级线程）。帧线程带来 `N-1` 帧输出延迟 —— 直播保持保守（`2`）；重型点播软解（4K/HDR 硬解回退）可给更多。硬解生效期间无效果 |

直播流的 `durationMs` 为 `0` 且 `'end'` 事件不会触发；用 `stop()` 结束
播放。`stop()` 也会立即中止进行中的网络读取。

### 直播队列事件、恢复与统计

三个解码器都暴露其共享解复用订阅的实时健康度：

- `'overflow'`（`{ dropped }`）：队列的直播追赶阀丢弃了一段积压 ——
  `dropped` 为本次丢弃的包数。暂停期间也会触发（工作线程侧轮询计数器），
  而这正是直播积压溢出的高发时刻。
- `'discontinuity'`（`{ reason, seekMs? }`）：流不连续。`'resync'` 出现在
  显式 `flushToKeyframe()` / `flushBacklog()` 或队列积压丢弃之后；
  `'reconnect'` 出现在会话原地重建连接之后；`'seek'` 出现在共享连接
  `seekTo` 重新锚定之后 —— 负载携带 `seekMs`，即实际落点目标（ms，流 pts
  域），当目标前关键帧间隔大到无法逐帧解码穿越时（快速落点），它可能早于
  请求目标，见「运行时 seek 与帧捕获」；`'hw2sw'`（仅视频）出现在播放中途
  硬件 → 软件解码切换之后，见「硬件解码」。
- `'reconnected'`（仅视频/音频，无负载）：共享会话从 401/403/404/429 中
  恢复 —— 重新解析源并重开，流索引保持有效。随后跟随
  `discontinuity('reconnect')`。
- `'error'`（`(msg: string, err: FFError)`）：运行时失败。第一个参数是
  原始消息串（1.15 之前的形态，不变）；第二个参数是 1.15.0 新增的结构化
  `{ code, message }` 负载，`code` 为从消息推导的最佳努力机器可读分类：
  `'surface' | 'hw' | 'seek' | 'network' | 'open' | 'demux' | 'decode' |
  'internal'`（根因子串优先于包装层，例如 "shared demux open
  failed: ..." 内层的 HTTP 403 归类为 `network`）。未知取值请按
  `'internal'` 处理。`open()` 的 rejection 仍为纯字符串。
- `off(event?)`（三个解码器，1.15.0）：移除单个事件的监听器（每事件
  单槽位）；不带参数调用则移除该解码器的全部监听器
  （`removeAllListeners` 语义）。
- `getBufferStats()`（视频/音频）：`{ bufferedAheadMs, readBytes,
  measuredBitrateBps, liveLatencyMs, overflowCount, reconnectCount,
  variantIndex, variantBandwidthBps, variantSwitchCount }` —
  队列深度、已消费字节、EWMA 码率（5s 窗口）、溢出/重连计数器，以及
  （1.16）ABR 变体状态：当前主播放列表变体（0 基文档索引，未选择/私有
  连接为 -1）、其 `BANDWIDTH`（未知为 0）与会话迄今应用的变体切换次数。
- `getStats()`（视频/音频）：`{ decodedFrames, droppedFrames,
  overflowCount, reconnectCount, discontinuityCount }`。

`FFMediaInfo.programs[].bandwidth` 携带 HLS 变体的 `BANDWIDTH`（普通
MPEG-TS 节目为 `0`），应用可据此通过 `probe()` 按真实带宽排序/挑选变体。

### `FFAudioDecoder`

```ts
open(src: string, streamIndex?: number, options?: FFNetOptions): Promise<FFAudioOpenInfo>
on(event: 'pcm', cb: (packet: FFAudioPacket) => void): void   // F32LE 交织
on(event: 'end', cb: () => void): void
on(event: 'error', cb: (msg: string, err: FFError) => void): void
on(event: 'switched', cb: (info: FFAudioOpenInfo) => void): void
on(event: 'overflow', cb: (ev: FFOverflowEvent) => void): void
on(event: 'discontinuity', cb: (ev: FFDiscontinuityEvent) => void): void
on(event: 'reconnected', cb: () => void): void
off(event?: 'pcm' | 'end' | 'error' | 'switched' | 'overflow' | 'discontinuity' | 'reconnected'): void
start(paced?: boolean): void      // 默认 true = 实时步调
switchTrack(streamIndex: number): void  // 播放中瞬时热切换
flushBacklog(): void              // 直播音画重同步：丢弃队列积压
detach(): void                    // 防泄漏清理，不做线程 join
setRate(rate: number): void       // 音画同步微变速 [0.98, 1.02]
setSpeed(speed: number): void     // 用户倍速 [0.5, 2.0]，保音高（atempo）
seekTo(targetMs: number): Promise<void>  // 运行时点播 seek（任意连接）
getBufferStats(): FFBufferStats
getStats(): FFPlaybackStats
pause(): void; resume(): void
stop(): void; close(): void
```

源的全部音轨（锁定 `program` 时以节目为限）从起播起并行解码，因此
`switchTrack` 只是重路由输出：不重开解码器、不重新缓冲、无间隙。每条轨道
都重采样到初始轨道的采样率与声道布局，所以输出格式（以及你的
`AudioRenderer` 配置）在切换间保持不变。

`FFAudioOpenInfo.channels` 上报输出声道数，`FFAudioOpenInfo.channelLayout`
上报输出声道掩码（FFmpeg `AV_CH_*` 位，数值上与 OHOS
`audio.AudioChannelLayout` 取值一致；`0x3` = 立体声）—— 可直接传给
`AudioRendererOptions.streamInfo`。

### `FFVideoDecoder`

```ts
open(src: string, streamIndex?: number, options?: FFNetOptions): Promise<FFVideoOpenInfo>
on(event: 'frame', cb: (ev: FFVideoFrameEvent) => void): void
on(event: 'end', cb: () => void): void
on(event: 'error', cb: (msg: string, err: FFError) => void): void
on(event: 'info', cb: (ev: FFVideoInfoEvent) => void): void  // { backend: 'hw' | 'sw' }
on(event: 'overflow', cb: (ev: FFOverflowEvent) => void): void
on(event: 'discontinuity', cb: (ev: FFDiscontinuityEvent) => void): void
on(event: 'reconnected', cb: () => void): void
off(event?: 'frame' | 'end' | 'error' | 'info' | 'overflow' | 'discontinuity' | 'reconnected'): void
start(surfaceId: string): void    // 渲染到 XComponent surface
flushToKeyframe(): void            // 直播音画重同步：积压丢到最新关键帧
setSpeed(speed: number): void      // 用户倍速 [0.5, 2.0]：步调 /speed，内容 pts 不变
detach(): void                     // 防泄漏清理，不做线程 join
seekTo(targetMs: number): Promise<void>   // 运行时点播 seek（任意连接）
captureFrame(): Promise<FFCapturedFrame>  // 下一解码帧，RGBA8888（软解）
getBufferStats(): FFBufferStats
getStats(): FFPlaybackStats
pause(): void; resume(): void
stop(): void; close(): void
```

#### 运行时 seek 与帧捕获

`seekTo(targetMs)` 在任意连接上可用：

- **私有连接**：seek 到目标处或之前的关键帧，冲刷解码器（硬解路径经
  `OH_VideoDecoder_Flush` 冲刷），重新锚定步调时钟并丢弃目标前帧，播放
  精确地从目标处恢复。进行中的 seek 会拒绝并发的 seek。
- **共享连接**（`sharedDemux: true`）：共享解复用会话在其解复用线程上
  全会话地执行 seek 并通知每个订阅者；本解码器收到通知后自行重新锚定
  （解码器/硬解冲刷、步调、目标前丢弃过滤）。仍在等待的共享 seek 会被
  更新的 seek 取代（后写优先 —— 被取代的 promise 立即 resolve）。

**快速落点**：当目标处或之前的关键帧回溯超过 12 s 时，逐帧解码穿越整段
目标前 intra-GOP 会让落点停摆数秒 —— 此时消费方改为锚定在该关键帧上
（MX-Player 式快进快退），实际落点目标经 `discontinuity('seek')` 上报
（`seekMs` 负载，ms）。更小的间隔保持精确落点。

两条路径在解码器/会话未运行或 15 s 超时后都会 reject。`captureFrame()`
resolve 出下一解码帧 `{ width, height, data: ArrayBuffer }`（RGBA8888，
原始尺寸）—— 仅限软解，需要截图时请以 `hw: 'never'` 打开；播放运行中或
暂停时调用均安全。

#### 播放倍速

任一解码器上的 `setSpeed(speed)` 把用户级播放倍速钳制到 [0.5, 2.0]
（起播前后均可调用）：

- **音频**：经插入在重采样器之后的 `atempo` 滤镜保音高。输出的 PCM 块
  pts 保持在内容时间线上 —— 速度 S 下 N 个输出样本覆盖
  `N*S/sampleRate` 秒内容 —— 因此 `AudioRenderer` 配置与内容时钟进度
  计算均不变。与 `setRate` 微变速正交（有效输出速率 = speed × rate）；
  变速在块边界重建滤镜；seek 会复位它。
- **视频**：渲染步调（硬解渲染线程与软解路径一致）把 pts 增量的墙钟
  目标除以倍速 —— 内容增量 D 以 D/speed 墙钟秒呈现。帧 pts 事件保持在
  内容时间线上；变速在下一帧重新锚定步调。
- **字幕**：步调模式同样把 pts 增量的墙钟目标除以倍速，与音/视频步调
  保持一致。

三个解码器请保持同一倍速：音频与视频都留在内容时间线上，音画同步天然
保持。

#### 硬件解码

`hw: 'auto'`（默认）下，Annex-B 封装（MPEG-TS、HLS-TS）的 H.264/HEVC 流
由 AVCodec Kit 硬件解码器（`OH_VideoDecoder`，surface 模式）解码并直接
渲染到 XComponent surface —— 包括 10-bit HEVC（已用 4K 3840x2160@50
HEVC Main10 直播流在实时帧率下验证）。MP4 式长度前缀封装会被自动识别并
留在软件路径。`start` 后经 `'info'` 事件（`{ backend: 'hw' | 'sw' }`）
一次性上报实际后端。

首包喂给硬解之前，AVCodec Kit 能力预检
（`OH_AVCodec_GetCapabilityByCategory`）会校验该编解码器存在硬件解码器、
且支持流的分辨率与档次。预检是保守的：未知或不可映射的档次、未知尺寸
从不阻断解码，容器帧率元数据仅作参考（不匹配只告警）。失败时
`hw: 'force'` 经 `'error'` 事件上报原因，`hw: 'auto'` 则静默回退软解。

**播放中途回退**：`hw: 'auto'` 下，播放*中途*的硬解失败（编解码服务
错误、输入停摆）不再终止播放 —— 工作线程改道常备的软解：停掉并销毁
codec，用捕获的 surfaceId 重建窗口（codec 服务已消耗旧 surface），冲刷
解码器并落到下一关键帧（直播源把共享积压跳到其最新关键帧；点播保留
积压、只跳过不可解码的 GOP 中段包）。切换表现为一次新的 `'info'` 事件
（`{ backend: 'sw' }`）加 `discontinuity('hw2sw')`。HDR 内容（PQ/HLG）
除外 —— 软件路径无法做色调映射 —— 其硬解错误照旧上抛；`hw: 'force'`
也继续把错误上抛。

#### 软件渲染（NV12 直写）

只要内容允许，软件视频路径会把解码帧不经 RGB 转换直接渲染进 surface：
偶数尺寸、SDR 传递函数、非全量程 YUV 的 NV12 / YUV420P / YUV420P10LE 帧
把窗口协商为 NV12（`NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP`）—— NV12 帧
按平面逐块拷贝，其余符合条件的格式由 swscale 直接转换进映射平面。SDR
YUV→RGB 矩阵会声明给合成器（`SET_COLOR_GAMUT`，SD/旧式原色用 BT.601，
HD 及以上用 BT.709）。不符合条件的内容（奇数尺寸、全量程/JPEG、HDR
传递函数、其他像素格式）保持经典 RGBA_8888 路径。

#### HDR（HDR10 / HLG）

`FFVideoOpenInfo` 上报流的 `colorPrimaries`、`colorTransfer` 与
`colorRange`（ffprobe 命名，如 `bt2020`、`smpte2084`、`arib-std-b67`、
`tv`）。硬解路径上，SMPTE2084（HDR10）或 ARIB-STD-B67（HLG）传递的
BT.2020 流会进一步通过设置 surface 色域
（`NATIVEBUFFER_COLOR_GAMUT_BT2100_PQ` / `_HLG`）路由给显示端，由合成器
正确做色调映射。软件路径不受影响：它渲染 RGBA，本就无法还原 HDR。

### `FFSubtitleDecoder`

```ts
open(src: string, streamIndex?: number, options?: FFNetOptions): Promise<FFSubtitleOpenInfo>
on(event: 'subtitle', cb: (ev: FFSubtitleEvent) => void): void
on(event: 'pes', cb: (payload: ArrayBuffer, pts: number) => void): void  // 裸 PES 泵
on(event: 'end', cb: () => void): void
on(event: 'error', cb: (msg: string, err: FFError) => void): void
on(event: 'overflow', cb: (ev: FFOverflowEvent) => void): void
on(event: 'discontinuity', cb: (ev: FFDiscontinuityEvent) => void): void
off(event?: 'subtitle' | 'pes' | 'end' | 'error' | 'overflow' | 'discontinuity'): void
setRawPump(enable: boolean): void  // start() 之前：按原样转发 PES
setSpeed(speed: number): void      // 用户倍速 [0.5, 2.0]：步调模式步调 /speed
setDelayMs(delayMs: number): void  // 文本字幕同步偏移 ±60s：仅平移显示时刻
start(paced?: boolean): void       // 默认 true = 按呈现时间步调
detach(): void                    // 防泄漏清理，不做线程 join
pause(): void; resume(): void
stop(): void; close(): void
```

`open` 接受仅被容器*声明*的流 —— PMT 里的一条 DVB 字幕表项就够：即使该
字幕 PID 从不携带一个包也能打开成功并保持使能（打开信息中
`declared: true`），因此字幕可以直接从节目表开启。`streamIndex < 0` 选取
第一条被声明的字幕流（设置了 `program` 时以该节目为限）。

每条 `'subtitle'` 事件携带一个解码出的显示集：

| 字段 | 说明 |
|---|---|
| `clear` | `true` 表示空显示集 —— 清除当前屏幕上的内容 |
| `ptsMs` / `durationMs` | 呈现时间与显示时长 |
| `canvasWidth` / `canvasHeight` | 用于缩放的合成尺寸（缺失时回退到视频尺寸） |
| `x` / `y` / `width` / `height` | 可见字幕内容的包围盒，合成坐标系 |
| `rgba` | 该包围盒的 RGBA8888 位图（`width * height * 4` 字节），`clear` 时为空 |
| `regionCount` | 合成进该包围盒的位图区域数 |
| `text` | 文本字幕流的 UTF-8 文本（位图流留空） |

位图区域（DVB、DVD）被合成为单个包围盒并裁剪到非透明区域 —— 整屏 DVB
页面会收缩为一条小字幕条，每个显示集跨桥的只有几十 KB。

```ts
const subs = new FFSubtitleDecoder();
const info = await subs.open(liveUrl, -1, { liveStartIndex: -1 }); // PMT 声明即可
subs.on('subtitle', async (ev) => {
  if (ev.clear || ev.rgba.byteLength === 0) { overlay = undefined; return; }
  overlay = await image.createPixelMap(ev.rgba, {      // 绘制在 (ev.x, ev.y)，
    size: { width: ev.width, height: ev.height },      // 按 boxWidth / ev.canvasWidth 缩放
    pixelFormat: image.PixelMapFormat.RGBA_8888,
    srcPixelFormat: image.PixelMapFormat.RGBA_8888
  });
});
subs.start(true);
```

`durationMs` 给出每个显示集的在屏寿命：为它安排一次清除（如演示应用所
做），直播流丢弃终止性空显示集时字幕就不会滞留屏幕。

暂停/恢复是步调感知的：暂停期间呈现时钟冻结，恢复时从暂停处继续，而不是
爆发冲过积压。

### 自定义 I/O — `registerStreamIO` / `pushStreamIO` / `closeStreamIO`

把任意字节源（SMB 共享、自定义缓存、加密存储）桥接进完整管线而无需触碰
C++：注册一个 JS 支持的源，之后凡接受 URL 之处（`probe`、`extractFrame`、
`transcode`、三个解码器）都可以用 `"streamio:<id>"` 寻址。

```ts
import { registerStreamIO, pushStreamIO, closeStreamIO } from '@suplayer/ffmpeg';

registerStreamIO(1, {
  read: (seq, offset, length) => {
    mySource.read(offset, length).then((buf: ArrayBuffer) => {
      pushStreamIO(1, seq, buf.byteLength, buf); // byteCount 0 = EOF，< 0 = 错误
    });
  },
  seek: (seq, offset, whence) => {   // 可选；省略 = 不可 seek
    // whence 0 = 绝对定位，2 = 上报总大小，3 = 相对末尾
    pushStreamIO(1, seq, whence === 2 ? mySource.size : mySource.seek(offset, whence));
  },
  close: () => mySource.close()
});

const info = await probe('streamio:1');           // 经 JS 桥读取元数据
await transcode('streamio:1', { dst: 'out.mp4', vcodec: 'copy', acodec: 'aac' });
closeStreamIO(1);                                  // 释放回调
```

每次 read 最多交付 64 KiB，且必须在 15 s 内经 `pushStreamIO` 完成，否则
该源永久失效（后续 read 立即返回 EOF）。不提供 `seek` 回调则源不可
seek —— moov 在尾部的 mp4 文件将无法打开。可 seek 的源让 `seekTo`、
`startTimeMs` 与 mp4 解复用器和真实文件完全一样地工作。

### 描述符源 — `fd://`

`src` 还接受 `"fd://<n>"`，其中 `<n>` 是本进程内已打开的 POSIX 文件
描述符（例如来自 `@ohos.file.fs`），凡接受文件路径/URL 之处（`probe`、
`extractFrame`、`transcode`、三个解码器）均可用。描述符归调用方所有：
内核经自定义 AVIOContext 读取和 seek 它但绝不关闭它；大小查询
（`AVSEEK_SIZE`）的解析不会移动描述符位置。已关闭的描述符使打开失败并
给出明确错误。

## 支持的 ABI

- `arm64-v8a`（真机）
- `x86_64`（模拟器）

自动捆绑 `libc++_shared.so`。

## 从源码构建

预编译原生库由项目仓库 `tools/ffmpeg-build/` 下的脚本生成（MSYS2 +
OpenHarmony LLVM 工具链）：

```bash
./build-openssl.sh          # OpenSSL 3.4.1 -> prebuilt/<abi>/libssl.a, libcrypto.a
./build-openh264.sh         # OpenH264 2.5.0 -> prebuilt/<abi>/libopenh264.a
./build-davs2.sh            # davs2 (AVS2)  -> prebuilt/<abi>/libdavs2.a
./build-uavs3d.sh           # uavs3d (AVS3) -> prebuilt/<abi>/libuavs3d.a
./build-ffmpeg.sh m7        # FFmpeg 7.1.1  -> prebuilt/<abi>/libav*.a 等
```

把产物拷入 `ffmpeg/src/main/cpp/third_party/ffmpeg/libs/<abi>/`
（`tools/ffmpeg-build/sync-m5-libs.sh` 负责此事）并重新构建 HAR。这也
同时说明了如何按 GPL/LGPL 合规要求，基于修改过的 FFmpeg 重新链接本库。

## 许可证

`libffmpeg.so` 以 **GPL-3.0-or-later** 分发 —— 见 [LICENSE](LICENSE)。
构建启用了 `--enable-gpl --enable-version3`，原因在 libdavs2
（GPL-2.0-or-later，AVS2 解码器）与 OpenSSL 3.x（Apache-2.0）。在
`tools/ffmpeg-build/build-ffmpeg.sh` 中移除
`--enable-gpl --enable-version3 --enable-libdavs2` 并重新构建，即可得到
不含 AVS2 支持的 LGPL-2.1-or-later 库。完整三方许可证文本与静态链接/
重链接细节见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 三方组件

| 组件 | 版本 | 许可证 |
|---|---|---|
| FFmpeg | 7.1.1 | LGPL-2.1-or-later（以 GPL-3.0 选项构建） |
| OpenH264 | 2.5.0 | BSD-2-Clause |
| davs2 | 1.6.0（master 快照） | GPL-2.0-or-later |
| uavs3d | 1.2（master 快照） | BSD-2-Clause |
| OpenSSL | 3.4.1 | Apache-2.0 |
| LLVM libc++（`libc++_shared.so`） | NDK 自带 | Apache-2.0 WITH LLVM-exception |
| zlib | 系统提供 | Zlib |

许可证文本、精确的 FFmpeg 构建配置与 GPL 重链接说明：
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
