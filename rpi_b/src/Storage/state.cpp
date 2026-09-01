#include "Storage/state.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

void State::load(const std::string& p) {
  path = p;
  std::ifstream f(p);
  if (!f) return;
  try {
    json j; f >> j;
    det_cursor     = j.value("det_cursor", "");
    face_cursor    = j.value("face_cursor", "");
    shape_hash     = j.value("shape_hash", (uint64_t)0);
    config_version = j.value("config_version", 1);
  } catch (...) { /* 손상 시 기본값 유지 */ }
}

void State::save() const {
  // v16: 부모 디렉터리 보장 + 실패를 소리내기. state/ 미존재로 save가
  // 조용히 무위가 되어 커서·epoch 영속이 통째로 죽어 있던 실사고
  // (2026-07-28 발견: 재시작마다 링 재수신 → 중복 detections, cfg 베이스라인
  // 리셋). 영속 실패는 치명이라 반드시 로그로 드러낸다.
  std::error_code ec;
  const auto dir = std::filesystem::path(path).parent_path();
  if (!dir.empty()) std::filesystem::create_directories(dir, ec);
  json j{{"det_cursor", det_cursor}, {"face_cursor", face_cursor},
         {"shape_hash", shape_hash}, {"config_version", config_version}};
  std::ofstream f(path);
  f << j.dump(2);
  f.flush();
  if (!f) std::cerr << "[state] save 실패: " << path << "\n";
}

// FNV-1a — 플랫폼·재시작 간 안정적인 해시 (형상 변경 감지용)
uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}

