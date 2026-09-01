# GuardX VMS — Wired LAN Test Checklist

**Use this the moment you plug in the LAN cable.** Work top to bottom; each step says *why*,
so you can skip one deliberately rather than by accident.

Prior results for comparison: [VMS_LATENCY_REPORT_2026-07-21.md](VMS_LATENCY_REPORT_2026-07-21.md)
— best so far **582 ms median** on 2.4 GHz WiFi.
**Target on wired LAN: 150–300 ms.**

---

## ✅ RESULT (2026-07-22 wired test — done)

**Wired LAN + software decoder = median 128 ms** (was 528 ms with the hardware decoder).
The D3D hardware decoders were queuing ~400 ms internally; that, not the network, was the
whole latency problem. `decoder = software` is now the default.

| Decoder | Median | CPU | GPU-decode |
|---|---|---|---|
| auto / d3d12 (HW) | 528 ms | 8.0% | 2.4% |
| d3d11 (HW) | 524 ms | 8.5% | 2.6% |
| **software** | **128 ms** | 9.5% | 0.0% |

To go lower still: `.\run.ps1 nosync` on top (per-tile freshest frame, not held to the
slow channel). Everything below is kept for reference / re-testing.

---

## ⚡ Ready-to-run kit

A prebuilt, self-contained test kit is at **`C:\Users\3-11\guardx_vms_build\`** — it
survives temp cleanup and needs no rebuild. It contains the exe (+ Qt DLLs), `run.ps1`,
`measure.ps1`, and `analyze_latency.py`.

```powershell
cd C:\Users\3-11\guardx_vms_build

# 1. run a preset (each starts from a clean settings slate)
.\run.ps1                # baseline: udp, jitter 0, auto decoder, sync on
.\run.ps1 nosync         # channel sync OFF  → lowest latency, tiles unaligned
.\run.ps1 software       # software decoder
.\run.ps1 d3d11          # d3d11 hardware decoder
.\run.ps1 tcp            # TCP transport (only if UDP shows loss)
.\run.ps1 jitter50       # 50 ms jitter buffer (only if artifacts)

# 2. let it run >= 5 min, close the window, then:
.\measure.ps1 nosync     # same preset name; prints median/p90/per-channel/reconnects
```

`run.ps1` prints a **[WARN] if Tailscale is still running** — heed it (see §1.1).
`measure.ps1` assumes the clock is synced (offset 0). If you skipped `w32tm /resync`,
pass the measured offset: `.\measure.ps1 nosync 40`.

The decoder + sync presets are the **Tier-4 latency levers**, already wired and verified
(software decode confirmed at 0% GPU decode engine). Test order tomorrow:
**baseline → nosync → software → d3d11**, recording each.

---

## 0. TL;DR — the six that actually matter

| # | Do this | Why |
|---|---|---|
| **0** | **Verify the camera is reached ON-LINK** — `ping 192.168.0.3` must show **TTL 64** | ⭐ 2026-08-10: this is the gate. TTL 63 means the traffic is tunnelling through a Tailscale subnet route and **every number in this file becomes meaningless** (§1.3) |
| 1 | **Quit Tailscale completely** | It stops RTCP/UDP sockets binding → kills latency measurement *and* channel sync. ⚠ **Quitting it is not the only failure mode** — see #0 |
| 2 | **Pause OneDrive** | Largest CPU consumer on this machine (39,861 CPU-seconds), constantly syncing this very folder |
| 3 | **Close Slack / VS Code / Chrome / Claude desktop** | They were using **~30% of GPU 3D** — they compete with the video renderer for the same Iris Xe |
| 4 | **Shut down the VirtualBox VM** | 11,310 CPU-seconds; shares the same power/thermal budget |
| 5 | **Disable the Wi-Fi adapter once the cable is in** | Removes a same-subnet second path. ⚠ **Necessary but not sufficient** — it does not fix #0 (2026-08-10: the cable was in, Wi-Fi was up, and the route still pointed at Tailscale) |

Everything else below is verification and measurement.

> [!tip] One command runs most of this
> `python VMS/tools/acceptance.py all --sec 60` checks the route (#0), the camera TLS pin,
> MQTT node liveness, then launches the VMS, judges 14 things from its log, and captures
> packets with tshark. `latency` mode measures glass-to-glass. See §3.

---

## 1. Pre-flight — things to TURN OFF

### 1.1 Tailscale (critical)

```powershell
# Fully exit, not just disconnect
Stop-Service Tailscale -ErrorAction SilentlyContinue
Get-Process tailscale* -ErrorAction SilentlyContinue | Stop-Process -Force
# verify: no 100.x address should remain
ipconfig | Select-String "100\."
```

> **Why:** with Tailscale running, `udpsrc` fails to bind RTCP ports
> (`Error binding to address 0.0.0.0:60276`). No RTCP → no NTP capture timestamps →
> the latency HUD prints nothing and channel sync silently does nothing.
> This single issue caused every earlier "UDP doesn't work" conclusion.

### 1.2 Background load

```powershell
# OneDrive — biggest CPU consumer, and it syncs the VMS folder itself
Get-Process OneDrive -ErrorAction SilentlyContinue | Stop-Process -Force

