// task_fan_level.cpp — 설계 배경은 Poller/task_fan_level.hpp 참조.
#include "Poller/task_fan_level.hpp"
#include "Mqtt/mqtt_pub.hpp"

#include "fan_protocol.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>

namespace {

// zone_id -> 마지막 판정. -1 = 판단 불가(값 없음과 구분).
std::map<int, int> g_level;

int g_sent = -2;            // 아직 한 번도 안 보냄
long long g_sent_ms = 0;

// 값이 그대로여도 이 주기로는 한 번 다시 쏜다. 브로커가 재시작해 retained가
// 날아가면 RPi C는 다음 단계 변화까지 옛 값(또는 정상)으로 남는다.
const long long REFRESH_MS = 30000;

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

int fanLevelFromSeverityName(const std::string& severity) {
  if (severity == "critical") return 2;
  if (severity == "warn") return 1;
  // "clear" 와 빈 문자열은 정상이다. 모르는 값도 정상으로 떨어뜨린다 —
  // 팬을 올리는 쪽으로 추측하면 오작동이 소음으로 드러난다.
  return 0;
}

void reportZoneLevel(int zone_id, int level) {
  g_level[zone_id] = level;
}

void publishFanLevel() {
  int worst = -1;
  for (const auto& kv : g_level)
    if (kv.second > worst) worst = kv.second;

  // 판단 가능한 존이 하나도 없다. 이때는 아무것도 보내지 않는다 —
  // 0(정상)을 보내면 "정원을 아직 안 넣은 현장"에서 팬이 40%로 돌기
  // 시작한다. 값을 모를 때는 조용히 있는 편이 맞다.
  if (worst < 0) return;

  const long long now = nowMs();
  if (worst == g_sent && now - g_sent_ms < REFRESH_MS) return;

  char json[160];
  const int n = std::snprintf(json, sizeof(json),
      "{\"node_id\":\"rpib\",\"timestamp\":%lld,\"level\":%d}", now, worst);
  if (n <= 0) return;

  mqttPublishRetained(GUARDX_FAN_LEVEL_TOPIC, std::string(json, (size_t)n));

  if (worst != g_sent)
    std::cout << "[fan] 혼잡 단계 " << (g_sent < 0 ? 0 : g_sent) << " -> "
              << worst << " 발행\n";
  g_sent = worst;
  g_sent_ms = now;
}
