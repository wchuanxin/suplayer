# 变更日志

[English](CHANGELOG.md) | 中文

## 1.12.0

### 用户级倍速播放 — 三路解码器的 `setSpeed`

- 新增 `FFAudioDecoder.setSpeed(speed)` / `FFVideoDecoder.setSpeed(speed)` /
  `FFSubtitleDecoder.setSpeed(speed)`：用户级播放倍速，钳制到 [0.5, 2.0]
  （NaN 回退为 1.0），起播前后均可调用。
- 音频保音高：在每轨重采样器之后插入 FFmpeg `atempo` 滤镜图
  （abuffer → atempo → abuffersink）。输出的 PCM 块 pts 保持在内容时间线上
  —— 速度 S 下 N 个输出样本覆盖 `N*S/sampleRate` 秒内容 —— 因此应用的
  `AudioRenderer` 配置与内容时钟进度计算均不变。1.0x 完全绕过滤镜图
  （直通）；变速在块边界排空并重建 atempo 图；seek 会复位它（atempo 窗口
  里存有 seek 前的音频）。与 `setRate` 微变速正交：有效输出速率为
  `speed × rate`。
- 视频渲染步调（硬解渲染线程与软解路径一致）与字幕步调模式把 pts 增量的
  墙钟目标除以倍速 —— 内容增量 D 以 `D/speed` 墙钟秒呈现 —— 变速在下一
  帧重新锚定；帧/字幕 pts 事件保持在内容时间线上。起播前的 `setSpeed()`
  会在解码器启动时重放（硬解在播放中途冲刷后也会重新应用）。

### `fd://` 描述符源

- 新增 `fd://<n>` 源方案，凡接受文件路径/URL 之处（`probe`、
  `extractFrame`、`transcode`、三路解码器）均可用：`<n>` 是本进程内已打开
  的 POSIX 文件描述符（例如来自 `@ohos.file.fs`）。描述符归调用方所有
  —— 内核经自定义 AVIOContext 读取/seek 它但绝不关闭它；`AVSEEK_SIZE`
  以 save/end/restore 三步解析，描述符位置不被移动。无效（已关闭）的
  描述符使打开失败并给出明确错误。

### 收尾与快速失败加固

- 有界 join 的解复用收尾：直播读取合法地可能在 `av_read_frame` 内滞留至
  30 s 读停摆看门狗；从 `stop()`/`close()` join 该线程会让 JS 调用方阻塞
  同样久（应用卡死风险）。`DemuxSession::Shutdown` 现在只等 2 s join
  预算，然后分离解复用线程 —— 孤儿线程退出时自行关闭格式上下文（互斥
  锁下无竞态的 exited/orphan 交接：恰好一方关闭，无双重关闭、无泄漏）。
- 软件渲染 surface 楔死快速失败：被毒化的 BufferQueue（每次
  `NativeWindowRequestBuffer` 都失败，如残留的 codec 服务生产者楔死
  surface 后的 41206000）曾让解码循环全速空转 —— 实测 28 分钟 / 3.4 万行
  日志 / 85% CPU。现在连续约 1 s 的请求缓冲失败后，解码器发出 `error`
  （"video surface wedged: ..., needs surface rebuild"）并停止，而不是
  空转，把恢复交给应用的重建 surface/重载链。
- 有序硬件收尾：`HwVideoDecoder::Fail()` 现在即使从 configured/error 状态
  也会先调 `OH_VideoDecoder_Stop` 再调 `OH_VideoDecoder_Destroy` —— 在
  部分设备上单靠 Destroy 会让 codec 服务的 BufferQueue 生产者仍挂在
  surface 上，永久楔死该 surface 上后续的任何软件生产者
  （`NativeWindowRequestBuffer` 41206000）。codec 错误回调与输入停摆现在
  也会记录 `GetLastError()`，收尾/回退上报的是底层硬件错误而非泛化消息。
- 字幕解码器在 `avcodec_open2` 之前设置 `pkt_timebase`：时间信息只存在于
  容器中的文本字幕（无 `-->` 行的 SRT）此前解码出的 pts/duration 为 0，
  应用永远无法在到期时清除它 —— 对白会一直留在屏幕上，直到被下一条
  字幕替换。

## 1.11.0

### NV12 直写软件渲染

- 软件视频路径跳过 RGB 转换：偶数尺寸、SDR 传递函数、非全量程 YUV 的
  NV12/YUV420P/YUV420P10LE 帧把窗口协商为 NV12
  （`NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP`），解码帧直接写进半平面缓冲
  —— NV12 帧按平面 memcpy，其余符合条件的格式由 swscale 直接转换进映射
  平面（无中间缓冲）。
- SDR YUV→RGB 矩阵声明给合成器（`SET_COLOR_GAMUT`）：SD/旧式原色用
  BT.601，HD 及以上与未指定用 BT.709。
- 平面布局解析是防御式的：`OH_NativeBuffer_MapPlanes` 被严格校验（部分
  gralloc HDI 上报垃圾布局 —— 实测 planeCount=3、rowStride 1/2），带
  `BufferHandle` 推导的回退，且每个候选布局都对照缓冲大小做边界检查。
