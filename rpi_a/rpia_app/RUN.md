# 빌드 후 실행 방법 (실 센서 연결 기준)

드라이버 4종: `rpia_adc`(MCP3008, SPI) · `rpia_temphum`(SHT30, I2C) ·
`rpia_irtemp`(MLX90614, I2C) · `rpia_button`(버튼, GPIO 인터럽트).

- `rpia_adc`만 DT 오버레이 + probe 방식이라 오버레이 설치가 필요하고 `modprobe`로 로드.
- 나머지 3종은 init에서 직접 attach라 `insmod`.
- 배선/핀은 `../README.md` 4절 참조.

## A. 드라이버 (drivers/)

### A-1. SPI/I2C 활성화 + 오버레이 (최초 1회)
```bash
sudo raspi-config nonint do_spi 0
sudo raspi-config nonint do_i2c 0

cd rpia_app/drivers
make                           # rpia_adc/temphum/irtemp/button .ko
dtc -@ -I dts -O dtb -o rpia-adc.dtbo rpia-adc-overlay.dts
sudo cp rpia-adc.dtbo /boot/firmware/overlays/
# /boot/firmware/config.txt 의 [all] 섹션에 추가:
#   dtoverlay=rpia-adc
sudo reboot
```

### A-2. 로드
```bash
cd rpia_app/drivers
sudo modprobe rpia_adc         # 오버레이가 선언한 spi0/CE0 장치에 probe 바인딩
sudo insmod rpia_temphum.ko
sudo insmod rpia_irtemp.ko
sudo insmod rpia_button.ko     # GPIO23(=base 512+23) legacy 요청

lsmod | grep rpia_             # 로드 확인
ls /dev/rpia_*                 # adc/temphum/irtemp/button 4개 노드
ls /dev/spidev0.0              # 없어야 정상 (CE0가 rpia_adc로 넘어감)
dmesg | grep -E "rpia_adc|rpia_temphum|rpia_irtemp|rpia_button"
```
언로드:
```bash
sudo rmmod rpia_button rpia_irtemp rpia_temphum
sudo modprobe -r rpia_adc
```

> temphum/irtemp/button은 실 센서 없이 시험하려면 `simulate=1`로 로드해
> debugfs로 값 주입이 가능하다(예: `sudo insmod rpia_temphum.ko simulate=1`).
> `rpia_adc`는 시뮬레이션 모드가 없다(오버레이+probe라 실제 SPI 장치 필요).

## B. App (app/)

```bash
cd rpia_app/app
make                           # rpia_publisher (프로덕션: RPi B + mTLS)
```

App은 시작 시 `OPEN_ALL()`이 하나라도 실패하면 즉시 exit(1). "No such
file or directory"가 나면 드라이버가 로드 안 된 것이니 A단계부터 확인.

### 로컬 테스트 (RPi B 없이 파이프라인 확인)
프로덕션 기본값은 RPi B(`172.20.33.251`) + mTLS라 브로커가 없으면
`mqtt_pub_init`에서 종료된다. 로컬 브로커로 검증하려면:
```bash
make clean
make EXTRA_CFLAGS='-DMQTT_USE_TLS=0 -DMQTT_BROKER_HOST=\"localhost\"'

sudo systemctl start mosquitto
mosquitto_sub -h localhost -t 'guardx/sensor/#' -v   # 별도 터미널
sudo ./rpia_publisher
```
발행 JSON에 `gas_raw`/`spark_raw`(0~1023 raw), `irtemp_ambient`/
`irtemp_object`, `temperature`/`humidity`가 나오면 정상. 버튼 누르면
`guardx/sensor/rpia/button` 이벤트(QoS2).

## C. 부팅 시 자동 적재 (배포)

```bash
cd rpia_app
sudo ./install.sh
```
하는 일:
1. `drivers/`·`app/` 빌드 + `rpia-adc.dtbo` 컴파일
2. 산출물을 `/opt/guardx/rpia/{drivers,app}/`로 복사, 오버레이를
   `/boot/firmware/overlays/`에 설치(+ config.txt에 `dtoverlay=rpia-adc`)
3. `systemd/*.service`를 `/etc/systemd/system/`로 복사 + `daemon-reload`
4. 5개 서비스 `enable`: rpia_adc, rpia_temphum, rpia_irtemp, rpia_button,
   rpia_publisher

부팅 순서: 드라이버 4개(oneshot, RemainAfterExit) → `rpia_publisher`
(`Requires=`로 강제, 하나라도 실패 시 App도 안 뜸).

### 지금 바로 (오버레이 적용 상태에서, 재부팅 없이)
```bash
sudo systemctl start rpia_adc rpia_temphum rpia_irtemp rpia_button
sudo systemctl start rpia_publisher
journalctl -u rpia_publisher -f
```

## 주의
- `insmod` 경로(`/opt/guardx/rpia/drivers/*.ko`)는 커널 버전이 바뀌면
  재빌드해야 한다(유닛이 자동 감지 안 함).
- systemd 밑에서 App이 `/dev/rpia_*`에 접근하려면 root로 도는 걸 전제.
  비root로 돌리려면 udev 규칙으로 device 그룹을 지정하고 유닛에
  `User=`/`Group=` 추가.
- 프로덕션 유닛은 mTLS 기본값(포트 8883)으로 App을 실행하므로,
  `/etc/guardx/certs/`에 CA/노드 인증서가 배치돼 있어야 한다(없으면
  App이 tls_set에서 종료).
