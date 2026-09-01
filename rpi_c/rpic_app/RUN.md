# 빌드 후 실행 방법

## A. 드라이버 (drivers/)

```bash
cd rpic_app/drivers
make                       # rpic_pca9685/stepper/pump.ko 생성 (3개)
                           # (Makefile과 같은 위치, drivers/ 바로 밑에 생김)
                           # 팬은 커널 모듈이 아니라 App이 sysfs로 제어(.ko 없음)
                           # 앰프(MAX98357A I2S)는 커널 드라이버 없음 - 소리는 rpic_audio(ALSA)

sudo insmod rpic_pca9685.ko simulate=1    # 시뮬레이션 모드로 로드
sudo insmod rpic_stepper.ko simulate=1
sudo insmod rpic_pump.ko    simulate=1

lsmod | grep rpic_          # 로드 확인
ls /dev/rpic_*              # /dev 노드 자동 생성 확인 (udev가 class_create 보고 만들어줌)
dmesg | tail                # 로드 로그 확인

sudo rmmod rpic_pca9685 rpic_stepper rpic_pump   # 언로드
```

**실물 연결 후**에는 `simulate=1` 없이 그냥 `insmod rpic_pca9685.ko`만 하면
드라이버 내부에서 `simulate` 기본값(false)으로 실제 I2C/GPIO/PWM 경로를 탑니다.

실물 경로는 부팅 오버레이에 의존하므로 config.txt에 아래가 있어야 합니다:

```
dtparam=i2c_arm=on             # PCA9685 (i2cdetect -y 1 에서 0x40 확인)
dtparam=audio=off              # 아날로그 오디오 PWM 비활성화
dtoverlay=pwm,pin=12,func=4    # rpic_fan: GPIO12 하드웨어 PWM (pwmchip0)
dtoverlay=max98357a,sdmode-pin=4  # I2S 앰프 SD_MODE=BCM GPIO4(물리 7번), 커널이 전원/뮤트 관리
gpio=17=ip,pu                  # 스텝모터 CW(+) 리밋 리드센서 (풀업)
gpio=27=ip,pu                  # 스텝모터 CCW(-) 리밋 리드센서 (풀업)
```

- 팬(App sysfs 제어): `ls /sys/class/pwm/pwmchip0`이 보여야 실제 구동.
  없으면 App이 팬 no-op(soft) 모드로 뜨고 경고를 남긴다(노드는 계속 동작).
- `rpic_stepper`: GPIO 5/6/16/26이 다른 기능에 물려있지 않아야 함
- `rpic_pca9685`: `i2cdetect -y 1`에서 0x40 확인

`test/01_load_modules.sh`가 위 과정(빌드+언로드+로드+확인)을 자동화한 것입니다.

## B. App (app/)

```bash
sudo apt install -y libasound2-dev      # I2S 알림음(ALSA) - 구독자 빌드에 필요
cd rpic_app/app
make                        # rpic_subscriber 바이너리 생성 (app/ 밑에 생김)

# 실행 전: 드라이버가 로드되어 /dev/rpic_* 가 있어야 하고,
#          MQTT 브로커(로컬 테스트 or RPi B)에 접속 가능해야 함
sudo ./rpic_subscriber      # 포그라운드 실행, Ctrl+C로 종료
```

App은 시작 시 `OPEN_ALL()`이 하나라도 실패하면 즉시 exit(1)합니다.
"No such file or directory" 에러가 나면 드라이버가 로드 안 된 것이니
A단계부터 다시 확인하세요.

**I2S 상황음(통합됨):** `sound` 명령을 받을 때 MAX98357A로 상황음을
냅니다. 백그라운드 스레드 재생이라 명령 처리는
막지 않습니다. 소리는
부가 기능이라 카드가 없어도(시뮬레이션 등) 노드는 정상 동작하며, `sound`
명령을 처리할 때 open 실패 경고만 남기고 넘어갑니다. 구독자 시작 시에는
ALSA 장치를 열지 않고, 재생할 때만 열었다가 끝나면 즉시 닫습니다.
화재(`sound SET 1`)와 거수자(`sound SET 2`) 음원은 현재 작업 디렉터리가
아니라 실행 중인 `rpic_subscriber`의 위치를 기준으로 `../assets/audio`에서
찾습니다. 소스 트리에서 직접 실행해도, `install.sh`로 `/opt/guardx/rpic`에
설치해도 동일한 `app/`·`assets/audio/` 구조가 유지됩니다. 파일을 읽거나
재생하지 못하면 해당 상황의 기존 내장 톤으로 대체됩니다.

혼잡 경보는 VMS와 같은 `guardx/alert/rpib` 토픽을 구독합니다. 채널별
`severity`가 `critical`로 전이할 때 `crowd_alert.wav`를 한 번 재생하며,
같은 QoS 1 메시지가 중복 도착해도 다시 재생하지 않습니다.

## B-2. 오디오 단독 검증 도구 (audio_test)

`rpic_subscriber` 에 알림음이 이미 통합돼 있지만, **소리만 따로** 확인하고
싶을 때 쓰는 도구입니다(톤/WAV 재생).

