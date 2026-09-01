#include "prediction_feed.h"
#include "credentials.h"
#include "sunapi_request.h"

#include <QAuthenticator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QUrlQuery>
#include <QDebug>

static const QString PRED_PATH = "/opensdk/juan_application/prediction";

// 모델이 1분 해상도 — 60초보다 잦으면 같은 값만 온다 (CAMERA_API_v15.md)
static const int POLL_INTERVAL_MS = 60 * 1000;

PredictionFeed *PredictionFeed::instance()
{
    static PredictionFeed feed;
    return &feed;
}

PredictionFeed::PredictionFeed(QObject *parent)
    : QObject(parent)
{
    m_net = new QNetworkAccessManager(this);
    Credentials::install_tls_pinning(m_net);

    // Digest 인증 — DetectionFeed와 동일 (401 시 자격 공급, 재거부면 중단)
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [](QNetworkReply *reply, QAuthenticator *auth) {
                if (auth->user() == Credentials::camera_user()) {
                    qWarning() << "[PredictionFeed] 인증 거부 — 계정/비밀번호 확인 필요";
                    reply->abort();
                    return;
                }
                auth->setUser(Credentials::camera_user());
                auth->setPassword(Credentials::camera_password());
            });

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this] {
        for (int ch = 0; ch < 4; ++ch)
            request_channel(ch);
    });
    m_timer->start(POLL_INTERVAL_MS);

    for (int ch = 0; ch < 4; ++ch)
        request_channel(ch);
}

void PredictionFeed::set_capacity(int ch, int cap)
{
    if (ch >= 0 && ch < 4)
        m_capacity[ch] = cap;
}

void PredictionFeed::request_channel(int ch)
{
    if (m_pending[ch])
        return;   // 이전 요청이 아직 — 60초 안에 안 끝났으면 그게 더 큰 문제

    QUrl url = Credentials::camera_base_url();
    url.setPath(PRED_PATH);
    QUrlQuery q;
    q.addQueryItem("channel", QString::number(ch));
    if (m_capacity[ch] > 0)   // v16: 용량 주입 — p_over_capacity가 이 기준으로 계산됨
        q.addQueryItem("capacity", QString::number(m_capacity[ch]));
    url.setQuery(q);

    // 타임아웃이 없으면 한 번 매달린 채널이 m_pending[ch]에 영구히 박혀
    // 그 채널 예측이 재부팅까지 갱신되지 않는다 (2026-08-10)
    QNetworkReply *reply = m_net->get(sunapi_request(url));
    m_pending[ch] = reply;
    connect(reply, &QNetworkReply::finished, this, [this, ch, reply] {
        m_pending[ch] = nullptr;
        handle_reply(ch, reply);
        reply->deleteLater();
    });
}

void PredictionFeed::handle_reply(int ch, QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "[PredictionFeed] ch" << ch << "요청 실패:" << reply->errorString();
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
    if (!root.contains("predictions"))
        return;

    Info info;
    info.served = QDateTime::fromString(root["served_utc"].toString(), Qt::ISODate);
    info.warmup = root["model"].toObject()["warmup"].toBool(true);

    for (const QJsonValue &v : root["predictions"].toArray()) {
        const QJsonObject o = v.toObject();
        Horizon h;
        h.minutes = o["horizon_min"].toInt();
        h.p50 = o["p50"].toDouble();
        h.p10 = o.contains("p10") ? o["p10"].toDouble() : -1;
        h.p90 = o.contains("p90") ? o["p90"].toDouble() : -1;
        h.p_over_capacity = o.contains("p_over_capacity")
                                ? o["p_over_capacity"].toDouble() : -1;
        if (h.minutes > 0)
            info.horizons.append(h);
    }
    if (info.horizons.isEmpty())
        return;

    emit prediction_arrived(ch, info);
}
