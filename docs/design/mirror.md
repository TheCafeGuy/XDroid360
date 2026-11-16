# MIRROR-1: game video + audio to a PC over the USB cable

Status: v2 DESIGN, building (2026-11-13). the maintainer approved the subsystem behind a switch, scrcpy as a
comparison baseline and llvm-mingw (2026-11-13 21:01Z, "ok los 3"). v1 (b145692f8) is superseded:
its RefreshGuestOutputImpl hook, abstract socket, ffplay v0, 60 fps encode, 3-image swapchain and
clip-ring audio tap were all wrong (round-1 research, 5 agents).

## Ask (the maintainer, 2026-11-13)
- Game video and audio from the OnePlus 12 to the Windows PC, latency "en su minima expresion".
- Plain USB 3 cable first (Cast "un asco", HDMI capture "un perno", O+ Connect over USB works badly).
- Our own system, better than scrcpy. External software only as a comparison: everything shipped is
  ours (no ffplay/scrcpy in the product).
- Viewer: PC window, resizable, fullscreen, scaling that fits any screen. Smart TV / TV box: bonus.

## Budget (estimates, P0 measures them)
| Stage | 720p | 1080p |
|---|---|---|
| Wake + blit on the mirror thread | 0.1-0.3 ms (GPU 0.05-0.1 ms) | same |
| Surface -> encoder input (vendor RGB->YUV) | 0.5-2 ms | 1-3 ms |
| Encode, AVC CBR low-latency (STAGE_1) | 3-6 ms | 6-12 ms |
| adb reverse over USB 3 (20-40 KB AU) | 0.5-1.5 ms | 1-2 ms |
| Decode, Media Foundation hardware MFT | 2-4 ms | 3-5 ms |
| Present: tearing / vsync / composed | 0-1 / 8-16 / 16-33 ms | same |
Realistic glass-to-glass beyond the phone's own frame: 15-25 ms with tearing, plus the PC panel.

## Threads and data flow
```
GPU Commands thread                         (untouched: no new work, one relaxed load + futex wake)
  presenter PaintAndPresent -> OnPresent -> MirrorOnPresent(): frame counter++, notify
XDroid Mirror (capture)       waits the counter; AcquireGuestOutputForCopy(); blit+scale into the
                              encoder-surface swapchain image; CPU waits the blit fence; present
XDroid Mirror Out             AMediaCodec_dequeueOutputBuffer (100 ms) -> kConfig/kVideo -> video socket
XDroid Mirror Net             connect/handshake/reconnect; audio ring -> kAudio every guest frame
                              (5.33 ms); control messages from both sockets (poll)
Audio Worker (XHostThread)    SubmitFrame -> MirrorOnAudioFrame(): 6ch BE -> 2ch s16 into the SPSC ring
```
Off (`xdroid_mirror = false`, default): the capture thread sleeps on a futex, the net thread checks
the switch and the token every 250 ms, the present hook is one relaxed load.
Bitrate: g_kbps holds the session value (the cvar at connect, then kSetKbps or a cvar change), so a
recreated encoder keeps the viewer's choice. Teardown without quick_exit (boot failure, GPU-BENCH
quit): ~XDroidApp nulls the live graphics system and MirrorDetachGraphics waits until the capture
thread freed its Capture, before the device goes away.

## Capture (phone)
- Second consumer of the guest-output mailbox, like upstream `CaptureGuestOutput` (presenter.h:323,
  "callable from any thread"). New public `VulkanPresenter::AcquireGuestOutputForCopy(image, extent,
  keep_alive)`: ConsumeGuestOutput under the consumer mutex, take the shared_ptr, leave. The
  shared_ptr keeps the image alive across an extent change; queue order plus the barrier chain keep
  the content whole: the mirror blit is either before the next refresh of that slot (the refresh's
  first barrier, src FRAGMENT_SHADER, chains on ours) or after it (RAW, same chain).
- Guest output: VK_FORMAT_A2B10G10R10_UNORM_PACK32, optimal tiling, internal layout
  SHADER_READ_ONLY_OPTIMAL, stage FRAGMENT_SHADER, access SHADER_READ (vulkan_presenter.h:87-92).
  Barriers: internal -> TRANSFER_SRC before the blit; TRANSFER_SRC -> internal after it, dst stage
  FRAGMENT_SHADER. One-time check: BLIT_SRC on A2B10G10R10, BLIT_DST on the swapchain format.
