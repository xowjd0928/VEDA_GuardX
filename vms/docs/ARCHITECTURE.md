# GuardX VMS — Architecture

**Last updated:** 2026-08-04 (startup stagger · explicit software chain · sync outlier guard — §4b)
**Scope:** the Qt6 VMS client (`VMS/`). The prediction backend (RPi B poller → Postgres →
forecaster) is a separate system; the VMS only *consumes* its detection metadata.

---

## 1. Guiding principle

> **Match each component to what it is good at, and let stream *role* — live-grid vs
> fullscreen vs (future) record — drive the choices, not one global setting.**

Every decision below traces back to a measurement in
[VMS_LATENCY_REPORT_2026-07-21.md](VMS_LATENCY_REPORT_2026-07-21.md) and
[LATENCY_WINS.md](LATENCY_WINS.md). Nothing here is aspirational — it is the shape the
code already has, plus the two extensions worth adding next (§6).

---

## 2. The per-channel pipeline

Each of the 4 channels is a fully independent GStreamer pipeline feeding one GPU-rendered
widget. Isolation is deliberate: one stalled channel cannot stall the others.

```
        ┌──────────────────────── camera (Hanwha, per sensor) ────────────────────────┐
        │  H.264 · no B-frames · WiseStream/DynamicGOV/DynamicFPS off (tuner enforces   │
        │  only these; bitrate-mode/GOV are the user's — 2026-07-23 policy)             │
        │  sub-stream  profile4  800×448@30  (grid AND fullscreen since factory reset)  │
        │  main-stream profile2 2592×1520@30 (4 MP — NOT 1080p; 4-ch grid collapses,    │
        │                                     LATENCY_WINS §8.3 — do not wall it)       │
        └───────────────┬──────────────────────────────────────────────────────────────┘
                        │  RTSP / UDP · latency=0 · drop-on-latency · no retransmission
                        ▼
   ┌── rtspsrc ──┬─► rtph264depay ─► h264parse ─► DECODER ─► videoconvert ─► NV12 ─┐
   │  (RTP+RTCP) │        │                         │            (→CPU only if       │
   │             │   [future: record tap           │             sw; else on GPU)   │
   │  RTCP SR    │    tees ENCODED h264 here]       │                               │
   │  → NTP ts   │                                  │                               │
   │  per buffer │              DECODER =           │                               │
   └──────┬──────┘         software (avdec_h264)    │                               │
          │                  = 128 ms, CPU-bound    │        appsink                │
          │                d3d11/d3d12 (Quick Sync)  │        sync=false             │
          │                  = 528 ms, +~400 ms      │        max-buffers = 1 | 24   │
          ▼                  queue, GPU-bound        │        drop=true              │
    ChannelSync  ◄───────────────────────────────────┘            │  (streaming thread)
    (optional: hold fast                                          ▼
     tiles to a common                                       FrameQueue
     presentation deadline)                            (mailbox: newest-frame-wins;
          │                                             depth 1 if sync off, else ~24)
          └──────────────────────────────────────────────────►   │
                                                                  ▼  (UI thread)
                                            QRhiWidget ── D3D11 ── NV12→RGB shader + scale
                                                  │
                                            BoxOverlay (transparent sibling;
                                             repaints only when a box moves)
```

**Startup (2026-08-04):** the four channels no longer connect simultaneously — they are
staggered 250 ms apart (`live_viewer.cpp`). Four concurrent RTSP SETUPs, each with a
2-round-trip digest handshake, hammered the camera's control channel at boot and was one
cause of first-connect failures. The last channel is up by +750 ms; no perceptible cost.

**Decoder segment by preset:** `software` is an **explicit chain**
(`rtph264depay ! h264parse ! avdec_h264`) since 2026-08-04 — `decodebin` had a
stream-race with this camera's ONVIF-metadata substream (pt 107): when it grabbed the
metadata pad first the pipeline died not-linked and the backoff loop masked it as a random
first-connect failure. The explicit chain only accepts the video pad. `decodebin` remains
available as `decoder=auto` (the escape hatch for H.265 profiles, which the explicit
H.264 chain cannot decode).

