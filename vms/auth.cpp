#include "auth.h"
#include "credentials.h"
#include "mqtt_link.h"

#include <QDebug>
#include <QEventLoop>
#include <QPair>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QSettings>
#include <QTimer>
#include <QWidget>

#include <algorithm>

namespace {

/** @brief 로그인 3종 토픽 (핸드오프 §5 — 계약 정본) */
const QString TOPIC_LOGIN         = QStringLiteral("guardx/db/rpib/cmd/login");
const QString TOPIC_SESSION_CHECK = QStringLiteral("guardx/db/rpib/cmd/session_check");
const QString TOPIC_LOGOUT        = QStringLiteral("guardx/db/rpib/cmd/logout");
// §5b (08-11 저녁) — 비밀번호 변경 · 계정 생성
const QString TOPIC_CHANGE_PW     = QStringLiteral("guardx/db/rpib/cmd/change_password");
const QString TOPIC_CREATE_USER   = QStringLiteral("guardx/db/rpib/cmd/create_user");
// 08-12 — 계정 목록 · 사용 중지/재개
const QString TOPIC_LIST_USERS    = QStringLiteral("guardx/db/rpib/cmd/list_users");
const QString TOPIC_SET_ENABLED   = QStringLiteral("guardx/db/rpib/cmd/set_user_enabled");

/**
 * @brief 기동 시 브로커 접속을 기다리는 시간 (ms)
 *
 * MqttLink 접속은 비동기라 기동 직후에는 **항상** 미연결이다. 그때 바로
 * 오프라인 유예로 넘어가면 브로커가 멀쩡한 날에도 매번 읽기 전용으로
 * 들어가게 된다. 잠깐 기다렸다가 판단한다.
 */
const int BROKER_WAIT_MS = 6000;

/**
 * @brief 스텁의 응답 지연 (ms)
 *
 * 0으로 두면 화면의 "확인 중…" 상태가 한 프레임도 안 보여서, 중복 제출·
 * 버튼 비활성 같은 것이 시험되지 않은 채로 남는다. 실제 PBKDF2 검증이
 * 100~300ms인 것과 비슷한 값으로 둔다.
 */
const int STUB_DELAY_MS = 300;

/** @brief 실패 잠금 정책 (핸드오프 §5: 5회 → 60초, 10회 → 10분) */
const int STUB_LOCK_AT       = 5;
const int STUB_LOCK_SEC      = 60;
const int STUB_HARD_LOCK_AT  = 10;
const int STUB_HARD_LOCK_SEC = 600;

/** @brief 스텁 계정 — 진짜 자격이 아니다. 진짜 검증은 폴러가 한다 */
struct StubAccount {
    const char *username;
    const char *password;
    const char *role;
    const char *display_name;
    bool enabled;
};

/**
 * @brief 스텁의 가변 상태 — 비밀번호·강제변경 플래그·추가된 계정
 *
 * 서버 없이 §5b 를 시험하려면 스텁도 "바뀐 비밀번호"를 기억해야 한다.
 * 프로세스가 죽으면 사라진다(진짜 저장소가 아니다).
 */
struct StubMutable {
    QString password;
    QString display_name;
    QString role;
    bool enabled = true;
    bool must_change = false;
};
QHash<QString, StubMutable> g_stub_state;
const StubAccount STUB_ACCOUNTS[] = {
    { "admin",    "admin",    "admin",    "Admin (stub)",    true  },
    { "operator", "operator", "operator", "Operator (stub)", true  },
    // 사용 중지 계정 문구(§2b)를 서버 없이 확인하기 위한 자리
    { "disabled", "disabled", "operator", "Disabled acct",   false },
    // §5b 강제 변경 경로를 서버 없이 확인하기 위한 자리 (must_change=true)
    { "newbie",   "newbie12", "operator", "New account",     true  },
};

/**
 * @brief 스텁 계정표를 처음 한 번 채운다 — 이후에는 g_stub_state 가 진실이다
 *
 * 예전에는 `stub_reply()` 안에서만 채웠다. 그때는 로그인이 유일한 입구라
 * 성립했지만, 계정 목록·사용 중지는 **로그인 뒤에** 들어오는 입구라 자기가
 * 먼저 불릴 수 있다(자가시험이 그 순서로 돈다). 씨앗 뿌리는 자리를 하나로 모은다.
 */
void ensure_stub_seed();

/**
 * @brief 서버가 준 만료 시각을 읽는다
 *
 * 계약(핸드오프 §5, 08-11 확정): **ISO8601 · UTC · `Z` 접미 · 초 단위**
 * (`"2026-09-10T01:22:31Z"`). 프로젝트 규칙은 `timestamp` = epoch 밀리초 정수,
 * `*_at` = ISO8601 UTC 문자열이다.
 *
 * 그래도 epoch 숫자를 함께 받는다. 형식이 서버 코드에 못 박혀 있다지만
 * (`to_char(... AT TIME ZONE 'UTC')`), 여기서 못 읽으면 **자동 로그인이
 * 조용히 죽는** 자리라 관대한 쪽이 싸다.
 *
 * 못 읽으면 **무효 QDateTime**을 돌려준다 — 임의의 값을 지어내면 "만료됨"을
 * 영영 못 알아채거나 멀쩡한 세션을 버린다.
 */
QDateTime parse_expires_at(const QJsonValue &v)
{
    if (v.isString()) {
        const QDateTime dt = QDateTime::fromString(v.toString(), Qt::ISODate);
        if (dt.isValid())
            return dt.toLocalTime();
        return QDateTime();
    }
    if (v.isDouble()) {
        const qint64 n = static_cast<qint64>(v.toDouble());
        // 10^12 를 넘으면 밀리초다 (초로 읽으면 서기 33658년이 된다).
        // 거듭제곱 함수를 부르지 않는다 — CMakeLists 의
        // -Wl,--disable-auto-import 가드레일에 libm 호출이 걸린다.
        const qint64 MS_THRESHOLD = 1000000000000LL;
        return n > MS_THRESHOLD ? QDateTime::fromMSecsSinceEpoch(n)
                                : QDateTime::fromSecsSinceEpoch(n);
    }
    return QDateTime();
}

void ensure_stub_seed()
{
    if (!g_stub_state.isEmpty())
        return;
    for (const StubAccount &a : STUB_ACCOUNTS) {
        StubMutable st;
        st.password = QString::fromLatin1(a.password);
        st.display_name = QString::fromUtf8(a.display_name);
        st.role = QString::fromLatin1(a.role);
        st.enabled = a.enabled;
        // 새로 만든 계정과 같은 취급 — 첫 로그인에서 바꾸게 한다
        st.must_change = (QLatin1String(a.username) == QLatin1String("newbie"));
        g_stub_state.insert(QString::fromLatin1(a.username), st);
    }
}

} // namespace

Auth *Auth::instance()
{
    static Auth a;
    return &a;
}

Auth::Auth(QObject *parent)
    : QObject(parent)
{
    // 브로커가 (다시) 붙으면 두 가지를 한다:
    //  ① 기동 검증을 기다리던 중이었다면 그때 검증을 보낸다
    //  ② 오프라인 유예로 들어와 있었다면 재검증해 **읽기 전용을 푼다**
    // 유예는 임시 상태여야 한다 — 브로커가 살아났는데 계속 읽기 전용이면
    // 운영자가 그 사실을 모른 채 조작이 막힌다.
    connect(MqttLink::instance(), &MqttLink::online_changed, this,
            [this](bool online) {
        if (!online || stub_enabled())
            return;
        if (m_resuming && m_state == State::Checking) {
            if (m_broker_wait)
                m_broker_wait->stop();
            verify_token();
        } else if (m_state == State::LoggedIn && !m_verified && !m_token.isEmpty()) {
            qInfo() << "[Auth] 브로커 복귀 — 세션 재검증";
            verify_token();
        }
    });
}

bool Auth::stub_enabled()
{
    // 값은 한 번만 읽는다 — 실행 중에 인증 경로가 바뀌면 그게 더 위험하다.
    //
    // ⚠ 기본값은 **꺼짐**이다(08-11 mTLS 절체 후 뒤집었다). 켜짐이 기본이던
    // 동안에는 레지스트리를 건드린 적 없는 PC가 전부 스텁으로 떴다 —
    // admin/admin 으로 관리자 전권, 서버 검증 0. 개발 편의 기본값이 곧
    // 배포 기본값이 되는 자리라, 안전한 쪽을 기본으로 둔다.
    // 스텁이 필요하면 명시적으로 켠다:
    //   reg add HKCU\Software\GuardX\VMS\auth /v stub /t REG_DWORD /d 1 /f
    static const bool v =
        QSettings("GuardX", "VMS").value("auth/stub", false).toBool();
    return v;
}

bool Auth::stub_offline()
{
    static const bool v =
        QSettings("GuardX", "VMS").value("auth/stub_offline", false).toBool();
    return v;
}

QString Auth::role_text(Role r)
{
    return r == Role::Admin ? QStringLiteral("Administrator")
                            : QStringLiteral("Operator");
}

bool Auth::can(Action a)
{
    Q_UNUSED(a);   // §5 표의 모든 항목이 오늘은 관리자 전용이다
    const Auth *s = instance();

    // 순서가 곧 문구의 순서다(deny_reason). 미로그인 → 오프라인 → 역할.
    if (s->m_state != State::LoggedIn)
        return false;
    if (!s->m_verified)
        return false;   // §4b 오프라인 유예 = 읽기 전용
    return s->m_role == Role::Admin;
}