- Path R1 (default): VkSwapchainKHR on the encoder's input surface (`AMediaCodec_createInputSurface`,
  API 26). R8G8B8A8_UNORM, FIFO only (MAILBOX/IMMEDIATE give SURFACE_LOST), usage TRANSFER_DST,
  ~16-18 images expected (codec surface: 16 buffers; ~59-66 MB at 720p; the start log prints the
  real count). vkCmdBlitImage LINEAR scales the
  guest image into a fixed letterboxed encode size: never rebuild on a GSR2 extent toggle.
- Queue rules (graphics_compute queue 0, `AcquireQueue` recursive mutex shared with the GPU thread):
  hold it only around vkQueueSubmit and vkQueuePresentKHR. Acquire (timeout 0, skip the frame if no
  image) and the fence wait happen outside it. The CPU waits the blit fence before the present, so
  the EGL-throttled queueBuffer never waits while the queue lock is held. The present waits a binary
  semaphore that the blit signals (0 wait semaphores give fence -1 and a torn encoder read).
- Acquire timeout 0 is grade C: FIFO on a codec surface may still block inside the driver. The
  first acquire result is logged; P0-9 checks drops > 0 under load and that Cap never blocks > 1 ms.
  After a free image, AcquireGuestOutputForCopy runs again, so the newest output is copied.
- The queue lock is held across vkQueuePresentKHR: the stats line prints `present max` ms (P0-5).
  Teardown drains the queue (vkQueueWaitIdle) before freeing semaphores and swapchain; a blit whose
  fence never signals leaks its fence, pool and semaphores instead of freeing them in use.
- Path R2 (arm, only if P0-3 shows the vendor RGB->YUV costs > 0.2 ms GPU or P0-4 fails): own compute
  RGB->NV12 (BT.709 limited) into a host-visible buffer, codec in ByteBuffer mode (color-format 21).
- Rule 8: no lock in a watchdog region; the mirror thread never runs inside one. The stall watchdog is
  120 ms; one blit is far below it.

## Encoder (c2.qti.avc.encoder, NDK AMediaCodec, libmediandk)
- video/avc (arm hevc), 1280x720 (cvar height 720/1080), 16 Mbps CBR (`bitrate-mode` 2: sets
  LOWLATENCY_MODE and STAGE_1; the VBR default runs STAGE_2), frame-rate 30, operating-rate 30,
  priority 0, latency 1, max-bframes 0, i-frame-interval `xdroid_mirror_key_interval` (10 s), intra-refresh-period 30 (no IDR bursts:
  VBV 200-300 ms, an IDR at 16 Mbps is ~600 KB), prepend-sps-pps-to-idr-frames 1,
  repeat-previous-frame-after 100000 us, BT.709 limited (color-standard 1, range 2, transfer 3),
  profile High. Never i-frame-interval 0 (forces STAGE_2). STAGE_1/STAGE_2 and the IDR size are
  unverified: P0-1 reads them from the logged encoder input/output formats and the stats line
  (largest frame KB, keys per 60 s).
- Output thread: sync dequeueOutputBuffer, 100 ms timeout. Never drop an encoded frame: when the
  viewer lags, drop before encode (skip the capture) and lower `video-bitrate` via setParameters.
  `request-sync` only on connect, on kRequestKey or after a real loss.
- PTS: input-surface timestamp (CLOCK_MONOTONIC at queueBuffer). aux = dequeue time: aux - pts =
  present-to-output (queueBuffer to encoded AU: codec queue wait + encode), shown by the viewer and
  logged as `present-to-output`; P0-2 measures pure encode on a synthetic stream.

## Audio (phone)
- New lock-free SPSC ring (s16 stereo, 64-bit write cursor, one CLOCK_MONOTONIC stamp per guest
  frame), written in `XDroidAAudioDriver::SubmitFrame` (arm A): 6ch x 256 BE -> 2ch LE, volume,
  clamp, s16, release store. No lock, no allocation, no syscall. The clip ring is not reusable
  (overrun, data race, mutex held across WAV IO, Render lags submit by the guest queue, 85-341 ms).
- Arm B (later): the same ring at the Render clip site = the exact phone output, in 40 ms bursts.
- Reader (net thread): clamps on lag (kFlagLoss) and re-checks the write cursor after the copy.