- 不符合条件的内容（奇数尺寸、全量程/JPEG、HDR 传递函数、其他像素格式）
  保持经典 RGBA_8888 路径。

### 播放中途硬件 → 软件回退

- `hw: 'auto'` 下，播放中途的硬解失败（codec 服务错误、输入停摆、喂包
  失败）不再终止播放：工作线程改道常备的软解 —— 停止/销毁 codec，用捕获
  的 surfaceId 重建窗口（codec 服务已消耗旧 surface），冲刷解码器并落到
  下一关键帧；直播源把共享积压跳到其最新关键帧（一个关键帧间隔的缺口），
  点播保留积压、只跳过不可解码的 GOP 中段包。
- 切换表现为一次新的 `'info'` 事件（`{ backend: 'sw' }`）加
  `discontinuity('hw2sw')`，应用可据此更新 UI 与统计。
- HDR 内容除外（软件路径无法对 PQ/HLG 做色调映射）—— 其错误照旧上抛给
  应用。`hw: 'force'` 继续把硬件错误上抛。

## 1.10.1

### 共享连接 `seekTo`

- `seekTo(targetMs)` 现在在共享解复用连接上同样可用：共享解复用会话在其
  解复用线程上全会话地执行 seek（`avformat_seek_file` —— `fmt_` 的唯一
  所有者）并通知每个订阅者队列（积压丢弃 + seek 纪元推进）；各解码器本地
  重新锚定（解码器/硬解冲刷、步调、目标前丢弃过滤）并在收到通知时完成
  pending 的 seek 应答。仍在等待的共享 seek 会被更新的 seek 取代（后写
  优先 —— 被取代的应答立即 resolve）。失败的 seek 仍会通知队列
  （`ok=false`），让已武装的应答快速失败而非超时。私有连接的 seek 行为
  与之前完全一致。
- 快速落点：当目标处或之前的关键帧回溯超过 12 s 时，逐帧解码穿越整段
  目标前 intra-GOP 会让落点停摆数秒；消费方改为锚定在该关键帧上
  （MX-Player 式快进快退），实际落点目标经新的 `discontinuity('seek')`
  事件上报 —— `FFDiscontinuityEvent.seekMs`（ms，流 pts 域）。更小的
  间隔保持精确落点。

### HLS 打开与硬解冲刷修复

- 伪装播放列表打开修复：FFmpeg 7.1 的 `hls_probe` 会拒绝路径不含
  `.m3u8` 且响应 MIME 不是 `mpegurl` 的播放列表 URL（如以 `text/plain`
  下发的 `.php` 直播播放列表）；`allowedExtensions` 提示只在解复用器选定
  之后放宽 SEGMENT 检查。当源解析器已经抓取并校验过播放列表文本
  （`#EXTM3U` + 结构标签 —— 变体/延迟解析期间）时，HLS 输入格式现在被
  预设到上下文上，`avformat_open_input` 完全跳过探测；非播放列表正文
  （服务器错误页 / 空 200）会带头部字节记日志以便诊断。
- 硬件解码器冲刷修复：`OH_VideoDecoder_Flush` 把 codec 留在 FLUSHED
  状态，此时不会再发放输入缓冲 —— 点播 seek 后喂包循环死等空闲输入缓冲
  （"hw decoder input stalled (no free buffer in 10s)"），缓冲释放失败于
  "Not in running state"。现在冲刷后立即重启 codec
  （`OH_VideoDecoder_Start`），且失败的冲刷会中止缓冲回收步骤，而不是
  半冲刷着继续。

## 1.10.0

### 长度前缀容器（MP4/MKV HDR）的硬件解码

- 硬解路径现覆盖 MP4/MKV 源：当硬解资格窥探发现长度前缀（hvcC/avcC）的
  包封装而非 Annex-B 时，会拉起 `h264_mp4toannexb` / `hevc_mp4toannexb`
  比特流滤镜，每个包在喂给 OH 硬件解码器前即时转换。滤镜在 IRAP 帧前
  重新注入参数集，因此播放中途的冲刷/seek 依然有效。
- 实际效果：MP4/MKV 内的 4K HDR10/HLG HEVC 不再被迫走软件路径（该路径
  经 sws_scale RGBA 输出把 10-bit HDR 丢弃殆尽）；这些源现在硬解码码，
  并和 TS 源一样进入 BT.2100 色域/色调映射管线（`SET_COLOR_GAMUT`）。
- 回退不变：没有 avcC/hvcC extradata 的流、或任何比特流滤镜失败，仍与
  之前完全一样地回退软件解码。

## 1.9.0（未发布）

### 运行时点播 seek 与帧捕获

- 新增 `FFVideoDecoder.seekTo(targetMs)` /
  `FFAudioDecoder.seekTo(targetMs)`：私有连接（`sharedDemux: false`）上的
  运行时 seek。seek 到目标处或之前的关键帧，冲刷解码器（硬解经
  `OH_VideoDecoder_Flush` 冲刷），重新锚定步调时钟并丢弃目标前帧，播放
  精确地从目标处恢复。直播/共享连接、进行中的另一个 seek、15 s 超时均
  拒绝。