**Threading:** GStreamer runs each pipeline's receive/decode on its own threads. The
`appsink` callback (streaming thread) drops the frame into the per-channel `FrameQueue`
under a short mutex and posts one coalesced "update" to the UI thread. The UI thread only
ever *paints* — it never blocks on network or decode. This is why a dead channel (e.g. the
camera refusing a 5th session) leaves the others untouched.

**Files:** [gstreamer_backend.cpp](../gstreamer_backend.cpp) ·
[frame_queue.h](../frame_queue.h) · [rhi_video_widget.cpp](../rhi_video_widget.cpp) ·
[channel_sync.cpp](../channel_sync.cpp) · [channel_view.cpp](../channel_view.cpp)

### 2b. Direct render path — the field default since 2026-08-04

`video_backend=direct` selects [direct_sink_backend.cpp](../direct_sink_backend.cpp):
the pipeline ends in **`d3d11videosink`** which renders straight into the channel
widget's native HWND (`gst_video_overlay_set_window_handle`) — no appsink, no
FrameQueue, no QRhi upload. Boxes/blur/badges are NOT a Qt overlay widget (airspace
forbids it over a native window): `ChannelView::paint_chrome()` — the single source of
truth shared with `BoxOverlay` — is rendered into a QImage and attached per-buffer as a
**`GstVideoOverlayComposition` meta** in a sink-pad probe; the sink composites it on
the GPU. Measured gain vs the appsink path: arrival→submit 12 ms → ~2 ms; the present
tail (~55 ms) is identical. Trade-offs: no ChannelSync (the sink owns display timing),
single glass-to-sink metric instead of the stage breakdown, no pixel access.
Field config with it: grid=fullscreen=**profile2 (4 MP)** + `software` decoder + TCP —
same-URL guard in `play()` makes grid↔fullscreen switches instant (no stream swap),
and a `setsynchronizationpoint` request on every "연결 중…" event cuts the GOV=60
first-keyframe wait on (re)connects to ~0.2 s. The appsink path stays intact as
`video_backend=gstreamer` for alignment/instrumentation work.

---

## 3. Decisions & rationale

| Layer | Decision | Why (measured) |
|---|---|---|
| **Transport** | UDP, `latency=0`, `drop-on-latency=true` | TCP turns loss into an unbounded backlog on any jitter; UDP+drop stays live. Wired loss ≈ 0. |
| **Decoder** | **`d3d12-flex` default (low-latency HW), per-profile override** | `compliance=flexible` gives 298 ms @ 1.7 % CPU on the GPU — matches software's latency without the CPU cost. Software is the fallback. Per-profile (§4). |
| **Render** | GPU `QRhiWidget`, NV12→RGB in a shader | keeps CPU flat as resolution/channels grow; overlay composits on top of the GPU surface |
| **Frame handoff** | 1-slot mailbox (drop-oldest); grows only for sync | never accumulate latency; the golden rule of live video is *drop, don't queue* |
| **Channel sync** | Optional; **OFF = lowest latency** (measured 2026-08-04: queue hold 52→11 ms, TOTAL −50 ms — `run.ps1 flex-nosync`) + **outlier guard** (§4b) | sync *adds* a display hold to align tiles to the slowest — a feature, not free. A pathological channel no longer drags the wall: beyond fastest+150 ms it is excluded and shown unsynced. Depends on RTCP (→ needs UDP-RTCP working, i.e. no Tailscale). |
| **Recovery** | bus-watch (ERROR/EOS) + no-frame watchdog + latency-creep guard + exponential backoff + visible "연결 중…" | silent black tiles were the worst failure mode; every failure now self-heals and is labelled |
| **Camera** | `CameraTuner`, idempotent, writes only on drift (0 requests steady-state) | enforces only WiseStream/DynamicGOV/DynamicFPS **off**; bitrate mode and GOV are left to the user (2026-07-23 — the earlier CBR/GOV forcing was over-applied) |
| **Secrets** | `credentials.ini` (DPAPI-encrypted, off-repo) + TLS cert-pinning | no plaintext in the OneDrive-synced source tree |
| **Config** | `QSettings("GuardX","VMS")` (registry) + `credentials.ini` | flip a preset, relaunch, measure — no rebuild |

