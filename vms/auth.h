#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

/**
 * @brief 로그인 상태와 역할 — 앱 전체의 단일 진실원천 (싱글턴)
 *
 * 인증 원천은 **RPi B 경유 DB**다. VMS는 DB에 직접 붙지 않는다는 v6 원칙
 * 그대로, 아이디/비밀번호를 `guardx/db/rpib/cmd/login` 으로 보내고 폴러가
 * PBKDF2로 검증한 뒤 토큰과 역할을 돌려준다 (계약: 로그인 핸드오프 §5).
 *
 * ⚠ **여기 있는 것은 인증이 아니라 UI 가드다.** 클라이언트의 역할 판정은
 * 바이너리를 고치면 뚫린다. 진짜 방어선은 폴러가 명령마다 토큰·역할을 다시
 * 보는 것이고(핸드오프 §6), 이 클래스는 "관제 PC 앞에 앉은 아무나가 위험
 * 조작을 누르는 것"을 막는다. 둘을 헷갈리면 보안 연극이 된다.
 *
 * 서버가 아직 없어도 화면을 완성할 수 있게 **스텁 경로**를 함께 둔다
 * (`stub_enabled()`). 스텁은 계약과 **같은 모양의 JSON**을 만들어 같은
 * 파서에 넣으므로, 붙이는 날 바뀌는 것은 전송 한 줄뿐이다.
 *
 * 스레드: GUI 스레드 전용. MqttLink 콜백도 GUI 스레드라 마샬링이 없다.
 */
