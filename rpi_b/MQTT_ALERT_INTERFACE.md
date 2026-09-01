# GuardX 혼잡 경보 MQTT 인터페이스 — transmission layer 전달용

> 발신: rpi_b 폴러 (담당: 카메라/폴러) · 수신: transmission layer
> 상태: **실기기 검증 완료** (2026-07-28, critical·clear 실수신 — 하단 실측 샘플)
> ⚠ 토픽명은 잠정 — 통신 프로토콜 규약 문서에 alert 토픽 신설 합의 필요

## 1. 개요

폴러가 카메라 기반 혼잡 판정을 하고, **상태가 바뀔 때마다** 이벤트 1건을
발행한다. 지속 상황에서는 재발행하지 않는다 (에피소드당 1~2건).
**어떤 액추에이터를 어떻게 움직일지는 전적으로 수신 측(너) 소관** —
이 인터페이스는 "무슨 일이 일어났는지"만 전달한다.

## 2. 접속 정보

| 항목 | 값 |
|---|---|
| 브로커 | RPi B (localhost:1883 — 원격 개발 시 `-h <RPi B IP>`) |
| 인증 | 현재 anonymous 허용 (추후 인증 도입 시 공동 변경) |
| 토픽 | `guardx/alert/rpib` |
| QoS | 1 (구독도 1 권장) |
| retain | false |
| client id | **고유값 필수** (예: `rpib-txlayer`). 폴러는 `rpib-poller` 사용 — 중복 금지 |

권장: `clean_session=false` + QoS 1 구독 — 네 프로세스가 죽어 있는 동안의
경보도 브로커(persistence 켜짐)가 보관했다가 재접속 시 배달해 준다.

## 3. Payload 스키마 (JSON)

```json
{"node_id":"rpib","timestamp":1785220328723,"seq":0,
 "event":"congestion","zone_id":1,"channel":1,"incident_id":1,
 "severity":"critical","count":1,"capacity":1,"source":"detection"}
```

| 필드 | 타입 | 의미 |
|---|---|---|
| `node_id` | string | 발행자 = `"rpib"` 고정 |
| `timestamp` | int | epoch 밀리초 |
| `seq` | int | 발행 프로세스 기준 단조 증가 (재시작 시 0 리셋 — 규약 §3 준수) |
| `event` | string | `"congestion"` 고정 (향후 이벤트 종류 확장 대비) |
| `zone_id` | int | DB zones.zone_id (1=ch1, 2=ch0, 3=ch2, 4=ch3) |
| `channel` | int | 카메라 채널 (0-기반) |
| `incident_id` | int | DB incidents PK — **에피소드 식별자** (아래 중복 제거 키) |
| `severity` | string | `"warn"` \| `"critical"` \| `"clear"` |
| `count` | int | 판정에 쓰인 인원수 |
| `capacity` | int | 존 수용 한계 (비율 = count/capacity) |
| `source` | string | `"detection"`(실측) \| `"prediction"`(예측) — 무엇이 판정을 이겼는지 |

## 4. severity 상태 머신 (수신 측이 알아야 할 전부)

```
(평상) ──warn──▶ (주의) ──critical──▶ (위험)
   ▲                │                    │
   └────── clear ◀──┴────────────────────┘
```

- **warn**: 주의 단계 진입 (기본 임계: 수용의 75%)
- **critical**: 위험 단계 (90%) — warn 없이 바로 올 수도 있음 (급증 시)
- **clear**: 에피소드 종료 — **액추에이터 원복 트리거** (이걸 놓치면 경보가 안 꺼진다)
- 강등(critical→warn) 이벤트는 없다 — critical이면 clear까지 유지
- clear 후 재발은 **새 incident_id**로 새 에피소드 시작

## 5. 중복·유실 처리

- QoS 1 = 드물게 같은 메시지 2회 도착 가능 → **(incident_id, severity) 조합이
  이미 처리한 것이면 무시** — 이거 하나면 끝 (명령이 멱등이면 생략도 가능)
- 발행 주기는 60초 틱 — 상황 발생부터 이벤트까지 최대 ~60초 지연

## 6. 구독 예제

빠른 확인:

```bash
mosquitto_sub -t 'guardx/alert/rpib' -v
```

C (libmosquitto — `sudo apt install libmosquitto-dev`):

```c
#include <mosquitto.h>

void on_message(struct mosquitto *m, void *ud,
                const struct mosquitto_message *msg) {
    // msg->payload = §3 JSON. severity 파싱 → 정책 실행:
    //   warn/critical → 액추에이터 ON (단계별 정책은 네 소관)
    //   clear         → 원복 (OFF)
}

int main(void) {
    mosquitto_lib_init();
    struct mosquitto *m = mosquitto_new("rpib-txlayer", /*clean_session=*/false, NULL);
    mosquitto_message_callback_set(m, on_message);
    mosquitto_connect(m, "localhost", 1883, 60);
    mosquitto_subscribe(m, NULL, "guardx/alert/rpib", /*qos=*/1);
    mosquitto_loop_forever(m, -1, 1);
}
```

## 7. 실측 샘플 (2026-07-28 검증 세션 원본)

```
guardx/alert/rpib {"node_id":"rpib","timestamp":1785220328723,"seq":0,"event":"congestion","zone_id":1,"channel":1,"incident_id":1,"severity":"critical","count":1,"capacity":1,"source":"detection"}
guardx/alert/rpib {"node_id":"rpib","timestamp":1785220416878,"seq":1,"event":"congestion","zone_id":1,"channel":1,"incident_id":1,"severity":"clear","count":...,"capacity":1,"source":"detection"}
```

## 8. 통합 테스트 트리거 (네 subscriber 개발 시)

RPi B에서 존 1 수용 한계를 임시로 1로 낮추고 ch1 앞에 사람 1명:

```bash
sudo -u postgres psql -d guardx -c "UPDATE zone_thresholds SET capacity_limit = 1 WHERE zone_id = 1;"
```

→ 60초 내 critical 수신. 원복(`capacity_limit = 10`) 후 사람이 빠지면
1~2분 내 clear 수신. 테스트 끝나면 **반드시 10으로 원복**.

## 9. 변경 절차

토픽·스키마 변경은 이 문서 + 통신 프로토콜 규약 문서 + 폴러 코드
(`rpi_b/src/Mqtt/mqtt_pub.cpp`)를 함께 갱신. 규약 문서에 미결 3건:
① `guardx/alert/rpib` 토픽 정식 등재, ② §3 node_id 의미(발행자/대상) 명시,
③ §4-5 camera 토픽은 "불요 — DB 직접 적재" 회신.
