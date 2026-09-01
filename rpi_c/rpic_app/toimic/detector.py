#!/usr/bin/env python3
# GuardX RPi C TOIMIC 오디오 감지기 v3
#
#   SPH0645(I2S) → 전처리 → 1단 DSP 게이트 → 2단 YAMNet 확증 → MQTT → VMS 팝업
#
# v2 대비 바뀐 것 (근거는 README.md "튜닝 근거" 절):
#   - 100Hz 하이패스        : 환풍기·공조 저주파 럼블 제거 (v2는 38Hz DC 블로커)
#   - 클래스 융합           : AudioSet 온톨로지의 형제 클래스 가중 합산 → 재현율↑
#   - 히스테리시스 + N-of-M : 임계 경계에서 깜빡이는 것 방지
#   - 게이트 통과 후 정규화 : 낮은 마이크 감도 보정 (게이트 전에 하면 오탐↑)
#   - 임펄스 창 정렬        : 20ms 총성이 975ms 창에 희석되는 문제 대응
#   - 방송 중 상시 감지     : 스피커 사본을 기준 신호로 받아 AEC 로 지운다
#                             (기준 신호가 없으면 예전처럼 재생 중 뮤트)
#   - 캡처 자동 복구        : arecord가 죽어도 재기동
#   - 프로파일/캘리브레이션 : demo·prod 전환, 현장 노이즈 플로어 실측
#
# 사용 (venv 활성화 상태에서):
#   python detector.py                 감지 시작
#   python detector.py --calibrate 30  30초간 배경소음 실측 → 권장 임계 출력
#   GUARDX_TOIMIC_PROFILE=prod python detector.py
#   GUARDX_TOIMIC_DEBUG_MUTE=1 python detector.py   뮤트 판정 상세 출력

import argparse
import csv
import glob
import json
import os
import re
import signal
import subprocess
import sys
import time
from collections import deque

import numpy as np
import paho.mqtt.client as mqtt
from ai_edge_litert.interpreter import Interpreter
from scipy.signal import butter, lfilter, resample_poly

import aec as aec_mod

# ===== 오디오 입력 =====
ALSA_DEV = os.environ.get("GUARDX_TOIMIC_ALSA", "plughw:CARD=MAX98357A,DEV=0")
IN_RATE, CH = 48000, 2          # SPH0645 네이티브 (S32_LE, 왼쪽 채널만 신호)
MODEL_RATE = 16000              # YAMNet 입력 규격
DOWN = IN_RATE // MODEL_RATE    # 48k → 16k 데시메이션 비 (=3)
HOP_SEC = 0.25                  # 추론 주기 = 최악 감지 지연의 하한
WIN_FALLBACK = 15600            # 0.975s @ 16kHz — 모델에서 못 읽을 때만 사용

# ===== 모델 =====
HERE = os.path.dirname(os.path.abspath(__file__))
MODEL = os.path.join(HERE, "yamnet.tflite")
CLASS_MAP = os.path.join(HERE, "yamnet_class_map.csv")

# ===== MQTT =====
# VMS(credentials.ini)가 붙는 브로커와 같은 곳에 쏴야 팝업이 뜬다 — RPi B가 기본.
MQTT_HOST = os.environ.get("GUARDX_MQTT_HOST", "172.20.33.251")
MQTT_PORT = int(os.environ.get("GUARDX_MQTT_PORT", "1883"))
ALERT_TOPIC = "guardx/alert/rpic"
ALERT_QOS = 1
ZONE_ID = int(os.environ.get("GUARDX_TOIMIC_ZONE", "1"))
CHANNEL = int(os.environ.get("GUARDX_TOIMIC_CHANNEL", "0"))

