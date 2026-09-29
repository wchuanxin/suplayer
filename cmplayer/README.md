# cmplayer

English | [中文](README_zh.md)

Player UI component library on top of the [`@suplayer/ffmpeg`](../ffmpeg/README.md)
media kernel: an AV-sync player component (`SupPlayer`) and its controller
(`SupPlayerController`), ready to drop into any page.

## Features

- **AV-sync playback** — parallel video and audio decoders with the audio
  stream as the master clock; position is tracked from packet PTS, and a
  periodic drift correction freezes whichever stream runs ahead instead of
  dropping or bursting frames.
- **Pause / resume that actually pauses** — the kernel shifts its pacing
  anchors on pause, so resuming continues where playback left off instead of
  bursting through the backlog; the `AudioRenderer` is paused along with both
  decoders.
- **Hardware decode with fallback** — `hw: 'auto' | 'force' | 'never'` is
  forwarded to the kernel; the backend actually in use (`hw`/`sw`) is shown
  in the UI once reported.
- **Audio track switching** — the track list comes from a background `probe`;
  `switchTrack` re-routes the kernel's parallel audio decoders with zero gap.
- **Surface-ready handling** — playback starts when both the `src` prop and
  the XComponent surface are ready, in either order; changing `src` re-opens
  the player automatically.

## Install

The package is not yet published; reference it locally:

```json5
// oh-package.json5
"dependencies": {
  "cmplayer": "file:../cmplayer"
}
```

`cmplayer` itself depends on `@suplayer/ffmpeg` (`file:../ffmpeg` inside the
workspace — change it to a published version, e.g. `^1.7.0`, when consuming
from outside).

## Quick start

```ts
import { SupPlayer } from 'cmplayer';

@Entry
@Component
struct Index {
  @State src: string = '';

  build() {
    Column({ space: 12 }) {
      TextInput({ placeholder: 'Media file path or HLS/TS URL' })
        .onChange((v: string) => { this.src = v; })
      if (this.src.length > 0) {
        SupPlayer({ src: this.src, hwMode: 'auto' })
      }
    }
    .width('100%').height('100%').padding(16)
  }
}
```

`MainPage` is a ready-made page wrapping exactly this flow.

## API

### `SupPlayer` (component)

| Prop | Type | Description |
|---|---|---|
| `src` | `string` | Media source (file path or URL); watched — changing it re-opens playback |
| `hwMode` | `SupHwMode` | Video decode backend selection, default `'auto'` |

Renders a 220vp `XComponent` video surface, play/pause/resume/stop controls,
a progress readout, the active decode backend, the audio track chips and an
error text. All UI state mirrors the controller every 500 ms.

### `SupPlayerController`

```ts
open(src: string, options: SupPlayerOptions): void   // stops anything playing, then opens
pause(): void; resume(): void; stop(): void
switchTrack(streamIndex: number): void               // zero-gap audio track switch
release(): void                                      // stop + release renderer/decoders
```

Observable state:

| Field | Description |
|---|---|
| `playing` / `paused` | Session state |
| `positionMs` / `durationMs` | Playback position (master clock) and duration |
| `backend` | `'hw'` \| `'sw'` \| `''` — actual video backend once reported |
| `tracks` / `currentTrack` | `SupTrackInfo[]` from the background probe and the active track |
| `errorMessage` | Last playback error (displayed by `SupPlayer`) |

### `SupPlayerOptions`

| Field | Type | Description |
|---|---|---|
| `surfaceId` | `string` | XComponent surface id the video decoder renders into |
| `hw` | `SupHwMode?` | Decode backend selection, default `'auto'` |
| `net` | `FFNetOptions?` | Network/live options forwarded to both decoders and the track probe |

`SupTrackInfo` carries `index`, `codec`, `language`, `sampleRate`, `channels`.

## Permissions

Network sources require `ohos.permission.INTERNET` in the consuming HAP's
`module.json5` (see [ffmpeg/README.md](../ffmpeg/README.md#permissions)).

## License

LGPL-2.1-or-later (project code, see [../LICENSE](../LICENSE)). The bundled
dependency `@suplayer/ffmpeg` distributes its native `libffmpeg.so` under
GPL-3.0-or-later — see
[../ffmpeg/README.md#license](../ffmpeg/README.md#license).