```bash
cd rpic_app/app
make tools                                      # audio_test 생성
aplay -l                                        # I2S 카드(MAX98357A) 확인
./audio_test - tone 3 880 20                    # 880Hz 3초 20%
./audio_test plughw:CARD=MAX98357A,DEV=0 wav ../assets/audio/fire_alert.wav   # WAV 재생
```

### 방향별 리밋 정지(CW=GPIO17, CCW=GPIO27)는 스텝모터 드라이버에 통합됨

별도 프로세스 없이 **스텝모터 드라이버가 두 리드센서를 IRQ로 직접 감시**합니다.
CW(+)로 돌 때 GPIO17이, CCW(-)로 돌 때 GPIO27이 감지되면(LOW) 커널 안에서
그 방향 회전을 즉시 멈추고 `dmesg` 에 로그가 남습니다.

```bash
./03_publish_cmds.sh shutter close  # 셔터 닫기(CCW)
#  → GPIO27 BOTTOM 리드센서 감지 → 즉시 정지
./03_publish_cmds.sh shutter open   # 셔터 열기(CW)
#  → GPIO17 TOP 리드센서 감지 → 즉시 정지
dmesg | grep rpic_stepper | tail
#  rpic_stepper: CCW start
#  rpic_stepper: CCW(GPIO27) limit hit -> stepper stop
#  rpic_stepper: CW start
#  rpic_stepper: CW(GPIO17) limit hit -> stepper stop
./03_publish_cmds.sh shutter stop   # 수동 정지도 가능
```

MQTT는 `shutter CLOSE/OPEN/STOP`을 사용한다. RPi C가 이를 실물 모터
방향(닫기=CCW, 열기=CW)으로 변환한다.

> GPIO17/GPIO27은 **풀업이 필요**합니다(리드 스위치 접점이 GND). config.txt에
> `gpio=17=ip,pu` 와 `gpio=27=ip,pu` 를 넣거나, 온보드 풀업 리드센서 모듈을 쓰세요.
> 한 방향 리밋에 걸려 멈춘 뒤에도 **반대 방향 명령은 먹어서** 빠져나올 수 있습니다.

## C. 전체 순서 (처음부터)

```bash
sudo apt install mosquitto mosquitto-clients libmosquitto-dev libasound2-dev raspberrypi-kernel-headers

cd rpic_app/test
sudo ./01_load_modules.sh     # 드라이버 빌드+로드
./02_start_broker.sh &        # 테스트 브로커 (백그라운드 or 별 터미널)

cd ../app
make
sudo ./rpic_subscriber        # 구독 시작, 이제 명령 대기 상태

# 다른 터미널에서 RPi B 역할로 명령 발행:
cd rpic_app/test
./03_publish_cmds.sh demo
```

시나리오별 테스트는 `test/TEST_GUIDE.md` 참조.

---

# 부팅 시 자동 적재 (systemd)

RPi A와 동일한 패턴: 드라이버 3개(oneshot, insmod/rmmod) + App 1개
(simple, Restart=on-failure).

## 설치 (실제 배포 시, RPi 위에서 1회)

```bash
cd rpic_app
sudo ./install.sh
```

이 스크립트가 하는 일:
1. `drivers/`, `app/` 각각 `make`
2. 산출물을 `/opt/guardx/rpic/{drivers,app}/`로 복사
3. `systemd/*.service`를 `/etc/systemd/system/`로 복사 + `daemon-reload`
4. 4개 서비스 전부 `systemctl enable` (다음 부팅부터 자동 시작)

## 부팅 순서 (유닛 의존관계)

```
rpic_pca9685.service ─┐
rpic_stepper.service ─┼─→ rpic_subscriber.service (Requires + After)
rpic_pump.service    ─┘
(팬/앰프는 커널 모듈이 아니라 유닛 의존에 없음 - 팬은 App sysfs, 앰프는 I2S/ALSA)
```

드라이버 3개가 모듈을 올린 "뒤" App이 시작됩니다(`Requires=`로 강제,
하나라도 실패하면 App도 안 뜸). MQTT 브로커(RPi B)는 별도 노드라 부팅
순서를 맞출 수 없으므로, App은 브로커가 없어도 일단 뜨고
`mosquitto_loop_start`의 내부 재접속 로직에 맡깁니다.

## 지금 바로 켜보기 (재부팅 없이)

```bash
sudo systemctl start rpic_pca9685 rpic_stepper rpic_pump
sudo systemctl start rpic_subscriber
systemctl status rpic_subscriber
journalctl -u rpic_subscriber -f      # 로그 실시간
```

## 주의: RPi A와 공통으로 검증이 필요한 지점

- `insmod` 경로(`/opt/guardx/rpic/drivers/*.ko`)는 커널 버전이 바뀌면
  재빌드해야 하는데, 유닛 파일 자체는 그걸 감지하지 않습니다.
- 디바이스 파일 접근 권한은 root 실행 전제입니다.
- 시뮬레이션 모드(`simulate=1`)는 유닛 파일에 넣지 않았습니다(운영
  배포 기준). 테스트용 자동 적재가 필요하면 별도 유닛을 만들 것.
- 배포 전 `app/include/mqtt_sub.h`의 `MQTT_USE_TLS`/`MQTT_BROKER_HOST`를
  운영값(TLS=1, RPi B IP)으로 바꿔야 합니다 - 현재는 로컬 테스트
  기본값(평문/localhost)입니다.