## Transport and protocol
- `adb reverse tcp:P tcp:P`: the viewer listens on the PC's 127.0.0.1:P, the app connects to its own
  127.0.0.1:P with TCP_NODELAY. Two connections: video, audio + control (no audio behind a large AU).
- Wire format v2: `xdroid-core/src/main/cpp/xdroid_mirror_proto.h` (shared by both ends). Two random
  16-byte keys, never on the wire: ServerHello (viewer nonce), ClientHello (app nonce + SipHash-2-4-128
  MAC with the client key), ServerProof (MAC with the server key; the app sends no frame before it).
  A phone app that connects to or squats the port learns nothing replayable, so the viewer keeps its
  keys on a failed handshake. Packet header 24 B; control 16 B (ping, request key, set kbps).
- Start: the viewer reads live.toml (`run-as ... cat`), drops only its own three keys (A/B arms such as `xdroid_mirror_kbps` stay), puts
  `xdroid_mirror_port`, `xdroid_mirror_token` (64 hex: server + client) and `xdroid_mirror = true` at
  the top and writes it back (tmp + mv), so other live keys stay. toml++ tables are sorted, so
  `xdroid_mirror` applies before port and token: the net thread re-reads both on every connect try
  (1 s after a failure) and a stale try just fails the handshake. The app deletes live.toml when it
  starts, so the viewer writes it again when the app restarts (no connection for a few seconds).
  `phone.sh live` rewrites the same file: never run both within the same 3 s.
  Stop: `xdroid_mirror = false`, empty token, `adb reverse --remove`. run-as needs a debuggable APK.
- Public APK (no run-as, viewer v0.3.0): when the run-as publish fails, the viewer keeps `adb reverse`
  and reads `/sdcard/Android/data/io.github.thecafeguy.xdroid360/files/xdroid/mirror_token` with
  `adb shell cat` every 1 s while no stream runs (trimmed, exactly 64 lowercase hex = server + client
  keys, else only its own unpublished keys pass). The in-game switch "PC mirror (experimental)"
  writes a new token on every switch-on, sets the live cvars itself (port 27360) and deletes the file
  on switch-off. No token: window and console say to turn the switch on. Either order works; at exit
  the viewer removes only the reverse (the switch owns the cvars).
- Connect order: video first, then audio; the app serves a viewer only when both handshakes pass.
  kPong goes on the audio socket whichever socket carried the kPing (no pong behind a large AU).
