# GuardX 액추에이터 BBC 테스트 매뉴얼

BBC는 **RPi B 발행 → RPi B 브로커 → RPi C 구독·구동** 경로를 뜻한다.

## 0. 구성 & 터미널 (총 3개)

```text
[RPi B] actuator_test  ──publish──►  [RPi B] mosquitto 브로커  ──►  [RPi C] rpic_subscriber ──► 액추에이터
        (터미널 B2)                    (터미널 B1)                    (터미널 C1)
```

| 터미널 | 노드 | 역할 | 계속 떠 있나 |
|---|---|---|---|
| B1 | RPi B | 브로커(mosquitto) | ✅ 유지 |
| C1 | RPi C | 드라이버 로드 + 구독자 | ✅ 유지 |
| B2 | RPi B | `actuator_test` 메뉴(발행) | 조작용 |

실행 순서: **B1 브로커 → C1 구독자 → B2 발행**

## 1. 최초 1회 준비

### RPi C

```bash
sudo apt install -y mosquitto-clients libmosquitto-dev libasound2-dev raspberrypi-kernel-headers
```

`config.txt`에 다음 설정이 없으면 추가한 뒤 재부팅한다.

```ini
dtparam=i2c_arm=on
dtparam=audio=off
dtoverlay=pwm,pin=12,func=4
dtoverlay=max98357a,no-sdmode
gpio=17=ip,pu
gpio=27=ip,pu
```

구독자 브로커 주소를 확인한다.

```text
파일: ~/guardx/rpi_c/rpic_app/app/include/mqtt_sub.h
설정: MQTT_BROKER_HOST = "172.20.33.251"
포트: 1883 (MQTT_USE_TLS=0)
```

드라이버와 구독자를 빌드한다.

```bash
cd ~/guardx/rpi_c/rpic_app/drivers
make

cd ~/guardx/rpi_c/rpic_app/app
make
```

### RPi B

```bash
sudo apt install -y mosquitto mosquitto-clients libmosquitto-dev

cd ~/guardx/rpi_b/rpib_app/test
make actuator_test
```

## 2. 실행 순서

### 터미널 B1 — RPi B 브로커

```bash
cd ~/guardx/rpi_b/rpib_app/test
./01_start_broker.sh
```

- `테스트 브로커 기동`이 나오고 대기하면 정상이다.
- `1883 포트에 이미 브로커가 떠 있음`과 기존 브로커 사용 안내가 나오면 그대로 다음 단계로 간다.
- 테스트가 끝날 때까지 브로커를 종료하지 않는다.

### 터미널 C1 — RPi C 드라이버 + 구독자

기존 구독자가 실행 중이면 먼저 `Ctrl+C`로 종료한다.

```bash
cd ~/guardx/rpi_c/rpic_app/drivers

sudo insmod ./rpic_pca9685.ko
sudo insmod ./rpic_stepper.ko
sudo insmod ./rpic_pump.ko

ls -l /dev/rpic_*
```

다음 장치 3개가 보이면 정상이다.

```text
/dev/rpic_pca9685
/dev/rpic_stepper
/dev/rpic_pump
```

이미 모듈이 적재됐다는 오류가 나오면 중복 적재하지 말고 `lsmod | grep rpic_`로 확인한다.

구독자를 실행한다.

```bash
cd ~/guardx/rpi_c/rpic_app/app
sudo ./rpic_subscriber
```

기대 로그:

```text
mqtt: connected to 172.20.33.251:1883 (tls=0)
main: rpic subscriber started
mqtt: subscribed to guardx/actuator/rpic/# (qos1)
```

이 상태로 계속 대기하면 정상이다.

하드웨어 없이 MQTT 파이프라인만 확인할 때는 실제 모듈 대신 다음처럼 적재한다.

```bash
sudo insmod ./rpic_pca9685.ko simulate=1
sudo insmod ./rpic_stepper.ko simulate=1
sudo insmod ./rpic_pump.ko simulate=1
```

### 터미널 B2 — RPi B 명령 발행

```bash
cd ~/guardx/rpi_b/rpib_app/test
./actuator_test
```

RPi B의 로컬 브로커가 아니라 다른 브로커를 사용할 때만 다음처럼 지정한다.

```bash
MQTT_HOST=172.20.33.251 ./actuator_test
```

메뉴:

```text
1) 팬 켜기(속도 입력)       2) 팬 끄기
3) 가스밸브 서보 각도
4) 워터펌프 ON              5) 워터펌프 OFF
6) 화재셔터(close/open/stop)
7) 스피커(0=기본 1=화재 2=강도 3=비상)
a) 전부 끄기                d) 데모
q) 종료
```

## 3. 권장 테스트 순서와 합격 기준

1. 팬 `30%` → C1에 `main: fan SET 30 ok`, 팬 회전, **스피커 무음**
2. 팬 OFF → `main: fan OFF ok`, 팬 정지
3. 가스밸브 서보 → 입력 각도로 이동
4. 펌프 ON/OFF → 펌프 동작/정지
5. 스텝모터 `+1` → CW 회전, GPIO17 감지 시 `position=TOP`으로 자동 정지
6. TOP에서 `+1` 재입력 → 안전 차단
7. TOP에서 `-1` → 즉시 반대 방향 이동 허용
8. GPIO27 감지 시 `position=BOTTOM`으로 자동 정지
9. BOTTOM에서 `-1` 재입력 → 안전 차단
10. BOTTOM에서 `+1` → 즉시 반대 방향 이동 허용
11. 스피커 메뉴 `7` → 선택한 상황음 재생

스텝모터 커널 로그:

```bash
sudo dmesg | grep rpic_stepper | tail -30
```

기대 예시:

```text
rpic_stepper: initial position=BETWEEN
rpic_stepper: CW(GPIO17) limit hit -> stepper stop, position=TOP
rpic_stepper: at TOP - CW rotate ignored
rpic_stepper: CCW start
rpic_stepper: CCW(GPIO27) limit hit -> stepper stop, position=BOTTOM
rpic_stepper: at BOTTOM - CCW rotate ignored
rpic_stepper: CW start
```

선택적으로 RPi B에 모니터 터미널을 하나 더 열 수 있다.

```bash
mosquitto_sub -h localhost -t 'guardx/actuator/rpic/#' -v
```

## 4. 종료 / 정리

```text
B2: q 입력
C1: Ctrl+C로 구독자 종료
```

구독자가 완전히 종료된 후 RPi C에서 모듈을 내린다.

```bash
sudo rmmod rpic_pump rpic_stepper rpic_pca9685
```

테스트용 브로커를 직접 띄운 경우 B1에서 `Ctrl+C`로 종료한다.

```bash
cd ~/guardx/rpi_b/rpib_app/test
./04_cleanup.sh
```
