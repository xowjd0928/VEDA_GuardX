#include "business_flow_page.h"

#include "mqtt_link.h"
#include "theme.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>

namespace {

const QString TRAJECTORY_REQ_TOPIC = "guardx/db/rpib/query/trajectory";
constexpr int ANALYTICS_REFRESH_MS = 5000;
constexpr bool USE_DUMMY_FALLBACK = true;

QString color_css(const QColor &color)
{
    return color.name(QColor::HexRgb);
}

QString seconds_text(double ms)
{
    return QString("%1s").arg(QString::number(ms / 1000.0, 'f', 1));
}

QString zone_label(int zone_id)
{
    return QString("Z%1").arg(zone_id);
}

/**
 * @brief KPI 값에 쓰는 색의 **역할**
 *
 * QColor 를 그대로 들고 다니면 테마가 바뀔 때 옛 팔레트의 색이 굳는다 —
 * 색이 아니라 "왜 이 색인가"를 넘기고, 실제 색은 그릴 때 고른다.
 */
enum class Tone { Accent, Good, Warn };

QColor tone_color(Tone tone)
{
    switch (tone) {
    case Tone::Good: return Theme::green;
    case Tone::Warn: return Theme::amber;
    case Tone::Accent: break;
    }
    return Theme::accent;
}

QString card_style()
{
    // ⚠ **반드시 #FlowCard 로 한정한다.** 선택자 없이 쓰면 Qt 스타일시트가
    //   자식 위젯에도 그대로 적용돼 카드 안 라벨 하나하나에 테두리 상자가
    //   생긴다 (08-19 실측: KPI 카드의 제목·값·설명이 각각 네모를 두르고
    //   있었다). objectName 은 카드 위젯 쪽에서 붙인다.
    //
    // 모서리는 각지게 — 8px 은 DEVICE·SETTINGS 패널(#Panel = 4px)보다도
    // 둥글어 이 화면만 다른 앱처럼 보였다.
    return QString(
        "#FlowCard {"
        "background:%1;"
        "border:1px solid %2;"
        "border-radius:4px; }")
        .arg(color_css(Theme::panel), color_css(Theme::border));
}

}  // namespace

class KpiCard : public QFrame
{
public:
    explicit KpiCard(const QString &title, QWidget *parent = nullptr)
        : QFrame(parent)
    {
        // ⚠ setStyleSheet 를 직접 부르면 **생성 시점 팔레트가 그대로 굳는다.**
        //   restyle 은 지금 한 번 칠하고 테마가 바뀔 때 다시 칠한다(부록 D).
        setObjectName("FlowCard");
        Theme::restyle(this, [] { return card_style(); });
        // 보드 KPI 카드: padding 12/14, gap 4, 자연 높이 ≒ 89px,
        // 라벨 대문자 10/600(자간 1px), 값 mono 22/700
        setMinimumHeight(89);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(4);

        m_title = new QLabel(title.toUpper(), this);
        m_title->setFont(Theme::ui_font(10, 600, 0.1));
        Theme::restyle(m_title, [] {
            return QString("color:%1;").arg(color_css(Theme::textDim));
        });
        layout->addWidget(m_title);

        m_value = new QLabel("--", this);
        m_value->setFont(Theme::mono_font(22, 700));
        Theme::restyle(m_value, [] {
            return QString("color:%1;").arg(color_css(Theme::accent));
        });
        layout->addWidget(m_value);

        m_caption = new QLabel("--", this);
        m_caption->setFont(Theme::mono_font(10));
        Theme::restyle(m_caption, [] {
            return QString("color:%1;").arg(color_css(Theme::textMuted));
        });
        layout->addWidget(m_caption);
    }

    void set_value(const QString &value, const QString &caption, Tone tone)
    {
        m_value->setText(value);
        m_caption->setText(caption);
        // 갱신마다 다시 걸어도 된다 — restyle 은 앞 연결을 끊고 새로 건다.
        // 색이 아니라 tone 을 캡처하므로 테마가 바뀌면 그때 다시 고른다.
        Theme::restyle(m_value, [tone] {
            return QString("color:%1;").arg(color_css(tone_color(tone)));
        });
    }

private:
    QLabel *m_title = nullptr;
    QLabel *m_value = nullptr;
    QLabel *m_caption = nullptr;
};

