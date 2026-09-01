#pragma once

#include <QString>
#include <QWidget>

class QEvent;
class QFrame;
class QGraphicsOpacityEffect;
class QLabel;
class QLineEdit;
class QPushButton;
class QResizeEvent;
class QTimer;

/**
 * @brief 로그인 화면 (기획서 §6a)
 *
 * **별도 창이 아니라 같은 창의 첫 페이지**다. 창이 바뀌면 시연에서 한 번
 * 깜빡이고, 로그인 창을 닫았는지 뒤로 숨겼는지가 헷갈린다.
 *
 * 화면은 크롬(상단바·내비)이 없는 자리라 `chrome*` 토큰이 아니라 **본문
 * 팔레트**를 쓴다(§6c). 새 색은 만들지 않는다 — 카드는 `#LoginCard`
 * (=`#Panel`과 같은 면), 주 버튼만 `#PrimaryBtn`(accent 채움).
 *
 * 인증 자체는 여기 없다. `Auth`가 하고 이 화면은 상태를 그린다.
 */
class LoginPage : public QWidget
{
    Q_OBJECT

public:
    explicit LoginPage(QWidget *parent = nullptr);

    /** @brief 폼을 비우고 아이디 칸에 포커스 (로그아웃으로 돌아왔을 때) */
    void reset();

    /**
     * @brief 비밀번호 변경 폼을 연다 (§5b)
     * @param forced true = `must_change` 로 들어온 강제 변경(취소 없음)
     *
     * 강제와 자발적 변경이 **같은 폼**을 쓴다. 두 벌을 만들면 정책·문구가
     * 언젠가 갈린다.
     */
    void begin_change(bool forced);

signals:
    /** @brief 인증 성공 — MainWindow가 본 화면으로 넘긴다 */
    void authenticated();

    /** @brief 자발적 비밀번호 변경이 끝났다(성공·취소 모두) — 셸로 돌아간다 */
    void change_finished();

private:
    void submit();
    void render_error();          ///< 상태(멤버)로부터 문구를 **다시 만든다**
    void render_server();         ///< 브로커 상태줄 (§6a — 원인을 먼저 보여준다)

    /**
     * @brief 폼 대신 "세션 확인 중…"을 보인다 (자동 로그인 중)
     *
     * 자동 로그인이 성공할 때 로그인 화면이 **깜빡이지 않아야** 한다(§6a).
     * 폼을 먼저 그려두고 성공하면 지우는 방식으로는 그 깜빡임을 없앨 수 없다.
     */
    void show_splash(bool splash);

    /**
     * @brief 강제 비밀번호 변경 화면 (§5b)
     *
     * `must_change` 로 들어온 사람은 **화면에 못 들어간다**. 로그인 카드와 같은
     * 자리에 변경 폼을 세워, 바꾸기 전에는 아무것도 할 수 없게 한다 —
     * 시드 비밀번호가 저장소에 공개돼 있는 동안 이 강제가 유일한 방어다.
     */
    void build_change_card();
    void submit_change();
    void render_change_error();

    /**
     * @brief 로그인 버튼을 지금 눌러도 되는가 — **판정은 여기 한 곳뿐**
     *
     * 막는 이유가 셋이다(확인 중·잠금 카운트다운·브로커 미연결). 이유마다
     * `setEnabled` 를 흩뿌리면 마지막에 부른 쪽이 이깁니다 — 잠긴 동안
     * 브로커가 붙는 순간 버튼이 되살아나는 식이다. 세 조건을 한 함수가
     * 매번 다시 계산한다.
     */
    void update_submit_state();

    /** @brief 잠금 남은 초 감소 (1초 주기, 정수 연산만) */
    void tick_lock();

    /** @brief 계약의 reason → 화면 문구 (기획서 §2b 표) */
    static QString message_for(const QString &reason, int retry_after_s);

    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
    QPushButton *m_submit = nullptr;
    QLabel *m_error = nullptr;
    QLabel *m_server = nullptr;
    QFrame *m_card = nullptr;

    /**
     * 세션 확인 화면 = [도담 전신][문구] 한 덩어리. 카드와 같은 자리를 쓰고
     * 한 번에 하나만 보인다. 전신이 나오는 자리는 여기뿐이다(08-13 결정).
     */
    QWidget *m_splash_box = nullptr;
    QLabel *m_splash_mascot = nullptr;
    QGraphicsOpacityEffect *m_splash_fade = nullptr;  ///< 라이트 테마 0.85 (§5)
    QLabel *m_splash = nullptr;
    QLabel *m_avatar = nullptr;                       ///< 카드 브랜드 줄의 얼굴 원
    QFrame *m_change_card = nullptr;
    QLineEdit *m_cur_pw = nullptr;
    QLineEdit *m_new_pw = nullptr;
    QLineEdit *m_new_pw2 = nullptr;
    QPushButton *m_change_btn = nullptr;
    QPushButton *m_change_cancel = nullptr;
    QLabel *m_change_why = nullptr;
    bool m_change_forced = false;
    QLabel *m_change_msg = nullptr;
    QString m_change_reason;
    QTimer *m_lock_timer = nullptr;

    /**
     * 오류는 **문구가 아니라 사유로 기억한다.** 테마가 바뀌면 라벨을 다시
     * 만드는데, 그때 화면에 있던 문장을 잃거나 다른 문장으로 갈리면 안 된다
     * (테마 정본 §10-3).
     */
    QString m_error_reason;
    int m_error_retry_s = 0;      ///< `locked` 의 남은 초 (0이면 잠금 아님)
};
