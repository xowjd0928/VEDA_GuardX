#pragma once
// task_vms — VMS가 Postgres에 직접 붙던 조회를 RPi B가 대신 수행한다.
//
// 배경: VMS는 운영자 PC에서 돌기 때문에 DB에 직접 붙으면 PC마다 Postgres
// 계정이 배포되고, Postgres를 외부에 열어둬야 한다(schema.sql의 "DB는 RPi B
// localhost 전용" 전제가 깨진다). 조회 주체를 RPi B로 옮기면 자격이 여기에만
// 남는다. 전체 설계는 vms/docs/DB_LINK_AND_MQTT_MIGRATION.md 참조.
//
// 두 종류를 다룬다:
//   A. 상태 — 사용자가 뭘 고르든 답이 같다. 값이 바뀔 때만 retained 발행.
//              guardx/db/rpib/zones, guardx/db/rpib/dates
//   B. 질의 — 사용자 선택(날짜)에 따라 답이 달라진다. 요청-응답.
//              guardx/db/rpib/query/heatday -> {reply_to}
//
// v2(VEDA-155): 카메라 폴러에서 떨어져 나와 guardx_mqttd 전용 모듈이 됐다.
// 이 모듈은 카메라를 모른다 — http_client 도 State 도 참조하지 않는다.
#include <pqxx/pqxx>

#include "Config/config.hpp"

/**
 * @brief 상태 토픽 발행 — guardx_mqttd 틱에서 주기 호출 (cfg_interval_s 권장)
 *
 * 매번 조회하되 *값이 바뀌었을 때만* 발행한다. 같은 값을 계속 쏘면 브로커
 * 로그만 지저분해지고 구독자도 얻는 게 없다.
 *
 * 호출자의 커넥션을 그대로 쓴다 (단일 스레드).
 *
 * ⚠ 발행 주체는 한 프로세스여야 한다. 같은 retained 토픽을 둘이 쏘면 어느
 *   쪽 값이 최종인지 알 수 없고, 한쪽 배포가 밀리는 순간 옛 값이 새 값을
 *   덮어쓴다. guardx_mqttd 전용이다.
 */
void publishVmsState(pqxx::connection& db);

/**
 * @brief 센서 상태 토픽 발행 — guardx_mqttd 틱에서 주기 호출 (sensor_interval_s 권장)
 *
 * publishVmsState()와 분리한 이유: zones/dates는 거의 안 바뀌어 30초
 * (cfg_interval_s)로 충분하지만, RPi A 센서는 1Hz라 같은 틱에 묶으면 VMS의
 * 실시간 그래프가 30초에 한 번만 갱신된다 — 독립된 빠른 틱이 필요하다.
 */
void publishSensorState(pqxx::connection& db);

/**
 * @brief 조회 서비스 시작 — guardx_mqttd INIT에서 1회
 *
 * query/heatday·occseries·incidents 와 cmd/set_zone 을 구독한다. 콜백이
 * libmosquitto 네트워크 스레드에서 불리므로, 상태 발행용 커넥션과 공유하지
 * 않도록 여기서 전용 커넥션을 따로 연다 (pqxx::connection은 스레드 안전하지
 * 않다). DB가 안 붙어도 구독은 걸고, 요청이 올 때마다 재연결을 시도한다.
 *
 * v1(VEDA-155)부터 이 함수는 guardx_poller 가 아니라 guardx_mqttd 가
 * 부른다 — 카메라가 꺼져도 VMS 조회는 살아 있어야 하기 때문이다.
 */
void startVmsQueryService(const Config& cfg);

// set_zone 직후 즉시 재판정하는 절반(신호 수신 + 큐)은 카메라 폴러의
// 일이라 v2에서 Poller/realert_signal.hpp 로 옮겼다. 여기서는 handleSetZone
// 이 Mqtt/topics.hpp 의 TOPIC_ZONE_CHANGED 로 신호를 쏘기만 한다.
