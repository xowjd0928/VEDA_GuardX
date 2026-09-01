#include "audio_alert_center.h"

#include "alert_feed.h"
#include "audio_alert_card.h"

#include <QDateTime>
#include <QWidget>

AudioAlertCenter *AudioAlertCenter::instance()
{
    static AudioAlertCenter center;
    return &center;
}

AudioAlertCenter::AudioAlertCenter(QObject *parent) : QObject(parent) {}

void AudioAlertCenter::attach(QWidget *parent)
{
    m_parent = parent;
    if (m_subscribed)
        return;
    m_subscribed = true;

    connect(AlertFeed::instance(), &AlertFeed::audio_alert, this,
            &AudioAlertCenter::on_audio_alert);
}

void AudioAlertCenter::on_audio_alert(int channel, const QString &event,
                                      double confidence, const QDateTime &ts)
{
    // 죽은 포인터를 먼저 걷어낸다. 카드는 확인 시 deleteLater 로 사라지므로
    // 목록에 빈 자리가 생긴다.
    m_cards.removeIf([](const QPointer<AudioAlertCard> &c) { return c.isNull(); });

    // 열려 있는 같은 종류의 카드를 뒤에서부터 찾는다 — 같은 종류의 카드가
    // 여러 장 있을 수 있고(확인 후 재감지, 10분 경과 후 새 구간), 그중
    // 누적 대상은 항상 가장 최근 것이다.
    for (int i = m_cards.size() - 1; i >= 0; --i) {
        AudioAlertCard *card = m_cards.at(i);
        if (card && card->accepts(event, ts)) {
            card->add_detection(channel, confidence, ts);
            return;
        }
    }

    auto *card = new AudioAlertCard(event, channel, confidence, ts, m_parent);
    connect(card, &AudioAlertCard::acknowledged, this, &AudioAlertCenter::drop);
    connect(card, &AudioAlertCard::goto_live_requested, this,
            &AudioAlertCenter::goto_live_requested);
    m_cards.append(card);

    // 새 카드가 자리를 넘기면 **새 것**을 대기시킨다. 오래된 것을 내리면
    // 운영자가 아직 안 본 경보가 화면에서 사라진다 — 확인 전에는 사라지지
    // 않는다는 계약을 깨는 쪽이다.
    int visible = 0;
    for (const QPointer<AudioAlertCard> &c : m_cards)
        if (c && !c->pending())
            ++visible;
    if (visible > MAX_VISIBLE)
        card->set_pending(true);
}

void AudioAlertCenter::drop(AudioAlertCard *card)
{
    if (!card)
        return;
    m_cards.removeIf([card](const QPointer<AudioAlertCard> &c) {
        return c.isNull() || c == card;
    });
    // 시그널 처리 중이라 지금 지우면 안 된다.
    card->deleteLater();

    promote_pending();
}

void AudioAlertCenter::promote_pending()
{
    int visible = 0;
    for (const QPointer<AudioAlertCard> &c : m_cards)
        if (c && !c->pending())
            ++visible;

    // 오래된 것부터 올린다 — 대기열은 먼저 들어온 순서대로 나가야 한다.
    for (const QPointer<AudioAlertCard> &c : m_cards) {
        if (visible >= MAX_VISIBLE)
            break;
        if (c && c->pending()) {
            c->set_pending(false);
            ++visible;
        }
    }
}
