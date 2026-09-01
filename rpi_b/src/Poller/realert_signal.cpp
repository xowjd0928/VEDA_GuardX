#include "Poller/realert_signal.hpp"

#include "Mqtt/mqtt_pub.hpp"
#include "Mqtt/topics.hpp"

#include <iostream>
#include <mutex>
#include <string>

namespace {

// 구독 콜백(mosquitto 네트워크 스레드)이 넣고 메인 루프가 꺼낸다.
std::mutex g_mtx;
std::vector<int> g_zones;

/** @brief 평면 JSON 에서 숫자 값 하나 (없으면 def) — payload가 한 필드뿐이라 충분 */
int jnum(const std::string& s, const std::string& key, int def) {
  const std::string pat = "\"" + key + "\"";
  size_t p = s.find(pat);
  if (p == std::string::npos) return def;
  p = s.find(':', p + pat.size());
  if (p == std::string::npos) return def;
  try {
    return std::stoi(s.substr(p + 1));
  } catch (...) {
    return def;
  }
}

}  // namespace

void subscribeZoneChangedSignal() {
  mqttSubscribe(TOPIC_ZONE_CHANGED, [](const std::string& payload) {
    const int zone_id = jnum(payload, "zone_id", -1);
    if (zone_id <= 0) return;
    {
      std::lock_guard<std::mutex> lk(g_mtx);
      g_zones.push_back(zone_id);
    }
    std::cout << "[realert] 임계 변경 신호 — zone " << zone_id << " 즉시 재판정\n";
  });
  std::cout << "[realert] 신호 수신 — " << TOPIC_ZONE_CHANGED << "\n";
}

std::vector<int> takeRealertZones() {
  std::lock_guard<std::mutex> lk(g_mtx);
  std::vector<int> out;
  out.swap(g_zones);
  return out;
}
