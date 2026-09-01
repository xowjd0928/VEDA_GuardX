#pragma once
// task_track_display — VMS가 지목한 대상의 최신 좌표를 LED 매트릭스로 보낸다.
//
// 경로:  VMS(TRACKING 패널 [매트릭스 송출] 토글)
//          -> guardx/db/rpib/cmd/track_display   (지목만. 좌표는 안 실린다)
//        여기(guardx_mqttd)
//          -> DB detections 최신 행 조회 -> 평면도 좌표 변환
//          -> guardx/display/rpic/track
//        RPi C(matrix_link) -> Modbus 121~125 -> STM32 HUB75 평면도
//
// 왜 VMS가 좌표를 직접 안 보내는가: 화면이 들고 있는 것은 그리기용 박스라
// 예측 위치가 없다. 카메라가 계산한 predicted_x/predicted_y(현재 위치 +
// 속도 x 2초)는 폴러를 거쳐 detections.predicted_geom 에만 있다. 좌표의
// 진실원천이 DB이므로 조회도 DB 옆에서 한다 — VMS는 "누구를 볼 것인가"만
// 정한다.
//
// 이 모듈은 카메라를 모른다(guardx_mqttd 의 분리 기준). DB를 읽을 뿐이고,
// 그 DB를 채우는 것이 폴러라는 사실에 의존하지 않는다 — 폴러가 죽어 있으면
// 최신 행이 낡아서 조회가 비고, 그러면 발행이 멈춘다. 그게 올바른 결과다.
//
// ── 수신 (guardx/db/rpib/cmd/track_display) ──
//   {"node_id":"vms","timestamp":1234567890,"action":"START",
//    "global_id":0,"channel":0,"object_id":6461,"label":"P-6461"}
//   {"node_id":"vms","timestamp":1234567890,"action":"STOP"}
//
//   global_id != 0 이면 그것으로, 0이면 (channel, object_id)로 대상을
//   찾는다 — VMS TrackId(track_history.h)와 같은 분기다. channel 은 화면
//   채널(0-based)이고 detections.raw_channel 과 같은 축이다.
//
//   START 는 되풀이해서 온다(VMS 가 5초마다 재전송). 마지막 START 로부터
//   ARM_TTL 안에 다음 것이 안 오면 스스로 무장 해제한다 — VMS 가 죽거나
//   네트워크가 끊겼을 때 LED 가 영영 옛 사람을 가리키지 않도록.
//
// ── 발행 (guardx/display/rpic/track) ──
//   {"node_id":"rpib","timestamp":...,"seq":N,
//    "status":3,"ax":420,"ay":180,"bx":510,"by":205}
//
//   status bit0 = 현재점 A 유효, bit1 = 예측점 B 유효. 0 이면 두 점 제거.
//   좌표는 LED 평면도 기준 0~1000 (좌상단 원점). 규격은 rpi_c 의
//   matrix_link.h 및 STM32 modbus_slave.h 와 같은 표를 본다.
#include <pqxx/pqxx>

#include "Config/config.hpp"

/**
 * @brief 추적 송출 서비스 시작 — guardx_mqttd INIT 에서 1회
 *
 * cmd 토픽 구독만 건다. DB 는 만지지 않는다 — 구독 콜백은 libmosquitto
 * 네트워크 스레드에서 불리고 pqxx::connection 은 스레드 안전하지 않아서,
 * 이 모듈은 콜백에서 "누구를 볼지"만 기록하고 조회는 전부 메인 루프의
 * publishTrackDisplay() 에서 한다. 그래서 전용 커넥션이 필요 없다
 * (task_vms 가 ensureQdb 로 커넥션을 따로 여는 것과 다른 선택이다).
 */
void startTrackDisplayService(const Config& cfg);

/**
 * @brief 무장 상태면 최신 좌표를 조회해 발행 — guardx_mqttd 틱에서 매초 호출
 *
 * 실제 조회/발행은 내부에서 PUBLISH_INTERVAL_S 로 솎는다. 카메라 폴러가
 * det_interval_s(기본 2초)로 DB 를 채우므로 그보다 자주 읽어봐야 같은
 * 행만 다시 나온다.
 *
 * 호출자의 커넥션을 그대로 쓴다(단일 스레드). 예외는 내부에서 삼킨다 —
 * 이 발행이 실패했다고 mqttd 의 상태 발행 루프가 죽으면 안 된다.
 */
void publishTrackDisplay(pqxx::connection& db);
