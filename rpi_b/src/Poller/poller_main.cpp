// poller_main.cpp — 폴러 본체 (runPoller): 초기화 + 1초 틱 루프.
// main.cpp는 runPoller() 호출만 한다. (구명: poller_integration_example.cpp —
// v16 정리 때 실체에 맞게 개명)
//
// VEDA-155: VMS 조회 서비스는 여기서 빠져 guardx_mqttd 로 갔다.
// 이 프로세스는 "카메라 → DB 쓰기 + 경보 판정"만 한다. 카메라 사정으로
// 죽어도 VMS의 조회·정원 편집은 MqttDb 쪽에서 계속 산다.
// v2에서 모듈까지 갈라져, 이 파일은 이제 MqttDb 를 include 하지 않는다.
#include <curl/curl.h>
#include <pqxx/pqxx>
#include <chrono>
#include <iostream>
#include <thread>

#include "Config/config.hpp"
#include "Config/credentials.hpp"
#include "Poller/time_util.hpp"
#include "Database/db.hpp"
#include "Storage/state.hpp"
#include "Mqtt/mqtt_pub.hpp"
#include "Poller/task_alert.hpp"
#include "Poller/task_analytics_trajectories.hpp"
#include "Poller/task_config_sync.hpp"
#include "Poller/task_detections.hpp"
#include "Poller/task_fan_level.hpp"
#include "Poller/task_faces.hpp"
#include "Poller/task_flow.hpp"
#include "Poller/task_occupancy.hpp"
#include "Poller/task_prediction.hpp"
#include "Poller/realert_signal.hpp"
#include "Poller/task_tracks.hpp"

