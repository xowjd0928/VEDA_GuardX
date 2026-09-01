#include "login_page.h"
#include "auth.h"
#include "credentials.h"
#include "mqtt_link.h"
#include "site_config.h"
#include "theme.h"

#include <QEvent>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/** @brief 카드 폭 — 글자를 담는 고정 크기라 전역 배율을 탄다(테마 §8) */
int card_width() { return Theme::px(360); }

// ---- 마스코트 치수 (배치 기획 §3·§5) ---------------------------------------
// ⚠ Theme::px 배율을 태우지 않는다. 그 배율은 **글자를 담는** 고정 크기에만
// 거는 것이고(테마 §8), 이건 그림이다 — 글자를 키운다고 개가 커질 이유가 없다.
constexpr int MASCOT_SPLASH_W = 160;   ///< 세션 확인 중 — 전신이 나오는 유일한 자리
constexpr int MASCOT_AVATAR_D = 72;    ///< 카드 브랜드 줄의 얼굴 아바타
constexpr int MASCOT_TEXT_GAP = 12;    ///< 글자와의 최소 거리 (§5)

/** @brief 입력 라벨 (아이디 / 비밀번호) */
QLabel *field_label(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setFont(Theme::ui_font(12, 500));
    Theme::restyle(l, [] { return QString("color:%1;").arg(Theme::textMuted.name()); });
    return l;
}

} // namespace