# ===== 자기간섭 차단 (AEC 폴백) =====
# 기본은 AEC 다 — 스피커로 나간 소리의 디지털 사본을 받아 마이크 신호에서
# 지우고, 재생 중에도 감지를 계속한다(aec.py). 방송 중에 난 비명과 사이렌
# 중에 난 총성이 제일 중요한데 예전 구조는 하필 그때 귀를 막았다.
#
# 아래 뮤트는 AEC 를 못 쓸 때의 폴백이다. AEC 없이 감지를 강행하면 스피커
# 소리를 비명으로 오인해 오탐이 쏟아진다 — 그건 못 잡는 것보다 나쁘다.
# 판정 방법은 speaker_running() 참조 — "RUNNING이면 재생 중"이 아니다.
SPEAKER_CARD = os.environ.get("GUARDX_TOIMIC_SPEAKER_CARD", "MAX98357A")
MUTE_RELEASE_SEC = 0.6          # 재생이 끝나도 잔향이 남으므로 조금 더 닫아둔다

# ===== 클래스 융합 (AudioSet 온톨로지 형제 클래스 가중 합산) =====
# 실제 비명은 Screaming 하나에 몰리지 않고 Shout/Yell 등으로 확신도가 분산된다.
# 핵심 클래스는 1.0, 인접할수록 낮은 가중치 — 인접 클래스만으로는 임계를
# 넘지 못하게 해서 재현율은 올리되 오탐은 억제한다.
FUSION = {
    "scream": {
        "Screaming": 1.00,
        "Shout": 0.80,
        "Yell": 0.80,
        "Children shouting": 0.60,
        "Whoop": 0.40,          # 환호성 — 단독으로는 임계 미달
    },
    "gunshot": {
        "Gunshot, gunfire": 1.00,
        "Machine gun": 0.90,
        "Fusillade": 0.90,
        "Artillery fire": 0.70,
        "Cap gun": 0.60,
        "Explosion": 0.50,
    },
}

# ===== 프로파일 =====
# demo : 시연 우선 — 놓치지 않는 쪽. 임계를 낮추고 확증 창을 줄인다.
# prod : 운영 우선 — 오탐 억제. 임계를 올리고 확증을 더 요구한다.
PROFILES = {
    "demo": dict(ENTER_TH=0.35, SUSTAIN_TH=0.25, VOTE_N=2, VOTE_M=3,
                 SUSTAIN_X=4.0, ABS_FLOOR=0.010, CREST_TH=8.0,
                 IMPULSE_MIN=0.05, COOLDOWN=5.0),
    "prod": dict(ENTER_TH=0.50, SUSTAIN_TH=0.35, VOTE_N=3, VOTE_M=4,
                 SUSTAIN_X=6.0, ABS_FLOOR=0.020, CREST_TH=9.0,
                 IMPULSE_MIN=0.08, COOLDOWN=15.0),
}
PROFILE = os.environ.get("GUARDX_TOIMIC_PROFILE", "demo")
if PROFILE not in PROFILES:
    # 조용히 demo로 떨어지면 prod로 돌고 있다고 착각한 채 운영하게 된다
    print("[경고] 알 수 없는 프로파일 '%s' — demo로 진행 (가능: %s)"
          % (PROFILE, ", ".join(PROFILES)))
    PROFILE = "demo"
P = dict(PROFILES[PROFILE])

# 개별 항목만 환경변수로 덮어쓰기 (현장 실측값 반영용)
for _k in list(P):
    _v = os.environ.get("GUARDX_TOIMIC_" + _k)
    if _v is None:
        continue
    try:
        P[_k] = type(P[_k])(_v)
    except ValueError:
        # 오타 하나로 traceback만 남기고 죽으면 systemd가 무한 재시작만 돈다
        print("[경고] GUARDX_TOIMIC_%s='%s' 해석 불가 — 기본값 %s 유지"
              % (_k, _v, P[_k]))

# ===== 이벤트별 판정 규칙 =====
# 비명과 총성은 시간 구조가 달라 같은 규칙을 쓰면 안 된다.
#   비명  : 수백 ms~수 초 지속 → 연속 창이 여러 개 나오므로 N-of-M 투표가 맞다.
#   총성  : 10~50ms 단발 → 정렬된 창이 **하나만** 생긴다. 여기에 2창을 요구하면
#           단발 총성은 영영 경보가 안 나간다. 대신 1창으로 하되 임계를 올리고,
#           crest 게이트(임펄스가 아니면 애초에 여기까지 못 온다)를 사전 확률로
#           삼아 오탐을 억제한다.
EVENT_RULES = {
    "scream": dict(enter=P["ENTER_TH"], sustain=P["SUSTAIN_TH"],
                   vote_n=P["VOTE_N"], vote_m=P["VOTE_M"]),
    "gunshot": dict(enter=min(0.95, P["ENTER_TH"] + 0.10), sustain=P["SUSTAIN_TH"],
                    vote_n=1, vote_m=2),
}