void Auth::change_password(const QString &old_pw, const QString &new_pw)
{
    if (m_state != State::LoggedIn && m_state != State::MustChangePassword) {
        emit password_changed(false, QString::fromLatin1(REASON_EXPIRED));
        return;
    }
    // 정책 선검사가 없다(08-12 폐지). 빈 값도 **서버가** 판정한다 — 화면이
    // 자체 규칙을 하나라도 갖는 순간, 서버가 받아 줄 비밀번호를 화면이 거부하는
    // 어긋남이 다시 생기고 사용자는 이유를 알 수 없다.
    // ⚠ new_pw 를 다듬지 않는다(trimmed 금지). 앞뒤 공백도 비밀번호다.

    const quint64 epoch = m_epoch;

    if (stub_enabled()) {
        QTimer::singleShot(STUB_DELAY_MS, this, [this, epoch, old_pw, new_pw] {
            if (epoch != m_epoch)
                return;
            // 서버가 거부하는 유일한 값 — 빈 비밀번호(08-12 정책 폐지 후).
            // 스텁이 이걸 받아 주면 화면은 "빈 값도 된다"는 거짓을 배운다.
            if (new_pw.isEmpty()) {
                emit password_changed(false,
                                      QString::fromLatin1(REASON_WEAK_PASSWORD));
                return;
            }
            StubMutable &st = g_stub_state[m_username];
            if (st.password != old_pw) {
                emit password_changed(false,
                                      QString::fromLatin1(REASON_BAD_CREDENTIALS));
                return;
            }
            st.password = new_pw;
            st.must_change = false;
            // 서버는 새 토큰을 준다 — 스텁도 같은 모양으로 흉내 낸다.
            quint32 raw[8];
            QRandomGenerator::global()->fillRange(raw);
            m_token = QString::fromLatin1(
                QByteArray(reinterpret_cast<const char *>(raw), sizeof(raw)).toHex());
            m_verified = true;
            m_verified_at = QDateTime::currentDateTime();
            store_session();
            set_state(State::LoggedIn);
            emit password_changed(true, QString());
        });
        return;
    }

    QJsonObject params;
    params["token"] = m_token;
    params["old_password"] = old_pw;
    params["new_password"] = new_pw;

    MqttLink::instance()->request(
        TOPIC_CHANGE_PW, params,
        [this, epoch](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            // ⚠ 서버가 기존 세션을 전부 무효화했다 — 새 토큰으로 갈아 끼우지
            //   않으면 **방금 바꾼 본인이 튕긴다.**
            const QString tok = reply.value("token").toString();
            if (tok.isEmpty()) {
                qWarning() << "[Auth] change_password 응답에 새 토큰이 없다 —"
                              "이 세션은 곧 무효가 된다";
            } else {
                m_token = tok;
                m_expires_at = parse_expires_at(reply.value("expires_at"));
                m_verified = true;
                m_verified_at = QDateTime::currentDateTime();
                store_session();
            }
            set_state(State::LoggedIn);   // 강제 변경이었다면 이제 풀린다
            emit password_changed(true, QString());
        },
        [this, epoch](const QString &why) {
            if (epoch != m_epoch) return;
            qWarning() << "[Auth] 비밀번호 변경 실패:" << why;
            emit password_changed(false, QString::fromLatin1(REASON_UNREACHABLE));
        },
        MqttLink::DEFAULT_TIMEOUT_MS,
        [this, epoch](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            emit password_changed(false, reply.value("reason").toString());
        });
}

QString Auth::initial_password()
{
    // 판단 근거는 헤더 주석에 있다 — 요약하면 "이 비밀번호로 할 수 있는 일은
    // 비밀번호를 바꾸는 것뿐"이다(서버가 must_change_pw=TRUE 로 만든다).
    return QStringLiteral("qkdwnsgks123!");
}

void Auth::create_user(const QString &username, const QString &display_name,
                       Role role)
{
    const QString password = initial_password();
    // 백스톱 — 화면은 관리자에게만 보이지만 그건 표면이다(§5 원칙).
    if (!can(Action::AccountAdmin)) {   // = 로그인 ∧ verified ∧ admin
        emit user_created(false, QString::fromLatin1(REASON_FORBIDDEN));
        return;
    }
    const quint64 epoch = m_epoch;
    const QString role_s = (role == Role::Admin) ? "admin" : "operator";

    if (stub_enabled()) {
        QTimer::singleShot(STUB_DELAY_MS, this,
                           [this, epoch, username, display_name, role_s, password] {
            if (epoch != m_epoch)
                return;
            const bool exists = g_stub_state.contains(username);
            if (exists) {
                emit user_created(false, QString::fromLatin1(REASON_DUPLICATE));
                return;
            }
            StubMutable st;
            st.password = password;
            st.display_name = display_name;
            st.role = role_s;
            st.must_change = true;   // 서버와 같다 — 본인이 첫 로그인에서 바꾼다
            g_stub_state.insert(username, st);
            qWarning() << "[Auth] 스텁 계정 생성:" << username << role_s;
            emit user_created(true, QString());
        });
        return;
    }

    QJsonObject params;
    params["token"] = m_token;
    params["username"] = username;
    params["display_name"] = display_name;
    params["role"] = role_s;
    params["password"] = password;

    MqttLink::instance()->request(
        TOPIC_CREATE_USER, params,
        [this, epoch](const QJsonObject &) {
            if (epoch != m_epoch) return;
            emit user_created(true, QString());
        },
        [this, epoch](const QString &why) {
            if (epoch != m_epoch) return;
            qWarning() << "[Auth] 계정 생성 실패:" << why;
            emit user_created(false, QString::fromLatin1(REASON_UNREACHABLE));
        },
        MqttLink::DEFAULT_TIMEOUT_MS,
        [this, epoch](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            // ⚠ 다른 쓰기 넷(set_zone·set_actuator·set_fire_threshold·
            //   set_site_config)은 전부 이걸 탄다. create_user 만 안 타서,
            //   만료된 토큰으로 계정을 만들면 세션이 죽은 채 화면에 남아 누를
            //   때마다 같은 거절만 반복했다. 계약이 create_user 에도
            //   must_change_password 를 돌려주도록 명시하고 있어(§토큰) 더 그렇다.
            note_write_reject(reply);
            emit user_created(false, reply.value("reason").toString());
        });
}

void Auth::list_users()
{
    // 백스톱 — 화면은 관리자에게만 열리지만 그건 표면이다(§5 원칙).
    if (!can(Action::AccountAdmin)) {
        emit users_listed(false, {}, QString::fromLatin1(REASON_FORBIDDEN));
        return;
    }

    const quint64 epoch = m_epoch;

    if (stub_enabled()) {
        QTimer::singleShot(STUB_DELAY_MS, this, [this, epoch] {
            if (epoch != m_epoch)
                return;
            ensure_stub_seed();
            QVector<UserRow> rows;
            for (auto it = g_stub_state.cbegin(); it != g_stub_state.cend(); ++it) {
                UserRow r;
                r.username = it.key();
                r.display_name = it->display_name;
                r.role = it->role == QLatin1String("admin") ? Role::Admin
                                                            : Role::Operator;
                r.enabled = it->enabled;
                r.must_change_pw = it->must_change;
                rows.append(r);
            }
            // 서버가 username ASC 로 준다 — 스텁도 같은 순서여야 화면이 같다
            // (QHash 순회 순서는 실행마다 달라 목록이 매번 뒤바뀐다).
            std::sort(rows.begin(), rows.end(),
                      [](const UserRow &a, const UserRow &b) {
                          return a.username < b.username;
                      });
            emit users_listed(true, rows, QString());
        });
        return;
    }

    QJsonObject params;
    attach_token(params);

    MqttLink::instance()->request(
        TOPIC_LIST_USERS, params,
        [this, epoch](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            // ⚠ `users` 키가 없으면 **빈 목록이 아니라 형식 오류**다 — 계약은
            //   0건이어도 `[]` 를 싣는다. 빈 목록으로 읽으면 "계정이 하나도
            //   없다"는 화면이 뜨고, 관리자는 그걸 사실로 믿는다.
            const QJsonValue v = reply.value("users");
            if (!v.isArray()) {
                qWarning() << "[Auth] list_users 응답에 users 배열이 없다:"
                           << QJsonDocument(reply).toJson(QJsonDocument::Compact);
                emit users_listed(false, {},
                                  QString::fromLatin1(REASON_BAD_REPLY));
                return;
            }
            QVector<UserRow> rows;
            const QJsonArray arr = v.toArray();
            for (const QJsonValue &e : arr) {
                const QJsonObject o = e.toObject();
                UserRow r;
                r.username = o.value("username").toString();
                if (r.username.isEmpty())
                    continue;   // 아이디 없는 행은 누를 수도 없다
                r.display_name = o.value("display_name").toString();
                if (r.display_name.isEmpty())
                    r.display_name = r.username;
                // 모르는 역할은 운영자로 읽는다 — apply_ok 와 같은 규칙
                r.role = o.value("role").toString() == QLatin1String("admin")
                             ? Role::Admin : Role::Operator;
                // ⚠ 없으면 **true** 로 읽는다. false 로 읽으면 서버가 필드를
                //   빠뜨린 날 멀쩡한 계정이 전부 "사용 중지"로 보인다.
                r.enabled = o.value("enabled").toBool(true);
                r.must_change_pw = o.value("must_change_pw").toBool(false);
                r.last_login_at = parse_expires_at(o.value("last_login_at"));
                rows.append(r);
            }
            emit users_listed(true, rows, QString());
        },
        [this, epoch](const QString &why) {
            if (epoch != m_epoch) return;
            qWarning() << "[Auth] 계정 목록 실패:" << why;
            emit users_listed(false, {}, QString::fromLatin1(REASON_UNREACHABLE));
        },
        MqttLink::DEFAULT_TIMEOUT_MS,
        [this, epoch](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            note_write_reject(reply);   // 만료·비활성이면 세션을 접는다
            emit users_listed(false, {}, reply.value("reason").toString());
        });
}

