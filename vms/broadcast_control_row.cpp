#include "broadcast_control_row.h"

#include "auth.h"
#include "broadcast_controller.h"
#include "broadcast_protocol.h"
#include "mqtt_link.h"
#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSlider>

BroadcastControlRow::BroadcastControlRow(QWidget *parent)
    : QWidget(parent), m_broadcast(new BroadcastController(this))
{
    // 행 구분선 — 생성자에서 한 번만 도는 자리라 restyle로 테마에 묶는다
    Theme::restyle(this, [] {
        return QString("border-bottom:1px solid %1;")
            .arg(Theme::rowDivider.name());
    });

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(10, 10, 10, 10);
    row->setSpacing(10);

    auto *label = new QLabel("Live broadcast", this);
    label->setFont(Theme::ui_font(12, QFont::DemiBold));
    row->addWidget(label, 1);

    m_status = new QLabel("idle", this);
    m_status->setFont(Theme::mono_font(10));
    row->addWidget(m_status);

    // 송출 레벨 — UDP는 단방향이라 VMS가 "수신되고 있는가"는 알 수 없다.
    // 최소한 "마이크가 잡히고 내보내고는 있는가"를 눈으로 확인할 수 있게 한다.
    // (수신기가 죽어 있어도 이 막대는 움직인다 — 그건 RPi C 쪽에서 본다)
    m_level = new QProgressBar(this);
    m_level->setRange(0, 100);
    m_level->setValue(0);
    m_level->setTextVisible(false);
    m_level->setFixedSize(72, 8);
    m_level->setToolTip(
        "Outgoing audio level (after noise cancelling). If you speak and the "
        "bar does not move, the microphone input is the problem.");
    // 막대 색은 값과 무관하다(set_level은 setValue만 한다) — 여기서 한 번
    // restyle로 묶어 두면 테마 전환까지 따라온다
    Theme::restyle(m_level, [] {
        return QString(
            "QProgressBar{background:%1;border:none;border-radius:4px;}"
            "QProgressBar::chunk{background:%2;border-radius:4px;}")
            .arg(Theme::rowDivider.name(), Theme::green.name());
    });
    row->addWidget(m_level);
    connect(m_broadcast, &BroadcastController::level_changed,
            this, &BroadcastControlRow::set_level);

    // ── 출력 음량 ──────────────────────────────────────────────────
    // 방송에만 걸린다. 사이렌은 RPi C 가 로컬 음원으로 재생하므로 여기를
    // 0 으로 내려도 화재 경보 소리는 그대로다 — 안전 기능을 UI 로 끌 수
    // 있으면 안 되기 때문에 이 분리가 중요하다.
    m_volume = new QSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(Theme::px(96));
    m_volume->setCursor(Qt::PointingHandCursor);
    m_volume->setToolTip(
        "Broadcast output volume. The fire siren is played locally by "
        "RPi C and is not affected by this.");
    m_volume_label = new QLabel(this);
    m_volume_label->setFont(Theme::mono_font(10));
    m_volume_label->setMinimumWidth(Theme::px(34));
    m_volume_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(m_volume);
    row->addWidget(m_volume_label);

    // 저장값으로 시작하고, 움직이는 즉시 저장 + 방송 중이면 즉시 반영.
    m_volume->setValue(m_broadcast->volume_percent());
    set_volume_label(m_volume->value());
    connect(m_volume, &QSlider::valueChanged, m_broadcast,
            &BroadcastController::set_volume_percent);
    connect(m_broadcast, &BroadcastController::volume_changed, this,
            [this](int percent) {
        // 값이 다른 경로로 바뀌었을 때만 슬라이더를 되돌린다. 조건 없이
        // setValue 하면 드래그 중에 손잡이가 튄다.
        if (m_volume->value() != percent)
            m_volume->setValue(percent);
        set_volume_label(percent);
    });

    // 08-10: 전송방식(MQTT↔RTP) 전환 버튼과 노캔 토글 버튼을 없앴다.
    // 전송은 RTP 전용으로 확정됐고(RPi C 담당자 합의) 노캔은 항상 켜므로
    // 고를 것이 없다. 노캔 상태는 아래 "방송 중" 문구에 그대로 드러난다.
    m_on = new QPushButton("On", this);
    m_off = new QPushButton("Off", this);
    for (QPushButton *b : { m_on, m_off }) {
        // 스타일 없는 맨 버튼은 OS 기본 모양이라 라이트 테마의 흰 카드 위에서
        // 통째로 사라진다 — 디자인 시스템의 #OutlineBtn을 입힌다(전역 QSS가 두
        // 테마를 모두 정의한다). 폭은 글자 배율과 함께 늘린다.
        b->setObjectName("OutlineBtn");
        b->setCursor(Qt::PointingHandCursor);
        b->setMinimumWidth(Theme::px(56));
    }
    row->addWidget(m_on);
    row->addWidget(m_off);

    connect(m_on, &QPushButton::clicked, this,
            &BroadcastControlRow::on_clicked_on);
    // RPi C 가 거절한 경우(거의 동시에 눌렸을 때)도 같은 확인창으로 온다.
    connect(m_broadcast, &BroadcastController::takeover_required, this,
            [this](const QString &owner) {
        if (confirm_takeover(owner))
            m_broadcast->start_takeover();
    });
    // 다른 VMS 의 점유 상태가 바뀌면 버튼 활성 상태를 다시 계산한다.
    connect(m_broadcast, &BroadcastController::remote_state_changed, this,
            [this] { sync_ui(m_broadcast->active()); });
    connect(m_off, &QPushButton::clicked, m_broadcast, &BroadcastController::stop);
    connect(m_broadcast, &BroadcastController::active_changed,
            this, &BroadcastControlRow::sync_ui);
    // 권한이 바뀌면 같은 함수로 다시 계산한다 (판정이 한 곳에 모여 있다)
    connect(Auth::instance(), &Auth::state_changed, this,
            [this] { sync_ui(m_broadcast->active()); });
    connect(Auth::instance(), &Auth::verification_changed, this,
            [this] { sync_ui(m_broadcast->active()); });
    connect(m_broadcast, &BroadcastController::status_changed, this,
            [this](const QString &message, bool error) {
        m_status->setText(message);
        set_status_color(error ? &Theme::alarm
                               : m_broadcast->active() ? &Theme::green
                                                       : &Theme::textDim);
    });
    // ⚠ 08-10: MqttLink 연동을 전부 뺐다. RTP 방송은 UDP 직결이라 **브로커와
    // 무관하다** (BroadcastController::start 는 MQTT 를 건드리지 않는다).
    // 그런데 예전 코드는 sync_ui 에서 ON 버튼을 `MqttLink::online()` 으로 잠그고
    // 상태를 "MQTT 오프라인"으로 덮었다 — 브로커가 죽으면 **멀쩡한 음성 방송을
    // 시작할 수 없었다.** (병합 후 리뷰 확정건)
    sync_ui(false);

    // 상태 글자색은 스타일시트에 굽지만 반복 호출되는 자리라 restyle 을 쓰면
    // notifier 연결이 끝없이 쌓인다. 다시 칠하는 쪽만 여기서 한 번 등록한다.
    Theme::on_theme_changed(this, [this] { set_status_color(m_status_color); });
}

