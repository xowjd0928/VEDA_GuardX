A → B → C 3대 테스트

실물 센서/액추에이터 없이(simulate 모드) 세 노드 실기로 전 구간을 검증한다.
A의 가스 센서 드라이버에 값을 주입하면 → B가 판단하고 → C의 액추에이터
드라이버가 움직이는지까지 관통 확인.

노드

IP

역할

RPi A 

172.20.27.70 

센서 퍼블리셔 

RPi B 

172.20.33.251 

브로커 + 판단 엔진 

RPi C 



액추에이터 서브스크라이버 

C는 IP가 필요 없다. A와 C는 B에만 붙는다.

1. 연결 확인

A, C 각각에서:

mosquitto_pub -h 172.20.33.251 -t test -m hi -d

received CONNACK (0) 뜨면 통과.

안 되면 B에서:

sudo systemctl start mosquitto
ss -lnt | grep 1883        # 0.0.0.0:1883 이어야 함

2. 실행 (기기별 터미널 1개씩)

B:

cd ~/7th_VEDA_GROUP2/rpi_b/rpib_app/app
make && ./rpib_engine

잘된 경우:

mqtt: connected to localhost:1883 (tls=0)
mqtt: subscribed to guardx/sensor/rpia (qos0), guardx/sensor/rpia/button (qos2)

C:

cd ~/7th_VEDA_GROUP2/rpi_c/rpic_app/test
sudo ./01_load_modules.sh
cd ../app && make && sudo ./rpic_subscriber

잘된 경우:

mqtt: connected to 172.20.33.251:1883 (tls=0)     ← localhost면 실패
mqtt: subscribed to guardx/actuator/rpic (qos1)

A:

cd ~/hyeon/7th_VEDA_GROUP2/rpi_a/rpia_app/test
sudo ./01_load_modules.sh
cd ../app && make && sudo ./rpia_publisher

잘된 경우:

mqtt: connected to 172.20.33.251:1883 (tls=0)
main: rpia publisher started



안될경우

sed -i 's/\r$//' *.sh
chmod +x *.sh

3. 감시 + 발화

C에서 (새 터미널):

dmesg -w | grep rpic_

A에서 (새 터미널):

echo Y | sudo tee /sys/module/rpia_gas/parameters/simulate_detected

3초 뒤 C 화면에:

rpic_pca9685: ch=1 value=90 applied     ← 가스밸브 잠금
rpic_pca9685: ch=0 value=90 applied     ← 문 개방
rpic_pca9685: ch=2 value=100 applied    ← 팬 최대
rpic_pump: ON
rpic_amp: ON

B 화면에도 !!! FIRE CONFIRMED (cause=gas, sensor_seq=N) !!! 이 떠야 한다.

해제 — A에서:

echo N | sudo tee /sys/module/rpia_gas/parameters/simulate_detected

10초 뒤 C에 rpic_pump: OFF, rpic_amp: OFF, ch=2 value=0 세 줄.
서보는 안 움직이는 게 정상이다 (가스밸브 자동 재개방 금지 — 사람이
현장 확인 후 수동 복구).

4. 정리 (다시 테스트하려면)

A, B, C 각각:

sudo pkill -f rpia_publisher
sudo pkill -f rpib_engine
sudo pkill -f rpic_subscriber

A:

sudo rmmod rpia_button rpia_spark rpia_temphum rpia_gas

C:

sudo rmmod rpic_amp rpic_pump rpic_pca9685

확인:

ps aux | grep -E 'rpia_|rpib_|rpic_' | grep -v grep    # 비어야 함
lsmod | grep -E 'rpia_|rpic_'                          # 비어야 함

pkill 먼저, rmmod 나중이다. 앱이 /dev를 잡고 있으면 rmmod가 실패한다.

막힐 때

mosquitto.h: No such file (B 빌드 시)

sudo apt install libmosquitto-dev

그 외 증상

원인

Operation not permitted (insmod) 

sudo 빠짐 

connected to localhost (A/C에서) 

헤더 IP 안 바뀜, 또는 make clean 필요 

B가 조용함 

A가 안 붙음. B의 rpib_events.jsonl이 1초에 한 줄씩 느는지 확인 

C가 조용한데 B는 FIRE 

C가 엉뚱한 브로커 봄. C 화면의 connected to 확인 

참고: 판단 규칙

셋 중 아무거나 하나가 3사이클(3초) 연속 걸리면 화재 확정:

가스 > 1000ppm   이거나
온도 > 60도       이거나
불꽃 감지         이거나

해제는 모든 지표가 10사이클(10초) 연속 정상일 때. valid=false인
지표는 카운터를 동결한다(올리지도 리셋하지도 않음).

임계값·사이클 수는 rpi_b/rpib_app/app/include/decision.h에 있다.