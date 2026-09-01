#include "rpi_alert_popup.h"
#include "alert_popup_stack.h"
#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

namespace {

const int PULSE_MS = 550;      // AlertPopup·FireAlertPopup과 동일
const int POPUP_W = 420;

QString hex(const QColor &c) { return c.name(); }

} // namespace

RpiAlertPopup::RpiAlertPopup(DeviceControlPage *device, QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint
                          | Qt::WindowStaysOnTopHint)
{
    setObjectName("RpiAlertPopup");
    setAttribute(Qt::WA_StyledBackground);
    setFixedWidth(POPUP_W);

    // device_control_page.cpp의 노드 스트립 라벨과 맞춘다 — 같은 노드를
    // 가리키는데 화면마다 이름이 다르면 안 된다.
    m_nodes[0].label = "RPI-A · environment sensors";
    m_nodes[1].label = "RPI-B · MQTT broker";
    m_nodes[2].label = "RPI-C / STM32 · actuators";

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    m_title = new QLabel(this);
    m_title->setFont(Theme::ui_font(15, 700, 0.08));
    Theme::restyle(m_title, [] {
        return QString("color:%1;").arg(hex(Theme::alarm));
    });
    m_title->setWordWrap(true);
    head->addWidget(m_title, 1);
    head->addStretch(1);
    m_time = new QLabel(this);
    m_time->setFont(Theme::mono_font(10));
    Theme::restyle(m_time, [] {
        return QString("color:%1;").arg(hex(Theme::textDim));
    });
    head->addWidget(m_time);
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

    m_btn_device = new QPushButton("Open Device Control", this);
    m_btn_device->setObjectName("OutlineBtn");
    m_btn_device->setFont(Theme::ui_font(11, 600, 0.06));
    m_btn_device->setFixedHeight(28);
    m_btn_device->setCursor(Qt::PointingHandCursor);
    connect(m_btn_device, &QPushButton::clicked, this, [this] {
        emit goto_device_control_requested();
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
    root->addLayout(btns);

    m_pulse = new QTimer(this);
    m_pulse->setInterval(PULSE_MS);
    connect(m_pulse, &QTimer::timeout, this, [this] {
        m_pulse_on = !m_pulse_on;
        refresh();
    });

    if (device)
        connect(device, &DeviceControlPage::node_state_changed,
                this, &RpiAlertPopup::on_node_changed);

    // ⚠ refresh()를 걸면 안 된다 — FireAlertPopup과 같은 이유
    // (표시 상태 자체를 바꾸는 함수라 테마 전환의 부수효과가 되면 안 된다).
    Theme::on_theme_changed(this, [this] { apply_frame(); });

    AlertPopupStack::instance()->add(this, AlertPopupStack::Device);

    hide();
}

void RpiAlertPopup::on_node_changed(DeviceControlPage::Node node, bool online)
{
    NodeSlot &n = m_nodes[int(node)];
    n.offline = !online;
    // 확정된(끊긴) 전이에서만 pending을 켠다 — 복구 전이는 절대 안 켠다.
    // 그래야 "끊긴 적이 있었다"는 사실이 확인 전까지 화면에 남는다
    // (FireAlertPopup의 m_fire_pending과 같은 규칙).
    if (n.offline) {
        n.pending = true;
        n.since = QDateTime::currentDateTime();
    }
    refresh();
}

/** @brief 창틀만 다시 굽는다 — apply_frame 주석은 FireAlertPopup과 동일 */
void RpiAlertPopup::apply_frame()
{
    const QColor edge = m_pulse_on ? Theme::alarm : Theme::alarm.darker(230);
    setStyleSheet(QString("#RpiAlertPopup { background:%1; border:2px solid %2;"
                          "border-radius:4px; }")
                      .arg(hex(Theme::panel), hex(edge)));
}

void RpiAlertPopup::refresh()
{
    QStringList lines;
    for (const NodeSlot &n : m_nodes) {
        if (!n.pending)
            continue;
        const QString state = n.offline ? QString("offline")
                                        : QString("recovered (needs a look)");
        lines << QString("%1 — %2 (since %3)")
                     .arg(n.label, state, n.since.toString("HH:mm:ss"));
    }

    if (lines.isEmpty()) {
        m_pulse->stop();
        hide();
        return;
    }

    m_title->setText(lines.size() == 1
        ? QString("⚠ RPi link lost")
        : QString("⚠ RPi link lost (%1 nodes)").arg(lines.size()));
    m_body->setText(lines.join("\n"));
    m_time->setText(QDateTime::currentDateTime().toString("HH:mm:ss"));

    apply_frame();
    adjustSize();
    setFixedWidth(POPUP_W);

    if (!isVisible()) {
        reposition();
        // 로그인 전이면 뜨지 않는다(보류). 미확인 상태는 그대로 남아
        // 로그인 시 그대로 뜬다 — AlertPopupStack::try_show 주석 참조.
        AlertPopupStack::instance()->try_show(this);
    }
    if (!m_pulse->isActive()) {
        m_pulse_on = true;
        m_pulse->start();
    }
}

void RpiAlertPopup::dismiss_current()
{
    // 화재/버튼과 달리 셋이 배타적이지 않아 "지금 보이는 쪽 하나만"이 없다 —
    // 지금 목록에 떠 있는 노드를 전부 확인 처리한다.
    for (NodeSlot &n : m_nodes)
        n.pending = false;
    refresh();
}

void RpiAlertPopup::reposition()
{
    AlertPopupStack::instance()->relayout();
}
