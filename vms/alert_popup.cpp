#include "alert_popup.h"
#include "alert_popup_stack.h"
#include "alert_feed.h"
#include "theme.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

const int PULSE_MS = 550;      // 눈에 띄되 거슬리지 않는 선
const int POPUP_W = 420;

QString hex(const QColor &c) { return c.name(); }

} // namespace

AlertPopup::AlertPopup(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint
                          | Qt::WindowStaysOnTopHint)
{
    setObjectName("AlertPopup");
    setAttribute(Qt::WA_StyledBackground);
    setFixedWidth(POPUP_W);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    m_title = new QLabel(this);
    m_title->setFont(Theme::ui_font(15, 700, 0.08));
    Theme::restyle(m_title, [=] {
        return QString("color:%1;").arg(hex(Theme::alarm));
    });
    head->addWidget(m_title);
    head->addStretch(1);
    m_time = new QLabel(this);
    m_time->setFont(Theme::mono_font(10));
    Theme::restyle(m_time, [=] {
        return QString("color:%1;").arg(hex(Theme::textDim));
    });
    head->addWidget(m_time);
    root->addLayout(head);

    m_body = new QLabel(this);
    m_body->setFont(Theme::mono_font(11));
    Theme::restyle(m_body, [=] {
        return QString("color:%1;").arg(hex(Theme::textMid));
    });
    m_body->setWordWrap(true);
    root->addWidget(m_body);

    auto *btns = new QHBoxLayout;
    btns->setSpacing(8);
    btns->addStretch(1);

    m_btn_live = new QPushButton("View Live", this);
    m_btn_live->setObjectName("OutlineBtn");
    m_btn_live->setFont(Theme::ui_font(11, 600, 0.06));
    m_btn_live->setFixedHeight(28);
    m_btn_live->setCursor(Qt::PointingHandCursor);
    connect(m_btn_live, &QPushButton::clicked, this, [this] {
        emit goto_live_requested();
        m_dismissed = true;
        hide();
        m_pulse->stop();
    });
    btns->addWidget(m_btn_live);

    m_btn_close = new QPushButton("Acknowledge", this);
    m_btn_close->setObjectName("OutlineBtn");
    m_btn_close->setFont(Theme::ui_font(11, 600, 0.06));
    m_btn_close->setFixedHeight(28);
    m_btn_close->setCursor(Qt::PointingHandCursor);
    connect(m_btn_close, &QPushButton::clicked, this, [this] {
        // 닫아도 경보 자체는 유효하다 — LIVE 타일은 계속 빨갛게 남는다.
        // 새 critical 전이가 오면 다시 뜬다.
        m_dismissed = true;
        hide();
        m_pulse->stop();
    });
    btns->addWidget(m_btn_close);
    root->addLayout(btns);

    m_pulse = new QTimer(this);
    m_pulse->setInterval(PULSE_MS);
    connect(m_pulse, &QTimer::timeout, this, [this] {
        m_pulse_on = !m_pulse_on;
        refresh();
    });

    AlertFeed *feed = AlertFeed::instance();
    connect(feed, &AlertFeed::state_changed, this, &AlertPopup::on_state_changed);
    connect(feed, &AlertFeed::alert_raised, this,
            [this](int, int severity) {
                // 새 critical 전이 — 사용자가 닫아뒀어도 다시 띄운다
                if (severity == AlertFeed::Critical)
                    m_dismissed = false;
                on_state_changed();
            });
    // 카메라/앱 경보(§4c-2)도 같은 규칙 — critical이면 닫힘을 푼다
    connect(feed, &AlertFeed::device_alert, this,
            [this](int, int severity, const QString &) {
                if (severity == AlertFeed::Critical)
                    m_dismissed = false;
                on_state_changed();
            });

    // 테마 전환 — 제목·본문·테두리에 색을 구워 넣는다
    Theme::on_theme_changed(this, [this] { refresh(); });

    AlertPopupStack::instance()->add(this, AlertPopupStack::Congestion);

    hide();
}

void AlertPopup::on_state_changed()
{
    const bool critical = AlertFeed::instance()->critical_count() > 0;

    if (!critical) {
        // 전부 해제 — 스스로 닫히고, 다음 경보를 위해 dismiss 상태도 푼다
        m_dismissed = false;
        m_pulse->stop();
        hide();
        return;
    }

    if (m_dismissed)
        return;

    refresh();
    if (!isVisible()) {
        reposition();
        AlertPopupStack::instance()->try_show(this);   // 로그인 전이면 보류
    }
    if (!m_pulse->isActive()) {
        m_pulse_on = true;
        m_pulse->start();
    }
}

void AlertPopup::refresh()
{
    AlertFeed *feed = AlertFeed::instance();

    QStringList lines;
    QStringList names;
    for (int ch = 0; ch < 4; ++ch) {
        if (feed->severity(ch) != AlertFeed::Critical)
            continue;
        const AlertFeed::State st = feed->state(ch);
        names << Theme::channel_name(ch).section(QString::fromUtf8(" · "), 0, 0);

        QString l = Theme::channel_name(ch);
        if (st.count >= 0 && st.capacity > 0) {
            l += QString("  %1/%2 people (%3%)")
                     .arg(st.count).arg(st.capacity)
                     .arg(qRound(100.0 * st.count / st.capacity));
        }
        l += st.predicted ? QString("  · triggered by forecast")
                          : QString("  · triggered by measurement");
        lines << l;
    }

    // 카메라/앱 critical (§4c-2) — 같은 팝업에 함께 싣는다
    const QStringList dev = feed->device_messages(AlertFeed::Critical);
    if (!dev.isEmpty()) {
        names << QStringLiteral("Camera");
        lines << dev;
    }
    if (lines.isEmpty())
        return;

    m_title->setText(QString::fromUtf8("⚠  Critical — %1").arg(names.join(", ")));
    m_body->setText(lines.join("\n"));
    m_time->setText(QTime::currentTime().toString("HH:mm:ss"));

    // 점멸은 테두리로만 — 글자가 깜박이면 읽을 수가 없다
    const QColor edge = m_pulse_on ? Theme::alarm : Theme::alarm.darker(230);
    setStyleSheet(QString("#AlertPopup { background:%1; border:2px solid %2;"
                          "border-radius:4px; }")
                      .arg(hex(Theme::panel), hex(edge)));
    adjustSize();
    setFixedWidth(POPUP_W);
}

void AlertPopup::reposition()
{
    // 자리는 AlertPopupStack 이 정한다 — 팝업이 넷이라 각자 오프셋을 들고
    // 있으면 언젠가 겹친다(실제로 화재·음향이 겹쳐 있었다).
    AlertPopupStack::instance()->relayout();
}
