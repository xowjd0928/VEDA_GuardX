#pragma once

#include "business_flow_page.h"   // AnalyticsDwellRow / AnalyticsFlowRow
#include "report_page.h"          // ReportPage::ChanData / FcRow (예측 모델 재사용)

#include <QJsonObject>
#include <QVector>
#include <QWidget>

class AKpiCard;
class ADwellCard;
class APathsCard;
class ASankeyCard;
class AZoneCard;

class QButtonGroup;
class QLabel;
class QNetworkAccessManager;
class QTimer;

/**
 * @brief ANALYTICS 화면 — Predictions + Flow 통합 (08-20)
 *
 * 옛 Predictions(예측)·Flow(동선) 두 탭을 **대신하는** 화면이다. 같은
 * 데이터를 "처음 보는 사람이 한눈에" 읽도록 재배치했다:
 *
 *  - 상단 KPI 4장은 지표가 아니라 **답**이다 — 지금 어디가 붐비나, 앞으로
 *    어디가 붐비나, 어디에 오래 머무나, 사람들은 어디로 움직이나.
 *  - 구역 카드마다 예측 차트/표 아래에 그 구역의 **동선 한 줄**(들어옴·나감·
 *    평균 체류)이 붙는다 — 두 화면을 오가지 않고 한 카드에서 읽는다.
 *  - 구역 이름·색을 통일한다. 기존 화면은 같은 곳을 Predictions에서 CH2,
 *    Flow에서 Z2로 부르고 색도 제각각이었다 — 여기서는 Z·이름·색이 하나다
 *    (동선 차트도 순위색이 아니라 구역색을 쓴다).
 *  - Flow의 KPI 3장(신뢰 세그먼트·사용 비율·ID 연속성)은 건물이 아니라
 *    추적기 얘기라 헤드라인에서 내리고, 경로 목록 밑 한 문장으로 옮겼다.
 *
 * 데이터 원천은 두 원본 화면과 동일하다. 원본 코드(report_page ·
 * business_flow_page)는 탭에서만 빠졌을 뿐 그대로 살아 있고 계속 빌드된다 —
 * 되돌릴 여지를 남긴 것이므로, 여기 경로를 바꾸면 그쪽도 같이 봐야 한다:
 *  - 현재 인원 + 60분 이력 : 카메라 /occupancy (ReportPage와 동일)
 *  - 예측 4-horizon        : 카메라 /prediction (ReportPage와 동일)
 *  - 6h/24h 이력           : MQTT occseries (ReportPage와 동일)
 *  - 동선 분석             : MQTT trajectory + 더미 폴백 (BusinessFlowPage와 동일)
 *
 * 갱신 주기도 원본을 따른다 — 예측 60초 · 동선 5초.
 *
 * @see nav_rail.cpp items[] · mainwindow.cpp setup_ui() — 옛 두 탭을 되살리는
 *      법이 그 두 자리에 적혀 있다.
 */
class AnalyticsPage : public QWidget
{
    Q_OBJECT

public:
    explicit AnalyticsPage(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *ev) override;

private:
    enum Range { R60 = 0, R6H = 1, R24H = 2 };

    QWidget *build_header();
    QWidget *build_kpi_row();
    QWidget *build_body();

    // ---- 예측 (ReportPage와 같은 경로) ----
    void refresh_predictions();
    void request_camera(int ch, bool pred);
    void request_series(int minutes, int bucket_min, const QString &tag);
    void apply_series(const QJsonObject &reply, const QString &tag);

    // ---- 동선 (BusinessFlowPage와 같은 경로) ----
    void request_flow();
    void apply_flow(const QJsonObject &reply);
    QJsonObject make_dummy_flow() const;

    // ---- 표시 갱신 ----
    void update_views();         ///< 예측 카드 + KPI 1·2 + 인사이트
    void update_flow_views();    ///< 동선 차트·경로 + 카드 동선 줄 + KPI 3·4
    void update_kpis();
    void update_insight();
    void retitle_channels();
    void paint_refresh_label();  ///< 색이 구워진 RichText — 테마 전환 때 다시

    const QVector<double> &hist_for(int ch) const;
    QString range_label() const;
    QVector<ReportChart::FcPt> chart_fc(int ch) const;

    QNetworkAccessManager *m_net = nullptr;
    QTimer *m_pred_timer = nullptr;
    QTimer *m_flow_timer = nullptr;

    // 헤더
    QLabel *m_insight = nullptr;
    QButtonGroup *m_seg_group = nullptr;
    QLabel *m_refresh_lb = nullptr;
    QString m_refresh_at;        ///< 마지막 갱신 시각 (문자열은 그릴 때 만든다)

    // KPI (왼쪽부터: 지금 · 앞으로 · 체류 · 이동)
    AKpiCard *m_kpi_now = nullptr;
    AKpiCard *m_kpi_next = nullptr;
    AKpiCard *m_kpi_dwell = nullptr;
    AKpiCard *m_kpi_move = nullptr;

    // 본문
    AZoneCard *m_cards[4] = {};
    ADwellCard *m_dwell_card = nullptr;
    ASankeyCard *m_sankey_card = nullptr;
    APathsCard *m_paths_card = nullptr;

    // 상태
    Range m_range = R60;
    ReportPage::ChanData m_ch[4];
    QVector<AnalyticsDwellRow> m_dwell;
    QVector<AnalyticsFlowRow> m_flows;
    int m_cov_reliable = 0;
    int m_cov_total = 0;
    double m_cov_confidence = 0.0;
    bool m_cov_dummy = false;
    bool m_flow_received = false;   ///< 첫 응답 전에는 "—"로 그린다
    QString m_flow_request_id;
};
