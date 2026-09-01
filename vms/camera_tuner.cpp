#include "camera_tuner.h"
#include "credentials.h"
#include "sunapi_request.h"

#include <QAuthenticator>
#include <QDebug>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>

CameraTuner::CameraTuner(const QVector<int> &channels,
                         const QVector<int> &profiles, QObject *parent)
    : QObject(parent), m_channels(channels), m_profiles(profiles)
{
    m_net = new QNetworkAccessManager(this);
    Credentials::install_tls_pinning(m_net);

    // SUNAPI는 Digest 인증 — DetectionFeed와 같은 패턴
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [](QNetworkReply *reply, QAuthenticator *auth) {
                if (auth->user() == Credentials::camera_user()) {
                    qWarning() << "[CameraTuner] 인증 거부 — 계정/비밀번호 확인 필요";
                    reply->abort();
                    return;
                }
                auth->setUser(Credentials::camera_user());
                auth->setPassword(Credentials::camera_password());
            });
}

void CameraTuner::start()
{
    // 튜닝 체인은 이 응답 하나에 직렬로 매달려 있다 — 여기서 매달리면
    // 인코더 설정이 통째로 적용되지 않고 조용히 멈춘다 (2026-08-10 타임아웃 추가)
    QNetworkReply *reply =
        m_net->get(sunapi_request(cgi("msubmenu=videoprofile&action=view")));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        on_profiles(reply);
        reply->deleteLater();
    });
}

void CameraTuner::request_sync_point(int channel, int profile)
{
    QNetworkReply *reply = m_net->get(sunapi_request(
        cgi(QString("msubmenu=setsynchronizationpoint&action=control"
                    "&Channel=%1&Profile=%2").arg(channel).arg(profile))));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

QUrl CameraTuner::cgi(const QString &query) const
{
    QUrl url = Credentials::camera_base_url();  // 핀이 설정돼 있으면 https
    url.setPath("/stw-cgi/media.cgi");
    url.setQuery(query);
    return url;
}

void CameraTuner::on_profiles(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "[CameraTuner] 프로파일 조회 실패:" << reply->errorString()
                   << "— 인코더 튜닝 생략 (지연이 커질 수 있음)";
        return;
    }

    // 텍스트 응답: "Channel.0.Profile.2.H264.GOVLength=1" 형식 한 줄씩
    m_state.clear();
    const QStringList lines = QString::fromUtf8(reply->readAll()).split('\n');
    for (const QString &line : lines) {
        const int eq = line.indexOf('=');
        if (eq > 0)
            m_state.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }

    // WiseStream 상태도 읽은 뒤에 판단한다 (읽기 1회가 쓰기 20회보다 싸다)
    QNetworkReply *next =
        m_net->get(sunapi_request(cgi("msubmenu=wisestream&action=view")));
    connect(next, &QNetworkReply::finished, this, [this, next] {
        on_wisestream(next);
        next->deleteLater();
    });
}

void CameraTuner::on_wisestream(QNetworkReply *reply)
{
    m_wisestream_on.clear();
    if (reply->error() == QNetworkReply::NoError) {
        // "Channel.0.Mode=Off" 형식
        const QStringList lines = QString::fromUtf8(reply->readAll()).split('\n');
        for (const QString &line : lines) {
            const int eq = line.indexOf('=');
            if (eq <= 0)
                continue;
            const QString key = line.left(eq).trimmed();
            const QString value = line.mid(eq + 1).trimmed();
            if (!key.endsWith(".Mode") || value == "Off")
                continue;
            const QStringList parts = key.split('.');
            if (parts.size() >= 2)
                m_wisestream_on.insert(parts[1].toInt());
        }
    } else {
        // 못 읽었으면 안전하게 전 채널에 Off를 쓴다
        for (int ch : m_channels)
            m_wisestream_on.insert(ch);
    }

    build_queue();
}

