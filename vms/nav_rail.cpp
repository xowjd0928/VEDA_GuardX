#include "nav_rail.h"
#include "alert_feed.h"
#include "theme.h"

#include <QAbstractButton>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QPainter>
#include <QTimer>

#include <iterator>

/**
 * @brief 워크스페이스 탭 하나 (40px 높이) — 라벨 + 경보 배지를 직접 그린다
 *
 * QSS로는 "체크 시 상단 2px 마커 + 활성 면 + 라벨 뒤 배지"의 조합이 어색해서
 * paintEvent로 그린다. 시그널은 QAbstractButton 것을 그대로 쓴다.
 */
class NavButton : public QAbstractButton
{
public:
    NavButton(const QString &label, QWidget *parent)
        : QAbstractButton(parent)
    {
        setText(label);
        setCheckable(true);
        setFixedHeight(40);
        setCursor(Qt::PointingHandCursor);
        setFont(Theme::ui_font(12, 500));
    }

    QSize sizeHint() const override
    {
        // 좌우 18px 패딩 + **배지 자리 상시 확보**(Live 탭에만 붙지만 자리는
        // 항상 잡는다) — 경보가 떴다 사라질 때 탭 폭이 변하면 바 전체가
        // 옆으로 밀렸다 돌아오는 것처럼 보인다 (08-19 사용자 신고).
        const QFontMetricsF fm(font());
        return QSize(qRound(fm.horizontalAdvance(text())) + 36 + 22, 40);
    }

    /**
     * @brief 라벨 뒤 배지 — count 0이면 숨김
     * @param color 배지 바탕색 (단계별)
     * @param dim   점멸의 어두운 위상
     */
    void set_badge(int count, const QColor &color, bool dim)
    {
        if (m_badge == count && m_badge_color == color && m_badge_dim == dim)
            return;
        m_badge = count;
        m_badge_color = color;
        m_badge_dim = dim;
        update();   // 폭은 불변(자리 상시 확보) — 다시 그리기만 한다
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (isChecked()) {
            p.fillRect(rect(), Theme::chromeSelBg);
            p.fillRect(0, 0, width(), 2, Theme::chromeSelText);  // 상단 2px 마커
        }

        const QColor fg = isChecked()   ? Theme::chromeText
                        : underMouse()  ? Theme::chromeText
                                        : Theme::chromeTextDim;
        p.setPen(fg);
        p.setFont(font());
        const QFontMetricsF fm(font());
        const double text_w = fm.horizontalAdvance(text());
        const double x = 18;
        p.drawText(QRectF(x, 0, text_w + 2, height()),
                   Qt::AlignVCenter | Qt::AlignLeft, text());

        if (m_badge > 0) {
            QColor bg = m_badge_color;
            if (m_badge_dim)
                bg.setAlpha(90);
            const QRectF r(x + text_w + 8, (height() - 16) / 2.0, 16, 16);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawEllipse(r);
            p.setPen(Theme::OnVideo::bg0);  // 배지 숫자는 항상 어두운 글자
            p.setFont(Theme::mono_font(9, 600));
            p.drawText(r, Qt::AlignCenter, QString::number(m_badge));
        }
    }

    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    int m_badge = 0;
    QColor m_badge_color;
    bool m_badge_dim = false;
};

// ---------------------------------------------------------------------------

NavRail::NavRail(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("NavRail");
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(40);   // 워크스페이스 탭 줄 (08-19 — 좌측 레일 폐지)

    // 순서 = QStackedWidget 페이지 인덱스 (UI_REDESIGN_SPEC §2).
    // TRACK은 없다 — 동선은 LIVE 우측 TrackingPanel로 들어갔다.
    // Predictions·Flow 도 없다 (08-20) — 둘을 합친 Analytics 가 그 자리다.
    // 두 화면의 코드는 그대로 살아 있다(report_page·business_flow_page, 계속
    // 빌드된다) — 되살리려면 여기 항목과 mainwindow 의 addWidget 을 함께
    // 되돌리고 아래 인덱스 상수를 다시 맞추면 된다.
    // 경보도 별도 화면이 아니라 팝업(AlertPopup) + LIVE 타일 색으로 알린다.
    // 여기 Live 탭에 붙는 배지는 "지금 몇 채널이 경보인가"의 상시 표시다.
    // tip = 탭 위에 커서를 올리면 나오는 도움말 (Edge Map 버튼과 같은 방식).
    struct Item { const char *label; const char *tip; };
    static const Item items[] = {
        {"Live",
         "Live monitoring - 4-channel video wall with person tracking,\n"
         "event ticker, and zone occupancy"},
        {"Crowd",
         "Crowd analytics - per-channel density heatmaps and\n"
         "occupancy history"},
        {"Analytics",
         "How busy each zone will get, where visitors stay longest,\n"
         "and which paths they take between zones"},
        {"Device",
         "Device control - environment sensors, actuators,\n"
         "and RPi node status"},
        {"Camera",
         "Camera control - device status, on-camera apps,\n"
         "and stream profiles"},
        {"Settings",
         "Settings - zone capacity and congestion thresholds,\n"
         "fire detection thresholds"},
    };
    const int count = int(sizeof(items) / sizeof(items[0]));

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(2);

    for (int i = 0; i < count; ++i) {
        auto *btn = new NavButton(QString::fromUtf8(items[i].label), this);
        btn->setToolTip(QString::fromUtf8(items[i].tip));
        connect(btn, &QAbstractButton::clicked, this, [this, i] {
            set_current(i);
            emit screen_selected(i);
        });
        lay->addWidget(btn);
        m_buttons.append(btn);
    }
    lay->addStretch(1);

    m_buttons[0]->setChecked(true);

    // 경보 배지 — critical일 때만 점멸 타이머가 돈다
    m_pulse = new QTimer(this);
    m_pulse->setInterval(550);
    connect(m_pulse, &QTimer::timeout, this, [this] {
        m_pulse_on = !m_pulse_on;
        refresh_alert_badge();
    });
    connect(AlertFeed::instance(), &AlertFeed::state_changed,
            this, &NavRail::refresh_alert_badge);
    refresh_alert_badge();
}

void NavRail::refresh_alert_badge()
{
    if (m_buttons.size() <= ALERT_INDEX)
        return;

    AlertFeed *feed = AlertFeed::instance();
    const int crit = feed->critical_count();  // 채널 + 장비(§4c-2) 합산
    int warn = 0;
    for (int ch = 0; ch < 4; ++ch)
        warn += (feed->severity(ch) == AlertFeed::Warn);
    warn += feed->device_count(AlertFeed::Warn);

    if (crit > 0) {
        if (!m_pulse->isActive()) {
            m_pulse_on = true;
            m_pulse->start();
        }
        // 탭 줄 크롬은 두 테마 모두 어두우므로 OnVideo 밝은 값으로 칠한다
        m_buttons[ALERT_INDEX]->set_badge(crit, Theme::OnVideo::alarm,
                                          !m_pulse_on);
    } else {
        m_pulse->stop();
        // warn은 알리되 점멸하지 않는다 — 계속 깜박이면 경보 피로가 온다
        m_buttons[ALERT_INDEX]->set_badge(warn, Theme::OnVideo::amber, false);
    }
}

void NavRail::set_current(int index)
{
    for (int i = 0; i < m_buttons.size(); ++i)
        m_buttons[i]->setChecked(i == index);
}
