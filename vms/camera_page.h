#pragma once

#include <QHash>
#include <QJsonObject>
#include <QWidget>

#include "camera_status.h"

class AppCard;
class CameraControl;
class QButtonGroup;
class QGridLayout;
class QJsonObject;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class ResourceGauge;

/**
 * @brief CAMERA 탭 — 장비·앱 상태 대시보드 + 카메라 제어 (기획서 2026-08-05)
 *
 * 3단계 골격: 헤더(장비 요약 + 연결 상태) · 자원 히어로(아크 게이지
 * CPU/MEM/NPU + 스파크라인 + RAM/저장소) · 앱 요약 줄(4단계에서 카드로
 * 교체) · 하위 탭(시스템=deviceinfo 패널, 나머지는 6~8단계).
 *
 * 데이터는 전부 CameraStatus 싱글턴 구독 — 이 페이지는 폴링하지 않는다.
 * 예외는 deviceinfo(불변) 1회 조회로, 탭 첫 진입 때 이 페이지가 직접 한다.
 */
class CameraPage : public QWidget
{
    Q_OBJECT

public:
    explicit CameraPage(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *ev) override;

private:
    QWidget *build_header();
    QWidget *build_hero();
    QWidget *build_apps_section();
    QWidget *build_subtabs();

    /** @brief 하위 탭 버튼 줄 — 보드처럼 **화면 제목 줄** 오른쪽에 들어간다 */
    QWidget *build_subtab_bar(QWidget *parent);

    /**
     * @brief 우측 장비 열 (보드 360px) — 어느 하위 탭에서도 보인다
     *
     * 예전 build_system_tab() 을 대신한다. 그때는 장비·펌웨어가 System 탭
     * 안에 있어서 Profiles·Image·Network 를 보는 동안 사라졌다.
     */
    QWidget *build_device_column();

    /**
     * @brief 카메라 웹 UI 링크 (주소는 Credentials가 유일한 출처)
     *
     * 시스템 탭 「장비·펌웨어 정보」 헤더 오른쪽. 누르면 기본 브라우저로 열리고
     * 주소는 드래그해 복사할 수 있다.
     */
    QWidget *build_camera_link(QWidget *parent);

    void on_resources(const CameraResources &res);
    void on_apps(const QVector<CameraAppInfo> &apps);
    void on_link_state(CameraStatus::LinkState state);

    // ---- 앱 Start/Stop (4단계) ----
    void on_start_requested(const QString &app_id);
    void on_stop_requested(const QString &app_id);
    void on_control_finished(const QString &app_id, bool start, bool ok,
                             const QString &error);
    void on_set_finished(const QString &app_id, const QString &key, bool ok,
                         const QString &error);
    /** @brief 조작 결과 안내 한 줄 — 4초 뒤 자동 소거 */
    void show_toast(const QString &text, bool ok);
    const CameraAppInfo *find_app(const QString &app_id) const;

    /** @brief system.cgi deviceinfo 1회 조회 (진입 시, 불변 §3-0) */
    void fetch_deviceinfo();
    void fill_deviceinfo(const QJsonObject &obj);

    // 헤더
    QLabel *m_dev_summary = nullptr;   ///< 모델 · FW · ONVIF 요약 한 줄
    QLabel *m_state_chip = nullptr;    ///< ● ONLINE/STALE/OFFLINE/REBOOT
    QLabel *m_banner = nullptr;        ///< 오프라인 배너 (평소 숨김)

    // 히어로
    ResourceGauge *m_gauges[3] = {};   ///< CPU / MEM / NPU
    QLabel *m_ram_label = nullptr;
    QLabel *m_sto_label = nullptr;

    // 앱 카드 (4단계)
    QVBoxLayout *m_cards_lay = nullptr;
    QHash<QString, AppCard *> m_cards;
    QVector<CameraAppInfo> m_apps;      ///< 최신 목록 (다이얼로그 문구용)
    CameraControl *m_control = nullptr;
    QLabel *m_toast = nullptr;

    // 하위 탭
    QStackedWidget *m_subpages = nullptr;
    /// 하위 탭 버튼 그룹 — 버튼은 제목 줄(build_subtab_bar)에서 먼저 만들어지고
    /// 스택(m_subpages)은 나중에 생기므로, 연결을 위해 그룹을 들고 있는다.
    QButtonGroup *m_subtab_group = nullptr;
    QGridLayout *m_dev_grid = nullptr;  ///< 시스템 탭 deviceinfo 필드 표
    QLabel *m_dev_loading = nullptr;    ///< "조회 중…" (성공 시 표로 대체)

    // deviceinfo 조회
    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_pending = nullptr;
    bool m_devinfo_loaded = false;
    QJsonObject m_devinfo;   ///< 표를 다시 만들 수 있게 보관 (테마 전환)

    // 재부팅 카운트다운 (§4d)
    QTimer *m_reboot_timer = nullptr;
    qint64 m_reboot_started_ms = 0;
};