LoginPage::LoginPage(QWidget *parent)
    : QWidget(parent)
{
    // ---- 카드 -------------------------------------------------------------
    auto *card = new QFrame(this);
    card->setObjectName("LoginCard");
    card->setFixedWidth(card_width());

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(28, 26, 28, 22);
    lay->setSpacing(0);

    // 도담 아바타 + (로고 + 제품명 / 현장 문구) 를 한 덩어리로 가운데 놓는다.
    // 전신은 카드 옆에 세우지 않는다 — 창을 좁히면 사라지는 자리라 브랜드가
    // 붙어 있을 곳이 못 된다(08-13 결정). 전신은 세션 확인 화면에만 남겼다.
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(12);
    header->addStretch(1);

    m_avatar = new QLabel(card);
    m_avatar->setFixedSize(MASCOT_AVATAR_D, MASCOT_AVATAR_D);
    const auto paint_avatar = [this] {
        // 배경 원 색이 테마를 타므로 테마가 바뀌면 다시 받아 온다.
        const QPixmap pm = Theme::mascot_avatar(MASCOT_AVATAR_D);
        m_avatar->setVisible(!pm.isNull());   // 자산이 없으면 자리째 빠진다
        m_avatar->setPixmap(pm);
    };
    paint_avatar();
    Theme::on_theme_changed(m_avatar, paint_avatar);
    header->addWidget(m_avatar);

    auto *brand_col = new QVBoxLayout;
    brand_col->setContentsMargins(0, 0, 0, 0);
    brand_col->setSpacing(2);

    // 로고 + 제품명 — 상단바와 같은 것. #Logo 는 전역 QSS 라 테마를 저절로 탄다.
    auto *brand = new QHBoxLayout;
    brand->setContentsMargins(0, 0, 0, 0);
    brand->setSpacing(10);

    auto *logo = new QLabel("G", card);
    logo->setObjectName("Logo");
    logo->setFixedSize(26, 26);
    logo->setAlignment(Qt::AlignCenter);
    logo->setFont(Theme::ui_font(15, 700));
    brand->addWidget(logo);

    auto *title = new QLabel(card);
    title->setFont(Theme::ui_font(16, 700, 0.14));
    auto paint_title = [title] {
        // 크롬이 없는 화면이라 본문 팔레트를 쓴다(§6c). 색은 명시한다 —
        // 안 주면 테마에 따라 사라진다(테마 §6b 와 같은 함정).
        title->setText(QString("<span style=\"color:%1\">GuardX</span> "
                               "<span style=\"color:%2;font-weight:500;"
                               "font-size:%3px\">VMS 1.0</span>")
                           .arg(Theme::textHi.name(), Theme::textMuted.name())
                           .arg(Theme::px(12)));
    };
    paint_title();
    Theme::on_theme_changed(title, paint_title);
    brand->addWidget(title);
    brand->addStretch(1);
    brand_col->addLayout(brand);

    // 현장 문구는 전역 설정에서 온다 (08-12, 7번). 로그인 화면은 브로커 연결
    // 전에 그려지므로 SiteConfig 가 캐시/기본값으로 항상 뭔가를 돌려준다.
    auto *site = new QLabel(SiteConfig::instance()->site_name(), card);
    site->setFont(Theme::mono_font(10, 500, 0.14));
    site->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    Theme::restyle(site, [] { return QString("color:%1;").arg(Theme::textDim.name()); });
    connect(SiteConfig::instance(), &SiteConfig::site_name_changed, site,
            [site] { site->setText(SiteConfig::instance()->site_name()); });
    brand_col->addWidget(site);

    header->addLayout(brand_col);
    header->addStretch(1);
    lay->addLayout(header);

    // ---- 스텁 배지 ---------------------------------------------------------
    // 서버 검증 없이 들어가고 있다는 사실은 **화면에서 보여야 한다.** 로그에만
    // 두면 그 로그를 안 보는 사람에게는 없는 것과 같고, 그대로 시연·배포되는
    // 것이 이 스위치의 유일한 위험이다.
    // 스텁이 아닐 때는 숨기지 않고 **아예 만들지 않는다** — 숨긴 위젯은
    // 언젠가 잘못 보이지만, 없는 위젯은 그럴 수 없다.
    if (Auth::stub_enabled()) {
        lay->addSpacing(12);
        auto *row_badge = new QHBoxLayout;
        row_badge->setContentsMargins(0, 0, 0, 0);
        row_badge->addStretch(1);

        auto *badge = new QLabel(
            Auth::stub_offline() ? "Dev Stub · Offline Simulation"
                                 : "Dev Stub · No Server Check", card);
        badge->setObjectName("Pill");          // 본문 칩 (전역 QSS, 테마 §9)
        badge->setFont(Theme::mono_font(10, 500, 0.1));
        badge->setFixedHeight(Theme::px(20));
        badge->setAlignment(Qt::AlignCenter);
        // 면은 #Pill 그대로 두고 글자·테두리만 amber 로 덮는다. 새 색을
        // 만들지 않고, 팔레트가 바뀌면 이 칩도 따라온다.
        Theme::restyle(badge, [] {
            return QString("color:%1; border-color:%1;").arg(Theme::amber.name());
        });
        row_badge->addWidget(badge);
        row_badge->addStretch(1);
        lay->addLayout(row_badge);
    }

    lay->addSpacing(24);

    // ---- 입력 -------------------------------------------------------------
    lay->addWidget(field_label("Username", card));
    lay->addSpacing(6);
    m_user = new QLineEdit(card);
    m_user->setFont(Theme::ui_font(13));
    m_user->setMinimumHeight(Theme::px(30));
    m_user->setMaxLength(64);
    lay->addWidget(m_user);

    lay->addSpacing(14);

    lay->addWidget(field_label("Password", card));
    lay->addSpacing(6);
    m_password = new QLineEdit(card);
    m_password->setFont(Theme::ui_font(13));
    m_password->setMinimumHeight(Theme::px(30));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setMaxLength(128);
    lay->addWidget(m_password);

    lay->addSpacing(20);

    // ---- 로그인 버튼 -------------------------------------------------------
    m_submit = new QPushButton("Sign in", card);
    m_submit->setObjectName("PrimaryBtn");
    m_submit->setFont(Theme::ui_font(13, 600, 0.06));
    m_submit->setMinimumHeight(Theme::px(34));
    m_submit->setCursor(Qt::PointingHandCursor);
    m_submit->setDefault(true);
    lay->addWidget(m_submit);

    lay->addSpacing(10);

    // 실패 문구 자리 — 비어 있어도 자리를 차지한다. 실패할 때만 나타나게
    // 하면 카드 높이가 튀어 버튼이 커서 밑에서 움직인다.
    m_error = new QLabel(card);
    m_error->setFont(Theme::ui_font(11.5));
    m_error->setWordWrap(true);
    m_error->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_error->setMinimumHeight(Theme::px(32));
    lay->addWidget(m_error);
    // 색을 문구와 함께 다시 만든다 — 사유를 기억하고 있으므로 문장은 보존된다.
    Theme::on_theme_changed(m_error, [this] { render_error(); });
    render_error();

    lay->addSpacing(4);

    // ---- 서버 상태 ---------------------------------------------------------
    // 브로커가 끊겨 있으면 사용자가 비밀번호를 의심하며 세 번 치기 전에
    // 원인을 말해준다(§6a).
    m_server = new QLabel(card);
    m_server->setFont(Theme::mono_font(10));
    m_server->setAlignment(Qt::AlignCenter);
    m_server->setWordWrap(true);
    // 상태를 그때그때 읽어 다시 만든다 — 문구를 저장해 두면 테마 전환에서
    // 옛 문장이 되살아난다(테마 정본 §10-3).
    Theme::on_theme_changed(m_server, [this] { render_server(); });
    render_server();
    lay->addWidget(m_server);

    // ---- 스플래시 ----------------------------------------------------------
    // 저장된 세션을 검증하는 동안 보여준다. 폼을 먼저 그리면 자동 로그인이
    // 성공하는 흔한 경우에도 로그인 화면이 한 번 **깜빡인다**(§6a).
    //
    // 도담 전신이 나오는 **유일한 자리**다(08-13 결정). 이 화면은 폼도 카드도
    // 없이 한 줄만 떠 있어 원래 가장 비어 보이던 자리고, 최대 6초 기다린다.
    m_splash_box = new QWidget(this);
    auto *splash_lay = new QVBoxLayout(m_splash_box);
    splash_lay->setContentsMargins(0, 0, 0, 0);
    splash_lay->setSpacing(MASCOT_TEXT_GAP);

    m_splash_mascot = new QLabel(m_splash_box);
    m_splash_mascot->setAlignment(Qt::AlignCenter);
    const auto paint_splash_mascot = [this] {
        const QPixmap pm = Theme::mascot(MASCOT_SPLASH_W);
        // 자산이 없으면 그림 없이 문구만 — 로그인이 마스코트에 걸리면 안 된다.
        m_splash_mascot->setVisible(!pm.isNull());
        m_splash_mascot->setPixmap(pm);
        // 라이트 테마에서만 0.85 (§5). 그림 자체는 건드리지 않는다.
        m_splash_fade->setOpacity(Theme::mascot_opacity());
    };
    m_splash_fade = new QGraphicsOpacityEffect(m_splash_mascot);
    m_splash_mascot->setGraphicsEffect(m_splash_fade);
    paint_splash_mascot();
    Theme::on_theme_changed(m_splash_mascot, paint_splash_mascot);
    splash_lay->addWidget(m_splash_mascot);

    m_splash = new QLabel("Checking session...", m_splash_box);
    m_splash->setFont(Theme::ui_font(13));
    m_splash->setAlignment(Qt::AlignCenter);
    Theme::restyle(m_splash,
                   [] { return QString("color:%1;").arg(Theme::textDim.name()); });
    splash_lay->addWidget(m_splash);

    // ---- 배치 -------------------------------------------------------------
    // 카드는 화면 한가운데 그대로 둔다 — 경보 팝업은 로그인 전에는 아예
    // 뜨지 않으므로(AlertPopupStack::set_gated) 비켜 줄 상대가 없다.
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->addStretch(1);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(card);
    build_change_card();
    row->addWidget(m_change_card);
    row->addWidget(m_splash_box);
    row->addStretch(1);
    outer->addLayout(row);
    outer->addStretch(1);

    m_card = card;

    show_splash(Auth::instance()->resuming());

    // ---- 배선 -------------------------------------------------------------
    connect(m_submit, &QPushButton::clicked, this, &LoginPage::submit);
    // Enter 제출 — 두 칸 어디서 눌러도 같다(§6a)
    connect(m_user, &QLineEdit::returnPressed, this, &LoginPage::submit);
    connect(m_password, &QLineEdit::returnPressed, this, &LoginPage::submit);

    // 잠금 카운트다운 — 서버가 준 남은 초를 1초씩 깎는다
    m_lock_timer = new QTimer(this);
    m_lock_timer->setInterval(1000);
    connect(m_lock_timer, &QTimer::timeout, this, &LoginPage::tick_lock);

    Auth *auth = Auth::instance();
    connect(auth, &Auth::state_changed, this, [this](Auth::State s) {
        // 자동 로그인 검증이 끝나면(성공이든 실패든) 폼으로 돌아온다.
        show_splash(Auth::instance()->resuming() && s == Auth::State::Checking);
        // §5b — 강제 변경이면 로그인 폼 대신 변경 폼을 세운다.
        if (s == Auth::State::MustChangePassword)
            begin_change(true);
        update_submit_state();
        if (s == Auth::State::LoggedIn) {
            m_lock_timer->stop();
            m_password->clear();
            m_error_reason.clear();
            m_error_retry_s = 0;
            render_error();
            emit authenticated();
        }
    });
    connect(auth, &Auth::login_failed, this,
            [this](const QString &reason, int retry_after_s) {
        m_error_reason = reason;
        // 잠금일 때만 카운트다운을 돈다. 서버 판정을 그대로 표시하는 것이라
        // 여기서 남은 초를 지어내지 않는다 — 안 실려 오면 숫자 없이 둔다.
        m_error_retry_s = (reason == QLatin1String(Auth::REASON_LOCKED))
                              ? qMax(0, retry_after_s) : 0;
        if (m_error_retry_s > 0)
            m_lock_timer->start();
        else
            m_lock_timer->stop();

        render_error();
        update_submit_state();
        // 아이디는 남기고 비밀번호만 지운다 — 다시 치는 것은 비밀번호다.
        m_password->clear();
        m_password->setFocus();
    });

    connect(auth, &Auth::password_changed, this, [this](bool ok, const QString &reason) {
        m_change_reason = ok ? QString() : reason;
        render_change_error();
        m_change_btn->setEnabled(true);
        if (ok) {
            m_change_card->hide();
            m_card->show();
            // 강제였다면 Auth 가 LoggedIn 으로 바꾸며 MainWindow 가 셸로 넘긴다.
            // 자발적이었다면 상태가 그대로라 여기서 알려 줘야 돌아간다.
            if (!m_change_forced)
                emit change_finished();
        }
    });

    // 브로커 상태는 상태줄과 버튼 둘 다에 영향을 준다
    connect(MqttLink::instance(), &MqttLink::online_changed, this, [this](bool) {
        render_server();
        update_submit_state();
    });
    // 설정 오류(인증서 없음 등)는 online_changed 가 영영 안 오는 상태라
    // 따로 듣는다 — 안 들으면 "서버 확인 중"이라는 거짓말이 화면에 남는다.
    connect(MqttLink::instance(), &MqttLink::fault_changed, this, [this] {
        render_server();
        update_submit_state();
    });

    update_submit_state();
}

