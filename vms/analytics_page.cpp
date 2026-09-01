#include "analytics_page.h"
#include "credentials.h"
#include "mqtt_link.h"
#include "sunapi_request.h"
#include "theme.h"
#include "zone_config.h"

#include <QAuthenticator>
#include <QButtonGroup>
#include <QDebug>
#include <QFontMetricsF>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QTime>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

// 갱신 주기 — 원본 화면 그대로다 (예측 60초 = ReportPage, 동선 5초 =
// BusinessFlowPage). 모델이 분 단위로 내는 예측을 더 자주 긁을 이유가 없고,
// 동선은 5초면 사람이 움직이는 속도를 따라간다. 두 원본은 되살릴 여지를
// 남겨 둔 코드라, 여기를 바꾸면 그쪽 상수도 같이 봐야 갈리지 않는다.
const int PRED_REFRESH_MS = 60 * 1000;
const int FLOW_REFRESH_MS = 5000;

const QString PRED_PATH = "/opensdk/juan_application/prediction";
const QString OCC_PATH = "/opensdk/juan_application/occupancy";
const QString OCCSERIES_TOPIC = "guardx/db/rpib/query/occseries";
const QString TRAJECTORY_REQ_TOPIC = "guardx/db/rpib/query/trajectory";
constexpr bool USE_DUMMY_FALLBACK = true;   // BusinessFlowPage와 동일 정책

// 이력 해상도 (report_page.cpp와 동일 — 버킷 크기가 점 수를 결정한다)
const int SLOTS_6H = 360 / 5;
const int SLOTS_24H = 1440 / 15;

QString hex(const QColor &c) { return c.name(); }
QString fmt1(double v) { return QString::number(v, 'f', 1); }
const QString DASH = QString::fromUtf8("—");

/** @brief "CH1 · LOBBY EAST" -> {"CH1", "LOBBY EAST"} */
QStringList split_channel_name(int ch)
{
    QStringList parts = Theme::channel_name(ch).split(QString::fromUtf8(" · "));
    if (parts.size() < 2)
        parts = { QString("CH%1").arg(ch + 1), QString() };
    return parts;
}

QString zone_tag(int zone_id)
{
    return QString("Z%1").arg(zone_id);
}

/**
 * @brief 구역색 = **그 구역이 매핑된 채널의 색**
 *
 * 이 페이지의 핵심 규칙이다: 카드(채널 기준)와 동선 차트(zone_id 기준)가
 * 같은 곳을 같은 색으로 그려야 "저 초록 막대가 저 초록 카드"로 읽힌다.
 * 원본 Flow 화면은 순위색(1위 파랑·2위 초록)이라 갱신마다 색이 옮겨 다녔다.
 */
QColor zone_color(int zone_id)
{
    const int ch = ZoneConfig::zone_channel(zone_id);
    return ch >= 0 ? ReportTk::channel_color(ch) : ReportTk::textFaint;
}

QString seconds_text(double ms)
{
    return QString("%1s").arg(QString::number(ms / 1000.0, 'f', 1));
}

/** @brief GETTING FULL / PLENTY OF ROOM 칩 — report_page와 동일 규칙 */
QString pill_style(bool near)
{
    const QColor c = near ? ReportTk::amber : ReportTk::green;
    return QString("color:%1; background:rgba(%2,%3,%4,15%);"
                   "border:none; border-radius:0px; padding:2px 8px;")
        .arg(c.name())
        .arg(c.red()).arg(c.green()).arg(c.blue());
}

/**
 * @brief KPI 값 색의 **역할** (business_flow_page의 Tone과 같은 패턴)
 *
 * QColor를 캡처하면 테마 전환 때 옛 팔레트가 굳는다 — 역할만 들고 다니고
 * 색은 그릴 때 고른다.
 */
enum class KpiTone { Info, Good, Warn, Bad };

QColor kpi_color(KpiTone tone)
{
    switch (tone) {
    case KpiTone::Good: return ReportTk::green;
    case KpiTone::Warn: return ReportTk::amber;
    case KpiTone::Bad:  return Theme::alarm;
    case KpiTone::Info: break;
    }
    return ReportTk::blue;
}

} // namespace

// ============================================================ AForecastTable
/**
 * @brief 4열 예측 표 — report_page.cpp ForecastTable의 사본
 *
 * 원본은 report_page.cpp 파일 내부 클래스라 가져다 쓸 수 없다. 통합 시안이
 * 채택되면 하나로 합치면 되고, 그 전까지는 **표기 규칙까지 똑같이** 둔다
 * (p_over_capacity == -1 은 '—', 0%로 표기 금지 — 07-30 규칙).
 */
class AForecastTable : public QWidget
{
public:
    explicit AForecastTable(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedWidth(216);

        auto *g = new QGridLayout(this);
        g->setContentsMargins(0, 0, 0, 0);
        g->setHorizontalSpacing(8);
        g->setVerticalSpacing(4);
        g->setColumnMinimumWidth(0, 38);
        g->setColumnMinimumWidth(3, 44);
        g->setColumnStretch(1, 1);
        g->setColumnStretch(2, 1);

        const QFont f9 = Theme::mono_font(9);
        const char *heads[3] = { "When", "Likely", "Low\xE2\x80\x93high" };
        for (int c = 0; c < 3; ++c) {
            auto *h = new QLabel(QString::fromUtf8(heads[c]), this);
            h->setFont(f9);
            Theme::restyle(h, [=] {
                return QString("color:%1;").arg(hex(ReportTk::textFaint));
            });
            g->addWidget(h, 0, c);
        }
        m_pov_head = new QLabel(this);
        m_pov_head->setFont(f9);
        Theme::restyle(m_pov_head, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textFaint));
        });
        m_pov_head->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        g->addWidget(m_pov_head, 0, 3);

        const QFont f10 = Theme::mono_font(10);
        const QFont f10b = Theme::mono_font(10, 600);
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                auto *lb = new QLabel(DASH, this);
                lb->setFont(c == 1 ? f10b : f10);
                if (c == 3)
                    lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                g->addWidget(lb, r + 1, c);
                m_cell[r][c] = lb;
            }
        }
    }

    void set_data(const QVector<ReportPage::FcRow> &rows, bool warmup, int cap)
    {
        m_pov_head->setText(QString("Over %1?").arg(cap));

        for (int r = 0; r < 4; ++r) {
            if (r >= rows.size()) {
                for (int c = 0; c < 4; ++c) {
                    m_cell[r][c]->setText(DASH);
                    m_cell[r][c]->setStyleSheet(
                        QString("color:%1;").arg(hex(ReportTk::textFaint)));
                }
                continue;
            }
            const ReportPage::FcRow &f = rows[r];

            m_cell[r][0]->setText(QString("+%1m").arg(f.minutes));
            m_cell[r][0]->setStyleSheet(
                QString("color:%1;").arg(hex(ReportTk::blueLight)));

            const bool band = !warmup && f.p10 >= 0 && f.p90 >= 0;
            m_cell[r][1]->setText(warmup || f.p50 < 0 ? DASH : fmt1(f.p50));
            m_cell[r][1]->setStyleSheet(
                QString("color:%1;").arg(hex(ReportTk::textHi)));
            m_cell[r][2]->setText(
                band ? QString::fromUtf8("%1 – %2").arg(fmt1(f.p10), fmt1(f.p90))
                     : DASH);
            m_cell[r][2]->setStyleSheet(
                QString("color:%1;").arg(hex(ReportTk::textSec)));

            if (warmup || f.pov < 0) {
                m_cell[r][3]->setText(DASH);
                m_cell[r][3]->setStyleSheet(
                    QString("color:%1;").arg(hex(ReportTk::textFaint)));
            } else {
                m_cell[r][3]->setText(fmt1(f.pov * 100) + "%");
                const QColor c = f.pov > 0.5 ? Theme::alarm
                               : f.pov > 0.2 ? ReportTk::amber
                                             : ReportTk::green;
                m_cell[r][3]->setStyleSheet(QString("color:%1;").arg(hex(c)));
            }
        }
    }

