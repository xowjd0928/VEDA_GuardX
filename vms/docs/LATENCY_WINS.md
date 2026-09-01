# GuardX VMS — Where the Latency Wins Come From

**A compare-and-contrast of every architectural lever, and exactly how much each one buys.**

Last updated: 2026-08-03 (metric rename + stage-instrumentation note; measurements still 2026-07-22). All "wired" figures are from the 2026-07-22 wired-LAN sweep
(ping 0 ms, host clock resynced, glass-to-arrival via camera RTCP NTP timestamps). "WiFi/VPN"
figures are from the earlier runs A–F (see [VMS_LATENCY_REPORT_2026-07-21.md](VMS_LATENCY_REPORT_2026-07-21.md)).

> **One-line summary:** of the ~528 ms → ~128 ms improvement on wired, **~400 ms is the
> decoder swap alone.** Everything else either prevents latency *blowing up* on a bad link
> (transport, drop-policy) or trades latency for a different good (sync, hardware decode).

---

## The whole stack, one frame's journey

```
capture → encode → network → jitter buffer → DEPACKETIZE → DECODE → color-convert → queue → render → display
          └camera┘ └transport┘ └latency=N┘                └DECODER┘   └shader/GPU┘  └mailbox┘        └vsync┘
             ~80ms    ~0-∞       0-Nms                      128 or 528   ~few ms      ~0-700ms       ~8ms
```

Each section below is **A vs B**: the two ways to build that stage, and what it costs.

### Where each number in this file stops (read before comparing anything)

The headline `glass-to-arrival` figure is taken in the `appsink` callback — **capture →
arrival in our process**, before queue, display selection, upload and draw. It was called
`glass-to-display` until 2026-08-03; the name was wrong and every reader (including
customers) mis-read it as screen-referred.

Since 2026-07-31 the backend also logs a **per-stage breakdown** that goes further:

| Log line | What it covers | Cadence |
|---|---|---|
| `[GStreamerBackend] ch N glass-to-arrival` | capture → appsink, **clock-corrected** | 5 s |
| `[Pipeline] chN` | `net` / `decode` / `queue` / `render` / `TOTAL` p50·p95·max | 30 s |
| `[Trace] chN` | one single frame walked capture → render submit, with per-hop deltas | `trace_interval_ms`, default 5 s |
| `[DirectSink] ch N glass-to-sink … (원시)` | capture → sink input on the **direct** path — ⚠ **RAW, not clock-corrected** (added 2026-08-04) | 5 s |

> [!warning] The direct path logs a **raw** number and it is normally negative
> `[DirectSink] … glass-to-sink -3168 ms (원시)` is not a bug. The direct backend has no
> `PipelineStats`, so it stores `now − capture` with the camera↔PC clock difference still
> in it. The HUD's `video Nms` shows the same value. **Measured 2026-08-10: this camera
> runs 3.3 s ahead of the PC**, so all four channels sit at −3.0 … −3.3 s.
>
> To judge anything from it, use **excess over the session minimum** — the minimum is
> (clock offset + fastest frame's real latency), so subtracting it cancels the offset.
> That is exactly what the direct path's latency-creep guard does (and why porting the
> appsink guard's *absolute* threshold would have been wrong in both directions —
> see `direct_sink_backend.cpp`).

`render` is **CPU-side submit** (texture upload + draw encoding), not GPU completion. So the
still-unmeasured tail is: backingstore present (vsync, ≤16.7 ms) → DWM composition (~1 frame)
→ panel response. Structurally that is ~30–70 ms, and **nothing in this file includes it.** A
stopwatch glass-to-glass test is the only way to pin it down.

> **`net` and `TOTAL` are relative, not absolute.** This camera's clock runs ahead of the PC
> (~1.04 s measured 2026-07-31; **3.3 s measured 2026-08-10** — the offset drifts, so treat it
> as "some seconds", never as a constant). `PipelineStats::observe_net()` subtracts the
> observed minimum as a clock-offset estimate. That makes the floor structurally 0 — the
> values read as "excess over the fastest frame in the window". Absolute latency requires
> camera+PC NTP sync. Do not pass a nonzero `OFFSET` to `analyze_latency.py` on
> post-2026-07-31 logs; it double-corrects.

