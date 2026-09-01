#include "report_page.h"
#include "alert_feed.h"
#include "credentials.h"
#include "mqtt_link.h"
#include "site_config.h"
#include "sunapi_request.h"
#include "theme.h"
#include "zone_config.h"

#include <QAuthenticator>
#include <QButtonGroup>
#include <QDateTime>
#include <QDebug>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace {

const int REFRESH_MS = 60 * 1000;   // 모델·점유가 1분 해상도 — 그보다 잦으면 낭비

const QString PRED_PATH = "/opensdk/juan_application/prediction";
const QString OCC_PATH = "/opensdk/juan_application/occupancy";

// 6h/24h 이력 — RPi B zone_occupancy 집계 (heatday와 같은 요청-응답 규약)
const QString OCCSERIES_TOPIC = "guardx/db/rpib/query/occseries";

// 이력 해상도 (핸드오프 §Chart specification — 60min은 카메라 series_1min을
// 그대로 쓰므로 60점. 6h/24h는 버킷 크기가 점 수를 결정한다)
const int SLOTS_6H = 360 / 5;     // 5분 버킷 72점
const int SLOTS_24H = 1440 / 15;  // 15분 버킷 96점

// 예측 유발 경보 표시 상한 — 리포트는 훑어보는 화면이다. 전수는 ALERT 화면.
const int PRED_ROWS_MAX = 12;

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

/** @brief COMPARE 칩의 8×8 radius-2 색 견본 (미선택 = 30% 불투명) */
QIcon chip_icon(const QColor &color, bool selected)
{
    QPixmap pm(8, 8);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QColor c = color;
    if (!selected)
        c.setAlpha(76);
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(0, 0, 8, 8), 2, 2);
    return QIcon(pm);
}

/** @brief NEAR CAP / HEADROOM 칩 — 색은 ReportTk 토큰에서 (테마 따라감) */
QString pill_style(bool near)
{
    // 08-19 보드: 칩은 테두리도 라운드도 없는 **채움 15%** 배지다
    // (18px, 9/700, 자간 1px, 대문자). 테두리를 두르면 버튼처럼 보여
    // 누를 수 있는 것으로 읽힌다 — 이건 상태 표시다.
    const QColor c = near ? ReportTk::amber : ReportTk::green;
    return QString("color:%1; background:rgba(%2,%3,%4,15%);"
                   "border:none; border-radius:0px; padding:2px 8px;")
        .arg(c.name())
        .arg(c.red()).arg(c.green()).arg(c.blue());
}

} // namespace

// ============================================================ ForecastTable
/**
 * @brief 4열 예측 표 (HZN | p50 | p10–p90 | P(>cap)) — 카드/미니카드 공용
 *
 * p_over_capacity == -1 은 "불명" → '—' (0%로 표기 금지 — 07-30 규칙 유지).
 * 색 편차: 핸드오프는 P(초과)를 항상 초록으로 그리지만, 50%가 초록이면
 * 오독을 부른다 — >50% alarm · >20% amber (구 구현의 안전 규칙 유지).
 */
class ForecastTable : public QWidget
{
public:
    explicit ForecastTable(bool fixed_width, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        if (fixed_width)
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
        // 08-19: 통계 약어를 평범한 말로 바꿨다. Hzn/p50/p10–p90/P(>cap) 은
        // 이 표를 만든 사람에게만 읽히는 표기다 — 화면은 처음 보는 사람이
        // 읽을 수 있어야 한다. 뜻은 그대로다(중앙값 = 가장 그럴듯한 값,
        // 10~90 백분위 = 낮게~높게 잡은 범위).
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
        // "P(>8)" -> "Full?" — 정원을 넘길 확률이라는 뜻을 열 이름이 직접 말한다
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

// ============================================================ ChannelCard
/** @brief 그리드 카드 1장 — 헤더(이름·메타·현재/정원·pill) + 차트 + 예측 표 */
class ChannelCard : public QFrame
{
public:
    explicit ChannelCard(int ch, QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName("RCard");

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
        const QStringList nm = split_channel_name(ch);
        m_id = new QLabel(nm[0], this);
        m_id->setFont(Theme::mono_font(12, 600));
        Theme::restyle(m_id, [=] {
            return QString("color:%1;").arg(hex(ReportTk::blue));
        });
        m_name = new QLabel(nm[1], this);
        m_name->setFont(Theme::ui_font(13, 700, 1.0 / 13));
        Theme::restyle(m_name, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textHi));
        });
        name_row->addWidget(m_id, 0, Qt::AlignBottom);
        name_row->addWidget(m_name, 0, Qt::AlignBottom);
        name_row->addStretch(1);
        left->addLayout(name_row);

