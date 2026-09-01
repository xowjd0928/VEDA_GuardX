#!/usr/bin/env python3
"""GuardX TOIMIC 감지기 벤치마크 — 라벨 데이터셋 없이 돌아간다.

재는 것 (데이터셋 불필요):
  preprocess   48k S32 스테레오 청크 → 16k 모노 float32 (하이패스 + 리샘플)
  inference    YAMNet 한 창(0.975s) 추론
  decide       융합·히스테리시스·N-of-M 투표
  hop total    위 셋을 합친 한 홉(0.25s 주기) 처리 시간
  cost         프로세스 CPU 시간과 RSS

재지 않는 것: **정확도**. 비명·총성을 실제로 맞히는지는 라벨이 붙은 음원이
있어야 한다. 붙일 자리는 --dataset 에 이미 뚫어 두었다 (benchmark/README.md
"데이터셋 붙이기" 절).

실행 (RPi C, detector 와 같은 venv 안에서):
    python bench_toimic.py                     합성 입력으로 단계별 측정
    python bench_toimic.py --iters 200
    python bench_toimic.py --live 30           실제 마이크로 30초 (RPi C 전용)
    python bench_toimic.py --json out.json
    python bench_toimic.py --dataset ./clips   정확도 (데이터셋이 생긴 뒤)

개발 PC 에서는 detector 의 의존성(numpy/scipy/ai_edge_litert)이 없어 임포트에서
멈춘다. 그때는 무엇이 없는지 찍고 끝낸다 — "도구는 준비됐고 실기가 없다"를
0 개의 측정값과 구분하기 위해서다.
"""

import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common"))
# detector.py 를 그대로 임포트한다. 여기서 재구현하면 "벤치마크는 빠른데
# 실제 감지기는 느린" 상태를 못 잡는다.
DETECTOR_DIR = os.path.join(HERE, "..", "..", "rpi_c", "rpic_app", "toimic")
sys.path.insert(0, DETECTOR_DIR)

import bench_common as bc   # noqa: E402


def load_detector():
    try:
        import detector          # noqa: F401
        import numpy             # noqa: F401
        return detector
    except ImportError as e:
        print("TOIMIC 감지기를 임포트할 수 없다: %s" % e)
        print("  감지기 경로 : %s" % os.path.normpath(DETECTOR_DIR))
        print("  필요 패키지 : rpi_c/rpic_app/toimic/requirements.txt")
        print("  RPi C 의 venv 를 활성화한 뒤 다시 실행할 것.")
        print("  (개발 PC 에서는 여기까지가 정상이다 — 측정은 실기에서 한다)")
        return None


# ------------------------------------------------------------ 합성 입력