void LoginPage::show_splash(bool splash)
{
    if (!m_card || !m_splash_box)
        return;
    // 강제 변경 중이면 이 함수가 로그인 카드를 되살리면 안 된다 — 그러면
    // 바꾸지 않고도 폼이 다시 보인다.
    if (m_change_card && m_change_card->isVisible())
        return;
    m_card->setVisible(!splash);
    m_splash_box->setVisible(splash);
}

void LoginPage::build_change_card()
{
    m_change_card = new QFrame(this);
    m_change_card->setObjectName("LoginCard");
    m_change_card->setFixedWidth(card_width());
    m_change_card->hide();

    auto *lay = new QVBoxLayout(m_change_card);
    lay->setContentsMargins(28, 26, 28, 22);
    lay->setSpacing(0);

    auto *title = new QLabel("Change password", m_change_card);
    title->setFont(Theme::ui_font(16, 700, 0.06));
    title->setAlignment(Qt::AlignCenter);
    Theme::restyle(title, [] { return QString("color:%1;").arg(Theme::textHi.name()); });
    lay->addWidget(title);
    lay->addSpacing(6);

    m_change_why = new QLabel(
        "You must change your password to continue", m_change_card);
    m_change_why->setFont(Theme::ui_font(11.5));
    m_change_why->setAlignment(Qt::AlignCenter);
    m_change_why->setWordWrap(true);
    Theme::restyle(m_change_why,
                   [] { return QString("color:%1;").arg(Theme::amber.name()); });
    lay->addWidget(m_change_why);
    lay->addSpacing(20);

    const auto add_field = [&](const QString &label, QLineEdit **out) {
        lay->addWidget(field_label(label, m_change_card));
        lay->addSpacing(6);
        *out = new QLineEdit(m_change_card);
        (*out)->setEchoMode(QLineEdit::Password);
        (*out)->setFont(Theme::ui_font(13));
        (*out)->setMinimumHeight(Theme::px(30));
        (*out)->setMaxLength(128);
        lay->addWidget(*out);
        lay->addSpacing(12);
    };
    add_field("Current password", &m_cur_pw);
    add_field("New password", &m_new_pw);
    add_field("Confirm new password", &m_new_pw2);

    m_change_btn = new QPushButton("Change", m_change_card);
    m_change_btn->setObjectName("PrimaryBtn");
    m_change_btn->setFont(Theme::ui_font(13, 600, 0.06));
    m_change_btn->setMinimumHeight(Theme::px(34));
    m_change_btn->setCursor(Qt::PointingHandCursor);
    lay->addWidget(m_change_btn);
    lay->addSpacing(8);

    // 자발적 변경일 때만 보인다 — 강제 변경에는 빠져나갈 문이 없어야 한다.
    m_change_cancel = new QPushButton("Cancel", m_change_card);
    m_change_cancel->setObjectName("OutlineBtn");
    m_change_cancel->setFont(Theme::ui_font(12));
    m_change_cancel->setMinimumHeight(Theme::px(30));
    m_change_cancel->setCursor(Qt::PointingHandCursor);
    lay->addWidget(m_change_cancel);
    lay->addSpacing(6);
    connect(m_change_cancel, &QPushButton::clicked, this, [this] {
        m_change_card->hide();
        m_card->show();
        emit change_finished();
    });

    m_change_msg = new QLabel(m_change_card);
    m_change_msg->setFont(Theme::ui_font(11.5));
    m_change_msg->setWordWrap(true);
    m_change_msg->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_change_msg->setMinimumHeight(Theme::px(32));
    lay->addWidget(m_change_msg);
    Theme::on_theme_changed(m_change_msg, [this] { render_change_error(); });

    connect(m_change_btn, &QPushButton::clicked, this, &LoginPage::submit_change);
    for (QLineEdit *e : { m_cur_pw, m_new_pw, m_new_pw2 })
        connect(e, &QLineEdit::returnPressed, this, &LoginPage::submit_change);
}