private:
    QLabel *m_pov_head = nullptr;
    QLabel *m_cell[4][4] = {};
};

// ============================================================ AZoneCard
/**
 * @brief 구역 카드 1장 — 예측(차트+표) 위, 동선 한 줄 아래
 *
 * ChannelCard(report_page.cpp)의 구성에 두 가지를 더했다:
 *  - 제목이 채널이 아니라 **구역**이다: "Z1 LOBBY EAST" + 작은 CH1 태그.
 *    채널 번호는 카메라 배선의 문제고, 운영자가 아는 이름은 구역이다.
 *  - 하단 WHO PASSES THROUGH 줄 — 이 구역으로 들어온/나간 이동과 평균 체류.
 *    예측과 동선을 한 시야에 두는 것이 이 통합 시안의 존재 이유다.
 */
class AZoneCard : public QFrame
{
public:
    explicit AZoneCard(int ch, QWidget *parent = nullptr)
        : QFrame(parent), m_ch_index(ch)
    {
        setObjectName("ACard");

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(16, 16, 16, 16);
        root->setSpacing(10);

        // ── 헤더 ──
        auto *head = new QHBoxLayout;
        head->setSpacing(12);

        auto *left = new QVBoxLayout;
        left->setSpacing(3);
        auto *name_row = new QHBoxLayout;
        name_row->setSpacing(8);
        m_id = new QLabel(this);
        m_id->setFont(Theme::mono_font(12, 600));
        Theme::restyle(m_id, [ch] {
            return QString("color:%1;").arg(hex(ReportTk::channel_color(ch)));
        });
        m_name = new QLabel(this);
        m_name->setFont(Theme::ui_font(13, 700, 1.0 / 13));
        // 구역 이름은 DB(MQTT)에서 온 남의 글자다 — QLabel 기본 AutoText 는
        // '<'가 든 이름을 HTML 로 해석한다. 화면 이름은 서식이 아니라 글자다.
        m_name->setTextFormat(Qt::PlainText);
        m_name->setMinimumWidth(1);   // 긴 구역명이 카드 최소 폭을 밀지 않게
        Theme::restyle(m_name, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textHi));
        });
        m_tag = new QLabel(QString("CH%1").arg(ch + 1), this);
        m_tag->setFont(Theme::mono_font(9));
        Theme::restyle(m_tag, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textFaint));
        });
        name_row->addWidget(m_id, 0, Qt::AlignBottom);
        name_row->addWidget(m_name, 0, Qt::AlignBottom);
        name_row->addWidget(m_tag, 0, Qt::AlignBottom);
        name_row->addStretch(1);
        left->addLayout(name_row);

        m_meta = new QLabel(DASH, this);
        m_meta->setFont(Theme::mono_font(9));
        // 이 줄이 카드 최소 폭을 정하게 두지 않는다 — 실데이터가 오면
        // "(24,031 readings)"까지 79자라, 카드 2장 + 우측 열이 1600px 창을
        // 넘겨 가로 스크롤이 생긴다(08-20 실측). 좁을 때는 꼬리가 잘리는
        // 쪽이 화면 전체가 옆으로 밀리는 것보다 낫다.
        m_meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        Theme::restyle(m_meta, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textFaint));
        });
        left->addWidget(m_meta);
        head->addLayout(left, 1);

        auto *right = new QVBoxLayout;
        right->setSpacing(3);
        m_cur = new QLabel(this);
        m_cur->setTextFormat(Qt::RichText);
        m_cur->setFont(Theme::mono_font(22, 600));
        m_cur->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        right->addWidget(m_cur);
        m_pill = new QLabel(this);
        m_pill->setFont(Theme::mono_font(9, 400, 1.0 / 9));
        right->addWidget(m_pill, 0, Qt::AlignRight);
        head->addLayout(right, 0);
        root->addLayout(head);

        // ── 본문: 차트 + 표 ──
        auto *body = new QHBoxLayout;
        body->setSpacing(14);
        m_chart = new ReportChart(168, this);
        body->addWidget(m_chart, 1);
        m_table = new AForecastTable(this);
        body->addWidget(m_table, 0, Qt::AlignVCenter);
        root->addLayout(body);

        // ── 동선 줄 ──
        auto *sep = new QFrame(this);
        sep->setFixedHeight(1);
        sep->setAttribute(Qt::WA_StyledBackground);
        Theme::restyle(sep, [] {
            return QString("background:%1;").arg(hex(ReportTk::gridLine));
        });
        root->addWidget(sep);

        auto *strip = new QHBoxLayout;
        strip->setSpacing(12);
        auto *strip_lb = new QLabel("WHO PASSES THROUGH", this);
        strip_lb->setFont(Theme::mono_font(9, 400, 1.0 / 9));
        Theme::restyle(strip_lb, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textFaint));
        });
        strip->addWidget(strip_lb);
        // in/out 은 데이터에 따라 길이가 변한다 — 최소 1px 로 눌러 카드
        // 최소 폭을 잡지 못하게 한다 (좁으면 꼬리가 잘린다)
        m_in = new QLabel(DASH, this);
        m_in->setFont(Theme::mono_font(10));
        m_in->setMinimumWidth(1);
        Theme::restyle(m_in, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textHi));
        });
        strip->addWidget(m_in);
        m_out = new QLabel(DASH, this);
        m_out->setFont(Theme::mono_font(10));
        m_out->setMinimumWidth(1);
        Theme::restyle(m_out, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textHi));
        });
        strip->addWidget(m_out);
        strip->addStretch(1);
        m_stay = new QLabel(DASH, this);
        m_stay->setFont(Theme::mono_font(10));
        Theme::restyle(m_stay, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textSec));
        });
        strip->addWidget(m_stay);
        root->addLayout(strip);

        retitle(ch);
    }

    void set_data(const ReportPage::ChanData &d, int cap, bool near,
                  const QVector<double> &hist,
                  const QVector<ReportChart::FcPt> &fc,
                  const QString &range_label)
    {
        m_meta->setText(
            QString::fromUtf8("typically off by %1 people · learned from %2 days "
                              "of history (%3 readings)")
                .arg(d.mae < 0 ? DASH : QString::number(d.mae, 'f', 1),
                     d.profile_days < 0 ? DASH : fmt1(d.profile_days),
                     QLocale(QLocale::English).toString(d.obs)));
        m_meta->setToolTip(QString("model %1 · MAE %2 · profile %3 d · obs %4")
                               .arg(d.model,
                                    d.mae < 0 ? DASH : QString::number(d.mae, 'f', 3),
                                    d.profile_days < 0 ? DASH : fmt1(d.profile_days),
                                    QLocale(QLocale::English).toString(d.obs)));

        m_cur->setText(QString("<span style='color:%1;'>%2</span>"
                               "<span style='font-size:%5px;color:%3;'> / %4</span>")
                           .arg(hex(ReportTk::textHi))
                           .arg(d.cur)
                           .arg(hex(ReportTk::textSec))
                           .arg(cap)
                           .arg(Theme::px(11)));

        m_pill->setText(near ? "GETTING FULL" : "PLENTY OF ROOM");
        m_pill->setStyleSheet(pill_style(near));

        // 시리즈색 = 구역색 — 카드 제목·동선 차트와 같은 색이어야 한다.
        // 밴드는 그 색의 밝은 판. CH1 만 앱이 팔레트에 지정해 둔 blueLight 를
        // 쓴다 — 파랑 계열의 밝은 값은 이미 정해져 있으므로 굳이 계산하지
        // 않는다(Device 탭 차트·게이지와도 같은 값이다).
        const QColor series = ReportTk::channel_color(m_ch_index);
        m_chart->set_single({ series, hist, fc,
                              m_ch_index == 0 ? ReportTk::blueLight
                                              : series.lighter(125) },
                            cap, range_label);
        m_table->set_data(d.fc, d.warmup, cap);
    }

    void set_flow(const QString &in, const QString &out, const QString &stay)
    {
        m_in->setText(in);
        m_out->setText(out);
        m_stay->setText(stay);
    }

    /** @brief 구역 이름/매핑이 DB에서 바뀌었을 때 (ZoneConfig::Notifier) */
    void retitle(int ch)
    {
        const QStringList nm = split_channel_name(ch);
        m_id->setText(zone_tag(ZoneConfig::zone_id(ch)));
        // 이름을 아직 못 받았으면 **비워 둔다** — nm[0]("CH1")로 채우면 옆의
        // CH 태그와 겹쳐 "Z1 CH1 CH1"이 된다 (브로커가 죽은 데모에서 상시
        // 보이는 상태였다, 08-20 리뷰 확인).
        m_name->setText(nm[1]);
        m_tag->setText(QString("CH%1").arg(ch + 1));
    }

