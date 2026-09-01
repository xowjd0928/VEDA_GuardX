#include "Mqtt/mqtt_pub.hpp"
#include <mosquitto.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>

// envelope 규약 (§3): node_id(발행자)·timestamp(epoch ms)·seq(프로세스 단조,
// 재시작 리셋 허용). client id: 규약은 노드ID=클라이언트ID지만 "rpib"는
// transmission layer와 충돌 가능 → "rpib-poller" (client id는 연결당 유일).

namespace {
mosquitto* g_m = nullptr;
long g_seq = 0;

// 구독 핸들러. mosquitto 네트워크 스레드가 읽고 메인 스레드가 등록하므로
// 뮤텍스로 보호한다 (등록은 INIT에서 끝나지만 규칙을 지켜둔다).
std::mutex g_sub_mtx;
std::map<std::string, std::function<void(const std::string&)>> g_subs;

int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void onMessage(mosquitto*, void*, const mosquitto_message* msg) {
  if (!msg || !msg->topic) return;
  std::function<void(const std::string&)> h;
  {
    std::lock_guard<std::mutex> lk(g_sub_mtx);
    auto it = g_subs.find(msg->topic);
    if (it == g_subs.end()) return;
    h = it->second;
  }
  const std::string payload(static_cast<const char*>(msg->payload),
                            msg->payloadlen);
  try {
    h(payload);
  } catch (const std::exception& e) {
    // 핸들러가 던지면 네트워크 스레드가 죽는다 — 폴러 전체가 조용히 멎으므로
    // 반드시 여기서 막는다.
    std::cerr << "[mqtt] 핸들러 예외 (" << msg->topic << "): " << e.what() << "\n";
  }
}

// 재접속 시 구독이 사라지므로 다시 건다 (clean_session=true라 브로커가
// 세션을 기억하지 않는다).
void onConnect(mosquitto* m, void*, int rc) {
  if (rc != 0) return;
  std::lock_guard<std::mutex> lk(g_sub_mtx);
  for (const auto& kv : g_subs) {
    if (mosquitto_subscribe(m, nullptr, kv.first.c_str(), 1) == MOSQ_ERR_SUCCESS)
      std::cout << "[mqtt] 구독 " << kv.first << " (qos1)\n";
  }
}

void publish(const std::string& topic, const std::string& payload, bool retain) {
  if (!g_m) return;
  const int rc = mosquitto_publish(g_m, nullptr, topic.c_str(),
                                   (int)payload.size(), payload.data(),
                                   /*qos=*/1, retain);
  if (rc != MOSQ_ERR_SUCCESS) {
    std::cerr << "[mqtt] publish 실패 rc=" << rc << " (" << topic
              << ") — 재접속 시도\n";
    mosquitto_reconnect_async(g_m);
  }
}
}  // namespace

bool mqttInit(const Config& cfg, const std::string& client_id) {
  mosquitto_lib_init();
  g_m = mosquitto_new(client_id.c_str(), true, nullptr);
  if (!g_m) { std::cerr << "[mqtt] client 생성 실패\n"; return false; }
  mosquitto_connect_callback_set(g_m, onConnect);
  mosquitto_message_callback_set(g_m, onMessage);
  const int rc = mosquitto_connect(g_m, cfg.mqtt_host.c_str(), cfg.mqtt_port, 60);
  if (rc != MOSQ_ERR_SUCCESS) {
    // 브로커 미가동 등 — 폴러는 계속 간다 (경보는 DB에 남고, 발행 시 재접속 시도)
    std::cerr << "[mqtt] connect 실패 (" << cfg.mqtt_host << ":" << cfg.mqtt_port
              << ") rc=" << rc << " — 발행 시 재시도\n";
  }
  mosquitto_loop_start(g_m);   // 네트워크 스레드 — 끊김 시 자동 재접속
  std::cout << "[mqtt] client_id=" << client_id << " (" << cfg.mqtt_host << ":"
            << cfg.mqtt_port << ")\n";
  return rc == MOSQ_ERR_SUCCESS;
}

void mqttPublishRetained(const std::string& topic, const std::string& payload) {
  publish(topic, payload, /*retain=*/true);
}

void mqttPublishReply(const std::string& topic, const std::string& payload) {
  publish(topic, payload, /*retain=*/false);
}

void mqttPublishEvent(const std::string& topic, const std::string& payload) {
  publish(topic, payload, /*retain=*/false);
}

void mqttSubscribe(const std::string& topic,
                   std::function<void(const std::string&)> handler) {
  {
    std::lock_guard<std::mutex> lk(g_sub_mtx);
    g_subs[topic] = std::move(handler);
  }
  // 이미 붙어 있으면 즉시 구독. 아직이면 onConnect가 일괄 처리한다.
  if (g_m) mosquitto_subscribe(g_m, nullptr, topic.c_str(), 1);
}

void mqttPublishAlert(int zone_id, int channel, long long incident_id,
                      const char* severity, int count, int capacity,
                      const char* source) {
  if (!g_m) return;
  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"node_id\":\"rpib\",\"timestamp\":%lld,\"seq\":%ld,"
           "\"event\":\"congestion\",\"zone_id\":%d,\"channel\":%d,"
           "\"incident_id\":%lld,\"severity\":\"%s\","
           "\"count\":%d,\"capacity\":%d,\"source\":\"%s\"}",
           (long long)nowMs(), g_seq++, zone_id, channel,
           incident_id, severity, count, capacity, source);
  const int rc = mosquitto_publish(g_m, nullptr, "guardx/alert/rpib",
                                   (int)strlen(payload), payload,
                                   /*qos=*/1, /*retain=*/false);
  if (rc != MOSQ_ERR_SUCCESS) {
    std::cerr << "[mqtt] publish 실패 rc=" << rc << " (zone " << zone_id
              << " " << severity << ") — 재접속 시도\n";
    mosquitto_reconnect_async(g_m);
  } else {
    std::cout << "[mqtt] alert/rpib <- zone " << zone_id << " " << severity
              << " (" << count << "/" << capacity << ")\n";
  }
}
