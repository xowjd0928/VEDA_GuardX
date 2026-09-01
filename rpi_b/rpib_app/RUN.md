# 빌드 후 실행 방법

RPi B는 커널 드라이버가 없다 (하드웨어 없음). 구성 요소는 둘:

1. **mosquitto 브로커** - 설정 파일만 (broker/guardx_broker.conf)
2. **rpib_engine** - 판단 엔진 (app/)

## A. 브로커 (broker/)

```bash
sudo apt install mosquitto mosquitto-clients
sudo cp broker/guardx_broker.conf /etc/mosquitto/conf.d/
sudo systemctl restart mosquitto
ss -lnt | grep 1883        # 리스닝 확인
```

지금은 평문+익명(1단계). mTLS 전환(2단계)은 `common/certs/` 참조.

## B. 엔진 (app/)

```bash
cd rpib_app/app
make                        # rpib_engine 바이너리 생성

# 브로커가 localhost에 떠 있어야 함 (같은 기기라 IP 설정 불필요)
./rpib_engine               # 포그라운드 실행, Ctrl+C로 종료
```

sudo가 필요 없다 - /dev 접근이 없고, 이벤트 기록도 현재 디렉토리의
`rpib_events.jsonl`에 쓴다.

## C. 전체 검증 (A/C 실기 없이)

```bash
cd rpib_app/test
./01_start_broker.sh        # 브로커 확보
# 별 터미널: ../app/rpib_engine
# 별 터미널: ./03_watch_rpic.sh
./02_fake_rpia.sh fire_demo
```

시나리오별 상세는 `test/TEST_GUIDE.md`.

---

# 부팅 시 자동 시작 (systemd)

```bash
cd rpib_app
sudo ./install.sh
```

- 엔진은 `/opt/guardx/rpib/app/rpib_engine`, 기록은
  `/opt/guardx/rpib/rpib_events.jsonl` (WorkingDirectory 기준)
- `After=mosquitto.service`로 순서만 잡고 `Requires`는 안 건다 -
  브로커가 늦게 떠도 엔진의 내부 재접속에 맡긴다 (A/C와 동일 정책)

```bash
sudo systemctl start rpib_engine
journalctl -u rpib_engine -f
```

## 배포 전 확인 사항

- B의 IP를 고정할 것 (A/C 코드에 하드코딩되므로 DHCP 변동 금지)
- 3대 NTP 동기화 (payload timestamp 전제)
- mTLS 전환 시: broker conf 교체 + A/C의 MQTT_USE_TLS=1 재빌드