# VirtualBox VM (Ubuntu) — shares CPU/thermal budget
Get-Process VirtualBoxVM -ErrorAction SilentlyContinue | Stop-Process -Force

# Electron apps competing for GPU 3D
Get-Process Slack, Code, chrome, claude -ErrorAction SilentlyContinue | Stop-Process -Force
```

> **Why:** measured GPU 3D at the time of the last test — claude 12.4%, VS Code 8.3%,
> Slack 7.7%, dwm 1.9%. The VMS itself only needs ~10%. On an integrated Iris Xe the CPU
> and GPU share one power budget, which is why the CPU was seen throttled to **1.19 GHz**.

### 1.3 Network isolation — ⭐ the gate (rewritten 2026-08-10)

```powershell
# After the LAN cable is connected and working:
Get-NetAdapter | Where-Object { $_.Name -like "*Wi-Fi*" } | Disable-NetAdapter -Confirm:$false

# THE check. TTL 64 = on-link (good). TTL 63 = something is routing for you (bad).
ping 192.168.0.3

# If TTL is 63, look at why — the winner is decided by metric, not by which cable is in:
Get-NetRoute -AddressFamily IPv4 |
  Where-Object DestinationPrefix -like "192.168.0.*" |
  Select-Object DestinationPrefix, NextHop, InterfaceAlias, RouteMetric, ifIndex
Get-NetIPInterface -AddressFamily IPv4 | Select-Object InterfaceAlias, InterfaceMetric
```

**Expected wired ping: TTL 64, < 2 ms, jitter < 1 ms.**

> [!danger] ⚠ Plugging the cable in does **not** move the route (measured 2026-08-10)
> A Tailscale node that accepts an advertised `192.168.0.0/24` installs it with
> **route metric 0 on an interface of metric 5 → total 5**. The on-link wired route is
> **256 + 1 = 257**. Same prefix length, so the *metric* decides: **Tailscale wins**, and
> the camera traffic leaves the building and comes back through the site router.
>
> | | TTL | RTT p50 | loss | TCP retrans | video creep in-session |
> |---|---|---|---|---|---|
> | on-link (what this file assumes) | **64** | **1 ms** | **0 %** | **0.00 %** | **34–46 ms** |
> | via tunnel | 63 | 476 ms | 75 % | 0.02 % | 6,456–8,854 ms |
>
> Note what the tunnel case looks like: retransmission and zero-window are **fine**.
> The damage is throughput + RTT, so a loss-only check says "healthy" while the picture
> falls further behind every second. Check the route, not just the loss.
>
> **Fix (admin PowerShell)** — a `/32` beats the `/24` on prefix length regardless of
> metric, and only pulls the camera out of the tunnel (tailnet peers such as the MQTT
> broker keep working):
>
> ```powershell
> # InterfaceIndex = the wired NIC (ifIndex column above)
> New-NetRoute -DestinationPrefix 192.168.0.3/32 -InterfaceIndex 14 -NextHop 0.0.0.0 -PolicyStore ActiveStore
> ```
>
> `ActiveStore` disappears on reboot (good for a test session); add
> `-PolicyStore PersistentStore` to keep it. Undo with `Remove-NetRoute`.
> Alternative: `tailscale set --accept-routes=false` (drops *all* advertised subnets —
> only safe while you are physically on the camera LAN).

### 1.4 Power / GPU settings

```powershell
# High-performance power plan — stops the package downclocking
powercfg /setactive SCHEME_MIN

