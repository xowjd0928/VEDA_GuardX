#include "Poller/task_faces.hpp"
#include "Poller/http_client.hpp"
#include "Poller/time_util.hpp"
#include <ctime>
#include <nlohmann/json.hpp>
#include <iostream>
#include <optional>
#include <string>

using json = nlohmann::json;

namespace {
template <typename T>
std::optional<T> jopt(const json& d, const char* key) {
  auto it = d.find(key);
  if (it == d.end() || it->is_null()) return std::nullopt;
  return it->get<T>();
}
}  // namespace

// task_detections와 동일한 커서·배치·방어 패턴 (그쪽 주석 참조).
void pollFaces(const Config& cfg, pqxx::connection& db, State& st) {
  std::string url = cfg.base() + "/faces";
  if (!st.face_cursor.empty()) url += "?since=" + st.face_cursor;
  HttpResp r = httpGet(cfg, url);
  if (!r.ok()) { std::cerr << "[face] http " << r.code << "\n"; return; }

  json j;
  try { j = json::parse(r.body); }
  catch (...) { std::cerr << "[face] json parse fail\n"; return; }
  if (!j.contains("faces") || !j["faces"].is_array()) return;

  const auto& arr = j["faces"];
  if (arr.empty()) return;

  std::string max_ts = st.face_cursor;
  // v16: 미래 ts 가드 — task_detections와 동일 (그쪽 주석 참조).
  // 실사고 2026-07-28: 미래 ts 레코드가 face_cursor를 +20h로 밀어 피드 정지.
  const std::string ts_limit = epochToIso(time(nullptr) + 120);
  size_t n = 0, future = 0;
  try {
    pqxx::work tx(db);
    for (const auto& f : arr) {
      const std::string ts = f.value("ts", "");
      if (ts > ts_limit) { ++future; continue; }   // 적재·커서 전진 모두 금지
      if (ts.empty() || !f.contains("object_id")) {
        if (ts > max_ts) max_ts = ts;   // malformed: 커서만 전진
        continue;
      }
      tx.exec(
        "INSERT INTO faces"
        " (camera_id, channel, object_id, likelihood,"
        "  rect_sx, rect_sy, rect_ex, rect_ey, image_ref, ts)"
        " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
        pqxx::params{
          cfg.camera_id,
          f.value("channel", cfg.channel),
          f.value("object_id", 0),
          jopt<double>(f, "likelihood"),
          jopt<int>(f, "rect_sx"), jopt<int>(f, "rect_sy"),
          jopt<int>(f, "rect_ex"), jopt<int>(f, "rect_ey"),
          jopt<std::string>(f, "image_ref"),
          ts});
      if (ts > max_ts) max_ts = ts;
      ++n;
    }
    tx.commit();
  } catch (const std::exception& e) {
    std::cerr << "[face] db error: " << e.what() << "\n";
    return;   // 커서 미전진 → 재수신
  }
  if (max_ts > st.face_cursor) { st.face_cursor = max_ts; st.save(); }
  if (n || future) {
    std::cout << "[face] +" << n << " rows";
    if (future) std::cout << " (skipped " << future << " future-ts)";
    std::cout << ", cursor=" << st.face_cursor << "\n";
  }
}
