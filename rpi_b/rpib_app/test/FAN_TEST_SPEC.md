# RPi B 팬 제어 테스트 사양

## 이름

| 구분 | 설정 |
|---|---|
| 소스 파일 | `fan_test.c` |
| 실행 파일 | `fan_test` |
| MQTT Client ID | `rpib_fan_test` |

## MQTT 설정

| 항목 | 설정 |
|---|---|
| 기본 Broker | `172.20.33.251` |
| Port | `1883` |
| 발행 Topic | `guardx/actuator/rpic/fan` |
| RPi C 구독 Topic | `guardx/actuator/rpic/#` |
| QoS | `1` |
| Retain | `false` |
| TLS / 계정 인증 | 사용 안 함 |

브로커 주소만 바꿀 때는 소스를 수정하지 않고 `MQTT_HOST` 환경 변수를 사용한다.

```bash
MQTT_HOST=192.168.0.10 ./fan_test 60
```

## 빌드

필요 패키지:

```bash
sudo apt install -y gcc make libmosquitto-dev
```

빌드 및 삭제:

```bash
make
make clean
```

## 명령

```bash
./fan_test on    # 팬 켜기: action=ON
./fan_test off   # 팬 끄기: action=OFF
./fan_test 60    # 팬 강도 60%: action=SET, value=60
./fan_test 0     # 팬 강도 0%: 정지
```

- 강도 범위는 정수 `0~100`이다.
- `on`, `off`는 소문자로 입력한다.
- `timestamp`는 실행 시점의 Unix epoch 밀리초를 사용한다.
- 단발성 테스트 프로그램이므로 `seq`는 항상 `1`이다.
- `node_id`는 요청한 명령 형식에 맞춰 `rpic`으로 설정했다.
- RPi C는 `guardx/actuator/rpic/#`를 구독하므로 기존
  `guardx/actuator/rpic` 토픽도 계속 수신한다.

강도 60% 전송 payload:

```json
{"node_id":"rpic","timestamp":1785200000000,"seq":1,"command":"fan","action":"SET","value":60}
```

동일한 명령을 `mosquitto_pub`으로 보내면 다음과 같다.

```bash
mosquitto_pub -h 172.20.33.251 \
  -t guardx/actuator/rpic/fan -q 1 \
  -m '{"node_id":"rpic","timestamp":0,"seq":1,"command":"fan","action":"SET","value":60}'
```