void LoginPage::begin_change(bool forced)
{
    m_change_forced = forced;
    m_change_why->setVisible(forced);
    m_change_cancel->setVisible(!forced);
    m_cur_pw->clear();
    m_new_pw->clear();
    m_new_pw2->clear();
    m_change_reason.clear();
    m_change_btn->setEnabled(true);
    render_change_error();
    m_card->hide();
    m_splash_box->hide();
    m_change_card->show();
    m_cur_pw->setFocus();
}

void LoginPage::submit_change()
{
    if (!m_change_btn->isEnabled())
        return;
    // 두 번 입력을 여기서 대조한다 — 서버에 보낼 것도 없는 실수다.
    if (m_new_pw->text() != m_new_pw2->text()) {
        m_change_reason = QStringLiteral("mismatch");
        render_change_error();
        return;
    }
    // 정책 선검사는 없다(08-12 폐지). 두 번 입력 대조만 여기서 한다 — 그건
    // 서버가 알 수 없는 사실(사용자가 같은 것을 두 번 쳤나)이라 화면의 몫이다.
    // ⚠ 어느 쪽도 trimmed() 하지 않는다 — 공백도 비밀번호다.
    m_change_btn->setEnabled(false);
    Auth::instance()->change_password(m_cur_pw->text(), m_new_pw->text());
}

