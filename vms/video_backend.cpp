#include "video_backend.h"
#include "qmediaplayer_backend.h"
#ifdef HAVE_GSTREAMER
#include "direct_sink_backend.h"
#include "gstreamer_backend.h"
#endif
#ifdef HAVE_FFMPEG
#include "ffmpeg_backend.h"
#endif

#include <QDebug>
#include <QSettings>
#include <QStringList>

QString rtsp_transport()
{
    const QString v = QSettings("GuardX", "VMS")
                          .value(QStringLiteral("rtsp_transport"),
                                 QStringLiteral("udp"))
                          .toString().toLower();
    if (v == QLatin1String("udp") || v == QLatin1String("tcp"))
        return v;

    qWarning() << "[VideoBackend] 알 수 없는 rtsp_transport =" << v
               << "— 기본값 udp 로";
    return QStringLiteral("udp");
}

VideoBackend *create_video_backend(int channel, QObject *parent)
{
    // 재시작 시 적용. 값: "direct"(기본 · sink 직접 렌더) | "gstreamer"(appsink)
    //                    | "ffmpeg"(예비) | "qmediaplayer"(최후)
    //
    // 08-20: 코드 기본을 **direct** 로 확정했다. 08-04 저지연 실측 이후 이
    // PC 는 레지스트리에 direct 를 박아 두고 계속 그걸로 돌려 왔는데, 코드
    // 기본은 "gstreamer" 로 남아 있었다. 그러면 레지스트리가 없는 새 PC
    // (팀원 클론·시연 노트북)만 **조용히 다른 경로로** 돌아간다 — 같은 앱을
    // 두 사람이 서로 다른 디코드 경로로 보게 되는 상태였다.
    //
    // 요청값이 이 빌드에 없으면 아래 **예비 순서**로 내려간다:
    //     ffmpeg → gstreamer → qmediaplayer
    // ffmpeg 가 먼저인 이유는 지연이 QMediaPlayer 보다 훨씬 작기 때문이고,
    // qmediaplayer 가 마지막인 이유는 Qt Multimedia 라 **항상 있기** 때문이다
    // (그래서 이 사슬은 반드시 하나를 고른다).
    QSettings settings("GuardX", "VMS");
    QString kind = settings.value("video_backend", "direct").toString().toLower();

    static const QStringList KNOWN = {
        QStringLiteral("direct"), QStringLiteral("gstreamer"),
        QStringLiteral("ffmpeg"), QStringLiteral("qmediaplayer") };
    if (!KNOWN.contains(kind)) {
        qWarning() << "[VideoBackend] 알 수 없는 video_backend =" << kind
                   << "— 기본값 direct 로";
        kind = QStringLiteral("direct");
    }

    // ---- 이 빌드에 실제로 들어 있는 백엔드인가 ----
    const auto available = [](const QString &k) {
#ifndef HAVE_GSTREAMER
        // direct 도 GStreamer 파이프라인이다 (d3d11videosink 직접 렌더)
        if (k == QLatin1String("direct") || k == QLatin1String("gstreamer"))
            return false;
#endif
#ifndef HAVE_FFMPEG
        if (k == QLatin1String("ffmpeg"))
            return false;
#endif
        return true;
    };

    QString chosen = kind;
    if (!available(chosen)) {
        for (const QString &fallback : { QStringLiteral("ffmpeg"),
                                         QStringLiteral("gstreamer"),
                                         QStringLiteral("qmediaplayer") }) {
            if (available(fallback)) {
                chosen = fallback;
                break;
            }
        }
        qWarning() << "[VideoBackend]" << kind
                   << "은(는) 이 빌드에 없다 —" << chosen << "로 대체";
    }

#ifdef HAVE_GSTREAMER
    // direct: appsink 없이 d3d11videosink가 위젯에 직접 렌더 (2026-08-04 실험)
    if (chosen == QLatin1String("direct"))
        return new DirectSinkBackend(channel, parent);
    if (chosen == QLatin1String("gstreamer"))
        return new GStreamerBackend(channel, parent);
#endif
#ifdef HAVE_FFMPEG
    // libavformat/libavcodec 직접 디코드
    if (chosen == QLatin1String("ffmpeg"))
        return new FFmpegBackend(channel, parent);
#endif
    return new QMediaPlayerBackend(parent);   // 최후 — 항상 있다
}
