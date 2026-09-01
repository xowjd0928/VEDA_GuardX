#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

/**
 * @brief 현장 전역 설정 (SITE 문구 · 바닥 캘리브레이션)의 단일 진실원천 (08-12)
 *
 * 계약: `guardx/db/rpib/site_config` (retained 상태) +
 *       `cmd/set_site_config` (부분 갱신 쓰기 · admin 전용 · 16KB 상한)
 *       — docs/DB_LINK_AND_MQTT_MIGRATION.md 08-12 신설 절이 정본.
 *
 * 합의 ④(08-12): 배포 시 모든 사용자가 같은 환경을 본다 — 그래서 저장은
 * 서버(DB)가 하고 여기는 수신·캐시·발행만 한다. 다만 캘리브레이션에는
 * 두 번째 축이 있다:
 *
 *   - **관리자 [적용]** = 전역. 서버로 발행 → retained 재방송 → 전 VMS 적용
 *   - **운영자 [적용]** = 자기만. 서버에 안 보내고 이 PC·이 계정의
 *     오버라이드로 저장한다. 오버라이드가 있는 동안 전역 수신은 캐시만
 *     갱신하고 화면에는 안 올라간다
 *
 * 재시작 영속(요구 ④): 마지막 전역값과 계정별 오버라이드를 QSettings 에
 * 남긴다. 기동 시 브로커보다 먼저 캐시로 그리고, retained 가 오면 덮는다.
 *
 * ⚠ 없는 키 = "설정 안 됨" (계약). site_name 없이 calibration 만 저장된
 *   payload 도 정상이다 — 키 존재 여부로만 판단하고 기본값으로 메운다.
 */
class SiteConfig : public QObject
{
    Q_OBJECT

public:
    static SiteConfig *instance();

    /** @brief 구독 등록 + 캐시 복원. MqttLink::start() 전에 불러도 된다 */
    void init();

    /**
     * @brief 화면에 쓸 현장 문구 — 서버값 → 캐시 → 설계 기본값 순
     *
     * 값이 어디서 왔든 항상 뭔가는 돌려준다 — 로그인 화면은 브로커 연결
     * 전에 그려지므로 "빈 문구" 상태를 만들지 않는다.
     */
    QString site_name() const;

    /** @brief 설계 기본값 (서버·캐시 둘 다 없을 때) */
    static const char *DEFAULT_SITE_NAME;

    // ---- 쓰기 (결과는 콜백 — reason 은 계약의 기계값 그대로) ----
    using Done = std::function<void(bool ok, const QString &reason)>;

    /** @brief SITE 문구 저장 (admin 전용 — 서버가 토큰으로 재검증) */
    void save_site_name(const QString &name, Done done);

    /**
     * @brief 캘리브레이션 전역 발행 (관리자 [적용])
     *
     * 로컬에는 이미 적용된 상태(CalibrationStore)의 raw_json 을 그대로
     * 싣는다 — 서버는 해석하지 않고 통짜 보관한다(계약).
     */
    void publish_calibration(const QJsonObject &calib, Done done);

    /** @brief 캘리브레이션 로컬 오버라이드 저장 (운영자 [적용] — 서버 무접촉) */
    void save_local_calibration(const QJsonObject &calib);

    /** @brief 이 계정에 로컬 오버라이드가 있나 */
    bool has_local_override() const;

    /** @brief 오버라이드 해제 — 다음 전역값부터 다시 따라간다 */
    void clear_local_override();

signals:
    /** @brief site_name 이 바뀌었다 — 상단바·로그인·REPORT 가 다시 그린다 */
    void site_name_changed();

private:
    explicit SiteConfig(QObject *parent = nullptr);

    void on_payload(const QByteArray &payload);
    /** @brief 지금 계정 기준으로 "화면에 올라갈" 캘리브레이션을 고른다 */
    void apply_effective_calibration();
    QString override_key() const;   ///< 계정별 QSettings 키 (로그인 전엔 빈 값)

    QString m_site_name;            ///< 서버/캐시에서 온 값 (빈 값 = 미설정)
    QJsonObject m_global_calib;     ///< 마지막 전역 캘리브레이션 (빈 것 = 없음)
    QString m_last_updated_at;      ///< dedup 용
    bool m_started = false;
};
