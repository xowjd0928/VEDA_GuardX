#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/** @brief 카메라 장비 전체 자원 스냅샷 (opensdk.cgi appstatus, 1초 폴) */
struct CameraResources
{
    int cpu = -1;                    ///< TotalCPUUsage % (-1 = 미수신)
    int mem = -1;                    ///< TotalMemoryUsage %
    int npu = -1;                    ///< TotalNPUUsage %
    double ram_free_mb = -1.0;       ///< FreeRamSpace (MB)
    double ram_total_mb = -1.0;      ///< TotalRamSpace (MB)
    double storage_free_mb = -1.0;   ///< FreeStorageSpace (MB)
    double storage_total_mb = -1.0;  ///< TotalStorageSpace (MB)

    bool valid() const { return cpu >= 0; }
};

/**
 * @brief 설치 앱 하나 — apps 목록(5초)과 appstatus 앱별 자원(1초)의 병합
 *
 * 목록 필드는 apps&action=view, 자원 필드는 appstatus&action=view 에서 온다.
 * 자원이 아직 안 왔거나 그 앱이 정지 상태면 자원 필드는 -1로 남는다.
 */
struct CameraAppInfo
{
    // ---- apps&action=view ----
    QString id;                 ///< AppID (WiseAI/test/juan_application/…)
    QString version;
    QString status;             ///< Running/Stopped/Starting/… (7종 원문)
    QString priority;           ///< Low/Medium/High
    QString installed_date;
    bool auto_start = false;    ///< false면 정전 시 안 뜬다 (test_calibration 구멍)
    bool is_default = false;    ///< 기본앱(WiseAI) — 정지 비권장
    QString control_forbidden;  ///< 금지된 조작 원문 — UI 버튼 비활성 근거

    // ---- appstatus&action=view (없으면 -1) ----
    int cpu = -1;
    int mem = -1;
    int npu = -1;
    int threads = -1;
    double ram_mb = -1.0;
    qint64 uptime_s = -1;       ///< Duration(ISO 8601 "P0Y0M0DT8H3M11S") 파싱값
};

/**
 * @brief 카메라 자원·앱 상태 폴링 원천 (싱글턴 — DetectionFeed 패턴)
 *
 * top_bar의 전역 pill과 CAMERA 탭이 모두 여기 하나를 구독한다 — 폴링 1회,
 * 소비 N곳. 전역 상시 표시가 목적이므로 탭 가시성과 무관하게 항상 돈다.
 *
 *  - appstatus 1초: 장비 CPU/MEM/NPU + RAM/저장소 + 앱별 자원
 *  - apps view 5초: 설치 앱 목록·상태·AutoStart·Priority
 *
 * 실패·오프라인·재부팅 처리 (기획서 §4d):
 *  - 폴 1~2회 연속 실패 = Stale (UI는 값 dim), 3회+ = Offline (값 "—")
 *  - 재부팅 버튼이 눌리면 enter_reboot_mode() — 실패를 오프라인으로 오해하지
 *    않고, 카메라가 다시 뜨면(첫 폴 성공) 자동 Online 복귀
 *  - 실패 로그는 상태 전이 때 1회만 — 팝업·재시도 폭주 금지
 *
 * 킬스위치: 레지스트리 camera_status_poll=0 이면 폴링을 아예 돌리지 않는다.
 * 카메라 제어 채널 반죽음(2026-08-05)이 재발하면 첫 용의선상에서 이 폴러를
 * 제외할 수 있게 — onvif_meta 킬스위치와 같은 목적.
 */
class CameraStatus : public QObject
{
    Q_OBJECT

public:
    enum class LinkState {
        Online,     ///< 폴 정상
        Stale,      ///< 1~2회 연속 실패 — 일시 지연일 수 있음
        Offline,    ///< 3회+ 연속 실패 — 카메라 접근 불가
        Rebooting,  ///< 재부팅 지시 후 복구 대기 (실패가 정상인 구간)
    };
    Q_ENUM(LinkState)