private:
    int m_ch_index = 0;
    QLabel *m_id = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_tag = nullptr;
    QLabel *m_meta = nullptr;
    QLabel *m_cur = nullptr;
    QLabel *m_pill = nullptr;
    ReportChart *m_chart = nullptr;
    AForecastTable *m_table = nullptr;
    QLabel *m_in = nullptr;
    QLabel *m_out = nullptr;
    QLabel *m_stay = nullptr;
};

// ============================================================ AKpiCard
/**
 * @brief 상단 KPI 카드 — 라벨은 질문, 값은 답
 *
 * business_flow_page의 KpiCard와 같은 골격(패딩 12/14 · 라벨 10/600 ·
 * 값 mono 22/700)이되, 이 페이지의 ReportTk 토큰을 쓴다.
 */
class AKpiCard : public QFrame
{
public:
    explicit AKpiCard(const QString &title, QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName("ACard");
        setMinimumHeight(89);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(4);

        auto *lb = new QLabel(title.toUpper(), this);
        lb->setFont(Theme::ui_font(10, 600, 0.1));
        Theme::restyle(lb, [] {
            return QString("color:%1;").arg(hex(ReportTk::textFaint));
        });
        layout->addWidget(lb);

        m_value = new QLabel(DASH, this);
        m_value->setFont(Theme::mono_font(22, 700));
        layout->addWidget(m_value);

        m_caption = new QLabel(DASH, this);
        m_caption->setFont(Theme::mono_font(10));
        // 카드 4장이 한 줄을 나눠 쓴다 — 긴 설명이 페이지 최소 폭을 밀어
        // 올리지 않게 한다 (좁으면 꼬리가 잘린다)
        m_caption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        Theme::restyle(m_caption, [] {
            return QString("color:%1;").arg(hex(ReportTk::textSec));
        });
        layout->addWidget(m_caption);

        set_value(DASH, DASH, KpiTone::Info);
    }

    void set_value(const QString &value, const QString &caption, KpiTone tone)
    {
        m_value->setText(value);
        m_caption->setText(caption);
        // 색이 아니라 역할을 캡처 — 테마가 바뀌면 그때 다시 고른다
        Theme::restyle(m_value, [tone] {
            return QString("color:%1;").arg(hex(kpi_color(tone)));
        });
    }

private:
    QLabel *m_value = nullptr;
    QLabel *m_caption = nullptr;
};

// ============================================================ ADwellCard
/**
 * @brief 구역별 평균 체류 막대 — business_flow_page DwellChart의 구역색 판
 *
 * 순위색(1위 파랑·2위 초록·3위 보라) 대신 **구역색**을 쓴다 — 왼쪽 카드와
 * 같은 곳이 같은 색이어야 나란히 놓고 읽힌다. 각진 막대·트랙 등 나머지
 * 규칙은 원본과 같다.
 */
class ADwellCard : public QFrame
{
public:
    explicit ADwellCard(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName("ACard");
        setMinimumHeight(216);
        setMouseTracking(true);
    }

    void set_rows(const QVector<AnalyticsDwellRow> &rows, bool received)
    {
        m_rows = rows;
        m_received = received;
        std::sort(m_rows.begin(), m_rows.end(),
                  [](const AnalyticsDwellRow &a, const AnalyticsDwellRow &b) {
                      return a.avg_dwell_ms > b.avg_dwell_ms;
                  });
        m_hit_boxes.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent *ev) override
    {
        QFrame::paintEvent(ev);   // QSS 카드 배경·테두리 먼저
        m_hit_boxes.clear();

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF area = rect().adjusted(22, 20, -22, -20);
        painter.setPen(ReportTk::textHi);
        painter.setFont(Theme::ui_font(13, 700, 0.10));
        painter.drawText(QPointF(area.left(), area.top()), "Avg Dwell Time by Zone");

        painter.setPen(ReportTk::textSec);
        painter.setFont(Theme::mono_font(9));
        painter.drawText(QPointF(area.left(), area.top() + 22.0),
                         "longer bar = longer average stay, value = visits");

        if (m_rows.isEmpty()) {
            painter.setPen(ReportTk::textFaint);
            painter.setFont(Theme::mono_font(11));
            painter.drawText(area.adjusted(0, 46, 0, 0),
                             Qt::AlignLeft | Qt::AlignTop,
                             m_received ? "No dwell data"
                                        : "waiting for movement data");
            return;
        }

        double max_dwell = 1.0;
        for (const AnalyticsDwellRow &row : m_rows)
            max_dwell = std::max(max_dwell, row.avg_dwell_ms);

        const int row_count = std::min(static_cast<int>(m_rows.size()), 4);
        const double top = area.top() + 44.0;
        const double row_height = 34.0;
        const double label_width = 44.0;

        // 값 라벨 자리는 **실제 글자를 재서** 잡는다. 상수(158px)로 두면
        // 체류가 세 자리(102.3s)거나 방문이 네 자리(1523 visits)가 되는 순간
        // drawText 가 rect 로 잘라 "…1523 visi" 로 끝난다 (08-20 리뷰 확인).
        const QFont value_font = Theme::mono_font(10, 600);
        const QFontMetricsF value_fm(value_font);
        QVector<QString> value_texts;
        double value_width = 0.0;
        for (int i = 0; i < row_count; ++i) {
            const QString t = QString("avg %1 | %2 visits")
                                  .arg(seconds_text(m_rows[i].avg_dwell_ms))
                                  .arg(m_rows[i].visit_count);
            value_texts.append(t);
            value_width = std::max(value_width, value_fm.horizontalAdvance(t));
        }
        value_width += 4.0;   // 마지막 글자가 테두리에 닿지 않게

        const double bar_width =
            std::max(40.0, area.width() - label_width - value_width - 12.0);

        for (int i = 0; i < row_count; ++i) {
            const AnalyticsDwellRow &row = m_rows[i];
            const double y = top + i * row_height;

            painter.setPen(ReportTk::textSec);
            painter.setFont(Theme::mono_font(10, 700));
            painter.drawText(QRectF(area.left(), y, label_width - 8, 26),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             zone_tag(row.zone_id));

            painter.setPen(Qt::NoPen);
            painter.setBrush(ReportTk::pageBg);   // 각진 트랙 (원본 규칙)
            painter.drawRect(QRectF(area.left() + label_width, y + 5,
                                    bar_width, 18));

            painter.setBrush(zone_color(row.zone_id));
            painter.drawRect(QRectF(area.left() + label_width, y + 5,
                                    bar_width * (row.avg_dwell_ms / max_dwell),
                                    18));
            m_hit_boxes.append({ QRectF(area.left() + label_width, y,
                                        bar_width + value_width, 28.0), row });

            painter.setPen(ReportTk::textHi);
            painter.setFont(value_font);
            painter.drawText(QRectF(area.left() + label_width + bar_width + 12,
                                    y, value_width, 26),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             value_texts[i]);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        for (const HitBox &hit : m_hit_boxes) {
            if (!hit.rect.contains(event->position()))
                continue;
            QToolTip::showText(
                event->globalPosition().toPoint(),
                QString("%1\nAverage dwell: %2\nVisits: %3\nAvg confidence: %4")
                    .arg(zone_tag(hit.row.zone_id),
                         seconds_text(hit.row.avg_dwell_ms))
                    .arg(hit.row.visit_count)
                    .arg(QString::number(hit.row.avg_confidence, 'f', 2)),
                this);
            return;
        }
        QToolTip::hideText();
    }

private:
    struct HitBox {
        QRectF rect;
        AnalyticsDwellRow row;
    };

    QVector<AnalyticsDwellRow> m_rows;
    QVector<HitBox> m_hit_boxes;
    bool m_received = false;
};

// ============================================================ ASankeyCard
/**
 * @brief 구역 간 이동 곡선 — business_flow_page SankeyCanvas의 구역색 판
 *
 * 선색 = 출발 구역색 (원본은 순위색). 노드·칩·두께 규칙은 원본 그대로.
 */
class ASankeyCard : public QFrame
{
public:
    explicit ASankeyCard(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName("ACard");
        setMinimumHeight(250);
        setMouseTracking(true);
    }

