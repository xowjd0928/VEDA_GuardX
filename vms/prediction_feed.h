#pragma once

#include <QDateTime>
#include <QObject>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/**
 * @brief 카메라 /prediction 폴링 피드 — 채널별 60초 (DetectionFeed와 같은 패턴)
 *
 * 실시간 소비는 카메라 HTTP 직결이 원칙 (박스=/detections와 동일) — DB·MQTT를
 * 거치지 않는다. Digest 인증·TLS 핀은 Credentials가 담당.
 *
 * capacity를 알면 `?capacity=N`을 부착한다 — 카메라 v16부터 p_over_capacity가
 * 주입된 용량 기준으로 계산된다 (미부착 시 앱 상수 20 기준이라 표시 무의미).
 * 60초 주기인 이유: 모델이 1분 해상도라 더 자주 읽어도 같은 값만 온다.
 */
class PredictionFeed : public QObject
{
    Q_OBJECT

public:
    struct Horizon {
        int minutes = 0;                 ///< 5 / 30 / 60 / 180
        double p50 = 0;                  ///< 점유 중앙값 예측 (명, 소수)
        double p10 = -1;                 ///< 하한 분위수 (-1 = 미제공)
        double p90 = -1;                 ///< 상한 분위수 (-1 = 미제공)
        double p_over_capacity = -1;     ///< P(점유>capacity), -1 = 불명 (0 아님!)
    };
    struct Info {
        QDateTime served;                ///< 카메라 시계 (UTC)
        bool warmup = true;              ///< true = 학습 전 — 표시 신뢰 낮음
        QVector<Horizon> horizons;       ///< target 순 (5,30,60,180)

        /** @brief 해당 horizon의 p50 (-1 = 없음) */
        double p50_at(int minutes) const
        {
            for (const Horizon &h : horizons)
                if (h.minutes == minutes)
                    return h.p50;
            return -1;
        }
    };

    static PredictionFeed *instance();

    /** @brief 존 정원 주입 — p_over_capacity의 기준 (zones/set_zone 갱신 시 재호출) */
    void set_capacity(int ch, int cap);

signals:
    void prediction_arrived(int ch, const PredictionFeed::Info &info);

private:
    explicit PredictionFeed(QObject *parent = nullptr);
    void request_channel(int ch);
    void handle_reply(int ch, QNetworkReply *reply);

    QNetworkAccessManager *m_net = nullptr;
    QTimer *m_timer = nullptr;
    QNetworkReply *m_pending[4] = {};
    int m_capacity[4] = {};
};
