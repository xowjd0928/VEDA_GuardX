#pragma once
// mqtt_pub — 혼잡 경보 "상황" 이벤트 발행 (액추에이터 결정은 transmission
// layer 소관 — 여기는 무슨 일이 일어났는지만 알린다).
// 토픽 guardx/alert/rpib (잠정 — 규약 문서에 alert 토픽 미존재, 팀 합의 필요),
// QoS 1: (incident_id, severity)로 중복 제거 가능, 유실은 불가.
// 브로커는 RPi B localhost. 발행 실패는 로그 후 계속 — 경보의 진실원천은
// DB(incidents/alerts)이고 MQTT는 전달 경로다.
#include "Config/config.hpp"

#include <functional>
#include <string>

/**
 * @brief 브로커 접속 + 네트워크 스레드 시작 — 프로세스 INIT에서 1회
 *
 * client_id는 호출자가 정한다. 브로커는 client id 하나당 연결 하나만
 * 허용하므로, 같은 id로 두 프로세스가 붙으면 서로를 끊어내며 무한
 * 재접속에 빠진다. guardx_poller 와 guardx_mqttd 가 반드시 다른 값을
 * 넘겨야 한다 ("rpib-poller" / "rpib-mqttd").
 */
bool mqttInit(const Config& cfg, const std::string& client_id);

// severity: "warn" | "critical" | "clear"(해제). count/capacity로 비율 재구성 가능.
void mqttPublishAlert(int zone_id, int channel, long long incident_id,
                      const char* severity, int count, int capacity,
                      const char* source);

// ── VMS 조회 대체 경로 (v7) ────────────────────────────────────────────
// VMS가 Postgres에 직접 붙던 것을 걷어내고 폴러가 대신 읽어 발행한다.
// 배경과 토픽 규격: vms/docs/DB_LINK_AND_MQTT_MIGRATION.md

/**
 * @brief 상태성 값을 retained 로 발행
 *
 * alert(retain=false)와 성격이 다르다. alert는 "사건"이라 지나간 걸 뒤늦게
 * 받아봐야 의미가 없지만, 이쪽은 "지금 값이 얼마인가"에 답해야 한다.
 * retained 라야 VMS가 언제 켜지든 구독 즉시 현재 값을 받는다.
 *
 * 값이 바뀔 때만 발행하므로 QoS 0은 쓸 수 없다(다음 주기에 새 값이 온다는
 * 전제가 성립하지 않는다). 덮어쓰기라 중복은 무해하므로 QoS 1.
 */
void mqttPublishRetained(const std::string& topic, const std::string& payload);

/** @brief 조회 응답 발행 (retain=false). 요청자가 준 reply_to 로 보낸다 */
void mqttPublishReply(const std::string& topic, const std::string& payload);

/**
 * @brief 노드 간 내부 신호 발행 (retain=false)
 *
 * guardx_mqttd -> guardx_poller 처럼 프로세스가 갈린 뒤 메모리로 넘기지
 * 못하게 된 신호를 브로커를 통해 전달한다. retain 하지 않는다 — 지나간
 * 신호를 재기동 때 다시 실행하면 안 되는 성격이다.
 */
void mqttPublishEvent(const std::string& topic, const std::string& payload);

/**
 * @brief 토픽 구독 + 메시지 도착 시 호출될 핸들러 등록
 *
 * mqttInit이 loop_start를 걸어두므로 콜백은 libmosquitto 네트워크 스레드에서
 * 불린다. 핸들러 안에서 DB를 쓰려면 폴러 메인 루프의 커넥션과 겹치지 않게
 * 별도 커넥션을 쓸 것 — pqxx::connection은 스레드 안전하지 않다.
 */
void mqttSubscribe(const std::string& topic,
                   std::function<void(const std::string& payload)> handler);