- 新增 `FFVideoDecoder.captureFrame()`：resolve 出下一解码帧，RGBA8888、
  原始分辨率（仅限软解；硬解连接上拒绝 —— 需要截图时以 `hw: 'never'`
  打开）。

### 自定义 AVIO — JS 字节源

- 新增模块函数 `registerStreamIO(id, callbacks)` /
  `pushStreamIO(id, seq, value, data?)` / `closeStreamIO(id)`：注册一个
  JS 字节源，所有接受源 URL 的 API（probe / extractFrame / transcode /
  三路解码器）都可以用 `"streamio:<id>"` 寻址。`read(seq, offset, length)`
  每次最多交付 64 KiB，必须在 15 s 内经 `pushStreamIO` 完成（0 = EOF，
  负数 = 错误）；可选的 `seek(seq, offset, whence)` 解锁 moov 在尾部的
  mp4 文件（whence 0 = 绝对定位，2 = 大小，3 = 相对末尾）。应用由此可以
  把原生文件/协议库（SMB、自定义缓存、加密存储）桥接进 FFmpeg 管线，
  无需触碰 C++ 层。

### 协议选项

- 新增 `FFNetOptions.rtspTransport`（`'tcp' | 'udp'`）：rtsp:// 源的
  RTSP 传输选择。
- 新增 `FFNetOptions.bufferSize`：底层 socket 缓冲区大小（FFmpeg
  `buffer_size`），高码率 RTSP/组播流有用。
- 新增 `FFNetOptions.rwTimeoutMs`：I/O 读写超时（FFmpeg `rw_timeout`），
  防护非 HLS 协议上停摆的 socket。

### 转码转封装与滤镜

- 新增 `FFTranscodeOptions.vcodec`：`'h264'`（默认，libopenh264）、
  `'mpeg2'`（mpeg2video）或 `'copy'` —— 不重编码直接转封装视频轨
  （h264/hevc 输入在输出为 mpegts 时转换为 Annex-B）。
- 新增 `FFTranscodeOptions.acodec`：`'aac'`（默认）、`'ac3'` 或
  `'copy'` —— 不重编码直接转封装音频轨。
- 新增 `FFTranscodeOptions.filter`：插在 scale/fps 之前的附加
  libavfilter 视频链（如 `'hqdn3d'`、`'unsharp'`）。

### 探测元数据

- `FFMediaInfo.chapters`（`FFChapterInfo[]`）：容器章节（mp4/MKV 章节
  轨），含 `id` / `startMs` / `endMs` / `title`。
- `FFMediaInfo.tags`（`Record<string, string>`）：全部容器元数据标签
  （title、artist、encoder、service_name、...）。
- `FFStreamInfo.dispositionDefault` / `.dispositionForced`：容器
  disposition 位（默认轨选择、强制叙事）。
- `FFStreamInfo.rotation`：由 display matrix 推导的顺时针旋转角度
  （0/90/180/270）。

## 1.8.0（未发布）

开发中 —— 条目随特性落地持续累积。下方 1.7.6–1.7.10 的过渡构建已并入
本版本。

### 直播队列事件与解码器统计（B2 / D4 / F3 / E2）

- 三路解码器新增 `'overflow'` 事件：共享解复用队列的直播追赶阀丢弃一段
  积压时发出（负载：该段丢弃的包数）。工作线程侧轮询使其在暂停期间也能
  检测 —— 暂停的直播消费方正是积压溢出的高发时刻。
- 三路解码器新增 `'discontinuity'` 事件，原因为 `'resync'`：出现在显式
  `flushToKeyframe()` / `flushBacklog()` 之后及队列积压丢弃之后（负载：
  原因字符串）。
- 新增 `FFVideoDecoder.getBufferStats()` /
  `FFAudioDecoder.getBufferStats()`（`FFBufferStats`）：
  `bufferedAheadMs`、`readBytes`、`measuredBitrateBps`（5s 窗口 EWMA）、
  `liveLatencyMs`（预留，-1）、`overflowCount`、`reconnectCount`。
- 新增 `FFVideoDecoder.getStats()` / `FFAudioDecoder.getStats()`
  （`FFPlaybackStats`）：`decodedFrames`、`droppedFrames`、
  `overflowCount`、`reconnectCount`、`discontinuityCount`。
- `FFAudioDecoder` 不再在切换边界双重施加变速速率；字幕防泄漏清理现在
  真正把队列从共享会话摘除并释放 PES 引用。

### HLS 源解析与连接恢复（A2 / A3 / F1）

- HLS 主播放列表变体选择的新 `FFNetOptions` 字段（A2）：`variantIndex`
  （选取该 0 基索引处的变体）与 `maxBandwidth`（选取
  `BANDWIDTH <= maxBandwidth` 的最高带宽变体；全部超限时回退到最低）。
  源 URL 在解复用打开前解析 —— 主播放列表经 FFmpeg avio 抓取一次
  （与解复用器相同的 user agent / headers / 中断处理，1 MiB 大小上限），
  选中的变体 URL 替换源。解析按设计是万无一失的：任何抓取或解析失败都
  记日志并回退到原始 URL（1.8.0 之前的行为）。共享解复用会话与私有解码
  连接上均可工作。