void Auth::set_user_enabled(const QString &username, bool enabled)
{
    if (!can(Action::AccountAdmin)) {
        emit user_enabled_changed(false, username, enabled,
                                  QString::fromLatin1(REASON_FORBIDDEN));
        return;
    }

    const quint64 epoch = m_epoch;

    if (stub_enabled()) {
        QTimer::singleShot(STUB_DELAY_MS, this, [this, epoch, username, enabled] {
            if (epoch != m_epoch)
                return;
            ensure_stub_seed();
            const auto it = g_stub_state.find(username);
            if (it == g_stub_state.end()) {
                emit user_enabled_changed(false, username, enabled,
                                          QString::fromLatin1(REASON_NOT_FOUND));
                return;
            }
            // 서버가 막는 두 가지를 같은 사유로 흉내 낸다. 화면 문구를 서버 없이
            // 시험하려면 스텁이 같은 답을 줘야 한다.
            //
            // ⚠ **last_admin 을 self_target 보다 먼저 본다.** 이 명령은 관리자만
            //   부를 수 있어서, "마지막 관리자를 끄는" 상황은 사실상 **자기 자신을
            //   끄는 경우뿐**이다(남을 끄는데 그가 마지막이라면 나는 관리자가
            //   아니라는 뜻이다). self_target 을 먼저 보면 last_admin 은 영영
            //   안 나오고, 그러면 화면은 "네 계정이라 안 된다"고만 말한다 —
            //   진짜 이유("이걸 끄면 아무도 계정을 못 만든다")가 더 쓸모 있다.
            //   ⓘ 실서버의 검사 순서는 미확인이다. 화면은 두 사유를 모두
            //     사람 말로 처리하므로 순서가 달라도 문구만 바뀐다.
            if (!enabled && it->role == QLatin1String("admin")) {
                int admins = 0;
                for (auto a = g_stub_state.cbegin(); a != g_stub_state.cend(); ++a)
                    if (a->enabled && a->role == QLatin1String("admin"))
                        ++admins;
                if (admins <= 1) {
                    emit user_enabled_changed(
                        false, username, enabled,
                        QString::fromLatin1(REASON_LAST_ADMIN));
                    return;
                }
            }
            if (username == m_username) {
                emit user_enabled_changed(false, username, enabled,
                                          QString::fromLatin1(REASON_SELF_TARGET));
                return;
            }
            it->enabled = enabled;
            qWarning().noquote() << QString("[Auth] 스텁 계정 %1: %2")
                                        .arg(username,
                                             enabled ? "재개" : "사용 중지");
            emit user_enabled_changed(true, username, enabled, QString());
        });
        return;
    }

    QJsonObject params;
    attach_token(params);
    params["username"] = username;
    params["enabled"] = enabled;

    MqttLink::instance()->request(
        TOPIC_SET_ENABLED, params,
        [this, epoch, username, enabled](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            // 서버가 **반영된 값을 되돌려 준다**(계약). 그걸 우선 쓰고, 없으면
            // 보낸 값으로 떨어진다 — 화면이 목록을 다시 받지 않아도 된다.
            const QString who = reply.value("username").toString(username);
            const bool now = reply.value("enabled").toBool(enabled);
            emit user_enabled_changed(true, who, now, QString());
        },
        [this, epoch, username, enabled](const QString &why) {
            if (epoch != m_epoch) return;
            qWarning() << "[Auth] 계정 사용여부 변경 실패:" << why;
            emit user_enabled_changed(false, username, enabled,
                                      QString::fromLatin1(REASON_UNREACHABLE));
        },
        MqttLink::DEFAULT_TIMEOUT_MS,
        [this, epoch, username, enabled](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            note_write_reject(reply);   // expired·disabled 면 세션을 접는다
            emit user_enabled_changed(false, username, enabled,
                                      reply.value("reason").toString());
        });
}

void Auth::attach_token(QJsonObject &params)
{
    const QString t = instance()->m_token;
    if (t.isEmpty())
        return;   // 필드 자체를 넣지 않는다 (§6 과도기 규약)
    params["token"] = t;
}

QString Auth::note_write_reject(const QJsonObject &reply)
{
    const QString reason = reply.value("reason").toString();
    Auth *s = instance();

    if (reason == QLatin1String(REASON_EXPIRED)
        || reason == QLatin1String(REASON_DISABLED)) {
        // 이 세션으로는 앞으로도 아무것도 못 한다. 화면에 남겨두면 누를 때마다
        // 거절만 반복하므로 로그인 화면으로 되돌린다.
        qWarning() << "[Auth] 쓰기 거절로 세션 무효:" << reason;
        s->logout();
        return reason == QLatin1String(REASON_DISABLED)
                   ? QString("This account is disabled - contact an administrator")
                   : QString("Your session expired - sign in again");
    }

    if (reason == QLatin1String(REASON_MUST_CHANGE_PW)) {
        // 서버가 "이 세션은 비밀번호를 바꾸기 전엔 아무것도 못 한다"고 답했다.
        // 우리 화면이 그 상태를 놓쳤다는 뜻이므로 상태를 맞추고 변경 화면으로
        // 되돌린다 — 일반 실패로 흘리면 사용자는 영문 사유만 보고 갇힌다.
        qWarning() << "[Auth] 서버가 강제 변경 대상이라고 거절 — 변경 화면으로";
        s->set_state(State::MustChangePassword);
        return QStringLiteral("You must change your password first");
    }

    if (reason == QLatin1String("forbidden")) {
        // 서버가 역할로 거절했다. 화면 잠금과 어긋났다는 뜻이라 로그를 남긴다 —
        // 정상 동작이면 버튼이 이미 잠겨 있어 여기까지 오지 않는다.
        qWarning() << "[Auth] 서버가 권한으로 거절(forbidden) — 화면 잠금과 불일치";
        return QStringLiteral("Administrator rights required");
    }

    return reason.isEmpty()
               ? QString("The server refused the request")
               : QString("The server refused the request (%1)").arg(reason);
}

void Auth::bind(QWidget *w, Action a)
{
    if (!w)
        return;
    // 원래 툴팁을 기억해 둔다 — 권한이 생기면 화면의 설명이 돌아와야 한다.
    const QString original = w->toolTip();
    const auto apply = [w, a, original] {
        const bool ok = can(a);
        w->setEnabled(ok);
        w->setToolTip(ok ? original : deny_reason(a));
    };
    apply();
    // 위젯을 context 로 준다 — 위젯이 죽으면 연결도 함께 죽는다.
    QObject::connect(instance(), &Auth::state_changed, w,
                     [apply](State) { apply(); });
    QObject::connect(instance(), &Auth::verification_changed, w,
                     [apply](bool) { apply(); });
}

QString Auth::deny_reason(Action a)
{
    if (can(a))
        return QString();

    const Auth *s = instance();
    if (s->m_state != State::LoggedIn)
        return QStringLiteral("Sign in required");
    if (!s->m_verified)
        return QStringLiteral("Offline - read only until the server confirms");
    return QStringLiteral("Administrator rights required");
}

void Auth::set_state(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit state_changed(s);
}

void Auth::login(const QString &username, const QString &password)
{
    // 백스톱 — 화면이 버튼을 비활성시키지만 그건 표면이다. 잠긴 위젯도
    // 코드로는 clicked 를 낼 수 있고, 새 호출부가 목록에서 빠질 수 있다.
    if (m_state == State::Checking) {
        qWarning() << "[Auth] 확인 중에 들어온 로그인 요청 — 무시";
        return;
    }

    const QString user = username.trimmed();
    if (user.isEmpty() || password.isEmpty()) {
        // 빈 칸을 서버까지 보내지 않는다. 사유는 오입력과 같은 것을 쓴다 —
        // 화면 문구가 어차피 하나로 합쳐져 있다(§2b).
        //
        // ⚠ **결과는 반드시 login() 이 돌아간 뒤에 알린다.** 여기서 바로
        // emit 하면 이 경우에만 시그널이 호출 도중에 도착해, 버튼 핸들러가
        // login() 뒤에 "확인 중…"을 세팅하는 순간 오류 문구가 지워진다.
        // (자가시험이 실제로 이 자리에서 걸렸다.) 통보 시점은 한 가지여야 한다.
        const quint64 epoch = ++m_epoch;
        QTimer::singleShot(0, this, [this, epoch] {
            if (epoch != m_epoch)
                return;
            fail(QString::fromLatin1(REASON_BAD_CREDENTIALS), 0);
        });
        return;
    }

    ++m_epoch;
    m_human_login = true;   // 사람이 친 로그인 — apply_ok 가 재확인 시계를 돌린다
    set_state(State::Checking);
    submit(user, password);
}

bool Auth::fresh() const
{
    if (!m_human_auth_at.isValid())
        return false;   // 자동 로그인만으로 들어온 세션 — 아직 아무도 안 물었다
    return m_human_auth_at.secsTo(QDateTime::currentDateTime())
           < qint64(FRESH_MINUTES) * 60;
}

