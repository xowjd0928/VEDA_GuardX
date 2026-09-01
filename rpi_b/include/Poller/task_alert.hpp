#pragma once
// task_alert — 혼잡 경보 발화기. pred 틱(60초)에서 존별 호출.
// 최신 예측(p50, 최근접 horizon)·실측(zone_occupancy)을 zone_thresholds와
// 비교해 incidents + alerts 발화. 상태 전이 시에만 발화 (hysteresis).
// capacity_limit NULL 존은 판단 불가 → skip (값 채우면 자동 활성).
// DB만 읽고 쓴다 — 카메라 HTTP 비의존이라 Config를 받지 않음.
// v16: current_count = pollOccupancy가 방금 받은 현재 인원(now_smoothed).
//   분 중앙값(체류)·예측과 함께 3신호 중 나쁜 쪽 채택 — 급증은 즉시(현재값),
//   순간 드롭아웃은 무시(중앙값이 바닥을 받침). -1 = 불명(신호 제외).
#include <pqxx/pqxx>

void pollAlert(pqxx::connection& db, int zone_id, int channel,
               int current_count);
