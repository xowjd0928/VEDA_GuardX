#include "account_settings_card.h"
#include "auth.h"
#include "panel_chrome.h"
#include "reauth_dialog.h"
#include "theme.h"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

AccountSettingsCard::AccountSettingsCard(QWidget *parent)
    : QWidget(parent)
{
    build_ui();

    // 권한이 바뀌는 두 경로를 모두 듣는다. 로그인/로그아웃(state)뿐 아니라
    // **오프라인 유예 진입·해제(verification)** 도 쓰기 가능 여부를 바꾼다 —
    // 한쪽만 들으면 유예 중에 버튼이 열린 채로 남는다(§4b 는 읽기 전용).
    connect(Auth::instance(), &Auth::state_changed, this,
            [this](Auth::State) { refresh_write_enable(); });
    connect(Auth::instance(), &Auth::verification_changed, this,
            [this](bool) { refresh_write_enable(); });
    refresh_write_enable();

    // 상태 문구는 반복 호출로 칠해지므로 restyle 을 걸 수 없다(부록 D).
    // 테마가 바뀌면 **같은 의미로** 다시 칠하기만 한다.
    Theme::on_theme_changed(this, [this] {
        set_status(m_status->text(), m_status_err);
        set_user_error(m_user_err->text());
    });
}

void AccountSettingsCard::build_ui()
{
    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(20);
    col->addWidget(build_create_card());
    col->addWidget(build_users_card());
    col->addStretch(1);
}