void LoginPage::render_change_error()
{
    QString msg;
    if (m_change_reason == QLatin1String("mismatch"))
        msg = "The new passwords do not match";
    else if (m_change_reason == QLatin1String(Auth::REASON_WEAK_PASSWORD))
        // 정책 폐지 후 서버가 이 사유를 주는 경우는 **빈 값** 하나뿐이다
        msg = "The new password cannot be empty";
    else if (m_change_reason == QLatin1String(Auth::REASON_BAD_CREDENTIALS))
        msg = "Current password is not correct";
    else if (m_change_reason == QLatin1String(Auth::REASON_UNREACHABLE))
        msg = "The server is not responding - try again shortly";
    else if (!m_change_reason.isEmpty())
        msg = QString("Could not change the password (%1)").arg(m_change_reason);

    m_change_msg->setText(msg);
    m_change_msg->setStyleSheet(QString("color:%1;").arg(Theme::alarm.name()));
}

void LoginPage::reset()
{
    show_splash(false);
    m_password->clear();
    m_error_reason.clear();
    m_error_retry_s = 0;
    m_lock_timer->stop();
    render_error();
    render_server();
    update_submit_state();
    if (m_user->text().trimmed().isEmpty())
        m_user->setFocus();
    else
        m_password->setFocus();
}

