#include "audio_alert_card.h"

#include "alert_popup_stack.h"
#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

const int PULSE_MS = 500;   // 테두리 점멸 주기
const int CARD_W = 420;

QString hex(const QColor &c) { return c.name(); }

QString title_for(const QString &event)
{
    return event == QLatin1String("gunshot") ? QStringLiteral("Gunshot detected")
                                             : QStringLiteral("Scream detected");
}

} // namespace

AudioAlertCard::AudioAlertCard(const QString &event, int channel,
                               double confidence, const QDateTime &ts,
                               QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint
                          | Qt::WindowStaysOnTopHint),
      m_event(event),
      m_first_ts(ts),
      m_last_ts(ts),
      m_channel(channel),
      m_max_confidence(confidence)
{
    setObjectName("AudioAlertPopup");   // 기존 팝업과 같은 QSS 규칙을 쓴다
    setAttribute(Qt::WA_StyledBackground);
    setFixedWidth(CARD_W);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    m_title = new QLabel(this);
    // 글자 크기는 스타일시트에서 뺀다 — 거기 박으면 전역 글자 배율을 못 탄다
    m_title->setFont(Theme::ui_font(15, 700));
    Theme::restyle(m_title, [] {
        return QString("color:%1;").arg(hex(Theme::alarm));
    });
    head->addWidget(m_title);
    head->addStretch(1);
    m_when = new QLabel(this);
    m_when->setFont(Theme::mono_font(10));
    Theme::restyle(m_when, [] {
        return QString("color:%1;").arg(hex(Theme::textDim));
    });
    head->addWidget(m_when);
    root->addLayout(head);

    m_body = new QLabel(this);
    m_body->setFont(Theme::mono_font(11));
    Theme::restyle(m_body, [] {
        return QString("color:%1;").arg(hex(Theme::textMid));
    });
    m_body->setWordWrap(true);
    root->addWidget(m_body);

    auto *btns = new QHBoxLayout;
    btns->setSpacing(8);
    btns->addStretch(1);

    m_btn_live = new QPushButton("View Live", this);
    m_btn_live->setObjectName("OutlineBtn");
    m_btn_live->setFixedHeight(28);
    m_btn_live->setCursor(Qt::PointingHandCursor);
    connect(m_btn_live, &QPushButton::clicked, this,
            [this] { emit goto_live_requested(m_channel); });
    btns->addWidget(m_btn_live);

    // 자동 닫힘 타이머는 두지 않는다. 이 버튼이 카드가 사라지는 유일한 길이다.
    m_btn_ack = new QPushButton("Acknowledge", this);
    m_btn_ack->setObjectName("OutlineBtn");
    m_btn_ack->setFixedHeight(28);
    m_btn_ack->setCursor(Qt::PointingHandCursor);
    connect(m_btn_ack, &QPushButton::clicked, this, [this] {
        m_acknowledged = true;   // 이후 감지는 이 카드에 붙지 않는다
        m_pulse->stop();
        hide();
        emit acknowledged(this);
    });
    btns->addWidget(m_btn_ack);
    root->addLayout(btns);

    m_pulse = new QTimer(this);
    m_pulse->setInterval(PULSE_MS);
    connect(m_pulse, &QTimer::timeout, this, [this] {
        m_pulse_on = !m_pulse_on;
        apply_frame();
    });

    // 면·테두리는 색을 스타일시트에 구워 넣는 자리라 테마를 안 따라온다.
    // 점멸이 매번 부르는 함수라 restyle 로 감쌀 수 없어(호출마다 notifier 가
    // 쌓인다) 여기서 한 번만 등록한다.
    Theme::on_theme_changed(this, [this] { apply_frame(); });

    AlertPopupStack::instance()->add(this, AlertPopupStack::Audio);

    refresh();
    apply_frame();
    adjustSize();
    setFixedWidth(CARD_W);
    AlertPopupStack::instance()->try_show(this);   // 로그인 전이면 보류
    m_pulse->start();
}

bool AudioAlertCard::accepts(const QString &event, const QDateTime &ts) const
{
    if (m_acknowledged || event != m_event)
        return false;
    // 구간은 최초 감지 시각부터 고정이다 — 반복 감지로 늘리지 않는다.
    const qint64 elapsed = m_first_ts.msecsTo(ts);
    return elapsed >= 0 && elapsed < WINDOW_MS;
}

void AudioAlertCard::add_detection(int channel, double confidence,
                                   const QDateTime &ts)
{
    ++m_count;
    m_last_ts = ts;
    if (confidence > m_max_confidence)
        m_max_confidence = confidence;
    if (channel >= 0)
        m_channel = channel;   // 마지막으로 잡힌 채널로 LIVE 를 연다

    refresh();
    adjustSize();
    setFixedWidth(CARD_W);
    // 카드 높이가 바뀌면 아래 카드들이 겹친다 — 다시 쌓는다.
    AlertPopupStack::instance()->relayout();
    // 대기 중인 카드는 여기서 도로 띄우지 않는다. 띄우면 장수 제한이
    // 무의미해지고, 화면 밖으로 밀려 안 보이는 카드가 다시 생긴다.
    if (!m_pending && !isVisible())
        AlertPopupStack::instance()->try_show(this);
}

void AudioAlertCard::set_pending(bool pending)
{
    if (m_pending == pending)
        return;
    m_pending = pending;

    if (m_pending) {
        m_pulse->stop();
        hide();
        return;
    }
    if (m_acknowledged)
        return;   // 대기 중에 확인될 일은 없지만, 되살리지는 않는다
    AlertPopupStack::instance()->try_show(this);
    m_pulse->start();
}

void AudioAlertCard::refresh()
{
    m_title->setText(QString("🔊  %1").arg(title_for(m_event)));

    // 한 번만 감지된 경우에는 최초 시각 하나만 보여준다 — "17:05 ~ 17:05"는
    // 구간처럼 보이지만 실제로는 점 사건이라 오해를 부른다.
    if (m_count > 1) {
        m_when->setText(QString("%1 ~ %2")
                            .arg(m_first_ts.toString("HH:mm:ss"),
                                 m_last_ts.toString("HH:mm:ss")));
        m_body->setText(QString("%1 detections · peak confidence %2% · channel %3")
                            .arg(m_count)
                            .arg(qRound(m_max_confidence * 100))
                            .arg(m_channel));
    } else {
        m_when->setText(m_first_ts.toString("HH:mm:ss"));
        m_body->setText(QString("confidence %1% · channel %2")
                            .arg(qRound(m_max_confidence * 100))
                            .arg(m_channel));
    }
}

void AudioAlertCard::apply_frame()
{
    // 점멸은 테두리로만 — FireAlertPopup 과 같은 방식(alarm 을 어둡게)
    const QColor edge = m_pulse_on ? Theme::alarm : Theme::alarm.darker(230);
    setStyleSheet(QString("#AudioAlertPopup { background:%1;"
                          " border:2px solid %2; border-radius:4px; }")
                      .arg(hex(Theme::panel), hex(edge)));
}