QWidget *AccountSettingsCard::build_create_card()
{
    auto *card = new QFrame(this);
    card->setObjectName("Panel");
    auto *col = new QVBoxLayout(card);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    col->addWidget(PanelChrome::header(
        QStringLiteral("Create Account"),
        QStringLiteral("vms_user · admin only"), card));

    auto *body = new QWidget(card);
    // 08-19: 우측 열(560px, 보드 배치)에 들어가게 두 줄로 — 필드 세 개와
    // [Create]·상태를 한 줄에 세우면 열 폭을 넘어 잘린다.
    auto *body_col = new QVBoxLayout(body);
    body_col->setContentsMargins(20, 14, 20, 16);
    body_col->setSpacing(6);
    auto *row = new QHBoxLayout();
    row->setSpacing(10);
    body_col->addLayout(row);

    const auto add = [&](const QString &label, QWidget *w, int width) {
        auto *box = new QVBoxLayout;
        box->setSpacing(4);
        auto *l = new QLabel(label, body);
        l->setFont(Theme::ui_font(11, 500));
        Theme::restyle(l, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        w->setMinimumHeight(Theme::px(30));
        if (width > 0)
            w->setFixedWidth(Theme::px(width));
        box->addWidget(l);
        box->addWidget(w);
        row->addLayout(box);
    };

    m_new_user = new QLineEdit(body);
    m_new_user->setMaxLength(64);
    m_new_user->setFont(Theme::ui_font(12));
    add("Username", m_new_user, 130);

    m_new_name = new QLineEdit(body);
    m_new_name->setMaxLength(64);
    m_new_name->setFont(Theme::ui_font(12));
    add("Display name", m_new_name, 130);

    m_new_role = new QComboBox(body);
    m_new_role->addItem("Operator", "operator");
    m_new_role->addItem("Administrator", "admin");
    m_new_role->setFont(Theme::ui_font(12));
    add("Role", m_new_role, 110);

    // 초기 비밀번호 입력칸은 없다(08-12). 관리자가 값을 정하면 그 사람이 남의
    // 비밀번호를 아는 상태가 되고, 어차피 첫 로그인에서 바뀐다.

    m_new_submit = new QPushButton("Create", body);
    m_new_submit->setObjectName("OutlineBtn");
    m_new_submit->setFont(Theme::ui_font(12, 600));
    m_new_submit->setMinimumHeight(Theme::px(30));
    m_new_submit->setCursor(Qt::PointingHandCursor);
    auto *btn_box = new QVBoxLayout;
    btn_box->setSpacing(4);
    btn_box->addWidget(new QLabel(" ", body));
    btn_box->addWidget(m_new_submit);
    row->addStretch(1);

    auto *row2 = new QHBoxLayout();
    row2->setSpacing(10);
    row2->addLayout(btn_box);

    m_status = new QLabel(body);
    m_status->setFont(Theme::ui_font(11.5));
    m_status->setWordWrap(true);
    auto *st_box = new QVBoxLayout;
    st_box->setSpacing(4);
    st_box->addWidget(new QLabel(" ", body));
    st_box->addWidget(m_status);
    row2->addLayout(st_box, 1);
    body_col->addLayout(row2);
    col->addWidget(body);

    // 아이디 칸 오류 — 칸 바로 밑, 같은 왼쪽 정렬. 칸이 130px 로 고정이라
    // 그 폭 안에 넣으면 문구가 잘리므로 줄을 통째로 쓴다.
    m_user_err = new QLabel(card);
    m_user_err->setFont(Theme::ui_font(11.5));
    m_user_err->setWordWrap(true);
    m_user_err->setContentsMargins(20, 0, 20, 10);
    m_user_err->hide();
    col->addWidget(m_user_err);

    // 안내 — 초기 비밀번호는 고정이고, 첫 로그인에서 반드시 바뀐다(서버 규약)
    auto *note = new QLabel(
        QString("New accounts start with the initial password %1 and must "
                "change it at first sign-in")
            .arg(Auth::initial_password()), card);
    note->setFont(Theme::ui_font(11));
    note->setContentsMargins(20, 0, 20, 14);
    Theme::restyle(note, [] {
        return QString("color:%1;").arg(Theme::textDim.name());
    });
    col->addWidget(note);

    connect(m_new_submit, &QPushButton::clicked, this,
            &AccountSettingsCard::submit_new_user);
    // 마지막 입력칸에서 Enter — 비밀번호 칸이 없어진 뒤로는 표시 이름이 끝이다
    connect(m_new_name, &QLineEdit::returnPressed, this,
            &AccountSettingsCard::submit_new_user);
    // 아이디를 고치는 순간 그 칸의 오류는 옛말이 된다. textEdited 를 쓰는 이유는
    // clear() 같은 코드 변경에는 반응하지 않기 위해서다(구역 표와 같은 규칙).
    //
    // 여기서 **받아 둔 목록으로 즉시** 중복을 알려준다 — 계약을 늘리지 않고,
    // 타이핑마다 서버로 쏘지도 않는다(그래서 디바운스가 필요 없고, 늦게 온
    // 응답이 최신 입력을 덮는 문제도 아예 생기지 않는다).
    connect(m_new_user, &QLineEdit::textEdited, this,
            [this](const QString &t) { set_user_error(taken_hint(t.trimmed())); });
    connect(Auth::instance(), &Auth::user_created, this,
            [this](bool ok, const QString &reason) {
        m_creating = false;
        refresh_write_enable();
        if (ok) {
            // 만든 사람이 그 자리에서 전해줄 수 있게 초기 비밀번호를 되짚어준다.
            // 안내 문구에도 있지만, 방금 만든 계정 이름과 붙어 있어야 옮겨 적는다.
            set_status(QString("Created %1 · initial password: %2 · must be "
                               "changed at first sign-in")
                           .arg(m_new_user->text(), Auth::initial_password()));
            set_user_error(QString());
            m_new_user->clear();
            m_new_name->clear();
            refresh_list();   // 방금 만든 계정이 목록에 보여야 한다
            return;
        }
        QString msg;
        if (reason == QLatin1String(Auth::REASON_DUPLICATE)) {
            // ⚠ 비활성 계정도 아이디를 계속 점유한다(서버 확인). 그래서 "이미
            // 있다"로 끝내면 관리자는 kim2·kim3 를 만들기 시작한다 — 정상 경로가
            // **재활성**이라는 것까지 말해야 문구가 제 일을 한다.
            set_user_error("That username is taken · a disabled account keeps "
                           "its name - re-enable it instead of making a new one");
            set_status(QString());
            m_new_user->setFocus();
            m_new_user->selectAll();
            return;
        }
        if (reason == QLatin1String(Auth::REASON_WEAK_PASSWORD))
            // 초기 비밀번호는 고정이라 정상 경로에서는 올 수 없다 — 오면 서버와
            // 이 빌드의 상수가 어긋났다는 뜻이므로 그렇게 말한다.
            msg = "The server rejected the fixed initial password - this build "
                  "and the server disagree";
        else if (reason == QLatin1String(Auth::REASON_FORBIDDEN))
            msg = "Only an administrator can create accounts";
        else if (reason == QLatin1String(Auth::REASON_UNREACHABLE))
            msg = "The server is not responding";
        else
            msg = QString("Could not create the account (%1)").arg(reason);
        set_status(msg, true);
    });

    return card;
}

void AccountSettingsCard::submit_new_user()
{
    // Enter 는 버튼 비활성을 우회한다 — 같은 판정을 여기서도 본다.
    // (권한 백스톱은 Auth::create_user() 안에 한 번 더 있다.)
    if (!m_new_submit->isEnabled())
        return;

    set_user_error(QString());   // 지난 답을 지운다 — 이제 다시 물어보는 중이다
    const QString user = m_new_user->text().trimmed();
    const QString name = m_new_name->text().trimmed();
    if (user.isEmpty() || name.isEmpty()) {
        set_status("Enter a username and a display name", true);
        return;
    }
    if (!ReauthDialog::ensure_fresh(this))
        return;
    m_creating = true;
    refresh_write_enable();
    set_status("creating...");
    Auth::instance()->create_user(
        user, name,
        m_new_role->currentData().toString() == QLatin1String("admin")
            ? Auth::Role::Admin : Auth::Role::Operator);
}

void AccountSettingsCard::set_status(const QString &text, bool error)
{
    m_status_err = error;
    m_status->setText(text);
    m_status->setStyleSheet(
        QString("color:%1;").arg(error ? Theme::alarm.name() : Theme::green.name()));
}

QWidget *AccountSettingsCard::build_users_card()
{
    auto *card = new QFrame(this);
    card->setObjectName("Panel");
    auto *col = new QVBoxLayout(card);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    col->addWidget(PanelChrome::header(
        QStringLiteral("Accounts"),
        QStringLiteral("vms_user · disabling keeps the account and its history"),
        card));

    auto *body = new QWidget(card);
    auto *body_col = new QVBoxLayout(body);
    body_col->setContentsMargins(20, 14, 20, 16);
    body_col->setSpacing(10);

    auto *head = new QHBoxLayout();
    head->setSpacing(10);
    m_refresh = new QPushButton("Refresh", body);
    m_refresh->setObjectName("OutlineBtn");
    m_refresh->setFont(Theme::ui_font(11, 600));
    m_refresh->setMinimumHeight(Theme::px(26));
    m_refresh->setCursor(Qt::PointingHandCursor);
    connect(m_refresh, &QPushButton::clicked, this,
            &AccountSettingsCard::refresh_list);
    head->addWidget(m_refresh);

    m_list_status = new QLabel(body);
    m_list_status->setFont(Theme::mono_font(10));
    m_list_status->setWordWrap(true);
    head->addWidget(m_list_status, 1);
    body_col->addLayout(head);

    m_users_grid = new QGridLayout();
    m_users_grid->setHorizontalSpacing(18);
    m_users_grid->setVerticalSpacing(8);
    body_col->addLayout(m_users_grid);

    col->addWidget(body);

    connect(Auth::instance(), &Auth::users_listed, this,
            [this](bool ok, const QVector<Auth::UserRow> &users,
                   const QString &reason) {
        m_listing = false;
        refresh_write_enable();
        if (ok) {
            m_users = users;
            rebuild_user_rows();
            set_list_status(QString("%1 accounts").arg(users.size()));
            // 목록이 새로 왔으니 아이디 칸의 판정도 다시 한다 — 방금 재활성한
            // 계정 때문에 문구가 달라질 수 있다.
            set_user_error(taken_hint(m_new_user->text().trimmed()));
            return;
        }
        if (reason == QLatin1String(Auth::REASON_UNREACHABLE))
            set_list_status("The server did not answer - press Refresh to retry",
                            true);
        else if (reason == QLatin1String(Auth::REASON_FORBIDDEN))
            set_list_status("Only an administrator can list accounts", true);
        else
            set_list_status(
                QString("Could not load the account list (%1)").arg(reason), true);
    });

    connect(Auth::instance(), &Auth::user_enabled_changed, this,
            [this](bool ok, const QString &username, bool enabled,
                   const QString &reason) {
        m_toggling = false;
        refresh_write_enable();
        if (ok) {
            // 응답이 반영된 값을 실어 주므로(계약) 목록을 다시 받지 않고
            // 그 행만 고친다. 그래도 Refresh 는 남겨 둔다 — 다른 PC 에서
            // 바뀐 것까지 맞추는 유일한 방법이다.
            for (Auth::UserRow &r : m_users)
                if (r.username == username)
                    r.enabled = enabled;
            rebuild_user_rows();
            set_list_status(QString("%1 is now %2")
                                .arg(username, enabled ? "active" : "disabled"));
            set_user_error(taken_hint(m_new_user->text().trimmed()));
            return;
        }
        QString msg;
        if (reason == QLatin1String(Auth::REASON_SELF_TARGET))
            msg = "You cannot disable the account you are signed in with";
        else if (reason == QLatin1String(Auth::REASON_LAST_ADMIN))
            msg = "This is the last active administrator - disabling it would "
                  "leave nobody who can manage accounts";
        else if (reason == QLatin1String(Auth::REASON_NOT_FOUND))
            msg = QString("There is no account called %1 anymore - press Refresh")
                      .arg(username);
        else if (reason == QLatin1String(Auth::REASON_FORBIDDEN))
            msg = "Only an administrator can change accounts";
        else if (reason == QLatin1String(Auth::REASON_UNREACHABLE))
            msg = "The server did not answer - nothing was changed";
        else
            msg = QString("The server refused the change (%1)").arg(reason);
        set_list_status(msg, true);
    });

    return card;
}

void AccountSettingsCard::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refresh_list();
}

