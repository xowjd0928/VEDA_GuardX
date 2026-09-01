#include "broadcast_pipeline.h"

// GStreamer is optional in the VMS build (HAVE_GSTREAMER). Without it this
// becomes a stub, exactly like broadcast_rtp_sender.cpp, so a team member
// without GStreamer still builds.
#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#endif

namespace guardx {
namespace broadcast {

bool has_element(const char *factory)
{
#ifdef HAVE_GSTREAMER
    GstElementFactory *f = gst_element_factory_find(factory);
    if (!f)
        return false;
    gst_object_unref(f);
    return true;
#else
    Q_UNUSED(factory);
    return false;
#endif
}

QString build_pipeline(const QString &host, int port, int bitrate,
                       bool denoise, bool agc, int volume_percent,
                       const QString &sink_override, QString *backend_out)
{
#ifdef HAVE_GSTREAMER
    // ── 노이즈 제거(노캔) 단 ────────────────────────────────────────────
    // 1순위: webrtcdsp (plugins-bad). WebRTC의 잡음억제 + 자동이득(AGC) +
    //        하이패스. 음성 방송에서 체감 차이가 가장 큰 단계다.
    //        echo-cancel은 끈다 — 원단(스피커) 신호를 되받는 webrtcechoprobe가
    //        VMS엔 없고, 방송은 단방향이라 에코 경로 자체가 없다.
    // 2순위: audiocheblimit(하이패스) + audiodynamic(확장기=노이즈 게이트).
    //        plugins-good에만 의존해 어느 설치본에서든 동작한다.
    QString dsp;
    QString backend;
    if (has_element("webrtcdsp")) {
        backend = QStringLiteral("webrtcdsp");
        // gain-control(AGC)은 기본 끔 — 이득을 자동으로 밀어올리는 기능이라
        // 마이크와 스피커가 가까운 현장에서는 피드백 루프 이득까지 같이 올려
        // 하울링을 앞당긴다. 필요하면 broadcast/agc 로 켠다.
        dsp = QStringLiteral(
            "webrtcdsp name=dsp echo-cancel=false "
            "noise-suppression=%1 noise-suppression-level=high "
            "gain-control=%2 high-pass-filter=true voice-detection=false ! ")
            .arg(denoise ? "true" : "false", agc ? "true" : "false");
    } else if (has_element("audiocheblimit") && has_element("audiodynamic")) {
        backend = QStringLiteral("highpass+gate");
        // 하이패스는 노캔을 꺼도 유지한다 — 공조기·핸들링 럼블 제거는
        // 부작용이 없고 낮은 비트레이트를 낭비하지 않게 해준다.
        //
        // ⚠ audiofx(audiocheblimit)는 **부동소수 샘플만** 받는다. 위 capsfilter의
        // S16LE를 그대로 물리면 "can't handle caps ...format=S16LE"로 링크가
        // 깨진다(실측). 앞뒤로 audioconvert를 감싸 F32 구간을 만들어 준다.
        dsp = QStringLiteral(
            "audioconvert ! audio/x-raw,format=F32LE ! "
            "audiocheblimit mode=high-pass cutoff=110 poles=4 ! "
            "audiodynamic name=gate mode=expander characteristics=soft-knee "
            "ratio=%1 threshold=0.04 ! audioconvert ! ")
            // 로케일 무관 표기 — 소수점이 ','가 되면 파이프라인 파싱이 깨진다.
            .arg(QString::number(denoise ? kGateRatioOn : kGateRatioOff, 'f', 2));
    } else {
        backend = QStringLiteral("none");
    }
    if (backend_out)
        *backend_out = backend;

    // ── 출력 음량 ───────────────────────────────────────────────────────
    // 노캔·AGC **뒤**에 둔다. 앞에 두면 AGC 가 줄인 만큼 도로 밀어올려
    // 슬라이더가 듣지 않는 것처럼 보인다.
    //
    // 사이렌은 여기를 지나지 않는다 — RPi C 가 로컬 음원으로 재생하므로
    // 방송 음량을 0 으로 내려도 화재 경보 소리는 그대로다. 그게 이 단을
    // VMS 쪽 파이프라인에 둔 이유다.
    //
    // volume 원소가 없는 설치본이면 이 단만 빠지고 나머지는 그대로 돈다
    // (방송이 안 되는 것보다 음량이 안 먹는 편이 낫다).
    const QString vol = has_element("volume")
        ? QStringLiteral("volume name=vol volume=%1 ! ")
              // 로케일 무관 표기 — 소수점이 ','가 되면 파싱이 깨진다.
              .arg(QString::number(volume_gain(volume_percent), 'f', 3))
        : QString();

    // ── 레벨 측정 ───────────────────────────────────────────────────────
    // 인코더 **직전**에 둔다 — 마이크 원본이 아니라 "실제로 송출되는 소리"의
    // 레벨을 봐야 진단에 쓸모가 있다(노캔·AGC가 반영된 뒤 값).
    // UDP는 단방향이라 VMS가 수신 여부를 알 길이 없다. 최소한 "내보내고는
    // 있는가"는 이걸로 눈으로 확인된다.
    const QString level = has_element("level")
        ? QStringLiteral("level name=lvl interval=100000000 "
                         "post-messages=true ! ")
        : QString();

    // ── 인코더 ─────────────────────────────────────────────────────────
    // audio-type=voice     : 음성 최적화 모드(SILK 계열 선호)
    // inband-fec + packet-loss-percentage : 손실 구간을 다음 패킷이 복원 →
    //                        LAN 잡음/무선 구간에서 "뚝뚝" 끊김이 크게 준다.
    //                        수신부 opusdec use-inband-fec=true 와 세트.
    // complexity=10        : PC 인코딩이라 CPU는 남는다. 같은 비트레이트에서
    //                        품질만 오른다.
    // frame-size=20        : 기존 20ms 프레이밍 유지(지터버퍼 가정과 일치).
    const QString enc =
        QStringLiteral(
            "opusenc name=enc bitrate=%1 bitrate-type=constrained-vbr "
            "audio-type=voice frame-size=20 complexity=10 "
            "inband-fec=true packet-loss-percentage=10 dtx=false ! ")
            .arg(bitrate);

    const QString sink = sink_override.isEmpty()
        ? QStringLiteral("udpsink name=sink host=%1 port=%2 "
                         "sync=false async=false").arg(host).arg(port)
        : sink_override;

    // ── 전체 ───────────────────────────────────────────────────────────
    // queue leaky=downstream: 캡처 스레드와 인코딩/송신을 분리한다. 막히면
    // 지연을 쌓지 말고 오래된 오디오를 버리는 쪽이 실시간 방송에 맞다.
    // audioresample quality=10: 마이크가 44.1k여도 48k 변환 품질이 유지된다.
    // sync=false: 실시간 송신이라 시계 동기로 지연 쌓지 않는다.
    return QStringLiteral(
               "autoaudiosrc ! "
               "queue max-size-time=100000000 max-size-buffers=0 "
               "max-size-bytes=0 leaky=downstream ! "
               "audioconvert ! audioresample quality=10 ! "
               "audio/x-raw,rate=48000,channels=1,format=S16LE ! "
               "%1%2%3%4rtpopuspay name=pay pt=96 ! %5")
        .arg(dsp, vol, level, enc, sink);
#else
    Q_UNUSED(host);
    Q_UNUSED(port);
    Q_UNUSED(bitrate);
    Q_UNUSED(denoise);
    Q_UNUSED(agc);
    Q_UNUSED(volume_percent);
    Q_UNUSED(sink_override);
    if (backend_out)
        *backend_out = QStringLiteral("none");
    return QString();
#endif
}

}  // namespace broadcast
}  // namespace guardx
