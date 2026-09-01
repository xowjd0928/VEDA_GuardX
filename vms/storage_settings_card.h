#pragma once

#include <QWidget>

class QLabel;

/**
 * @brief SETTINGS 의 "Event recording" 카드 — 클립 저장 폴더 + 테스트 녹화
 *
 * ClipRecorder(화재·혼잡 critical 전후 15초 클립)의 사용자 접점:
 *  - 저장 폴더 표시·변경([Change...])·열기([Open]) — QSettings "storage_dir",
 *    이 PC·이 계정의 로컬 설정이라 관리자 게이트가 없다 (테마와 같은 급).
 *  - [Record test clip] — 실제 이벤트 경로 그대로 전 채널 클립을 굽는다.
 *    운영자가 고른 폴더에 정말 써지는지 이벤트 없이 확인하는 용도.
 *
 * 카드 chrome 은 SiteSettingsCard 패턴 (#Panel + PanelChrome::header).
 */
class StorageSettingsCard : public QWidget
{
    Q_OBJECT

public:
    explicit StorageSettingsCard(QWidget *parent = nullptr);

private:
    void refresh_dir_label();
    void set_status(const QString &text, bool error);

    QLabel *m_dir_lbl = nullptr;
    QLabel *m_status = nullptr;
    bool m_status_error = false;   ///< restyle 람다가 읽는다 (등록은 1회)
};