void Auth::reauthenticate(const QString &password)
{
    if (m_state != State::LoggedIn || m_username.isEmpty()) {
        emit reauth_result(false, QString::fromLatin1(REASON_EXPIRED), 0);
        return;
    }
    if (password.isEmpty()) {
        // 빈 값을 서버까지 보내지 않는다. 로그인과 같은 사유를 쓴다 —
        // 화면 문구가 어차피 "비밀번호가 맞지 않는다" 하나다.
        emit reauth_result(false, QString::fromLatin1(REASON_BAD_CREDENTIALS), 0);
        return;
    }

    const quint64 epoch = m_epoch;
    const QString user = m_username;

    if (stub_enabled()) {
        QTimer::singleShot(STUB_DELAY_MS, this, [this, epoch, user, password] {
            if (epoch != m_epoch)
                return;
            ensure_stub_seed();
            const auto it = g_stub_state.constFind(user);
            if (it == g_stub_state.cend() || it->password != password) {
                emit reauth_result(false,
                                   QString::fromLatin1(REASON_BAD_CREDENTIALS), 0);
                return;
            }
            m_human_auth_at = QDateTime::currentDateTime();
            emit reauth_result(true, QString(), 0);
        });
        return;
    }

    // ⚠ **세션을 갈아엎지 않는다.** state 를 Checking 으로 돌리거나 실패 시
    //   fail() 을 부르면 화면이 통째로 로그인으로 돌아간다 — 비밀번호를 한 번
    //   잘못 친 대가로는 너무 크다. 여기서는 응답만 보고 시계를 돌린다.
    const QString old_token = m_token;

    QJsonObject params;
    params["username"] = user;
    params["password"] = password;
    params["device"] = QString("vms %1").arg(MqttLink::instance()->client_id());

    MqttLink::instance()->request(
        TOPIC_LOGIN, params,
        [this, epoch, old_token](const QJsonObject &reply) {
            if (epoch != m_epoch) return;

            // 새 세션이 생겼다 — 옛 토큰은 서버에서 지운다. 안 지우면 재확인
            // 할 때마다 세션 행이 하나씩 쌓이고, 그 토큰들은 30일을 산다.
            const QString tok = reply.value("token").toString();
            if (!tok.isEmpty() && tok != old_token) {
                m_token = tok;
                m_expires_at = parse_expires_at(reply.value("expires_at"));
                store_session();
                if (!old_token.isEmpty() && MqttLink::instance()->online()) {
                    QJsonObject bye;
                    bye["token"] = old_token;
                    MqttLink::instance()->request(
                        TOPIC_LOGOUT, bye,
                        [](const QJsonObject &) {},
                        [](const QString &why) {
                            qWarning() << "[Auth] 재확인 후 옛 세션 삭제 실패:"
                                       << why << "— 만료까지 남는다";
                        });
                }
            }
            m_verified = true;
            m_verified_at = QDateTime::currentDateTime();
            m_human_auth_at = m_verified_at;
            qInfo() << "[Auth] 비밀번호 재확인 성공";
            emit reauth_result(true, QString(), 0);
        },
        [this, epoch](const QString &why) {
            if (epoch != m_epoch) return;
            qWarning() << "[Auth] 재확인 요청 실패:" << why;
            emit reauth_result(false, QString::fromLatin1(REASON_UNREACHABLE), 0);
        },
        MqttLink::DEFAULT_TIMEOUT_MS,
        [this, epoch](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            const QJsonValue retry = reply.value("retry_after_s");
            int retry_s = retry.toInt(0);
            if (retry_s == 0 && retry.isString())
                retry_s = retry.toString().toInt();
            emit reauth_result(false, reply.value("reason").toString(), retry_s);
        });
}

void Auth::logout()
{
    // 진행 중이던 요청의 응답이 뒤늦게 도착해 되살아나지 못하게 세대를 올린다.
    ++m_epoch;
    if (!m_req_id.isEmpty()) {
        MqttLink::instance()->cancel(m_req_id);
        m_req_id.clear();
    }
    if (m_broker_wait)
        m_broker_wait->stop();
    m_resuming = false;

    // 서버 세션도 지운다 — 안 지우면 그 토큰이 30일간 다른 PC에서도 산다.
    // **응답을 기다리지 않는다**: 브로커가 죽어 있어도 로컬 로그아웃은 반드시
    // 되어야 한다(관제 PC를 로그아웃 못 하는 상황을 만들지 않는다).
    if (!stub_enabled() && !m_token.isEmpty() && MqttLink::instance()->online()) {
        QJsonObject params;
        params["token"] = m_token;
        MqttLink::instance()->request(
            TOPIC_LOGOUT, params,
            [](const QJsonObject &) { qInfo() << "[Auth] 서버 세션 삭제됨"; },
            [](const QString &why) {
                qWarning() << "[Auth] 서버 세션 삭제 실패:" << why
                           << "— 로컬 로그아웃은 그대로 진행";
            });
    }

    clear_session();
    m_username.clear();
    m_display_name.clear();
    m_token.clear();
    m_expires_at = QDateTime();
    m_verified_at = QDateTime();
    // 다음 사람이 이 자리에 앉는다 — 앞사람의 재확인을 물려주지 않는다
    m_human_auth_at = QDateTime();
    m_human_login = false;
    m_verified = false;
    m_role = Role::Operator;
    set_state(State::LoggedOut);
    qInfo() << "[Auth] 로그아웃";
}

// ------------------------------------------------- 자동 로그인 (§4a) · 유예(§4b)

void Auth::resume()
{
    if (!load_session()) {
        qInfo() << "[Auth] 저장된 세션 없음 — 로그인 화면";
        return;
    }

    // 만료는 서버에 묻지 않아도 안다. 만료된 토큰으로 검증을 보내면 브로커가
    // 죽어 있을 때 그 세션이 유예로 부활한다 — 그건 30일 정책을 우회하는 것이다.
    if (m_expires_at.isValid() && m_expires_at <= QDateTime::currentDateTime()) {
        qInfo() << "[Auth] 저장된 세션 만료 — 지운다";
        clear_session();
        m_token.clear();
        return;
    }

    m_resuming = true;
    ++m_epoch;
    set_state(State::Checking);
    qInfo().noquote() << QString("[Auth] 저장된 세션으로 자동 로그인 시도: %1")
                             .arg(m_username);

    if (stub_enabled()) {
        const quint64 epoch = m_epoch;
        QTimer::singleShot(STUB_DELAY_MS, this, [this, epoch] {
            if (epoch != m_epoch)
                return;
            if (stub_offline()) {
                qWarning() << "[Auth] 스텁 오프라인 흉내 — 유예 경로로 간다";
                enter_offline_grace();
                return;
            }
            // 스텁은 자기가 발급한 토큰을 늘 인정한다
            m_verified = true;
            m_verified_at = QDateTime::currentDateTime();
            store_session();
            finish_resume();
            set_state(State::LoggedIn);
            emit verification_changed(true);
        });
        return;
    }

    if (MqttLink::instance()->online()) {
        verify_token();
        return;
    }

    // 아직 브로커에 안 붙었다. 생성자에 걸어둔 online_changed 가 먼저 오면
    // 그때 검증하고, 안 오면 이 타이머가 유예를 판단한다.
    if (!m_broker_wait) {
        m_broker_wait = new QTimer(this);
        m_broker_wait->setSingleShot(true);
        connect(m_broker_wait, &QTimer::timeout, this, [this] {
            if (m_state != State::Checking)
                return;
            qInfo() << "[Auth] 브로커 미연결 —" << BROKER_WAIT_MS / 1000
                    << "초 기다렸다 유예 판단";
            enter_offline_grace();
        });
    }
    m_broker_wait->start(BROKER_WAIT_MS);
}

void Auth::verify_token()
{
    if (m_token.isEmpty()) {
        enter_offline_grace();
        return;
    }
    const quint64 epoch = m_epoch;
    QJsonObject params;
    params["token"] = m_token;

    m_req_id = MqttLink::instance()->request(
        TOPIC_SESSION_CHECK, params,
        [this, epoch](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            m_req_id.clear();
            const bool was_offline = (m_state == State::LoggedIn && !m_verified);
            apply_ok(reply);                 // verified=true 로 승격 + 저장
            if (was_offline)
                qInfo() << "[Auth] 재검증 성공 — 읽기 전용 해제";
        },
        // 브로커에 못 닿았거나 응답이 없다 → 서버 판정이 아니다 → 유예로 간다
        [this, epoch](const QString &why) {
            if (epoch != m_epoch) return;
            m_req_id.clear();
            qWarning() << "[Auth] 세션 검증 실패:" << why;
            enter_offline_grace();
        },
        MqttLink::DEFAULT_TIMEOUT_MS,
        // 서버가 거절했다 → 이 세션은 진짜 무효다. 유예로 살리지 않는다.
        [this, epoch](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            m_req_id.clear();
            const QString reason = reply.value("reason").toString();
            qInfo().noquote() << QString("[Auth] 저장된 세션 거절: %1").arg(reason);
            clear_session();
            m_token.clear();
            m_verified = false;
            finish_resume();
            set_state(State::LoggedOut);
            emit login_failed(reason.isEmpty()
                                  ? QString::fromLatin1(REASON_EXPIRED) : reason, 0);
        });
}

void Auth::enter_offline_grace()
{
    finish_resume();

    const QDateTime now = QDateTime::currentDateTime();
    // 정수 연산만 — libm 호출은 CMakeLists 의 가드레일에 걸린다.
    const qint64 age_s = m_verified_at.isValid() ? m_verified_at.secsTo(now) : -1;
    const qint64 limit_s = qint64(GRACE_HOURS) * 3600;

    if (age_s < 0 || age_s > limit_s) {
        qInfo().noquote()
            << QString("[Auth] 오프라인 유예 초과(%1h > %2h) — 로그인 필요")
                   .arg(age_s < 0 ? -1 : age_s / 3600).arg(GRACE_HOURS);
        set_state(State::LoggedOut);
        return;
    }

    // 캐시된 역할로 통과시키되 **읽기 전용**이다(§4b). 관제 화면이 안 뜨는
    // 것은 화재를 놓치는 것과 같은 급의 사고라, 못 들어가게 하지 않는다.
    m_verified = false;
    qInfo().noquote()
        << QString("[Auth] 오프라인 유예 진입 — %1(%2) · 마지막 확인 %3 (%4시간 전) · 읽기 전용")
               .arg(m_username, role_text(m_role),
                    m_verified_at.toString("MM-dd HH:mm"))
               .arg(age_s / 3600);
    set_state(State::LoggedIn);
    emit verification_changed(false);
}

void Auth::finish_resume()
{
    m_resuming = false;
    if (m_broker_wait)
        m_broker_wait->stop();
}

// ------------------------------------------------------------- 세션 보관 (§4a)

