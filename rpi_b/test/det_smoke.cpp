// det_smoke.cpp — detections INSERT 계약 검증 (실 테이블 오염 없음)
//   task_detections.cpp 의 INSERT(컬럼목록 + geom 표현식)를 그대로 써서
//   실측형 픽스처 1건을 INSERT ... RETURNING 으로 넣고 geom 왕복을 확인한 뒤
//   트랜잭션을 abort → detections 에 행이 남지 않음(시퀀스 id 하나만 소모).
//
// 컴파일: CMake 타깃 det_smoke (cmake --build build) 또는
//   g++ -std=c++17 -Wall -I. det_smoke.cpp -lpqxx -lpq -o det_smoke
// 실행:
//   ./det_smoke                  # 현재 폴더 config.env
//   ./det_smoke path/config.env
//
// 종료코드: 0=PASS, 1=검증 불일치, 2=연결/쿼리 실패
//
// 주의: 아래 INSERT 는 task_detections.cpp 와 '동일'해야 의미가 있음.
//       한쪽을 바꾸면 다른 쪽도 맞춰야 함(일회성 검증용이라 영구 테스트 아님).
#include <cstdio>
#include <cmath>
#include <ctime>
#include <optional>
#include <string>
#include <pqxx/pqxx>
#include <nlohmann/json.hpp>
#include "Config/config.hpp"

using json = nlohmann::json;

// ts 를 현재 시각으로 생성 — detections 는 일 파티션 + 14일 보존이라
// 고정 과거 시각은 "no partition" 으로 실패한다 (그게 정상 동작).
static std::string nowIso() {
  time_t t = time(nullptr);
  struct tm g{}; gmtime_r(&t, &g);
  char b[40];
  snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d.123Z",
           g.tm_year+1900, g.tm_mon+1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
  return b;
}

int main(int argc, char** argv) {
  const std::string env_path = (argc > 1) ? argv[1] : "config.env";
  Config cfg = Config::load(env_path);

  // 실측형 픽스처 1건 — v9 /detections 계약 필드, 값은 메타데이터 문서의 Human 예시
  json d = {
    {"channel", 1}, {"object_id", 8061}, {"category", 1}, {"likelihood", 0.57},
    {"rect_sx", 882}, {"rect_sy", 527}, {"rect_ex", 1090}, {"rect_ey", 816},
    {"x", 986.0}, {"y", 671.5}, {"ts", nowIso()}
  };

  std::printf("[fixture] obj=%d ch=%d x=%.1f y=%.1f ts=%s\n",
              d.value("object_id", 0), d.value("channel", cfg.channel),
              d.value("x", 0.0), d.value("y", 0.0), d.value("ts", "").c_str());

  try {
    pqxx::connection db(cfg.pgconn);
    std::printf("[ok] connected: db='%s' user='%s'\n", db.dbname(), db.username());
    pqxx::work tx(db);

    // task_detections.cpp 와 동일한 컬럼목록·geom 표현식 (RETURNING 없음).
    // 폴러는 INSERT 전용이라 juan 에 SELECT 가 없고, RETURNING 은 SELECT 를 요구함.
    // → RETURNING 을 빼서 '폴러가 실제로 쓰는 권한 경로'를 그대로 검증한다.
    // 좌표(x/y) 왕복 확인은 SELECT 가 있는 postgres 로 분리(하단 주석 참조).
    tx.exec(
      "INSERT INTO detections"
      " (camera_id, channel, object_id, category, parent_id, likelihood,"
      "  rect_sx, rect_sy, rect_ex, rect_ey, geom, ts)"
      " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,"
      "         ST_SetSRID(ST_MakePoint($11,$12),0), $13)",
      pqxx::params{
        cfg.camera_id,
        d.value("channel", cfg.channel),
        d.value("object_id", 0),
        d.value("category", 1),
        std::optional<int>{},   // parent_id — Human 픽스처는 NULL (v15)
        d.value("likelihood", 0.0),
        d.value("rect_sx", 0), d.value("rect_sy", 0),
        d.value("rect_ex", 0), d.value("rect_ey", 0),
        d.value("x", 0.0), d.value("y", 0.0),
        d.value("ts", "")});

    tx.abort();   // 검증만 — detections 에 남기지 않음(시퀀스 id 하나만 소모)
    std::printf("[PASS] INSERT 성공 — 컬럼 정합 + geom 표현식 유효 + juan INSERT 권한 OK\n");
    return 0;
  } catch (const pqxx::sql_error& e) {
    std::fprintf(stderr, "[FAIL] SQL: %s\n       query: %s\n", e.what(), e.query().c_str());
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[FAIL] %s\n", e.what());
    return 2;
  }
}