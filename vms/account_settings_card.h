#pragma once

#include "auth.h"

#include <QVector>
#include <QWidget>

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;

/**
 * @brief SETTINGS 의 **Accounts 탭** — 계정 만들기 (§5b, 08-12 분리)
 *
 * `zone_settings_page.cpp` 의 `build_account_card()` 를 그대로 옮겨 왔다.
 * 별도 파일인 이유는 `SiteSettingsCard` 와 같다 — 페이지에는 생성 한 줄만
 * 남기고, 계정 기능이 커져도 구역·화재 표와 diff 가 섞이지 않게.
 *
 * @section perm 권한 잠금 — **밖에서 잠그지 않는다**
 *
 * 탭 버튼(자체 활성 조건이 없다)만 `Auth::bind()` 로 묶고, 여기 있는 버튼들은
 * **자기 활성 계산 안에 `Auth::can()` 을 AND 로** 넣는다(`refresh_write_enable`).
 * [Create] 는 이미 "전송 중이면 잠근다"는 자체 조건이 있어서, 권한을 밖에서
 * `setEnabled` 로 걸면 두 주인이 생기고 **나중에 부른 쪽이 이긴다** —
 * 응답이 도착해 버튼을 되살리는 순간 권한 잠금이 함께 풀린다.
 *
 * ⚠ 화면 잠금은 보안이 아니라 UI 가드다. 진짜 방어선은 `Auth::create_user()`
 * 의 백스톱과 서버의 `role='admin'` 재검사다.
 */
class AccountSettingsCard : public QWidget
{
    Q_OBJECT

public:
    explicit AccountSettingsCard(QWidget *parent = nullptr);

protected:
    /**
     * @brief 탭이 보일 때 목록을 새로 받는다
     *
     * 폴링하지 않는 이유: 계정 명부는 30초마다 바뀌는 값이 아니고, 그걸 주기적으로
     * 발행하면 안 보고 있는 동안에도 전 직원 목록이 브로커를 오간다.
     */
    void showEvent(QShowEvent *e) override;

private:
    void build_ui();
    QWidget *build_create_card();
    void submit_new_user();
    void set_status(const QString &text, bool error = false);

    /**
     * @brief 아이디 칸 바로 밑에 붙는 오류 — 중복 확인의 답이 여기 뜬다
     *
     * 오른쪽 상태 문구와 나누는 기준은 **"어느 칸을 고쳐야 하나"** 다.
     * `duplicate` 는 아이디 칸의 문제라 거기 붙어야 하고, "서버가 응답하지
     * 않는다"는 어느 칸의 문제도 아니라 상태 문구로 간다.
     *
     * 빈 문자열이면 숨는다 — 빈 라벨이 자리를 먹으면 카드 높이가 상태에 따라
     * 출렁인다.
     */
    void set_user_error(const QString &text);

    /**
     * @brief 쓰기 버튼의 활성 조건을 **한 곳에서** 계산한다
     *        = (전송 중이 아닌가) AND (권한이 있는가)
     */
    void refresh_write_enable();

    // ---- 계정 목록 (cmd/list_users · cmd/set_user_enabled) ----

    QWidget *build_users_card();

    /** @brief 목록 요청. 이미 요청 중이면 겹쳐 보내지 않는다 */
    void refresh_list();

    /** @brief 받은 목록으로 표를 다시 그린다 (행 수가 바뀌므로 통째로) */
    void rebuild_user_rows();

    /**
     * @brief 사용 중지 확인 대화상자 → 확인하면 명령을 보낸다
     *
     * ⚠ 새 오버레이를 만들지 않는다. `QMessageBox` 는 모달 대화창이라
     * `AlertPopupStack` 이 자리를 잡아 줄 대상이 아니다(등록 대상은 화면 위에
     * 겹쳐 뜨는 경보 팝업이다). DEVICE 의 화재 해제 확인과 같은 조립이다.
     */
    void confirm_disable(const Auth::UserRow &row);

    void set_list_status(const QString &text, bool error = false);

    /**
     * @brief 이 아이디가 이미 쓰이고 있나 — **받아 둔 목록으로 즉시** 판정
     *
     * 서버에 묻지 않는다(계약 추가 없음). 목록이 이미 답을 갖고 있어서
     * [Create] 를 누르기 전에 알려줄 수 있다. 목록이 아직 없거나 낡았을 수
     * 있으므로 **막지는 않는다** — 최종 판정은 언제나 서버의 `duplicate` 다.
     *
     * @return 비어 있으면 쓸 수 있는 아이디
     */
    QString taken_hint(const QString &username) const;

    QLineEdit *m_new_user = nullptr;
    QLineEdit *m_new_name = nullptr;
    QComboBox *m_new_role = nullptr;
    QPushButton *m_new_submit = nullptr;
    QLabel *m_user_err = nullptr;
    QLabel *m_status = nullptr;
    /// 마지막 상태가 오류였나 — 테마가 바뀌면 **같은 의미로** 다시 칠해야 한다
    /// (색을 값으로 들고 있으면 옛 팔레트의 색이 그대로 굳는다)
    bool m_status_err = false;
    /// 요청이 나가 있는 동안 — 버튼 활성 계산의 **자체 조건**이다
    bool m_creating = false;

    // ── 계정 목록 ──
    QGridLayout *m_users_grid = nullptr;
    QPushButton *m_refresh = nullptr;
    QLabel *m_list_status = nullptr;
    bool m_list_status_err = false;
    /// 마지막으로 받은 목록. 표를 다시 그리는 근거이자 중복 검사의 자료다
    QVector<Auth::UserRow> m_users;
    bool m_listing = false;   ///< 목록 요청 진행 중
    bool m_toggling = false;  ///< 사용여부 변경 진행 중 (행 버튼의 자체 조건)
};