void Auth::store_session()
{
    QSettings s("GuardX", "VMS");
    // 토큰만 DPAPI 로 감싼다. 평문으로 두면 같은 PC의 다른 사용자에게 그대로
    // 넘어간다. 나머지(아이디·역할)는 비밀이 아니다 — 게다가 역할을 손으로
    // 고쳐 봐야 오프라인 진입은 어차피 읽기 전용이고, 온라인이면 서버 응답이
    // 덮어쓴다.
    s.setValue("auth/session_token", Credentials::protect(m_token));
    s.setValue("auth/session_user", m_username);
    s.setValue("auth/session_display_name", m_display_name);
    s.setValue("auth/session_role", m_role == Role::Admin ? "admin" : "operator");
    s.setValue("auth/session_expires_at",
               m_expires_at.isValid() ? m_expires_at.toString(Qt::ISODate) : QString());
    s.setValue("auth/session_verified_at",
               m_verified_at.isValid() ? m_verified_at.toString(Qt::ISODate) : QString());
}

bool Auth::load_session()
{
    QSettings s("GuardX", "VMS");
    const QString stored = s.value("auth/session_token").toString();
    if (stored.isEmpty())
        return false;

    const QString token = Credentials::unprotect(stored);
    if (token.isEmpty()) {
        // 다른 계정/PC 에서 만든 암호문은 DPAPI 가 못 푼다 — 그게 정상이다.
        qWarning() << "[Auth] 저장된 토큰을 복호화하지 못했다 — 지운다";
        clear_session();
        return false;
    }

    m_token = token;
    m_username = s.value("auth/session_user").toString();
    m_display_name = s.value("auth/session_display_name").toString();
    m_role = s.value("auth/session_role").toString() == QLatin1String("admin")
                 ? Role::Admin : Role::Operator;
    m_expires_at = QDateTime::fromString(
        s.value("auth/session_expires_at").toString(), Qt::ISODate);
    m_verified_at = QDateTime::fromString(
        s.value("auth/session_verified_at").toString(), Qt::ISODate);
    return true;
}

void Auth::clear_session()
{
    QSettings s("GuardX", "VMS");
    s.remove("auth/session_token");
    s.remove("auth/session_user");
    s.remove("auth/session_display_name");
    s.remove("auth/session_role");
    s.remove("auth/session_expires_at");
    s.remove("auth/session_verified_at");
}

void Auth::submit(const QString &username, const QString &password)
{
    const quint64 epoch = m_epoch;

    if (stub_enabled()) {
        // 개발용. 이 줄이 배포 로그에 보이면 인증이 서버를 안 거치고 있다.
        // (로그 파일은 cp949 라 ⚠ 같은 기호가 '?' 로 깨진다 — 한글로 적는다.)
        qWarning() << "[Auth] 스텁 로그인 — 서버 검증 없음"
                   << "(레지스트리 auth/stub=0 으로 끈다):" << username;

        if (stub_offline()) {
            // 서버에 닿지 못한 것처럼 답한다 — 실경로의 타임아웃과 같은 사유라
            // 화면 문구도 같은 것이 뜬다.
            QTimer::singleShot(STUB_DELAY_MS, this, [this, epoch] {
                if (epoch != m_epoch)
                    return;
                qWarning() << "[Auth] 스텁 오프라인 흉내 — 로그인 불가";
                fail(QString::fromLatin1(REASON_UNREACHABLE), 0);
            });
            return;
        }

        const QJsonObject reply = stub_reply(username, password);
        QTimer::singleShot(STUB_DELAY_MS, this, [this, epoch, reply] {
            if (epoch != m_epoch)
                return;                       // 그 사이 로그아웃/재시도
            if (reply.value("ok").toBool())
                apply_ok(reply);
            else
                apply_fail(reply);
        });
        return;
    }

    QJsonObject params;
    params["username"] = username;
    params["password"] = password;   // ⚠ 평문 — mTLS(단계 0c) 이후에만 유효한 설계
    // 감사용(`vms_session.device`). 규약은 `updated_by` 와 같은 "vms {client_id}".
    // 선택 필드지만 안 보내면 세션에 기기 표기가 안 남는다 — 누가 어디서
    // 들어왔는지가 사고 때 가장 먼저 필요한 정보다.
    params["device"] = QString("vms %1").arg(MqttLink::instance()->client_id());
    // 감사용(선택). `vms_session.device` 에 그대로 들어간다 — 기존 updated_by 와
    // 같은 "vms {client_id}" 형식. 안 보내면 세션에 기기 표기가 안 남는다.
    params["device"] = QString("vms %1").arg(MqttLink::instance()->client_id());

    m_req_id = MqttLink::instance()->request(
        TOPIC_LOGIN, params,
        // 성공
        [this, epoch](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            m_req_id.clear();
            apply_ok(reply);
        },
        // 타임아웃·미연결 — 서버가 거절한 것이 **아니다**
        [this, epoch](const QString &why) {
            if (epoch != m_epoch) return;
            m_req_id.clear();
            qWarning() << "[Auth] 로그인 요청 실패:" << why;
            fail(QString::fromLatin1(REASON_UNREACHABLE), 0);
        },
        MqttLink::DEFAULT_TIMEOUT_MS,
        // ok:false — 사유를 해석해야 하므로 객체째 받는다
        [this, epoch](const QJsonObject &reply) {
            if (epoch != m_epoch) return;
            m_req_id.clear();
            apply_fail(reply);
        });
}

void Auth::apply_ok(const QJsonObject &reply)
{
    const QString role_s = reply.value("role").toString();

    Role role;
    if (role_s == QLatin1String("admin")) {
        role = Role::Admin;
    } else if (role_s == QLatin1String("operator")) {
        role = Role::Operator;
    } else {
        // 모르는 역할은 **운영자로 강등**한다. 거절하는 편이 엄격해 보이지만,
        // 관제 화면이 안 뜨는 것은 화재를 놓치는 것과 같은 급의 사고다(§4b).
        // 강등이면 보기는 되고 위험 조작만 막힌다 — 안전한 쪽의 오류다.
        qWarning() << "[Auth] 모르는 역할 → 운영자로 강등:" << role_s;
        role = Role::Operator;
    }

    m_role = role;
    m_username = reply.value("username").toString(m_username);
    const QString name = reply.value("display_name").toString();
    if (!name.isEmpty())
        m_display_name = name;
    if (m_display_name.isEmpty())
        m_display_name = m_username;   // 표시할 이름이 없으면 아이디를 쓴다

    // ⚠ `session_check` 응답에는 토큰이 없다 — 덮어쓰면 가지고 있던 세션을
    //   스스로 지우는 꼴이 된다. 새 토큰이 실려 왔을 때만 바꾼다.
    const QString tok = reply.value("token").toString();
    if (!tok.isEmpty())
        m_token = tok;
    m_expires_at = parse_expires_at(reply.value("expires_at"));

    if (m_token.isEmpty()) {
        // 자동 로그인(§4a)이 이걸로 산다. 조용히 넘어가면 나중에 "왜 매번
        // 로그인 화면이 뜨지"를 여기까지 거슬러 와야 한다.
        qWarning() << "[Auth] 토큰이 없다 — 자동 로그인 불가";
    }
    if (!m_expires_at.isValid())
        qWarning() << "[Auth] expires_at 을 읽지 못했다:"
                   << reply.value("expires_at");

    // 서버가 확인해 준 순간이다 — 유예 계산의 기준점이자 읽기 전용 해제 지점.
    m_verified = true;
    m_verified_at = QDateTime::currentDateTime();

    // ⚠ **사람이 친 로그인일 때만** 재확인 시계를 돌린다. 이 함수는 자동
    //   로그인(resume)의 session_check 응답으로도 들어오는데, 저장된 토큰이
    //   유효하다는 것과 "지금 이 자리에 있는 사람이 계정 주인이다"는 다른
    //   사실이다. 여기서 구분하지 않으면 켜 둔 PC 가 영원히 신선해진다.
    if (m_human_login) {
        m_human_auth_at = m_verified_at;
        m_human_login = false;
    }

    store_session();
    finish_resume();

    // §5b — 강제 변경. **없으면 false 로 읽는다**(서버가 아직 안 실어도 안 깨진다).
    if (reply.value("must_change").toBool(false)) {
        qInfo().noquote()
            << QString("[Auth] %1 은 비밀번호를 바꿔야 한다 — 화면 진입 보류")
                   .arg(m_username);
        set_state(State::MustChangePassword);
        emit verification_changed(true);
        return;
    }

    qInfo().noquote() << QString("[Auth] 로그인 성공: %1 (%2)%3")
                             .arg(m_username, role_text(m_role),
                                  m_expires_at.isValid()
                                      ? QString(" · 만료 %1").arg(
                                            m_expires_at.toString("MM-dd HH:mm"))
                                      : QString());
    set_state(State::LoggedIn);
    emit verification_changed(true);
}

void Auth::apply_fail(const QJsonObject &reply)
{
    QString reason = reply.value("reason").toString();
    if (reason.isEmpty()) {
        // 계약을 어긴 응답. 오입력으로 뭉뚱그리면 "비밀번호가 틀렸나"를
        // 세 번 치게 되므로 다른 사유로 남긴다.
        qWarning() << "[Auth] reason 없는 실패 응답:"
                   << QJsonDocument(reply).toJson(QJsonDocument::Compact);
        reason = QString::fromLatin1(REASON_BAD_REPLY);
    }
    // 숫자로 오는 것이 계약이지만 문자열("60")로 와도 읽는다. QJsonValue::toInt()
    // 는 문자열이면 **조용히 0** 을 준다 — 그러면 잠금 카운트다운이 사라지고
    // "0초 후 다시 시도"라는 거짓 문구가 뜬다. 서버 한 줄에 화면이 깨지는 자리라
    // 받는 쪽에서 관대하게 읽는다.
    const QJsonValue retry = reply.value("retry_after_s");
    int retry_s = retry.toInt(0);
    if (retry_s == 0 && retry.isString())
        retry_s = retry.toString().toInt();

    fail(reason, retry_s);
}

