#pragma once

#include "report_chart.h"

#include <QJsonObject>
#include <QVector>
#include <QWidget>

class QButtonGroup;
class QGridLayout;
class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QStackedWidget;
class QTimer;

class ChannelCard;   // report_page.cpp 내부 위젯 (그리드 카드)
class MiniCard;      // report_page.cpp 내부 위젯 (COMPARE 미니 카드)

/**
 * @brief Post-Analysis Reports 화면 — 2×2 채널 카드 + COMPARE 오버레이
 *
 * 디자인: Downloads/…/design_handoff_reports_tab (README §2a가 최종안).
 * QTextBrowser 구현(07-30)을 커스텀 페인팅으로 교체 — 차트는 ReportChart.
 *
 * 데이터 원천 (실시간=카메라 직결 · 이력/설정=DB via MQTT 원칙 유지):
 *  - 현재 인원 + 60분 이력  : 카메라 /occupancy?channel=N (series_1min)
 *  - 예측 4-horizon + 모델 메타: 카메라 /prediction?channel=N&capacity=cap
 *  - 정원·임계(pill 기준)    : ZoneConfig (MQTT guardx/db/rpib/zones)
 *  - 6h/24h 이력            : MQTT guardx/db/rpib/query/occseries
 *                             (RPi B zone_occupancy 집계 — heatday와 같은
 *                             요청-응답, req_id로 짝 맞춤)
 *
 * 60초 자동 갱신 + REFRESH + 화면 진입 시 즉시 갱신.
 */
class ReportPage : public QWidget
{
    Q_OBJECT

public:
    explicit ReportPage(QWidget *parent = nullptr);

    // 카드 위젯(ChannelCard/MiniCard, report_page.cpp 내부)이 함께 쓰는 모델
    struct FcRow {
        int minutes = 0;
        double p50 = -1, p10 = -1, p90 = -1;
        double pov = -1;                 ///< P(정원 초과), -1 = 불명 (0 아님!)
    };
    struct ChanData {
        int cur = 0;
        bool warmup = true;
        QString model = QStringLiteral("—");
        double mae = -1;
        double profile_days = -1;
        int obs = 0;
        QVector<double> h60, h6h, h24h;  ///< 이력 (과거→현재)
        QVector<FcRow> fc;
    };

protected:
    void showEvent(QShowEvent *ev) override;

private:
    enum Range { R60 = 0, R6H = 1, R24H = 2 };

    QWidget *build_header();
    QWidget *build_grid_view();
    QWidget *build_compare_view();
    QWidget *build_predicted_panel();   ///< 예측이 유발한 혼잡 경보 (AlertFeed)

    void update_predicted();            ///< 경보 이력 -> 예측 유발분만 표

    void refresh_all();

    /// 구역 이름(zones.zone_name)이 바뀌면 카드 제목·COMPARE 칩을 다시 쓴다.
    /// 이 라벨들은 생성 시 한 번 채워지므로 스스로 갱신되지 않는다.
    void retitle_channels();

    void request_camera(int ch, bool pred);
    void request_series(int minutes, int bucket_min, const QString &tag);
    void apply_series(const QJsonObject &reply, const QString &tag);

    void update_views();                 ///< m_ch -> 카드/오버레이 전체 반영
    void update_compare_layout();        ///< 선택 채널 수에 맞춰 미니 그리드 재배치
    const QVector<double> &hist_for(int ch) const;
    QString range_label() const;
    QVector<ReportChart::FcPt> chart_fc(int ch) const;

    QNetworkAccessManager *m_net = nullptr;
    QTimer *m_timer = nullptr;

    // 헤더
    QPushButton *m_btn_compare = nullptr;
    QButtonGroup *m_seg_group = nullptr;
    QLabel *m_refresh_lb = nullptr;

    // 본문
    QStackedWidget *m_stack = nullptr;
    ChannelCard *m_cards[4] = {};
    QPushButton *m_chips[4] = {};
    ReportChart *m_overlay = nullptr;
    MiniCard *m_minis[4] = {};
    QGridLayout *m_mini_grid = nullptr;

    // 예측 유발 경보 패널
    QGridLayout *m_pred_rows = nullptr;
    QLabel *m_pred_caption = nullptr;

    // 상태 (핸드오프 §State Management)
    Range m_range = R60;
    bool m_compare = false;
    bool m_sel[4] = { true, true, true, true };   ///< 불변식: 최소 1개 true
    ChanData m_ch[4];
};
