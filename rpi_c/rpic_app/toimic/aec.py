#!/usr/bin/env python3
"""GuardX TOIMIC 음향 반향 제거(AEC).

방송이나 사이렌이 나가는 동안에도 비명·총성을 잡으려면, 마이크가 되받은
스피커 소리를 지워야 한다. 예전에는 재생 중 감지를 통째로 껐다 — 안전한
선택이었지만 하필 그때 난 비명을 영영 못 잡는다.

기준 신호는 마이크로 되잡지 않고 **소리를 낸 쪽이 디지털 사본을 보낸다**
(shared/audio_ref_protocol.h). 그래서 이 파일은 UDP 로 오는 16 kHz 모노
PCM 을 받아 speex 의 적응 필터에 먹이는 일만 한다.

── 없으면 없는 대로 ──
libspeexdsp 가 없거나 기준 신호가 안 오면 `available()` 이 False 다.
그때 호출부는 예전처럼 "재생 중에는 감지 정지"로 돌아간다. AEC 없이
감지를 강행하면 스피커 소리를 비명으로 오인해 오탐이 쏟아진다 — 그건
못 잡는 것보다 나쁘다.

의존: libspeexdsp1 (apt). pip 패키지가 아니라 ctypes 로 직접 부른다 —
aarch64 파이썬 휠 사정에 감지기가 묶이지 않게 하려는 것이다.

    sudo apt install -y libspeexdsp1
"""

import ctypes
import ctypes.util
import os
import socket

import numpy as np

# shared/audio_ref_protocol.h 와 같은 값이어야 한다. 이 파일은 C 헤더를 읽지
# 않으므로(파이썬 쪽 의존을 늘리지 않으려고) 그대로 옮겨 적는다 —
# 액추에이터 토픽 상수를 양쪽이 각자 들고 있는 것과 같은 관례다.
REF_PORT = int(os.environ.get("GUARDX_AUDIO_REF_PORT", "5005"))
REF_RATE = 16000

# speex 한 번에 처리하는 길이. 10 ms 는 speex 문서의 권장 구간이고,
# 짧을수록 지연이 줄지만 호출 횟수가 늘어 CPU 를 더 쓴다.
FRAME = 160

# 적응 필터가 덮는 반향 꼬리 길이. 스피커 재생 지연(ALSA 버퍼)과 실제
# 음향 경로를 합친 것보다 길어야 한다 — 방송 alsasink 가 100 ms 버퍼를
# 쓰므로 300 ms 면 여유가 있다. 길수록 CPU 를 더 쓴다.
TAIL_MS = int(os.environ.get("GUARDX_TOIMIC_AEC_TAIL_MS", "300"))

# 기준 신호가 마이크보다 이만큼 넘게 앞서면 앞쪽을 버린다.
# 기준은 ALSA 에 쓰기 **직전**에 뽑으므로 항상 실제 소리보다 앞선다. 그
# 앞섬은 필터가 흡수하지만, 꼬리 길이를 넘어가면 못 잡는다. 감지기가 한
# 홉 밀렸을 때 앞섬이 무한정 쌓이는 것을 여기서 막는다.
MAX_LEAD_MS = int(os.environ.get("GUARDX_TOIMIC_AEC_MAX_LEAD_MS", "400"))

SPEEX_ECHO_SET_SAMPLING_RATE = 24


# --------------------------------------------------------------- 기준 신호

class SpeakerRef:
    """스피커 출력 사본을 UDP 로 받아 샘플 큐로 들고 있는다."""

    def __init__(self, port=REF_PORT):
        self.sock = None
        self.buf = np.zeros(0, dtype=np.int16)
        self.max_lead = int(REF_RATE * MAX_LEAD_MS / 1000)
        self.received = 0
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 18)
            s.bind(("127.0.0.1", port))
            s.setblocking(False)
            self.sock = s
        except OSError as e:
            print("[AEC] 기준 신호 포트 %d 열기 실패(%s)" % (port, e))

    def ok(self):
        return self.sock is not None

    def _drain(self):
        """도착한 데이터그램을 전부 큐 뒤에 붙인다."""
        if self.sock is None:
            return
        chunks = []
        while True:
            try:
                data = self.sock.recv(65535)
            except BlockingIOError:
                break
            except OSError:
                # 보내는 쪽이 없을 때 loopback 은 ICMP 거절을 돌려준다.
                # 정상 상태다(재생이 없으면 아무도 안 보낸다).
                break
            if not data:
                break
            # 홀수 바이트는 잘린 데이터그램이다. 마지막 반 샘플만 버린다.
            if len(data) & 1:
                data = data[:-1]
            if data:
                chunks.append(np.frombuffer(data, dtype=np.int16))
        if chunks:
            self.buf = np.concatenate([self.buf] + chunks)
            self.received += sum(len(c) for c in chunks)

    def pull(self, n):
        """마이크 n 샘플에 대응하는 기준 n 샘플. 모자라면 0 으로 채운다.

        0 으로 채우는 것이 맞다 — 재생이 없으면 지울 반향도 없다.
        """
        self._drain()

        # 앞섬이 너무 커지면 앞쪽을 버린다. 이게 없으면 감지기가 한 번 밀린
        # 뒤로 기준과 마이크가 영영 어긋난 채 돈다(반향이 안 지워진다).
        if len(self.buf) > self.max_lead:
            self.buf = self.buf[len(self.buf) - self.max_lead:]

        if len(self.buf) >= n:
            out = self.buf[:n]
            self.buf = self.buf[n:]
            return out

        out = np.zeros(n, dtype=np.int16)
        if len(self.buf):
            out[:len(self.buf)] = self.buf
            self.buf = np.zeros(0, dtype=np.int16)
        return out

    def active(self):
        """지금 재생 중인가 (큐에 기준 신호가 남아 있는가)."""
        return len(self.buf) > 0

    def close(self):
        if self.sock is not None:
            self.sock.close()
            self.sock = None