> [!danger] ⚠ Every number in this file assumes the camera is reached **on-link** (2026-08-10)
> If `ping <camera>` shows **TTL 63** instead of 64, the traffic is going through a
> Tailscale subnet route and out of the building. Measured in that state: RTT p50 476 ms,
> loss 75 %, and video latency that **climbs monotonically inside a session**
> (−3.2 s → +5.5 s over about a minute) until the creep guard restarts the stream.
> Retransmission and zero-window stay clean, so a loss-only check calls it healthy.
> Plugging in the cable does **not** move the route — see `LAN_TEST_CHECKLIST.md` §1.3.

---

## 1. Decoder — software vs hardware vs low-latency-hardware  ⭐ the decisive win

**15 fps, wired, clean link (2026-07-22 sweep G):**

| | Software (`avdec_h264`, CPU) | Hardware default (`d3d12`, Quick Sync) |
|---|---|---|
| **Median latency** | **128 ms** | 528 ms |
| GPU "Video Decode" engine | 0.0 % | 2.4 % |
| CPU | 9.5 % | 8.0 % |
| Per-frame behaviour | decode → emit **immediately** | pipelines ~6 frames deep before emitting |

**How it wins: −400 ms.** The default hardware decoder is built for throughput — it keeps
~6 frames in flight in a pool of output surfaces. At 15 fps that pool *is* ~400 ms of
latency, sitting inside the decoder before a frame ever reaches the screen. Software decodes
each frame synchronously and hands it on with no queue. Proof it's the decoder and nothing
else: the GPU Video-Decode engine drops from 2.4 % to **exactly 0.0 %** as latency collapses.

### The fix that keeps hardware decode: `compliance=flexible`  ⭐⭐

The ~6-frame queue exists to satisfy H.264's Decoded-Picture-Buffer reordering rules — which
this camera **never needs**, because it encodes with no B-frames. Setting the D3D decoder's
`compliance=flexible` tells it to stop waiting on DPB reordering and emit frames as soon as
they're decoded. Result (**30 fps, wired, 2026-07-22 — same session A/B**):

| Decoder @ 30 fps | Median | p10 | min | CPU | Decode on |
|---|---|---|---|---|---|
| software | 287 ms | 96 | 84 | 7.7 % | CPU |
| `d3d12` default HW | 579 ms | 290 | 184 | ~8 % | GPU |
| **`d3d12` + `compliance=flexible`** | **298 ms** | 123 | 96 | **1.7 %** | **GPU** |

> **This dissolves the whole latency-vs-efficiency tension.** `d3d12-flex` matches software's
> latency (298 vs 287 ms) while decoding on the GPU — so CPU is **1.7 %** instead of 7.7 %,
> and it scales to 1080p×N without eating cores. Low-latency **and** low-CPU **and** hardware.
> **It is the recommended field setting — but not the built-in default** (that stays
> `software`, which needs no HW support and so can never fail pipeline construction). Select
> it per run: `run.ps1 flex`, or the `decoder` registry key. See ARCHITECTURE.md §4.

**Caveats (honest):** (1) tonight's link was jittery (ping 5–269 ms) and the clock offset was
+293 ms, so the *absolute* numbers are noisy — but the three-way A/B ran back-to-back under
the same conditions, so the *relative* ranking is solid. (2) `flexible` relaxes spec
compliance; on a **lossy** link it could in principle emit a frame before all references
arrive (brief artifacts). Not observed in testing, but worth a dedicated clean-vs-lossy check
before trusting it in production. If it ever misbehaves, `decoder=software` is one setting away.

---

## 2. Transport — UDP vs TCP  (the win is *avoiding a blow-up*)

| | UDP + `drop-on-latency` | TCP |
|---|---|---|
| **Median (WiFi, same day)** | **582 ms** | 1481 ms |
| **p90 (WiFi)** | 1159 ms | 3672 ms |
| **Worst case (WiFi)** | 2717 ms | 6625 ms |
| On a clean **wired** link | baseline | ≈ same (loss ≈ 0, nothing to back up) |

**How it wins: −900 ms on a lossy link; ~0 on a clean one.** This is not a constant saving —
it's a *failure-mode* difference. TCP guarantees delivery, so on any packet loss it
retransmits and **holds newer frames behind the missing one** — the backlog grows without
bound (we saw channels drift 30–60 s behind). UDP with `drop-on-latency=true` throws the late
packet away and shows the newest frame. On wired (0 % loss) the two converge, because there's
nothing to retransmit. So: **UDP doesn't make a good link faster; it stops a jittery link from
snowballing.**

