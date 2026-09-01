#include "Poller/task_flow.hpp"
#include "Poller/http_client.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <map>
#include <string>

using json = nlohmann::json;

void pollFlow(const Config& cfg, pqxx::connection& db) {
  // 직전 폴의 누적 카운트 (메모리 전용 — 폴러 재시작 시 첫 폴은 기준선만
  // 잡고 적재하지 않는다. 재시작 사이의 통과분은 유실이 아니라 미계측:
  // /events 자체가 누적이라 총량은 카메라가 들고 있다.)
  static std::map<std::string, long long> prev;

  HttpResp r = httpGet(cfg, cfg.base() + "/events");
  if (!r.ok()) { std::cerr << "[flow] http " << r.code << "\n"; return; }
  json j;
  try { j = json::parse(r.body); }
  catch (...) { std::cerr << "[flow] parse fail\n"; return; }
  if (!j.contains("rules") || !j["rules"].is_array()) return;

  const std::string served = j.value("served_utc", "");

  try {
    pqxx::work tx(db);
    size_t n = 0;
    for (const auto& ru : j["rules"]) {
      const std::string topic = ru.value("topic", "");
      if (topic.find("LineCrossing") == std::string::npos) continue;
      const std::string rule   = ru.value("rule", "");
      const std::string action = ru.value("action", "");
      if (rule.empty() || action.empty()) continue;

      const long long count = ru.value("count", 0LL);
      const std::string key = rule + "|" + action;
      auto it = prev.find(key);
      if (it == prev.end()) { prev[key] = count; continue; }   // 첫 폴: 기준선
      const long long delta = count - it->second;
      it->second = count;
      if (delta < 0) { continue; }   // 카메라 앱 재시작 (카운터 리셋) — 재기준선
      if (delta == 0) { continue; }  // 통과 없음 — 0행은 적재 안 함 (공백=0 해석)

      tx.exec(
        "INSERT INTO line_flow (rule, action, bucket_ts, flow_count)"
        " VALUES ($1, $2, date_trunc('minute', $3::timestamptz), $4)"
        " ON CONFLICT (rule, action, bucket_ts)"
        " DO UPDATE SET flow_count = line_flow.flow_count + EXCLUDED.flow_count",
        pqxx::params{rule, action, served, delta});
      ++n;
    }
    tx.commit();
    if (n) { std::cout << "[flow] +" << n << " rule-buckets\n"; }
  } catch (const std::exception& e) {
    std::cerr << "[flow] db error: " << e.what() << "\n";
  }
}