class DwellChart : public QWidget
{
public:
    explicit DwellChart(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(300);
        setMouseTracking(true);
        setObjectName("FlowCard");
        Theme::restyle(this, [] { return card_style(); });
    }

    void set_rows(const QVector<AnalyticsDwellRow> &rows)
    {
        m_rows = rows;
        std::sort(m_rows.begin(), m_rows.end(), [](const AnalyticsDwellRow &a,
                                                   const AnalyticsDwellRow &b) {
            return a.avg_dwell_ms > b.avg_dwell_ms;
        });
        m_hit_boxes.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        m_hit_boxes.clear();

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), Theme::panel);

        const QRectF area = rect().adjusted(22, 20, -22, -22);
        painter.setPen(Theme::textHi);
        painter.setFont(Theme::ui_font(13, 700, 0.10));
        painter.drawText(QPointF(area.left(), area.top()), "Avg Dwell Time by Zone");

        painter.setPen(Theme::textMuted);
        painter.setFont(Theme::mono_font(9));
        painter.drawText(QPointF(area.left(), area.top() + 22.0),
                         "longer bar = longer average stay, value = visits");

        if (m_rows.isEmpty()) {
            painter.setPen(Theme::textFaint);
            painter.setFont(Theme::mono_font(11));
            painter.drawText(area.adjusted(0, 58, 0, 0),
                             Qt::AlignLeft | Qt::AlignTop,
                             "No dwell data");
            return;
        }

        double max_dwell = 1.0;
        for (const AnalyticsDwellRow &row : m_rows)
            max_dwell = std::max(max_dwell, row.avg_dwell_ms);

        const int row_count = std::min(static_cast<int>(m_rows.size()), 6);
        const double top = area.top() + 62.0;
        const double row_height = 34.0;
        const double label_width = 68.0;
        const double bar_width = area.width() - label_width - 116.0;

        for (int i = 0; i < row_count; ++i) {
            const AnalyticsDwellRow &row = m_rows[i];
            const double y = top + i * row_height;
            const double ratio = row.avg_dwell_ms / max_dwell;
            const QRectF bar(area.left() + label_width, y + 7,
                             bar_width * ratio, 14);

            painter.setPen(Theme::textMuted);
            painter.setFont(Theme::mono_font(10, 700));
            painter.drawText(QRectF(area.left(), y, label_width - 8, 26),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             zone_label(row.zone_id));

            painter.setPen(Qt::NoPen);
            // 보드의 막대는 **각진** 18px 막대에 #111111 트랙이다. 둥근
            // 막대(radius 7)는 짧은 값에서 알약처럼 뭉쳐 길이 비교가 어렵다.
            painter.setBrush(Theme::bg0);
            painter.drawRect(QRectF(area.left() + label_width, y + 5,
                                    bar_width, 18));

            // 순위색도 보드 그대로 — 3위는 호박이 아니라 보라다(호박은 이
            // 앱에서 "주의" 뜻이라 순위 표시에 쓰면 경보처럼 읽힌다).
            QColor bar_color = Theme::accent;
            if (i == 1)
                bar_color = Theme::green;
            else if (i == 2)
                bar_color = QColor(0xB4, 0x8C, 0xFF);
            else if (i > 2)
                bar_color = Theme::textDim;
            painter.setBrush(bar_color);
            painter.drawRect(QRectF(bar.left(), y + 5, bar.width(), 18));
            m_hit_boxes.append({QRectF(area.left() + label_width, y, bar_width + 116.0, 28.0), row});

            painter.setPen(Theme::textHi);
            painter.setFont(Theme::mono_font(10, 600));
            painter.drawText(QRectF(area.left() + label_width + bar_width + 12,
                                    y, 104, 26),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             QString("avg %1 | %2 visits")
                                 .arg(seconds_text(row.avg_dwell_ms))
                                 .arg(row.visit_count));
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        for (const DwellHitBox &hit_box : m_hit_boxes) {
            if (!hit_box.rect.contains(event->position()))
                continue;

            const AnalyticsDwellRow &row = hit_box.row;
            QToolTip::showText(
                event->globalPosition().toPoint(),
                QString("%1\nAverage dwell: %2\nVisits: %3\nAvg confidence: %4")
                    .arg(zone_label(row.zone_id),
                         seconds_text(row.avg_dwell_ms))
                    .arg(row.visit_count)
                    .arg(QString::number(row.avg_confidence, 'f', 2)),
                this);
            return;
        }

        QToolTip::hideText();
    }

    void leaveEvent(QEvent *) override
    {
        QToolTip::hideText();
    }