void LoginPage::submit()
{
    // Enter 는 버튼 비활성을 우회한다 — 그래서 같은 판정을 여기서도 본다.
    // (중복 제출은 Auth 쪽 백스톱이 한 번 더 막는다.)
    if (!m_submit->isEnabled())
        return;
    Auth::instance()->login(m_user->text(), m_password->text());
}

void LoginPage::update_submit_state()
{
    const bool checking = Auth::instance()->state() == Auth::State::Checking;
    const bool locked = m_error_retry_s > 0;
    // 스텁은 브로커 없이 도는 개발 경로다 — 미연결로 막으면 서버가 서기 전에는
    // 아무도 못 들어간다. 실경로일 때만 브로커를 요구한다.
    const bool need_broker = !Auth::stub_enabled();
    const bool offline = need_broker && !MqttLink::instance()->online();

    m_submit->setEnabled(!checking && !locked && !offline);
    // 요청 중에는 스피너 대신 문구를 바꾼다(§6a) — 무엇을 기다리는지 읽힌다.
    m_submit->setText(checking ? QString("Checking...") : QString("Sign in"));
    m_user->setEnabled(!checking);
    m_password->setEnabled(!checking);

    // 왜 못 누르는지를 버튼 자신이 말하게 한다. 상태줄에도 이유가 있지만
    // 사람은 누르려던 것을 먼저 본다.
    if (offline)
        m_submit->setToolTip("Not connected to the server");
    else if (locked)
        m_submit->setToolTip("Wait until the lockout expires");
    else
        m_submit->setToolTip(QString());
}

void LoginPage::tick_lock()
{
    if (m_error_retry_s > 0)
        --m_error_retry_s;

    if (m_error_retry_s <= 0) {
        m_lock_timer->stop();
        // 0초가 되면 문구를 지운다 — "0초 후 다시 시도"는 거짓말이다.
        if (m_error_reason == QLatin1String(Auth::REASON_LOCKED))
            m_error_reason.clear();
    }
    render_error();
    update_submit_state();
}