        m_meta = new QLabel(DASH, this);
        m_meta->setFont(Theme::mono_font(9));
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
        m_table = new ForecastTable(true, this);
        body->addWidget(m_table, 0, Qt::AlignVCenter);
        root->addLayout(body);
    }

    void set_data(const ReportPage::ChanData &d, int cap, bool near,
                  const QVector<double> &hist,
                  const QVector<ReportChart::FcPt> &fc,
                  const QString &range_label)
    {
        // 08-19: 모델 내부 지표를 평범한 말로. "MAE 0.412" 는 이 숫자가 뭔지
        // 아는 사람에게만 뜻이 있다 — 화면에는 "얼마나 잘 맞았나"와 "무엇을
        // 보고 배웠나"만 남기고, 원래 지표는 툴팁으로 옮겼다.
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

        // 칩도 평범한 말로 — "NEAR CAP / HEADROOM" 은 업계 용어다
        m_pill->setText(near ? "GETTING FULL" : "PLENTY OF ROOM");
        m_pill->setStyleSheet(pill_style(near));

        m_chart->set_single({ ReportTk::blue, hist, fc }, cap, range_label);
        m_table->set_data(d.fc, d.warmup, cap);
    }

    /** @brief 구역 이름이 DB에서 바뀌었을 때 (ZoneConfig::Notifier) */
    void retitle(int ch)
    {
        const QStringList nm = split_channel_name(ch);
        m_id->setText(nm[0]);
        m_name->setText(nm[1]);
    }

private:
    QLabel *m_id = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_meta = nullptr;
    QLabel *m_cur = nullptr;
    QLabel *m_pill = nullptr;
    ReportChart *m_chart = nullptr;
    ForecastTable *m_table = nullptr;
};

// ============================================================ MiniCard
/** @brief COMPARE 하단 미니 카드 — 채널색 상단 보더 + 헤더 + 예측 표 */
class MiniCard : public QFrame
{
public:
    explicit MiniCard(int ch, QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName("RMini");
        Theme::restyle(this, [ch] {
            return QString("#RMini { background:%1; border:1px solid %2;"
                           "border-radius:6px; border-top:2px solid %3; }")
                .arg(hex(ReportTk::cardBg), hex(ReportTk::border),
                     hex(ReportTk::channel_color(ch)));
        });

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(16, 14, 16, 14);
        root->setSpacing(10);

        auto *head = new QHBoxLayout;
        head->setSpacing(8);
        const QStringList nm = split_channel_name(ch);
        m_id = new QLabel(nm[0], this);
        m_id->setFont(Theme::mono_font(11, 600));
        Theme::restyle(m_id, [ch] {
            return QString("color:%1;").arg(hex(ReportTk::channel_color(ch)));
        });
        m_name = new QLabel(nm[1], this);
        m_name->setFont(Theme::ui_font(12, 700, 1.0 / 12));
        Theme::restyle(m_name, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textHi));
        });
        head->addWidget(m_id, 0, Qt::AlignBottom);
        head->addWidget(m_name, 0, Qt::AlignBottom);
        head->addStretch(1);
        m_cur = new QLabel(this);
        m_cur->setFont(Theme::mono_font(11));
        Theme::restyle(m_cur, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textSec));
        });
        head->addWidget(m_cur, 0, Qt::AlignBottom);
        root->addLayout(head);

        m_table = new ForecastTable(false, this);
        root->addWidget(m_table);
    }

    void set_data(const ReportPage::ChanData &d, int cap)
    {
        m_cur->setText(QString("%1 / %2").arg(d.cur).arg(cap));
        m_table->set_data(d.fc, d.warmup, cap);
    }

    /** @brief 구역 이름이 DB에서 바뀌었을 때 (ZoneConfig::Notifier) */
    void retitle(int ch)
    {
        const QStringList nm = split_channel_name(ch);
        m_id->setText(nm[0]);
        m_name->setText(nm[1]);
    }