    void set_flows(const QVector<AnalyticsFlowRow> &flows, bool received)
    {
        m_flows = flows;
        m_received = received;
        std::sort(m_flows.begin(), m_flows.end(),
                  [](const AnalyticsFlowRow &a, const AnalyticsFlowRow &b) {
                      return a.transition_count > b.transition_count;
                  });
        m_hit_boxes.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent *ev) override
    {
        QFrame::paintEvent(ev);
        m_hit_boxes.clear();

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF area = rect().adjusted(22, 20, -22, -22);
        painter.setPen(ReportTk::textHi);
        painter.setFont(Theme::ui_font(13, 700, 0.10));
        painter.drawText(QPointF(area.left(), area.top()), "Zone-to-Zone Movement");

        painter.setPen(ReportTk::textSec);
        painter.setFont(Theme::mono_font(9));
        painter.drawText(QPointF(area.left(), area.top() + 22.0),
                         "line thickness = movement count");

        if (m_flows.isEmpty()) {
            painter.setPen(ReportTk::textFaint);
            painter.setFont(Theme::mono_font(11));
            painter.drawText(area.adjusted(0, 46, 0, 0),
                             Qt::AlignLeft | Qt::AlignTop,
                             m_received ? "No transition data"
                                        : "waiting for movement data");
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

        const double left_x = area.left() + 46.0;
        const double right_x = area.right() - 46.0;

        auto zone_y = [&](int zone_id) {
            const int index = std::max(0, static_cast<int>(zones.indexOf(zone_id)));
            const double usable = area.height() - 100.0;
            if (zones.size() <= 1)
                return area.center().y();
            return area.top() + 72.0
                + usable * static_cast<double>(index)
                    / static_cast<double>(zones.size() - 1);
        };

        painter.setPen(ReportTk::textFaint);
        painter.setFont(Theme::mono_font(9, 700));
        painter.drawText(QPointF(left_x - 24.0, area.top() + 46.0), "From");
        painter.drawText(QPointF(right_x - 12.0, area.top() + 46.0), "To");

        const int flow_count = std::min(static_cast<int>(m_flows.size()), 8);
        for (int i = flow_count - 1; i >= 0; --i) {
            const AnalyticsFlowRow &flow = m_flows[i];
            const double width = 2.0 + 16.0
                * static_cast<double>(flow.transition_count)
                / static_cast<double>(max_count);
            QColor color = zone_color(flow.from_zone_id);
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
            m_hit_boxes.append({ path, width + 12.0, flow });

            if (i < 3) {
                const double label_t = (i % 2 == 0) ? 0.36 : 0.64;
                const QPointF label_point = path.pointAtPercent(label_t);
                const double label_y_offset = (i % 2 == 0) ? -18.0 : 18.0;
                const QString label =
                    QString("%1 moves").arg(flow.transition_count);
                const QRectF label_rect(label_point.x() - 38.0,
                                        label_point.y() + label_y_offset - 11.0,
                                        76.0, 22.0);

                painter.setPen(Qt::NoPen);
                QColor chip = Theme::elevated;   // 글자는 textHi — 면도 팔레트에서
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
            draw_node(&painter, QPointF(left_x, y), zone_tag(zone_id));
            draw_node(&painter, QPointF(right_x, y), zone_tag(zone_id));
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF position = event->position();
        for (int i = m_hit_boxes.size() - 1; i >= 0; --i) {
            const HitBox &hit = m_hit_boxes[i];
            QPainterPathStroker stroker;
            stroker.setWidth(hit.width);
            stroker.setCapStyle(Qt::RoundCap);
            stroker.setJoinStyle(Qt::RoundJoin);
            if (!stroker.createStroke(hit.path).contains(position))
                continue;

            const AnalyticsFlowRow &row = hit.row;
            QToolTip::showText(
                event->globalPosition().toPoint(),
                QString("%1 -> %2\nTotal moves: %3\nConfirmed: %4\n"
                        "Estimated: %5\nAvg score: %6")
                    .arg(zone_tag(row.from_zone_id), zone_tag(row.to_zone_id))
                    .arg(row.transition_count)
                    .arg(row.confirmed_count)
                    .arg(row.estimated_count)
                    .arg(QString::number(row.avg_score, 'f', 2)),
                this);
            return;
        }
        QToolTip::hideText();
    }

private:
    void draw_node(QPainter *painter, const QPointF &center, const QString &label)
    {
        const QRectF r(center.x() - 28.0, center.y() - 16.0, 56.0, 32.0);
        painter->setPen(QPen(Theme::border2, 1));
        painter->setBrush(Theme::elevated);
        painter->drawRoundedRect(r, 7, 7);
        painter->setPen(Theme::textHi);
        painter->drawText(r, Qt::AlignCenter, label);
    }

    struct HitBox {
        QPainterPath path;
        double width = 0.0;
        AnalyticsFlowRow row;
    };

    QVector<AnalyticsFlowRow> m_flows;
    QVector<HitBox> m_hit_boxes;
    bool m_received = false;
};

// ============================================================ APathsCard
/**
 * @brief 이동 경로 순위 + 추적 품질 한 문장
 *
 * Flow 화면의 KPI 3장(신뢰 세그먼트·사용 비율·ID 연속성)은 추적기 자기
 * 얘기라 헤드라인 자격이 없다 — 여기 문장 하나로 접는다. 숫자는 그대로
 * 남아 있으므로 품질을 의심할 때 읽으면 된다.
 */
class APathsCard : public QFrame
{
public:
    explicit APathsCard(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName("ACard");
        setMinimumHeight(170);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(18, 14, 18, 14);
        root->setSpacing(10);

        auto *title = new QLabel("Most Common Movement Paths", this);
        title->setFont(Theme::ui_font(13, 700, 0.12));
        Theme::restyle(title, [] {
            return QString("color:%1;").arg(hex(ReportTk::textHi));
        });
        root->addWidget(title);

        m_rows_host = new QWidget(this);
        auto *grid = new QGridLayout(m_rows_host);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(18);
        grid->setVerticalSpacing(6);
        root->addWidget(m_rows_host, 1);

        auto *sep = new QFrame(this);
        sep->setFixedHeight(1);
        sep->setAttribute(Qt::WA_StyledBackground);
        Theme::restyle(sep, [] {
            return QString("background:%1;").arg(hex(ReportTk::gridLine));
        });
        root->addWidget(sep);

        m_summary = new QLabel(this);
        m_summary->setTextFormat(Qt::RichText);
        m_summary->setWordWrap(true);
        m_summary->setFont(Theme::mono_font(9));
        Theme::restyle(m_summary, [] {
            return QString("color:%1;").arg(hex(ReportTk::textSec));
        });
        root->addWidget(m_summary);
    }

    void set_data(const QVector<AnalyticsFlowRow> &flows,
                  int reliable, int total, double confidence,
                  bool dummy, bool received)
    {
        auto *grid = static_cast<QGridLayout *>(m_rows_host->layout());
        while (QLayoutItem *item = grid->takeAt(0)) {
            if (QWidget *w = item->widget())
                w->deleteLater();
            delete item;
        }

        QVector<AnalyticsFlowRow> sorted = flows;
        std::sort(sorted.begin(), sorted.end(),
                  [](const AnalyticsFlowRow &a, const AnalyticsFlowRow &b) {
                      return a.transition_count > b.transition_count;
                  });

        if (sorted.isEmpty()) {
            auto *empty = new QLabel(received ? "No movement paths yet"
                                              : "waiting for movement data",
                                     m_rows_host);
            empty->setFont(Theme::mono_font(10));
            Theme::restyle(empty, [] {
                return QString("color:%1;").arg(hex(ReportTk::textFaint));
            });
            grid->addWidget(empty, 0, 0);
        }

        const int max_rows = std::min(static_cast<int>(sorted.size()), 4);
        for (int i = 0; i < max_rows; ++i) {
            const AnalyticsFlowRow &row = sorted[i];
            auto *rank = new QLabel(QString("#%1").arg(i + 1), m_rows_host);
            auto *path = new QLabel(QString::fromUtf8("%1 → %2")
                                        .arg(zone_tag(row.from_zone_id),
                                             zone_tag(row.to_zone_id)),
                                    m_rows_host);
            auto *count = new QLabel(QString("%1   confirmed %2")
                                         .arg(row.transition_count)
                                         .arg(row.confirmed_count),
                                     m_rows_host);

            rank->setFont(Theme::mono_font(10, 800));
            path->setFont(Theme::mono_font(11, 700));
            count->setFont(Theme::mono_font(10));

            // 순위색 = 출발 구역색 — 옆 곡선과 같은 색으로 짝을 맞춘다
            const int from_zone = row.from_zone_id;
            Theme::restyle(rank, [from_zone] {
                return QString("color:%1;").arg(hex(zone_color(from_zone)));
            });
            Theme::restyle(path, [] {
                return QString("color:%1;").arg(hex(ReportTk::textHi));
            });
            Theme::restyle(count, [] {
                return QString("color:%1;").arg(hex(ReportTk::textSec));
            });

            grid->addWidget(rank, i, 0);
            grid->addWidget(path, i, 1);
            grid->addWidget(count, i, 2);
        }
        grid->setColumnStretch(2, 1);
        grid->setRowStretch(std::max(max_rows, 1), 1);

        // ── 추적 품질 한 문장 ──
        if (!received) {
            m_summary->setText("waiting for movement data");
            return;
        }
        const double ratio = total > 0
            ? static_cast<double>(reliable) * 100.0 / total : 0.0;
        const QLocale en(QLocale::English);
        m_summary->setText(
            QString::fromUtf8(
                "Based on <span style='color:%1;'>%2</span> of %3 tracked "
                "movements that were reliable enough to trust (%4%). Identity "
                "is held across cameras with <span style='color:%1;'>%5</span> "
                "confidence.%6")
                .arg(hex(ReportTk::textHi),
                     en.toString(reliable),
                     en.toString(total),
                     fmt1(ratio),
                     QString::number(confidence, 'f', 2),
                     dummy ? QString::fromUtf8(" · preview data") : QString()));
    }

private:
    QWidget *m_rows_host = nullptr;
    QLabel *m_summary = nullptr;
};

// ============================================================ AnalyticsPage

AnalyticsPage::AnalyticsPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("AnalyticsPage");
    setAttribute(Qt::WA_StyledBackground);

    // 페이지 한정 크롬 — ReportPage와 같은 토큰을 쓴다 (같은 데이터 계열의
    // 화면이 다른 색을 쓰면 비교가 아니라 딴 앱처럼 보인다)
    Theme::restyle(this, [] {
        return QString(R"(
            #AnalyticsPage { background:%1; }
            QScrollArea#AScroll { background:transparent; border:none; }
            #AScroll > QWidget > QWidget { background:transparent; }
            QFrame#ACard { background:%2; border:1px solid %3; border-radius:4px; }
            QFrame#ASegWrap { background:%2; border:1px solid %3; border-radius:5px; }
            QPushButton#ASeg {
                color:%4; background:transparent;
                border:1px solid transparent; border-radius:3px; padding:4px 12px;
            }
            QPushButton#ASeg:hover { color:%5; }
            QPushButton#ASeg:checked {
                color:%5; background:%6; border:1px solid %7;
            }
        )")
            .arg(hex(ReportTk::pageBg), hex(ReportTk::cardBg), hex(ReportTk::border),
                 hex(ReportTk::textSec), hex(ReportTk::textHi), hex(ReportTk::ctrlBg),
                 hex(ReportTk::ctrlBorder));
    });

