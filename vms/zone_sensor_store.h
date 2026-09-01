#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

/**
 * @brief zone별 센서 데이터 보관소 (싱글턴) — 구독은 여기 한 곳만
 *
 * 전에는 DeviceControlPage가 zone 하나마다 하나씩 만들어져 각자 MQTT를
 * 구독했다(탭 4개 = 구독 4벌). 화면이 "한 zone만 보기 / 여러 zone 비교하기"로
 * 바뀌면서 그 구조가 무너졌다 — 비교 화면은 zone 전부의 이력이 동시에
 * 필요한데, 데이터가 화면 인스턴스마다 흩어져 있으면 모을 수가 없다.
 *
 * 그래서 데이터를 화면에서 떼어 여기 모은다:
 *   - guardx/db/rpib/sensors      구독 1회 (zones 배열 — zone마다 최신 사이클)
 *   - guardx/db/rpib/fire_threshold 구독 1회 (게이지·색 기준)
 *   - query/sensor_history        요청 1회 (전 zone 백필)
 *   - 더미 zone 값 생성           (실 하드웨어가 없는 시연용 구역)
 *
 * 화면은 snapshot()/history()를 읽어 그리기만 한다. 더미인지 실데이터인지도
 * 화면은 모른다 — 여기서 같은 모양으로 채워 넣기 때문이다.
 */
class ZoneSensorStore : public QObject
{
    Q_OBJECT

public:
    /** @brief 채널 하나의 최신값. present=false면 그 사이클에 아예 없던 채널 */
    struct Sample {
        double value = 0;
        bool valid = false;
        bool present = false;
    };

    /** @brief 그래프 롤링 버퍼 원소 (그래프용 값 — spark_raw는 EMA 적용됨) */
    struct Point {
        qint64 t_ms = 0;
        double value = 0;
    };

    /** @brief zone 하나의 최신 상태 */
    struct Snapshot {
        int zone_id = 0;
        QString zone_name;
        bool has_data = false;          ///< false = 존재하는 zone이지만 아직 수신 없음
        double composite_score = -1;    ///< 종합 위험도 0~100. 음수 = 계산 안 함(동결)
        /**
         * @brief 마지막 **실측 사이클** 수신 시각. 0 = 없음
         *
         * ⚠ 도착 시각이 아니다 (2026-08-20 수정). 이 토픽의 발행자는 RPi A가
         * 아니라 RPi B(DB 스냅샷)다 — A가 꺼져도 B는 행을 계속 쓰고 seq 도
         * 전진하며(실측), 그때 값은 0·is_valid=false 였다. 도착 시각으로
         * 신선도를 찍으면 **A가 꺼져도 영원히 초록**이 된다.
         * on_sensors 의 세 조건(seq 전진 · 유효 채널 존재 · 값 이동)을 모두
         * 만족한 수신에만 찍는다.
         */
        qint64 last_ms = 0;
        qint64 last_seq = -1;           ///< 마지막으로 본 sensor_seq. -1 = 아직 없음
        /// 직전 사이클의 채널 값 지문 — 같으면 "얼어붙은 값"(재발행/센서 무응답)
        QString value_print;
        /// 끊김 없이 이어진 새 사이클 수 (STALE_MS 를 넘겨 쉬면 1부터 다시)
        int fresh_streak = 0;
        QHash<QString, Sample> channels;
    };

    static ZoneSensorStore *instance();

    Snapshot snapshot(int zone_id) const;
    QVector<Point> history(int zone_id, const QString &channel_key) const;

    /** @brief fire_threshold의 "threshold" 오브젝트. 비었으면 fallback 범위로 */
    QJsonObject threshold() const { return m_threshold; }

    /**
     * @brief RPi A 생존 판정 — **모든 표시기가 여기 하나만 본다**
     *
     * 예전에는 top_bar 와 device_control_page 가 각자 zone 을 훑어 판정했고,
     * 더미 zone 가드가 한쪽에만 있어 두 지시기가 **서로 모순된 상태**를 표시한
     * 적이 있다(2026-08-10). 판정 규칙이 늘어난 지금(재발행 배제·연속 조건)
     * 그 중복은 그대로 두면 반드시 다시 갈라진다.
     */
    struct NodeAHealth {
        bool streaming = false;  ///< 초록 조건: 최근 값 + 연속 사이클 충족
        bool seen = false;       ///< 값을 한 번이라도 받았나 (false = 대기 중)
        qint64 age_ms = -1;      ///< 마지막 **새 사이클** 이후 경과. -1 = 없음
        int streak = 0;          ///< 끊김 없이 이어진 새 사이클 수
    };
    NodeAHealth node_a_health() const;

    /**
     * @brief 초록이 되기까지 필요한 연속 사이클 수
     *
     * 한 번 온 값으로 초록을 켜면, 재발행 하나에도 5초간 "정상"이 뜬다.
     * "꾸준히 보내는 동안만 초록"이라는 요구(2026-08-20)의 구현이다.
     * 1Hz 발행이라 3이면 ~3초 안에 정상 판정이 난다.
     */
    static constexpr int STREAM_STREAK = 3;

    /** @brief 그래프에 보여줄 최근 시간 폭 */
    static constexpr qint64 HISTORY_WINDOW_MS = 5 * 60 * 1000;

    /** @brief RPi A 1Hz 발행 기준 여유 5배 — 이보다 오래되면 오프라인 판정 */
    static constexpr qint64 STALE_MS = 5000;

signals:
    /** @brief 어느 zone의 값이 갱신됨 (실데이터·더미 공통) */
    void updated(int zone_id);
    /** @brief fire_threshold 재수신 — 게이지 색 기준이 바뀜 */
    void threshold_changed();

private:
    explicit ZoneSensorStore(QObject *parent = nullptr);

    void on_sensors(const QByteArray &payload);
    void on_sensor_history(const QJsonObject &reply);
    void on_fire_threshold(const QByteArray &payload);
    void tick_dummy();

    /**
     * @brief 그래프 초기 백필을 1회 요청한다 (브로커 연결 후에만 의미가 있다)
     *
     * ⚠ 생성자에서 부르면 안 된다 — 그 시점엔 MqttLink 가 확정적으로
     * 오프라인이라 요청이 매번 실패했고(재시도 없음) on_sensor_history 는
     * 사실상 죽은 코드, 그래프는 항상 빈 채로 시작했다 (2026-08-10 수정).
     * online_changed(true) 에서 부른다. 재접속마다 다시 긁어오지 않도록
     * m_backfill_asked 로 한 번만 나간다.
     */
    void request_backfill();

    /** @brief 한 점을 롤링 버퍼에 넣고 윈도 밖은 버린다 (EMA는 여기서) */
    void push_point(int zone_id, const QString &channel_key, qint64 t_ms,
                    double value, bool valid);

    QHash<int, Snapshot> m_zones;                              ///< zone_id -> 최신
    QHash<int, QHash<QString, QVector<Point>>> m_history;      ///< zone -> ch -> 버퍼
    QHash<int, QHash<QString, double>> m_ema;                  ///< zone -> ch -> EMA 상태
    QJsonObject m_threshold;

    bool m_backfill_asked = false;   ///< 백필은 실행당 1회 (재접속 때 재요청 안 함)

    QTimer *m_dummy_timer = nullptr;
    /// 더미 구역의 채널별 현재값(랜덤워크 상태). "__score" 키는 종합 위험도.
    /// 직전 값에서 조금씩만 움직여야 선이 안 튀므로 상태를 들고 있어야 한다.
    QHash<int, QHash<QString, double>> m_dummy_val;
};