# Optional: force the app onto the high-performance GPU
# Settings → System → Display → Graphics → add gstream_VMS.exe → High performance
```

---

## 2. Settings to VERIFY (not change)

### 2.1 App settings — should be **empty**

```powershell
$k = Get-Item 'HKCU:\Software\GuardX\VMS' -ErrorAction SilentlyContinue
if ($k) { "overrides: [" + ($k.GetValueNames() -join ', ') + "]" } else { "none (clean)" }
```

**Expected: `[]` — no overrides.** The code defaults are already the fastest configuration:

| Setting | Default (leave alone) | Only change if… |
|---|---|---|
| `video_backend` | `gstreamer` | never — `qmediaplayer` is the 1 s+ fallback |
| `rtsp_transport` | `udp` | packet loss on a *wired* link (shouldn't happen) |
| `rtsp_jitter_ms` | `0` | visible artifacts; try `50` before anything larger |
| `sync_channels` | `true` | set `false` for the absolute lowest latency, unaligned tiles |
| `decoder` | `auto` (d3d12 HW) | A/B test: `software`, `d3d11`, `d3d12` |

> Use `run.ps1 <preset>` rather than editing these by hand — it sets them cleanly and
> clears the others. The table is just so you know what each key does.

> Delete a key to restore its default — do not set it explicitly:
> `Remove-ItemProperty -Path 'HKCU:\Software\GuardX\VMS' -Name rtsp_transport`

### 2.2 Clock — do this before measuring

```powershell
# Run as Administrator. Without this every latency reading is ~0.95 s too low.
w32tm /resync
w32tm /stripchart /computer:pool.ntp.org /dataonly /samples:4
```

**If the offset is under ±50 ms, set `OFFSET = 0` in `docs/analyze_latency.py`.**
Otherwise put the measured offset there.

### 2.3 Camera — should need no writes

On launch you should now see exactly this:

```
[CameraTuner] 인코더 설정이 이미 최적 — 요청 없음 (기동 지연 0)
```

If instead it prints `변경 필요 N 건`, something reset the camera — let it apply, then
restart the VMS so the next run starts clean.

---

## 3. Run procedure

```powershell
$env:PATH = "$env:LOCALAPPDATA\Programs\gstreamer\1.0\msvc_x86_64\bin;$env:PATH"
cd <build-dir>
.\gstream_VMS.exe 2> lan_test.log
```

Let it run **at least 5 minutes** — latency creep only appears over time, and short runs
flattered TCP in earlier tests.

Then:

```powershell
python <repo>\VMS\docs\analyze_latency.py lan_test.log "wired LAN, UDP, latency=0"
```

### What to record

| Measurement | How |
|---|---|
| median / p10 / p90 / max latency | analyze script output |
| per-channel medians + spread | analyze script output |
| reconnect count | analyze script output |
| **Channel sync check** | screenshot all 4 tiles — **the OSD clocks must read identically** |
| VMS CPU % | Task Manager → Details → gstream_VMS (NOT the total) |
| VMS GPU % | `Get-Counter '\GPU Engine(*)\Utilization Percentage'`, filter by the VMS pid |
| Wired ping min/avg/max | `Test-Connection 192.168.0.3 -Count 20` |

### Success criteria

| Metric | Target | Last WiFi result |
|---|---|---|
| Median latency | **≤ 300 ms** | 582 ms |
| p90 | ≤ 500 ms | 1159 ms |
| Max | ≤ 1000 ms | 2717 ms |
| Reconnects | 0 | 0 ✅ |
| Channel OSD alignment | identical | identical ✅ |
| VMS CPU | ≤ 7% | 6.6% |
| VMS GPU 3D | ≤ 10% | 9.9% |

---

### 3b. Automated pass (2026-08-10) — `tools/acceptance.py`

The manual steps above still stand, but most of them now have a machine-checked
equivalent that prints the **measured value next to every verdict**.

```powershell
python VMS\tools\acceptance.py all --sec 60     # route + devices + VMS log + packets
python VMS\tools\acceptance.py latency --pick   # once: drag over the video tile
python VMS\tools\acceptance.py latency          # glass-to-glass vs the 300 ms target
```

| Mode | What it judges |
|---|---|
| `net` | on-link TTL, loss, RTT p50/p95, RTSP 554 + HTTPS 443 reachability, egress interface |
| `devices` | TLS pin vs `certs/*.pem`, SUNAPI deviceinfo, MQTT node liveness per RPi |
| `vms` | launches the app, closes it with **WM_CLOSE** (never a kill — ghost RTSP sessions), then judges 14 things from the log: 4-channel start, no-frame reconnects, latency-creep restarts, in-session creep span, ONVIF doc/frame/box counts, box↔frame matching, camera poll state, RPi A verdict, Qt warnings |
| `--capture` | tshark alongside: retransmission %, zero-window count, throughput |
| `latency` | **glass-to-glass**, see below |

**How `latency` measures g2g without a stopwatch:** it renders a 9-bit Gray-coded bar
pattern (20 ms tick) on screen, and the camera has to see that screen. It then reads
**both** the live pattern and the pattern inside the VMS tile **from the same screenshot** —
so the screenshot's own timing error cancels out. Two marker bars give an exposure-independent
threshold and a parity bar rejects frames caught mid-flip.

> [!warning] Two traps this tool hit while being built — worth knowing before trusting any checker
> - The VMS log is **cp949 with occasional broken bytes**. A strict decode fails, and a
>   checker that then falls back silently loses every Korean rule while ASCII rules still
>   match — i.e. it reports **"all PASS"** on a failing run.
> - `RPi A 점: 정상 → 끊김` contains the word "정상". Substring matching passed a
>   transition **into** the failed state. Read the arrow's right-hand side.

**RPi A liveness must exclude dummy zones.** Per `fire_zone_map.h` only zone 1 has real
hardware; zones 2–4 are demo dummies whose values the VMS generates itself. Counting them
makes a dead RPi A look alive — that was a real defect fixed on 2026-08-10.

---

## 4. "More GPU, less CPU" — where things stand

### Already on the GPU
- **H.264 decode** — `d3d12h264dec` hardware decoder (VideoDecode engine ~1%; low because
  4×640×480@15fps is only ~3% of the engine's capacity, *not* because it's unused)
- **NV12 → RGB colour conversion** — fragment shader
- **Scaling** — sampler, free
- **Compositing** — Qt RHI

### Still on the CPU (in rough order of cost)
1. **GPU→CPU→GPU round trip.** The decoder outputs `D3D12Memory`; we download to system
   memory and re-upload to Qt's D3D11 device. Removing this needs cross-device texture
   sharing (shared NT handles + fences). **The single biggest remaining item.**
2. RTP depacketisation + jitter buffer (GStreamer, unavoidable)
3. `DetectionFeed` HTTP polling every 200 ms
4. Box overlay painting — now only repaints when boxes actually move (see §6)

### If you want to push further tomorrow
Run **one channel fullscreen at 1080p** and measure CPU + GPU. That is the workload the GPU
path was built for; at 640×480 thumbnails it is running at ~2% of its design load, which is
why the CPU win looked small (8.3% → 6.6%).

---

## 5. Startup time — you were right, and it's fixed

**Your observation:** starting all 4 streams takes a long time; once running they behave.

**Cause found:** `CameraTuner` was firing **20 sequential SUNAPI requests** at launch, each
needing digest auth (2 round trips) — ~40 round trips, serialised, competing with RTSP setup
for the same link. 8 of those (`EntropyCoding=CAVLC`) were **rejected every single time**
because this camera model doesn't support it.

**Fixed (already built):**
- The tuner now reads current state first and **writes only what differs**. Steady state is
  now **zero write requests** — verified: `요청 없음 (기동 지연 0)`.
- The always-failing CAVLC request was removed entirely.

**On your caching idea — half right.** You can't cache live video. But the *configuration
state* absolutely can be, and that was the real cost. That's now done.

**What still takes time at startup (unavoidable or not worth it):**

| Cost | Amount | Fixable? |
|---|---|---|
| RTSP handshake: OPTIONS → DESCRIBE → SETUP ×2 → PLAY | ~5–10 RTTs × 4 channels | No — but on wired LAN an RTT is ~1 ms instead of ~400 ms, so this collapses to nothing |
| Wait for first I-frame | up to 1 s (GOV 15 @ 15 fps) | Only by shortening GOV, which costs bandwidth |
| D3D12 device + decoder init per channel | a few hundred ms | Shareable, but complex for little gain |

> Expect startup to feel dramatically faster on wired simply because every round trip
> shrinks from ~400 ms to ~1 ms.

---

## 6. Changes made since the last report

| Change | Effect | Verified |
|---|---|---|
| `CameraTuner` writes only on difference | startup: 20 requests → **0** | ✅ log confirms |
| Removed always-rejected CAVLC request | 8 fewer failed requests per launch | ✅ |
| Overlay repaints only when boxes move | **GPU 3D 13.7% → 9.9% (−28%)** | ✅ measured |

The overlay is a translucent widget, so every repaint forces Qt to re-composite the whole
window. It was updating 33×/second per channel regardless of whether anything moved.

---

## 7. Troubleshooting

| Symptom | Likely cause | Action |
|---|---|---|
| No `glass-to-arrival` lines in the log | RTCP not arriving | Tailscale still running, or a firewall blocking UDP → check §1.1 |
| Tiles show `연결 중…` and stay black | Camera refused the stream / link saturated | Check `Error binding` in the log; count concurrent streams |
| Latency grows steadily over minutes | TCP backlog | Confirm transport really is UDP (registry should be empty) |
| One tile consistently ~500 ms behind | **known: ch2 does this on every run** | Not yet diagnosed — record it, don't chase it during the test |
| Latency reads negative | Clock offset | Run `w32tm /resync`, set `OFFSET` in the analyse script |
| All tiles black, no status text | Shaders failed to load | Look for `셰이더 로드 실패` in the log |

---

## 8. After the test

1. Append the wired run to the benchmark report as **run G**.
2. If median ≤ 300 ms → the streaming work is done; move on.
3. If median > 400 ms on wired, the remaining suspects in order:
   camera encoder latency → switch profiles to 30 fps → measure a single channel alone to
   separate per-stream cost from aggregate load.

---

*Written 2026-07-21 for the 2026-07-22 wired test. Every figure here was measured on this
machine, not estimated.*