NF_ALPHA = 0.98         # 배경소음 기준선 적응 속도(1에 가까울수록 천천히)
NORM_TARGET = 0.70      # 정규화 목표 피크
NORM_MAX_GAIN = 20.0    # 과증폭 방지 상한

# ===== 하이패스 100Hz (2차 버터워스) =====
# SPH0645의 DC 오프셋 제거가 최소 요건이고, 화장실은 환풍기 럼블이 상시로
# 깔리므로 컷오프를 DC가 아니라 100Hz에 둔다. 비명(200Hz~)·총성(광대역)의
# 유효 대역은 건드리지 않는다.
HPF_B, HPF_A = butter(2, 100.0 / (IN_RATE / 2.0), btype="highpass")

_running = True


def _stop(_sig, _frm):
    global _running
    _running = False


# --------------------------------------------------------------- 스피커 감시

def speaker_status_paths(card_name):
    """MAX98357A 카드의 재생 서브스트림 status 파일 경로들."""
    idx = None
    try:
        with open("/proc/asound/cards") as f:
            for line in f:
                m = re.match(r"\s*(\d+)\s+\[([^\]]+)\]", line)
                if m and m.group(2).strip() == card_name:
                    idx = int(m.group(1))
                    break
    except OSError:
        return []
    if idx is None:
        return []
    return sorted(glob.glob("/proc/asound/card%d/pcm*p/sub*/status" % idx))


_OWNER_PID_RE = re.compile(r"owner_pid\s*:\s*(\d+)")
CLK_TCK = os.sysconf("SC_CLK_TCK")     # 보통 100 → 1틱=10ms
# RTP 수신기(guardx-broadcast-rtp.service)는 부팅 시 자동 실행되어 상시
# 켜져 있고, 방송이 없어도 스피커 장치를 계속 열어둔 채 대기한다. 그래서
# ALSA "state: RUNNING"은 "장치가 열려 있다"만 뜻하지 "실제로 소리가 나가고
# 있다"를 뜻하지 않는다 — 이것만 보면 감지가 영구히 뮤트된다.
# 대신 그 프로세스(owner_pid)의 CPU 사용량으로 구분한다: 대기 중엔 거의 0,
# 실제 Opus 디코드+리샘플+재생이 돌면 튄다.
CPU_ACTIVE_TICKS = int(os.environ.get("GUARDX_TOIMIC_CPU_ACTIVE_TICKS", "1"))
# 실제 방송은 폴링(0.25초)마다 연속으로 CPU가 튄다(실측: 20회+ 연속). 반면
# 커널 스케줄러 틱(10ms) 우연 하나가 유휴 상태에서 드물게 찍히는 건 단발성이다.
# 그래서 즉시 뮤트하지 않고 N번 연속 튀어야 인정한다 — 실제 방송 감지에는
# 지장 없이(연속으로 튀므로) 그 드문 단발 오판정만 걸러낸다.
CPU_ACTIVE_DEBOUNCE = int(os.environ.get("GUARDX_TOIMIC_CPU_DEBOUNCE", "2"))
DEBUG_MUTE = os.environ.get("GUARDX_TOIMIC_DEBUG_MUTE") == "1"


