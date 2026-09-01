#pragma once

#include <QNetworkRequest>
#include <QUrl>

/**
 * @brief 공통 규약이 걸린 SUNAPI/OpenSDK 요청 — 타임아웃을 깜빡할 수 없게 한다
 *
 * 12곳에서 QNetworkRequest를 직접 만들었는데 그중 5곳에 타임아웃이 없었다
 * (2026-08-10). 응답 없이 매달린 reply는 `finished`를 내지 않고, 폴러들의
 * in-flight 가드(DetectionFeed::m_pending, PredictionFeed::m_pending[ch])는
 * 그 `finished` 핸들러에서만 풀리므로 — 한 번 매달리면 그 피드는 영구히
 * 멈춘다. 재부팅 전까지 박스도 예측도 갱신되지 않는다.
 *
 * 그래서 "요청 만들기"를 한 함수로 모은다. 새 요청을 추가하는 사람이
 * setTransferTimeout을 기억할 필요가 없다 — 기본값이 이미 걸려 나온다.
 *
 * **Accept 헤더는 넣지 않는다.** SUNAPI는 같은 호스트에서 JSON 엔드포인트와
 * `Key=Value` 줄 텍스트 엔드포인트(videoprofile·wisestream·network·log 등)를
 * 함께 쓴다. 헬퍼가 `Accept: application/json`을 일괄로 붙이면 텍스트를
 * 기대하고 파싱하는 호출부의 응답 형식을 바꿀 위험이 있다. JSON을 요구하는
 * 호출부는 지금처럼 각자 `req.setRawHeader("Accept", ...)`를 명시한다 —
 * 헬퍼를 늘리는 대신 특수한 것만 호출부에 남긴다.
 *
 * @param timeout_ms 무전송 타임아웃(ms). 응답 총 시간이 아니라 "이 시간 동안
 *                   한 바이트도 못 받으면 끊는다"는 뜻이라 느린 링크에서도
 *                   진행 중인 전송을 자르지 않는다. 설정 파일 조회처럼 원래
 *                   오래 걸리는 요청은 호출부가 값을 명시한다.
 */
inline QNetworkRequest sunapi_request(const QUrl &url, int timeout_ms = 5000)
{
    QNetworkRequest req(url);
    req.setTransferTimeout(timeout_ms);
    return req;
}
