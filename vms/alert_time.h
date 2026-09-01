#pragma once

#include <QDateTime>
#include <QDebug>
#include <QString>

/**
 * @file alert_time.h
 * @brief 경보 페이로드의 **사건 시각**과 신선도 판정 — 한 곳
 *
 * 왜 모았는가 (2026-08-10 리뷰): 같은 판정이 파일마다 제각각이었다.
 * `FireAlertFeed::on_button` 은 payload 의 timestamp 를 쓰는데 같은 파일의
 * `on_live_alert` 는 수신 시각을 쓰고, `AlertFeed::on_audio_alert` 는
 * timestamp 를 **읽지도 않아서** 비명·총성 팝업에 수신 시각이 사건 시각으로
 * 찍혔다. 링크가 밀리면 그 차이가 그대로 오표시가 된다.
 *
 * 한 지표를 여러 곳에서 각자 계산하면 반드시 어긋난다 — 그래서 경보
 * 페이로드의 시각을 다루는 코드는 전부 여기를 지난다.
 */

/// 순간 이벤트를 "지금 일어난 일"로 띄울 수 있는 최대 나이
inline constexpr qint64 ALERT_MOMENTARY_MAX_AGE_MS = 60000;

/**
 * @brief payload timestamp(epoch ms) → 사건 시각. 없으면 수신 시각.
 *
 * ⚠ 0 은 "1970년"이 아니라 **필드가 없다**는 뜻이다. 발행자 버전이 섞여 있어
 * 반드시 구분해야 한다 — 0 을 시각으로 믿으면 모든 경보가 56년 묵은 것이 된다.
 */
inline QDateTime alert_event_time(qint64 ts_ms)
{
    return ts_ms > 0 ? QDateTime::fromMSecsSinceEpoch(ts_ms)
                     : QDateTime::currentDateTime();
}

/**
 * @brief **순간** 이벤트(비명·총성 등)가 지금 띄울 만큼 신선한가
 *
 * ⚠ **상태를 나르는 경보에는 쓰지 말 것** — `fire_confirmed` 나 혼잡 단계는
 * "그때 그런 일이 있었다"가 아니라 "지금 이 상태다"를 말한다. 60초 전에
 * 시작한 화재는 여전히 화재이므로, 낡았다고 버리면 실제로 진행 중인 사건을
 * 화면에서 지우게 된다. 그쪽은 버리는 대신 사건 시각을 정확히 실어 보내고,
 * 순서 문제는 각 피드의 `m_last_live_ms` 비교가 이미 지키고 있다.
 *
 * 반대로 비명·총성은 **점 사건**이다. 지나간 것을 지금 일처럼 띄우면 운영자가
 * 아무것도 없는 현장으로 달려간다 — 그래서 이쪽만 버린다.
 *
 * 미래 시각(나이가 음수)은 통과시킨다. 발행자(RPi) 시계가 앞선 것을 경보
 * 억제의 사유로 삼지 않는다 — 억제는 언제나 위험한 쪽 오류다.
 */
inline bool alert_momentary_is_fresh(qint64 ts_ms, const QString &what)
{
    if (ts_ms <= 0)
        return true;   // 시각을 모르면 판정하지 않는다 (모름 ≠ 낡음)

    const qint64 age = QDateTime::currentMSecsSinceEpoch() - ts_ms;
    if (age <= ALERT_MOMENTARY_MAX_AGE_MS)
        return true;

    qWarning().noquote()
        << QString("[Alert] %1 무시 — %2초 지난 사건 (상한 %3초). "
                   "상시 발생하면 발행자와 PC 시계를 맞출 것")
               .arg(what)
               .arg(age / 1000)
               .arg(ALERT_MOMENTARY_MAX_AGE_MS / 1000);
    return false;
}
