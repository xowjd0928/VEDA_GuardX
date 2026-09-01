#pragma once

#include <QWidget>

class CameraControl;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QTimer;

/**
 * @brief CAMERA 탭 [시스템] 하위 탭의 운영 기능 묶음 (기획서 §3-B, 6단계)
 *
 *  - 시계/NTP: date 30초 폴(탭 보일 때만) — 카메라 시계와 PC UTC의 오프셋
 *    표시. 08-04 시계 사고(수동설정 잔재 1.5s 슬루) 재발 점검용
 *  - 프로파일 시청 세션: profileaccessinfo (RTSP 부하 가시화 — 세션 굶주림
 *    간접 진단). 미실측 API라 원문 표시 + 미지원(608) 대비
 *  - 로그: systemlog/accesslog/eventlog 읽기전용 뷰 (텍스트 응답 원문)
 *  - 설정 백업: configbackup → Downloads에 저장 (08-03 초기화 사고 보험)
 *  - 재부팅: 2단 확인 + CameraStatus 재부팅 모드 연동 (§4d) — 위험 구역
 */
class SystemPanel : public QWidget
{
    Q_OBJECT

public:
    SystemPanel(CameraControl *control, QWidget *parent = nullptr);

protected:
    /** @brief date 폴은 보일 때만 돈다 (§5 폴링 규율) */
    void showEvent(QShowEvent *ev) override;
    void hideEvent(QHideEvent *ev) override;

private:
    QWidget *build_clock_card();
    QWidget *build_profile_card();
    QWidget *build_log_card();
    QWidget *build_danger_card();

    void fetch_date();
    void fetch_profile_access();
    void fetch_logs(const QString &submenu);
    void save_backup();
    void confirm_reboot();

    CameraControl *m_control = nullptr;
    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_pending_date = nullptr;

    // 시계 카드
    QLabel *m_clock_body = nullptr;
    QPushButton *m_btn_ntp = nullptr;
    QTimer *m_date_timer = nullptr;
    QString m_sync_type;

    // 프로파일·로그 카드
    QLabel *m_profile_body = nullptr;
    QPlainTextEdit *m_log_view = nullptr;

    QPushButton *m_btn_backup = nullptr;
};
