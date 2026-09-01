#include "reauth_dialog.h"
#include "auth.h"
#include "theme.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

bool ReauthDialog::ensure_fresh(QWidget *parent)
{
    Auth *auth = Auth::instance();

    // 아직 신선하면 아무것도 묻지 않는다. 조작마다 비밀번호를 받으면 사람은
    // 그 창을 읽지 않고 손가락으로 넘기게 되고, 그때부터 이 확인은 없는 것과
    // 같아진다.
    if (auth->fresh())
        return true;

    // 오프라인 유예(§4b)에서는 물어봐야 대조할 서버가 없다. 게다가 쓰기 명령
    // 자체가 브로커로 나가는 것이라 통과시켜도 보낼 곳이 없다 — 여기서 창을
    // 띄우면 "맞게 쳤는데도 안 된다"가 된다. 사유는 호출부가 이미 말한다.
    if (!auth->verified())
        return false;

    ReauthDialog dlg(parent);
    return dlg.exec() == QDialog::Accepted;
}

ReauthDialog::ReauthDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Confirm it is you");
    setModal(true);

    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(22, 20, 22, 18);
    col->setSpacing(10);

    auto *title = new QLabel(
        QString("Enter the password for %1").arg(Auth::instance()->username()),
        this);
    title->setFont(Theme::ui_font(13, 600));
    col->addWidget(title);

    auto *why = new QLabel(
        QString("This session signed in automatically, or it has been more than "
                "%1 minutes since the password was last entered. Settings "
                "changes need it once more.").arg(Auth::FRESH_MINUTES),
        this);
    why->setFont(Theme::ui_font(11.5));
    why->setWordWrap(true);
    why->setMaximumWidth(Theme::px(380));
    Theme::restyle(why, [] {
        return QString("color:%1;").arg(Theme::textDim.name());
    });
    col->addWidget(why);

    m_pw = new QLineEdit(this);
    m_pw->setEchoMode(QLineEdit::Password);
    m_pw->setMaxLength(128);
    m_pw->setFont(Theme::ui_font(12));
    m_pw->setMinimumHeight(Theme::px(32));
    m_pw->setPlaceholderText("Password");
    col->addWidget(m_pw);

    m_msg = new QLabel(this);
    m_msg->setFont(Theme::ui_font(11.5));
    m_msg->setWordWrap(true);
    m_msg->setMinimumHeight(Theme::px(30));
    col->addWidget(m_msg);

    auto *row = new QVBoxLayout();
    row->setSpacing(6);
    m_ok = new QPushButton("Confirm", this);
    m_ok->setObjectName("PrimaryBtn");
    m_ok->setFont(Theme::ui_font(12, 600));
    m_ok->setMinimumHeight(Theme::px(32));
    m_ok->setCursor(Qt::PointingHandCursor);
    row->addWidget(m_ok);

    auto *cancel = new QPushButton("Cancel", this);
    cancel->setObjectName("OutlineBtn");
    cancel->setFont(Theme::ui_font(12));
    cancel->setMinimumHeight(Theme::px(28));
    cancel->setCursor(Qt::PointingHandCursor);
    row->addWidget(cancel);
    col->addLayout(row);

    // ⚠ **Enter 가 [Cancel] 을 누르고 있었다** (08-13 실측 로그).
    //   QDialog 안의 QPushButton 은 기본이 autoDefault 라, 어느 것이 기본
    //   버튼이 될지는 포커스 순서가 정한다 — 비밀번호를 치고 Enter 를 누르면
    //   확인이 아니라 취소가 눌려 **조작이 조용히 버려졌다.**
    //   기본 버튼을 못 박고, 취소는 자동 기본에서 뺀다.
    m_ok->setAutoDefault(true);
    m_ok->setDefault(true);
    cancel->setAutoDefault(false);

    connect(m_ok, &QPushButton::clicked, this, &ReauthDialog::submit);
    connect(m_pw, &QLineEdit::returnPressed, this, &ReauthDialog::submit);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    // 잠금 카운트다운 — 로그인 화면과 같은 규칙(0초가 되면 문구를 지운다)
    m_lock_timer = new QTimer(this);
    m_lock_timer->setInterval(1000);
    connect(m_lock_timer, &QTimer::timeout, this, &ReauthDialog::tick_lock);

    connect(Auth::instance(), &Auth::reauth_result, this,
            [this](bool ok, const QString &reason, int retry_after_s) {
        if (ok) {
            accept();
            return;
        }
        m_reason = reason;
        m_retry_s = retry_after_s;
        if (m_retry_s > 0)
            m_lock_timer->start();
        m_ok->setEnabled(m_retry_s <= 0);
        m_pw->setEnabled(true);
        m_pw->selectAll();
        m_pw->setFocus();
        render_error();
    });

    m_pw->setFocus();
}

void ReauthDialog::submit()
{
    if (!m_ok->isEnabled())
        return;   // Enter 는 버튼 비활성을 우회한다 — 같은 판정을 여기서도
    m_ok->setEnabled(false);
    m_pw->setEnabled(false);
    m_reason.clear();
    m_msg->setText("checking...");
    m_msg->setStyleSheet(QString("color:%1;").arg(Theme::textDim.name()));
    // ⚠ 다듬지 않는다 — 공백도 비밀번호다(§비밀번호 정책 폐지).
    Auth::instance()->reauthenticate(m_pw->text());
}

void ReauthDialog::tick_lock()
{
    if (m_retry_s > 0)
        --m_retry_s;
    if (m_retry_s <= 0) {
        m_lock_timer->stop();
        m_reason.clear();
        m_ok->setEnabled(true);
    }
    render_error();
}

void ReauthDialog::render_error()
{
    QString msg;
    if (m_reason == QLatin1String(Auth::REASON_BAD_CREDENTIALS))
        msg = "That password is not correct";
    else if (m_reason == QLatin1String(Auth::REASON_LOCKED))
        // ⚠ 재확인 실패도 서버의 실패 잠금에 쌓인다. 남은 초를 그대로 보여주지
        //   않으면 사람은 계속 눌러 보고, 그때마다 잠금이 다시 채워진다.
        msg = QString("Too many attempts - wait %1 s. Trying again before that "
                      "only restarts the wait.").arg(m_retry_s > 0 ? m_retry_s : 0);
    else if (m_reason == QLatin1String(Auth::REASON_UNREACHABLE))
        msg = "The server is not responding - the change could not be sent "
              "anyway";
    else if (m_reason == QLatin1String(Auth::REASON_EXPIRED))
        msg = "This session is no longer valid - sign in again";
    else if (!m_reason.isEmpty())
        msg = QString("Could not confirm (%1)").arg(m_reason);

    m_msg->setText(msg);
    m_msg->setStyleSheet(QString("color:%1;").arg(Theme::alarm.name()));
}