void CameraTuner::build_queue()
{
    const QHash<QString, QString> &map = m_state;
    int skipped = 0;

    for (int ch : m_channels) {
        for (int profile : m_profiles) {
            const QString base =
                QString("Channel.%1.Profile.%2.").arg(ch).arg(profile);

            // EncodingType 키가 없으면 코덱별 키 존재 여부로 판별
            QString codec = map.value(base + "EncodingType");
            if (codec.isEmpty()) {
                if (map.contains(base + "H264.GOVLength"))
                    codec = "H264";
                else if (map.contains(base + "H265.GOVLength"))
                    codec = "H265";
            }
            if (codec != "H264" && codec != "H265") {
                qWarning() << "[CameraTuner] ch" << ch << "profile" << profile
                           << "코덱" << codec << "— 건너뜀";
                continue;
            }

            // 튜너는 "명백히 지연을 망치는" 것만 끈다: DynamicGOV/SmartCodec/
            // DynamicFPS(+WiseStream). 이들은 정적 장면에서 GOP를 늘리거나 fps를
            // 떨어뜨려 지연을 키운다.
            //
            // BitrateControlType(CBR/VBR)과 GOVLength는 건드리지 않는다:
            //  - CBR vs VBR: 저비트레이트 LAN 스트림에선 지연 차이가 거의 없고,
            //    오히려 VBR이 인코더 rate-buffer 지연이 없어 미세하게 유리할 수 있다.
            //    CBR의 장점(스파이크 억제)은 대역 제한/유실 링크에서만 의미가 있다.
            //  - GOVLength: 정상 재생 지연과 무관하다(P프레임은 즉시 디코드). GOP
            //    길이는 유실 후 복구 시간에만 영향. 사용자 설정을 존중한다.
            const bool needs_change =
                map.value(base + codec + ".DynamicGOVEnable") != "False"
                || map.value(base + codec + ".SmartCodecEnable") != "False"
                || map.value(base + codec + ".DynamicFPSEnable") != "False";

            if (!needs_change) {
                ++skipped;
                continue;
            }

            // 주의: videoprofile 수정은 set이 아니라 update다 (set은 601 NG)
            // BitrateControlType/GOVLength는 의도적으로 보내지 않는다(위 참고).
            m_queue << QString("msubmenu=videoprofile&action=update"
                               "&Channel=%1&Profile=%2"
                               "&%3.DynamicGOVEnable=False"
                               "&%3.SmartCodecEnable=False"
                               "&%3.DynamicFPSEnable=False")
                           .arg(ch).arg(profile).arg(codec);
        }

        // WiseStream도 이미 Off면 건드리지 않는다
        if (m_wisestream_on.contains(ch))
            m_queue << QString("msubmenu=wisestream&action=set&Channel=%1&Mode=Off")
                           .arg(ch);
        else
            ++skipped;
    }

    if (m_queue.isEmpty()) {
        qInfo() << "[CameraTuner] 인코더 설정이 이미 최적 — 요청 없음 (기동 지연 0)";
        return;
    }
    qInfo() << "[CameraTuner] 변경 필요" << m_queue.size() << "건, 이미 정상"
            << skipped << "건";
    send_next();
}

void CameraTuner::send_next()
{
    if (m_queue.isEmpty()) {
        qDebug() << "[CameraTuner] 인코더 저지연 설정 완료";
        return;
    }

    const QString query = m_queue.takeFirst();
    // 큐는 이 핸들러가 자기 자신을 다시 부르며 전진한다 — 타임아웃이 없으면
    // 한 건이 매달릴 때 나머지 큐 전체가 영구히 멈춘다 (send_next 재귀)
    QNetworkReply *reply = m_net->get(sunapi_request(cgi(query)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, query] {
        // SUNAPI는 실패도 HTTP 200 + "NG\nError Code: ..." 본문으로 온다
        const QString body = QString::fromUtf8(reply->readAll()).trimmed();
        if (reply->error() != QNetworkReply::NoError)
            qDebug() << "[CameraTuner] 실패:" << query << "|"
                     << reply->errorString();
        else if (!body.startsWith("OK"))
            qDebug() << "[CameraTuner] 거부:" << query << "|" << body;
        reply->deleteLater();
        send_next(); // 실패해도 다음 항목 진행 — 부분 적용이 무적용보다 낫다
    });
}
