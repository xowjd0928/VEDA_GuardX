#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;

/**
 * @brief 카메라 조작 클라이언트 (기획서 §4b — camera_tuner의 일반화)
 *
 * CameraStatus가 "읽기 1원천"이라면 여기는 "쓰기 창구"다. 조작은 전부
 * 이벤트성 1회 호출이고, 결과는 시그널로 알린다. 실제 상태 확정은
 * 호출부가 CameraStatus 재폴로 한다(낙관적 전이 → 폴 확정 루프, §3-A).
 *
 * 4단계: 앱 Start/Stop. 5단계: AutoStart/Priority. 6단계+: 재부팅·프로파일.
 */
class CameraControl : public QObject
{
    Q_OBJECT

public:
    explicit CameraControl(QObject *parent = nullptr);

    /**
     * @brief 앱 시작/정지 — opensdk.cgi apps&action=control
     *
     * ⚠ 우리 카메라는 1앱이 전 채널 관리(ChannelType=Multiple)라 Channel
     * 파라미터가 필요 없다. OneOpenAppPerChannel=true인 기기는 다르다(§3-A).
     */
    void control_app(const QString &app_id, bool start);

    /**
     * @brief 앱 속성 설정 — apps&action=set
     *
     * ⚠ 스펙 예제(2.4.7)대로 **AutoStart와 Priority를 항상 함께** 보낸다 —
     * 실측(08-06): 한 키만 보내면 "Invalid Input Value(s)"로 거부된다.
     * 호출부가 바꾸지 않는 쪽엔 현재값을 채워 넘긴다.
     *
     * @param label 토스트용 — 실제로 바뀐 키 이름 ("AutoStart"/"Priority")
     */
    void set_app(const QString &app_id, bool auto_start,
                 const QString &priority, const QString &label);

    /**
     * @brief 카메라 재부팅 — system.cgi power&action=control&Type=Restart (6단계)
     *
     * ⚠ 호출 전에 반드시 CameraStatus::enter_reboot_mode()를 먼저 — 이후의
     * 폴 실패를 오프라인으로 오해하지 않게. 재부팅이 즉시 시작되면 응답이
     * 안 오고 끊길 수 있어, 타임아웃/연결끊김도 "아마 재부팅 중"으로 본다.
     */
    void reboot_camera();

signals:
    /** @brief 조작 응답 도착 (성공이어도 상태 확정은 재폴이 한다) */
    void app_control_finished(const QString &app_id, bool start, bool ok,
                              const QString &error);

    /** @brief 재부팅 요청 결과 (likely_rebooting=true면 복구 대기로 간주) */
    void reboot_finished(bool likely_rebooting, const QString &detail);

    /** @brief set 응답 도착 (label = set_app에 넘긴 변경 키 이름) */
    void app_set_finished(const QString &app_id, const QString &label, bool ok,
                          const QString &error);

private:
    QNetworkAccessManager *m_net = nullptr;
};
