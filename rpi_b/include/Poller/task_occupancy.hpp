#pragma once
// task_occupancy — GET /occupancy?channel=N → zone_occupancy upsert (v15)
//   series_1min(최근 60분 중앙값) + series_end_utc 앵커를 통째로 upsert.
//   PK(zone_id, bucket_ts) + ON CONFLICT DO NOTHING = 멱등 — 최대 1시간의
//   통신 단절은 다음 폴에서 자동 복구된다.
// v16: 반환값 = now_smoothed (현재 인원수, median-of-5 프레임 ≈ 1초 창).
//   task_alert의 즉시 신호용 — 분 중앙값은 체류 판정이라 1~2분 지연이 있어
//   급증 감지가 늦다. -1 = 조회 실패(불명) — 0(없음)과 구분.
#include <pqxx/pqxx>
#include "Config/config.hpp"

int pollOccupancy(const Config& cfg, pqxx::connection& db, int zone_id,
                  int channel);