private:
    QLabel *m_id = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_cur = nullptr;
    ForecastTable *m_table = nullptr;
};

// ============================================================ ReportPage

ReportPage::ReportPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("ReportPage");
    setAttribute(Qt::WA_StyledBackground);

    // 페이지 한정 크롬 — 색은 ReportTk 토큰과 동일 값 (핸드오프 §Design Tokens)
    Theme::restyle(this, [] {
        return QString(R"(
            #ReportPage { background:%1; }
            QScrollArea#RScroll { background:transparent; border:none; }
            #RScroll > QWidget > QWidget { background:transparent; }
            QFrame#RCard { background:%2; border:1px solid %3; border-radius:4px; }
            QFrame#RSegWrap { background:%2; border:1px solid %3; border-radius:5px; }
            QPushButton#RSeg, QPushButton#RChip {
                color:%4; background:transparent;
                border:1px solid transparent; border-radius:3px; padding:4px 12px;
            }
            QPushButton#RCompare {
                color:%4; background:transparent;
                border:1px solid %3; border-radius:3px; padding:4px 12px;
            }
            QPushButton#RSeg:hover, QPushButton#RChip:hover,
            QPushButton#RCompare:hover { color:%5; }
            QPushButton#RSeg:checked, QPushButton#RChip:checked,
            QPushButton#RCompare:checked {
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
    root->setContentsMargins(26, 24, 26, 24);   // 4a §Geometry
    root->setSpacing(20);
    root->addWidget(build_header());

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(build_grid_view());
    m_stack->addWidget(build_compare_view());

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("RScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_stack);
    root->addWidget(scroll, 1);

    // 구역 이름은 DB가 원천이라 실행 중에 바뀔 수 있다. 카드 제목과 COMPARE
    // 칩은 생성 시 한 번 쓰인 라벨이므로 알림을 받아 다시 써준다.
    connect(ZoneConfig::notifier(), &ZoneConfig::Notifier::changed,
            this, &ReportPage::retitle_channels);

    // 경보 이력은 AlertFeed가 단일 창구다 — 이 화면은 표시만 한다
    connect(AlertFeed::instance(), &AlertFeed::history_changed,
            this, &ReportPage::update_predicted);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &ReportPage::refresh_all);
    m_timer->start(REFRESH_MS);

    update_views();
    update_predicted();

    // 테마 전환 — 카드 수치·라벨은 HTML에 색을 구워 넣으므로 다시 만든다
    // (네트워크 없이 캐시된 값으로 다시 그린다)
    Theme::on_theme_changed(this, [this] {
        update_views();
        update_predicted();
    });
}

void ReportPage::showEvent(QShowEvent *ev)
{
    QWidget::showEvent(ev);
    refresh_all();
}

// ---------------------------------------------------------------- UI 구축

