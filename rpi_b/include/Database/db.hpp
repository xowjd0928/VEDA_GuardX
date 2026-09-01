#pragma once
// db — Postgres 헬퍼
#include <pqxx/pqxx>

// (camera_id, channel) → zone_id. 시작 시 1회 조회 후 캐시.
int lookupZoneId(pqxx::connection& db, int camera_id, int channel);

