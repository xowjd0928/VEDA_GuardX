#include "Poller/task_occupancy.hpp"
#include "Poller/http_client.hpp"
#include "Poller/time_util.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

// /occupancy의 series_1min은 오래된 것부터 최대 60개, 마지막 원소가
// series_end_utc 분(minute)의 중앙값이다. i번째 원소의 분 =
// end - (len-1-i). 사람이 없던 분은 카메라가 0으로 backfill해 두므로
// 여기 값은 "실측"이다 (v13 §3.1 신호 사슬).
int pollOccupancy(const Config& cfg, pqxx::connection& db, int zone_id,
                  int channel) {
  HttpResp r = httpGet(cfg, cfg.base() + "/occupancy?channel=" +
                                std::to_string(channel));
  if (!r.ok()) { std::cerr << "[occ] ch" << channel << " http " << r.code << "\n"; return -1; }
  json j;
  try { j = json::parse(r.body); }
  catch (...) { std::cerr << "[occ] parse fail\n"; return -1; }

  // v16: 현재 인원 (카메라 median-of-5, 신선도 2초 — stale이면 카메라가 0).
  const int now_cnt = j.value("now_smoothed", -1);

  const std::string end_utc = j.value("series_end_utc", "");
  if (end_utc.empty() || !j.contains("series_1min")) return now_cnt;   // 관측 전 (정상)
  const time_t end_ep = isoToEpoch(end_utc);
  if (end_ep == 0) { std::cerr << "[occ] bad series_end_utc\n"; return now_cnt; }
  const long long end_min = (long long)end_ep / 60;

  const auto& series = j["series_1min"];
  if (!series.is_array() || series.empty()) return now_cnt;
  const long long n = (long long)series.size();

  try {
    pqxx::work tx(db);
    for (long long i = 0; i < n; ++i) {
      if (!series[i].is_number()) continue;
      const std::string bucket = epochToIso((time_t)((end_min - (n - 1 - i)) * 60));
      tx.exec(
        "INSERT INTO zone_occupancy (zone_id, bucket_ts, person_count)"
        " VALUES ($1, $2, $3) ON CONFLICT (zone_id, bucket_ts) DO NOTHING",
        pqxx::params{zone_id, bucket, series[i].get<int>()});
    }
    tx.commit();
  } catch (const std::exception& e) {
    std::cerr << "[occ] db error: " << e.what() << "\n";
  }
  return now_cnt;
}