class Auth : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 권한 2단계 (기획서 §0)
     *
     * "미로그인"을 여기 넣지 않는다. 역할이 없는 상태는 State가 표현한다 —
     * Role에 None을 두면 `can()`을 쓰는 모든 자리가 "None이면?"을 따로
     * 처리해야 하고, 그 분기를 하나만 빠뜨려도 조용히 열린다.
     */
    enum class Role { Operator, Admin };

    /**
     * @brief 로그인 진행 상태. 오프라인 유예는 `verified()` 로 표현한다
     *
     * `MustChangePassword` = **인증은 됐지만 아직 아무것도 못 한다**(§5b).
     * `logged_in()` 이 false 라서 셸 진입·경보 팝업·`can()` 이 전부 막힌 채로
     * 비밀번호 변경 화면만 뜬다 — 상태를 하나 더 두는 대신 이 조건을 곳곳에
     * 흩뿌리면 반드시 한 군데를 빠뜨린다.
     */
    enum class State { LoggedOut, Checking, MustChangePassword, LoggedIn };

    /**
     * @brief 계정 목록 한 줄 (`cmd/list_users` 응답 · 08-12)
     *
     * 화면이 아이디를 **손으로 받아 적지 않게** 하려고 둔다. 오타는 서버가
     * `not_found` 로 돌려주지만, 애초에 "누가 있는지"를 모르는 화면이면
     * 비활성 계정을 재활성할 방법이 없다(그 계정은 로그인도 못 하므로 어디에도
     * 안 나타난다).
     */
    struct UserRow {
        QString username;
        QString display_name;
        Role role = Role::Operator;
        bool enabled = true;
        /// 초기 비밀번호를 아직 안 바꿨다 — 선택 필드라 없으면 false
        bool must_change_pw = false;
        /// 서버가 안 주거나 로그인한 적이 없으면 무효 QDateTime
        QDateTime last_login_at;
    };

    /**
     * @brief 잠글 조작의 목록 (기획서 §5 표 전수)
     *
     * 오늘은 표의 모든 항목이 관리자 전용이라 `can()` 의 판정이 하나다.
     * 그런데도 열거형을 두는 이유는 **호출부가 무엇을 묻는지 남기기 위해서**다 —
     * 한 줄이 나중에 운영자에게 열리면 `can()` 만 고치면 되고 화면은 안 바뀐다.
     * (조회·보기는 여기 없다 — 전부 허용이라 물을 필요가 없다.)
     */
    enum class Action {
        ActuatorControl,   ///< DEVICE 액추에이터 ON/OFF·SET
        FireClear,         ///< DEVICE 화재 수동 해제
        Broadcast,         ///< DEVICE 실시간 방송
        ZoneSettings,      ///< SETTINGS 구역 정원·혼잡 임계 적용
        FireThreshold,     ///< SETTINGS 화재 판단 임계 적용·기본값
        CameraApps,        ///< CAMERA 앱 Start/Stop·AutoStart·Priority
        CameraSystem,      ///< CAMERA 재부팅·NTP 재동기·설정 백업
        CameraTuning,      ///< CAMERA 프로파일·저지연 프리셋·ISP·Focus
        /**
         * @brief SETTINGS ▸ Accounts 탭 — 계정 만들기·비활성/재활성 (08-12)
         *
         * 계정 화면은 `ZoneSettings` 를 빌려 묻고 있었다. 오늘은 `can()` 이
         * 인자를 무시해 정답이 나오지만, 위 주석대로 **한 줄이 나중에
         * 운영자에게 열리는 날** 구역 정원을 여는 커밋이 계정 생성까지 함께
         * 연다. 그 커밋을 쓰는 사람은 자기가 무엇을 열었는지 모른다.
         */
        AccountAdmin,
    };
    Q_ENUM(Role)
    Q_ENUM(State)
    Q_ENUM(Action)

    static Auth *instance();

    /**
     * @brief 이 조작을 지금 해도 되는가 — **판정은 여기 한 곳뿐**
     *
     * 셋을 함께 본다: 로그인했는가 · **서버가 확인해 준 세션인가**(§4b 오프라인
     * 유예는 읽기 전용) · 역할이 관리자인가.
     *
     * ⚠ **이건 UI 가드지 인가가 아니다.** 바이너리를 고치면 뚫린다. 진짜
     * 방어선은 폴러가 명령마다 토큰·역할을 다시 보는 것이다(핸드오프 §6).
     * 그래서 화면 잠금과 **명령이 나가는 지점의 백스톱**을 둘 다 둔다.
     */
    static bool can(Action a);

    /** @brief 권한이 없을 때 버튼에 붙일 문구 (§5 — 숨기지 않고 이유를 말한다) */
    static QString deny_reason(Action a);

    /**
     * @brief 쓰기 명령 payload 에 세션 토큰을 얹는다 (핸드오프 §6)
     *
     * ⚠ **토큰이 없으면 필드를 아예 넣지 않는다.** 빈 문자열도 서버는 "없음"으로
     * 치지만, 아예 안 보내는 쪽이 규약과 정확히 맞는다(작업지시 §3).
     *
     * 🔴 **08-12: 서버가 `REQUIRE_TOKEN=1` 로 갔다.** 이제 토큰 없는 쓰기는 전부
     * `forbidden` 이다 — "스텁으로 개발하는 동안에는 토큰을 빼라"던 옛 조언은
     * 무효다. 면제는 `login`·`session_check`·`logout` 셋뿐이다.
     */
    static void attach_token(QJsonObject &params);

    /**
     * @brief 쓰기 명령이 **인증 이유로** 거절됐을 때 (`reason` 이 실려 온 응답)
     *
     * 서버는 어느 층에서 막혔는지를 **필드 이름으로** 알려준다 —
     * `reason` 이면 권한/세션, `error` 면 값 검증이다(작업지시 §0-1).
     * 그래서 이 함수는 `reason` 있는 응답만 받는다.
     *
     * `expired`·`disabled` 면 **세션을 무효화**한다 — 그 상태로 화면에 남아
     * 있으면 누를 때마다 거절만 반복하고 원인을 알 수 없다.
     *
     * @return 화면에 그대로 띄울 문구
     */
    static QString note_write_reject(const QJsonObject &reply);

    /**
     * @brief 위젯 하나를 권한에 묶는다 — 활성/툴팁이 로그인 상태를 따라간다
     *
     * ⚠ **다른 곳에서 `setEnabled` 를 부르지 않는 위젯에만** 쓴다.
     * 자체 활성 조건이 있는 위젯(전이 중 잠금·데이터 도착 여부 등)에 이걸
     * 걸면 두 주인이 생겨 **나중에 부른 쪽이 이긴다** — 그런 자리는 그
     * 계산식 안에 `can()` 을 AND 로 넣어야 한다.
     *
     * 원래 툴팁은 보존한다(권한이 있으면 되돌려 놓는다).
     */
    static void bind(QWidget *w, Action a);

    // ---- 조작 --------------------------------------------------------------

    /**
     * @brief 로그인 시도. 결과는 시그널로 온다 (state_changed / login_failed)
     *
     * 이미 Checking이면 **무시한다** — 화면이 버튼을 비활성시키지만 그건
     * 표면이다(잠긴 위젯도 코드로는 clicked를 낼 수 있다). 명령이 나가는
     * 지점에 백스톱을 둔다.
     */
    void login(const QString &username, const QString &password);

    /**
     * @brief 로그아웃 — 서버 세션까지 지운다(다른 PC에서도 무효)
     *
     * 저장된 토큰도 함께 지운다. 서버 요청은 fire-and-forget 이다 — 응답을
     * 기다리다 화면이 멈추면 안 되고, 브로커가 죽어 있어도 로컬 로그아웃은
     * 반드시 되어야 한다(§4b 의 정신).
     */
    void logout();

    /**
     * @brief 비밀번호 변경 (§5b) — 결과는 시그널로 온다
     *
     * 성공하면 서버가 **그 사용자의 기존 세션을 전부 무효화**하고 새 토큰을
     * 준다(다른 기기에 남은 세션이 살아 있으면 바꾼 의미가 없다). 받은 토큰으로
     * 저장분을 갈아 끼우므로 본인은 다시 로그인하지 않는다.
     */
    void change_password(const QString &old_pw, const QString &new_pw);

    /**
     * @brief 계정 생성 (§5b) — **관리자만**. 결과는 시그널로 온다
     *
     * 새 계정은 서버에서 `must_change_pw=TRUE` 로 만들어진다 — 만든 사람이 초기
     * 비밀번호를 알고 있으므로 본인이 첫 로그인에서 바꿔야 한다.
     *
     * ⚠ 비밀번호 인자가 **없다**(08-12). 초기 비밀번호는 `initial_password()`
     * 하나로 고정이다 — 호출부가 다른 값을 넣을 수 있으면 "관리자가 정한 비밀번호"
     * 경로가 되살아나고, 그건 만든 사람이 남의 비밀번호를 아는 상태를 만든다.
     */
    void create_user(const QString &username, const QString &display_name,
                     Role role);

    /**
     * @brief 사람이 **직접 비밀번호를 친** 마지막 시각 (08-13)
     *
     * 자동 로그인(§4a)은 이 시각을 갱신하지 않는다 — 저장된 토큰이 유효하다는
     * 것과 "지금 이 자리에 있는 사람이 그 계정 주인이다"는 다른 사실이다.
     * 관제 PC 는 하루 종일 켜져 있고 자리를 비우는 일이 잦다.
     */
    QDateTime last_human_auth_at() const { return m_human_auth_at; }

    /**
     * @brief 그 인증이 아직 신선한가 (설정을 바꿀 수 있는가)
     *
     * ⚠ 이건 **권한(can)이 아니라 최근성**이다. 둘을 한 함수에 합치지 않는다 —
     * can() 은 화면을 칠하는 경로에서 수백 번 불리는 순수 판정이고, 이쪽은
     * "물어봐야 한다"는 사건을 부른다.
     */
    bool fresh() const;

    /** @brief 재확인 없이 설정을 바꿀 수 있는 시간 (분) */
    static constexpr int FRESH_MINUTES = 10;

    /**
     * @brief 비밀번호 재확인 (08-13) — 아이디는 묻지 않는다
     *
     * 지금 로그인된 계정 그대로 `cmd/login` 을 한 번 더 보낸다. 계약을 늘리지
     * 않는 대신 **성공하면 옛 토큰을 서버에서 지운다**(logout) — 안 그러면
     * 재확인할 때마다 세션 행이 하나씩 쌓인다.
     *
     * ⚠ 실패해도 **세션을 건드리지 않는다.** 로그인 실패와 달리 여기서
     * 로그아웃시키면, 비밀번호를 한 번 잘못 친 사람이 보던 화면을 통째로 잃는다.
     * ⚠ 서버의 실패 잠금(5회 → 60초)은 재확인 실패에도 그대로 쌓인다.
     */
    void reauthenticate(const QString &password);

    /**
     * @brief 새 계정의 고정 초기 비밀번호 (08-12)
     *
     * ⚠ **이 값은 소스에 박혀 있어 git 에 공개된다.** 그런데도 이렇게 두는 이유:
     * 서버가 새 계정을 `must_change_pw=TRUE` 로 만들고, 그 상태의 토큰으로는
     * 쓰기 명령이 전부 거절된다(계약 §must_change_password). 즉 이 비밀번호로
     * 할 수 있는 일은 **비밀번호를 바꾸는 것뿐**이고, 창은 "관리자가 계정을
     * 만든 순간부터 본인이 처음 들어올 때까지"로 좁다.
     *
     * 그 창이 넓어지는 순간(강제 변경이 꺼지거나, 만들어만 두고 안 쓰는 계정이
     * 쌓이거나) 이 결정은 다시 봐야 한다.
     */
    static QString initial_password();

    /**
     * @brief 계정 목록 요청 (`cmd/list_users`) — **관리자만**. 결과는 시그널로
     *
     * ⚠ 구독이 아니라 **요청-응답**이다. 계정 명부를 retained 토픽에 두면
     * 브로커에 붙는 누구에게나 전 직원 아이디·역할이 상시 노출된다.
     */
    void list_users();

    /**
     * @brief 계정 사용 중지/재개 (`cmd/set_user_enabled`) — **관리자만**
     *
     * **삭제가 아니다.** 행을 지우면 그 계정이 남긴 `updated_by`·세션 기록이
     * 가리킬 곳을 잃는다 — 사고 조사에서 가장 먼저 보는 것이 "누가 언제 무엇을
     * 바꿨나"라, 지우는 쪽이 잃는 것이 더 크다(계약 §set_user_enabled).
     *
     * 자기 계정(`self_target`)과 마지막 관리자(`last_admin`)는 **서버가** 막는다.
     * 화면도 같은 이유를 사람 말로 보여주되, 판정을 화면에 옮겨오지 않는다 —
     * "지금 관리자가 몇 명인가"는 이 PC 가 알 수 있는 사실이 아니다.
     */
    void set_user_enabled(const QString &username, bool enabled);

    // ⚠ 비밀번호 정책은 **폐지됐다**(08-12, 서버가 먼저 폐지). 길이 규칙도
    //   `password_policy_error()` 도 없다 — 서버가 거부하는 것은 **빈 값 하나**
    //   뿐이고 그때만 `weak_password` 가 온다.
    //   ⚠ 어디서도 `trimmed()` 를 걸지 않는다. 공백도 비밀번호의 일부이고
    //     개수까지 보존된다(실기 확인: 공백 3개로 바꾼 뒤 같은 3개로 재로그인
    //     성공, 1개는 실패). 한쪽만 깎으면 방금 바꾼 본인이 못 들어온다.

    /**
     * @brief 저장된 세션으로 자동 로그인 시도 — **기동 시 1회** (§4a)
     *
     * 성공하면 로그인 화면 없이 바로 들어간다. 브로커에 못 붙으면 §4b 의
     * 오프라인 유예로 넘어간다. 결과는 다른 경로와 똑같이 시그널로 온다.
     */
    void resume();

    // ---- 조회 --------------------------------------------------------------

    State state() const { return m_state; }
    bool logged_in() const { return m_state == State::LoggedIn; }

    /** @brief 역할. **LoggedIn이 아닐 때의 값은 의미가 없다** */
    Role role() const { return m_role; }

    QString username() const { return m_username; }
    QString display_name() const { return m_display_name; }

    /** @brief 세션 만료 시각(로컬 시간). 서버가 안 줬으면 무효 QDateTime */
    QDateTime expires_at() const { return m_expires_at; }

    /** @brief 세션 토큰. DPAPI로 감싸 레지스트리에 저장된다(§4a) */
    QString token() const { return m_token; }

    /**
     * @brief **서버가 확인해 준 세션인가** (§4b)
     *
     * false = 브로커에 못 붙어 캐시된 역할로 통과한 상태다. 이때는
     * **읽기 전용**이다 — 단계 5의 `can()` 이 역할을 보기 전에 이것부터 본다.
     * 어차피 명령을 보낼 브로커가 없어 잃는 기능은 없다.
     */
    bool verified() const { return m_verified; }

    /** @brief 마지막으로 서버가 확인해 준 시각 (오프라인 배너 문구) */
    QDateTime last_verified_at() const { return m_verified_at; }

    /**
     * @brief 기동 시 세션 검증이 진행 중인가
     *
     * 이때 로그인 화면은 폼 대신 스플래시를 보인다 — 자동 로그인이면 화면이
     * **깜빡이지도 않아야** 하고(§6a), 폼을 먼저 그리면 반드시 깜빡인다.
     */
    bool resuming() const { return m_resuming; }

    /** @brief 오프라인 유예 한도 (§4b — 08-11 확정) */
    static constexpr int GRACE_HOURS = 72;

    // ---- 스텁 --------------------------------------------------------------

    /**
     * @brief 서버 없이 도는 개발용 스텁을 쓰는가 (레지스트리 `auth/stub`)
     *
     * 기본값이 **참**이다. 그대로 배포되면 그게 사고라서 ①로그인마다 경고
     * 로그 ②로그인 화면에 배지로 드러낸다.
     *
     * ⚠ **끄는 조건은 "서버가 준비됐는가"가 아니라 "mTLS 절체가 끝났는가"다**
     * (08-11 확정). 서버는 이미 동작한다 — login·session_check·logout 3종이
     * RPi B 에서 실기 검증됐고 응답 형식도 계약 그대로다. 그런데도 켜 두는
     * 이유는 브로커가 아직 **1883 평문**이라, 실경로로 바꾸는 순간
     * **비밀번호가 그대로 흐르기** 때문이다(payload 평문 — 핸드오프 §5).
     *
     * 그래서 "핸들러가 섰으니 이제 끄자"는 판단이 정확히 틀린 판단이다.
     * 순서: rpib.crt SAN 재발급 → 8883 mTLS 절체 → 그다음 이 값을 거짓으로.
     */
    static bool stub_enabled();

    /**
     * @brief 스텁이 **서버 불통을 흉내내는가** (레지스트리 `auth/stub_offline`)
     *
     * 오프라인 유예(§4b)는 "브로커가 죽은 동안"의 동작이라, 스텁이 늘
     * 성공하면 그 경로를 시험할 방법이 없다. 이 스위치를 켜면 스텁이
     * 로그인·세션검증 모두 **닿지 못한 것처럼** 답한다.
     */
    static bool stub_offline();

    /** @brief 역할의 화면 표기 ("관리자" / "운영자") */
    static QString role_text(Role r);

    // ---- 실패 사유 (계약 문자열) -------------------------------------------
    // 화면 문구로 옮기는 표는 기획서 §2b. **사유는 구분하되 문구는 합친다** —
    // "아이디가 없다"와 "비밀번호가 틀렸다"를 구분해 보여주면 계정 존재
    // 여부를 알려주는 꼴이다.
    static constexpr const char *REASON_BAD_CREDENTIALS = "bad_credentials";
    static constexpr const char *REASON_LOCKED          = "locked";   ///< + retry_after_s
    static constexpr const char *REASON_DISABLED        = "disabled";
    /** @brief 세션 만료 **또는 없는 토큰** (서버가 일부러 하나로 덮는다) */
    static constexpr const char *REASON_EXPIRED         = "expired";
    /**
     * @brief 토큰은 유효하나 역할이 admin 이 아님 — **쓰기 명령**의 거절 사유
     *
     * 로그인 응답에는 안 온다. 단계 6에서 `set_zone` 계열이 받는 값이라
     * 여기 함께 둔다 — 사유 목록이 두 군데로 갈리면 한쪽만 갱신된다.
     */
    static constexpr const char *REASON_FORBIDDEN       = "forbidden";
    /** @brief 계약에 없는 **VMS 로컬** 사유 — 서버에 닿지도 못했다 */
    static constexpr const char *REASON_UNREACHABLE     = "unreachable";
    /** @brief 계약에 없는 **VMS 로컬** 사유 — 응답이 계약과 다르다 */
    static constexpr const char *REASON_BAD_REPLY       = "bad_reply";
    // §5b 신설 (forbidden 은 위에 이미 있다 — 같은 값이 두 이름이 되면 안 된다)
    /** @brief **빈 비밀번호** 전용 (08-12 정책 폐지 이후 남은 유일한 경우) */
    static constexpr const char *REASON_WEAK_PASSWORD   = "weak_password";
    static constexpr const char *REASON_DUPLICATE       = "duplicate";
    /**
     * @brief 강제 변경 대상이 쓰기 명령을 보냈다 (서버가 08-11 저녁에 추가)
     *
     * 정상 흐름에서는 안 온다 — 그 상태의 VMS 는 애초에 명령을 안 보낸다.
     * 그래도 오면 **우리 화면이 서버 판정과 어긋났다는 뜻**이므로, 일반 실패로
     * 흘리지 말고 변경 화면으로 되돌린다.
     * ⚠ `change_password` 는 이 검사를 안 탄다 — 유일한 탈출구다.
     */
    static constexpr const char *REASON_MUST_CHANGE_PW  = "must_change_password";
    // set_user_enabled 전용 (08-12) — 셋 다 **서버가** 판정한다
    /** @brief 자기 계정을 자기가 비활성화하려 했다 (누르는 순간 자기 세션이 죽는다) */
    static constexpr const char *REASON_SELF_TARGET     = "self_target";
    /** @brief 마지막 관리자 — 끄면 아무도 계정을 관리할 수 없게 된다 */
    static constexpr const char *REASON_LAST_ADMIN      = "last_admin";
    /** @brief 그런 username 이 없다 */
    static constexpr const char *REASON_NOT_FOUND       = "not_found";

    /** @brief 개발용 자가시험 (`--auth-selftest`). 0이면 통과 */
    static int selftest();

    /** @brief 오프라인 유예 전용 시나리오 (`auth/stub_offline=1` 일 때) */
    static int selftest_offline();