void Auth::fail(const QString &reason, int retry_after_s)
{
    m_human_login = false;
    m_token.clear();
    m_expires_at = QDateTime();
    set_state(State::LoggedOut);
    qInfo().noquote() << QString("[Auth] 로그인 실패: %1%2")
                             .arg(reason,
                                  retry_after_s > 0
                                      ? QString(" (%1초)").arg(retry_after_s)
                                      : QString());
    emit login_failed(reason, retry_after_s);
}

QJsonObject Auth::stub_reply(const QString &username, const QString &password)
{
    QJsonObject r;
    const QDateTime now = QDateTime::currentDateTime();
    StubLock &lock = m_stub_locks[username];

    // ① 잠금이 먼저다 — 잠긴 동안은 비밀번호를 보지 않는다(횟수도 안 올린다).
    if (lock.locked_until.isValid() && lock.locked_until > now) {
        r["ok"] = false;
        r["reason"] = QString::fromLatin1(REASON_LOCKED);
        r["retry_after_s"] = static_cast<int>(now.secsTo(lock.locked_until));
        return r;
    }

    // 스텁 가변 상태를 처음 한 번 채운다 — 이후에는 여기가 진실이다
    // (비밀번호 변경·계정 생성이 반영돼야 §5b 를 서버 없이 시험할 수 있다).
    ensure_stub_seed();

    const StubMutable *acct = nullptr;
    const auto it = g_stub_state.constFind(username);
    if (it != g_stub_state.cend())
        acct = &it.value();

    // ② 계정이 없는 것과 비밀번호가 틀린 것을 **같은 사유**로 답한다.
    //    구분해 주면 계정 존재 여부를 알려주는 꼴이다(§2b).
    if (!acct || password != acct->password) {
        ++lock.failed;
        if (lock.failed >= STUB_HARD_LOCK_AT)
            lock.locked_until = now.addSecs(STUB_HARD_LOCK_SEC);
        else if (lock.failed >= STUB_LOCK_AT)
            lock.locked_until = now.addSecs(STUB_LOCK_SEC);

        r["ok"] = false;
        if (lock.locked_until.isValid() && lock.locked_until > now) {
            r["reason"] = QString::fromLatin1(REASON_LOCKED);
            r["retry_after_s"] = static_cast<int>(now.secsTo(lock.locked_until));
        } else {
            r["reason"] = QString::fromLatin1(REASON_BAD_CREDENTIALS);
        }
        return r;
    }

    if (!acct->enabled) {
        r["ok"] = false;
        r["reason"] = QString::fromLatin1(REASON_DISABLED);
        return r;
    }

    // ③ 성공 — 실패 장부를 지우고 계약과 같은 모양으로 돌려준다.
    lock = StubLock{};

    quint32 raw[8];   // 32바이트 — 서버가 발급하는 것과 같은 길이
    QRandomGenerator::global()->fillRange(raw);
    const QByteArray token(reinterpret_cast<const char *>(raw), sizeof(raw));

    r["ok"] = true;
    r["username"] = username;
    r["role"] = acct->role;
    // ⚠ 한글이라 **fromUtf8** 이어야 한다. fromLatin1 이면 "운영자(스텁)" 이
    //   상단바 사용자 칩에서 깨진 글자로 나온다(단계 5에서 표시 자리가
    //   생기고 나서야 드러났다).
    r["display_name"] = acct->display_name;
    r["token"] = QString::fromLatin1(token.toHex());
    r["expires_at"] =
        QDateTime::currentDateTimeUtc().addDays(30).toString(Qt::ISODate);
    if (acct->must_change)
        r["must_change"] = true;   // §5b — 계약과 같은 모양
    return r;
}

// ---------------------------------------------------------------- 자가시험
//
// 화면이 없는 단계에서 로그인 경로를 확인하는 유일한 방법이다. GUI 없이
// 돌기 때문에 나중에 CI 에서도 쓸 수 있다. `gstream_VMS --auth-selftest`.

namespace {

struct Attempt {
    bool ok = false;
    QString reason;
    int retry_after_s = 0;
};

/** @brief 로그인 1회를 동기적으로 돌린다 (시그널을 기다리는 중첩 이벤트 루프) */
Attempt run_attempt(Auth *a, const QString &user, const QString &pass)
{
    Attempt out;
    QEventLoop loop;

    // 결과가 exec() 전에 도착해도 매달리지 않게 — quit()은 루프가 돌기 전엔
    // 아무 일도 하지 않는다. (Auth 는 결과를 항상 비동기로 알리지만,
    // 시험 도구가 그 규약에 의존해 12초씩 굳어 있으면 안 된다.)
    bool done = false;

    const auto c1 = QObject::connect(
        a, &Auth::state_changed, &loop, [&](Auth::State s) {
            if (s == Auth::State::LoggedIn) {
                out.ok = true;
                done = true;
                loop.quit();
            }
        });
    const auto c2 = QObject::connect(
        a, &Auth::login_failed, &loop, [&](const QString &reason, int retry) {
            out.reason = reason;
            out.retry_after_s = retry;
            done = true;
            loop.quit();
        });

    // 아무 시그널도 안 오는 경우(백스톱에 걸려 무시됐다든가)에 매달리지 않게
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
        out.reason = QStringLiteral("(응답 없음)");
        loop.quit();
    });
    guard.start(MqttLink::DEFAULT_TIMEOUT_MS + 4000);

    a->login(user, pass);
    if (!done)
        loop.exec();

    QObject::disconnect(c1);
    QObject::disconnect(c2);
    return out;
}

/** @brief `resume()` 1회를 동기적으로 돌린다 (자동 로그인 경로 시험용) */
void run_resume(Auth *a)
{
    QEventLoop loop;
    bool done = false;
    const auto c = QObject::connect(a, &Auth::state_changed, &loop,
                                    [&](Auth::State s) {
        if (s == Auth::State::Checking)
            return;                    // 진행 중 — 결론은 그다음 전이다
        done = true;
        loop.quit();
    });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] { loop.quit(); });
    guard.start(MqttLink::DEFAULT_TIMEOUT_MS + 9000);

    a->resume();
    // 저장된 세션이 없으면 resume() 은 아무 전이도 내지 않는다 — 그 경우
    // 이미 결론(LoggedOut)이므로 루프에 들어가지 않는다.
    if (!done && a->state() == Auth::State::Checking)
        loop.exec();
    QObject::disconnect(c);
}

/** @brief `password_changed`/`user_created` 처럼 (bool, QString) 시그널을 기다린다 */
template <typename Sig>
QPair<bool, QString> wait_result(Auth *a, Sig sig, const std::function<void()> &fire)
{
    QPair<bool, QString> out(false, QStringLiteral("(응답 없음)"));
    QEventLoop loop;
    bool done = false;
    const auto c = QObject::connect(a, sig, &loop,
                                    [&](bool ok, const QString &reason) {
        out = { ok, reason };
        done = true;
        loop.quit();
    });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] { loop.quit(); });
    guard.start(MqttLink::DEFAULT_TIMEOUT_MS + 4000);
    fire();
    if (!done)
        loop.exec();
    QObject::disconnect(c);
    return out;
}

/** @brief `list_users()` 한 번 (계정 목록 시나리오용) */
QPair<bool, QVector<Auth::UserRow>> wait_list(Auth *a)
{
    QPair<bool, QVector<Auth::UserRow>> out(false, {});
    QEventLoop loop;
    bool done = false;
    const auto c = QObject::connect(
        a, &Auth::users_listed, &loop,
        [&](bool ok, const QVector<Auth::UserRow> &users, const QString &) {
            out = { ok, users };
            done = true;
            loop.quit();
        });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] { loop.quit(); });
    guard.start(MqttLink::DEFAULT_TIMEOUT_MS + 4000);
    a->list_users();
    if (!done)
        loop.exec();
    QObject::disconnect(c);
    return out;
}

/** @brief `set_user_enabled()` 한 번. 돌아오는 것은 (성공, 사유) */
QPair<bool, QString> wait_enable(Auth *a, const QString &user, bool enabled)
{
    QPair<bool, QString> out(false, QStringLiteral("(응답 없음)"));
    QEventLoop loop;
    bool done = false;
    const auto c = QObject::connect(
        a, &Auth::user_enabled_changed, &loop,
        [&](bool ok, const QString &, bool, const QString &reason) {
            out = { ok, reason };
            done = true;
            loop.quit();
        });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] { loop.quit(); });
    guard.start(MqttLink::DEFAULT_TIMEOUT_MS + 4000);
    a->set_user_enabled(user, enabled);
    if (!done)
        loop.exec();
    QObject::disconnect(c);
    return out;
}

/** @brief `reauthenticate()` 한 번. 돌아오는 것은 (성공, 사유) */
QPair<bool, QString> wait_reauth(Auth *a, const QString &password)
{
    QPair<bool, QString> out(false, QStringLiteral("(응답 없음)"));
    QEventLoop loop;
    bool done = false;
    const auto c = QObject::connect(
        a, &Auth::reauth_result, &loop,
        [&](bool ok, const QString &reason, int) {
            out = { ok, reason };
            done = true;
            loop.quit();
        });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] { loop.quit(); });
    guard.start(MqttLink::DEFAULT_TIMEOUT_MS + 4000);
    a->reauthenticate(password);
    if (!done)
        loop.exec();
    QObject::disconnect(c);
    return out;
}

int g_fail_count = 0;

void check(bool cond, const QString &what)
{
    if (!cond)
        ++g_fail_count;
    qInfo().noquote() << (cond ? "  [OK]  " : "  [실패]") << what;
}

} // namespace

