#pragma once

#include <QHash>
#include <QWidget>

class CameraTuner;
class QComboBox;
class QGridLayout;
class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QSpinBox;

/**
 * @brief CAMERA 탭 [프로파일] 하위 탭 — camera_tuner 흡수 (기획서 §3-C, 7단계)
 *
 *  - 채널×프로파일 표: 코덱·해상도·fps·비트레이트·CBR/VBR·GOV
 *    (media.cgi videoprofile view — camera_tuner와 같은 텍스트 파싱)
 *  - 인라인 편집: 선택한 (채널, 프로파일)의 fps·비트레이트·GOV·
 *    BitrateControlType을 update로 적용. ⚠ RTSP 재협상 유발 가능 — 확인 후
 *  - [저지연 프리셋]: CameraTuner::start() 재실행 (멱등 — 이미 최적이면 0건)
 *  - [키프레임 강제]: 선택 스트림에 setsynchronizationpoint
 */
class ProfilePanel : public QWidget
{
    Q_OBJECT

public:
    explicit ProfilePanel(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *ev) override;

private:
    QWidget *build_table_card();
    QWidget *build_editor_card();

    void fetch_profiles();
    void rebuild_table();
    /** @brief 편집 콤보 선택이 바뀌면 필드를 현재값으로 채운다 */
    void load_selection();
    void apply_edit();
    void show_result(const QString &text, bool ok);

    QNetworkAccessManager *m_net = nullptr;
    CameraTuner *m_tuner = nullptr;         ///< 프리셋·키프레임 재사용

    QHash<QString, QString> m_state;        ///< videoprofile view 평탄 맵
    QList<QPair<int, int>> m_rows;          ///< 표에 있는 (채널, 프로파일)

    QGridLayout *m_table = nullptr;
    QLabel *m_result = nullptr;             ///< 적용 결과 토스트

    QComboBox *m_sel_ch = nullptr;
    QComboBox *m_sel_profile = nullptr;
    QSpinBox *m_fps = nullptr;
    QSpinBox *m_bitrate = nullptr;
    QSpinBox *m_gov = nullptr;
    QComboBox *m_cbr = nullptr;
    QPushButton *m_btn_apply = nullptr;
    bool m_loading = false;                 ///< 필드 채우는 중 시그널 무시
};