    m_net = new QNetworkAccessManager(this);
    Credentials::install_tls_pinning(m_net);
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [](QNetworkReply *reply, QAuthenticator *auth) {
                if (auth->user() == Credentials::camera_user()) {
                    reply->abort();   // 비밀번호가 틀렸다 — 재시도 무한루프 방지
                    return;
                }
                auth->setUser(Credentials::camera_user());
                auth->setPassword(Credentials::camera_password());
            });

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(26, 24, 26, 24);
    root->setSpacing(20);
    root->addWidget(build_header());

    auto *content = new QWidget;
    auto *col = new QVBoxLayout(content);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(16);
    col->addWidget(build_kpi_row());
    col->addWidget(build_body(), 1);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("AScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    connect(ZoneConfig::notifier(), &ZoneConfig::Notifier::changed,
            this, &AnalyticsPage::retitle_channels);

    m_pred_timer = new QTimer(this);
    connect(m_pred_timer, &QTimer::timeout,
            this, &AnalyticsPage::refresh_predictions);
    m_pred_timer->start(PRED_REFRESH_MS);

    m_flow_timer = new QTimer(this);
    connect(m_flow_timer, &QTimer::timeout, this, &AnalyticsPage::request_flow);
    m_flow_timer->start(FLOW_REFRESH_MS);

    update_views();
    update_flow_views();

    // 테마 전환 — 수치 라벨은 HTML/스타일에 색을 구워 넣으므로 다시 만든다
    // (네트워크 없이 캐시된 값으로 다시 그린다)
    Theme::on_theme_changed(this, [this] {
        update_views();
        update_flow_views();
        paint_refresh_label();
    });
}

void AnalyticsPage::showEvent(QShowEvent *ev)
{
    QWidget::showEvent(ev);
    refresh_predictions();
    request_flow();
}

// ---------------------------------------------------------------- UI 구축