> Prerequisite discovered the hard way: UDP needs its RTCP sockets to bind. **Tailscale
> running blocks that**, which silently disables latency measurement *and* channel sync.

---

## 3. Jitter buffer — `latency=0` vs a buffer  (a direct 1:1 dial)

| | `latency=0` | `latency=200` | `latency=400` |
|---|---|---|---|
| Added latency | 0 ms | +~200 ms | +~400 ms |
| Robustness to jitter | none (drop late frames) | absorbs ≤200 ms jitter | absorbs ≤400 ms jitter |
| WiFi median (400 vs 150) | — | 645 ms | 672 ms |

**How it wins: every millisecond of buffer is a millisecond of latency, 1:1.** The jitter
buffer trades latency for smoothness — it holds packets so out-of-order/late arrivals can be
reordered before display. On a clean wired link there is no jitter to absorb, so `latency=0`
is free and correct. On WiFi, shrinking 400→150 ms barely moved the median (645 vs 672)
*because the decoder's 400 ms dominated everything* — a vivid example of why you fix the
biggest term first. **Rule: buffer = max jitter you must tolerate, and no more. Wired = 0.**

---

## 4. Channel sync — off vs on  (latency vs alignment)

| | Sync OFF | Sync ON |
|---|---|---|
| Each tile shows | its **freshest** frame | a common time (held to the slowest tile) |
| Displayed latency (fast tiles) | ~arrival (~520 ms HW / ~120 ms SW) | pinned to slow tile (ch2: ~1050 ms HW) |
| OSD clocks across tiles | differ by real arrival spread | identical |

**How it wins: OFF removes up to ~500 ms of *display hold* from the fast tiles.** Important
subtlety: our `glass-to-arrival` metric is measured at **arrival** (appsink), *before* the
sync hold — so the number looks identical with sync on/off (528 vs 530 ms). The real
difference is at **display time**: with sync ON, ch0/1/3 are held back to match ch2 (the
consistently-slow channel), so they *display* ~500 ms later than they arrived. With sync OFF
each shows immediately. Confirmed visually: sync-off screenshots show the OSD clocks diverge;
sync-on shows them locked together. **Choose per view: OFF for a single low-latency feed; ON
when cross-tile time alignment matters more than latency.**

---

## 5. Render path — GPU vs CPU  (a CPU/scaling win, not a latency win)

| | GPU (`QRhiWidget`, NV12 shader) | CPU (`QGraphicsView` + `QImage`) |
|---|---|---|
| CPU (4×640×480) | **6.6 %** | 8.3 % |
| Latency contribution | ~few ms | ~few ms (similar) |
| Scaling to 1080p×N | flat (shader does the work) | CPU grows with every pixel |

**How it wins: not much latency — it's *headroom*.** The color conversion + scaling +
compositing move from CPU to a GPU fragment shader. At thumbnail resolution the latency
difference is single-digit ms and the CPU saving is modest (−1.7 pts). The payoff is at
scale: the CPU path would saturate cores at 4×1080p and start dropping frames (which *does*
add latency); the GPU path stays flat. **This is insurance for growth, not a headline number.**
(One self-inflicted lesson: an early version repainted the overlay 33×/s unconditionally and
was *slower* than the CPU path — fixed by repainting only when a box moves. GPU 3D 13.7 %→9.9 %.)

---

## 6. Camera encode — the floor you can't code around

| Setting | Latency effect |
|---|---|
| B-frames = 0 (already absent on this camera) | would add ≥1 frame reorder each way; not present |
| **WiseStream / DynamicGOV / DynamicFPS off** | the real killers — each, when on, raises latency (GOP stretch, fps drop). **Tuner enforces only these.** |
| CBR vs VBR | **not a clear win either way on a low-bitrate LAN.** CBR steadies delivery (matters only on constrained/lossy links); VBR skips the encoder's rate-control buffer, so it can be marginally *lower* latency. **Left to the user — tuner no longer forces CBR** (2026-07-23 correction; earlier "CBR for latency" was over-applied). |
| GOV length | **no steady-state latency effect** (P-frames decode immediately); only changes post-loss recovery time. Left to the user. |
| **15 → 30 fps (applied 2026-07-22)** | frame interval 66 → 33 ms; also **halves the HW-decoder queue latency** (queue is N *frames*, so drains 2× faster) |

