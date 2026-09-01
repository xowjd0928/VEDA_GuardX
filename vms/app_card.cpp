#include "app_card.h"
#include "auth.h"
#include "theme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace {

constexpr qint64 PENDING_GIVEUP_MS = 20000;  ///< 전이 확정 대기 상한

/** @brief "8h 3m" — uptime_s 표시용 */
QString fmt_uptime(qint64 s)
{
    if (s < 0)
        return QStringLiteral("—");
    if (s < 3600)
        return QString("%1m").arg(s / 60);
    return QString("%1h %2m").arg(s / 3600).arg((s % 3600) / 60);
}

} // namespace

AppCard::AppCard(const QString &app_id, QWidget *parent)
    : QFrame(parent), m_id(app_id)
{
    Theme::restyle(this, [] {
        return QString("AppCard { background:%1; border:1px solid %2;"
                       " border-radius:3px; }")
            .arg(Theme::panel.name(), Theme::border.name());
    });
    setFixedHeight(66);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 14, 0);
    row->setSpacing(12);

    // 좌측 4px 상태 바 — LIVE 타일 경보 테두리와 같은 언어
    m_strip = new QFrame(this);
    m_strip->setFixedWidth(4);
    row->addWidget(m_strip);

    auto *col = new QVBoxLayout;
    col->setContentsMargins(0, 10, 0, 10);
    col->setSpacing(4);

    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    m_dot = new QLabel(QString::fromUtf8("●"), this);
    m_dot->setFont(Theme::ui_font(9));
    top->addWidget(m_dot);
    m_name = new QLabel(this);
    m_name->setFont(Theme::ui_font(13, 700));
    top->addWidget(m_name);
    m_status = new QLabel(this);
    m_status->setFont(Theme::mono_font(11));
    top->addWidget(m_status);
    top->addStretch(1);
    col->addLayout(top);

    m_res = new QLabel(this);
    m_res->setFont(Theme::mono_font(10));
    Theme::restyle(m_res, [=] {
        return QString("color:%1; border:none; background:transparent;")
                             .arg(Theme::textMuted.name());
    });
    col->addWidget(m_res);
    row->addLayout(col, 1);

    // ---- AutoStart 토글 + Priority (5단계) ----
    m_btn_autostart = new QPushButton(this);
    m_btn_autostart->setFixedSize(Theme::px(96), Theme::px(24));  // 글자와 같은 배율
    m_btn_autostart->setFont(Theme::mono_font(10));
    m_btn_autostart->setCursor(Qt::PointingHandCursor);
    connect(m_btn_autostart, &QPushButton::clicked, this,
            [this] { emit autostart_requested(m_id, !m_info.auto_start); });
    row->addWidget(m_btn_autostart);

    m_priority = new QComboBox(this);
    m_priority->addItems({"Low", "Medium", "High"});
    m_priority->setFixedSize(Theme::px(78), Theme::px(24));
    m_priority->setFont(Theme::mono_font(10));
    m_priority->setCursor(Qt::PointingHandCursor);
    Theme::restyle(m_priority, [=] {
        return QString("QComboBox { background:%1; border:1px solid %2; border-radius:2px;"
                " color:%3; padding-left:8px; }"
                "QComboBox::drop-down { border:none; width:16px; }"
                "QComboBox QAbstractItemView { background:%1; color:%3;"
                " border:1px solid %2; selection-background-color:%4; }")
            .arg(Theme::elevated.name(), Theme::border2.name(),
                 Theme::textMid.name(), Theme::elevated2.name());
    });
    connect(m_priority, &QComboBox::textActivated, this,
            [this](const QString &p) {
                if (p != m_info.priority)
                    emit priority_requested(m_id, p);
            });
    row->addWidget(m_priority);

    // c = 팔레트 슬롯 포인터 (값이면 테마 전환 때 옛 색이 굳는다)
    auto make_btn = [this](const QString &text, const QColor *c) {
        auto *b = new QPushButton(text, this);
        b->setFixedSize(Theme::px(64), Theme::px(28));
        b->setFont(Theme::ui_font(11, 600));
        b->setCursor(Qt::PointingHandCursor);
        Theme::restyle(b, [=] {
            return QString("QPushButton { background:transparent; border:1px solid %1;"
                    " border-radius:2px; color:%1; }"
                    "QPushButton:hover { background:%2; }"
                    "QPushButton:disabled { border-color:%3; color:%3; }")
                .arg(c->name(), Theme::elevated2.name(), Theme::textFaint.name());
        });
        return b;
    };
    m_btn_start = make_btn("▶ Start", &Theme::green);
    m_btn_stop = make_btn("■ Stop", &Theme::alarm);
    row->addWidget(m_btn_start);
    row->addWidget(m_btn_stop);

    connect(m_btn_start, &QPushButton::clicked, this,
            [this] { emit start_requested(m_id); });
    connect(m_btn_stop, &QPushButton::clicked, this,
            [this] { emit stop_requested(m_id); });

    // 권한이 바뀌면 버튼 활성을 다시 계산한다. render() 는 원래 폴마다
    // 불리는 함수라 여기에 걸어도 새로 쌓이는 것이 없다.
    connect(Auth::instance(), &Auth::state_changed, this, [this] { render(); });
    connect(Auth::instance(), &Auth::verification_changed, this,
            [this] { render(); });

    // Running 점 호흡 — 2.4초 주기로 알파가 오르내린다 (반짝임 아님)
    m_breath = new QVariantAnimation(this);
    m_breath->setDuration(2400);
    m_breath->setStartValue(0.35);
    m_breath->setKeyValueAt(0.5, 1.0);
    m_breath->setEndValue(0.35);
    m_breath->setLoopCount(-1);
    connect(m_breath, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &v) {
                m_breath_phase = v.toDouble();
                QColor c = Theme::green;
                c.setAlphaF(m_breath_phase);
                m_dot->setStyleSheet(
                    QString("color:%1; border:none; background:transparent;")
                        .arg(c.name(QColor::HexArgb)));
            });

    render();

    // 상태색·자원 줄은 HTML에 색을 구워 넣는다 — 테마가 바뀌면 다시 그린다
    Theme::on_theme_changed(this, [this] { render(); });
}

