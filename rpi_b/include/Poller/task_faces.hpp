#pragma once
// task_faces — GET /faces?since= → faces INSERT (v15)
//   face bestshot 이벤트 피드 (사람 object_id 링크 + 얼굴 bbox + JPEG 경로).
//   /detections와 동일한 커서 의미론 (state.face_cursor, 엄격초과).
//   뒷모습 등으로 얼굴이 안 잡힌 사람은 행이 없다 — 소비자는 object_id
//   LEFT JOIN으로 "얼굴 있으면 박스, 없으면 빈칸"을 얻는다.
#include <pqxx/pqxx>
#include "Config/config.hpp"
#include "Storage/state.hpp"

void pollFaces(const Config& cfg, pqxx::connection& db, State& st);