- 新增 `FFNetOptions.targetLatencyMs`（F1）：直播目标延迟。未显式设置
  `liveStartIndex` 时，由媒体播放列表的 `EXT-X-TARGETDURATION` 推导 HLS
  `live_start_index`（`-ceil(target/segmentDuration)`，钳制为落后边缘
  1..30 个分片）。播放期间共享解复用会话执行运行时追赶：每 ~5s 丢弃
  任何超过 `target + max(2s, target/2)` 的订阅者积压，让延迟向目标收敛
  而不是漂移。
- 可恢复 HTTP 错误后的原地会话重建（A3）：`av_read_frame` 以 401/403
  （令牌过期）、404（滑动窗口未命中）或 429（限流）失败时，共享解复用
  会话关闭上下文、重新解析源（受令牌保护的 CDN 签发新的带签名播放列表
  URL）并原地重开 —— 订阅者保持其流索引。防护：重建后的流布局（每流
  索引的编解码器 id）必须与原来一致，否则重建中止为 EOF，让应用围绕新
  布局重建管线。退避为指数式，1s..8s（429 为 2s..16s），至多
  `reconnectMaxRetries`（默认 3）次尝试；一次成功读取重置预算。由新增
  的 `FFNetOptions.autoReconnect` 控制（true = 开，false = 关，未设置 =
  自动：直播开、点播关）。
- `FFVideoDecoder` / `FFAudioDecoder` 新增 `'reconnected'` 事件（无
  负载）：共享会话重建连接后由解码工作线程发出，紧随其后的是原因为
  `'reconnect'` 的 `discontinuity`。`FFSubtitleDecoder` 只收到
  discontinuity。重建后的积压丢弃经既有的 `'overflow'`/
  `'discontinuity'` 管线上报；`getBufferStats().reconnectCount` 与
  `getStats().reconnectCount` 现在是实时计数器。
- `FFMediaInfo.programs[].bandwidth`（probe）：FFmpeg hls 解复用器发布的
  HLS 变体 BANDWIDTH 属性（`variant_bitrate` 节目元数据）；普通 MPEG-TS
  节点为 0。

## 1.7.6 – 1.7.10（2026-10-04 – 2026-10-05，内部迭代构建）

音频微变速、裸字幕 PES 泵与直播连接看门狗。这些版本是被应用直接消费的
内部迭代构建；其变更随 1.8.0 发布。

- 新增 `FFAudioDecoder.setRate(rate)`：音频微变速播放。解码工作线程以
  缩放到 `outRate / rate` 的重采样输出速率重建各轨的 `SwrContext`
  （rate 钳制到 [0.5, 2.0]），渲染器保持固定的采样率与缓冲格式 —— 无需
  重配 `AudioRenderer`。速率由工作线程在包边界惰性施加（线程安全，播放
  中可调用）；重配失败保持先前速率。
- 新增 `FFSubtitleDecoder.setRawPump(true)` + `'pes'` 事件：裸 PES 泵
  模式。完全跳过 avcodec 字幕解码，把每个解复用出的字幕包原样转发
  （payload、pts、流索引），供应用侧解析 DVB。必须在 `start()` 之前
  设置；`open()` 照常运行（裸模式下编解码上下文打开但不使用）。
- 直播读停摆看门狗（共享解复用、直播源）：30 s（`LIVE_READ_STALL_MS`）
  未交付一个包的直播连接即已死亡 —— 服务器静默关闭或网络黑洞，此时
  `av_read_frame` 永久阻塞，因为中断回调只在 stop 时中止。看门狗触发
  中断使阻塞的读取失败、解复用线程退出，应用的恢复机制可以在下次打开时
  重建会话。
- 直播慢饿看门狗（共享解复用、直播源）：被服务器限速的连接滴流式供包
  （无包看门狗永不触发），播放以远低于实时的速度爬行。解复用器现在按
  10s 墙钟窗口测量视频流的媒体时间进度（按包时长，对起播突发与 pts 跳变
  免疫）：持续 15 s 低于实时 15% 的进度中止会话（读取结束，全部订阅者
  收到 EOF），而不是吊死在一条死而未断的流上。

## 1.7.5（2026-10-04）

直播音画失步恢复 + 僵尸会话清理。

- 新增 `FFVideoDecoder.flushToKeyframe()` /
  `FFAudioDecoder.flushBacklog()`：直播音画重同步原语。丢弃共享队列积压，
  从队列中最新关键帧（视频）或下一个路由包（音频）恢复，把落后的轨道
  重新锚定到直播边缘。仍在运行的工作线程在前向 pts 跳变上重新锚定其
  步调，与原生直播溢出跳过完全一致。私有（非共享）连接上无操作。
