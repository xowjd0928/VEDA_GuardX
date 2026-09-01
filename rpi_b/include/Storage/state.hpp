#pragma once
// state — 재시작 안전 영속 (증분 커서 · 형상 해시 · config version)
#include <cstdint>
#include <string>

struct State {
  std::string det_cursor;          // 마지막 수신 detection ts (ISO8601)
  std::string face_cursor;         // v15: 마지막 수신 face ts (ISO8601)
  uint64_t shape_hash = 0;         // 최근 형상(CGI 본문) FNV-1a 해시
  int config_version = 1;
  std::string path;

  void load(const std::string& p);
  void save() const;
};

uint64_t fnv1a(const std::string& s);

