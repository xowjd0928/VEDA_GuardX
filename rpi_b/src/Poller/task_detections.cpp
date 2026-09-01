#include "Poller/task_detections.hpp"
#include "Poller/http_client.hpp"
#include "Poller/time_util.hpp"
#include <ctime>
#include <nlohmann/json.hpp>
#include <iostream>
#include <optional>
#include <string>

using json = nlohmann::json;

// 널 안전 추출: 키가 없거나 null 이면 nullopt → DB NULL. (nullable 컬럼용)
template <typename T>
static std::optional<T> jopt(const json& d, const char* key) {
  auto it = d.find(key);
  if (it == d.end() || it->is_null()) return std::nullopt;
  return it->get<T>();
}

// 널 안전 추출: 키가 없거나 null 이면 def. (NOT NULL 이나 기본값 무해한 컬럼용)
template <typename T>
static T jval(const json& d, const char* key, T def) {
  auto it = d.find(key);
  if (it == d.end() || it->is_null()) return def;
  return it->get<T>();
}

// 필수 필드(NOT NULL, 기본값 부적절) 존재+비널 여부
static bool present(const json& d, const char* key) {
  auto it = d.find(key);
  return it != d.end() && !it->is_null();
}

void pollDetections(const Config& cfg, pqxx::connection& db, State& st) {
  std::string url = cfg.base() + "/detections";
  if (!st.det_cursor.empty()) url += "?since=" + st.det_cursor;
  HttpResp r = httpGet(cfg, url);
  if (!r.ok()) { std::cerr << "[det] http " << r.code << "\n"; return; }

  json j;
  try { j = json::parse(r.body); }
  catch (...) { std::cerr << "[det] json parse fail\n"; return; }
  if (!j.contains("detections") || !j["detections"].is_array()) return;

  const auto& arr = j["detections"];
  if (arr.empty()) return;

  std::string max_ts = st.det_cursor;
  // v16: 미래 ts 가드 — 카메라가 드물게(~0.1%) 미래 시각 레코드를 찍는다.
  // 커서가 미래로 오염되면 since= 필터에 걸려 피드 전체가 그 시각까지 멈춤
  // (실사고 2026-07-28: face_cursor +20h). 지금+2분 초과 레코드는 적재도
  // 커서 전진도 하지 않는다 — 클램프만 하면 매 폴 중복 INSERT 폭주라
  // 스킵이 정답 (링에서 자연 소멸할 때까지 비교 1회/폴 비용뿐).
  const std::string ts_limit = epochToIso(time(nullptr) + 120);
  size_t n = 0, skipped = 0, future = 0;
  try {
    pqxx::work tx(db);
    for (const auto& d : arr) {
      const std::string ts = jval<std::string>(d, "ts", "");
      if (ts > ts_limit) { ++future; continue; }
      // 필수 필드 검증 — 없으면 SQL 안 던지고 건너뜀 (트랜잭션 오염·stuck 방지).
      // ts 가 있으면 커서만 전진시켜 malformed 레코드 무한 재수신 차단.
      if (!present(d, "object_id") || !present(d, "x") ||
          !present(d, "y") || ts.empty()) {
        ++skipped;
        if (!ts.empty() && ts > max_ts) max_ts = ts;
        continue;
      }

      // geom = ST_SetSRID(ST_MakePoint(x,y),0).
      // frame_w/h는 base 스키마에 없어 생략 (스키마 피드백 #1 반영 시 추가).
      // nullable 컬럼(likelihood, rect_*)은 없거나 null 이면 SQL NULL 로 저장.
      tx.exec(
        "INSERT INTO detections"
        " (camera_id, channel, object_id, category, parent_id, likelihood,"
        "  rect_sx, rect_sy, rect_ex, rect_ey, geom, ts)"
        " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,"
        "         ST_SetSRID(ST_MakePoint($11,$12),0), $13)",
        pqxx::params{
          cfg.camera_id,
          jval<int>(d, "channel", cfg.channel),
          jval<int>(d, "object_id", 0),
          jval<int>(d, "category", 1),
          jopt<int>(d, "parent_id"),   // v15: Face/Head → 사람 object_id
          jopt<double>(d, "likelihood"),
          jopt<int>(d, "rect_sx"), jopt<int>(d, "rect_sy"),
          jopt<int>(d, "rect_ex"), jopt<int>(d, "rect_ey"),
          jval<double>(d, "x", 0.0), jval<double>(d, "y", 0.0),
          ts});

      if (ts > max_ts) max_ts = ts;   // ISO8601 문자열 비교 = 시간순
      ++n;
    }
    tx.commit();
  } catch (const std::exception& e) {
    std::cerr << "[det] db error: " << e.what() << "\n";
    return;   // 커서 미전진 → 다음 틱에 재수신 (중복 방지)
  }
  if (max_ts > st.det_cursor) { st.det_cursor = max_ts; st.save(); }
  std::cout << "[det] +" << n << " rows";
  if (skipped) std::cout << " (skipped " << skipped << " malformed)";
  if (future)  std::cout << " (skipped " << future << " future-ts)";
  std::cout << ", cursor=" << st.det_cursor << "\n";
}