signals:
    void state_changed(State s);

    /**
     * @brief 로그인 실패
     * @param retry_after_s `locked`일 때 남은 초. 그 외에는 0
     */
    void login_failed(const QString &reason, int retry_after_s);

    /** @brief 오프라인 유예로 들어갔거나(false), 재검증으로 승격됐다(true) */
    void verification_changed(bool verified);

    /** @brief 비밀번호 변경 결과 (§5b). 실패 사유는 계약의 reason */
    void password_changed(bool ok, const QString &reason);

    /** @brief 계정 생성 결과 (§5b) */
    void user_created(bool ok, const QString &reason);

    /**
     * @brief 비밀번호 재확인 결과 (08-13)
     *
     * `retry_after_s` 는 잠금(`locked`)일 때만 의미가 있다 — 그동안은 다시
     * 물어봐야 소용없으므로 화면이 남은 초를 세어 준다.
     */
    void reauth_result(bool ok, const QString &reason, int retry_after_s);

    /** @brief 계정 목록 결과 (08-12). 실패면 users 는 비어 있다 */
    void users_listed(bool ok, const QVector<Auth::UserRow> &users,
                      const QString &reason);

    /**
     * @brief 사용 중지/재개 결과 (08-12)
     *
     * `username`·`enabled` 를 함께 싣는다 — 서버 응답이 **반영된 값을 되돌려
     * 주므로**(계약) 화면이 목록을 다시 받지 않고도 그 행만 고칠 수 있다.
     */
    void user_enabled_changed(bool ok, const QString &username, bool enabled,
                              const QString &reason);