- 三路解码器新增 `detach()`：防泄漏路径的僵尸清理。应用为避免主线程
  join 死锁而刻意跳过 `stop()`/`close()` 时，泄漏解码器的队列仍订阅着
  共享解复用会话 —— 解复用线程持续把包路由进孤儿队列，造成无穷无尽的
  "dropped 300 packets" 高水位风暴（实测 30 分钟以上）外加一条浪费的
  网络连接。`detach()` 摘除订阅而不 join 任何东西；仍在运行的工作线程
  看到被摘除的队列快速失败，喂入 EOS 自行退出，会话引用在辅助线程上
  释放，因此可能的末引用关闭（中断 + join 解复用线程 + 关闭连接）绝不
  在 JS 线程上运行。所有解码工作线程现在都在生命周期内持有自己的会话
  引用，排空期间借用的 `AVFormatContext` 保持有效。
- `HwVideoDecoder::RenderLoop` 步调现在在前向 pts 跳变（`waitMs >=
  1000`）时重新锚定，而不是全速爆发 —— 与软件路径语义一致；直播跳过后
  的爆发曾与（已重新锚定的）音频步调失步。

## 1.7.4（2026-10-03）

自适应直播反压宽限 + PCM 余量。

- 共享队列的直播追赶现在把反压宽限窗锚定到消费方的最后一次 POP，而不是
  固定的 500ms 填充等待：健康乃至被 CPU 严重饿着的消费方（应用启动争抢、
  系统侧停顿）都能让解复用步调稳定地跟在自己后面，延迟稳定、无积压丢弃；
  只有 `LIVE_STUCK_MS`（3s，暂停/挂死）内无 pop 的消费方才回退到立即的
  卡死丢弃。消除了 CPU 争抢下的起播误丢。
- 音频 PCM 线程安全队列上限 32 → 96（约 3s 余量），亚秒级 JS 线程停顿
  再也触及不到原生丢弃路径。

## 1.7.3（2026-10-02）

直播周期性跳过消除。

- 解复用直播队列溢出从立即积压丢弃切换为有界的先反压（丢弃前先宽限
  等待）；音频与软件视频步调循环在前向 pts 跳变（waitMs >= 1000）时
  重新锚定，而不是跳过后爆发。

## 1.7.2（2026-10-01）

Emit 死锁修复。

- 所有解码器 `Emit` 调用改为非阻塞（`napi_tsfn_nonblocking`）：实时
  负载在队列满时丢弃，关键事件短暂重试。线程安全队列满时的阻塞发送曾与
  JS 线程在 `stop()`/`close()` 中 join 发送工作线程相互死锁（6s 应用
  卡死）。

## 1.7.1（2026-09-30）

软件渲染崩溃修复（越界写入）。

- 修复软件视频渲染路径（`FFVideoDecoder::RenderFrame`）中的一处堆损坏
  崩溃：`SET_BUFFER_GEOMETRY` 与 surface 消费方的协商是异步的，因此最先
  请求到的缓冲可能仍带着旧的、更小的 surface 几何 —— `sws_scale` 随即把
  整幅 RGBA 帧（如 1920x1080）写进更小的缓冲，损坏堆并在下一次
  `av_malloc` 时于 jemalloc 内崩溃（`je_memalign`/`posix_memalign`），
  通常在起播后零点几秒。
- 现在写入前校验每个请求缓冲的实际 `width`/`height`/`stride`。缓冲装得
  下帧时照旧原地转换（无额外拷贝）；缓冲更小时，帧被转换进一个精确尺寸
  的临时缓冲并逐行拷贝、钳制到缓冲边界 —— 最坏情况是一次裁剪渲染而非
  崩溃。
- 兼容以像素而非字节上报 `BufferHandle::stride` 的设备（重新推导字节
  间距）。
- `SET_BUFFER_GEOMETRY`/`SET_FORMAT`/`SET_USAGE` 的返回码现在记日志而
  不再静默丢弃；修复了 sws 上下文分配失败路径上的 `OH_NativeBuffer_Unmap`
  泄漏。
- 新增 `FFNetOptions.audioTracks`（默认 `'all'` / `'first'`）：`'first'`
  只解码选中的音频轨而非并行解码全部 —— 多轨源上更低的 CPU 与功耗。
  `switchTrack` 按需惰性打开目标轨；切换有短暂拼接，而非并行路径的零
  间隙重路由。
- 新增 `FFNetOptions.frameEventMs`（默认 `0` = 每帧）：把
  `FFVideoDecoder` 的 `'frame'` 事件节流至每间隔至多一次，削减只需要
  周期性 PTS 更新的应用的 NAPI 桥流量。首帧必定发出。
- 新增 `FFNetOptions.startTimeMs`（默认 `0` = 从头）：点播起始位置。
  解码器 seek 到目标处或之前的关键帧并丢弃目标前输出（视频帧解码但不
  渲染，音频帧不重采样不发出），起播精确落在请求位置 —— 支持从指定位置
  续播与经重打开实现的应用级 seek。不可 seek 的源（直播）回退正常起播；
  丢弃防护上限保护时间戳损坏的流。
