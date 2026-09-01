#include "Poller/task_config_sync.hpp"
#include "Poller/http_client.hpp"
#include <iostream>

using json = nlohmann::json;

std::string polygonWkt(const json& ivaarea, int channel) {
  if (!ivaarea.contains("ivaArea")) return "";
  for (const auto& ch : ivaarea["ivaArea"]) {
    if (ch.value("channel", -1) != channel) continue;
    if (!ch.contains("definedArea") || ch["definedArea"].empty()) return "";
    const auto& area = ch["definedArea"][0];
    if (!area.contains("areaCoordinates")) return "";
    std::string wkt = "POLYGON((";
    std::string first;
    bool sep = false;
    for (const auto& pt : area["areaCoordinates"]) {
      char b[32];
      snprintf(b, sizeof(b), "%d %d", pt.value("x", 0), pt.value("y", 0));
      if (sep) wkt += ", ";
      wkt += b; sep = true;
      if (first.empty()) first = b;
    }
    wkt += ", " + first + "))";   // 링 닫기
    return wkt;
  }
  return "";
}

void syncConfig(const Config& cfg, pqxx::connection& db, State& st,
                const std::map<int, int>& zone_by_ch) {
  HttpResp lc = httpGet(cfg, cfg.wiseai() + "/linecrossing");
  HttpResp iv = httpGet(cfg, cfg.wiseai() + "/ivaarea");
  if (!lc.ok() || !iv.ok()) { std::cerr << "[cfg] cgi http err\n"; return; }

  const uint64_t h = fnv1a(lc.body + "\x1f" + iv.body);
  if (h == st.shape_hash) return;                    // 변경 없음 — 흔한 경로 (저비용)

  // 첫 실행(state 리셋 포함): 기준 해시가 없어 '변경'을 판정할 수 없음.
  // 통지·버전업 없이 현재 형상을 기준으로만 채택 (헛 version 증가 방지).
  if (st.shape_hash == 0) {
    st.shape_hash = h;
    st.save();
    std::cout << "[cfg] baseline hash recorded (no version bump)\n";
    return;
  }

  // 변경 감지 → 새 epoch
  const int new_version = st.config_version + 1;
  std::cout << "[cfg] shape change detected -> version " << new_version << "\n";

  // (a) 카메라 앱에 통지 (이후 버킷이 새 version 태그, 예측·from-flow 새 epoch)
  HttpResp notify = httpGet(cfg, cfg.base() + "/config?version=" + std::to_string(new_version));
  if (!notify.ok()) { std::cerr << "[cfg] notify failed http " << notify.code << "\n"; return; }

  // (b) 형상 버저닝 (Tier2: zone_geometry_history 이력 전환) — v14: 채널별.
  //     채널마다 한 트랜잭션 안에서 원자적으로:
  //       1) 현재 열린 이력행 마감(valid_to=now)
  //       2) 새 형상 이력행 추가(valid_from=now, config_version=new)
  //       3) zones 현재 캐시(roi_polygon) + epoch(config_version) 갱신
  //     zones.zone_id 는 불변 → congestion_prediction/devices/incidents FK 안전.
  //     IVA 영역이 없는 채널은 건너뜀 (전체 화면 ROI 유지).
  try {
    json ivj = json::parse(iv.body);
    for (const auto& zc : zone_by_ch) {
      const int ch = zc.first;
      const int zone_id = zc.second;
      std::string wkt = polygonWkt(ivj, ch);
      if (wkt.empty()) { continue; }
      pqxx::work tx(db);
      tx.exec(
        "UPDATE zone_geometry_history SET valid_to = now()"
        " WHERE zone_id = $1 AND valid_to IS NULL",
        pqxx::params{zone_id});
      tx.exec(
        "INSERT INTO zone_geometry_history"
        " (zone_id, roi_polygon, config_version, valid_from)"
        " VALUES ($1, ST_GeomFromText($2, 0), $3, now())",
        pqxx::params{zone_id, wkt, new_version});
      tx.exec(
        "UPDATE zones SET roi_polygon = ST_GeomFromText($1, 0), config_version = $2"
        " WHERE zone_id = $3",
        pqxx::params{wkt, new_version, zone_id});
      tx.commit();
      std::cout << "[cfg] ch" << ch << " zone " << zone_id
                << " roi versioned v" << new_version << ": " << wkt << "\n";
    }
  } catch (const std::exception& e) {
    std::cerr << "[cfg] zone update error: " << e.what() << "\n";
    return;
  }

  st.shape_hash = h;
  st.config_version = new_version;
  st.save();
}