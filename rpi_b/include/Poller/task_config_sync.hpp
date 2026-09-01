#pragma once
// task_config_sync — 형상 동기화 (§9.3 B+C: B가 형상 소유)
//   WiseAI CGI 폴링 → 본문 해시 비교 → 변경 시 version++ →
//   카메라 GET /config?version=N 통지 → zones.roi_polygon 갱신
#include <map>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <string>
#include "Config/config.hpp"
#include "Storage/state.hpp"

// v14: 채널→zone_id 맵으로 다채널 형상 동기화. 해시(변경 감지)는 전 채널
// 응답 본문 전체 기준(전역 epoch 1개), 존 갱신은 채널별로 수행.
void syncConfig(const Config& cfg, pqxx::connection& db, State& st,
                const std::map<int, int>& zone_by_ch);

// ivaarea 응답에서 channel의 areaCoordinates → WKT POLYGON (닫힌 링)
std::string polygonWkt(const nlohmann::json& ivaarea, int channel);