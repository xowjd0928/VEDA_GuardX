기준 정보:
RPi B IP: 172.20.33.251
RPi B 계정: hyeon
RPi C IP: 172.20.33.114
RPi C 계정: dev

### RPi B
# 1. 인증서 생성
cd ~/guardx/rpi_b/common/certs
./gen_certs.sh 172.20.33.251

# 2. Mosquitto 브로커용 인증서 배치
sudo mkdir -p /etc/mosquitto/certs
sudo cp out/ca.crt out/rpib.crt out/rpib.key /etc/mosquitto/certs/
sudo chown root:mosquitto /etc/mosquitto/certs/rpib.key
sudo chmod 640 /etc/mosquitto/certs/rpib.key

# mTLS 설정 적용 후 8883 포트 확인
sudo cp guardx_mtls.conf /etc/mosquitto/conf.d/
sudo systemctl restart mosquitto
ss -tln | grep 8883

# 3. actuator_test용 인증서도 별도로 배치
sudo mkdir -p /etc/guardx/certs
sudo cp out/ca.crt out/rpib.crt out/rpib.key /etc/guardx/certs/
sudo chown root:hyeon /etc/guardx/certs/rpib.key
sudo chmod 640 /etc/guardx/certs/rpib.key

# 4. RPi C로 C 전용 인증서 전송
cd out
scp ca.crt rpic.crt dev@172.20.33.114:~/
sudo scp rpic.key dev@172.20.33.114:~/


### RPi C
# 5. 구독자용 인증서 배치
sudo mkdir -p /etc/guardx/certs
sudo cp ~/ca.crt ~/rpic.crt ~/rpic.key /etc/guardx/certs/
sudo chmod 600 /etc/guardx/certs/rpic.key

# 6. 실제 하드웨어 드라이버 빌드 및 적재
cd ~/guardx/rpi_c/rpic_app/drivers
make
sudo insmod ./rpic_pca9685.ko
sudo insmod ./rpic_stepper.ko
sudo insmod ./rpic_pump.ko

# 7. 구독자 실행
cd ~/guardx/rpi_c/rpic_app/app
make
sudo ./rpic_subscriber
성공 메시지:
connected to 172.20.33.251:8883 (tls=1)
RPi B 새 터미널
# 8. 테스트 명령 발행
cd ~/guardx/rpi_b/rpib_app/test
make actuator_test
MQTT_TLS=1 MQTT_HOST=172.20.33.251 ./actuator_test
성공 메시지:
[접속] 172.20.33.251:8883 (tls=1)