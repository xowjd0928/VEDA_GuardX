// task_track_display.cpp — VMS 지목 -> DB 최신 좌표 -> LED 매트릭스 발행.
// 설계 배경과 페이로드 규격은 MqttDb/task_track_display.hpp 참조.
#include "MqttDb/task_track_display.hpp"
#include "Mqtt/mqtt_pub.hpp"
#include "MqttDb/track_plan.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>

namespace {

const char* TOPIC_TRACK_CMD = "guardx/db/rpib/cmd/track_display";

// RPi C 수신 토픽. rpi_c/rpic_app/app/include/matrix_link.h 의
// GUARDX_TOPIC_MATRIX_TRACK 과 같은 문자열이어야 한다. 이 파일은 rpi_c 헤더를
// include 하지 않으므로(빌드 의존 최소화) 그대로 옮겨 적는다 — 액추에이터
// 토픽(TOPIC_ACTUATOR_RPIC)과 같은 관례다. 바뀌면 양쪽을 함께 고칠 것.
const char* TOPIC_TRACK_RPIC = "guardx/display/rpic/track";

// 공통 좌표 규약은 MqttDb/track_plan.hpp 에 있다 - 프레임 크기, 사분면 배치,
// 상태 비트, 예측점 생략 거리. 하드웨어 없이 검증하려고 분리했다
// (test/track_plan_test.cpp). 값과 규칙은 그대로다.
using namespace guardx::track_plan;

// 마지막 START 로부터 이 시간 안에 다음 것이 안 오면 무장 해제.
// VMS 재전송 주기가 5초라 3회 유실까지 견딘다.
const long long ARM_TTL_MS = 15000;

// 발행 주기(초). 카메라 폴러의 det_interval_s(기본 2초)와 맞춘다.
const long PUBLISH_INTERVAL_S = 2;

// 이보다 낡은 행은 "지금 화면에 없다"로 본다. 폴러 주기(2초)의 몇 배를
// 두어 한두 틱 밀린 것으로 점이 깜빡이지 않게 한다.
const int FRESH_WINDOW_S = 10;

/** @brief 지목된 대상. VMS TrackId 와 같은 분기 규칙을 따른다 */
struct Target {
  long long global_id = 0;   // 0 = 미배정 -> (channel, object_id) 로 찾는다
  int channel = -1;          // 화면 채널(0-based) = detections.raw_channel
  int object_id = -1;
  std::string label;         // 로그용 ("P-6461")
};

std::mutex g_mtx;
bool g_armed = false;
Target g_target;
long long g_last_cmd_ms = 0;
// STOP 을 받았을 때 한 번 쏠 "지우기". 발행은 메인 루프에서만 하므로
// 콜백은 깃발만 세운다.
bool g_pending_off = false;
long g_seq = 0;
// 좌표를 잡고 있는 중인가. 대상이 사라졌다 돌아올 때만 로그를 한 줄 남기기
// 위한 것이다. 메인 루프와 콜백 스레드가 함께 만지므로 atomic 이다 —
// g_mtx 로 묶지 않은 이유는 이 값 하나 때문에 조회 구간까지 락을 잡게 되기
// 때문이다(락 안에서 DB를 만지면 콜백이 그 시간만큼 멎는다).
std::atomic<bool> g_had_fix{false};
// 메인 루프 전용(발행 주기 솎기).
long g_tick = 0;

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/** @brief JSON 문자열 값 하나 뽑기 (평면 payload 전용) */
std::string jget(const std::string& s, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  size_t p = s.find(pat);
  if (p == std::string::npos) return {};
  p = s.find(':', p + pat.size());
  if (p == std::string::npos) return {};
  p = s.find('"', p);
  if (p == std::string::npos) return {};
  const size_t e = s.find('"', p + 1);
  if (e == std::string::npos) return {};
  return s.substr(p + 1, e - p - 1);
}

/** @brief JSON 숫자 값 하나 뽑기. 없으면 def */
long long jnum(const std::string& s, const std::string& key, long long def) {
  const std::string pat = "\"" + key + "\"";
  size_t p = s.find(pat);
  if (p == std::string::npos) return def;
  p = s.find(':', p + pat.size());
  if (p == std::string::npos) return def;
  ++p;
  try {
    return std::stoll(s.substr(p, 32));
  } catch (const std::exception&) {
    return def;
  }
}

void publishFrame(const Frame& f) {
  const std::string payload = buildPayload(nowMs(), g_seq++, f);
  // retain 하지 않는다. 좌표는 상태가 아니라 흐름이라, retained 로 두면
  // RPi C 가 재접속할 때마다 옛 자리에 점이 되살아난다.
  mqttPublishEvent(TOPIC_TRACK_RPIC, payload.c_str());
}

void handleTrackDisplay(const std::string& req) {
  const std::string action = jget(req, "action");

  std::lock_guard<std::mutex> lk(g_mtx);

  if (action == "STOP") {
    if (g_armed)
      std::cout << "[track] 송출 중지 — " << g_target.label << "\n";
    g_armed = false;
    // 사용자가 직접 끈 것이므로 즉시 지운다. STM32 의 10초 자동 소멸을
    // 기다리면 버튼을 껐는데 점이 남아 있는 시간이 생긴다.
    g_pending_off = true;
    g_had_fix = false;
    return;
  }

  if (action != "START") {
    std::cerr << "[track] 알 수 없는 action: " << req << "\n";
    return;
  }

  Target t;
  t.global_id = jnum(req, "global_id", 0);
  t.channel   = (int)jnum(req, "channel", -1);
  t.object_id = (int)jnum(req, "object_id", -1);
  t.label     = jget(req, "label");

  if (t.global_id == 0 &&
      (t.channel < 0 || t.channel >= CHANNEL_COUNT || t.object_id < 0)) {
    std::cerr << "[track] 대상 식별 불가 — global_id 도 (channel,object_id) 도"
                 " 없음: " << req << "\n";
    return;
  }

  const bool changed = !g_armed || t.global_id != g_target.global_id ||
                       t.channel != g_target.channel ||
                       t.object_id != g_target.object_id;

  g_target = t;
  g_last_cmd_ms = nowMs();
  g_armed = true;
  // 껐다 바로 켠 경우 밀린 "지우기"가 새 대상 뒤에 나가면 방금 켠 점이
  // 한 주기 동안 사라진다. START 가 이긴다.
  g_pending_off = false;
  if (changed) {
    g_had_fix = false;
    std::cout << "[track] 송출 시작 — " << (t.label.empty() ? "(무명)" : t.label)
              << " (global_id=" << t.global_id << ", ch=" << t.channel
              << ", object_id=" << t.object_id << ")\n";
  }
}

/** @brief 대상의 최신 행 한 건. 못 찾으면 false */
bool queryLatest(pqxx::connection& db, const Target& t, Frame& out) {
  pqxx::work tx(db);

  // 최신 한 건만 본다. ts 는 파티션 키라 시간 창을 걸어야 옛 파티션까지
  // 훑지 않는다. category=1(Human) 로 좁히는 것은 얼굴/머리 박스가 같은
  // object_id 공간을 쓰지 않더라도 실수로 섞이는 것을 막기 위함이다.
  const pqxx::result r = tx.exec(
      "SELECT ST_X(geom), ST_Y(geom),"
      "       ST_X(predicted_geom), ST_Y(predicted_geom),"
      "       raw_channel, COALESCE(direction, 'UNKNOWN')"
      "  FROM detections"
      " WHERE ts > now() - make_interval(secs => $4::double precision)"
      "   AND category = 1"
      "   AND ( ($1::bigint <> 0 AND global_id = $1::bigint)"
      "      OR ($1::bigint  = 0 AND raw_channel = $2 AND object_id = $3) )"
      " ORDER BY ts DESC"
      " LIMIT 1",
      pqxx::params{t.global_id, t.channel, t.object_id, FRESH_WINDOW_S});
  tx.commit();

  if (r.empty()) return false;

  const pqxx::row& row = r[0];
  if (row[0].is_null() || row[1].is_null() || row[4].is_null()) return false;

  // 채널은 DB 행에서 읽는다. global_id 로 찾은 대상은 카메라를 넘어갔을 수
  // 있어서, VMS 가 지목할 때의 채널이 지금의 채널이 아니다.
  const int channel = row[4].as<int>();
  return buildFrame(channel, row[0].as<double>(), row[1].as<double>(),
                    !row[2].is_null() && !row[3].is_null(),
                    row[2].is_null() ? 0.0 : row[2].as<double>(),
                    row[3].is_null() ? 0.0 : row[3].as<double>(),
                    row[5].as<std::string>(), out);
}

}  // namespace