QWidget *ReportPage::build_header()
{
    auto *w = new QWidget(this);
    auto *row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(16);

    // 08-19 워크스페이스 (디자인 보드): 제목은 13px 볼드 + 부제를 **옆에**
    // 나란히 — 두 줄짜리 21px 제목 블록은 워크스페이스 밀도와 안 맞는다.
    // 08-19: 탭 이름을 Predictions 로 바꿨다 — 이 화면이 실제로 하는 일은
    // "지난 기록 보고서"가 아니라 "앞으로 얼마나 붐빌지"다.
    auto *title = new QLabel("Predictions", w);
    title->setFont(Theme::ui_font(13, 700, 0.08));
    Theme::restyle(title, [=] {
        return QString("color:%1;").arg(hex(ReportTk::textHi));
    });
    row->addWidget(title);
    // 현장 문구는 전역 설정에서 온다 (08-12, 7번) — 상단바·로그인 카드와 같은 값
    auto *sub = new QLabel(w);
    sub->setFont(Theme::mono_font(10, 400, 1.0 / 10));
    const auto paint_sub = [sub] {
        sub->setText(SiteConfig::instance()->site_name()
                     + QString::fromUtf8(" · 4 Channels"));
    };
    paint_sub();
    connect(SiteConfig::instance(), &SiteConfig::site_name_changed, sub, paint_sub);
    Theme::restyle(sub, [=] {
        return QString("color:%1;").arg(hex(ReportTk::textSec));
    });
    row->addWidget(sub);
    row->addStretch(1);

    m_btn_compare = new QPushButton("Compare: Off", w);
    m_btn_compare->setObjectName("RCompare");
    m_btn_compare->setFont(Theme::mono_font(11));
    m_btn_compare->setCheckable(true);
    m_btn_compare->setCursor(Qt::PointingHandCursor);
    connect(m_btn_compare, &QPushButton::toggled, this, [this](bool on) {
        m_compare = on;
        m_btn_compare->setText(on ? "Compare: On" : "Compare: Off");
        m_stack->setCurrentIndex(on ? 1 : 0);
        update_views();
    });
    row->addWidget(m_btn_compare);

    auto *seg_wrap = new QFrame(w);
    seg_wrap->setObjectName("RSegWrap");
    auto *seg_row = new QHBoxLayout(seg_wrap);
    seg_row->setContentsMargins(3, 3, 3, 3);
    seg_row->setSpacing(2);
    m_seg_group = new QButtonGroup(this);
    m_seg_group->setExclusive(true);
    const char *ranges[3] = { "60min", "6h", "24h" };
    for (int i = 0; i < 3; ++i) {
        auto *b = new QPushButton(QString::fromLatin1(ranges[i]), seg_wrap);
        b->setObjectName("RSeg");
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
    connect(m_refresh_lb, &QLabel::linkActivated,
            this, [this] { refresh_all(); });
    row->addWidget(m_refresh_lb);

    return w;
}

QWidget *ReportPage::build_grid_view()
{
    // 08-19 워크스페이스: 예측 경보 패널은 카드 아래가 아니라 **우측 열**이다
    // (디자인 "Post-Analysis Reports" 보드) — 카드와 경보 기록을 나란히 본다.
    auto *w = new QWidget;
    auto *row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(16);

    auto *grid_host = new QWidget(w);
    auto *grid = new QGridLayout(grid_host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(16);
    for (int ch = 0; ch < 4; ++ch) {
        m_cards[ch] = new ChannelCard(ch, grid_host);
        grid->addWidget(m_cards[ch], ch / 2, ch % 2);
    }
    // 카드 아래 범례 — 차트 기호(실선/점선/밴드/CAP)를 화면이 직접 설명한다
    // (08-19 가독성 개선: 기호를 아는 사람만 읽을 수 있으면 안 된다)
    auto *left_col = new QVBoxLayout;
    left_col->setSpacing(8);
    left_col->addWidget(grid_host, 1);
    auto *legend = new QLabel(QString::fromUtf8(
        "How to read these charts:   the solid line is how many people we "
        "actually counted   -   after \"now\" the dashed line is our best guess "
        "for what comes next   -   the shaded band shows how far off that guess "
        "could be   -   CAP is the most people the zone should hold"), w);
    legend->setFont(Theme::mono_font(9.5));
    Theme::restyle(legend, [] {
        return QString("color:%1;").arg(hex(ReportTk::textFaint));
    });
    left_col->addWidget(legend);
    row->addLayout(left_col, 1);

    auto *pred = build_predicted_panel();
    pred->setFixedWidth(380);
    row->addWidget(pred);
    return w;
}

/**
 * @brief 예측이 유발한 혼잡 경보 패널
 *
 * 위 카드/차트가 "지금과 앞으로의 숫자"라면, 여기는 **그 예측이 실제로 경보를
 * 울린 기록**이다. 원천은 AlertFeed(RPi B의 incidents/alerts) — 모델 출력이
 * 운영 판단으로 이어진 지점만 모아 보여준다.
 */
QWidget *ReportPage::build_predicted_panel()
{
    auto *card = new QFrame;
    card->setObjectName("RCard");

    auto *col = new QVBoxLayout(card);
    col->setContentsMargins(16, 16, 16, 16);
    col->setSpacing(10);

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    auto *title = new QLabel("Crowding Warnings", card);
    title->setFont(Theme::ui_font(12, 700, 1.0 / 12));
    Theme::restyle(title, [=] {
        return QString("color:%1;").arg(hex(ReportTk::textHi));
    });
    head->addWidget(title);
    head->addStretch(1);
    m_pred_caption = new QLabel(card);
    m_pred_caption->setFont(Theme::mono_font(9));
    Theme::restyle(m_pred_caption, [=] {
        return QString("color:%1;").arg(hex(ReportTk::textFaint));
    });
    head->addWidget(m_pred_caption);
    col->addLayout(head);

    auto *rows_host = new QWidget(card);
    m_pred_rows = new QGridLayout(rows_host);
    m_pred_rows->setContentsMargins(0, 0, 0, 0);
    m_pred_rows->setHorizontalSpacing(12);
    m_pred_rows->setVerticalSpacing(5);
    // 보드 열폭: Time 92 · CH 40 · Level 78 · Detail(남는 폭).
    // 120 이었던 것은 초 단위까지 찍던 탓이다 (아래 표기 참조).
    m_pred_rows->setColumnMinimumWidth(0, 92);
    m_pred_rows->setColumnMinimumWidth(1, 40);
    m_pred_rows->setColumnMinimumWidth(2, 78);
    m_pred_rows->setColumnStretch(3, 1);
    col->addWidget(rows_host);

    return card;
}

void ReportPage::update_predicted()
{
    // 행 위젯을 통째로 비우고 다시 만든다 — 최대 PRED_ROWS_MAX행이라 싸다
    while (QLayoutItem *it = m_pred_rows->takeAt(0)) {
        delete it->widget();
        delete it;
    }

    const QFont f9 = Theme::mono_font(9);
    const QFont f10 = Theme::mono_font(10);

    const char *heads[4] = { "Time", "CH", "Level", "Detail" };
    for (int c = 0; c < 4; ++c) {
        auto *h = new QLabel(QString::fromLatin1(heads[c]));
        h->setFont(f9);
        Theme::restyle(h, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textFaint));
        });
        m_pred_rows->addWidget(h, 0, c);
    }

    int shown = 0, total = 0;
    for (const AlertFeed::Event &e : AlertFeed::instance()->history()) {
        if (!e.predicted)
            continue;             // 실측 유발 경보는 ALERT 화면의 몫
        ++total;
        if (shown >= PRED_ROWS_MAX)
            continue;

        const QColor lc = e.severity == AlertFeed::Critical ? Theme::alarm
                        : e.severity == AlertFeed::Warn     ? ReportTk::amber
                                                            : ReportTk::textSec;
        const int r = ++shown;

        auto add = [&](int c, const QString &text, const QColor &fg,
                       const QFont &font) {
            auto *lb = new QLabel(text);
            lb->setFont(font);
            Theme::restyle(lb, [=] {
                return QString("color:%1;").arg(hex(fg));
            });
            m_pred_rows->addWidget(lb, r, c);
        };

        // 보드 표기 "08-19 17:10" — 예측 사건에 초는 의미가 없고(모델은
        // 분 단위로 낸다) 그 초 때문에 Time 열이 28px 넓었다.
        add(0, e.ts.isValid() ? e.ts.toString("MM-dd HH:mm") : DASH,
            ReportTk::textSec, f10);
        add(1, e.channel >= 0 ? QString("CH%1").arg(e.channel + 1) : DASH,
            ReportTk::blueLight, f10);
        add(2, (e.severity == AlertFeed::Critical ? QStringLiteral("Critical")
                                                  : QStringLiteral("Warn"))
                   + (e.resolved ? QString::fromUtf8(" ✓") : QString()),
            e.resolved ? ReportTk::textFaint : lc, f10);
        // 해결된 줄은 Detail 도 함께 흐려진다 — Level 만 흐리면 한 줄 안에서
        // 두 칸이 서로 다른 상태를 말하는 것처럼 보인다 (보드도 둘 다 흐림)
        add(3, e.message, e.resolved ? ReportTk::textFaint : ReportTk::textHi, f10);
    }

    if (total == 0) {
        auto *empty = new QLabel(
            "Nothing to report - no zone was predicted to get too crowded "
            "in the last 24 hours.");
        empty->setFont(f10);
        Theme::restyle(empty, [=] {
            return QString("color:%1;").arg(hex(ReportTk::textFaint));
        });
        m_pred_rows->addWidget(empty, 1, 0, 1, 4);
    }

    m_pred_caption->setText(
        total == 0
            ? QString("last 24 hours")
            : QString("last 24 hours · %1 events%2")
                  .arg(total)
                  .arg(total > shown ? QString(" (showing %1)").arg(shown)
                                     : QString()));
}

