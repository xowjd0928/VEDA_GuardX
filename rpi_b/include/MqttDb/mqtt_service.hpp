#pragma once
// mqtt_service — VMS ↔ DB 통로 (guardx_mqttd 본체).
//
// 왜 프로세스가 따로인가: 이 일은 카메라와 아무 상관이 없다. 과거 데이터
// 조회(CROWD 날짜·히트맵), 정원 편집(ZONE SETTINGS), 열린 incident 복원은
// 카메라가 꺼져 있어도 되어야 하는 기능이다. guardx_poller 에 얹혀 있던
// 동안에는 카메라 사정(존 매핑 없음·비밀번호 없음·DB 접속 실패)으로
// 폴러가 죽으면 VMS 화면 절반이 같이 멎었다.
//
// 이 프로세스에는 카메라라는 단어가 한 번도 나오지 않는다 — 그게 분리의
// 성공 기준이다. 토픽·페이로드 규약은 그대로라 VMS는 수정이 없다.
// 전체 설계: vms/docs/DB_LINK_AND_MQTT_MIGRATION.md, MQTT_SERVICE_SPLIT.md

/**
 * @brief 조회 서비스 본체 — 구독 등록 후 주기 상태 발행 루프
 *
 * 정상 동작 중에는 반환하지 않는다 (systemd Type=simple 전제).
 * 반환값은 mqtt_main.cpp 의 종료 코드가 된다.
 */
int runMqttService();
