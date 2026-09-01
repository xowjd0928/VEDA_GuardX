#pragma once

#include <QElapsedTimer>
#include <QFrame>

#include "camera_status.h"

class QComboBox;
class QLabel;
class QPushButton;
class QVariantAnimation;

/**
 * @brief 카메라 앱 카드 한 장 (기획서 §2b·§3-A + 디자인 §5-D)
 *
 * 좌측 4px 상태 바(LIVE 타일 경보 테두리와 같은 언어), Running 상태 점은
 * 느린 호흡, 앱별 자원은 CameraStatus 1초 폴을 타고 갱신된다.
 *
 * Start/Stop은 낙관적 전이: 버튼 즉시 "정지 중…" 표시로 바꾸고 실제
 * 확정은 폴이 한다. 20초 넘게 확정이 안 오면 원복(카메라가 씹은 것).
 */
class AppCard : public QFrame
{
    Q_OBJECT

public:
    AppCard(const QString &app_id, QWidget *parent = nullptr);

    QString app_id() const { return m_id; }

    /** @brief 폴 갱신 반영 — 낙관적 전이 중이면 목표 상태 도달 시 확정 */
    void set_info(const CameraAppInfo &info);

    /** @brief 조작 직후 낙관적 전이 시작 (start=시작 방향) */
    void begin_pending(bool start);
    /** @brief 조작 실패 — 전이 원복 */
    void cancel_pending();

signals:
    void start_requested(const QString &app_id);
    void stop_requested(const QString &app_id);
    /** @brief AutoStart 토글 클릭 (5단계 — enable = 목표값) */
    void autostart_requested(const QString &app_id, bool enable);
    /** @brief Priority 변경 선택 (Low/Medium/High) */
    void priority_requested(const QString &app_id, const QString &priority);

private:
    void render();

    QString m_id;
    CameraAppInfo m_info;

    bool m_pending = false;
    bool m_pending_start = false;   ///< 전이 방향 (목표: Running/Stopped)
    QElapsedTimer m_pending_since;

    QFrame *m_strip = nullptr;      ///< 좌측 4px 상태 바
    QLabel *m_dot = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_res = nullptr;        ///< 앱별 자원 모노 라인
    QPushButton *m_btn_start = nullptr;
    QPushButton *m_btn_stop = nullptr;
    QPushButton *m_btn_autostart = nullptr;  ///< AutoStart 토글 (5단계)
    QComboBox *m_priority = nullptr;         ///< Low/Medium/High
    QVariantAnimation *m_breath = nullptr;  ///< Running 점 호흡
    double m_breath_phase = 1.0;
};
