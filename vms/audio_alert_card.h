#pragma once

#include <QDateTime>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

/**
 * @brief 오디오 경보 카드 한 장 — 한 종류(비명/총성)의 10분 집계 한 구간
 *
 * 예전에는 팝업이 **하나**였고 20초 뒤 스스로 닫혔다. 그래서 비명이 나는
 * 동안 총성이 나면 뒤엣것이 앞엣것을 덮었고, 운영자가 자리를 비운 사이의
 * 경보는 아무 흔적도 남기지 않고 사라졌다. 카드를 여러 장으로 나누고
 * 자동 닫힘을 없앤 이유가 이것이다.
 *
 * ── 집계 규칙 ──
 * 같은 종류의 감지가 반복되면 한 장에 누적한다. 집계 구간은 **최초 감지
 * 시각부터 고정 10분**이고, 반복 감지로 연장되지 않는다 — 연장되면 소리가
 * 계속 나는 현장에서 카드가 영원히 안 닫히고, "언제부터 언제까지"라는
 * 정보가 사라진다.
 *
 * 구간이 끝나도 카드는 화면에 남는다. 사라지는 유일한 조건은 운영자가
 * `확인`을 누르는 것이다. 확인하면 집계도 함께 끝나므로, 그 뒤의 감지는
 * 10분 이내라도 새 카드가 된다 — 이미 확인한 사건에 조용히 덧붙이면
 * 운영자는 그 뒤에 무슨 일이 있었는지 못 본다.
 *
 * 시각은 전부 TOIMIC payload 의 감지 timestamp 다(수신 시각이 아니다).
 *
 * 카드는 **메모리에만** 있다. DB 를 건드리지 않으므로 VMS 를 다시 켜면
 * 사라진다 — 오디오 경보의 정본은 RPi C 의 감지 로그이고, 여기는 운영자가
 * 지금 봐야 할 것을 띄우는 자리다.
 */
class AudioAlertCard : public QWidget
{
    Q_OBJECT

public:
    /** @brief 집계 구간 길이. 최초 감지 시각부터 고정. */
    static constexpr qint64 WINDOW_MS = 10 * 60 * 1000;

    AudioAlertCard(const QString &event, int channel, double confidence,
                   const QDateTime &ts, QWidget *parent = nullptr);

    /** @brief 이 카드가 담당하는 이벤트 종류 ("scream" | "gunshot") */
    QString event() const { return m_event; }

    /**
     * @brief 이 감지를 이 카드에 누적할 수 있는가
     *
     * 종류가 같고, 아직 확인되지 않았고, 최초 감지로부터 10분 안일 때만.
     * 셋 중 하나라도 아니면 호출부가 새 카드를 만든다.
     */
    bool accepts(const QString &event, const QDateTime &ts) const;

    /** @brief 감지 한 건 누적 (최근 시각·횟수·최대 confidence 갱신) */
    void add_detection(int channel, double confidence, const QDateTime &ts);

    /**
     * @brief 대기 상태 — 화면에 자리가 없어 잠시 숨겨둔 카드
     *
     * 확인된 것과는 다르다. 집계는 계속되고, 앞의 카드가 확인되면 그대로
     * 뜬다. 이 구분이 없으면 대기 중에 들어온 감지가 카드를 도로 띄워서
     * 장수 제한이 무의미해진다.
     */
    void set_pending(bool pending);
    bool pending() const { return m_pending; }

signals:
    /** @brief `확인` 클릭 — 호출부가 카드를 목록에서 빼고 파괴한다 */
    void acknowledged(AudioAlertCard *card);
    /** @brief "LIVE 보기" 클릭 */
    void goto_live_requested(int channel);

private:
    void refresh();
    void apply_frame();

    const QString m_event;
    const QDateTime m_first_ts;
    QDateTime m_last_ts;
    int m_channel = -1;
    int m_count = 1;
    double m_max_confidence = 0.0;
    bool m_acknowledged = false;
    bool m_pending = false;

    QLabel *m_title = nullptr;
    QLabel *m_when = nullptr;
    QLabel *m_body = nullptr;
    QPushButton *m_btn_live = nullptr;
    QPushButton *m_btn_ack = nullptr;

    QTimer *m_pulse = nullptr;
    bool m_pulse_on = true;
};