private:
    struct DwellHitBox {
        QRectF rect;
        AnalyticsDwellRow row;
    };

    QVector<AnalyticsDwellRow> m_rows;
    QVector<DwellHitBox> m_hit_boxes;
};

class SankeyCanvas : public QWidget
{
public:
    explicit SankeyCanvas(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(300);
        setMouseTracking(true);
        setObjectName("FlowCard");
        Theme::restyle(this, [] { return card_style(); });
    }

    void set_flows(const QVector<AnalyticsFlowRow> &flows)
    {
        m_flows = flows;
        std::sort(m_flows.begin(), m_flows.end(), [](const AnalyticsFlowRow &a,
                                                     const AnalyticsFlowRow &b) {
            return a.transition_count > b.transition_count;
        });
        m_hit_boxes.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        m_hit_boxes.clear();

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), Theme::panel);

        const QRectF area = rect().adjusted(22, 20, -22, -22);
        painter.setPen(Theme::textHi);
        painter.setFont(Theme::ui_font(13, 700, 0.10));
        painter.drawText(QPointF(area.left(), area.top()), "Zone-to-Zone Movement");

        painter.setPen(Theme::textMuted);
        painter.setFont(Theme::mono_font(9));
        painter.drawText(QPointF(area.left(), area.top() + 22.0),
                         "line thickness = movement count");

        if (m_flows.isEmpty()) {
            painter.setPen(Theme::textFaint);
            painter.setFont(Theme::mono_font(11));
            painter.drawText(area.adjusted(0, 58, 0, 0),
                             Qt::AlignLeft | Qt::AlignTop,
                             "No transition data");
            return;
        }

        QVector<int> zones;
        int max_count = 1;
        for (const AnalyticsFlowRow &flow : m_flows) {
            if (!zones.contains(flow.from_zone_id))
                zones.append(flow.from_zone_id);
            if (!zones.contains(flow.to_zone_id))
                zones.append(flow.to_zone_id);
            max_count = std::max(max_count, flow.transition_count);
        }
        std::sort(zones.begin(), zones.end());

        const double left_x = area.left() + 56.0;
        const double right_x = area.right() - 56.0;

        auto zone_y = [&](int zone_id) {
            const int index = std::max(0, static_cast<int>(zones.indexOf(zone_id)));
            const double usable = area.height() - 108.0;
            if (zones.size() <= 1)
                return area.center().y();
            return area.top() + 78.0
                + usable * static_cast<double>(index) / static_cast<double>(zones.size() - 1);
        };

        painter.setPen(Theme::textDim);
        painter.setFont(Theme::mono_font(9, 700));
        painter.drawText(QPointF(left_x - 24.0, area.top() + 50.0), "From");
        painter.drawText(QPointF(right_x - 12.0, area.top() + 50.0), "To");

        const int flow_count = std::min(static_cast<int>(m_flows.size()), 8);
        for (int i = flow_count - 1; i >= 0; --i) {
            const AnalyticsFlowRow &flow = m_flows[i];
            const double width = 2.0 + 16.0
                * static_cast<double>(flow.transition_count) / static_cast<double>(max_count);
            // 1·2·3위 색은 옆 차트(AVG DWELL)와 같은 순서를 쓴다 — 두 그림에서
            // 같은 등수가 다른 색이면 나란히 놓고 못 읽는다.
            QColor color = i == 0 ? Theme::accent
                         : i == 1 ? Theme::green
                                  : Theme::amber;
            color.setAlpha(155);

            QPainterPath path;
            const QPointF start(left_x + 28.0, zone_y(flow.from_zone_id));
            const QPointF end(right_x - 28.0, zone_y(flow.to_zone_id));
            const double mid_x = (start.x() + end.x()) / 2.0;
            path.moveTo(start);
            path.cubicTo(QPointF(mid_x, start.y()),
                         QPointF(mid_x, end.y()),
                         end);

            painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
            painter.drawPath(path);
            m_hit_boxes.append({path, width + 12.0, flow});

            if (i < 3) {
                const double label_t = (i % 2 == 0) ? 0.36 : 0.64;
                const QPointF label_point = path.pointAtPercent(label_t);
                const double label_y_offset = (i % 2 == 0) ? -18.0 : 18.0;
                const QString label = QString("%1 moves").arg(flow.transition_count);
                const QRectF label_rect(label_point.x() - 38.0,
                                        label_point.y() + label_y_offset - 11.0,
                                        76.0,
                                        22.0);

                painter.setPen(Qt::NoPen);
                // ⚠ 글자가 Theme::textHi 다. 면을 하드코딩하면 라이트에서
                //   **검정 위 검정**이 된다(08-13 실측).
                QColor chip = Theme::elevated;
                chip.setAlpha(215);
                painter.setBrush(chip);
                painter.drawRoundedRect(label_rect, 5, 5);

                painter.setPen(QPen(Theme::border2, 1));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(label_rect, 5, 5);

                painter.setPen(Theme::textHi);
                painter.setFont(Theme::mono_font(9, 700));
                painter.drawText(label_rect, Qt::AlignCenter, label);
            }
        }

        painter.setFont(Theme::mono_font(11, 700));
        for (int zone_id : zones) {
            const double y = zone_y(zone_id);
            draw_node(&painter, QPointF(left_x, y), zone_label(zone_id));
            draw_node(&painter, QPointF(right_x, y), zone_label(zone_id));
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF position = event->position();

        for (int i = m_hit_boxes.size() - 1; i >= 0; --i) {
            const FlowHitBox &hit_box = m_hit_boxes[i];

            QPainterPathStroker stroker;
            stroker.setWidth(hit_box.width);
            stroker.setCapStyle(Qt::RoundCap);
            stroker.setJoinStyle(Qt::RoundJoin);

            if (!stroker.createStroke(hit_box.path).contains(position))
                continue;

            const AnalyticsFlowRow &flow = hit_box.row;
            QToolTip::showText(
                event->globalPosition().toPoint(),
                QString("%1 -> %2\nTotal moves: %3\nConfirmed: %4\nEstimated: %5\nAvg score: %6")
                    .arg(zone_label(flow.from_zone_id),
                         zone_label(flow.to_zone_id))
                    .arg(flow.transition_count)
                    .arg(flow.confirmed_count)
                    .arg(flow.estimated_count)
                    .arg(QString::number(flow.avg_score, 'f', 2)),
                this);
            return;
        }

        QToolTip::hideText();
    }

    void leaveEvent(QEvent *) override
    {
        QToolTip::hideText();
    }