QWidget *AnalyticsPage::build_header()
{
    auto *w = new QWidget(this);
    auto *row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(16);

    auto *title = new QLabel("Analytics", w);
    title->setFont(Theme::ui_font(13, 700, 0.08));
    Theme::restyle(title, [=] {
        return QString("color:%1;").arg(hex(ReportTk::textHi));
    });
    row->addWidget(title);

    auto *sub = new QLabel(QString::fromUtf8(
        "how busy each zone will get · where people move between them"), w);
    sub->setFont(Theme::mono_font(10, 400, 1.0 / 10));
    Theme::restyle(sub, [=] {
        return QString("color:%1;").arg(hex(ReportTk::textSec));
    });
    row->addWidget(sub);

    m_insight = new QLabel(w);
    m_insight->setFont(Theme::mono_font(10, 700));
    // ⚠ **이 줄이 창의 최소 폭을 정하게 두면 안 된다** (mainwindow.cpp 의 같은
    //   경고 참조 — 페이지 최소가 작업영역보다 크면 창이 그 밑으로 못 줄고,
    //   그 폭이 다른 페이지에도 그대로 내려간다). 헤더는 스크롤 밖이라 이
    //   라벨의 자연 폭이 곧 페이지 최소 폭이 되는데, 실데이터가 오면 문구가
    //   ~100자로 늘어 1600px 기본 창을 넘긴다 (08-20 리뷰 확인).
    //   최소 1px 로 눌러 두면 좁을 때만 꼬리가 잘리고 창은 자유롭게 줄어든다.
    m_insight->setMinimumWidth(1);
    Theme::restyle(m_insight, [] {
        return QString("color:%1;").arg(hex(ReportTk::green));
    });
    row->addWidget(m_insight, 1);

    auto *seg_wrap = new QFrame(w);
    seg_wrap->setObjectName("ASegWrap");
    auto *seg_row = new QHBoxLayout(seg_wrap);
    seg_row->setContentsMargins(3, 3, 3, 3);
    seg_row->setSpacing(2);
    m_seg_group = new QButtonGroup(this);
    m_seg_group->setExclusive(true);
    const char *ranges[3] = { "60min", "6h", "24h" };
    for (int i = 0; i < 3; ++i) {
        auto *b = new QPushButton(QString::fromLatin1(ranges[i]), seg_wrap);
        b->setObjectName("ASeg");
        b->setFont(Theme::mono_font(11));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        if (i == 0)
            b->setChecked(true);
        m_seg_group->addButton(b, i);
        seg_row->addWidget(b);
    }
    connect(m_seg_group, &QButtonGroup::idClicked, this, [this](int id) {
        m_range = Range(id);
        update_views();
    });
    row->addWidget(seg_wrap);

    m_refresh_lb = new QLabel(w);
    m_refresh_lb->setFont(Theme::mono_font(10));
    m_refresh_lb->setTextFormat(Qt::RichText);
    m_refresh_lb->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(m_refresh_lb, &QLabel::linkActivated, this, [this] {
        refresh_predictions();
        request_flow();
    });
    row->addWidget(m_refresh_lb);

    return w;
}

QWidget *AnalyticsPage::build_kpi_row()
{
    auto *w = new QWidget;
    auto *row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(14);

    m_kpi_now = new AKpiCard("Busiest right now", w);
    m_kpi_next = new AKpiCard("Busiest in the next 3 hours", w);
    m_kpi_dwell = new AKpiCard("People stay longest in", w);
    m_kpi_move = new AKpiCard("Most common move", w);

    row->addWidget(m_kpi_now, 1);
    row->addWidget(m_kpi_next, 1);
    row->addWidget(m_kpi_dwell, 1);
    row->addWidget(m_kpi_move, 1);
    return w;
}

QWidget *AnalyticsPage::build_body()
{
    auto *w = new QWidget;
    auto *row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(16);

    // ── 좌: 구역 카드 2×2 + 읽는 법 ──
    auto *left = new QVBoxLayout;
    left->setSpacing(10);
    auto *grid_host = new QWidget(w);
    auto *grid = new QGridLayout(grid_host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(16);
    for (int ch = 0; ch < 4; ++ch) {
        m_cards[ch] = new AZoneCard(ch, grid_host);
        grid->addWidget(m_cards[ch], ch / 2, ch % 2);
    }
    left->addWidget(grid_host, 1);

    auto *legend = new QLabel(QString::fromUtf8(
        "How to read these charts:   the solid line is how many people we "
        "actually counted   -   after \"now\" the dashed line is our best guess "
        "for what comes next   -   the shaded band shows how far off that guess "
        "could be   -   CAP is the most people the zone should hold   -   "
        "a thicker movement line means more people walked that way"), w);
    legend->setFont(Theme::mono_font(9.5));
    legend->setWordWrap(true);
    Theme::restyle(legend, [] {
        return QString("color:%1;").arg(hex(ReportTk::textFaint));
    });
    left->addWidget(legend);
    row->addLayout(left, 1);

    // ── 우: 동선 열 (체류 · 이동 곡선 · 경로 순위) ──
    auto *right_host = new QWidget(w);
    right_host->setFixedWidth(400);
    auto *right = new QVBoxLayout(right_host);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(14);
    m_dwell_card = new ADwellCard(right_host);
    right->addWidget(m_dwell_card);
    m_sankey_card = new ASankeyCard(right_host);
    right->addWidget(m_sankey_card, 1);
    m_paths_card = new APathsCard(right_host);
    right->addWidget(m_paths_card);
    row->addWidget(right_host);

    return w;
}

// ---------------------------------------------------------------- 데이터

void AnalyticsPage::retitle_channels()
{
    for (int ch = 0; ch < 4; ++ch)
        if (m_cards[ch])
            m_cards[ch]->retitle(ch);
    update_kpis();
    update_insight();
}

void AnalyticsPage::refresh_predictions()
{
    for (int ch = 0; ch < 4; ++ch) {
        request_camera(ch, true);
        request_camera(ch, false);
    }
    request_series(360, 5, "6h");
    request_series(1440, 15, "24h");

    m_refresh_at = QTime::currentTime().toString("HH:mm:ss");
    paint_refresh_label();
}

void AnalyticsPage::paint_refresh_label()
{
    // 색이 RichText 에 구워지므로 테마가 바뀌면 다시 써야 한다 — 그래서
    // 시각(m_refresh_at)만 들고 있고 문자열은 그릴 때 만든다.
    if (!m_refresh_lb)
        return;
    m_refresh_lb->setText(
        QString("<span style='color:%1;'>%2 · </span>"
                "<a href='refresh' style='color:%3;text-decoration:none;'>Refresh</a>")
            .arg(hex(ReportTk::textSec),
                 m_refresh_at,
                 hex(ReportTk::blueLight)));
}

void AnalyticsPage::request_camera(int ch, bool pred)
{
    QUrl url = Credentials::camera_base_url();
    url.setPath(pred ? PRED_PATH : OCC_PATH);
    url.setQuery(pred ? QString("channel=%1&capacity=%2")
                            .arg(ch).arg(ZoneConfig::capacity(ch))
                      : QString("channel=%1").arg(ch));

    QNetworkReply *reply = m_net->get(sunapi_request(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, ch, pred] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;                       // fail-soft: 직전 값 유지
        const QJsonObject o =
            QJsonDocument::fromJson(reply->readAll()).object();
        ReportPage::ChanData &d = m_ch[ch];

        if (pred) {
            const QJsonObject model = o["model"].toObject();
            d.model = model["version"].toString(DASH);
            d.mae = model.contains("rolling_mae_1step")
                        ? model["rolling_mae_1step"].toDouble() : -1;
            d.profile_days = model.contains("profile_coverage_days")
                                 ? model["profile_coverage_days"].toDouble() : -1;
            d.obs = model["observations"].toInt();
            d.warmup = model["warmup"].toBool(true);

            d.fc.clear();
            for (const QJsonValue &v : o["predictions"].toArray()) {
                const QJsonObject f = v.toObject();
                ReportPage::FcRow r;
                r.minutes = f["horizon_min"].toInt();
                r.p50 = f.contains("p50") ? f["p50"].toDouble() : -1;
                r.p10 = f.contains("p10") ? f["p10"].toDouble() : -1;
                r.p90 = f.contains("p90") ? f["p90"].toDouble() : -1;
                r.pov = f.contains("p_over_capacity")
                            ? f["p_over_capacity"].toDouble() : -1;
                d.fc.append(r);
            }
        } else {
            d.cur = o["now_smoothed"].toInt();
            d.h60.clear();
            for (const QJsonValue &v : o["series_1min"].toArray())
                d.h60.append(v.toDouble());
        }
        update_views();   // 소스가 도착하는 대로 점진 갱신
    });
}