void LoginPage::render_server()
{
    const QString endpoint = QString("MQTT %1:%2")
                                 .arg(Credentials::mqtt_host())
                                 .arg(Credentials::mqtt_port());

    QString state;
    const QColor *color = &Theme::textDim;
    if (!MqttLink::available()) {
        // 빌드에 libmosquitto 가 없다. "연결 안 됨"으로 뭉뚱그리면 브로커를
        // 붙잡고 몇 시간을 보낸다 — 다른 사건이므로 다르게 말한다.
        state = "MQTT not built into this binary";
        color = &Theme::amber;
    } else if (MqttLink::instance()->online()) {
        state = endpoint + " · connected";
    } else if (!MqttLink::instance()->fault().isEmpty()) {
        // 설정이 틀렸다 — 기다려도 안 붙는다. "확인 중"이라고 하면 사용자가
        // 원인을 못 찾고 로그인만 반복한다(08-12 실사고 — 팀원 전원이 인증서를
        // 두고도 1883 기본값에 막혀 이 화면만 봤다). 사유와 다음 행동을 말한다.
        state = endpoint + " · " + MqttLink::instance()->fault();
        color = &Theme::alarm;   // 재시도 중(amber)과 다른 사건 — 사람이 고쳐야 한다
    } else {
        state = endpoint + " · not connected - checking the server";
        color = &Theme::amber;   // 상태색은 건강 표시 전용 — 여기가 그 자리다
    }

    // "스텁이라 브로커와 무관하게 로그인된다"는 사실은 카드 위 배지가 말한다.
    // 여기에 또 적으면 같은 사실이 두 곳에 생겨, 나중에 한쪽만 고치게 된다.

    m_server->setText(state);
    m_server->setStyleSheet(QString("color:%1;").arg(color->name()));
}

void LoginPage::render_error()
{
    const QString msg = message_for(m_error_reason, m_error_retry_s);
    m_error->setText(msg);
    m_error->setStyleSheet(QString("color:%1;").arg(Theme::alarm.name()));
}

QString LoginPage::message_for(const QString &reason, int retry_after_s)
{
    if (reason.isEmpty())
        return QString();

    // ⭐ 사유는 구분하되 **문구는 합친다**(§2b). "아이디가 없다"와 "비밀번호가
    // 틀렸다"를 구분해 보여주면 계정 존재 여부를 알려주는 꼴이다.
    if (reason == QLatin1String(Auth::REASON_BAD_CREDENTIALS))
        return "Username or password is not correct";
    if (reason == QLatin1String(Auth::REASON_LOCKED))
        return QString("Locked after too many failures - try again in %1 s")
            .arg(retry_after_s > 0 ? retry_after_s : 0);
    if (reason == QLatin1String(Auth::REASON_DISABLED))
        return "This account is disabled - contact an administrator";
    if (reason == QLatin1String(Auth::REASON_EXPIRED))
        return "Your session expired - sign in again";
    if (reason == QLatin1String(Auth::REASON_FORBIDDEN))
        return "Not permitted - an administrator account is required";
    // 미연결과 타임아웃을 한 사유로 묶었으므로 문구도 둘 다 맞아야 한다.
    // "브로커에 연결 안 됨"이라고 단정하면, 브로커는 붙었는데 폴러가 응답을
    // 안 하는 흔한 경우에 엉뚱한 곳을 보게 만든다(상태줄이 "연결됨"인데
    // 오류는 "연결할 수 없습니다"인 모순도 생긴다).
    if (reason == QLatin1String(Auth::REASON_UNREACHABLE))
        return "The server is not responding - check the broker and poller";

    // 계약에 없는 사유는 삼키지 않는다. 원인을 화면에서 볼 수 있어야
    // "왜 안 되지"가 로그 뒤지기로 넘어가지 않는다.
    return QString("Cannot sign in (%1)").arg(reason);
}