QWidget *ReportPage::build_compare_view()
{
    auto *w = new QWidget;
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(14);

    auto *chips = new QHBoxLayout;
    chips->setSpacing(8);
    auto *lb = new QLabel("Channels", w);
    lb->setFont(Theme::mono_font(9, 400, 1.0 / 9));
    Theme::restyle(lb, [=] {
        return QString("color:%1;").arg(hex(ReportTk::textFaint));
    });
    chips->addWidget(lb);
    for (int ch = 0; ch < 4; ++ch) {
        const QStringList nm = split_channel_name(ch);
        auto *b = new QPushButton(
            QString::fromUtf8("%1 · %2").arg(nm[0], nm[1]), w);
        b->setObjectName("RChip");
        b->setFont(Theme::mono_font(11));
        b->setCheckable(true);
        b->setChecked(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setIcon(chip_icon(ReportTk::channel_color(ch), true));
        connect(b, &QPushButton::toggled, this, [this, ch](bool on) {
            if (!on) {
                // 불변식: 최소 1개 선택 — 마지막 하나는 끌 수 없다
                bool any = false;
                for (int i = 0; i < 4; ++i)
                    any = any || (i != ch && m_sel[i]);
                if (!any) {
                    QSignalBlocker block(m_chips[ch]);
                    m_chips[ch]->setChecked(true);
                    return;
                }
            }
            m_sel[ch] = on;
            m_chips[ch]->setIcon(chip_icon(ReportTk::channel_color(ch), on));
            update_views();
        });
        m_chips[ch] = b;
        chips->addWidget(b);
    }
    chips->addStretch(1);
    col->addLayout(chips);

    auto *panel = new QFrame(w);
    panel->setObjectName("RCard");
    auto *pv = new QVBoxLayout(panel);
    pv->setContentsMargins(16, 16, 16, 16);
    m_overlay = new ReportChart(400, panel);
    pv->addWidget(m_overlay);
    col->addWidget(panel);

    auto *mini_wrap = new QWidget(w);
    m_mini_grid = new QGridLayout(mini_wrap);
    m_mini_grid->setContentsMargins(0, 0, 0, 0);
    m_mini_grid->setSpacing(14);
    for (int ch = 0; ch < 4; ++ch)
        m_minis[ch] = new MiniCard(ch, mini_wrap);
    col->addWidget(mini_wrap);
    col->addStretch(1);
    update_compare_layout();
    return w;
}

// ---------------------------------------------------------------- 데이터

void ReportPage::retitle_channels()
{
    for (int ch = 0; ch < 4; ++ch) {
        if (m_cards[ch])
            m_cards[ch]->retitle(ch);
        if (m_minis[ch])
            m_minis[ch]->retitle(ch);
        if (m_chips[ch]) {
            const QStringList nm = split_channel_name(ch);
            m_chips[ch]->setText(QString::fromUtf8("%1 · %2").arg(nm[0], nm[1]));
        }
    }
}

void ReportPage::refresh_all()
{
    for (int ch = 0; ch < 4; ++ch) {
        request_camera(ch, true);
        request_camera(ch, false);
    }

    request_series(360, 5, "6h");
    request_series(1440, 15, "24h");
    AlertFeed::instance()->request_history();

    m_refresh_lb->setText(
        QString("<span style='color:%1;'>%2 · </span>"
                "<a href='refresh' style='color:%3;text-decoration:none;'>Refresh</a>")
            .arg(hex(ReportTk::textSec),
                 QTime::currentTime().toString("HH:mm:ss"),
                 hex(ReportTk::blueLight)));
}

void ReportPage::request_camera(int ch, bool pred)
{
    QUrl url = Credentials::camera_base_url();
    url.setPath(pred ? PRED_PATH : OCC_PATH);
    // v16: capacity 주입 — p_over_capacity가 존 정원 기준으로 계산돼 온다
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
        ChanData &d = m_ch[ch];

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
                FcRow r;
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
        update_views();   // 소스가 도착하는 대로 점진 갱신 (전체 대기 없음)
    });
}