---

## 4. Per-profile decoder (the resolution/latency tension)

Historically the decoder's cost flipped with resolution — but `d3d12-flex` now wins on
both axes, so the per-profile mechanism is mostly insurance:

| Workload | Software | Default HW | **`d3d12-flex`** |
|---|---|---|---|
| 4× 640×480 grid | 287 ms, 7.7 % CPU | 579 ms, ~8 % ❌ | **298 ms, 1.7 %** ✅ |
| 1080p × many (future) | low latency but ~1 core/stream ❌ | low CPU but 579 ms ❌ | **low latency + low CPU** ✅ |

*(figures at 30 fps, 2026-07-22)*

**The decoder is still chosen per profile** (insurance for a link where relaxed compliance
misbehaves, or a future H.265 stream where the explicit path doesn't apply): `gstreamer_backend.cpp` reads the
profile token from the RTSP path (`…/<ch>/<profile>/media.smp`) and resolves:

```
decoder_<profile>   >   decoder (global)   >   "software" (built-in default)
```

So `decoder_profile4 = software` (grid) and `decoder_profile2 = d3d12` (a busy 1080p wall)
can coexist. With no keys set, both fall back to software. Grid and fullscreen profiles
themselves are also settable
(`grid_profile`, `fullscreen_profile`) for benchmarking and policy.

> **The endgame — now reached (2026-07-22): `compliance=flexible`.** The D3D decoders have
> no `low-latency` property, but they *do* expose `compliance`. Setting it to `flexible`
> stops the decoder waiting on DPB reordering (unneeded — this camera has no B-frames) and it
> emits frames immediately. Measured at 30 fps: **`d3d12-flex` = 298 ms latency (≈ software's
> 287) at 1.7 % CPU, decoding on the GPU.** Low-latency *and* low-CPU *and* hardware — the
> tension above dissolves.
>
> **Which one is "the default"? Neither — and that is deliberate (2026-08-03).** The
> **built-in fallback is `software`**: it has no HW dependency, so a machine without
> `d3d12h264dec` still shows video instead of failing pipeline construction. The
> **measurement-backed field recommendation is `d3d12-flex`**, set through the `decoder` key.
> The architecture is still under A/B (RPi insertion, 1080p transition), so the decoder is
> meant to be swapped per run — `run.ps1 software` / `run.ps1 flex` / `run.ps1 grid1080-flex`
> — not frozen into the binary. Read the effective value off the startup log line
> `[GStreamerBackend] ch N profile=… decoder=…`, never off a document.
>
> (Relaxed compliance could in theory show brief artifacts on a lossy link — validate
> clean-vs-lossy before fully trusting it on a link that drops packets.)
>
> Decoder values: `d3d12-flex` / `d3d11-flex` (low-latency HW) · `software` (CPU fallback) ·
> `d3d12` / `d3d11` / `qsv` (raw HW) · `auto` (decodebin).

---

## 4b. Channel-sync outlier guard (2026-08-04)

The original sync rule aligned everyone to the slowest channel, capped only at
`fastest + MAX_HOLD_MS` (700 ms). That cap is capacity sizing, not policy — a channel
running +500 ms behind (the old ch2 signature) still slowed **every** tile by half a
second. Now:

```
per report:  latency > fastest + sync_outlier_ms (default 150)  →  channel marked OUTLIER
             OUTLIER channel: excluded from the target, displayed unsynced (freshest frame)
             healthy channels: keep aligning at their own low target
re-entry:    latency < fastest + 150×0.7   (hysteresis — no flapping at the border)
logs:        "[ChannelSync] ch N 정렬 이탈/복귀 …" on every transition
```

Files: `channel_sync.{h,cpp}` (state + decision) · `rhi_video_widget.cpp`
(`presentation_deadline()` returns mailbox mode for outliers) · registry key
`sync_outlier_ms`. Verified 08-04: zero false positives across healthy 4-channel runs.

> **⚠ Known issue (pre-existing, exposed 2026-08-04):** the sync deadline trusts the
> camera's **raw** capture timestamps. The camera's streaming clock wanders (LATENCY_WINS
> §7 caveat); when it runs *ahead* of the PC, frames appear "captured in the future" and
> every tile is held by the clock error — measured 229 ms hold with the clock +190 ms
> (vs 52 ms in the morning when the clock ran behind). Same arithmetic before and after
> today's change. Fix direction: correct `report_latency()` and the deadline comparison
> with the per-channel `PipelineStats` offset estimate — needs design, not started.
> Irrelevant while running sync OFF.

---

## 5. Data & control planes (they stay separate)

```
   VIDEO plane   camera ──RTSP/UDP──► VMS pipelines ──► display        (latency-critical)
   METADATA      on-camera OpenSDK app ──HTTP poll──► DetectionFeed ──► box overlay
   CONTROL       VMS ──SUNAPI/HTTP(S)──► camera (CameraTuner, encoder settings)
   PREDICTION    RPi B poller ──► Postgres ──► forecaster ──► (analytics pages, separate)
```

Video latency and metadata latency are tuned independently; the box overlay carries its own
`playback_delay_ms` to align detections to the (differently-delayed) video. Keep this
separation — coupling them would make each harder to reason about.

**Files:** [detection_feed.cpp](../detection_feed.cpp) · [box_source.cpp](../box_source.cpp)
· [camera_tuner.cpp](../camera_tuner.cpp) · [credentials.cpp](../credentials.cpp)

---

## 6. What to add next (slots into the diagram, no redesign)

1. **Recording tap** — `tee` the **encoded** H.264 right after `h264parse`, *before* the
   decoder, into `splitmuxsink` (segmented MP4). Never record decoded frames (huge; a
   pointless re-encode). One branch to disk, one to display, same pipeline.
2. **Decode only what's visible** — stop a channel's pipeline when its tile is hidden
   (other page, scrolled off). Frees decode + CPU linearly with what's actually on screen.
3. ~~**Low-latency HW decode**~~ — ✅ **done (2026-07-22)** via `compliance=flexible` on the
   D3D decoders (`d3d12-flex`/`d3d11-flex`). Latency matches software at 1.7 % CPU. §4's
   tension is resolved. Remaining: a clean-vs-lossy robustness check of relaxed compliance.

---

## 7. Deployment invariants (non-negotiable for latency)

- Wired LAN; camera and VMS on the same switch / subnet.
- **The camera must actually be reached on-link — verify, don't assume** (2026-08-10).
  `ping <camera>` must answer **TTL 64**. TTL 63 means something is routing for you.
  Two separate Tailscale failure modes exist and they are often confused:
  1. *(UDP era)* Tailscale running stops RTCP sockets binding → silently kills latency
     measurement and channel sync.
  2. *(any transport)* A node that accepts an advertised `192.168.0.0/24` installs it at
     **metric 5**, beating the on-link wired route (**257**) on the same prefix length.
     The camera then answers from the other side of the internet. **Plugging in the cable
     does not change this** — the route table decides, not the cable. Measured impact:
     RTT p50 1 → 476 ms, loss 0 → 75 %, in-session video creep 40 ms → 6–9 s, ONVIF box
     matching 929 → 0. Fix and evidence: `LAN_TEST_CHECKLIST.md` §1.3.
- Host clock NTP-synced (`w32tm /resync`) — for honest latency numbers and camera-time
  alignment.
- **Absolute latency numbers only right after a camera reboot** — the camera's streaming
  (RTCP SR) clock is loosely slaved to its NTP-synced system clock: it re-anchors on
  reboot, then wanders (±, ~2 ms/min observed). Floor-anchored relative stats stay valid.
- Don't restart the app in rapid succession against the camera — its concurrent-RTSP-session
  pool (shared with the poller + analytics app) takes ~30–60 s to reap stale sessions.
