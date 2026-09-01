// mqtt_service.cpp — guardx_mqttd 본체 (runMqttService).
// mqtt_main.cpp 는 이 함수 호출만 한다 (main.cpp ↔ poller_main.cpp 와 같은 꼴).
#include <pqxx/pqxx>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "Config/config.hpp"
#include "Mqtt/mqtt_pub.hpp"
#include "MqttDb/mqtt_service.hpp"
#include "MqttDb/task_track_display.hpp"
#include "MqttDb/task_vms.hpp"

namespace {

// 브로커 client id. guardx_poller("rpib-poller")와 반드시 달라야 한다 —
// 같은 id로 두 연결이 붙으면 브로커가 서로를 끊어내며 무한 재접속에 빠진다.
const char* CLIENT_ID = "rpib-mqttd";

// DB가 죽어 있을 때 재연결 시도 간격. 폴러의 틱과 달리 여기서는 급할 게
// 없다 — 조회 요청은 요청 시점에 각자 재연결을 시도하므로(ensureQdb),
// 이 루프는 상태 발행만 책임진다.
const int RETRY_INTERVAL_S = 5;

}  // namespace

int runMqttService() {
  Config cfg = Config::load();

  // ⚠ 롤 분리가 실제로 걸렸는지를 기동 시 한 번 말한다. 폴백 자체는 의도한
  // 것이지만(config.env 는 sync_to_rpi.sh 에서 제외된다 — Config 주석 참조),
  // 조용히 넘어가면 "분리한 줄 알았는데 아니었다"가 된다. 이 작업의 유일한
  // 실패 모드가 그것이라 침묵하지 않는다.
  if (cfg.pgconn_ro_is_fallback) {
    std::cerr << "[mqttd] ⚠ PGCONN_RO 미설정 — 조회도 쓰기 계정으로 붙는다"
                 " (롤 분리 무효). config.env 에 PGCONN_RO 를 추가할 것\n";
  }

  // 브로커가 아직 안 떠 있어도 계속 간다 — loop_start 가 재접속을 맡는다.
  mqttInit(cfg, CLIENT_ID);

  // 구독을 먼저 건다. DB 상태와 무관하게 등록해 두어야 요청이 왔을 때
  // 침묵(VMS 입장에서 타임아웃) 대신 실패 이유라도 답할 수 있다.
  startVmsQueryService(cfg);

  // VMS 추적 지목 -> LED 매트릭스 좌표 송출. 구독만 걸고 조회는 아래 루프에서
  // 한다(콜백 스레드가 DB를 만지지 않는 것이 이 모듈의 설계다).
  startTrackDisplayService(cfg);

  std::cout << "[mqttd] 시작 — 상태 발행 주기 " << cfg.cfg_interval_s
            << "s, 센서 발행 주기 " << cfg.sensor_interval_s << "s\n";

  // 상태 발행용 커넥션. 조회 핸들러의 커넥션과 별개다 — 핸들러는 mosquitto
  // 네트워크 스레드에서 불리고 pqxx::connection 은 스레드 안전하지 않다.
  //
  // **조회 계정(pgconn_ro)으로 연다** (08-11). 이 루프가 하는 일은 전부 읽기다 —
  // publishVmsState·publishSensorState·publishTrackDisplay 어디에도
  // INSERT/UPDATE 가 없다(전수 확인). 쓰기는 cmd/* 핸들러의 rwDb() 뿐이다.
  std::unique_ptr<pqxx::connection> db;
  long tick = 0;
  long next_retry = 0;

  while (true) {
    if (!db || !db->is_open()) {
      // 붙을 때까지 조용히 재시도. 여기서 예외를 흘리면 프로세스가 죽는데,
      // 이 서비스는 "카메라가 죽어도 사는 것"이 존재 이유라 DB가 잠깐
      // 내려간 것으로 같이 죽으면 안 된다.
      if (tick >= next_retry) {
        try {
          db = std::make_unique<pqxx::connection>(cfg.pgconn_ro);
          // 계정을 함께 찍는다 — "연결은 됐는데 permission denied" 일 때
          // 범인이 대개 접속 계정이다. DB 이름만 찍으면 그걸 못 본다
          // (실사고 2026-08-03: fire_threshold 권한 오진).
          std::cout << "[mqttd] DB 연결됨 (" << db->dbname()
                    << ", user=" << db->username() << ")\n";
        } catch (const std::exception& e) {
          db.reset();
          next_retry = tick + RETRY_INTERVAL_S;
          std::cerr << "[mqttd] DB 연결 실패: " << e.what() << " — "
                    << RETRY_INTERVAL_S << "초 후 재시도\n";
        }
      }
    } else {
      // 센서는 RPi A가 1Hz로 보내므로 별도의 빠른 틱(기본 1초)으로 뗀다 —
      // zones/dates와 같은 30초 틱에 묶으면 VMS 실시간 그래프가 30초에
      // 한 번만 갱신된다(2026-08-04 실측).
      if (tick % cfg.sensor_interval_s == 0)
        publishSensorState(*db);

      // 정원·날짜목록·열린 incident 는 거의 안 바뀌는 값이라 이 주기로 충분.
      // publishVmsState 는 내부에서 예외를 삼키므로, 끊긴 커넥션은 여기서
      // 감지해 버린다 (다음 틱에 위 분기가 다시 연다).
      if (tick % cfg.cfg_interval_s == 0)
        publishVmsState(*db);

      // 추적 좌표는 사람이 움직이는 속도로 갱신돼야 하므로 상태 토픽의
      // 30초 틱에 묶을 수 없다. 매초 부르되 실제 발행 주기는 내부에서
      // 폴러 적재 주기에 맞춰 솎는다.
      publishTrackDisplay(*db);

      if (!db->is_open()) {
        std::cerr << "[mqttd] DB 연결 끊김 — 재연결 대기\n";
        db.reset();
      }
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
    ++tick;
  }

  return 0;
}