int runPoller() {
  // ── [INIT] ──
  Config cfg = Config::load();
  curl_global_init(CURL_GLOBAL_DEFAULT);
  // 실패해도 폴러는 계속 — 경보는 DB에 남고 발행 시 재시도.
  // client_id는 guardx_mqttd 와 반드시 달라야 한다 (같으면 브로커가 서로 끊는다).
  mqttInit(cfg, "rpib-poller");
  State st; st.load(cfg.state_path);
  // v16: 미래로 오염된 커서 자가 치유 (실사고 2026-07-28: 미래 ts 레코드가
  // face_cursor를 +20h로 밀어 피드 정지). 이후 오염은 task_detections/faces의
  // 미래 ts 가드가 막지만, 이미 오염된 state는 여기서 지금 시각으로 되돌린다.
  // (되돌린 커서 이전의 링 잔여분은 재수신 안 됨 — 정지 구간 유실은 수용)
  {
    const std::string limit = epochToIso(time(nullptr) + 120);
    const std::string now_iso = epochToIso(time(nullptr));
    if (st.det_cursor > limit) {
      std::cerr << "[state] det_cursor 미래 오염 " << st.det_cursor
                << " -> " << now_iso << " 재설정\n";
      st.det_cursor = now_iso; st.save();
    }
    if (st.face_cursor > limit) {
      std::cerr << "[state] face_cursor 미래 오염 " << st.face_cursor
                << " -> " << now_iso << " 재설정\n";
      st.face_cursor = now_iso; st.save();
    }
  }
  pqxx::connection db(cfg.pgconn);
  if (!db.is_open()) { std::cerr << "DB 연결 실패\n"; return 1; }
  loadCameraCredentials(cfg, db);   // DB(camera_credentials) 우선, 없으면 config.env 유지
  // 비번 검증은 credentials 로드 '이후' — DB에서 오는 경우 config.env엔 없을 수 있음
  if (cfg.pass.empty()) {
    std::cerr << "카메라 비밀번호 없음 (camera_credentials 행도, CAM_PASS도 없음)\n";
    return 1;
  }
  // v14: 채널별 zone 매핑 (CHANNELS 설정 — 기본은 CHANNEL 단일).
  // 존이 없는 채널은 경고 후 제외 — 폴러는 나머지 채널로 계속 간다.
  std::map<int, int> zone_by_ch;
  for (int ch : cfg.channels) {
    const int z = lookupZoneId(db, cfg.camera_id, ch);
    if (z < 0) {
      std::cerr << "zone 없음: camera " << cfg.camera_id << " ch " << ch
                << " — 이 채널 폴링 제외 (zones에 행 추가 후 재시작)\n";
      continue;
    }
    zone_by_ch[ch] = z;
    std::cout << "poller map: ch " << ch << " -> zone " << z << "\n";
  }
  if (zone_by_ch.empty()) { std::cerr << "폴링 가능한 채널 없음\n"; return 1; }

  // VMS 조회 서비스는 guardx_mqttd 담당이다. 여기서는 그쪽이 보내는
  // 임계 변경 신호만 받는다 — 재판정은 이 프로세스의 틱에서만 돌기 때문.
  subscribeZoneChangedSignal();

  // ── [LOOP] 1초 틱 (det 2s / face 10s / occ·alert 10s / pred·flow 60s /
  //    cfg 30s + set_zone 직후 즉시 재판정) ──
  long tick = 0;
  while (true) {
    // 2026-08-03: pollDetections(/juan_application/detections) 중단.
    // pollTracks 가 **같은 `detections` 테이블**에 적재하므로 둘 다 돌리면 같은
    // 사람이 두 행으로 들어가 점유·유량이 2배로 뜬다. VMS 도 tracks 피드로
    // 갈아탔다. 호출만 뺐고 코드는 남겨 둔다 — 되돌리려면 이 줄만 살리면 된다.
    // (juan_application 의 prediction·occupancy·flow·faces 는 계속 쓴다)
    if (tick % cfg.det_interval_s == 0) {
      pollTracks(cfg, db);
    }
    if (cfg.trajectory_interval_s > 0 &&
        tick % cfg.trajectory_interval_s == 0) {
      pollAnalyticsTrajectories(cfg, db);
    }
    if (tick % cfg.face_interval_s == 0) pollFaces(cfg, db, st);
    if (tick % cfg.pred_interval_s == 0) {
      for (const auto& zc : zone_by_ch)
        pollPrediction(cfg, db, zc.second, zc.first);
      pollFlow(cfg, db);   // /events는 전 룰 일괄 — 채널 루프 밖
    }
    // 판정은 예측(60s)에서 분리 (07-31) — 60초에 묶여 있던 것이 "임계를
    // 바꿨는데 반응이 없다"의 원인. 현재 인원과 함께 10초마다 판정한다.
    if (tick % cfg.alert_interval_s == 0) {
      for (const auto& zc : zone_by_ch) {
        const int cur = pollOccupancy(cfg, db, zc.second, zc.first);
        pollAlert(db, zc.second, zc.first, cur);   // 방금 적재분 + 현재 인원으로 판정
      }
      // 존별 판정이 끝난 뒤 가장 나쁜 단계를 한 번만 내보낸다.
      // 존마다 쏘면 마지막 존의 값이 최종처럼 남는다.
      publishFanLevel();
    }
    // 임계 변경 직후 즉시 재판정 (guardx_mqttd 가 보낸 zone_changed 신호를
    // realert_signal 이 큐에 넣어둔 것. 현재 인원은 모름(-1) —
    // 분 중앙값·예측만으로 판정)
    for (int z : takeRealertZones()) {
      for (const auto& zc : zone_by_ch)
        if (zc.second == z) { pollAlert(db, z, zc.first, -1); break; }
    }
    // 정원·날짜목록 발행(publishVmsState)은 guardx_mqttd 로 옮겼다.
    // 같은 토픽을 두 프로세스가 쏘면 어느 쪽 값이 최종인지 알 수 없다.
    if (tick % cfg.cfg_interval_s  == 0) syncConfig(cfg, db, st, zone_by_ch);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ++tick;
  }
  curl_global_cleanup();
  return 0;
}
// 기존 main이 디바이스·재난 처리를 같이 하면 스레드로:
//   std::thread poller(runPoller); poller.detach();