private:
    explicit Auth(QObject *parent = nullptr);

    void set_state(State s);
    void submit(const QString &username, const QString &password);
    void apply_ok(const QJsonObject &reply);
    void apply_fail(const QJsonObject &reply);
    void fail(const QString &reason, int retry_after_s);

    /** @brief 계약과 같은 모양의 가짜 응답 (스텁 전용) */
    QJsonObject stub_reply(const QString &username, const QString &password);

    // ---- 세션 보관 (§4a) ---------------------------------------------------
    void store_session();
    bool load_session();      ///< 저장된 세션을 멤버로 읽는다. 없으면 false
    void clear_session();

    void verify_token();          ///< `cmd/session_check`
    void enter_offline_grace();   ///< §4b — 캐시된 역할로 읽기 전용 진입
    void finish_resume();         ///< 자동 로그인 절차 종료 표시

    State m_state = State::LoggedOut;
    Role m_role = Role::Operator;
    QString m_username;
    QString m_display_name;
    QString m_token;
    QDateTime m_expires_at;
    QDateTime m_verified_at;
    /// 사람이 직접 비밀번호를 친 시각. 자동 로그인은 갱신하지 않는다(§재확인).
    /// ⚠ 저장하지 않는다 — 앱을 다시 켜면 첫 설정 변경에서 다시 물어야 한다.
    QDateTime m_human_auth_at;
    /// 지금 나가 있는 로그인이 **사람이 친 것**인가 (apply_ok 가 이걸 보고 갱신)
    bool m_human_login = false;   ///< 마지막 성공 검증 시각 (유예 계산의 기준)
    bool m_verified = false;
    bool m_resuming = false;
    QTimer *m_broker_wait = nullptr;   ///< 기동 시 브로커 접속을 잠깐 기다린다

    /**
     * @brief 시도 세대 번호 — 늦게 온 응답이 되살아나지 못하게
     *
     * 로그아웃하거나 다시 시도하면 올린다. 콜백은 자기 세대가 아니면
     * 조용히 버린다. 이게 없으면 "로그아웃했는데 8초 전 요청의 응답이
     * 도착해 다시 로그인된 상태"가 만들어진다.
     */
    quint64 m_epoch = 0;

    QString m_req_id;   ///< 진행 중인 MQTT 요청 (취소용)

    /** @brief 스텁의 아이디별 실패 횟수/잠금 — 서버 잠금 정책을 흉내 낸다 */
    struct StubLock {
        int failed = 0;
        QDateTime locked_until;
    };
    QHash<QString, StubLock> m_stub_locks;
};
