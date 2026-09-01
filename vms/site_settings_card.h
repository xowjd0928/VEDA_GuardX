#pragma once

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;

/**
 * @brief SETTINGS 의 "현장 설정" 카드 — SITE 문구 편집 + 바닥 캘리브레이션 (08-12)
 *
 * zone_settings_page 의 build_calibration_card() 를 대체한다. 별도 파일인
 * 이유는 소유권이다 — zone_settings_page 는 계정 세션(A)이 크게 고치는 중이라
 * (SETTINGS 탭 분리), 이 카드는 화면 세션(B) 소유 파일로 분리해 페이지에는
 * 생성 한 줄만 남긴다.
 *
 * 동작 (합의 ④ + 10번):
 *  - [Load file...] 은 **적용하지 않는다** — 파싱 검증까지만 하고 대기(staged).
 *    기존 "고르면 즉시 반영"은 잘못 고른 파일이 그대로 평면도를 덮었다.
 *  - [Apply] 가 실제 적용이다. 관리자면 서버로 발행(전역 — 모든 VMS 가 받는다),
 *    운영자면 이 계정의 로컬 오버라이드로만 저장한다.
 *  - SITE 문구는 관리자 전용(서버 저장). 상단바·로그인·REPORT 가 함께 바뀐다.
 */
class SiteSettingsCard : public QWidget
{
    Q_OBJECT

public:
    explicit SiteSettingsCard(QWidget *parent = nullptr);

private:
    void build_ui();
    void pick_file();
    void apply_staged();
    void save_site_name();
    void refresh_status();
    void set_status(QLabel *label, const QString &text, bool error);

    // SITE 문구
    QLineEdit *m_site_edit = nullptr;
    QPushButton *m_site_apply = nullptr;
    QLabel *m_site_status = nullptr;

    // 캘리브레이션
    QPushButton *m_load_btn = nullptr;
    QPushButton *m_apply_btn = nullptr;
    QLabel *m_calib_status = nullptr;
    QCheckBox *m_force = nullptr;

    QJsonObject m_staged;     ///< 불러왔지만 아직 적용 전인 파일 내용
    QString m_staged_path;
};