void BroadcastControlRow::set_volume_label(int percent)
{
    m_volume_label->setText(QString("%1%").arg(percent));
}

void BroadcastControlRow::on_clicked_on()
{
    // 아무도 방송 중이 아니거나 우리가 방송 중이면 바로 시작한다 —
    // 평범한 조작에 확인창을 끼우지 않는다.
    if (!m_broadcast->other_broadcasting()) {
        m_broadcast->start();
        return;
    }
    if (confirm_takeover(m_broadcast->current_owner()))
        m_broadcast->start_takeover();
}

bool BroadcastControlRow::confirm_takeover(const QString &owner)
{
    const QString who = owner.isEmpty() ? QString("another VMS") : owner;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle("Broadcast in progress");
    box.setText(QString("%1 is broadcasting.").arg(who));
    box.setInformativeText(
        "Do you want to end that broadcast and take over the "
        "broadcast right?");
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    // 기본 버튼은 취소다. 엔터를 습관적으로 눌러 남의 방송이 끊기면 안 된다.
    box.setDefaultButton(QMessageBox::Cancel);
    return box.exec() == QMessageBox::Yes;
}

void BroadcastControlRow::set_status_color(const QColor *slot)
{
    // 색을 QColor 값으로 기억하면 테마가 바뀌어도 옛 색이 굳는다. 어느
    // 팔레트 슬롯이었는지만 들고 있다가 그때그때 다시 읽는다.
    m_status_color = slot;
    if (m_status_color)
        m_status->setStyleSheet(QString("color:%1;").arg(m_status_color->name()));
}

void BroadcastControlRow::sync_ui(bool active)
{
    // RTP 는 UDP 직결이라 브로커 상태와 무관하다 — 여기서 MqttLink 를 보지 않는다.
    // 권한(§5)은 방송 상태와 AND 로 묶는다. 밖에서 따로 걸면 이 함수가 다음
    // 상태 변화 때 도로 켜 버린다.
    const bool may = Auth::can(Auth::Action::Broadcast);
    // 다른 VMS 가 방송 중이어도 ON 은 열어 둔다 — 누르면 인수 확인창이
    // 뜬다. 잠가 버리면 왜 못 누르는지 화면에 아무 설명이 없다.
    m_on->setEnabled(!active && may);
    m_off->setEnabled(active && may);
    // 음량도 방송 권한과 함께 잠근다 — 방송을 못 거는 사람이 남의 방송
    // 음량만 바꿀 수 있으면 안 된다.
    m_volume->setEnabled(may);
    if (!may) {
        const QString why = Auth::deny_reason(Auth::Action::Broadcast);
        m_on->setToolTip(why);
        m_off->setToolTip(why);
    } else if (!active && m_broadcast->other_broadcasting()) {
        const QString who = m_broadcast->current_owner();
        const QString tip =
            QString("%1 is broadcasting. Pressing ON asks whether to take "
                    "over.").arg(who.isEmpty() ? QString("Another VMS") : who);
        m_on->setToolTip(tip);
        m_off->setToolTip(QString());
        m_status->setText(QString("in use by %1")
                              .arg(who.isEmpty() ? QString("another VMS") : who));
        set_status_color(&Theme::textDim);
    } else {
        m_on->setToolTip(QString());
        m_off->setToolTip(QString());
    }
}

void BroadcastControlRow::set_level(double rms_db)
{
    // RMS dBFS를 0..100으로. -50dB(거의 무음) ~ 0dB(풀스케일) 구간만 본다 —
    // 음성 방송은 대개 -30 ~ -6dB 사이에서 논다.
    constexpr double kFloorDb = -50.0;
    const double clamped = rms_db < kFloorDb ? kFloorDb
                         : (rms_db > 0.0 ? 0.0 : rms_db);
    m_level->setValue(int((1.0 - clamped / kFloorDb) * 100.0));
}

