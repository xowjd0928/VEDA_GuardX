#include "fire_alert_popup.h"
#include "alert_popup_stack.h"
#include "fire_alert_feed.h"
#include "sensor_fields.h"
#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

const int PULSE_MS = 550;      // AlertPopup과 동일 — 눈에 띄되 거슬리지 않는 선
const int POPUP_W = 420;

QString hex(const QColor &c) { return c.name(); }

/**
 * @brief 불꽃 스트로크 아이콘 (16px) — 디자인 보드의 SVG 를 QPainterPath 로
 *
 * 이모지(🔥)를 아이콘으로 쓰지 않는다는 08-19 디자인 규칙에 따라 직접
 * 그린다 — 크기·색이 팔레트를 따라가고 어느 배율에서도 선명하다.
 */
class FlameIcon : public QWidget
{
public:
    explicit FlameIcon(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(16, 16);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath f;
        f.moveTo(8.0, 1.5);
        f.cubicTo(9.0, 4.0, 12.5, 5.5, 12.5, 9.5);
        f.cubicTo(12.5, 12.0, 10.5, 14.2, 8.0, 14.2);
        f.cubicTo(5.5, 14.2, 3.5, 12.0, 3.5, 9.5);
        f.cubicTo(3.5, 7.5, 4.5, 6.0, 5.5, 5.0);
        f.cubicTo(5.5, 6.2, 6.2, 7.0, 7.0, 7.0);
        f.cubicTo(7.5, 5.5, 7.0, 3.5, 8.0, 1.5);
        p.setPen(QPen(Theme::alarm, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(f);
    }
};


} // namespace

FireAlertPopup::FireAlertPopup(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint
                          | Qt::WindowStaysOnTopHint)
{
    setObjectName("FireAlertPopup");
    setAttribute(Qt::WA_StyledBackground);
    setFixedWidth(POPUP_W);

    // 08-19 워크스페이스 (디자인 "Live — Alert States" 보드의 화재 팝업):
    // 상단은 붉게 물든 헤더 띠(불꽃 아이콘 + 제목 + 시각), 본문은 카드 면.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(2, 2, 2, 2);   // 2px 테두리(apply_frame) 안쪽
    root->setSpacing(0);

    auto *strip = new QWidget(this);
    strip->setAttribute(Qt::WA_StyledBackground);
    Theme::restyle(strip, [] {
        const QColor a = Theme::alarm;
        return QString("background:rgba(%1,%2,%3,0.16);")
            .arg(a.red()).arg(a.green()).arg(a.blue());
    });
    auto *head = new QHBoxLayout(strip);
    head->setContentsMargins(12, 7, 12, 7);
    head->setSpacing(8);
    head->addWidget(new FlameIcon(strip), 0, Qt::AlignTop);
    m_title = new QLabel(this);
    m_title->setFont(Theme::ui_font(12.5, 700, 0.04));
    // 라벨 3개는 생성자에서 한 번만 만들어지는 자리라 restyle로 묶는다 —
    // 연결이 라벨과 함께 죽으므로 쌓이지 않는다. 색을 지금 구워 두면
    // 다크↔라이트 전환 때 옛 색이 그대로 남는다.
    Theme::restyle(m_title, [] {
        return QString("color:%1;").arg(hex(Theme::alarm));
    });
    // 고정폭 팝업이라 제목이 길어지면 잘린다. 지금 문구는 짧지만 zone 번호가
    // 두 자리가 되는 등 조금만 늘어나도 경계에 닿으므로 접히게 해둔다
    // (adjustSize가 높이를 따라 늘려준다).
    m_title->setWordWrap(true);
    head->addWidget(m_title, 1);
    head->addStretch(1);
    m_time = new QLabel(this);
    m_time->setFont(Theme::mono_font(10));
    Theme::restyle(m_time, [] {
        return QString("color:%1;").arg(hex(Theme::textDim));
    });
    head->addWidget(m_time);
    root->addWidget(strip);

    auto *body_wrap = new QWidget(this);
    auto *body_col = new QVBoxLayout(body_wrap);
    body_col->setContentsMargins(14, 10, 14, 12);
    body_col->setSpacing(10);

    m_body = new QLabel(this);
    m_body->setFont(Theme::mono_font(11));
    Theme::restyle(m_body, [] {
        return QString("color:%1;").arg(hex(Theme::textMid));
    });
    m_body->setWordWrap(true);
    body_col->addWidget(m_body);

    auto *btns = new QHBoxLayout;
    btns->setSpacing(8);

    // 주 동작은 보드처럼 **붉게 채운** 버튼이다 — 화재 팝업에서 시선이 먼저
    // 가야 할 곳. (한 팝업에 채운 버튼은 하나만)
    m_btn_device = new QPushButton("Open Device Control", this);
    Theme::restyle(m_btn_device, [] {
        return QString(
            "QPushButton { background:%1; border:1px solid %1;"
            " border-radius:3px; color:%2; padding:0 14px; }"
            "QPushButton:hover { background:%3; border-color:%3; }")
            .arg(Theme::alarm.name(), Theme::OnVideo::bg0.name(),
                 Theme::alarm.lighter(115).name());
    });
    m_btn_device->setFont(Theme::ui_font(11, 600, 0.06));
    m_btn_device->setFixedHeight(28);
    m_btn_device->setCursor(Qt::PointingHandCursor);
    connect(m_btn_device, &QPushButton::clicked, this, [this] {
        // refresh()와 같은 우선순위(화재 > 버튼)로 "지금 보이는 쪽"에 맞춰
        // 목적지가 갈린다 — 화재는 Device Control, 버튼은 CCTV.
        // 버튼 문구도 refresh()에서 같은 기준으로 바꾸므로 둘이 어긋나지 않는다.
        if (m_fire_pending)
            emit goto_device_control_requested(FireAlertFeed::instance()->state().zone_id);
        else
            emit goto_live_requested(m_button_zone);
        dismiss_current();
    });
    btns->addWidget(m_btn_device);

    m_btn_close = new QPushButton("Acknowledge", this);
    m_btn_close->setObjectName("OutlineBtn");
    m_btn_close->setFont(Theme::ui_font(11, 600, 0.06));
    m_btn_close->setFixedHeight(28);
    m_btn_close->setCursor(Qt::PointingHandCursor);
    connect(m_btn_close, &QPushButton::clicked, this, [this] {
        dismiss_current();
    });
    btns->addWidget(m_btn_close);
    btns->addStretch(1);
    body_col->addLayout(btns);
    root->addWidget(body_wrap);

    m_pulse = new QTimer(this);
    m_pulse->setInterval(PULSE_MS);
    connect(m_pulse, &QTimer::timeout, this, [this] {
        m_pulse_on = !m_pulse_on;
        refresh();
    });

    FireAlertFeed *feed = FireAlertFeed::instance();
    connect(feed, &FireAlertFeed::state_changed, this, &FireAlertPopup::on_fire_changed);
    connect(feed, &FireAlertFeed::button_pressed, this, &FireAlertPopup::on_button_pressed);

    // 테마 전환 — 창의 배경·테두리를 다시 굽는다. 라벨 3개는 위에서 restyle로
    // 묶여 있어 자동으로 따라오므로 여기서 할 일은 창틀뿐이다.
    //
    // ⚠ refresh()를 걸면 안 된다. 그 함수는 "다시 칠하기"가 아니라 **팝업의
    //   표시 상태 자체**다(hide/show/raise/펄스 시작·정지가 들어 있다). 색을
    //   되살리려고 부른 것이 창을 띄우거나 감추는 부수효과를 내면 안 된다.
    Theme::on_theme_changed(this, [this] { apply_frame(); });

    AlertPopupStack::instance()->add(this, AlertPopupStack::Fire);

    hide();
}

void FireAlertPopup::on_fire_changed()
{
    const bool active = FireAlertFeed::instance()->state().active;

    // 확정된 순간에만 pending을 켠다 — 해제 전이(active=false)는 절대
    // 건드리지 않는다. 그게 이 팝업이 "확인"을 누르기 전까진 안 닫히는
    // 이유의 전부다(refresh()는 fire.active가 아니라 이 플래그만 본다).
    if (active)
        m_fire_pending = true;

    refresh();
}

void FireAlertPopup::on_button_pressed(int cumulative_count, int zone_id, const QDateTime &ts)
{
    // 매 눌림마다 다시 뜬다 — 이전 눌림을 확인했었어도 새 눌림은 별개 사건이다
    m_button_pending = true;
    m_button_count = cumulative_count;
    m_button_zone = zone_id;
    m_button_ts = ts;
    refresh();
}

/**
 * @brief 창틀(면·테두리)만 다시 굽는다 — 점멸과 테마 전환이 함께 쓰는 자리
 *
 * 점멸은 테두리로만 한다 — 글자가 깜박이면 읽을 수가 없다.
 * 토큰 선택(경보 팝업 공통 기준): 면은 panel — 앱 배경(bg0) 위에 뜨는 별개의
 * 창이라 한 단 위 면을 써야 창 경계가 보인다. 테두리는 alarm — 화재/비상
 * 전용 창이라 상태색을 쓸 자격이 있는 몇 안 되는 자리다. 어두운 쪽도
 * 하드코딩 대신 alarm에서 파생시킨다(라이트 테마의 alarm이 바뀌면 점멸의
 * 양쪽이 같이 따라온다).
 *
 * 반복 호출되는 자리라 restyle을 쓰지 않는다 — 생성자에서 이 함수를
 * on_theme_changed에 한 번 걸어 두는 것으로 테마 추종을 대신한다.
 */
void FireAlertPopup::apply_frame()
{
    const QColor edge = m_pulse_on ? Theme::alarm : Theme::alarm.darker(230);
    setStyleSheet(QString("#FireAlertPopup { background:%1; border:2px solid %2;"
                          "border-radius:4px; }")
                      .arg(hex(Theme::panel), hex(edge)));
}

void FireAlertPopup::refresh()
{
    const FireAlertFeed::State fire = FireAlertFeed::instance()->state();
    const bool show_fire = m_fire_pending;
    const bool show_button = !show_fire && m_button_pending;

    if (!show_fire && !show_button) {
        m_pulse->stop();
        hide();
        return;
    }

    if (show_fire) {
        // 지금도 진행 중인지/이미 꺼졌는지에 따라 문구만 바뀐다 — 뜨는 조건
        // 자체는(m_fire_pending) fire.active와 무관하게 유지된다.
        // 제목은 "무슨 일이 어느 구역에서"까지만 — 원인처럼 길이가 들쭉날쭉한
        // 것은 본문으로 내린다. 제목에 붙이면 채널명이 긴 경우(대상표면온도 등)
        // 고정폭(POPUP_W)을 넘겨 잘려 나간다.
        if (fire.active) {
            m_title->setText(QString("Fire Confirmed — Zone %1").arg(fire.zone_id));
            m_body->setText(QString(
                "Cause: %1\n"
                "The sensors confirmed a fire. Valve, shutter, smoke exhaust, "
                "suppression and alarm run automatically. Watch the live values "
                "in Device Control.")
                    .arg(cause_text(fire.cause)));
        } else {
            m_title->setText(QString("Fire Cleared — Zone %1").arg(fire.zone_id));
            m_body->setText(QString(
                "Cause: %1 (recorded before it cleared)\n"
                "The situation is over (pump, fan and alarm stopped). Valve and "
                "shutter were NOT restored automatically, for safety - check the "
                "site and operate them by hand.")
                    .arg(cause_text(fire.cause)));
        }
        m_time->setText(fire.since.toString("HH:mm:ss"));
        m_btn_device->setText("Open Device Control");
    } else {
        m_title->setText(QString("Emergency Button — Zone %1").arg(m_button_zone));
        m_body->setText(QString("The emergency button was pressed on site "
                                "(press #%1).\n"
                                "The hardware interlock already handled the "
                                "actual control - what needs checking now is "
                                "the site itself.")
                             .arg(m_button_count));
        m_time->setText(m_button_ts.toString("HH:mm:ss"));
        m_btn_device->setText("Open CCTV");
    }

    apply_frame();
    adjustSize();
    setFixedWidth(POPUP_W);

    if (!isVisible()) {
        reposition();
        // 로그인 전이면 보류 — m_fire_pending 은 건드리지 않는다.
        AlertPopupStack::instance()->try_show(this);
    }
    if (!m_pulse->isActive()) {
        m_pulse_on = true;
        m_pulse->start();
    }
}

void FireAlertPopup::dismiss_current()
{
    // refresh()와 같은 우선순위(화재 > 버튼)로 "지금 보이는 쪽"만 확인 처리한다
    if (m_fire_pending)
        m_fire_pending = false;
    else
        m_button_pending = false;

    refresh();   // 확인한 쪽이 사라진 뒤 다른 쪽이 남아있으면 이어서 뜬다
}

void FireAlertPopup::reposition()
{
    AlertPopupStack::instance()->relayout();
}