### Bitrate — measured A/B (2026-07-23, profile6 800×448@30 VBR, wired but jittery link)

| | 1024 kbps | 2048 kbps |
|---|---|---|
| Latency median | 167 ms | 206 ms (≈ flat — the diff is link noise; floor even improved 67→34 ms) |
| Smoothness jitter (frame-gap stddev) | 47 ms | 58 ms |
| Worst stutter (max gap) | 633 ms | 922 ms |
| Effective fps | ~30 | ~30 |

**Reading:** doubling the bitrate **did not change latency** (bitrate is bytes, the decoder
queue counts frames, and 1–2 Mbps is nothing on a LAN) but made **smoothness slightly worse**
— jitter +11 ms, worst-stutter +290 ms. Cause: at 2× bitrate the **I-frame bursts are ~2×
bigger**, so each one takes longer to deliver and opens a bigger gap. It buys **quality**
(a visual judgment the metrics can't score). Chosen: **1024 kbps** — no latency cost, smoother,
adequate quality for 800×448. (Caveat: the link was jittery that night, amplifying the
smoothness gap; on a clean link the regression would be smaller.)

### The smoothness metric

Latency alone doesn't tell you if video *looks* smooth. The backend now logs, per channel
every 5 s: **effective fps** (dropped frames if < 30), **jitter** (stddev of frame-arrival
gaps; ideal ~5–10 ms, 33 ms = one frame interval), and **max gap** (worst single stutter).
`analyze_latency.py` summarizes these alongside latency. Use both when tuning: a change can
hold latency flat while quietly hurting smoothness (as the bitrate bump did).

**How it wins: it sets the floor, and shrinks frame-counted delays.** After the decoder fix,
the remaining latency is mostly the camera: encode + one frame interval + our few-ms
pipeline. Going to 30 fps helps twice: (1) the frame interval halves (66 → 33 ms), so the
newest frame is fresher; (2) any delay measured in *frames* — including the default HW
decoder's ~6-frame queue — drains twice as fast, which is why default `d3d12` improved from
528 ms (15 fps) toward the ~300–580 ms range at 30 fps. The wired **minimum observed was
84 ms** at 30 fps (was 103 ms at 15 fps). **All 4 channels are now 30 fps**; bitrate was
raised 1.5× (profile4 1024 → 1536 kbps, profile2 2560 → 3840 kbps) so CBR quality holds —
without that, doubling fps at fixed bitrate would just halve per-frame quality.

---

## 5b. videoconvert — measured, split verdict (2026-08-04, on-site wired)

Question: is the `videoconvert n-threads=2 ! video/x-raw,format=NV12` stage a passthrough
or a real per-frame conversion? Answer: **depends on the decoder.**

| Decoder | videoconvert sink caps | Verdict |
|---|---|---|
| software (`avdec_h264`) | `video/x-raw, format=I420` (measured, gst-launch -v) | **real conversion, every frame** — I420→NV12 chroma re-interleave, ~16 MB/s at 800×448@30. Hidden CPU + one full-frame copy that was previously attributed to "decode" (the decode stage probe spans decoder-sink → appsink, so videoconvert time lands inside the `decode` number) |
| `d3d12-flex` | system-memory NV12 (by caps-template logic: decoder SRC template offers D3D12Memory/D3D11Memory/system NV12; `videoconvert` only accepts system memory → negotiation must pick system NV12) | **passthrough** (equal caps → basetransform zero-work mode). The GPU→CPU download happens *inside* the decoder — this is the §5 round-trip, and the zero-copy (⑥) target |

Practical consequence: `software` pays decode **plus** a per-frame I420→NV12 pass;
`d3d12-flex` pays a GPU→CPU download inside the decoder. A cheap improvement for the
software path (if ever needed): render I420 directly with a 3-plane shader, or pin
`avdec_h264` output via explicit chain — see the decodebin note below.

⚠ **Side-discovery — decodebin has a stream-race with this camera.** The RTSP session
carries a second stream (ONVIF metadata, `VND.ONVIF.METADATA`, pt 107). In gst-launch
the delayed-linking sometimes wired **the metadata pad** into `decodebin`, which then
depayed it to `application/x-onvif-metadata`, failed to link `videoconvert`, and killed
the pipeline with "Internal data stream error / not-linked". `gst_parse_launch` in the
VMS uses the same delayed-linking — the `software` preset (`decodebin
force-sw-decoders=true`) carries the same latent race, masked by the reconnect+backoff
loop (a lost race looks like one silent reconnect). The explicit chain
`rtph264depay ! h264parse ! avdec_h264` only accepts the video stream and has no race —
candidate hardening for the software preset, not yet applied.

---

## 7. Glass-to-glass — MEASURED (2026-08-04, on-site wired, stopwatch photos) ⭐

The last unmeasured gap is closed. Method: laptop showed a ms stopwatch page
(`guardx_vms_build/stopwatch.html`); the camera filmed that screen; the same screen showed
the VMS tile; a phone photographed both at once. `real digits − digits inside the tile` =
true glass-to-glass, **independent of every clock in the system** (the laptop's display
latency appears in both terms and cancels).

**Preconditions met**: wired on-link (TTL 64), Tailscale service stopped, camera SyncType=NTP
(after a camera reboot — see caveat), PC resynced (+7 ms vs pool.ntp.org).
Config: `flex` preset (d3d12-flex, UDP, latency=0, sync ON), focus view, CAMERA 2 (=ch1).

| Result | Value |
|---|---|
| **glass-to-glass median** | **216 ms** (n=9, range 199–270; one photo dropped for motion blur) |
| Same-run `[Pipeline]` ch1 | net 12 · decode 5 · queue 52 (sync hold) · render 0 · TOTAL p50 69 (floor-anchored) |
| Reading self-check | blue wall-clock − green stopwatch constant 7.694 s across all photos |

**Decomposition (absolute):**

```
216 ms ≈ 90 (capture→appsink: encode+wire+decode, from post-reboot raw)
       + 52 (ChannelSync display hold)
       +  0 (render submit)
       + ~17 (tile refresh quantization, mean)
       + ~57 (present→vsync→DWM→panel = the previously unmeasured tail)
```

**Verdict: the display tail is ~57 ms — structural 60 Hz territory (vsync ≤17 + DWM ~17 +
panel 10–20). Unexplained residual ≈ 0–15 ms → item ⑤ (display-path rework) is NOT
justified. Gate closed.** The instrumentation is also now validated end-to-end: log stages +
structural constants reproduce the photographed number.

Practical corollary: in single-view, `nosync` removes the 52 ms hold → ~165 ms displayed.

**⚠ Camera streaming-clock caveat (operational rule):** the camera's RTCP/SR clock is NOT
slaved tightly to its NTP-synced system clock. After factory-reset-era manual clock setting
it was 1.5 s off and only slewed ~15 ms/min; a **camera reboot re-anchors it** (raw floor
67 ms right after). It then drifts again (~2 ms/min observed; raw floor 67→130 in ~30 min).
So: absolute-latency measurements are trustworthy **shortly after a camera reboot**; the
floor-anchored relative stats remain valid always. (Discovered 2026-08-04 — the NTP
enable/step sequence and reboot behaviour are logged in the Obsidian execution-plan note.)

**Also fixed on the way (reboot fallout):** the `test` OpenSDK app (serves
`/opensdk/test/tracks` for the poller + VMS blur) had `AutoStart=False` and died with the
reboot — restarted via web UI, AutoStart enabled. `juan_application` also had
`AutoStart=False` (it survived only via the camera's running-state restore) — AutoStart
enabled too.

### 8.4 software vs d3d12-flex at the operational config — stage-level draw (2026-08-04 pm)

Head-to-head, back-to-back, new build (explicit software chain), 800×448×4, nosync,
~5.5 min each, ≥40 windows:

| p50 (window medians) | software | d3d12-flex |
|---|---|---|
| net | 12 ms | 11 ms |
| **decode** (incl. videoconvert) | **1 ms** | 2 ms |
| queue | 9 ms | 9 ms |
| render | 0 ms | 0 ms |
| **TOTAL** | **23 ms** | **21 ms** |
| arrival median / jitter | 16 ms / 7–12 | 14 ms / 8 |

**Verdict: a complete latency draw at this resolution** — software even decodes in 1 ms
*including* its real I420→NV12 conversion. The July "287 vs 298" conclusion is re-confirmed
at stage granularity. The only remaining differences are CPU (11 % vs ~2 % decode-side)
and high-res behaviour (§8.3: both collapse at 4 MP; software degrades more gracefully).
**If CPU is a non-issue for the deployment, `software` is a fully valid field default** —
it also removes the HW dependency. The swap-per-run policy stands.

**⚠ Confounder discovered and reclassified:** ch1 starves at startup (~2–2.5 min of
no-frame watchdog cycles, then stabilizes) — **under BOTH decoders** this afternoon,
while all morning runs were clean. So it is NOT a decoder or explicit-chain issue;
correlates with the afternoon state (test + juan_application apps now running on the
camera; ch1 is the analytics-primary sensor). Packet capture during an episode shows RTP
flowing at the NIC while appsink gets nothing. Needs a dedicated session-level
investigation (camera-side load / RTSP session contention suspected).

**Tooling fix:** `analyze_latency.py` read logs as UTF-8 while Qt writes CP949 — the
"재접속" reconnect counter was silently always 0 (this hid ch1's loop in the morning B4
baseline). Now decodes lenient-cp949/utf-8 and picks the better; counters are live
(17–19 reconnects in these runs, all ch1).

---

## 9. Camera encode+packetize — MEASURED for the first time (2026-08-04, packet capture) ⭐

The last opaque term. Method: on the wired on-link LAN (wire time ≈ 0), a host-side packet
capture timestamps each RTP packet at the NIC; the RTCP SR NTP↔RTP mapping converts each
frame's RTP timestamp to the camera's capture wall-clock. `first-packet NIC arrival −
capture` = **camera encode + packetize**. Preconditions: camera rebooted minutes before
(clock anchored: VMS raw floor 62 ms ≈ pcap floor 61), PC clock +3 ms vs NTP, Tailscale
stopped. 75 s, 19 k packets, 2,250 frames × 4 video SSRCs (tshark + scapy;
`scratchpad/analyze_encode.py`).

| Stream | capture → first packet at NIC | notes |
|---|---|---|
| video ×4 (pt 98, H.264 800×448@30) | **min 61 · p50 71–73 · p90 79–81 ms** | all four sensors identical; last-packet ≈ first-packet (frames fit in a few packets at 1 Mbps) |
| metadata ×2 (pt 107, ONVIF) | min 32 · p50 67 ms | no encoder → lower floor, confirms the method |

**Cross-check (same minutes, VMS log):** raw glass-to-arrival p50 ≈ 70–77 ms. So
`arrival − encode` = wire + kernel + jitterbuffer + depay + parse + decode ≈ **0–5 ms**
— matching the `[Pipeline]` decode p50 of 2 ms. The receiver's entire contribution before
the queue is a rounding error; **the camera owns ~72 of the ~75 ms to appsink.**

**Reading:** 72 ms at 30 fps ≈ 2.2 frame intervals of internal camera pipelining
(ISP → encoder → packetizer). This is the floor we cannot code around — with every
receiver-side trick applied (flex, nosync), displayed latency ≈ 72 (camera) + ~5
(transit+decode) + ~11 (mailbox) + ~17 (tile refresh) + ~55 (present) ≈ **~160 ms**,
which is exactly what we measure. The pipeline is now **fully decomposed end-to-end with
no unmeasured or inferred terms.** Encode jitter is tight (p10→p90 spread 16 ms ≈ half a
frame interval) — the camera is healthy.

---

## Scoreboard — cumulative, on wired

| Configuration | Median | vs previous |
|---|---|---|
| Hardware decoder (d3d12), sync on | 528 ms | — |
| **+ software decoder** | **128 ms** | **−400 ms** ⭐ |
| + sync off (display, fast tiles) | ~120 ms displayed | −~500 ms *display* hold on fast tiles |
| theoretical floor (min observed) | 103 ms | camera-bound |

On WiFi, add the transport story on top: TCP 1481 ms → UDP 582 ms (**−900 ms**) was purely
*preventing the backlog blow-up* that a lossy link causes — invisible on wired because wired
doesn't lose packets.

---

## How to read this as a priority list

1. **`compliance=flexible` HW decode** — low latency (298 ms) + low CPU (1.7 %) + GPU. The
   prize, now cracked. ✅ done, **recommended** (`run.ps1 flex`). The built-in fallback stays
   `software`, so an unsupported machine degrades instead of failing.
2. **Transport + drop-policy** — UDP/`latency=0`/drop. Stops lossy links snowballing. ✅ default.
3. **Sync off** — when you want the lowest *displayed* latency and don't need aligned tiles.
4. **Camera 30 fps** — fresher frames + halves frame-counted queues. ✅ applied.
5. **GPU render** — no latency headline; keeps CPU flat as you scale. ✅ done.
6. **Software decode** — the reliable low-latency fallback; best when HW/flex is unavailable
   or unproven on a link, at a CPU cost that grows with resolution.

---

## 8. Isolation runs — MEASURED (2026-08-04, on-site wired) — closes the old pending list

All three outstanding gaps measured, 5.5 min per preset, 30 fps, 0 reconnects everywhere.
⚠ Camera streaming clock was drifting (~2 ms/min, §7 caveat) — cross-run *raw* comparisons
below are drift-adjusted; within-run numbers are unaffected.

### 8.1 Transport on wired: UDP vs TCP (software, 800×448×4)

| | UDP (baseline) | TCP |
|---|---|---|
| raw arrival median | 193 ms | 224 ms (−~14 drift → **+~17 real**) |
| jitter / maxgap | 7 ms / 86 ms | 15 ms / 115 ms |
| CPU | 11.2 % | 10.5 % |

**As predicted: ≈ equal on a clean link.** TCP costs ~17 ms median and 2× jitter — small,
not the WiFi blow-up. UDP stays the default; TCP is an acceptable fallback on wired.

### 8.2 Jitter buffer 50 ms — measured **+0 ms** (not +50!) ⭐ correction to §3

jitter50 raw min 200 ms vs TCP-run min 198 ms — within clock drift. **On a clean wired
link with our `appsink sync=false` pipeline, `latency=50` added no measurable delay.**
The §3 "every ms of buffer is a ms of latency, 1:1" rule holds when the buffer actually
*holds* packets (reordering / loss / sync-to-clock renderers) — i.e. on the WiFi runs it
was real. On a loss-free link the buffer passes packets straight through. Practical
consequence: a small jitter buffer is **cheaper insurance than documented** for wired
deployments — revisit if loss appears (the drop-on-latency=false trade-off still applies).

### 8.3 High-res grid: profile2 is **2592×1520 (4 MP), not 1080p** — and 4 MP×4 collapses

Post-factory-reset profile2 = 2592×1520@30, 5120 kbps (the "grid1080" preset name is now
a misnomer; this is a 473 MPx/s load — ~2× the 1080p design target of §5).

| 4 MP×4 | corrected median | p90 | fps | worst maxgap | CPU |
|---|---|---|---|---|---|
| software | 275 ms | 3.7 s | 28.3 | 38 s | 35.5 % |
| d3d12 (hw) | 1.6 s | 4.4 s | 27.4 | 36 s | 22.6 % |
| d3d12-flex | 2.3 s | 4.8 s | **23.3** | 33 s | 18.4 % |

**All three modes collapse** — multi-second latencies and 16–38 s stalls. Notably flex
(the 800×448 champion) is *worst* on fps: the Iris Xe decode engine saturates near
473 MPx/s, while software-on-8-cores degrades more gracefully (ch0 even stayed clean at
30 fps / 182 ms maxgap — the collapse is uneven, pointing at receive-side burst loss
(5 Mbps VBR I-frames, GOV=60 → a lost I-frame costs up to 2 s) compounding decoder
saturation).

**Verdicts:**
1. **4 MP×4 grid is not viable on this host** in any decoder mode. Don't demo it.
2. The **actual 1080p question remains open** — profile2 must first be reconfigured to
   1920×1080 (half the load: extrapolating, software ≈ 18 % CPU and the GPU decoder is
   comfortably inside its envelope). Re-run the A/B then.
3. **⑥ zero-copy is NOT justified by this data** — at 4 MP the bottleneck is decode
   saturation + receive loss, not the GPU↔CPU copy (flex already has the lowest CPU).
   Gate stays closed until a real 1080p profile shows copy-bound behaviour.
4. If high-res is pursued: raise the OS UDP receive buffer and consider shorter GOV
   before blaming the pipeline.
