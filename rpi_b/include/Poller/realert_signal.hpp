#pragma once
// realert_signal — 임계 변경 직후 즉시 재판정 (카메라 폴러 쪽 절반).
//
// 짝: MqttDb/task_vms.cpp 의 handleSetZone 이 Mqtt/topics.hpp 의
// TOPIC_ZONE_CHANGED 로 신호를 쏜다. 이 파일은 그걸 받아 큐에 넣는다.
//
// 왜 task_vms 에서 떼어냈나 (v2): 큐를 소비하는 것은 pollAlert 이고 그건
// 카메라 폴러의 일이다. task_vms 에 남겨두면 guardx_poller 가 VMS 조회
// 서비스 전체(mqttdb_core)를 링크해야 해서, 안 쓰는 코드가 폴러 바이너리에
// 딸려오고 CMake 의존 그래프도 거짓말을 하게 된다.
#include <vector>

/**
 * @brief 임계 변경 신호 구독 — runPoller INIT에서 1회 (mqttInit 이후)
 *
 * 신호가 오면 zone_id 를 큐에 넣는다. 폴러가 꺼져 있는 동안 온 신호는
 * 그냥 사라지는데, 그때는 재판정할 대상 자체가 없으므로 무해하다.
 */
void subscribeZoneChangedSignal();

/**
 * @brief 재판정할 존 목록을 꺼낸다 (꺼내면 비워진다) — 메인 루프 1초 틱
 *
 * 없으면 다음 alert 틱(기본 10초)까지 옛 단계가 남는다
 * ("임계 바꿨는데 반응 없음"). pollAlert 는 메인 루프 커넥션을 쓰므로
 * 구독 콜백(네트워크 스레드)에서 직접 부르지 않고 이렇게 넘긴다.
 */
std::vector<int> takeRealertZones();