int Auth::selftest()
{
    Auth *a = instance();

    // 오프라인 흉내는 로그인 자체가 불가능한 모드다 — 시나리오를 통째로 가른다.
    // (같은 시나리오를 돌리면 "로그인 성공" 검사가 전부 실패로 잡혀 정작
    //  보려던 유예 판정이 소음에 묻힌다.)
    if (stub_enabled() && stub_offline())
        return selftest_offline();

    qInfo().noquote() << "[Auth] 자가시험 시작 — 스텁"
                      << (stub_enabled() ? "사용" : "미사용(실서버)");
    if (!stub_enabled()) {
        qWarning().noquote()
            << "  ⚠ auth/stub=0 이다. 이 시험은 RPi B 의 로그인 핸들러가 서 있어야"
               " 의미가 있다 — 아직이면 레지스트리 auth/stub 을 1 로 두고 다시 실행.";
    }

    g_fail_count = 0;

    // ① 정상 로그인
    Attempt r = run_attempt(a, "admin", "admin");
    check(r.ok, "admin 로그인 성공");
    check(a->logged_in(), "상태 = LoggedIn");
    check(a->role() == Role::Admin, "역할 = 관리자");
    check(!a->token().isEmpty(), "토큰 수신");
    check(a->expires_at().isValid(), "만료 시각 해석 성공");

    // ② 로그아웃
    a->logout();
    check(!a->logged_in(), "로그아웃 후 상태 = LoggedOut");
    check(a->token().isEmpty(), "로그아웃 후 토큰 제거");

    // ③ 빈 입력은 서버까지 가지 않는다
    r = run_attempt(a, "", "");
    check(!r.ok && r.reason == QLatin1String(REASON_BAD_CREDENTIALS),
          "빈 입력 → bad_credentials");

    if (!stub_enabled()) {
        qInfo() << "[Auth] 잠금 시험은 스텁에서만 돈다 — 여기서 멈춘다";
        qInfo().noquote() << QString("[Auth] 자가시험 종료 — 실패 %1건")
                                 .arg(g_fail_count);
        return g_fail_count == 0 ? 0 : 1;
    }

    // ③-2 운영자 권한 (§5) — **잠금 시험보다 먼저** 해야 한다.
    //     뒤에 두면 아래 ④가 operator 계정을 잠가 버려 로그인 자체가 실패한다
    //     (실제로 그렇게 짰다가 시험이 거짓 통과할 뻔했다).
    r = run_attempt(a, "operator", "operator");
    check(r.ok && a->role() == Role::Operator, "운영자 로그인");
    check(!Auth::can(Action::ActuatorControl) && !Auth::can(Action::ZoneSettings),
          "운영자: 쓰기 거부 (§5 표 전체가 관리자 전용)");
    check(Auth::deny_reason(Action::ZoneSettings).contains("Administrator"),
          "운영자 사유 문구");
    a->logout();

    // ④ 오입력 4회는 오입력으로, 5회째에 잠긴다 (핸드오프 §5)
    bool all_bad = true;
    for (int i = 0; i < STUB_LOCK_AT - 1; ++i) {
        r = run_attempt(a, "operator", "wrong");
        all_bad = all_bad && !r.ok
                  && r.reason == QLatin1String(REASON_BAD_CREDENTIALS);
    }
    check(all_bad, "오입력 4회 → bad_credentials");

    r = run_attempt(a, "operator", "wrong");
    check(!r.ok && r.reason == QLatin1String(REASON_LOCKED),
          "오입력 5회째 → locked");
    check(r.retry_after_s > 0 && r.retry_after_s <= STUB_LOCK_SEC,
          QString("남은 초가 실려 온다 (%1s)").arg(r.retry_after_s));

    // ⑤ 잠긴 동안에는 올바른 비밀번호도 통과하지 못한다
    r = run_attempt(a, "operator", "operator");
    check(!r.ok && r.reason == QLatin1String(REASON_LOCKED),
          "잠긴 동안에는 정답도 거절");

    // ⑥ 사용 중지 계정
    r = run_attempt(a, "disabled", "disabled");
    check(!r.ok && r.reason == QLatin1String(REASON_DISABLED),
          "사용 중지 계정 → disabled");

    // ⑦ 다른 계정은 잠금에 걸리지 않는다 (잠금은 계정 단위)
    r = run_attempt(a, "admin", "admin");
    check(r.ok && a->role() == Role::Admin, "다른 계정은 그대로 로그인");

    // ⑦-2 권한 판정 (§5) — 지금은 표의 모든 항목이 관리자 전용이다
    check(Auth::can(Action::ActuatorControl) && Auth::can(Action::CameraSystem),
          "관리자: 쓰기 허용");
    a->logout();
    check(!Auth::can(Action::ActuatorControl), "미로그인: 쓰기 거부");
    check(Auth::deny_reason(Action::ActuatorControl)
              .contains("Sign in"),
          "미로그인 사유 문구");
    r = run_attempt(a, "admin", "admin");
    check(r.ok, "다시 관리자 로그인");

    // ⑦-3 비밀번호 변경 · 계정 생성 (§5b)
    // 길이·복잡도 정책은 08-12 에 폐지됐다(서버가 먼저). 남은 거부는 빈 값 하나다 —
    // 짧은 비밀번호·한글 바이트 수 시험 4건은 그래서 지웠다.
    {
        auto empty = wait_result(a, &Auth::password_changed,
                                 [a] { a->change_password("admin", QString()); });
        check(!empty.first
                  && empty.second == QLatin1String(REASON_WEAK_PASSWORD),
              "빈 비밀번호 → weak_password (폐지 후 유일한 거부)");
        check(a->logged_in(), "거부돼도 세션은 그대로다");
    }

    {
        const QString before = a->token();
        auto r1 = wait_result(a, &Auth::password_changed,
                              [a] { a->change_password("wrong-current", "brandnew123"); });
        check(!r1.first && r1.second == QLatin1String(REASON_BAD_CREDENTIALS),
              "현재 비밀번호가 틀리면 거부");

        auto r2 = wait_result(a, &Auth::password_changed,
                              [a] { a->change_password("admin", "brandnew123"); });
        check(r2.first, "비밀번호 변경 성공");
        check(a->token() != before && !a->token().isEmpty(),
              "변경 후 **새 토큰**으로 갈아 끼운다 (기존 세션 무효화 대응)");

        // 바뀐 비밀번호가 실제로 반영됐는지 — 옛 비번은 이제 안 된다
        a->logout();
        Attempt old_pw = run_attempt(a, "admin", "admin");
        check(!old_pw.ok, "옛 비밀번호로는 못 들어간다");
        Attempt new_pw = run_attempt(a, "admin", "brandnew123");
        check(new_pw.ok, "새 비밀번호로 로그인된다");
    }

    // ⑦-3b 공백 보존 (08-12) — C 가 실기로 확인한 것을 여기 못 박는다.
    //  어딘가에서 trimmed() 를 한 줄만 걸어도 이 세 검사 중 하나는 깨진다.
    {
        const QString spaces3 = QStringLiteral("   ");
        auto to_spaces = wait_result(a, &Auth::password_changed,
                                     [a, spaces3] {
            a->change_password("brandnew123", spaces3);
        });
        check(to_spaces.first, "공백 3개짜리 비밀번호로 바꿀 수 있다");

        a->logout();
        check(!run_attempt(a, "admin", " ").ok,
              "공백 1개로는 못 들어간다 — 개수까지 보존된다");
        check(run_attempt(a, "admin", spaces3).ok, "같은 공백 3개로 로그인된다");

        // 뒤 시나리오가 brandnew123 을 쓰므로 되돌린다
        auto restore = wait_result(a, &Auth::password_changed,
                                   [a, spaces3] {
            a->change_password(spaces3, "brandnew123");
        });
        check(restore.first, "비밀번호 원복");
    }

    {
        auto d = wait_result(a, &Auth::user_created, [a] {
            a->create_user("operator", "중복", Role::Operator);
        });
        check(!d.first && d.second == QLatin1String(REASON_DUPLICATE),
              "이미 있는 아이디는 duplicate");

        auto ok = wait_result(a, &Auth::user_created, [a] {
            a->create_user("tester1", "시험 계정", Role::Operator);
        });
        check(ok.first, "계정 생성 성공");

        // 만든 계정은 첫 로그인에서 비밀번호를 바꿔야 한다 (§5b)
        a->logout();
        Attempt t = run_attempt(a, "tester1", initial_password());
        check(!t.ok, "새 계정은 곧바로 LoggedIn 이 되지 않는다");
        check(a->state() == State::MustChangePassword,
              "새 계정 첫 로그인 → 강제 변경 상태");
        check(!Auth::can(Action::ZoneSettings),
              "강제 변경 중에는 아무 권한도 없다");

        auto ch = wait_result(a, &Auth::password_changed, [a] {
            a->change_password(initial_password(), "changed12345");
        });
        check(ch.first && a->logged_in(), "바꾸면 그대로 진입한다");
        a->logout();
        Attempt back = run_attempt(a, "admin", "brandnew123");
        check(back.ok, "관리자로 복귀");
    }

    // ⑦-4 계정 목록 · 사용 중지/재개 (08-12)
    {
        auto listed = wait_list(a);
        check(listed.first, "관리자: 계정 목록 수신");

        bool sorted = true;
        for (int i = 1; i < listed.second.size(); ++i)
            sorted = sorted && (listed.second[i - 1].username
                                <= listed.second[i].username);
        check(sorted, "목록은 username 오름차순 (서버 정렬과 같다)");

        const Auth::UserRow *off = nullptr;
        for (const Auth::UserRow &u : listed.second)
            if (u.username == QLatin1String("disabled"))
                off = &u;
        // ⚠ 비활성 계정이 목록에서 빠지면 **재활성할 방법이 화면에서 사라진다**.
        check(off && !off->enabled, "사용 중지 계정도 목록에 있고 상태가 보인다");

        // 그 계정은 로그인은 못 하지만 아이디는 계속 점유한다 —
        // 같은 아이디로 새로 만들려 하면 duplicate 다(재활성이 정상 경로).
        auto dup_off = wait_result(a, &Auth::user_created, [a] {
            a->create_user("disabled", "겹침", Role::Operator);
        });
        check(!dup_off.first
                  && dup_off.second == QLatin1String(REASON_DUPLICATE),
              "비활성 계정의 아이디도 duplicate 로 막힌다");

        check(wait_enable(a, "nosuchuser", false).second
                  == QLatin1String(REASON_NOT_FOUND),
              "없는 아이디 → not_found");

        // 지금 관리자는 admin 하나뿐이다 — 자기를 끄면 아무도 계정을 못 만든다.
        check(wait_enable(a, "admin", false).second
                  == QLatin1String(REASON_LAST_ADMIN),
              "마지막 관리자는 못 끈다 → last_admin");

        auto made = wait_result(a, &Auth::user_created, [a] {
            a->create_user("admin2", "둘째 관리자", Role::Admin);
        });
        check(made.first, "두 번째 관리자 생성");
        // 관리자가 둘이 됐으니 이제 막는 이유가 바뀐다 — 여전히 못 끄지만
        // 사유가 "마지막이라서"가 아니라 "네 계정이라서"다.
        check(wait_enable(a, "admin", false).second
                  == QLatin1String(REASON_SELF_TARGET),
              "자기 계정은 못 끈다 → self_target");

        auto off2 = wait_enable(a, "admin2", false);
        check(off2.first, "다른 관리자는 끌 수 있다 (관리자가 둘 이상일 때)");

        // 재활성이 정상 경로 — 껐던 계정이 다시 로그인된다
        check(wait_enable(a, "disabled", true).first, "사용 중지 계정 재개");
        a->logout();
        Attempt revived = run_attempt(a, "disabled", "disabled");
        check(revived.ok, "재개한 계정으로 로그인된다");

        // 운영자는 목록도 못 보고 바꾸지도 못한다 (§5 — 서버도 forbidden)
        a->logout();
        // ⚠ operator 계정은 앞의 잠금 시험(④)으로 60초 잠겨 있다. 같은 역할의
        //   다른 계정(tester1)으로 확인한다 — 잠금이 풀리길 기다리면 자가시험이
        //   1분씩 멈춘다.
        Attempt as_op = run_attempt(a, "tester1", "changed12345");
        check(as_op.ok && a->role() == Role::Operator, "운영자(tester1) 로그인");
        check(!wait_list(a).first, "운영자: 계정 목록 거부");
        check(wait_enable(a, "admin2", true).second
                  == QLatin1String(REASON_FORBIDDEN),
              "운영자: 사용여부 변경 거부 → forbidden");

        a->logout();
        check(run_attempt(a, "admin", "brandnew123").ok, "관리자로 복귀");
    }

    // ⑦-5 비밀번호 재확인 (08-13) — 설정 쓰기 전에 한 번 더 묻는 경로
    {
        check(a->fresh(), "사람이 친 로그인 직후는 신선하다");

        // 10분을 기다릴 수 없으니 시계를 직접 되돌린다 (자가시험은 Auth 의
        // 멤버라 private 에 닿는다 — 이 시험을 위해 API 를 넓히지 않는다).
        a->m_human_auth_at =
            QDateTime::currentDateTime().addSecs(-(FRESH_MINUTES * 60 + 1));
        check(!a->fresh(), "FRESH_MINUTES 가 지나면 신선하지 않다");

        auto bad = wait_reauth(a, "wrong-password");
        check(!bad.first && bad.second == QLatin1String(REASON_BAD_CREDENTIALS),
              "틀린 비밀번호 → 거부");
        // ⚠ 로그인 실패와 다르다. 재확인이 틀렸다고 세션을 끊으면 보던 화면을
        //   통째로 잃는다.
        check(a->logged_in() && !a->token().isEmpty(),
              "재확인 실패가 세션을 건드리지 않는다");
        check(!a->fresh(), "실패로는 시계가 돌지 않는다");

        auto ok = wait_reauth(a, "brandnew123");
        check(ok.first, "맞는 비밀번호 → 통과");
        check(a->fresh(), "통과하면 다시 신선해진다");

        // ⚠ 자동 로그인은 시계를 돌리지 않는다 — "저장된 토큰이 유효하다"와
        //   "지금 이 자리에 있는 사람이 계정 주인이다"는 다른 사실이다.
        a->m_human_auth_at = QDateTime();
        run_resume(a);
        check(a->logged_in(), "자동 로그인 자체는 성공한다");
        check(!a->fresh(), "자동 로그인은 재확인 시계를 돌리지 않는다");

        auto back = wait_reauth(a, "brandnew123");
        check(back.first, "재확인으로 다시 신선해진다");
    }

    // ⑧ 세션 보관 · 자동 로그인 (§4a)
    check(!a->token().isEmpty() && a->verified(), "로그인 후 verified + 토큰 보유");
    {
        QSettings s("GuardX", "VMS");
        const QString stored = s.value("auth/session_token").toString();
        check(!stored.isEmpty(), "토큰이 레지스트리에 저장됐다");
        check(stored != a->token(),
              "저장된 토큰은 평문이 아니다 (DPAPI)");
        check(Credentials::unprotect(stored) == a->token(),
              "복호화하면 원래 토큰");
    }
    run_resume(a);
    check(a->logged_in() && a->verified(), "저장된 세션으로 자동 로그인");

    // ⑩ 쓰기 명령의 토큰 부착 (§6) — 화면을 거치지 않고 규약만 확인한다
    {
        QJsonObject params;
        params["cmd"] = "set_zone";
        attach_token(params);
        check(params.value("token").toString() == a->token(),
              "로그인 상태: 쓰기 payload 에 token 이 실린다");

        const QString saved = a->m_token;
        a->m_token.clear();
        QJsonObject none;
        attach_token(none);
        // ⚠ 빈 문자열이 아니라 **필드 자체가 없어야** 한다 — 서버는 "없음"만
        //   과도기에 관대하고 틀린 토큰은 언제나 거부한다(작업지시 §3).
        check(!none.contains("token"),
              "토큰이 없으면 필드를 아예 넣지 않는다 (빈 문자열 금지)");
        a->m_token = saved;
    }

    qInfo().noquote()
        << "  [안내] 오프라인 유예(§4b)는 레지스트리 auth/stub_offline=1 로 따로 돈다";

    a->logout();
    check(a->token().isEmpty(), "로그아웃 후 토큰 제거");
    {
        QSettings s("GuardX", "VMS");
        check(s.value("auth/session_token").toString().isEmpty(),
              "로그아웃이 저장된 세션도 지운다");
    }

    qInfo().noquote() << QString("[Auth] 자가시험 종료 — 실패 %1건")
                             .arg(g_fail_count);
    return g_fail_count == 0 ? 0 : 1;
}