void startTrackDisplayService(const Config& cfg) {
  (void)cfg;   // DB 도 카메라도 만지지 않는다 — 구독만 건다
  mqttSubscribe(TOPIC_TRACK_CMD, handleTrackDisplay);
  std::cout << "[track] 추적 송출 서비스 시작 — " << TOPIC_TRACK_CMD << " -> "
            << TOPIC_TRACK_RPIC << "\n";
}

void publishTrackDisplay(pqxx::connection& db) {
  Target target;
  bool send_off = false;

  {
    std::lock_guard<std::mutex> lk(g_mtx);

    if (g_pending_off) {
      g_pending_off = false;
      send_off = true;
    }

    // VMS 가 사라진 경우. 여기서는 지우기를 쏘지 않는다 — STM32 가 10초
    // 뒤 스스로 점을 내리므로, 마지막 위치가 잠시 남았다가 사라진다.
    // 브로커가 잠깐 끊긴 것과 사용자가 끈 것을 같게 취급하지 않기 위함이다.
    if (g_armed && nowMs() - g_last_cmd_ms > ARM_TTL_MS) {
      std::cout << "[track] VMS 신호 끊김 — 무장 해제 (LED 는 10초 뒤 자동 소멸)\n";
      g_armed = false;
      g_had_fix = false;
    }

    if (!g_armed) {
      if (!send_off) return;
    } else {
      target = g_target;
    }
  }

  if (send_off) {
    publishFrame(clearFrame());
    return;
  }

  // 무장 중에만 여기 온다. 매초 호출되지만 조회는 폴러 주기에 맞춰 솎는다.
  if (g_tick++ % PUBLISH_INTERVAL_S != 0) return;

  Frame frame;
  bool found = false;

  try {
    found = queryLatest(db, target, frame);
  } catch (const std::exception& e) {
    std::cerr << "[track] 좌표 조회 실패: " << e.what() << "\n";
    return;
  }

  if (!found) {
    // 대상이 화면에서 사라졌다. 발행을 멈추면 STM32 가 마지막 위치를 10초
    // 유지하다 지운다 — 잠깐 가려졌다 다시 잡히는 흔한 경우에 점이
    // 깜빡이지 않는다.
    if (g_had_fix) {
      std::cout << "[track] " << target.label << " 놓침 — 발행 중단"
                   " (LED 는 마지막 위치 유지 후 10초 소멸)\n";
      g_had_fix = false;
    }
    return;
  }

  if (!g_had_fix) {
    std::cout << "[track] " << target.label << " 좌표 확보 — 송출 중\n";
    g_had_fix = true;
  }

  publishFrame(frame);
}