def speaker_running(paths, cpu_state):
    """paths 중 RUNNING 서브스트림의 owner_pid가 최근 CPU_ACTIVE_DEBOUNCE
    폴링 연속으로 CPU를 썼으면 True. cpu_state는 호출부가 유지해서 넘겨주는
    {pid: (마지막 utime+stime, 연속 활성 횟수)}."""
    seen = set()
    active = False
    for p in paths:
        try:
            with open(p) as f:
                text = f.read()
        except OSError:
            continue
        if "state: RUNNING" not in text:
            continue
        m = _OWNER_PID_RE.search(text)
        if not m:
            active = True   # PID를 못 읽으면 안전하게 재생 중으로 간주
            continue
        pid = m.group(1)
        seen.add(pid)
        try:
            with open("/proc/%s/stat" % pid) as f:
                fields = f.read().split()
            now_cpu = int(fields[13]) + int(fields[14])   # utime+stime (틱)
        except (OSError, IndexError, ValueError):
            active = True
            continue
        prev_cpu, streak = cpu_state.get(pid, (None, 0))
        delta = (now_cpu - prev_cpu) if prev_cpu is not None else None
        streak = streak + 1 if (delta is not None and delta >= CPU_ACTIVE_TICKS) else 0
        cpu_state[pid] = (now_cpu, streak)
        if streak >= CPU_ACTIVE_DEBOUNCE:
            active = True
        if DEBUG_MUTE:
            print("[뮤트판정] pid=%s delta=%s틱(%sms) 연속=%d/%d"
                  % (pid, delta, None if delta is None else delta * 1000 // CLK_TCK,
                     streak, CPU_ACTIVE_DEBOUNCE))
    # 더는 안 보이는 pid는 다음 비교가 왜곡되지 않게 정리
    for pid in [k for k in cpu_state if k not in seen]:
        del cpu_state[pid]
    return active


# ------------------------------------------------------------------- 신호처리

class Preproc:
    """48k S32 스테레오 raw → 16k 모노 float32. 청크 경계에서 끊기지 않게
    필터·리샘플러 상태를 모두 이어간다.

    하이패스는 lfilter의 zi로 상태가 이어지지만, resample_poly는 상태를
    유지하지 못한다. 청크마다 따로 부르면 내부 FIR(안티앨리어싱)이 매번
    0으로 패딩된 경계에서 시작해 0.25초마다 미세한 불연속(클릭)이 생기고,
    그게 crest factor를 부풀려 총성 오탐을 만든다. 그래서 직전 입력 꼬리를
    앞에 덧대 리샘플한 뒤 그만큼 잘라내는 overlap-save 방식을 쓴다.
    """

    PAD_IN = 192                    # DOWN의 배수. 리샘플 FIR(≈61탭) 과도구간을 덮는다
    PAD_OUT = PAD_IN // DOWN        # 덧댄 만큼 출력에서 잘라낸다

    def __init__(self):
        self.zi = np.zeros(max(len(HPF_A), len(HPF_B)) - 1, dtype=np.float64)
        self.tail = np.zeros(0, dtype=np.float64)

    def __call__(self, raw):
        st = np.frombuffer(raw, dtype=np.int32).reshape(-1, CH)
        x = st[:, 0].astype(np.float64) / 2147483648.0   # 왼쪽 채널만
        y, self.zi = lfilter(HPF_B, HPF_A, x, zi=self.zi)

        if self.tail.size:
            seg, drop = np.concatenate([self.tail, y]), self.PAD_OUT
        else:
            seg, drop = y, 0                             # 첫 호출은 덧댈 게 없다
        self.tail = y[-self.PAD_IN:].copy()
        return resample_poly(seg, 1, DOWN)[drop:].astype(np.float32)


def normalize(w):
    """게이트 통과 후에만 호출. 마이크 감도가 낮아 그대로면 확신도가 떨어진다.
    게이트 전에 걸면 무음까지 증폭해 오탐이 늘기 때문에 순서가 중요하다."""
    peak = float(np.max(np.abs(w)))
    if peak <= 1e-6 or peak >= NORM_TARGET:
        return w
    return (w * min(NORM_TARGET / peak, NORM_MAX_GAIN)).astype(np.float32)


# ---------------------------------------------------------------------- 캡처

def open_capture():
    return subprocess.Popen(
        ["arecord", "-q", "-D", ALSA_DEV, "-f", "S32_LE",
         "-r", str(IN_RATE), "-c", str(CH), "-t", "raw"],
        stdout=subprocess.PIPE)


def close_capture(proc):
    """종료 후 반드시 회수한다 — wait()를 빼먹으면 재기동이 반복될 때
    좀비 프로세스가 쌓인다."""
    if proc is None:
        return
    try:
        proc.terminate()
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            proc.kill()
            proc.wait(timeout=2)
        except Exception:
            pass
    except Exception:
        pass
    finally:
        try:
            if proc.stdout:
                proc.stdout.close()
        except Exception:
            pass