def make_chunks(detector, seconds, hop_sec):
    """감지기가 실제로 받는 것과 같은 모양의 raw 청크를 만든다.

    arecord 출력 그대로 = S32_LE, 48kHz, 2채널 인터리브. 내용은 배경소음
    수준의 잡음에 주기적 임펄스를 얹는다 — 게이트를 통과하는 창과 통과하지
    못하는 창이 섞여야 판정기까지 포함한 실제 비용이 나온다.
    """
    import numpy as np

    n = int(detector.IN_RATE * hop_sec)
    rng = np.random.default_rng(20260812)
    chunks = []
    for i in range(int(seconds / hop_sec)):
        mono = rng.normal(0.0, 0.02, n)
        if i % 8 == 3:                      # 가끔 임펄스 (crest 게이트 통과용)
            mono[n // 2:n // 2 + 900] += 0.6
        st = np.zeros((n, detector.CH), dtype=np.int32)
        st[:, 0] = (mono * 2147483647.0).astype(np.int32)
        chunks.append(st.tobytes())
    return chunks


# ------------------------------------------------------------ 단계별 측정

def bench_stages(detector, iters, warmup):
    import numpy as np

    print("모델 로드 중 ...")
    t0 = time.perf_counter()
    ym = detector.Yamnet()
    load_ms = (time.perf_counter() - t0) * 1000.0
    win = ym.win

    pre = detector.Preproc()
    fired = []
    dec = detector.Decider(lambda ev, score, inc: fired.append((ev, score, inc)))

    chunks = make_chunks(detector, (iters + warmup) * detector.HOP_SEC,
                         detector.HOP_SEC)

    ring = np.zeros(0, dtype=np.float32)
    pre_ms, inf_ms, dec_ms, hop_ms = [], [], [], []

    wall0 = time.perf_counter()
    for i, raw in enumerate(chunks):
        measure = i >= warmup     # 첫 몇 홉은 캐시·JIT 예열이라 버린다
        h0 = time.perf_counter()

        t = time.perf_counter()
        y = pre(raw)
        t_pre = time.perf_counter() - t

        ring = np.concatenate([ring, y])[-win:] if ring.size else y[-win:]
        if ring.size < win:
            continue

        w = detector.normalize(ring)
        t = time.perf_counter()
        _raw_scores, fused = ym.score(w)
        t_inf = time.perf_counter() - t

        now = time.time()
        t = time.perf_counter()
        for ev, score in fused.items():
            dec.feed(ev, score, now)
        t_dec = time.perf_counter() - t

        if measure:
            pre_ms.append(t_pre * 1000.0)
            inf_ms.append(t_inf * 1000.0)
            dec_ms.append(t_dec * 1000.0)
            hop_ms.append((time.perf_counter() - h0) * 1000.0)
    wall = time.perf_counter() - wall0

    stats = dict(preprocess=bc.summarise(pre_ms),
                 inference=bc.summarise(inf_ms),
                 decide=bc.summarise(dec_ms),
                 hop_total=bc.summarise(hop_ms))

    print("\n결과 — 단계별 (합성 입력)")
    print("  %-22s %.0f ms" % ("model load", load_ms))
    print("  %-22s %d samples (%.3f s @ %d Hz)"
          % ("window", win, win / float(detector.MODEL_RATE),
             detector.MODEL_RATE))
    print("  %-22s %.2f s" % ("hop period", detector.HOP_SEC))
    for key, label in (("preprocess", "preprocess"), ("inference", "inference"),
                       ("decide", "decide"), ("hop_total", "hop total")):
        bc.print_stats(label, "ms", stats[key])

    # 한 홉 처리 시간이 홉 주기를 넘으면 감지가 실시간을 못 따라간다.
    # 이게 이 벤치마크의 유일한 합격/불합격 판정이다.
    budget_ms = detector.HOP_SEC * 1000.0
    p95 = stats["hop_total"]["p95"]
    print("  %-22s %.1f %% of the %.0f ms hop budget%s"
          % ("realtime margin", p95 / budget_ms * 100.0, budget_ms,
             "   *** OVER BUDGET ***" if p95 >= budget_ms else ""))
    print("  %-22s %d" % ("alerts fired", len(fired)))

    sys_stat = bc.sysstat()
    bc.print_sysstat(sys_stat, wall)

    return dict(mode="stages", model_load_ms=load_ms, window=int(win),
                hop_sec=detector.HOP_SEC, profile=detector.PROFILE,
                stages=stats, alerts=len(fired), wall_s=wall, cost=sys_stat)


# --------------------------------------------------------------- 실기 측정

def bench_live(detector, seconds):
    """실제 ALSA 캡처로 같은 값을 잰다. RPi C 에서만 의미가 있다."""
    import numpy as np

    print("ALSA 캡처 시작 (%s) ..." % detector.ALSA_DEV)
    ym = detector.Yamnet()
    win = ym.win
    pre = detector.Preproc()
    fired = []
    dec = detector.Decider(lambda ev, score, inc: fired.append((ev, score, inc)))

    try:
        cap = detector.open_capture()
    except Exception as e:                      # noqa: BLE001
        print("  ! 캡처를 열 수 없다: %s" % e)
        print("  (마이크가 없는 기계다 — 실기에서 다시 실행할 것)")
        return None

    nbytes = int(detector.IN_RATE * detector.HOP_SEC) * detector.CH * 4
    ring = np.zeros(0, dtype=np.float32)
    pre_ms, inf_ms, hop_ms, read_ms = [], [], [], []

    wall0 = time.perf_counter()
    try:
        while time.perf_counter() - wall0 < seconds:
            t = time.perf_counter()
            raw = detector.read_exact(cap, nbytes)
            read_ms.append((time.perf_counter() - t) * 1000.0)
            if raw is None:
                print("  ! 캡처가 끊겼다")
                break

            h0 = time.perf_counter()
            t = time.perf_counter()
            y = pre(raw)
            pre_ms.append((time.perf_counter() - t) * 1000.0)

            ring = np.concatenate([ring, y])[-win:] if ring.size else y[-win:]
            if ring.size < win:
                continue

            t = time.perf_counter()
            _raw_scores, fused = ym.score(detector.normalize(ring))
            inf_ms.append((time.perf_counter() - t) * 1000.0)

            now = time.time()
            for ev, score in fused.items():
                dec.feed(ev, score, now)
            hop_ms.append((time.perf_counter() - h0) * 1000.0)
    finally:
        detector.close_capture(cap)
    wall = time.perf_counter() - wall0

    stats = dict(capture_read=bc.summarise(read_ms),
                 preprocess=bc.summarise(pre_ms),
                 inference=bc.summarise(inf_ms),
                 hop_total=bc.summarise(hop_ms))

    print("\n결과 — 실기 (%s)" % detector.ALSA_DEV)
    for key, label in (("capture_read", "capture read"),
                       ("preprocess", "preprocess"),
                       ("inference", "inference"),
                       ("hop_total", "hop total")):
        bc.print_stats(label, "ms", stats[key])
    print("  %-22s %d" % ("alerts fired", len(fired)))

    sys_stat = bc.sysstat()
    bc.print_sysstat(sys_stat, wall)
    return dict(mode="live", device=detector.ALSA_DEV, stages=stats,
                alerts=len(fired), wall_s=wall, cost=sys_stat)


# ------------------------------------------------------------- 정확도 (미도입)

def bench_dataset(detector, path):
    """라벨 데이터셋이 생겼을 때의 정확도 측정.

    기대 구조 (benchmark/README.md 참조):
        <path>/manifest.csv     relative_path,label     label ∈ scream|gunshot|none
        <path>/<relative_path>  16kHz 이상 WAV

    지금은 데이터셋이 없으므로 여기서 끝난다. 형식을 먼저 못 박아 두는 이유는,
    나중에 음원을 모을 때 무엇을 모아야 하는지가 정해져 있어야 하기 때문이다.
    """
    manifest = os.path.join(path, "manifest.csv")
    if not os.path.isfile(manifest):
        print("데이터셋이 없다: %s" % manifest)
        print("  형식: relative_path,label   (label = scream | gunshot | none)")
        print("  음원: 16kHz 이상 WAV, 모노 권장")
        print("  현재 라벨 데이터셋 미보유 — 정확도는 측정하지 않는다.")
        return None

    import csv
    import wave

    import numpy as np

    ym = detector.Yamnet()
    win = ym.win
    rows = []
    with open(manifest, newline="", encoding="utf-8") as f:
        for row in csv.reader(f):
            if len(row) >= 2 and not row[0].startswith("#"):
                rows.append((row[0].strip(), row[1].strip().lower()))

    hits = {ev: dict(tp=0, fp=0, fn=0) for ev in detector.FUSION}
    scored = 0
    for rel, label in rows:
        wav_path = os.path.join(path, rel)
        try:
            with wave.open(wav_path, "rb") as w:
                frames = w.readframes(w.getnframes())
                rate = w.getframerate()
                chans = w.getnchannels()
        except OSError as e:                      # noqa: BLE001
            print("  ! %s: %s" % (rel, e))
            continue

        x = np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0
        if chans > 1:
            x = x.reshape(-1, chans)[:, 0]
        if rate != detector.MODEL_RATE:
            from scipy.signal import resample_poly
            g = np.gcd(int(rate), detector.MODEL_RATE)
            x = resample_poly(x, detector.MODEL_RATE // g, int(rate) // g)

        # 가장 강한 창 하나로 판정한다 — 짧은 총성이 긴 무음에 희석되지 않게.
        best = {ev: 0.0 for ev in detector.FUSION}
        for start in range(0, max(1, len(x) - win + 1), win // 2):
            seg = x[start:start + win]
            if len(seg) < win:
                seg = np.pad(seg, (0, win - len(seg)))
            _raw, fused = ym.score(detector.normalize(seg.astype(np.float32)))
            for ev, s in fused.items():
                best[ev] = max(best[ev], s)
        scored += 1

        for ev in detector.FUSION:
            th = detector.EVENT_RULES[ev]["enter"]
            predicted = best[ev] >= th
            actual = (label == ev)
            if predicted and actual:
                hits[ev]["tp"] += 1
            elif predicted and not actual:
                hits[ev]["fp"] += 1
            elif not predicted and actual:
                hits[ev]["fn"] += 1

    print("\n결과 — 정확도 (%d clips, profile=%s)" % (scored, detector.PROFILE))
    for ev, h in hits.items():
        prec = h["tp"] / (h["tp"] + h["fp"]) if (h["tp"] + h["fp"]) else 0.0
        rec = h["tp"] / (h["tp"] + h["fn"]) if (h["tp"] + h["fn"]) else 0.0
        f1 = 2 * prec * rec / (prec + rec) if (prec + rec) else 0.0
        print("  %-10s tp %3d  fp %3d  fn %3d   precision %.2f  recall %.2f  "
              "f1 %.2f" % (ev, h["tp"], h["fp"], h["fn"], prec, rec, f1))
    return dict(mode="dataset", clips=scored, per_event=hits,
                profile=detector.PROFILE)


def main():
    ap = argparse.ArgumentParser(description="GuardX TOIMIC benchmark")
    ap.add_argument("--iters", type=int, default=120,
                    help="합성 입력 홉 수 (기본 120 ≈ 30초 분량)")
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--live", type=float, default=0,
                    help="실제 마이크로 N초 측정 (RPi C 전용)")
    ap.add_argument("--dataset", default=None,
                    help="라벨 데이터셋 경로 (정확도 측정)")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    detector = load_detector()
    if detector is None:
        return 2

    print("GuardX TOIMIC benchmark  (profile=%s)" % detector.PROFILE)

    if args.dataset:
        result = bench_dataset(detector, args.dataset)
    elif args.live > 0:
        result = bench_live(detector, args.live)
    else:
        result = bench_stages(detector, args.iters, args.warmup)

    if result and args.json:
        result["target"] = "toimic"
        bc.write_json(args.json, result)
    return 0 if result else 1


if __name__ == "__main__":
    sys.exit(main())