int Auth::selftest_offline()
{
    Auth *a = instance();
    g_fail_count = 0;
    qInfo() << "[Auth] 자가시험 시작 — 스텁 오프라인 흉내 (§4b 유예 경로)";

    // "재시작 직전에 저장돼 있던 세션"을 만든다. 72시간을 기다리지 않고
    // 유예를 시험하려면 마지막 확인 시각을 직접 정하는 수밖에 없다.
    auto plant_session = [a](int hours_ago) {
        a->m_token = QStringLiteral("0123456789abcdef0123456789abcdef"
                                    "0123456789abcdef0123456789abcdef");
        a->m_username = QStringLiteral("admin");
        a->m_display_name = QStringLiteral("관리자(스텁)");
        a->m_role = Role::Admin;
        a->m_expires_at = QDateTime::currentDateTime().addDays(30);
        a->m_verified_at =
            QDateTime::currentDateTime().addSecs(-qint64(hours_ago) * 3600);
        a->store_session();
        // 메모리 상태를 비워 **기동 직후**와 같은 모양으로 만든다
        a->m_token.clear();
        a->m_verified = false;
        a->m_state = State::LoggedOut;
    };

    // ① 유예 안 — 캐시된 역할로 통과하되 읽기 전용
    plant_session(GRACE_HOURS - 1);
    run_resume(a);
    check(a->logged_in(), "유예 안(71h): 통과한다");
    check(!a->verified(), "유예 통과는 **읽기 전용**(verified=false)");
    check(a->role() == Role::Admin, "캐시된 역할이 복원된다");
    check(a->last_verified_at().isValid(), "마지막 확인 시각이 배너용으로 남는다");

    // ①-2 유예 중에는 **관리자여도** 쓰기가 막힌다 (§4b + §5)
    check(a->role() == Role::Admin && !Auth::can(Action::ActuatorControl),
          "유예 중 관리자도 쓰기 거부 (읽기 전용)");
    check(Auth::deny_reason(Action::ActuatorControl)
              .contains("Offline"),
          "유예 사유 문구가 오프라인을 말한다");

    // ② 오프라인에서는 새 로그인이 안 된다 (서버가 판정하는 일이므로)
    Attempt r = run_attempt(a, "admin", "admin");
    check(!r.ok && r.reason == QLatin1String(REASON_UNREACHABLE),
          "오프라인: 새 로그인은 unreachable");

    // ③ 유예 초과 — 통과시키지 않는다
    plant_session(GRACE_HOURS + 1);
    run_resume(a);
    check(!a->logged_in(), "유예 초과(73h): 거절한다");

    // ④ 만료된 세션은 유예로 부활하지 않는다 (30일 정책 우회 금지)
    plant_session(1);
    {
        QSettings s("GuardX", "VMS");
        s.setValue("auth/session_expires_at",
                   QDateTime::currentDateTime().addDays(-1).toString(Qt::ISODate));
    }
    run_resume(a);
    check(!a->logged_in(), "만료된 세션은 유예로도 안 살아난다");

    a->logout();
    qInfo().noquote() << QString("[Auth] 자가시험 종료 — 실패 %1건").arg(g_fail_count);
    return g_fail_count == 0 ? 0 : 1;
}