void ReportPage::request_series(int minutes, int bucket_min, const QString &tag)
{
    QJsonObject params;
    params["query"] = "occseries";
    params["minutes"] = minutes;
    params["bucket_min"] = bucket_min;

    // 실패(미연결·타임아웃·ok:false)해도 6h/24h 차트만 빈 채로 남는다 —
    // 60min은 카메라 직결이라 영향 없음 (fail-soft)
    MqttLink::instance()->request(
        OCCSERIES_TOPIC, params,
        [this, tag](const QJsonObject &reply) { apply_series(reply, tag); },
        [tag](const QString &reason) {
            qWarning() << "[ReportPage] occseries" << tag << "실패:" << reason;
        });
}

void ReportPage::apply_series(const QJsonObject &o, const QString &tag)
{

    // 주의: 'slots'는 Qt 키워드 매크로 — 변수명으로 못 쓴다
    const int n_slots = (tag == "6h") ? SLOTS_6H : SLOTS_24H;
    QVector<double> series[4];
    for (int ch = 0; ch < 4; ++ch)
        series[ch] = QVector<double>(n_slots, 0.0);

    // points: [[channel, s, v], ...] · s = "몇 버킷 전"(0=최신) → 뒤에서부터
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

// ---------------------------------------------------------------- 표시 갱신

const QVector<double> &ReportPage::hist_for(int ch) const
{
    switch (m_range) {
    case R6H:  return m_ch[ch].h6h;
    case R24H: return m_ch[ch].h24h;
    default:   return m_ch[ch].h60;
    }
}

QString ReportPage::range_label() const
{
    switch (m_range) {
    case R6H:  return "-6h";
    case R24H: return "-24h";
    default:   return "-60m";
    }
}

QVector<ReportChart::FcPt> ReportPage::chart_fc(int ch) const
{
    QVector<ReportChart::FcPt> out;
    if (m_ch[ch].warmup)
        return out;   // 학습 전 예측은 그리지 않는다 (표도 '—')
    for (const FcRow &f : m_ch[ch].fc)
        if (f.p50 >= 0)
            out.append({ f.minutes, f.p50, f.p10, f.p90 });
    return out;
}

void ReportPage::update_views()
{
    const QString rl = range_label();

    for (int ch = 0; ch < 4; ++ch) {
        const ChanData &d = m_ch[ch];
        const int cap = ZoneConfig::capacity(ch);
        // pill 임계: 핸드오프의 고정 80% 대신 DB zone_thresholds.warn_ratio
        // (MQTT 수신, 기본 0.75) — 임계는 운영 데이터가 진실원천
        const bool near =
            cap > 0 && double(d.cur) / cap > ZoneConfig::warn_ratio(ch);

        m_cards[ch]->set_data(d, cap, near, hist_for(ch), chart_fc(ch), rl);
        m_minis[ch]->set_data(d, cap);
    }

    if (m_compare) {
        QVector<ReportChart::Series> list;
        for (int ch = 0; ch < 4; ++ch)
            if (m_sel[ch])
                list.append({ ReportTk::channel_color(ch), hist_for(ch),
                              chart_fc(ch) });
        m_overlay->set_compare(list, rl);
        update_compare_layout();
    }
}

void ReportPage::update_compare_layout()
{
    // 선택 수에 맞춰 재배치: 1개 = 1열, 그 외 2열 (핸드오프 §3)
    while (QLayoutItem *it = m_mini_grid->takeAt(0))
        delete it;   // 아이템만 해제 — 위젯(m_minis)은 재사용

    int nsel = 0;
    for (int ch = 0; ch < 4; ++ch)
        nsel += m_sel[ch] ? 1 : 0;
    const int cols = nsel <= 1 ? 1 : 2;

    int idx = 0;
    for (int ch = 0; ch < 4; ++ch) {
        m_minis[ch]->setVisible(m_sel[ch]);
        if (!m_sel[ch])
            continue;
        m_mini_grid->addWidget(m_minis[ch], idx / cols, idx % cols);
        ++idx;
    }
    for (int c = 0; c < 2; ++c)
        m_mini_grid->setColumnStretch(c, c < cols ? 1 : 0);
}