- 新增 `FFNetOptions.sharedDemux`（默认 `false`）：共享解复用器。对同一
  源以匹配的解复用选项打开的每个解码器（视频/音频/字幕）通过进程级
  `DemuxSession` 注册表共享一条网络连接，而非每解码器一条。单个解复
  用线程把包路由到各订阅者的有界队列（300 包 / 64 MB 高水位反压保留了
  私有连接的自节流语义）。并发的首次打开者竞争，落败者挂接到胜者，启动
  并行性的同时稳态保持单连接。请求不同 `startTimeMs` 的挂接者用各自的
  目标前丢弃过滤补偿；`probe()` 复用活跃会话的上下文读取元数据，不开新
  连接。同时修复了硬解资格窥探在非 Annex-B 流上丢失首个视频包的问题
  （窥探到的关键帧现在为软件路径保留）。
- 修复共享解复用主线程冻结（appfreeze `THREAD_BLOCK_3S`）：
  - 硬解资格窥探在 JS 主线程上同步运行（`start()`）；当共享队列的解复
    用线程还在抓取首个分片、或被其他订阅者反压时，窥探可能阻塞满其 3s
    期限并触发看门狗。共享路径的窥探期限现在为 500ms（届时回退软件
    解码）。
  - `PacketQueue::Pop` 只在被唤醒时轮询中止标志，而设置解码器停止标志
    从不唤醒队列 —— 主线程上的 `stop()`/`close()` 可能永久挂死在 join
    一个阻塞在空、停滞队列上的工作线程。Pop 现在按 100ms `wait_for`
    间隔复查中止，`StopInternal` 调用新的 `PacketQueue::Wake()`。
  - 启动宽限：已订阅但尚未开始消费的订阅者（应用的视频/音频打开会合
    闸住音频启动）不再按常规 300 包水位反压共享读取 —— 在首次 pop 前
    最多缓冲 4000 包，一个尚未启动的解码器不会再饿死其他解码器的首帧。
- 修复直播流上慢消费方的音频饿死（共享解复用）：持续慢的视频订阅者
  （如强制软件解码设置下软件解码的 1080p50 直播）填满其 300 包队列并让
  解复用线程无限期阻塞在 `Route()` —— 音频订阅者被饿死（静音、PCM 队列
  满丢弃），HLS 读取者掉出直播边缘（整段跳过、PTS 跳变数分钟）。新增
  `FFNetOptions.live`（默认 `false`）：视频订阅者队列切换为追赶溢出策略
  —— 队列满时丢弃积压并从下一关键帧恢复路由（干净的 IDR 重启），解复
  用线程不再阻塞在慢的直播消费方上，音频保持实时流动。
- 直播追赶溢出策略扩展到音频队列：消费方永不排空的音频订阅者（如应用
  退后台/锁屏时应用侧 `AudioRenderer` `start()` IPC 挂死）填满队列并让
  `Route()` 永久阻塞 —— 整个共享会话死锁（视频也冻结，停摆看门狗的恢复
  循环进入僵尸态）。音频队列现在溢出时冲刷积压并从下一包恢复（音频帧
  独立可解码，无需等关键帧）；解复用线程不再可能被任何直播订阅者死锁。
  点播播放与字幕队列保持阻塞反压语义。

## 1.7.0（2026-09-29）

中国广播编解码支持（AVS2/AVS3）与真正的暂停/恢复。**许可证变更：**预编译
`libffmpeg.so` 现为 **GPL-3.0-or-later** —— 构建为 libdavs2（AVS2，
GPL-2.0-or-later）与 OpenSSL 3.x（Apache-2.0）启用
`--enable-gpl --enable-version3`。1.6.x 仍是最后一个 LGPL-2.1-or-later
构建。

- AVS2（AVS2-P2/IEEE.1857.4，GB/T 33475.2）软件解码，经交叉编译的 davs2
  库 —— CCTV 4K/UHD 广播 TS 流（流类型 0xd2）现在端到端可解码
  （`codec: avs2`）。
- AVS3（AVS3-P2/IEEE.1857.10）软件解码，经交叉编译的 uavs3d 库
  （BSD-2-Clause），arm64 带 NEON 汇编 —— 流类型 0xd4（`codec: avs3`）。
- AVS1（CAVS）仍仅在基准（Jizhun）档次内解码；AVS+ 广播档次（AVS-P16）
  上游 FFmpeg/davs2 不支持。AVS 流始终走软件路径（不存在硬件 MIME）。
- 原生解码器在 `pause()` 时平移步调锚点：`resume()` 从暂停位置继续，而
  不是爆发冲过积压（视频软件渲染器、硬件路径与音频解码器）。
- 新增构建脚本 `tools/ffmpeg-build/build-davs2.sh` 与 `build-uavs3d.sh`；
  FFmpeg 以 `--enable-gpl --enable-version3 --enable-libdavs2
  --enable-libuavs3d` 重新构建。

## 1.6.1（2026-09-29）

包元数据修复（无代码变更）。

- 修正 `oh-package.json5` 中的 `repository`/`homepage`/`bugs`/
  `author.url` URL：先前的 GitHub 地址（owner 有误）不可达，全部链接
  现指向可访问的 Gitee 镜像 `https://gitee.com/wchuanxin/suplayer`。

