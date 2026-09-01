#pragma once
// task_prediction — GET /prediction?channel=N → congestion_prediction INSERT
// (horizon별 1행). v14: 채널 파라미터 추가 — zone_id는 그 채널의 존.
// 참고: VMS의 라이브 예측 표시는 이 태스크와 무관 — VMS가 카메라 /prediction
// 을 직접 GET (실시간=카메라 직결, DB=이력·경보·MAE 원칙, 2026-07-29 확정).
#include <pqxx/pqxx>
#include "Config/config.hpp"

void pollPrediction(const Config& cfg, pqxx::connection& db, int zone_id,
                    int channel);