- LAN / internet / TV: NOT approved yet (security: needs the maintainer's separate OK). Planned only through a
  user VPN or a paired TLS link; the protocol is transport-agnostic.

## Viewer (PC): xdroid-viewer/, Win32 + D3D11 + Media Foundation + WASAPI, MinGW-w64
- Net thread per connection (TCP_NODELAY), handshake, packet parse.
- Decode: MFTEnumEx hardware H.264/HEVC decoder first, else CLSID_CMSH264DecoderMFT; D3D11 device
  manager; CODECAPI_AVLowLatencyMode = 1 and MF_LOW_LATENCY (VT_UI4 1); NV12 output. P0 check: one
  AU in gives one frame out (no decode hold); if the MFT holds frames, rewrite the SPS VUI
  (bitstream_restriction, max_dec_frame_buffering 0). Decode every AU into an 8-slot ring. Tear mode presents the newest; vsync paces (off with the
  launch option --unpaced, 942ae8805): a picture is due once the present deadline passes its phone pts
  plus the max arrival delay of the last 1-2 s (absolute schedule, quarter-refresh hysteresis,
  adaptive present guard), so 30 fps on 60 Hz holds 2,2,2 refreshes, not 1,3.
- Mid picture (2026-11-15; on by default again in v0.3.0, off with --no-interp; --interp is a no-op): paced, a picture 2 refreshes after the one on
  screen (30 fps on 60 Hz) first goes out as the picture halfway between them, and itself one
  refresh later: 60 pictures a second for +1 refresh (16.7 ms) on every real one. The phone gives
  only real frames (the OnePlus Game Manager interpolates on its own display, never in the
  stream), and frame generation in the emulator is refused, so the PC makes them. Four compute
  passes (render.cc): 1/4 and 1/8 res luma; per 32x32 region every motion up to 64 texels at 1/8
  res; per 8x8 block rest and the region's and 2 neighbors' motions a texel around at 1/4 res,
  then a texel around at full res (each level matches within half a texel); per texel the best of
  rest, its block's and 2 neighbors' motions on its quad's 4x4 window plus 8x its own match. The
  match is symmetric (A at p - d, B at p + d), so the mid picture has no holes; a still HUD keeps
  its edges. Any other gap (60 fps content, a missing picture) shows only real pictures. Selftest
  "mid picture": a 1280x720 noise plane moved (12, 6) under a still random HUD: error 0.4 of 255
  (plain blend 29.5), HUD 0.1, 3.6 ms per mid picture on Iris Xe (upper bound, 4 dispatches).
  viewer-frames.csv column `mid` = 1 for a real picture that followed its mid picture.
- Render: fullscreen triangle NV12 -> RGB (BT.709 limited) + scaling: fit (letterbox), fill (crop),
  integer; filters bilinear / Lanczos-2. HLSL compiled at runtime by the inbox
  d3dcompiler_47.dll.
- Swapchain FLIP_DISCARD, 2 buffers, ALLOW_TEARING | FRAME_LATENCY_WAITABLE_OBJECT, max latency 1.
  Present modes: vsync paced (default), tear (fullscreen with --no-interp, when tearing is
  supported), vsync.
- Window: per-monitor DPI v2, WM_DPICHANGED, borderless fullscreen on F11 / Alt+Enter / double-click,
  keeps aspect, any monitor. F1 overlay: fps, encode / transfer / decode / present ms, RTT, kbps,
  audio buffer ms, drops.
- Audio: IAudioClient3 at the engine's minimum period (fallback IAudioClient 10 ms), MMCSS "Pro
  Audio"; adaptive jitter target clamp(p99 arrival jitter + period, 8, 60 ms); own resampler with a
  PI loop within +-0.5 % to hold the target; fade-repeat on underrun, crossfaded drop on overflow.
- Clock: kPing every 500 ms; offset = phone_t - (pc_t0 + rtt/2) from the lowest-RTT sample.
- `--probe`: no window, no sound: DXGI tearing/overlay caps, WASAPI periods, MFT decode latency on
  ffmpeg-made test streams. Flags: `--no-adb`, `--adb PATH`, `--serial S`, `--port P` (27360), `--unpaced`,
  `--audio-low-latency`, `--no-autotune`, `--no-interp`. Without `--no-adb` the viewer runs adb reverse +
  run-as (or the mirror_token read) itself.

## Cvars (phone, all `xdroid_mirror*`)
All of them sit in kLiveCvars: live.toml must be able to set them (ApplyTable ignores a cvar outside
the list). "Applies" says when the threads read the value. `xdroid_mirror_token` is not a cvar:
ApplyTable hands it to MirrorSetToken before the cvar lookup, so the secret is never logged (the log
says only "token set", "cleared" or "INVALID").
| Cvar | Default | Applies | Meaning |
|---|---|---|---|
| xdroid_mirror | false | now | master switch |
| xdroid_mirror_port | 27360 | next connect | adb reverse port |
| xdroid_mirror_token (special key) | none | next connect | 64 hex: server + client key |
| xdroid_mirror_height | 720 | next connect | encode height (16:9 width) |
| xdroid_mirror_kbps | 16000 | now | video bitrate |
| xdroid_mirror_codec | 1 | next connect | 1 H.264 (avc), 2 HEVC |
| xdroid_mirror_bitrate_mode | 2 | next connect | 2 CBR, 1 VBR, 3 CBR_FD (0 CQ refused) |
| xdroid_mirror_intra_refresh | 30 | next connect | frames; 0 = IDR only |
| xdroid_mirror_key_interval | 10 | next connect | seconds between key frames (1 to 60) |
| xdroid_mirror_input | 1 | now (encoder restarts) | 1 own NEON NV12 into the encoder surface (falls back to 0), 0 RGBA swapchain (vendor GPU convert) |
| xdroid_mirror_keys | vendor.qti-ext-enc-low-latency.enable=1 | now (encoder restarts) | extra encoder keys "name=value,..."; other encoders ignore the default; "" = off |
| xdroid_mirror_audio | true | now | SubmitFrame tap (first audio client after arming; another
  takes over after 100 ms of silence, e.g. a new driver) |

## P0 probes (PASS thresholds)
Phone (when adb is free): P0-1 encoder capability dump (CBR, intra refresh, vendor keys); P0-2
synthetic encode 720p CBR p50 <= 6 ms, p95 <= 10 ms; P0-3 R1 GPU cost +<= 0.2 ms; P0-4 PSNR >= 35 dB;
P0-5 game frame time unchanged, present hook <= 0.1 ms; P0-6 socket path works under SELinux; P0-7
ping RTT p95 <= 3 ms; P0-8 glass-to-glass vs scrcpy v5.0.1 (phone timer filmed with the PC screen).
Details: P0-1 PASS = input format shows the CBR / low-latency keys taken, largest frame <= 2x the
mean AU outside key frames. P0-2 only if present-to-output p50 > 6 ms. P0-8: 240 fps camera on the
phone and the PC screen side by side, >= 30 marks, scrcpy v5.0.1 `--video-codec=h264
--video-bit-rate=16M --max-fps=30 --no-audio-playback`; PASS = tear median >= 10 ms below scrcpy.
P0-9 acquire: drops > 0 when the encoder lags and Cap never blocks > 1 ms. P0-10 energy: A/B/A
mirror on/off over wireless adb, phone unplugged, same scene; possible arm xdroid_mirror_audio_ms
(default 4). Open: the viewer's accept thread waits up to 500 ms per handshake, so a local process
that connects in a loop delays the app's connect (accepted; the MAC still holds).
PC: DXGI caps, WASAPI minimum period, MFT latency (`xdroid-viewer --probe`).

## Status (2026-11-14 21:40Z)
- Live test 1 (mission 6, phone c46faa625): picture 10/10, true to the phone (after viewer fix
  e31eb69e4). Smoothness poor, felt about 20 fps.
- Live test 2 (same phone build): "audio duplicado, super cortado, barbaro stuttering", the phone
  still plays and shows. Answers, all local and none seen live yet:
  - Choppy PC audio: guest audio comes in bursts (gaps 40 ms, often 64 to 87). Viewer safe mode
    8ecf02149 (target = windowed max lateness, about 86 ms; launch option --audio-low-latency = old). Simulated underruns 98 -> 1.
  - Phone still plays and shows: fdf00d327 xdroid_mirror_mute_local + 37d629ec8
    xdroid_mirror_blank_local. Need the deploy.
  - Stutter: 73% of the phone to screen delay and most of its jitter sit between the present and the
    encoder output (p50 18.9, p95 71.0 ms; about 25% of the pictures missing at 60 fps). The encoder
    ran at operating-rate 30. 85aeacd2d: rate 120, cvar xdroid_mirror_operating_rate (deployed);
    expected encode 4 to 5 ms.
  - Auto-tune (the maintainer 2026-11-14: the F4/F6/F7/F8 comparison and its toasts confuse; the best setup
    must win by itself). `xdroid-viewer/tune.h`: the viewer trials encoder rates 30/60/120/240 live,
    3 rounds of 6 s windows (first 2 s dropped), 72 s. Window score in ms = mean delay + 6 x (mean
    cadence error + mean picture interval above the round's lowest); arm = median of its rounds;
    the cheapest rate within 2 ms of the best wins. Result: `# tune` line in viewer-frames.csv,
    stderr, F1 overlay. Keys F4, F6, F7, F8 and the NEW/OLD toasts are gone. Switches are launch
    options, never stored: --no-autotune, --unpaced, --audio-low-latency (2026-11-15: the values
    F6 / F7 had stored in the registry stayed on with no key left: 1520 underruns in 124 s).
    Not trialled: audio (safe wins 1 vs 98 underruns) and present mode (the score cannot see tear
    lines). No persistence yet: one tuning per run, restarted if fullscreen toggles before it ends.
- Next: the maintainer plays 2 min or more with the viewer open (nothing to press), then
  `python xdroid-viewer/stutter.py` (per encoder rate + the tune line).
  Open: 0.5 to 1.4 s encoder stalls (p99), pacing bypassed with tearing (fullscreen default),
  energy of rate 120, low-latency AAudio while mirroring. Details: `docs/design/mirror-handoff.md`.

## Upstream touch
One public method in `src/xenia/ui/vulkan/vulkan_presenter.{h,cc}` (AcquireGuestOutputForCopy, ~15
lines). Everything else in xdroid-core/ (xdroid_mirror.{h,cc}, xdroid_mirror_proto.h, hooks in
xdroid_latency.cc, xdroid_audio_driver.cc, xdroid_live.cc, CMake: mediandk) and xdroid-viewer/.