private:
    void draw_node(QPainter *painter, const QPointF &center, const QString &label)
    {
        const QRectF rect(center.x() - 28.0, center.y() - 16.0, 56.0, 32.0);
        painter->setPen(QPen(Theme::border2, 1));
        painter->setBrush(Theme::elevated);   // 위와 같은 이유 (글자는 textHi)
        painter->drawRoundedRect(rect, 7, 7);
        painter->setPen(Theme::textHi);
        painter->drawText(rect, Qt::AlignCenter, label);
    }

private:
    struct FlowHitBox {
        QPainterPath path;
        double width = 0.0;
        AnalyticsFlowRow row;
    };

    QVector<AnalyticsFlowRow> m_flows;
    QVector<FlowHitBox> m_hit_boxes;
};

BusinessFlowPage::BusinessFlowPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("BusinessFlowPage");
    setAttribute(Qt::WA_StyledBackground);
    Theme::restyle(this, [] {
        return QString("#BusinessFlowPage { background:%1; }")
            .arg(color_css(Theme::bg0));
    });

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(18);

    root->addWidget(build_header());
    root->addWidget(build_kpi_row());

    // 08-19 워크스페이스: 이동 경로 순위는 차트 아래가 아니라 **우측 열**이다
    // (디자인 "Business Flow" 보드) — 차트와 순위를 나란히 본다.
    auto *mid = new QWidget(this);
    auto *mid_lay = new QHBoxLayout(mid);
    mid_lay->setContentsMargins(0, 0, 0, 0);
    mid_lay->setSpacing(14);
    mid_lay->addWidget(build_chart_area(), 1);
    auto *paths = build_transition_panel();
    paths->setFixedWidth(360);
    mid_lay->addWidget(paths);
    root->addWidget(mid, 1);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &BusinessFlowPage::request_analytics);
    m_timer->start(ANALYTICS_REFRESH_MS);

    request_analytics();
}