    static CameraStatus *instance();

    // ---- 늦게 구독한 소비자용 현재값 (CAMERA 탭 진입 시) ----
    CameraResources resources() const { return m_res; }
    QVector<CameraAppInfo> apps() const { return m_apps; }
    LinkState link_state() const { return m_state; }
    /** @brief 마지막 폴 성공 시각 (epoch ms, 0=아직 없음) — 오프라인 배너용 */
    qint64 last_success_ms() const { return m_last_ok_ms; }

    /**
     * @brief 재부팅 지시 직후 호출 — 폴 실패를 Offline로 분류하지 않는다
     *
     * 첫 폴 성공 시 자동으로 Online 복귀. 2분이 지나도록 안 돌아오면
     * 진짜 문제로 보고 Offline로 전환한다.
     */
    void enter_reboot_mode();

    /**
     * @brief 앱 목록 즉시 재폴 — Start/Stop 직후 낙관적 전이 확정용 (§3-A)
     *
     * 5초 주기를 기다리지 않고 한 번 더 묻는다. 진행 중 요청이 있으면
     * 겹침 방지가 알아서 건너뛴다.
     */
    void request_apps_now() { request_apps(); }

    /**
     * @brief 사용자가 이 앱을 곧 정지시킨다 — "예기치 않은 죽음" 경보 제외
     *
     * §4c-2: 사용자가 안 껐는데 Stopped가 된 앱만 경보 대상이다. 앱이 다시
     * Running이 되면 기대는 자동 소거된다.
     */
    void expect_app_stop(const QString &app_id) { m_expected_stops.insert(app_id); }

signals:
    /** @brief 장비 자원 갱신 (appstatus 성공마다 — 1초) */
    void resources_changed(const CameraResources &res);
    /** @brief 앱 목록·상태·앱별 자원 갱신 (appstatus/apps 성공마다) */
    void apps_changed(const QVector<CameraAppInfo> &apps);
    /** @brief 정상/지연/오프라인/재부팅 전이 (전이 때만) */
    void link_state_changed(CameraStatus::LinkState state);

private:
    explicit CameraStatus(QObject *parent = nullptr);

    void tick();
    void request_appstatus();
    void request_apps();
    void handle_appstatus(QNetworkReply *reply);
    void handle_apps(QNetworkReply *reply);
    void on_poll_success();
    void on_poll_failure(const QString &why);
    void set_state(LinkState s);
    void report_stats();

    // ---- AlertFeed 발행 (§4c-2) ----
    void publish_app_alerts();       ///< 예기치 않은 죽음 + AutoStart 구멍
    void publish_resource_alert();   ///< 자원 >90% 10초+ 지속

    QNetworkAccessManager *m_net = nullptr;
    QTimer *m_timer = nullptr;
    QNetworkReply *m_pending_status = nullptr;  ///< appstatus 요청 겹침 방지
    QNetworkReply *m_pending_apps = nullptr;    ///< apps view 요청 겹침 방지

    CameraResources m_res;
    QVector<CameraAppInfo> m_apps;       ///< apps view 순서, 자원 병합됨
    LinkState m_state = LinkState::Offline;

    int m_tick = 0;                ///< 1초 틱 카운터 (5틱마다 apps·통계)
    int m_fail_streak = 0;         ///< appstatus 연속 실패 수 (상태 분류 근거)
    qint64 m_last_ok_ms = 0;
    qint64 m_reboot_started_ms = 0;

    // 5초 통계 로그용 카운터
    int m_stat_ok = 0;
    int m_stat_fail = 0;

    // §4c-2 경보 상태
    QSet<QString> m_expected_stops;          ///< 사용자 지시 정지 (경보 제외)
    QSet<QString> m_dead_apps;               ///< 예기치 않게 죽은 앱
    QHash<QString, QString> m_prev_status;   ///< 죽음 전이 감지용 직전 상태
    int m_hot_seconds = 0;                   ///< 자원 >90% 연속 초
    bool m_res_alert = false;
};
