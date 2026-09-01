// db_smoke.cpp — DB 실연결 스모크 테스트
//   config.env 의 PGCONN 으로 접속 → lookupZoneId(camera_id, channel) 검증.
//   기대: (camera_id=1, channel=1) → zone_A → zone_id=1 (v2 시드: 채널 1 단일 존).
//
// 컴파일: CMake 타깃 db_smoke (cmake --build build) 또는
//   g++ -std=c++17 -Wall -I. db_smoke.cpp Database/db.cpp -lpqxx -lpq -o db_smoke
// 실행:
//   ./db_smoke                  # 현재 폴더의 config.env 사용
//   ./db_smoke path/to/config.env
//
// 종료코드: 0=PASS, 1=값 불일치, 2=연결/쿼리 실패
#include <cstdio>
#include <string>
#include <pqxx/pqxx>
#include "Config/config.hpp"
#include "Database/db.hpp"

// PGCONN 로그 출력 시 password 값만 가림 (평문 유출 방지)
static std::string redact(const std::string& s) {
  const std::string key = "password=";
  auto p = s.find(key);
  if (p == std::string::npos) return s;
  auto start = p + key.size();
  auto end = s.find(' ', start);          // conn 문자열은 공백 구분
  std::string out = s.substr(0, start) + "****";
  if (end != std::string::npos) out += s.substr(end);
  return out;
}

int main(int argc, char** argv) {
  const std::string env_path = (argc > 1) ? argv[1] : "config.env";
  Config c = Config::load(env_path);

  std::printf("[cfg] env_path = %s\n", env_path.c_str());
  std::printf("[cfg] pgconn   = %s\n", redact(c.pgconn).c_str());
  std::printf("[cfg] lookup   = camera_id=%d, channel=%d  (expect zone_A, zone_id=1)\n",
              c.camera_id, c.channel);

  try {
    pqxx::connection db(c.pgconn);
    std::printf("[ok] connected: db='%s' user='%s'\n", db.dbname(), db.username());

    int zone_id = lookupZoneId(db, c.camera_id, c.channel);
    std::printf("[result] lookupZoneId(%d,%d) = %d\n", c.camera_id, c.channel, zone_id);

    if (zone_id == 1) {
      std::printf("[PASS] zone_id == 1 (zone_A) — DB 실연결·조회 정상\n");
      return 0;
    }
    std::fprintf(stderr, "[FAIL] expected 1 (zone_A), got %d\n", zone_id);
    return 1;
  } catch (const pqxx::sql_error& e) {
    std::fprintf(stderr, "[FAIL] SQL: %s\n       query: %s\n", e.what(), e.query().c_str());
    return 2;
  } catch (const std::exception& e) {
    // 접속 실패(비번/pg_hba/네트워크)도 여기로: broken_connection 등
    std::fprintf(stderr, "[FAIL] %s\n", e.what());
    return 2;
  }
}