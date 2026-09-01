# GuardX RPi A 테스트 가이드

드라이버 → HAL → App → MQTT 파이프라인을 검증하는 절차. RPi 4 (커널 헤더
설치됨) 위에서 실행한다.

## 두 가지 테스트 모드

1. **실 하드웨어 (기본)**: 센서 4종을 실제로 연결하고 드라이버를 실 모드로
   로드해 검증. `01_load_modules.sh`가 이 모드다.
2. **부분 시뮬레이션**: 실 센서 없이 드라이버 로직만 볼 때. 단 **`rpia_adc`
   (가스/불꽃, SPI)는 시뮬레이션 모드가 없다**(오버레이+probe라 실제 SPI
   장치 필요). 그래서 `simulate=1`로 값 주입이 되는 건 `temphum`/`irtemp`/
   `button` 3종뿐이고, App은 `OPEN_ALL()`이 4개를 모두 열어야 하므로 **App
   전체 파이프라인은 adc 하드웨어 없이는 못 돌린다.**

## 사전 준비

```bash
sudo apt install -y mosquitto mosquitto-clients libmosquitto-dev
sudo apt install -y raspberrypi-kernel-headers
mount | grep debugfs || sudo mount -t debugfs none /sys/kernel/debug
# rpia_adc 오버레이 설치는 ../RUN.md A-1 참조 (dtoverlay=rpia-adc)
```

## 테스트 구성 요소

| 파일 | 역할 |
|---|---|
| `01_load_modules.sh` | 드라이버 4종 빌드 + **실 하드웨어 모드** 로드 + /dev 노드 확인 |
| `02_start_broker.sh` | 로컬 테스트 브로커(평문 1883, 익명 허용) 기동 |
| `03_subscribe.sh` | `guardx/#` 전체 토픽 구독 모니터 (QoS 2 포함) |
| `04_inject.sh` | 시뮬레이션 값 주입 (temphum/irtemp/button, adc 제외) |
| `05_driver_smoke.sh` | App 없이 /dev 직접 read로 드라이버 단독 검증 |
| `06_cleanup.sh` | 모듈 언로드, 프로세스 정리 |

## A. 실 하드웨어 E2E (기본 경로)

```
[터미널 1] sudo ./01_load_modules.sh        # 4종 실 로드 (adc는 오버레이 전제)
[터미널 1] sudo ./05_driver_smoke.sh        # /dev 직접 read 스모크
[터미널 2] ./02_start_broker.sh             # 로컬 브로커 (포그라운드)
[터미널 3] ./03_subscribe.sh                # 구독 모니터 (포그라운드)
[터미널 1] cd ../app && \
           make EXTRA_CFLAGS='-DMQTT_USE_TLS=0 -DMQTT_BROKER_HOST=\"localhost\"' && \
           sudo ./rpia_publisher            # App (로컬 브로커로)
```

### 검증 포인트
- 터미널 3에 `guardx/sensor/rpia {...}`가 **약 1초 간격**, `seq` +1 증가
- `values`에 `gas_raw`/`spark_raw`(0~1023), `temperature`/`humidity`,
  `irtemp_ambient`/`irtemp_object` 가 실측값으로 나옴
- `valid` 4개 모두 `true`
- 버튼 누르면 1초 주기를 안 기다리고 **즉시** `guardx/sensor/rpia/button`
  `{"event":"emergency_button","press_count":N}` (QoS 2)

## B. 부분 시뮬레이션 (temphum/irtemp/button 드라이버 로직)

해당 센서 없이 드라이버 로직만 볼 때. adc는 제외(하드웨어 필요).

```bash
# simulate=1로 개별 로드
sudo insmod ../drivers/rpia_temphum.ko simulate=1
sudo insmod ../drivers/rpia_irtemp.ko  simulate=1
sudo insmod ../drivers/rpia_button.ko  simulate=1
```

### S1. 값 변경 반영
```bash
./04_inject.sh temphum 305 750   # 30.5도 / 75.0%
./04_inject.sh irtemp 250 365    # 주변 25.0 / 대상 36.5도
./05_driver_smoke.sh             # od로 주입값 확인 (adc는 미로드라 에러 무시)
```

### S2. 음수 온도 (x10 스케일 경계값)
debugfs가 u16이라 음수는 2의 보수 unsigned로 입력:
```bash
./04_inject.sh temphum 65486 500   # -5.0도 / 50.0%
```
smoke read에서 `-5.0`(=-50 x10)로 재해석되면 s16 경로 정상.

### S3. 비상 버튼 이벤트 (즉시성, QoS 2)
```bash
./04_inject.sh button
```
- 별도 셸의 blocking read(05의 button 단계)가 즉시 깨어남
- App까지 물렸다면 `guardx/sensor/rpia/button` 즉시 발행, `press_count` 확인

## C. 내성 시나리오 (App 실행 중, adc 하드웨어 필요)

### S4. 부분 실패 격리 (valid=false)
App 실행 중 드라이버 하나만 제거:
```bash
sudo rmmod rpia_temphum
```
- App이 죽지 않고 계속 발행, `"valid":{"temphum":false,...}` 표시
- stderr에 `READ_ALL: temphum read failed`
- 다시 `insmod` 후에도 계속 false인 것이 정상(fd가 이미 끊김 → App 재시작
  으로 복구. 런타임 재연결이 필요하면 HAL에 reopen 로직 추가 검토)

### S5. 브로커 단절 내성
터미널 2 브로커를 Ctrl+C로 죽였다 재기동:
- App 크래시 없이 stderr 에러만 출력
- 브로커 재기동 후 libmosquitto 내부 스레드(loop_start)가 자동 재접속

### S6. 종료 정리
`Ctrl+C`로 App 종료 → "shutting down" 후 정상 종료(exit 0).
`sudo ./06_cleanup.sh`로 모듈/프로세스 전체 정리.

## 이 테스트가 검증하지 못하는 것

- MQ-2 예열 특성, 버튼 채터링/디바운스(실측)
- gas raw→ppm 환산 / 불꽃·가스 임계값 판단 (RPi B 몫, 미착수)
- mTLS(8883) 경로 — 인증서 준비 후 프로덕션 빌드(`MQTT_USE_TLS 1` 기본)로 검증
- RPi B와의 실제 연동 (새 스키마 파서 착수 후)
