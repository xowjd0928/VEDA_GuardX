#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QWidget>

class DwellChart;
class KpiCard;
class QLabel;
class QTimer;
class SankeyCanvas;

struct AnalyticsDwellRow {
    int zone_id = 0;
    int visit_count = 0;
    double avg_dwell_ms = 0.0;
    double avg_confidence = 0.0;
};

struct AnalyticsFlowRow {
    int from_zone_id = 0;
    int to_zone_id = 0;
    int transition_count = 0;
    int confirmed_count = 0;
    int estimated_count = 0;
    double avg_score = 0.0;
};

/**
 * Shows business-oriented movement analytics based on reliable trajectory data.
 */
class BusinessFlowPage : public QWidget
{
    Q_OBJECT

public:
    explicit BusinessFlowPage(QWidget *parent = nullptr);

private:
    QWidget *build_header();
    QWidget *build_kpi_row();
    QWidget *build_chart_area();
    QWidget *build_transition_panel();

    void request_analytics();
    void apply_analytics(const QJsonObject &reply);
    QJsonObject make_dummy_analytics() const;
    void set_status(const QString &message, bool warning);
    void update_transition_rows(const QVector<AnalyticsFlowRow> &flows);

private:
    KpiCard *m_reliable_card = nullptr;
    KpiCard *m_coverage_card = nullptr;
    KpiCard *m_confidence_card = nullptr;
    DwellChart *m_dwell_chart = nullptr;
    SankeyCanvas *m_sankey = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_insight = nullptr;
    QLabel *m_transition_title = nullptr;
    QWidget *m_transition_host = nullptr;
    QTimer *m_timer = nullptr;
    QString m_request_id;
};
