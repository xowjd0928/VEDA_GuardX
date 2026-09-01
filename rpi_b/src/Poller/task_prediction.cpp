#include "Poller/task_prediction.hpp"
#include "Poller/http_client.hpp"
#include "Poller/time_util.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <optional>

using json = nlohmann::json;

// v2 계약 (CAMERA_API_v15.md): horizon {5,30,60,180}분, 값은 predictions[].p50,
// model.version='hw_damped_v1'.
void pollPrediction(const Config& cfg, pqxx::connection& db, int zone_id,
                    int channel) {
  HttpResp r = httpGet(cfg, cfg.base() + "/prediction?channel=" +
                                std::to_string(channel));
  if (!r.ok()) { std::cerr << "[pred] http " << r.code << "\n"; return; }
  json j;
  try { j = json::parse(r.body); }
  catch (...) { std::cerr << "[pred] parse fail\n"; return; }
  if (!j.contains("predictions")) return;

  const std::string served = j.value("served_utc", "");
  const time_t served_ep = isoToEpoch(served);
  if (served_ep == 0) { std::cerr << "[pred] bad served_utc\n"; return; }

  // 스키마 피드백 4호 (2026-07-28): warmup 행도 적재 — warmup 컬럼으로 구분.
  // 소비자(task_alert·MAE 평가)는 warmup IS NOT TRUE 로 거른다.
  const json model = j.value("model", json::object());
  const bool warmup = model.value("warmup", false);
  const std::string model_version = model.value("version", "hw_damped_v1");

  try {
    pqxx::work tx(db);
    int rows = 0;
    for (const auto& p : j["predictions"]) {
      const int horizon = p.value("horizon_min", 0);
      if (horizon <= 0 || !p.contains("p50")) continue;
      const double p50 = p.value("p50", 0.0);
      const std::string target = epochToIso(served_ep + (time_t)horizon * 60);
      // 피드백 4호: p10/p90은 명 단위 소수 그대로, p_over_capacity 는
      // -1(불명) → NULL (확률 0 아님). config_version 은 v13부터 NULL 고정.
      std::optional<double> p10, p90, poc;
      if (p.contains("p10") && p["p10"].is_number()) p10 = p["p10"].get<double>();
      if (p.contains("p90") && p["p90"].is_number()) p90 = p["p90"].get<double>();
      const double pocv = p.value("p_over_capacity", -1.0);
      if (pocv >= 0.0) poc = pocv;
      tx.exec(
        "INSERT INTO congestion_prediction"
        " (zone_id, predicted_at, target_ts, predicted_count,"
        "  p10, p90, p_over_capacity, warmup, model_version, config_version)"
        " VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, NULL)",
        pqxx::params{
          zone_id, served, target,
          (int)(p50 + 0.5),
          p10, p90, poc, warmup,
          model_version});
      ++rows;
    }
    tx.commit();
    std::cout << "[pred] ch" << channel << " zone " << zone_id
              << " served=" << served << " rows=" << rows
              << " model=" << model_version
              << (warmup ? " (warmup)" : "") << "\n";
  } catch (const std::exception& e) {
    std::cerr << "[pred] db error: " << e.what() << "\n";
  }
}