## 1.6.0（2026-09-26）

硬件能力预检、HDR 显示适配与直播网络加固（以 4K HDR10 HLS 直播源验证：
3840x2160@50 HEVC Main10、双 AAC、DVB 字幕、伪装的 `.php`/`.jpg` URL）。

- 硬件解码能力预检：创建解码器之前经
  `OH_AVCodec_GetCapabilityByCategory`（HARDWARE）查询芯片能力 —— 该
  mime 无硬件解码器、或尺寸/档次超出上报范围（如仅 Main 的解码器遇上
  HEVC Main10），跳过整个硬件尝试直接从软件开始。`hw: 'force'` 下预检
  失败表现为 `start` 错误并附原因。流档次自 FFmpeg `AV_PROFILE_*` 映射
  （H.264 Baseline/Main/High，HEVC Main/Main10/MainStill）；未映射/未知
  档次绝不阻断。帧率元数据仅作参考（记日志，不阻断）。匹配到的硬件
  编解码器名记入日志（`FFVideoHW`）。
- HDR10/HLG 显示：硬件路径上，带 HDR 传递函数（SMPTE 2084 / ARIB std
  B67）的 BT.2020 内容在启动后立即把 surface 色域设为对应的 BT.2100
  变体（`SET_COLOR_GAMUT`），系统合成器从而正确地对 10-bit NV12 帧做
  色调映射。新增 `FFVideoOpenInfo.colorPrimaries` / `colorTransfer` /
  `colorRange` 字段（ffprobe 命名），供应用检测 HDR 内容。
- 新增 `FFNetOptions.httpPersistent`（默认 `true`）：HLS 播放列表刷新
  与分片下载复用一条 HTTP 连接，而非每个分片支付一次新的 TCP+TLS 握手。
- 新增 `FFNetOptions.reconnect`（默认 `true`）：瞬时网络失败 —— 连接
  错误、直播断开、HTTP 4xx/5xx 的分片/播放列表响应 —— 在错误上抛前以
  封顶退避（最大 5s）自动重试。两个选项对非 HTTP 源均为无操作。
- `FFSubtitleDecoder` 暂停现在步调感知：恢复时呈现锚点平移暂停时长，
  字幕不再爆发追赶墙钟。
- 无调色板的 PAL8 字幕区域（罕见、畸形的 DVB 集）现在回退为全透明，
  而不是涂一块实心白。

## 1.5.0（2026-09-25）

多声道音频输出：解码器不再下混为立体声。

- `FFAudioDecoder` 输出跟随源的声道布局（单声道到 7.1，原生顺序）。
  未指定布局采用该声道数的默认布局；自定义/ambisonic 布局与 8 声道
  以上下混为立体声。输出保持 F32LE 交织。
- 新增 `FFAudioOpenInfo.channelLayout`（及 `switched` 事件中）：输出
  声道掩码，FFmpeg `AV_CH_*` 位，数值上与 OHOS
  `audio.AudioChannelLayout` 取值一致，可直接映射到
  `AudioRendererOptions.streamInfo.channelLayout`。
- 音轨热切换保持零重缓冲行为：每条轨道重采样到初始轨道的采样率**和**
  声道布局，输出格式（及应用侧 `AudioRenderer`）在切换间保持不变。
  `switched` 中的 `channels`/`channelLayout` 上报锁定的格式。
- open/switched 信息中的 `channels` 现在上报真实输出声道数（原先在
  `switched` 事件中硬编码为 `2`）。

## 1.4.0（2026-09-24）

DVB 字幕解码，凭容器声明即可开启。

- 新增 `FFSubtitleDecoder`：打开字幕流并以事件形式发出解码的显示集。
  位图字幕（DVB/DVD）被合成为单幅 RGBA8888 位图 —— 全部区域的并集包围
  盒、裁剪到可见（非透明）区域，典型 DVB 页面由整屏收缩为一条小字幕条。
  文本字幕按 UTF-8 文本透传，空显示集上报为 `clear`。
- 流只要被容器*声明*即被接受 —— PMT 里的一条 DVB 字幕表项就够：即使该
  字幕 PID 从不携带一个包，`open` 也成功且解码器保持使能，功能可以
  直接从节目表开启。`FFSubtitleOpenInfo.declared` 上报这一点；在显示
  定义段到来之前，合成尺寸回退到配对的视频尺寸。
- 与其他解码器相同的选项（`FFNetOptions`，含 `program`）、实时步调
  （`start(paced)`）、`pause`/`resume`/`stop`/`close`。
- 真机以直播 DVB 流验证（1920x1080 @50，`dvb_subtitle`，语言 `mul`，
  合成尺寸 1280x720）：一个显示集解码为 (452,544) 处的 377x73 包围盒，
  以 110 KB RGBA 位图交付，随后是 `clear`；另以合成 TS 验证 —— PMT 声明
  了一个从不携带数据的字幕 PID（`declared=true`，无事件、无错误）。
