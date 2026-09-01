import re, statistics, sys, collections

path = sys.argv[1]
label = sys.argv[2] if len(sys.argv) > 2 else "run"
# Clock-correction in ms. Pass as 3rd arg. After `w32tm /resync` on a synced
# host the offset is ~0, so 0 is the default. Only use a nonzero value if
# `w32tm /stripchart` shows the host clock is off; pass that measured value.
OFFSET = int(sys.argv[3]) if len(sys.argv) > 3 else 0

# WARNING (2026-08-03): since 2026-07-31 the backend already subtracts its own
# estimated camera-clock offset before logging (PipelineStats::observe_net).
# The logged value is therefore *relative* — "excess over the fastest frame in
# the window", floor 0 — not an absolute glass-to-arrival time. Passing a
# nonzero OFFSET here double-corrects. Leave it at 0 unless you are re-parsing
# logs captured before 2026-07-31. Absolute latency needs camera+PC NTP sync.

per_ch = collections.defaultdict(list)
allv = []
# 로그는 Qt가 콘솔 코드페이지(CP949)로 쓴다 — utf-8로 읽으면 한글이 깨져
# "재접속" 카운터가 항상 0이 되는 버그가 있었다 (2026-08-04 발견: B4 baseline의
# ch1 재접속 루프가 이 때문에 안 보였다). cp949 우선, 실패 시 utf-8 폴백.
with open(path, "rb") as f:
    raw = f.read()
# strict 디코드는 한 바이트만 어긋나도 통째로 폴백된다 — 두 인코딩을 모두
# lenient로 시도해 "재접속"이 더 많이 살아남는 쪽을 쓴다.
_cands = [raw.decode("cp949", errors="replace"),
          raw.decode("utf-8", errors="replace")]
text = max(_cands, key=lambda t: t.count("재접속") + t.count("글래스"))

# "glass-to-arrival" since 2026-08-03; "glass-to-display" was the old name for
# the same measurement point (appsink callback). Both accepted so pre-rename
# logs still parse.
for m in re.finditer(r"ch\s+(\d+)\s+glass-to-(?:arrival|display)\s+(-?\d+)\s*ms",
                     text):
    ch, v = int(m.group(1)), int(m.group(2)) + OFFSET
    per_ch[ch].append(v)
    allv.append(v)

recon = len(re.findall(r"재접속", text))

if not allv:
    print(f"{label}: no samples")
    sys.exit(0)

s = sorted(allv)
print(f"=== {label} ===")
print(f"n={len(allv)}  reconnects={recon}  (clock offset applied: +{OFFSET}ms)")
print(f"median={statistics.median(allv):.0f}ms  p10={s[len(s)//10]}ms  "
      f"p90={s[len(s)*9//10]}ms  min={min(allv)}  max={max(allv)}")
for ch in sorted(per_ch):
    v = per_ch[ch]
    print(f"  ch{ch}: n={len(v):3d} median={statistics.median(v):6.0f}ms")

# 채널 간 정렬 품질: 각 채널 중앙값의 최대 격차
meds = [statistics.median(v) for v in per_ch.values() if len(v) >= 3]
if len(meds) > 1:
    print(f"channel spread (max-min of medians) = {max(meds)-min(meds):.0f}ms")

# ---- 부드러움(smoothness) ----
# 로그: "ch N smoothness: fps X.X jitter Y ms maxgap Z ms"
sm_fps = collections.defaultdict(list)
sm_jit = collections.defaultdict(list)
sm_gap = collections.defaultdict(list)
for m in re.finditer(
        r"ch\s+(\d+)\s+smoothness:\s+fps\s+([\d.]+)\s+jitter\s+(\d+)\s*ms\s+maxgap\s+(\d+)", text):
    ch = int(m.group(1))
    sm_fps[ch].append(float(m.group(2)))
    sm_jit[ch].append(int(m.group(3)))
    sm_gap[ch].append(int(m.group(4)))

if sm_fps:
    print("smoothness (30fps target; jitter=stddev of frame gaps; maxgap=worst stutter):")
    all_fps, all_jit, all_gap = [], [], []
    for ch in sorted(sm_fps):
        f, j, g = sm_fps[ch], sm_jit[ch], sm_gap[ch]
        all_fps += f; all_jit += j; all_gap += g
        print(f"  ch{ch}: fps {statistics.mean(f):4.1f}  "
              f"jitter {statistics.mean(j):3.0f}ms  maxgap {max(g):4d}ms")
    print(f"  ALL: fps {statistics.mean(all_fps):4.1f}  "
          f"jitter {statistics.mean(all_jit):3.0f}ms  maxgap {max(all_gap):4d}ms")
