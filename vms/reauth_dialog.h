#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

/**
 * @brief 설정을 바꾸기 전 비밀번호를 한 번 더 묻는 대화상자 (08-13)
 *
 * @section why 왜 읽기 전용 대신 이걸 만들었나
 *
 * 자동 로그인(§4a)으로 들어온 세션은 "저장된 토큰이 유효하다"만 말한다.
 * 지금 그 자리에 앉은 사람이 계정 주인인지는 아무도 안 물었다 — 관제 PC 는
 * 하루 종일 켜져 있고 자리를 비우는 일이 잦다.
 *
 * 그렇다고 화면을 통째로 읽기 전용으로 잠그면, 정작 필요한 순간에 값을 못
 * 고치고 원인도 안 보인다. **막는 대신 한 번 묻는다** — 마지막으로 사람이
 * 비밀번호를 친 지 `Auth::FRESH_MINUTES` 분이 지났으면 그때만.
 *
 * @section scope 어디에 거나
 *
 * **설정 쓰기에만** 건다(구역·화재 임계·현장 설정·계정). ⚠ 액추에이터 조작
 * (DEVICE)에는 걸지 않는다 — 그건 화재 대응이고, 펌프를 켜야 하는 순간에
 * 비밀번호 창을 띄우는 것은 안전 쪽에서 지는 거래다.
 *
 * @section modal 모달이다
 *
 * `QMessageBox` 와 같은 이유로 `AlertPopupStack` 등록 대상이 아니다 —
 * 등록 대상은 화면 위에 겹쳐 뜨는 경보 팝업이지, 조작을 막는 대화창이 아니다.
 */
class ReauthDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @brief 신선하면 그냥 통과, 아니면 물어본다
     *
     * @return 설정을 바꿔도 되나. 취소하거나 틀리면 false — 호출부는 아무것도
     *         보내지 않고 조용히 돌아가면 된다(사유는 이미 대화상자가 말했다).
     */
    static bool ensure_fresh(QWidget *parent);

private:
    explicit ReauthDialog(QWidget *parent);

    void submit();
    void render_error();
    void tick_lock();

    QLineEdit *m_pw = nullptr;
    QPushButton *m_ok = nullptr;
    QLabel *m_msg = nullptr;

    QString m_reason;
    int m_retry_s = 0;
    QTimer *m_lock_timer = nullptr;
};