- 流携带事件信息表时，`probe` 还会暴露 FFmpeg 基于 EIT 的 `epg` 数据
  流。

## 1.3.0（2026-09-24）

MPEG-TS 节目信息（PAT/PMT）与多节目（MPTS）播放。

- `probe` 现在暴露 PSI 节目表：`FFMediaInfo.programs` 中新增
  `FFProgramInfo`，含 `programNum`、`pmtPid`、`pcrPid`、`pmtVersion`
  （PMT version_number）、`flags`（bit 0 = `AV_PROGRAM_RUNNING`）、SDT
  的 `serviceName`/`serviceProvider` 及归属各节目的 `streamIndexes`。
  输入不含 PSI 时 `pmtPid`/`pcrPid`/`pmtVersion` 为 `-1`；无节目表的
  容器（MP4）为空。
- `FFAudioDecoder` 现在并行解码源的全部音轨（锁定 `program` 时以节目为
  限），`switchTrack` 是瞬时热切换：只重路由输出，不重开解码器、不重新
  缓冲。每条轨道重采样到初始轨道的采样率，输出速率在切换间恒定。锁定
  节目之外的流仍被拒绝。
- 每条 `FFStreamInfo` 现在携带 `pid`（MPEG-TS 传输 PID，流 id 不是 PID
  的容器为 `-1`）与 `programIndex`（指向 `programs` 的索引，不在任何
  节目中为 `-1`）。
- `FFNetOptions` 新增 `program` 选项（两个解码器的 `open` 均接受）：
  按节目号把解复用锁定到单个 MPEG-TS 节目。其他节目的流在解复用器层面
  丢弃，自动选轨被限制在节目内，显式 `streamIndex` 对照其校验，
  `FFAudioDecoder.switchTrack` 拒绝锁定节目之外的流。以双节目 TS 真机
  验证（480x270/44.1kHz 与 640x360/22.05kHz 两节目）独立播放，两节目
  均激活硬件解码。

## 1.2.0（2026-09-24）

经 AVCodec Kit 的硬件视频解码（surface 模式）。

- `FFNetOptions` 新增 `hw` 选项（`FFVideoDecoder.open` 接受）：`'auto'`
  （默认）优先硬件解码器、失败静默回退软件；`'force'` 强制硬件（否则
  `start` 失败）；`'never'` 始终软件。
- 硬件路径以 surface 模式用 `OH_VideoDecoder` 解码 H.264/HEVC Annex-B
  流（MPEG-TS、HLS-TS），直接渲染到 XComponent surface 并按墙钟步调。
  以 4K HEVC Main10 3840x2160@50（NV12 10-bit 输出、HDR BT.2020 元数据）
  与 1080p50 H.264 直播流真机验证，均达实时帧率。
- MP4 式长度前缀流通过窥探首个视频包（Annex-B 起始码检查）识别，留在
  软件路径；窥探的包被保留，两条路径都不丢失首关键帧。
- 新增 `FFVideoDecoder.on('info', ...)` 事件，解码器启动后一次性上报
  实际后端（`{ backend: 'hw' | 'sw' }`）。

## 1.1.0（2026-09-24）

网络与直播支持。

- 经内置 OpenSSL 3.4.1（Apache-2.0）的 HTTPS/TLS；`src` 现在凡接受文件
  路径之处均接受 URL（`https://`、`http://`、HLS `.m3u8`、HTTP 裸 TS）。
- HLS 直播播放：`liveStartIndex`（从直播边缘加入）、实时的音视频步调、
  直播音轨间无缝 `switchTrack`、经 I/O 中断回调的即时 `stop()`（不等
  网络读取）。
- `probe` 与两个解码器的 `open` 新增 `FFNetOptions`：`userAgent`、
  `headers`、`allowedExtensions`、`liveStartIndex`、`analyzeMs`。
  `allowedExtensions` 还为播放列表/分片 URL 伪装过的流禁用严格的扩展名/
  格式匹配（`.php` 播放列表、`.jpg` 命名的 TS 分片）。
- FFmpeg 诊断转发到 `hilog`（域 `0xFF00`，标签 `FFmpeg`）。

## 1.0.0（2026-09-23）

初始发布。

- `probe`：全部 FFmpeg 支持格式的容器/编解码/流元数据探测，包括带多
  音轨/字幕轨与语言标签的裸 MPEG-TS。
- `extractFrame`：按时间戳提取单帧为 RGBA。
- `FFAudioDecoder`：软件音频解码为 F32LE 立体声，实时步调、多轨输入的
  无缝 `switchTrack`、暂停/恢复。
- `FFVideoDecoder`：软件视频解码，OH_NativeWindow/XComponent Surface
  渲染。
- `transcode`：后台一次性转码/封装管线（解码 → avfilter setpts/scale/
  fps → OpenH264/AAC → MP4/MPEG-TS）。
- `arm64-v8a` 与 `x86_64` 原生库；内置 `libc++_shared.so`。
- 基于 FFmpeg 7.1.1（LGPL-2.1-or-later 构建，无 GPL 组件）与 OpenH264
  2.5.0。