void AnalyticsPage::request_series(int minutes, int bucket_min, const QString &tag)
{
    QJsonObject params;
    params["query"] = "occseries";
    params["minutes"] = minutes;
    params["bucket_min"] = bucket_min;

    MqttLink::instance()->request(
        OCCSERIES_TOPIC, params,
        [this, tag](const QJsonObject &reply) { apply_series(reply, tag); },
        [tag](const QString &reason) {
            qWarning() << "[AnalyticsPage] occseries" << tag << "실패:" << reason;
        });
}

void AnalyticsPage::apply_series(const QJsonObject &o, const QString &tag)
{
    const int n_slots = (tag == "6h") ? SLOTS_6H : SLOTS_24H;
    QVector<double> series[4];
    for (int ch = 0; ch < 4; ++ch)
        series[ch] = QVector<double>(n_slots, 0.0);

    for (const QJsonValue &v : o["points"].toArray()) {
        const QJsonArray p = v.toArray();
        const int ch = p.at(0).toInt(-1);
        const int idx = n_slots - 1 - p.at(1).toInt(-1);
        if (ch >= 0 && ch < 4 && idx >= 0 && idx < n_slots)
            series[ch][idx] = p.at(2).toDouble();
    }

    for (int ch = 0; ch < 4; ++ch) {
        if (tag == "6h")
            m_ch[ch].h6h = series[ch];
        else
            m_ch[ch].h24h = series[ch];
    }
    update_views();
}

void AnalyticsPage::request_flow()
{
    if (!m_flow_request_id.isEmpty())
        return;   // in-flight 가드 (BusinessFlowPage와 동일)

    QJsonObject params;
    params["query"] = "trajectory";
    params["limit"] = 500;

    m_flow_request_id = MqttLink::instance()->request(
        TRAJECTORY_REQ_TOPIC, params,
        [this](const QJsonObject &reply) {
            m_flow_request_id.clear();
            apply_flow(reply);
        },
        [this](const QString &reason) {
            m_flow_request_id.clear();
            if (USE_DUMMY_FALLBACK) {
                qWarning() << "[AnalyticsPage] trajectory 실패 — 더미 사용:"
                           << reason;
                apply_flow(make_dummy_flow());
            }
        },
        MqttLink::DEFAULT_TIMEOUT_MS);

    if (m_flow_request_id.isEmpty() && USE_DUMMY_FALLBACK && !m_flow_received)
        apply_flow(make_dummy_flow());
}

void AnalyticsPage::apply_flow(const QJsonObject &reply)
{
    if (!reply.value("ok").toBool(true))
        return;   // fail-soft: 직전 값 유지

    const QJsonObject coverage = reply.value("coverage").toObject();
    m_cov_reliable = coverage.value("reliable_count").toInt(
        reply.value("reliable_count").toInt(0));
    m_cov_total = coverage.value("total_count").toInt(
        reply.value("total_count").toInt(0));
    m_cov_confidence = coverage.value("avg_confidence").toDouble(
        reply.value("avg_confidence").toDouble(0.0));
    m_cov_dummy = reply.value("dummy").toBool(false);

    m_dwell.clear();
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
        m_dwell.append(row);
    }

    m_flows.clear();
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
        m_flows.append(row);
    }
    std::sort(m_flows.begin(), m_flows.end(),
              [](const AnalyticsFlowRow &a, const AnalyticsFlowRow &b) {
                  return a.transition_count > b.transition_count;
              });

    m_flow_received = true;
    update_flow_views();
}

