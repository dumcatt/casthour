# Casthour

Streams beatmania IIDX live to OBS, or to a file, with the game's own audio. It covers every screen: menus, demo play, songs and results. The game's built-in play recorder keeps working unchanged.

- **Video:** H.264 from your NVIDIA GPU's NVENC.
- **Audio:** AAC, tapped from the game's ASIO or WASAPI output inside the game process.
- **Delivery:** MPEG-TS over TCP, UDP or a local file. OBS plays it with a plain Media Source, no plugin needed.
- **Clean frames:** while the game's recorder runs, the stream uses its picture, which leaves out CREDITS, the arcade name and leaderboard names. Elsewhere, you configure your own masks.
- **Game audio timing is untouched:** buffer size, latency and offset stay as they were.

Tested with spice2x on 33, 34 (LDJ, TDJ mode (010 DLL), 1080p @ 120 Hz) and an ASUS Xonar U7 MKII (ASIO).

## Install

1. Build it (see [Build](#build)), or use a built `dist\` folder.
2. Copy `nvEncodeAPI64.dll` and `nvenc-proxy.ini` into the game's `contents\modules\` folder. spice2x reports it in its log as *found user-supplied nvEncodeAPI64.dll in modules directory*.
3. Start the game as usual. No launcher options are needed: the game loads the DLL itself.

## OBS

On the PC running OBS, add a **Media Source**, untick **Local File**, and set:

| Setting | Value |
|---|---|
| Input | `tcp://<game PC IP>:9910` |
| Input Format | `mpegts` |
| Reconnect Delay | 1–2 s |
| Network Buffering | 0–1 MB |
| Use hardware decoding when available | on |
| Restart playback when source becomes active | off |

The game's audio arrives through the same source. Enable *Audio Monitoring* on it if you want to hear it on the OBS PC.

On the game PC, allow the port through Windows Firewall once, from an administrator PowerShell. Do this before starting the game, because Windows' prompt can appear behind the fullscreen game.

```powershell
New-NetFirewallRule -DisplayName "nvenc-proxy stream" -Direction Inbound -Protocol TCP -LocalPort 9910 -Action Allow -Profile Private
```

Tips:
- Use a wired connection.
- Give the game PC a DHCP reservation so the URL doesn't change.
- If the PCs aren't on the same network, Tailscale or ZeroTier addresses work too.
- To check the connection, run `Test-NetConnection <game PC IP> -Port 9910` from the OBS PC while the game is running.
- Nothing is captured or encoded until a viewer connects (or a file/UDP output is configured).

Alternatives:
- **UDP push:** set `udp_target=<OBS PC IP>:9911` in the ini and use `udp://@:9911` as the OBS input. It's lossy on a poor network and forces a keyframe every 2 s.
- **Local file:** `file=casthour-{time}.ts` records the continuous stream across every song.

## Configuration

Everything lives in [`nvenc-proxy.ini`](nvenc-proxy.ini) next to the DLL; each option is commented there.

| Section | What it controls |
|---|---|
| `[proxy]` | `mode=live` (default) or `recorder` (only forward what the game's recorder writes to its MP4), the real NVENC DLL, logging |
| `[output]` | TCP listen address, UDP target, local file |
| `[capture]` | fps (60), resolution (default: the game's), bitrate (12000 kbps CBR), GOP, whether to use the recorder's clean frame, mask preview |
| `[audio]` | ASIO/WASAPI taps, AAC bitrate, `offset_ms` to nudge audio against video in the stream (never the game) |
| `[masks]` | Rectangles hidden while the screen is streamed |

### Masks

While a song is being recorded, the stream uses the recorder's own clean frame. On every other screen (menus, demo play), it uses what's on screen, with your `[masks]` applied:

```ini
[masks]
; name=x,y,width,height[,pixelate]   in 1920x1080 coordinates
credits=760,1030,400,50
arcade_name=0,0,480,60,pixelate
```

Set `mask_preview=1` to draw the masks as green outlines in the stream. Adjust them while watching OBS, then set it back to 0.

## Troubleshooting

`modules\nvenc-proxy.log` shows what the proxy is doing. A healthy session looks like this:

```
mode: live (own capture of every frame)
WASAPI render hooked
ASIO driver 'Xonar U7 MKII(64)' hooked
SwapChain::Present: hooked in code
hooks installed: audio 98 ms, video 9833 ms
ASIO buffers: 176 frames @ 44100 Hz, 2 output ch tapped ..., tap ready
viewer 192.168.1.20:50887 connected
audio source: ASIO
encoder: 1920x1080 @ 60 fps, 12000 kbps CBR, GOP 120, async
capture: following game encoder ... (1920x1080)
capture: source is now the game's recorder frame
video: repeated the last frame 88 times while the game wasn't presenting
ASIO tap: 2506 callbacks, game callback avg 9.6 us, our copy avg 1.70 us (max 50.4 us)
```

| Symptom | Look for |
|---|---|
| No picture in OBS | `listening for viewers`, then `viewer ... connected`. If OBS never connects, check the firewall and IP. |
| No audio | `ASIO buffers: ... tap ready` or `WASAPI stream: ...`, then `audio source: ...`. After 30 s without audio, an `audio watchdog` report lists what it can see. |
| Game crashes at startup with `nhAsus…OSD.dll` | That's Nahimic's overlay, which ships with ASUS audio drivers. The proxy hooks around it. If a log line says `vtable hook (may conflict with overlays such as Nahimic)`, report it. |
| Audio slightly off in OBS | Adjust `offset_ms` (positive = later). |

## How it works

`bm2dx.dll` imports `nvEncodeAPI64.dll` statically, so a DLL with that name in `modules\` is loaded before the game creates its D3D9 device or opens its audio. Every NVENC call is forwarded to the NVIDIA driver's own DLL, and the game's play recorder works as before.

**Video** ([src/capture.cpp](src/capture.cpp))
- **Present hooks:** `SwapChain::Present`, `Present`/`PresentEx` and `Reset`/`ResetEx` are hooked in `d3d9.dll`'s code. The hooks are located through throwaway devices, so it doesn't matter when or through which wrapper (spice2x) the game creates its device.
- **Vtables are left alone:** Nahimic's OSD loops forever when a vtable slot doesn't point into `d3d9.dll`. An entry another hook already redirected is chained; a plain prologue gets a trampoline ([src/common.cpp](src/common.cpp) `code_hook`).
- **Capture:** at the target fps, swapchain 0's back buffer is copied on the GPU into our own NVENC session: H.264 CBR, low-latency preset, no B-frames, keyframe every 2 s, plus one whenever a viewer connects.
- **The game's recorder frame:** the game runs two recorder encoders per song. The largest one's input surface is streamed while it records; otherwise the screen is streamed with masks applied.
- **Stale inputs:** when the game destroys its NVENC sessions at the end of a song, the driver starts handing our session stale copies of its inputs. The proxy re-registers them.
- **Loading screens:** while the game isn't presenting, the last frame is re-encoded at the target fps, so OBS gets a steady stream and keeps audio in sync.

**Audio** ([src/audio.cpp](src/audio.cpp))
- **Hook timing:** hooks are installed about 100 ms after the DLL loads, well before the game opens audio.
- **ASIO:** the driver's `createBuffers` is hooked. The driver is given wrapper callbacks: each one calls the game's `bufferSwitch` first, unchanged, then copies the half-buffer the game just filled into a lock-free ring. Buffer size, sample rate, latency reports, `getSamplePosition` and `outputReady` pass through untouched. The log reports the copy's cost; it measured ~1.5 µs per 4 ms callback.
- **Finding the game's driver:** `CoCreateInstance` is hooked beneath spice2x, and the driver is pinned so COM can't unload it.
- **WASAPI:** `IAudioRenderClient::ReleaseBuffer` copies what the game just wrote. Streams opened before the hook are adopted with the shared-mode mix format.
- **Encoding and timing:** AAC is encoded on our own thread. Timestamps follow the device's sample clock plus its reported output latency. A measured ~25 ms encode/decode skew is corrected, which puts audio within about 3 ms of video.

**Output** ([src/output.cpp](src/output.cpp), [src/tsmux.cpp](src/tsmux.cpp))
- A minimal MPEG-TS muxer runs on a worker thread, so the game's threads never wait on the network.
- Viewers always start on a keyframe.
- A viewer that falls 64 MB behind is dropped.
- Worker threads run above normal priority, because spice2x runs the game at realtime priority.

**Recorder mode** ([src/mfhook.cpp](src/mfhook.cpp)): `MFCreateMPEG4MediaSink` is redirected, and every compressed sample the game hands its MP4 writer is forwarded unchanged. Only the largest recording is used.

## Build

Requires the Visual Studio C++ build tools (2022 or later).

```bat
build.bat          :: dist\nvEncodeAPI64.dll + dist\nvenc-proxy.ini
build.bat test     :: also dist\harness.exe
```

### Test harness

[`test/harness.cpp`](test/harness.cpp) behaves like the game, so changes can be verified without it:
- It presents at 120 Hz with a red "credits" box on screen.
- It runs two recorder encoders, one of them small and solid green, which must never appear in the stream.
- It stalls presentation for 1.5 s, like a loading screen.
- It plays a quiet tone (about -60 dB) through WASAPI or the first ASIO driver.
- It flashes the screen while scheduling an audio tick for the same instant, to measure A/V sync.

```bat
dist\harness.exe dist\nvEncodeAPI64.dll out\test asio 3 3 300
```

Arguments: `<dll> <output prefix> <none|wasapi|wasapi-early|asio> [menu seconds] [record seconds] [start delay ms]`. Set `HARNESS_ASIO_RATE=44100` to ask the ASIO driver for that sample rate. Point the ini's `file=` at a `.ts` and inspect it with `ffprobe`/`ffmpeg`.

## Layout

```
src/proxy.cpp      exports, NVENC forwarding, game recorder tracking, startup
src/capture.cpp    D3D9 hooks, our NVENC encoder, masks, frame repeat
src/audio.cpp      ASIO / WASAPI taps, AAC encoder
src/output.cpp     sessions, TCP / UDP / file outputs
src/tsmux.cpp      MPEG-TS muxer
src/mfhook.cpp     recorder mode (MP4 sink tap)
src/common.cpp     config, log, vtable / IAT / code hooks
test/harness.cpp   game stand-in for testing
```

Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
