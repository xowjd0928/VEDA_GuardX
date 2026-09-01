# RPi C 테스트 가이드 (시뮬레이션 기반)

실물 액추에이터(PCA9685 서보 / HW PWM 팬 / 28BYJ-48 스텝모터 / HG7881
펌프) 없이 **MQTT 수신 → JSON 파싱 → 디스패치 → 드라이버 write**
전 경로를 검증하는 절차. 커널 드라이버 3종을 `simulate=1`로 올린다(팬은
커널 모듈이 아니라 App이 sysfs로 제어, 앰프는 I2S/ALSA라 커널 모듈 없음 -
pwmchip0가 없으면 자동으로 팬 no-op 모드로 뜬다). RPi A의 TEST_GUIDE와
대칭 구조이며, 방향만 반대다.

## 사전 준비

```bash
sudo apt install mosquitto mosquitto-clients libmosquitto-dev raspberrypi-kernel-headers
```

## 1. 기본 경로 검증

터미널 4개 기준:

```bash
# T1: 드라이버 빌드 + 시뮬레이션 로드
cd rpic_app/test
sudo ./01_load_modules.sh

# T2: 테스트 브로커 (평문 1883)
./02_start_broker.sh

# T3: 서브스크라이버
cd ../app && make
sudo ./rpic_subscriber          # "mqtt: subscribed to guardx/actuator/rpic" 확인

# T4: RPi B 역할로 명령 발행
cd ../test
./03_publish_cmds.sh demo
```

**기대 결과**
- T3 stdout: `main: servo_1 SET 90 ok` 등 명령마다 한 줄
- `./03_publish_cmds.sh state` (또는 `dmesg | grep rpic_`):
  드라이버가 실제로 받은 값 로그 (`rpic_pca9685: ch=0 value=90 applied ...`)

## 2. 이상 입력 격리 검증

잘못된 명령 한 건이 프로세스를 세우지 않아야 한다 (T3가 계속 살아있으면 통과):

```bash
# 알 수 없는 command
mosquitto_pub -q 1 -t guardx/actuator/rpic -m '{"command":"toaster","action":"ON"}'
# 범위 초과 value
./03_publish_cmds.sh servo1 999
# JSON 깨짐
mosquitto_pub -q 1 -t guardx/actuator/rpic -m 'not json at all'
# 미배선 액추에이터
mosquitto_pub -q 1 -t guardx/actuator/rpic -m '{"command":"led","action":"ON"}'
```

기대: T3 stderr에 각각 `unknown command` / `failed (-5)` /
`malformed command dropped` / `not wired` 로그, 프로세스는 유지.

## 3. 드라이버 장애 격리 검증

App이 fd를 물고 있는 상태에서 특정 드라이버만 실패시켜 본다:

```bash
echo 1 | sudo tee /sys/module/rpic_pump/parameters/simulate_fail
./03_publish_cmds.sh pump on        # T3: "water_pump ON failed (-6)"
./03_publish_cmds.sh fan 50         # 다른 액추에이터는 정상 동작해야 함
echo 0 | sudo tee /sys/module/rpic_pump/parameters/simulate_fail
./03_publish_cmds.sh pump on        # 복구 확인
```

## 4. 브로커 재접속 검증

```bash
# T2의 브로커를 Ctrl+C로 종료 → T3에 접속 끊김 로그 → 브로커 재기동
./02_start_broker.sh
# libmosquitto가 자동 재접속 + on_connect에서 자동 재구독
./03_publish_cmds.sh fan 30         # 다시 수신되면 통과
```

## 5. 정상 종료 검증

T3에서 Ctrl+C → `main: shutting down` 후 dmesg에 pump OFF + 스텝모터 STOP,
서보 안전 각도 로그가 남아야 한다(팬은 커널 모듈이 아니라 dmesg엔 없고 App이
듀티 0 처리). 이후:

```bash
sudo ./04_cleanup.sh
```

## 크로스 노드 테스트 (RPi B ↔ RPi C)

RPi B(또는 아무 PC)에서 브로커를 띄우고, `app/include/mqtt_sub.h`의
`MQTT_BROKER_HOST`를 그 IP로 바꿔 재빌드하면 위 절차를 크로스 노드로
반복할 수 있다. mTLS까지 붙이려면 `MQTT_USE_TLS 1` + 인증서 배치
(mqtt_sub.h 주석 참조).