QJsonObject AnalyticsPage::make_dummy_flow() const
{
    // business_flow_page.cpp make_dummy_analytics()와 같은 값 — 두 탭이 같은
    // 폴백 데이터를 보여야 "탭마다 숫자가 다르다"는 오해가 없다
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

// ---------------------------------------------------------------- 표시 갱신

const QVector<double> &AnalyticsPage::hist_for(int ch) const
{
    switch (m_range) {
    case R6H:  return m_ch[ch].h6h;
    case R24H: return m_ch[ch].h24h;
    default:   return m_ch[ch].h60;
    }
}

QString AnalyticsPage::range_label() const
{
    switch (m_range) {
    case R6H:  return "-6h";
    case R24H: return "-24h";
    default:   return "-60m";
    }
}

QVector<ReportChart::FcPt> AnalyticsPage::chart_fc(int ch) const
{
    QVector<ReportChart::FcPt> out;
    if (m_ch[ch].warmup)
        return out;   // 학습 전 예측은 그리지 않는다 (표도 '—')
    for (const ReportPage::FcRow &f : m_ch[ch].fc)
        if (f.p50 >= 0)
            out.append({ f.minutes, f.p50, f.p10, f.p90 });
    return out;
}

void AnalyticsPage::update_views()
{
    const QString rl = range_label();
    for (int ch = 0; ch < 4; ++ch) {
        const ReportPage::ChanData &d = m_ch[ch];
        const int cap = ZoneConfig::capacity(ch);
        const bool near =
            cap > 0 && double(d.cur) / cap > ZoneConfig::warn_ratio(ch);
        m_cards[ch]->set_data(d, cap, near, hist_for(ch), chart_fc(ch), rl);
    }
    update_kpis();
    update_insight();
}

void AnalyticsPage::update_flow_views()
{
    m_dwell_card->set_rows(m_dwell, m_flow_received);
    m_sankey_card->set_flows(m_flows, m_flow_received);
    m_paths_card->set_data(m_flows, m_cov_reliable, m_cov_total,
                           m_cov_confidence, m_cov_dummy, m_flow_received);

    // 카드 하단 동선 줄 — 이 구역으로 드나든 이동(각 방향 상위 2개)과 체류
    for (int ch = 0; ch < 4; ++ch) {
        const int z = ZoneConfig::zone_id(ch);

        // 방향별 상위 2개 — 두 번째부터는 "Z3 18"로 줄인다 (한 줄에 다
        // 펼치면 카드 최소 폭이 밀려 페이지에 가로 스크롤이 생긴다)
        QStringList in, out;
        for (const AnalyticsFlowRow &f : m_flows) {   // 이미 count 내림차순
            if (f.to_zone_id == z && in.size() < 2)
                in << (in.isEmpty()
                           ? QString::fromUtf8("%1 → here %2")
                                 .arg(zone_tag(f.from_zone_id))
                                 .arg(f.transition_count)
                           : QString("%1 %2")
                                 .arg(zone_tag(f.from_zone_id))
                                 .arg(f.transition_count));
            if (f.from_zone_id == z && out.size() < 2)
                out << (out.isEmpty()
                            ? QString::fromUtf8("here → %1 %2")
                                  .arg(zone_tag(f.to_zone_id))
                                  .arg(f.transition_count)
                            : QString("%1 %2")
                                  .arg(zone_tag(f.to_zone_id))
                                  .arg(f.transition_count));
        }

        QString stay = m_flow_received ? QString("stay time unknown") : DASH;
        for (const AnalyticsDwellRow &d : m_dwell)
            if (d.zone_id == z) {
                stay = QString("stays %1").arg(seconds_text(d.avg_dwell_ms));
                break;
            }

        m_cards[ch]->set_flow(
            in.isEmpty() ? (m_flow_received ? QString("no arrivals yet") : DASH)
                         : in.join(QString::fromUtf8("  ·  ")),
            out.isEmpty() ? (m_flow_received ? QString("no onward moves yet") : DASH)
                          : out.join(QString::fromUtf8("  ·  ")),
            stay);
    }

    update_kpis();
    update_insight();
}

void AnalyticsPage::update_kpis()
{
    // ── 1. 지금 가장 붐비는 구역 ──
    int busiest = 0;
    double busiest_load = -1;
    for (int ch = 0; ch < 4; ++ch) {
        const int cap = ZoneConfig::capacity(ch);
        const double load = cap > 0 ? double(m_ch[ch].cur) / cap : 0;
        if (load > busiest_load) {
            busiest_load = load;
            busiest = ch;
        }
    }
    {
        const int cap = ZoneConfig::capacity(busiest);
        const int cur = m_ch[busiest].cur;
        const int free = std::max(0, cap - cur);
        const QStringList nm = split_channel_name(busiest);
        const KpiTone tone =
            busiest_load >= ZoneConfig::critical_ratio(busiest) ? KpiTone::Bad
            : busiest_load >= ZoneConfig::warn_ratio(busiest)   ? KpiTone::Warn
                                                                : KpiTone::Good;
        m_kpi_now->set_value(
            QString("%1 / %2").arg(cur).arg(cap),
            QString::fromUtf8("%1 · %2 · %3 place%4 still free")
                .arg(zone_tag(ZoneConfig::zone_id(busiest)),
                     nm[1].isEmpty() ? nm[0] : nm[1])
                .arg(free)
                .arg(free == 1 ? "" : "s"),
            tone);
    }

    // ── 2. 앞으로 3시간 안에 가장 붐비는 구역 ──
    {
        int peak_ch = -1, peak_min = 0;
        double peak_p50 = -1, peak_pov = -1;
        for (int ch = 0; ch < 4; ++ch) {
            if (m_ch[ch].warmup)
                continue;
            for (const ReportPage::FcRow &f : m_ch[ch].fc) {
                if (f.p50 > peak_p50) {
                    peak_p50 = f.p50;
                    peak_pov = f.pov;
                    peak_min = f.minutes;
                    peak_ch = ch;
                }
            }
        }
        if (peak_ch < 0) {
            m_kpi_next->set_value(DASH, "models still learning", KpiTone::Info);
        } else {
            const int cap = ZoneConfig::capacity(peak_ch);
            const QStringList nm = split_channel_name(peak_ch);
            // -1 = 불명 → '—' 규칙(07-30): 모르는 확률을 0%로 말하지 않는다
            const QString risk = peak_pov < 0
                ? QString::fromUtf8("risk unknown")
                : QString::fromUtf8("%1% chance of going over %2")
                      .arg(qRound(peak_pov * 100)).arg(cap);
            const KpiTone tone = peak_pov >= 0.5 ? KpiTone::Bad
                               : peak_pov >= 0.1 ? KpiTone::Warn
                               : peak_pov >= 0   ? KpiTone::Good
                                                 : KpiTone::Info;
            m_kpi_next->set_value(
                fmt1(peak_p50),
                QString::fromUtf8("%1 · %2 at +%3m · %4")
                    .arg(zone_tag(ZoneConfig::zone_id(peak_ch)),
                         nm[1].isEmpty() ? nm[0] : nm[1])
                    .arg(peak_min)
                    .arg(risk),
                tone);
        }
    }

    // ── 3. 가장 오래 머무는 구역 ──
    {
        const AnalyticsDwellRow *top = nullptr;
        for (const AnalyticsDwellRow &row : m_dwell)
            if (!top || row.avg_dwell_ms > top->avg_dwell_ms)
                top = &row;
        if (!top) {
            m_kpi_dwell->set_value(DASH, "waiting for movement data",
                                   KpiTone::Info);
        } else {
            const int ch = ZoneConfig::zone_channel(top->zone_id);
            const QString name = ch >= 0 ? split_channel_name(ch).value(1)
                                         : QString();
            m_kpi_dwell->set_value(
                seconds_text(top->avg_dwell_ms),
                QString::fromUtf8("%1%2 · %3 visits")
                    .arg(zone_tag(top->zone_id),
                         name.isEmpty() ? QString()
                                        : QString::fromUtf8(" · %1").arg(name))
                    .arg(top->visit_count),
                KpiTone::Info);
        }
    }

    // ── 4. 가장 흔한 이동 ──
    {
        if (m_flows.isEmpty()) {
            m_kpi_move->set_value(DASH, "waiting for movement data",
                                  KpiTone::Info);
        } else {
            const AnalyticsFlowRow &top = m_flows.first();   // count 내림차순
            int total = 0;
            for (const AnalyticsFlowRow &f : m_flows)
                total += f.transition_count;
            const QString share = total > 0
                ? QString::fromUtf8(" · %1% of everything we tracked")
                      .arg(qRound(100.0 * top.transition_count / total))
                : QString();
            m_kpi_move->set_value(
                QString::fromUtf8("%1 → %2")
                    .arg(zone_tag(top.from_zone_id), zone_tag(top.to_zone_id)),
                QString::fromUtf8("%1 moves%2")
                    .arg(top.transition_count)
                    .arg(share),
                KpiTone::Info);
        }
    }
}

void AnalyticsPage::update_insight()
{
    // 헤더의 초록 한 줄 — "어디를 봐야 하나"의 답. 지금 가장 붐비는 구역을
    // 뼈대로, 체류·동선까지 같은 구역을 가리키면 그 근거를 이어 붙인다.
    int busiest = -1;
    double busiest_load = 0;
    for (int ch = 0; ch < 4; ++ch) {
        const int cap = ZoneConfig::capacity(ch);
        const double load = cap > 0 ? double(m_ch[ch].cur) / cap : 0;
        if (m_ch[ch].cur > 0 && load > busiest_load) {
            busiest_load = load;
            busiest = ch;
        }
    }
    if (busiest < 0) {
        m_insight->setText("Waiting for occupancy data");
        m_insight->setToolTip(QString());
        return;
    }

    const int z = ZoneConfig::zone_id(busiest);
    QStringList why("busiest now");

    const AnalyticsDwellRow *top_dwell = nullptr;
    for (const AnalyticsDwellRow &row : m_dwell)
        if (!top_dwell || row.avg_dwell_ms > top_dwell->avg_dwell_ms)
            top_dwell = &row;
    if (top_dwell && top_dwell->zone_id == z)
        why << "longest stays";

    if (!m_flows.isEmpty() && m_flows.first().to_zone_id == z)
        why << "the main path ends there";

    QString reasons = why.first();
    for (int i = 1; i < why.size(); ++i)
        reasons += (i == why.size() - 1 ? QString::fromUtf8(", and ")
                                        : QString::fromUtf8(", "))
                   + why[i];

    const QStringList nm = split_channel_name(busiest);
    const QString text =
        QString::fromUtf8("%1 · %2 is the one to watch — %3")
            .arg(zone_tag(z), nm[1].isEmpty() ? nm[0] : nm[1], reasons);
    m_insight->setText(text);
    m_insight->setToolTip(text);   // 좁은 창에서 잘린 꼬리를 여기서 읽는다
}