void AppCard::set_info(const CameraAppInfo &info)
{
    m_info = info;

    if (m_pending) {
        const bool reached = m_pending_start
                                 ? info.status == QLatin1String("Running")
                                 : info.status == QLatin1String("Stopped");
        if (reached || m_pending_since.elapsed() > PENDING_GIVEUP_MS)
            m_pending = false;  // 확정(또는 포기 — 폴이 진실을 계속 말해준다)
    }
    render();
}

void AppCard::begin_pending(bool start)
{
    m_pending = true;
    m_pending_start = start;
    m_pending_since.start();
    render();
}

void AppCard::cancel_pending()
{
    m_pending = false;
    render();
}

void AppCard::render()
{
    const bool running = m_info.status == QLatin1String("Running");
    const bool stopped = m_info.status == QLatin1String("Stopped");

    // 상태색: 정지=alarm · 전이류(-ing)=amber · Running=green
    QColor state_c = Theme::amber;
    if (m_pending)
        state_c = Theme::amber;
    else if (running)
        state_c = Theme::green;
    else if (stopped)
        state_c = Theme::alarm;
    m_strip->setStyleSheet(QString("background:%1; border:none;").arg(state_c.name()));

    // 이름 + 버전 + 기본앱 칩
    QString name = QString("<span style=\"color:%1\">%2</span>")
                       .arg(Theme::textHi.name(), m_id);
    if (!m_info.version.isEmpty())
        name += QString(" <span style=\"color:%1;font-size:%2px\">%3</span>")
                    .arg(Theme::textDim.name())
                    .arg(Theme::px(10))
                    .arg(m_info.version);
    if (m_info.is_default)
        name += QString(" <span style=\"color:%1;font-size:%2px\">· default app</span>")
                    .arg(Theme::accent.name())
                    .arg(Theme::px(10));
    m_name->setText(name);

    // 상태 문구 — 낙관적 전이 중엔 방향을 말한다
    QString status_text = m_info.status;
    if (m_pending)
        status_text = m_pending_start ? QString("Starting...")
                                      : QString("Stopping...");
    m_status->setText(QString("<span style=\"color:%1\">%2</span>")
                          .arg(state_c.name(), status_text));

    // 상태 점 — Running이면 호흡, 아니면 정색
    if (running && !m_pending) {
        if (m_breath->state() != QVariantAnimation::Running)
            m_breath->start();
    } else {
        m_breath->stop();
        m_dot->setStyleSheet(QString("color:%1; border:none; background:transparent;")
                                 .arg(state_c.name()));
    }

    // 앱별 자원 라인 (appstatus 1초) — 정지 앱은 자원이 없다
    QStringList parts;
    if (m_info.cpu >= 0) {
        parts << QString("CPU %1").arg(m_info.cpu)
              << QString("MEM %1").arg(m_info.mem)
              << QString("NPU %1").arg(m_info.npu)
              << QString("RAM %1MB").arg(qRound(m_info.ram_mb))
              << QString("%1thr").arg(m_info.threads)
              << fmt_uptime(m_info.uptime_s);
    } else {
        parts << QString("resources —");
    }
    m_res->setText(parts.join(QString::fromUtf8(" · ")));

    // AutoStart 토글 — 꺼짐은 amber로 경고한다 (정전 시 안 뜸, 08-05 구멍)
    {
        const bool on = m_info.auto_start;
        const QColor c = on ? Theme::green : Theme::amber;
        m_btn_autostart->setText(
            on ? QString::fromUtf8("AutoStart On")
               : QString::fromUtf8("⚠ AutoStart Off"));
        m_btn_autostart->setStyleSheet(
            QString("QPushButton { background:transparent; border:1px solid %1;"
                    " border-radius:2px; color:%1; }"
                    "QPushButton:hover { background:%2; }"
                    "QPushButton:disabled { border-color:%3; color:%3; }")
                .arg(c.name(), Theme::elevated2.name(), Theme::textFaint.name()));
        m_btn_autostart->setToolTip(
            on ? QString("click: turn AutoStart off")
               : QString("off - this app will not come back after a camera "
                         "reboot or a power cut.\nclick: turn AutoStart on"));
        const QSignalBlocker block(m_priority);
        const int idx = m_priority->findText(m_info.priority);
        if (idx >= 0)
            m_priority->setCurrentIndex(idx);
    }

    // 버튼 활성: 전이 중엔 잠금. 기본앱·ControlForbidden은 정지 불가.
    // 권한(§5)은 여기에 **AND** 로 들어간다 — 이 함수가 상태마다 다시 불리므로
    // 밖에서 setEnabled 를 걸면 다음 폴에서 도로 켜진다.
    const bool forbidden = !m_info.control_forbidden.isEmpty();
    const bool may = Auth::can(Auth::Action::CameraApps);
    m_btn_autostart->setEnabled(!m_pending && !forbidden && may);
    m_priority->setEnabled(!m_pending && !forbidden && may);
    m_btn_start->setVisible(!running || m_pending);
    m_btn_stop->setVisible(running || m_pending);
    m_btn_start->setEnabled(!m_pending && !forbidden && may);
    m_btn_stop->setEnabled(!m_pending && !forbidden && !m_info.is_default && may);
    if (!may) {
        const QString why = Auth::deny_reason(Auth::Action::CameraApps);
        for (QWidget *w : { (QWidget *)m_btn_start, (QWidget *)m_btn_stop,
                            (QWidget *)m_btn_autostart, (QWidget *)m_priority })
            w->setToolTip(why);
    }
    if (m_info.is_default)
        m_btn_stop->setToolTip("default app - cannot be stopped");
    else if (forbidden)
        m_btn_stop->setToolTip(
            QString("The camera forbids this action (ControlForbidden: %1)")
                .arg(m_info.control_forbidden));
    else
        m_btn_stop->setToolTip(QString());
    m_btn_start->setToolTip(m_btn_stop->toolTip());
}