# ------------------------------------------------------------- speex 바인딩

class _Speex:
    """libspeexdsp 의 에코 캔슬러 최소 바인딩."""

    def __init__(self, frame, tail, rate):
        name = ctypes.util.find_library("speexdsp") or "libspeexdsp.so.1"
        self.lib = ctypes.CDLL(name)

        self.lib.speex_echo_state_init.restype = ctypes.c_void_p
        self.lib.speex_echo_state_init.argtypes = [ctypes.c_int, ctypes.c_int]
        self.lib.speex_echo_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                            ctypes.c_void_p]
        self.lib.speex_echo_cancellation.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int16),
            ctypes.POINTER(ctypes.c_int16),
            ctypes.POINTER(ctypes.c_int16),
        ]
        self.lib.speex_echo_state_destroy.argtypes = [ctypes.c_void_p]

        self.st = self.lib.speex_echo_state_init(frame, tail)
        if not self.st:
            raise OSError("speex_echo_state_init 실패")
        r = ctypes.c_int(rate)
        self.lib.speex_echo_ctl(self.st, SPEEX_ECHO_SET_SAMPLING_RATE,
                                ctypes.byref(r))

    def cancel(self, rec, ref, out):
        p = ctypes.POINTER(ctypes.c_int16)
        self.lib.speex_echo_cancellation(self.st,
                                         rec.ctypes.data_as(p),
                                         ref.ctypes.data_as(p),
                                         out.ctypes.data_as(p))

    def close(self):
        if getattr(self, "st", None):
            self.lib.speex_echo_state_destroy(self.st)
            self.st = None


# ------------------------------------------------------------------ 단계

class AecStage:
    """마이크 float32(16 kHz) → 반향 제거된 float32(16 kHz).

    입력 길이는 FRAME 의 배수가 아니어도 된다 — 자투리는 다음 호출로 넘긴다.
    감지기의 한 홉(0.25초 = 4000 샘플)은 FRAME(160)의 정확한 배수라 실제로는
    자투리가 생기지 않지만, 홉 길이를 바꿔도 조용히 깨지지 않게 해 둔다.
    """

    def __init__(self):
        self.ref = SpeakerRef()
        self.speex = None
        self.pend_mic = np.zeros(0, dtype=np.int16)
        self.reason = ""

        if os.environ.get("GUARDX_TOIMIC_AEC", "1") != "1":
            self.reason = "GUARDX_TOIMIC_AEC=0 으로 꺼짐"
            return
        if not self.ref.ok():
            self.reason = "기준 신호 포트를 열 수 없음"
            return
        try:
            tail = int(REF_RATE * TAIL_MS / 1000)
            self.speex = _Speex(FRAME, tail, REF_RATE)
            print("[AEC] speexdsp 활성 — frame=%d tail=%dms port=%d"
                  % (FRAME, TAIL_MS, REF_PORT))
        except OSError as e:
            self.reason = "libspeexdsp 없음(%s) — sudo apt install libspeexdsp1" % e

    def available(self):
        return self.speex is not None

    def playback_active(self):
        return self.ref.active()

    def process(self, mic_f32):
        """반향을 지운 신호를 돌려준다. 비활성이면 입력을 그대로 돌려준다."""
        if self.speex is None:
            return mic_f32

        # float(-1..1) → int16. speex 는 정수만 다룬다.
        mic_i16 = np.clip(mic_f32 * 32767.0, -32768, 32767).astype(np.int16)
        if len(self.pend_mic):
            mic_i16 = np.concatenate([self.pend_mic, mic_i16])

        n_frames = len(mic_i16) // FRAME
        used = n_frames * FRAME
        self.pend_mic = mic_i16[used:].copy()
        if n_frames == 0:
            return np.zeros(0, dtype=np.float32)

        out = np.empty(used, dtype=np.int16)
        for i in range(n_frames):
            a, b = i * FRAME, (i + 1) * FRAME
            rec = np.ascontiguousarray(mic_i16[a:b])
            ref = np.ascontiguousarray(self.ref.pull(FRAME))
            frame_out = np.empty(FRAME, dtype=np.int16)
            self.speex.cancel(rec, ref, frame_out)
            out[a:b] = frame_out

        return (out.astype(np.float32) / 32768.0)

    def close(self):
        if self.speex is not None:
            self.speex.close()
            self.speex = None
        self.ref.close()
