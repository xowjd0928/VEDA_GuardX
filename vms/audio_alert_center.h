#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>

class AudioAlertCard;
class QWidget;

/**
 * @brief 오디오 경보 카드들의 관리자 — 어느 카드에 넣을지, 언제 새로 만들지
 *
 * `AlertFeed::audio_alert` 하나를 받아 카드 여러 장으로 나눈다. 규칙은
 * AudioAlertCard 헤더에 있고, 여기서는 그 규칙을 적용할 카드를 고른다:
 * 같은 종류의 **열려 있는** 카드가 있으면 거기 누적하고, 없으면 새로 만든다.
 *
 * 상태는 전부 메모리다. DB 도 스키마도 건드리지 않으므로 VMS 를 다시 켜면
 * 카드가 사라진다 — 허용된 동작이다.
 *
 * ── 화면에 몇 장까지 ──
 * 카드는 확인하기 전까지 사라지지 않으므로, 아무도 확인하지 않으면 계속
 * 쌓인다. AlertPopupStack 은 세로로 쌓기만 하므로 그대로 두면 화면 밖으로
 * 밀려 **보이지 않는 미확인 경보**가 생긴다 — 예전에 음향 팝업이 화재
 * 팝업과 겹쳐 가려졌던 것과 같은 실패다.
 *
 * 그래서 동시에 띄우는 장수를 제한하고, 넘치는 것은 **버리지 않고** 대기
 * 시킨다. 앞의 카드를 확인하면 대기하던 것이 그 자리에 뜬다. 확인 전에
 * 사라지는 카드는 없다는 계약이 유지된다.
 */
class AudioAlertCenter : public QObject
{
    Q_OBJECT

public:
    /** @brief 동시에 화면에 띄우는 최대 장수. 나머지는 대기한다. */
    static constexpr int MAX_VISIBLE = 4;

    static AudioAlertCenter *instance();

    /**
     * @brief 카드를 띄울 부모 창을 지정하고 구독을 시작한다 (MainWindow 에서 1회)
     *
     * 부모가 있어야 AlertPopupStack 이 창 중앙에 맞춰 쌓는다. 부모 없이
     * 만들면 고아 좌표로 떨어진다.
     */
    void attach(QWidget *parent);

signals:
    /** @brief 카드의 "LIVE 보기" — MainWindow 가 해당 채널로 전환한다 */
    void goto_live_requested(int channel);

private:
    explicit AudioAlertCenter(QObject *parent = nullptr);

    void on_audio_alert(int channel, const QString &event, double confidence,
                        const QDateTime &ts);
    void drop(AudioAlertCard *card);
    /** @brief 대기 중인 카드를 빈자리만큼 띄운다 */
    void promote_pending();

    QPointer<QWidget> m_parent;
    bool m_subscribed = false;
    /// 생성 순서 유지. 앞쪽이 오래된 카드다.
    QVector<QPointer<AudioAlertCard>> m_cards;
};