QWidget *BusinessFlowPage::build_header()
{
    auto *wrap = new QWidget(this);
    auto *layout = new QHBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);

    // 08-19 보드: 화면 제목 줄은 **한 줄 30px** — 13/700(자간 1px) 제목
    // 오른쪽에 mono 10 설명과 초록 인사이트가 나란히 온다. 22/800 + 3줄
    // 스택은 DEVICE·CROWD(13/700)와 달라 화면마다 제목 크기가 널뛰었다.
    auto *left = new QWidget(wrap);
    auto *left_layout = new QHBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(10);

    auto *title = new QLabel("Business Flow Analytics", left);
    title->setFont(Theme::ui_font(13, 700, 0.08));
    Theme::restyle(title, [] {
        return QString("color:%1;").arg(color_css(Theme::textHi));
    });
    left_layout->addWidget(title);

    auto *subtitle = new QLabel(
        "where visitors stayed longest · which zones they moved between", left);
    subtitle->setFont(Theme::mono_font(10));
    Theme::restyle(subtitle, [] {
        return QString("color:%1;").arg(color_css(Theme::textMuted));
    });
    left_layout->addWidget(subtitle);

    m_insight = new QLabel("Waiting for reliable trajectory data", left);
    m_insight->setFont(Theme::mono_font(10, 700));
    Theme::restyle(m_insight, [] {
        return QString("color:%1;").arg(color_css(Theme::green));
    });
    left_layout->addWidget(m_insight);
    left_layout->addStretch(1);

    layout->addWidget(left, 1);

    m_status = new QLabel("analytics waiting", wrap);
    m_status->setFont(Theme::mono_font(10, 700));
    Theme::restyle(m_status, [] {
        return QString("color:%1;").arg(color_css(Theme::textFaint));
    });
    layout->addWidget(m_status, 0, Qt::AlignRight | Qt::AlignBottom);

    return wrap;
}

QWidget *BusinessFlowPage::build_kpi_row()
{
    auto *wrap = new QWidget(this);
    auto *layout = new QHBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    m_reliable_card = new KpiCard("Trusted Segments", wrap);
    m_coverage_card = new KpiCard("Usable Data", wrap);
    m_confidence_card = new KpiCard("ID Confidence", wrap);

    layout->addWidget(m_reliable_card);
    layout->addWidget(m_coverage_card);
    layout->addWidget(m_confidence_card);
    return wrap;
}

QWidget *BusinessFlowPage::build_chart_area()
{
    auto *wrap = new QWidget(this);
    auto *layout = new QHBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    m_dwell_chart = new DwellChart(wrap);
    m_sankey = new SankeyCanvas(wrap);
    layout->addWidget(m_dwell_chart, 1);
    layout->addWidget(m_sankey, 1);
    return wrap;
}