def read_exact(proc, nbytes):
    """arecord가 죽거나 짧게 주면 None — 호출부가 재기동한다."""
    if proc is None or proc.stdout is None:
        return None
    buf = b""
    while len(buf) < nbytes:
        chunk = proc.stdout.read(nbytes - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


# ---------------------------------------------------------------------- 모델

class Yamnet:
    def __init__(self):
        with open(CLASS_MAP, newline="") as f:
            names = [r[2] for i, r in enumerate(csv.reader(f)) if i > 0]
        self.names = names
        self.interp = Interpreter(model_path=MODEL)
        self.interp.allocate_tensors()
        self.inp = self.interp.get_input_details()[0]
        self.out = self.interp.get_output_details()[0]
        w = int(self.inp["shape"][-1])
        self.win = w if w > 1000 else 15600          # 0.975s @ 16kHz

        idx_of = {n: i for i, n in enumerate(names)}
        self.groups = {}
        for ev, members in FUSION.items():
            pairs = [(idx_of[n], wt) for n, wt in members.items() if n in idx_of]
            missing = [n for n in members if n not in idx_of]
            if missing:
                print("[경고] 클래스맵에 없는 이름: %s" % ", ".join(missing))
            self.groups[ev] = pairs

    def score(self, w):
        self.interp.set_tensor(self.inp["index"],
                               w.reshape(self.inp["shape"]).astype(np.float32))
        self.interp.invoke()
        raw = self.interp.get_tensor(self.out["index"]).reshape(-1)
        fused = {ev: min(1.0, float(sum(raw[i] * wt for i, wt in pairs)))
                 for ev, pairs in self.groups.items()}
        return raw, fused

    def top(self, raw, k=3):
        order = np.argsort(raw)[-k:][::-1]
        return ", ".join("%s %.2f" % (self.names[i], raw[i]) for i in order)


# -------------------------------------------------------------------- 판정기

class Decider:
    """히스테리시스 + N-of-M 투표. 규칙은 이벤트별로 다르다(EVENT_RULES)."""

    def __init__(self, publish):
        self.publish = publish
        self.st = {ev: dict(active=False,
                            votes=deque(maxlen=EVENT_RULES[ev]["vote_m"]),
                            last_alert=0.0, incident=0)
                   for ev in FUSION}

    def feed(self, ev, score, now):
        s = self.st[ev]
        r = EVENT_RULES[ev]
        th = r["sustain"] if s["active"] else r["enter"]
        s["votes"].append(1 if score >= th else 0)

        if not s["active"]:
            if sum(s["votes"]) >= r["vote_n"] and now - s["last_alert"] > P["COOLDOWN"]:
                s["active"] = True
                s["incident"] += 1
                s["last_alert"] = now
                s["votes"].clear()
                self.publish(ev, score, s["incident"])
        elif len(s["votes"]) == s["votes"].maxlen and sum(s["votes"]) == 0:
            # 유지 임계 아래로 VOTE_M창 연속 → 에피소드 종료.
            # VMS 팝업은 20초 자동 닫힘이라 clear 이벤트를 따로 쏘지 않는다
            # (쏘면 팝업이 다시 뜬다). 다음 에피소드를 받을 준비만 한다.
            s["active"] = False
            s["votes"].clear()

    def miss(self, now):
        for ev in self.st:
            self.feed(ev, 0.0, now)


# ------------------------------------------------------------------ 캘리브레이션

def calibrate(seconds, win):
    """배경소음을 실측해 게이트 임계를 권장한다.

    측정 창은 감지기와 **똑같은 길이(win)** 여야 한다. 0.25초 청크로 재면
    창이 짧아 피크가 작게 잡히고, crest factor가 실제 감지 때보다 낮게
    나와 권장값이 어긋난다."""
    print("[캘리브레이션] %d초간 배경소음 측정 — 조용히 해주세요." % seconds)
    pre = Preproc()
    proc = open_capture()
    frames = int(IN_RATE * HOP_SEC)
    rms_list, crest_list = [], []
    buf = np.zeros(0, dtype=np.float32)
    t0 = time.time()
    while time.time() - t0 < seconds and _running:
        raw = read_exact(proc, frames * 4 * CH)
        if raw is None:
            break
        buf = np.concatenate([buf, pre(raw)])
        if len(buf) < win:
            continue
        buf = buf[-win:]                       # 감지기와 동일한 슬라이딩 창
        rms = float(np.sqrt(np.mean(buf ** 2)))
        peak = float(np.max(np.abs(buf)))
        if rms <= 0.0:
            continue                           # 디지털 무음 — crest가 무의미
        rms_list.append(rms)
        crest_list.append(peak / rms)
        print("  rms=%.5f crest=%.1f    " % (rms, crest_list[-1]), end="\r")
    close_capture(proc)

    if not rms_list:
        print("\n[실패] 오디오를 못 읽었습니다. ALSA 장치를 확인하세요: %s" % ALSA_DEV)
        return 1

    a = np.array(rms_list)
    p50, p95, mx = np.percentile(a, 50), np.percentile(a, 95), a.max()
    crest_p95 = np.percentile(np.array(crest_list), 95)
    print("\n\n[결과] 표본 %d개" % len(a))
    print("  배경 RMS  중앙값 %.5f / p95 %.5f / 최대 %.5f" % (p50, p95, mx))
    print("  배경 crest p95 %.1f" % crest_p95)
    print("\n[권장] 아래를 systemd 유닛 Environment= 에 넣으세요:")
    print("  GUARDX_TOIMIC_ABS_FLOOR=%.4f" % (p95 * 4.0))   # 배경 p95 + 12dB
    print("  GUARDX_TOIMIC_CREST_TH=%.1f" % max(8.0, crest_p95 * 1.5))
    print("\n  (ABS_FLOOR = 배경 p95 + 12dB — 배경소음이 게이트를 못 열게 하는 하한)")
    return 0


# ------------------------------------------------------------------------ 본체

def main():
    ap = argparse.ArgumentParser(description="GuardX TOIMIC 오디오 감지기")
    ap.add_argument("--calibrate", nargs="?", type=int, const=30, default=None,
                    metavar="초", help="배경소음 실측 후 권장 임계 출력 (기본 30초)")
    args = ap.parse_args()

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    if args.calibrate is not None:
        # 감지기와 같은 창 길이로 재야 권장값이 맞는다. 모델을 실제로 열어
        # 창 길이를 가져오면 둘이 어긋날 일이 없다(로드 1초면 끝난다).
        try:
            win = Yamnet().win
        except Exception as e:
            win = WIN_FALLBACK
            print("[경고] 모델 로드 실패(%s) — 창 길이 %d 가정" % (e, win))
        return calibrate(args.calibrate, win)

    net = Yamnet()
    WIN = net.win

    # --- MQTT (Wi-Fi 끊겨도 백그라운드 스레드가 재접속) ---
    try:
        cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                          client_id="rpic-audio-detector")
    except Exception:
        cli = mqtt.Client(client_id="rpic-audio-detector")
    cli.on_connect = lambda *a, **k: print("[MQTT] 연결됨 — %s:%d" % (MQTT_HOST, MQTT_PORT))
    cli.on_disconnect = lambda *a, **k: print("[MQTT] 끊김 — 자동 재접속 시도")
    cli.reconnect_delay_set(min_delay=1, max_delay=30)
    try:
        cli.connect_async(MQTT_HOST, MQTT_PORT, 60)
    except Exception as e:
        print("[MQTT] 초기 연결 예약 실패(%s) — 감지는 계속" % e)
    cli.loop_start()

    seq = [0]

    def publish(ev, conf, incident):
        p = {"node_id": "rpic", "timestamp": int(time.time() * 1000),
             "seq": seq[0], "event": ev, "zone_id": ZONE_ID, "channel": CHANNEL,
             "incident_id": incident, "severity": "critical",
             "confidence": round(float(conf), 3), "source": "audio"}
        cli.publish(ALERT_TOPIC, json.dumps(p), qos=ALERT_QOS)
        seq[0] += 1
        print("  [경보] %s conf=%.2f incident=%d → %s" % (ev, conf, incident, ALERT_TOPIC))

    dec = Decider(publish)

    # AEC 가 되면 재생 중에도 감지를 계속한다. 안 되면 예전처럼 뮤트한다 —
    # 그래서 뮤트 감시 경로는 AEC 여부와 무관하게 항상 준비해 둔다.
    aec = aec_mod.AecStage()
    spk_paths = speaker_status_paths(SPEAKER_CARD)
    if not aec.available():
        print("[AEC] 비활성 — %s" % (aec.reason or "사유 불명"))
        if not spk_paths:
            print("[경고] %s 재생 서브스트림을 못 찾음 — 뮤트 폴백도 비활성"
                  % SPEAKER_CARD)

    print("[TOIMIC v3] 프로파일=%s 창=%d" % (PROFILE, WIN))
    for ev, r in EVENT_RULES.items():
        print("           %-8s 진입=%.2f 유지=%.2f 투표=%d/%d"
              % (ev, r["enter"], r["sustain"], r["vote_n"], r["vote_m"]))
    print("           게이트: 하한=%.4f 배수=%.1f crest=%.1f / 뮤트감시 %d개"
          % (P["ABS_FLOOR"], P["SUSTAIN_X"], P["CREST_TH"], len(spk_paths)))
    print("           재생 중 동작: %s"
          % ("AEC 로 반향 제거 후 상시 감지" if aec.available()
             else "감지 정지(뮤트)"))

    frames = int(IN_RATE * HOP_SEC)
    need = frames * 4 * CH
    # 창 정렬에는 피크 뒤쪽으로 0.75창이 더 필요하다. 3창을 들고 있으면
    # 피크가 버퍼 맨 끝에 걸린 최악의 경우에도 3홉 안에 반드시 정렬된다.
    BUF_MAX = WIN * 3
    PEND_MAX_HOPS = 4            # 그래도 안 되면 포기(대기 상태에 갇히지 않게)

    pre = Preproc()
    proc = open_capture()
    buf = np.zeros(0, dtype=np.float32)
    buf_start = 0                # buf[0]의 절대 샘플 인덱스
    noise_floor = None
    pending_peak = None          # 정렬 대기 중인 임펄스의 절대 피크 위치
    pending_hops = 0
    muted_until = 0.0
    last_status = 0.0
    backoff = 1.0
    cpu_state = {}                # speaker_running()의 pid별 이전 CPU 틱

    while _running:
        raw = read_exact(proc, need)
        if raw is None:                                  # 캡처 자동 복구
            if not _running:
                break
            print("[캡처] arecord 중단 — %.0f초 후 재기동" % backoff)
            close_capture(proc)
            time.sleep(backoff)
            backoff = min(backoff * 2, 30.0)
            try:
                proc = open_capture()
            except OSError as e:
                print("[캡처] 재기동 실패(%s) — 계속 재시도" % e)
                proc = None
                continue
            # 스트림이 끊겼으므로 필터·버퍼·정렬 대기 상태를 모두 초기화한다.
            # (pending_peak을 남기면 옛 절대 인덱스로 엉뚱한 구간을 정렬한다)
            pre = Preproc()
            buf_start += len(buf)
            buf = np.zeros(0, dtype=np.float32)
            pending_peak = None
            pending_hops = 0
            continue
        backoff = 1.0

        w16 = pre(raw)
        # 반향 제거는 **게이트보다 먼저** 건다. 게이트(rms·crest)가 보는
        # 것이 마이크 원본이면 스피커 소리만으로 게이트가 열려, 지운 뒤
        # 조용한 신호를 굳이 YAMNet 에 넣는 낭비가 생긴다.
        w16 = aec.process(w16)
        buf = np.concatenate([buf, w16])
        if len(buf) > BUF_MAX:                           # 오래된 앞부분 버림
            drop = len(buf) - BUF_MAX
            buf = buf[drop:]
            buf_start += drop

        now = time.time()

        # ---- 자기간섭 차단 (AEC 를 못 쓸 때의 폴백) ----
        # AEC 가 돌면 여기를 통째로 건너뛴다 — 재생 중에도 감지를 계속하는
        # 것이 이번 변경의 목적이고, 뮤트는 그걸 정면으로 막는다.
        #
        # 길이 검사보다 먼저 둔다 — 버퍼가 덜 찼을 때도 폴링이 규칙적으로
        # 돌아야 CPU delta 비교(speaker_running)가 어긋나지 않는다.
        if not aec.available() and speaker_running(spk_paths, cpu_state):
            muted_until = now + MUTE_RELEASE_SEC
        if now < muted_until:
            # 버퍼를 비운다 — 유예(0.6초)가 창 길이(0.975초)보다 짧아서, 그냥
            # 두면 뮤트가 풀린 직후 창에 방송 음성이 남아 그걸 분류한다.
            # 비우면 해제 후 깨끗한 1창이 다 찰 때까지 자연히 기다리게 된다.
            buf_start += len(buf)
            buf = np.zeros(0, dtype=np.float32)
            # 노이즈 플로어도 얼린다 — 방송 음량으로 기준선이 올라가면
            # 방송이 끝난 뒤 한동안 진짜 비명을 놓친다.
            pending_peak = None
            pending_hops = 0
            dec.miss(now)
            if now - last_status > 5.0:
                print("[뮤트] 스피커 재생 중 — 감지 정지")
                last_status = now
            continue

        if len(buf) < WIN:
            continue

        tail = buf[-WIN:]
        rms = float(np.sqrt(np.mean(tail ** 2)))
        peak = float(np.max(np.abs(tail)))
        crest = peak / (rms + 1e-9)
        if noise_floor is None:
            noise_floor = rms

        # ---- 1단: DSP 게이트 (CPU 거의 0) ----
        loud = rms > max(noise_floor * P["SUSTAIN_X"], P["ABS_FLOOR"])
        impulse = (crest > P["CREST_TH"]) and (peak > P["IMPULSE_MIN"])

        if impulse and pending_peak is None:
            # 임펄스 창 정렬: 20ms 총성이 975ms 창 끝에 걸리면 희석돼 확신도가
            # 낮게 나온다. 피크가 창의 앞쪽 25% 지점에 오도록 뒤 소리를 조금
            # 더 모은 뒤 추론한다 (최대 3홉 = 750ms 추가 지연).
            pending_peak = buf_start + (len(buf) - WIN) + int(np.argmax(np.abs(tail)))
            pending_hops = 0

        aligned = None
        if pending_peak is not None:
            start = pending_peak - int(0.25 * WIN)
            pending_hops += 1
            if start >= buf_start and start + WIN <= buf_start + len(buf):
                aligned = buf[start - buf_start:start - buf_start + WIN]
                pending_peak = None
                pending_hops = 0
            elif pending_hops >= PEND_MAX_HOPS:          # 정렬 실패 — 포기
                pending_peak = None
                pending_hops = 0

        if aligned is None and not loud:
            # 게이트 닫힘 — 조용하고 임펄스도 없을 때만 배경 기준선을 갱신한다
            # (임펄스가 낀 구간으로 갱신하면 기준선이 들려 다음 소리를 놓친다)
            if not impulse and pending_peak is None:
                noise_floor = NF_ALPHA * noise_floor + (1 - NF_ALPHA) * rms
            dec.miss(now)
            if now - last_status > 5.0:
                print("[대기] rms=%.5f floor=%.5f crest=%.1f" % (rms, noise_floor, crest))
                last_status = now
            continue

        # ---- 2단: YAMNet 확증 (게이트가 열렸을 때만) ----
        win = aligned if aligned is not None else tail
        why = "임펄스(정렬)" if aligned is not None else "지속음"
        raw_scores, fused = net.score(normalize(win))
        print("[게이트:%s] rms=%.5f crest=%.1f | %s | 융합 %s"
              % (why, rms, crest, net.top(raw_scores),
                 " ".join("%s=%.2f" % (k, v) for k, v in fused.items())))
        for ev, score in fused.items():
            dec.feed(ev, score, now)
        last_status = now

    close_capture(proc)
    aec.close()
    cli.loop_stop()
    try:
        cli.disconnect()
    except Exception:
        pass
    print("\n[종료]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