void AccountSettingsCard::refresh_list()
{
    if (m_listing)
        return;   // 겹쳐 보내지 않는다 — 늦게 온 응답이 새 것을 덮는다
    if (!Auth::can(Auth::Action::AccountAdmin)) {
        // 탭 자체가 잠겨 있어 정상 경로로는 여기 오지 않는다. 그래도 조용히
        // 실패하지 않고 이유를 남긴다.
        set_list_status(Auth::deny_reason(Auth::Action::AccountAdmin), true);
        return;
    }
    m_listing = true;
    refresh_write_enable();
    set_list_status("loading...");
    Auth::instance()->list_users();
}

void AccountSettingsCard::rebuild_user_rows()
{
    // 행 수가 바뀌므로 통째로 다시 그린다. 위젯을 재사용하면 "지운 행의 버튼이
    // 다른 계정을 가리키는" 상태가 만들어진다 — 그 버튼은 사람을 지운다.
    while (QLayoutItem *item = m_users_grid->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    const char *headers[] = { "Username", "Display name", "Role", "State", "" };
    for (int c = 0; c < 5; ++c) {
        auto *h = new QLabel(QString::fromUtf8(headers[c]), this);
        h->setFont(Theme::ui_font(10.5, 700, 0.12));
        Theme::restyle(h, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        m_users_grid->addWidget(h, 0, c);
    }

    const QString me = Auth::instance()->username();
    int row = 1;
    for (const Auth::UserRow &u : m_users) {
        const bool self = (u.username == me);

        auto *name = new QLabel(self ? QString("%1  (you)").arg(u.username)
                                     : u.username, this);
        name->setFont(Theme::mono_font(11));
        m_users_grid->addWidget(name, row, 0);

        auto *disp = new QLabel(u.display_name, this);
        disp->setFont(Theme::ui_font(11.5));
        m_users_grid->addWidget(disp, row, 1);

        auto *role = new QLabel(Auth::role_text(u.role), this);
        role->setFont(Theme::ui_font(11.5));
        Theme::restyle(role, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        m_users_grid->addWidget(role, row, 2);

        // 상태 — 사용 중지는 경보색이 아니라 흐린 글자다. 빨강은 "지금 일어나고
        // 있는 사고"에 쓰는 색이고, 꺼둔 계정은 사고가 아니다.
        QString state = u.enabled ? QStringLiteral("active")
                                  : QStringLiteral("disabled");
        if (u.enabled && u.must_change_pw)
            state = QStringLiteral("active · initial password");
        auto *st = new QLabel(state, this);
        st->setFont(Theme::mono_font(10.5));
        const bool on = u.enabled;
        Theme::restyle(st, [on] {
            return QString("color:%1;").arg(on ? Theme::green.name()
                                               : Theme::textFaint.name());
        });
        m_users_grid->addWidget(st, row, 3);

        auto *act = new QPushButton(u.enabled ? "Disable" : "Enable", this);
        act->setObjectName("OutlineBtn");
        act->setFont(Theme::ui_font(11, 600));
        act->setMinimumHeight(Theme::px(26));
        act->setCursor(Qt::PointingHandCursor);
        // 자기 계정·마지막 관리자는 **서버가** 막는다. 여기서 미리 잠그지 않는
        // 이유는 "지금 관리자가 몇 명인가"가 이 PC 가 확신할 수 있는 사실이
        // 아니어서다(목록은 방금 것이지만 다른 PC 가 그 사이에 바꿀 수 있다).
        // 대신 거절 사유를 사람 말로 보여준다.
        const Auth::UserRow copy = u;
        connect(act, &QPushButton::clicked, this, [this, copy] {
            if (copy.enabled)
                confirm_disable(copy);
            else {
                if (!ReauthDialog::ensure_fresh(this))
                    return;
                m_toggling = true;
                refresh_write_enable();
                set_list_status(QString("enabling %1...").arg(copy.username));
                Auth::instance()->set_user_enabled(copy.username, true);
            }
        });
        m_users_grid->addWidget(act, row, 4);
        ++row;
    }

    if (m_users.isEmpty()) {
        auto *none = new QLabel("no accounts", this);
        none->setFont(Theme::mono_font(10.5));
        Theme::restyle(none, [] {
            return QString("color:%1;").arg(Theme::textFaint.name());
        });
        m_users_grid->addWidget(none, 1, 0, 1, 5);
    }
    m_users_grid->setColumnStretch(5, 1);
    refresh_write_enable();
}

void AccountSettingsCard::confirm_disable(const Auth::UserRow &row)
{
    // 되돌릴 수 있는 조작이지만 **그 사람은 그때까지 못 들어온다.** 관제 계정을
    // 실수로 끄면 교대 근무자가 로그인 화면 앞에서 막힌다.
    // QMessageBox::question() 대신 직접 조립하는 이유는 DEVICE 의 화재 해제와
    // 같다 — 표준 버튼은 Qt 번역에 딸려 "Yes/No"로 뜰 수 있다.
    QMessageBox box(this);
    box.setIcon(QMessageBox::NoIcon);
    box.setWindowTitle("Disable account");
    box.setTextFormat(Qt::RichText);
    box.setText(QString(
        "<b>Disable %1 (%2)?</b><br><br>"
        "They will not be able to sign in until someone turns the account "
        "back on.<br>"
        "The account is <b>not deleted</b> - it keeps its name and everything "
        "it changed stays attached to it.")
            .arg(row.display_name.toHtmlEscaped(),
                 row.username.toHtmlEscaped()));

    QPushButton *ok = box.addButton(QString("Disable account"),
                                    QMessageBox::AcceptRole);
    QPushButton *cancel = box.addButton(QString("Cancel"),
                                        QMessageBox::RejectRole);
    // 위험한 쪽만 경보색. 대화창과 함께 죽는 버튼이라 restyle 이 안전한 예외다.
    Theme::restyle(ok, [] {
        return QString("color:%1; border-color:%1;").arg(Theme::alarm.name());
    });
    box.setDefaultButton(cancel);   // 엔터 연타로 계정이 꺼지지 않게
    box.exec();

    if (box.clickedButton() != ok)
        return;
    // 확인 대화상자 **뒤**에 묻는다 — 어느 계정을 끄는지 보고 나서 비밀번호를
    // 치는 순서여야 한다(먼저 물으면 무엇에 동의하는지 모른 채 친다).
    if (!ReauthDialog::ensure_fresh(this))
        return;

    m_toggling = true;
    refresh_write_enable();
    set_list_status(QString("disabling %1...").arg(row.username));
    Auth::instance()->set_user_enabled(row.username, false);
}

void AccountSettingsCard::set_list_status(const QString &text, bool error)
{
    m_list_status_err = error;
    m_list_status->setText(text);
    m_list_status->setStyleSheet(
        QString("color:%1;").arg(error ? Theme::alarm.name()
                                       : Theme::textDim.name()));
}

QString AccountSettingsCard::taken_hint(const QString &username) const
{
    if (username.isEmpty())
        return QString();
    for (const Auth::UserRow &u : m_users) {
        // 서버의 비교 기준을 모르는 채로 대소문자를 구분하면, 화면은 통과시키고
        // 서버가 duplicate 를 주는 어긋남이 생긴다. 관대한 쪽(무시)으로 본다 —
        // 여기는 안내지 판정이 아니다.
        if (u.username.compare(username, Qt::CaseInsensitive) != 0)
            continue;
        return u.enabled
                   ? QString("That username is already taken")
                   : QString("That username belongs to a disabled account - "
                             "re-enable it below instead of making a new one");
    }
    return QString();
}

void AccountSettingsCard::set_user_error(const QString &text)
{
    if (!m_user_err)
        return;
    m_user_err->setText(text);
    m_user_err->setStyleSheet(QString("color:%1;").arg(Theme::alarm.name()));
    m_user_err->setVisible(!text.isEmpty());
}

void AccountSettingsCard::refresh_write_enable()
{
    // ⚠ 조건을 **한 곳에서** 계산한다 = (전송 중이 아닌가) AND (권한이 있는가).
    //   밖에서 setEnabled 로 권한을 걸면 응답이 도착해 버튼을 되살리는 순간
    //   권한 잠금까지 함께 풀린다 — 나중에 부른 쪽이 이긴다.
    const bool may = Auth::can(Auth::Action::AccountAdmin);
    const QString why = Auth::deny_reason(Auth::Action::AccountAdmin);

    m_new_submit->setEnabled(!m_creating && may);
    m_new_submit->setToolTip(may ? QString() : why);

    if (m_refresh) {
        m_refresh->setEnabled(!m_listing && may);
        m_refresh->setToolTip(may ? QString() : why);
    }

    // 행 버튼들(5번째 칸). 목록을 새로 받는 중에도 잠근다 — 곧 사라질 행의
    // 버튼을 누르면 방금 지워진 계정에게 명령이 간다.
    if (m_users_grid) {
        for (int r = 1; r < m_users_grid->rowCount(); ++r) {
            QLayoutItem *item = m_users_grid->itemAtPosition(r, 4);
            if (!item || !item->widget())
                continue;
            item->widget()->setEnabled(!m_toggling && !m_listing && may);
            item->widget()->setToolTip(may ? QString() : why);
        }
    }
}