QWidget *BusinessFlowPage::build_transition_panel()
{
    auto *frame = new QFrame(this);
    frame->setObjectName("FlowCard");
    Theme::restyle(frame, [] { return card_style(); });
    frame->setMinimumHeight(150);

    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(10);

    m_transition_title = new QLabel("Most Common Movement Paths", frame);
    m_transition_title->setFont(Theme::ui_font(13, 700, 0.12));
    Theme::restyle(m_transition_title, [] {
        return QString("color:%1;").arg(color_css(Theme::textHi));
    });
    layout->addWidget(m_transition_title);

    m_transition_host = new QWidget(frame);
    auto *grid = new QGridLayout(m_transition_host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(6);
    layout->addWidget(m_transition_host, 1);

    return frame;
}

void BusinessFlowPage::request_analytics()
{
    if (!m_request_id.isEmpty())
        return;

    QJsonObject params;
    params["query"] = "trajectory";
    params["limit"] = 500;

    m_request_id = MqttLink::instance()->request(
        TRAJECTORY_REQ_TOPIC, params,
        [this](const QJsonObject &reply) {
            m_request_id.clear();
            apply_analytics(reply);
        },
        [this](const QString &reason) {
            m_request_id.clear();
            if (USE_DUMMY_FALLBACK) {
                apply_analytics(make_dummy_analytics());
                set_status(QString("dummy analytics - %1").arg(reason), true);
                return;
            }
            set_status(QString("analytics request failed - %1").arg(reason), true);
        },
        MqttLink::DEFAULT_TIMEOUT_MS);

    if (m_request_id.isEmpty()) {
        if (USE_DUMMY_FALLBACK) {
            apply_analytics(make_dummy_analytics());
            set_status("dummy analytics - MQTT offline", true);
        } else {
            set_status("MQTT connection waiting", true);
        }
    } else {
        set_status("analytics request pending", false);
    }
}

void BusinessFlowPage::apply_analytics(const QJsonObject &reply)
{
    if (!reply.value("ok").toBool(true)) {
        set_status(reply.value("error").toString("analytics error"), true);
        return;
    }

    const QJsonObject coverage = reply.value("coverage").toObject();
    const int reliable_count = coverage.value("reliable_count").toInt(
        reply.value("reliable_count").toInt(0));
    const int total_count = coverage.value("total_count").toInt(
        reply.value("total_count").toInt(0));
    const double avg_confidence = coverage.value("avg_confidence").toDouble(
        reply.value("avg_confidence").toDouble(0.0));

    const double coverage_ratio = total_count > 0
        ? static_cast<double>(reliable_count) * 100.0 / static_cast<double>(total_count)
        : 0.0;

    m_reliable_card->set_value(
        QString("%1 / %2").arg(reliable_count).arg(total_count),
        "trusted movement segments",
        Tone::Accent);
    m_coverage_card->set_value(
        QString("%1%").arg(QString::number(coverage_ratio, 'f', 1)),
        "used for analytics",
        coverage_ratio >= 50.0 ? Tone::Good : Tone::Warn);
    m_confidence_card->set_value(
        QString::number(avg_confidence, 'f', 2),
        "ID continuity score",
        avg_confidence >= 0.65 ? Tone::Good : Tone::Warn);

    QVector<AnalyticsDwellRow> dwell_rows;
    QJsonArray dwell_array = reply.value("dwell_summary").toArray();
    if (dwell_array.isEmpty())
        dwell_array = reply.value("dwell").toArray();
    for (const QJsonValue &value : dwell_array) {
        const QJsonObject item = value.toObject();
        AnalyticsDwellRow row;
        row.zone_id = item.value("zone_id").toInt(0);
        row.visit_count = item.value("visit_count").toInt(0);
        row.avg_dwell_ms = item.value("avg_dwell_ms").toDouble(0.0);
        row.avg_confidence = item.value("avg_confidence").toDouble(0.0);
        dwell_rows.append(row);
    }

    QVector<AnalyticsFlowRow> flow_rows;
    QJsonArray flow_array = reply.value("transition_summary").toArray();
    if (flow_array.isEmpty())
        flow_array = reply.value("transitions").toArray();
    for (const QJsonValue &value : flow_array) {
        const QJsonObject item = value.toObject();
        AnalyticsFlowRow row;
        row.from_zone_id = item.value("from_zone_id").toInt(0);
        row.to_zone_id = item.value("to_zone_id").toInt(0);
        row.confirmed_count = item.value("confirmed_count").toInt(0);
        row.estimated_count = item.value("estimated_count").toInt(0);
        row.transition_count = item.value("transition_count").toInt(
            row.confirmed_count + row.estimated_count);
        row.avg_score = item.value("avg_score").toDouble(
            item.value("avg_estimated_score").toDouble(0.0));

        if (row.confirmed_count == 0 && row.estimated_count == 0)
            row.confirmed_count = row.transition_count;

        flow_rows.append(row);
    }

    m_dwell_chart->set_rows(dwell_rows);
    m_sankey->set_flows(flow_rows);
    update_transition_rows(flow_rows);

    if (m_insight) {
        AnalyticsDwellRow top_dwell;
        bool has_dwell = false;
        for (const AnalyticsDwellRow &row : dwell_rows) {
            if (!has_dwell || row.avg_dwell_ms > top_dwell.avg_dwell_ms) {
                top_dwell = row;
                has_dwell = true;
            }
        }

        AnalyticsFlowRow top_flow;
        bool has_flow = false;
        for (const AnalyticsFlowRow &row : flow_rows) {
            if (!has_flow || row.transition_count > top_flow.transition_count) {
                top_flow = row;
                has_flow = true;
            }
        }

        if (has_dwell && has_flow) {
            m_insight->setText(QString::fromUtf8(
                "Top dwell: %1 avg %2 (%3 visits)  ·  Main path: %4 → %5 (%6 moves)")
                                   .arg(zone_label(top_dwell.zone_id),
                                        seconds_text(top_dwell.avg_dwell_ms))
                                   .arg(top_dwell.visit_count)
                                   .arg(zone_label(top_flow.from_zone_id),
                                        zone_label(top_flow.to_zone_id))
                                   .arg(top_flow.transition_count));
        } else if (has_dwell) {
            m_insight->setText(QString("Top dwell: %1 avg %2 (%3 visits)")
                                   .arg(zone_label(top_dwell.zone_id),
                                        seconds_text(top_dwell.avg_dwell_ms))
                                   .arg(top_dwell.visit_count));
        } else {
            m_insight->setText("Waiting for reliable trajectory data");
        }
    }

    set_status(reply.value("dummy").toBool(false)
                   ? "dummy analytics preview"
                   : "analytics updated",
               reply.value("dummy").toBool(false));
}

QJsonObject BusinessFlowPage::make_dummy_analytics() const
{
    QJsonObject root;
    root["ok"] = true;
    root["dummy"] = true;

    QJsonObject coverage;
    coverage["reliable_count"] = 352;
    coverage["total_count"] = 1110;
    coverage["avg_confidence"] = 0.60;
    root["coverage"] = coverage;

    QJsonArray dwell;
    QJsonObject z2;
    z2["zone_id"] = 2;
    z2["visit_count"] = 198;
    z2["avg_dwell_ms"] = 23900.0;
    dwell.append(z2);

    QJsonObject z1;
    z1["zone_id"] = 1;
    z1["visit_count"] = 154;
    z1["avg_dwell_ms"] = 12500.0;
    dwell.append(z1);

    QJsonObject z3;
    z3["zone_id"] = 3;
    z3["visit_count"] = 72;
    z3["avg_dwell_ms"] = 9100.0;
    dwell.append(z3);
    root["dwell_summary"] = dwell;

    QJsonArray transitions;
    QJsonObject f12;
    f12["from_zone_id"] = 1;
    f12["to_zone_id"] = 2;
    f12["confirmed_count"] = 10;
    f12["estimated_count"] = 42;
    f12["transition_count"] = 52;
    f12["avg_score"] = 0.74;
    transitions.append(f12);

    QJsonObject f21;
    f21["from_zone_id"] = 2;
    f21["to_zone_id"] = 1;
    f21["confirmed_count"] = 3;
    f21["estimated_count"] = 18;
    f21["transition_count"] = 21;
    f21["avg_score"] = 0.69;
    transitions.append(f21);

    QJsonObject f23;
    f23["from_zone_id"] = 2;
    f23["to_zone_id"] = 3;
    f23["confirmed_count"] = 2;
    f23["estimated_count"] = 6;
    f23["transition_count"] = 8;
    f23["avg_score"] = 0.66;
    transitions.append(f23);
    root["transition_summary"] = transitions;

    return root;
}

void BusinessFlowPage::set_status(const QString &message, bool warning)
{
    if (!m_status)
        return;

    m_status->setText(message);
    // 갱신마다 다시 걸어도 안전하다(restyle 이 앞 연결을 끊는다). 색이 아니라
    // **경고인지 아닌지**를 캡처하므로 테마가 바뀌면 그때 다시 고른다.
    Theme::restyle(m_status, [warning] {
        return QString("color:%1;")
            .arg(color_css(warning ? Theme::amber : Theme::textMuted));
    });
}

void BusinessFlowPage::update_transition_rows(const QVector<AnalyticsFlowRow> &flows)
{
    auto *grid = static_cast<QGridLayout *>(m_transition_host->layout());
    if (!grid)
        return;

    while (QLayoutItem *item = grid->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    QVector<AnalyticsFlowRow> sorted = flows;
    std::sort(sorted.begin(), sorted.end(), [](const AnalyticsFlowRow &a,
                                               const AnalyticsFlowRow &b) {
        return a.transition_count > b.transition_count;
    });

    if (sorted.isEmpty()) {
        auto *empty = new QLabel("No transition data", m_transition_host);
        empty->setFont(Theme::mono_font(10));
        Theme::restyle(empty, [] {
            return QString("color:%1;").arg(color_css(Theme::textFaint));
        });
        grid->addWidget(empty, 0, 0);
        return;
    }

    const int max_rows = std::min(static_cast<int>(sorted.size()), 6);
    for (int i = 0; i < max_rows; ++i) {
        const AnalyticsFlowRow &row = sorted[i];
        auto *rank = new QLabel(QString("#%1").arg(i + 1), m_transition_host);
        // 보드는 화살표 글리프(→)와 "confirmed N" 한 조각만 쓴다 — ASCII
        // "->" 와 "(N confirmed / M estimated)" 는 한 칸에 다 넣으면 줄이
        // 길어져 순위·경로가 눈에 안 들어온다.
        auto *path = new QLabel(QString::fromUtf8("%1 → %2")
                                    .arg(zone_label(row.from_zone_id),
                                         zone_label(row.to_zone_id)),
                                m_transition_host);
        auto *count = new QLabel(
            QString("%1   confirmed %2")
                .arg(row.transition_count)
                .arg(row.confirmed_count),
            m_transition_host);

        rank->setFont(Theme::mono_font(10, 800));
        path->setFont(Theme::mono_font(11, 700));
        count->setFont(Theme::mono_font(10));

        Theme::restyle(rank, [] {
            return QString("color:%1;").arg(color_css(Theme::accent));
        });
        Theme::restyle(path, [] {
            return QString("color:%1;").arg(color_css(Theme::textHi));
        });
        Theme::restyle(count, [] {
            return QString("color:%1;").arg(color_css(Theme::textMuted));
        });

        const QString tooltip =
            QString("%1 -> %2\nTotal moves: %3\nConfirmed: %4\nEstimated: %5\nAvg score: %6")
                .arg(zone_label(row.from_zone_id),
                     zone_label(row.to_zone_id))
                .arg(row.transition_count)
                .arg(row.confirmed_count)
                .arg(row.estimated_count)
                .arg(QString::number(row.avg_score, 'f', 2));
        rank->setToolTip(tooltip);
        path->setToolTip(tooltip);
        count->setToolTip(tooltip);

        grid->addWidget(rank, i, 0);
        grid->addWidget(path, i, 1);
        grid->addWidget(count, i, 2);
    }
    // 줄은 위에서부터 차곡차곡 — 이게 없으면 QGridLayout 이 남는 높이를
    // 행마다 나눠 가져 6줄이 패널 전체에 흩뿌려진다(보드는 27px 줄 목록).
    grid->setRowStretch(max_rows, 1);
}
