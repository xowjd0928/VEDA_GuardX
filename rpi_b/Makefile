# Makefile — CMake 를 쓸 수 없는 환경용 백업 빌드.
# 정식 빌드는 CMakeLists.txt (cmake -B build && cmake --build build -j).
CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Iinclude
LDLIBS   = -lcurl -lpqxx -lpq -lmosquitto
# -lcrypto: 작업 E 로그인(PBKDF2·SHA-256·RAND_bytes). sudo apt install libssl-dev
MQTTD_LDLIBS = -lpqxx -lpq -lmosquitto -lcrypto

# 공용 — 브로커 전송 계층
MQTT_SRC = src/Mqtt/mqtt_pub.cpp

# 카메라 폴러 (카메라 → DB 적재 + 경보 판정)
POLLER_SRC = src/main.cpp \
      src/Poller/poller_main.cpp \
      src/Poller/http_client.cpp \
      src/Poller/task_detections.cpp \
      src/Poller/task_prediction.cpp \
      src/Poller/task_occupancy.cpp \
      src/Poller/task_faces.cpp \
      src/Poller/task_flow.cpp \
      src/Poller/task_alert.cpp \
      src/Poller/task_config_sync.cpp \
      src/Poller/realert_signal.cpp \
      src/Poller/task_tracks.cpp \
      src/Poller/task_analytics_trajectories.cpp \
      src/Database/db.cpp \
      src/Storage/state.cpp \
      src/Config/credentials.cpp

# VMS 조회 서비스 (DB ↔ VMS) — curl 불필요
MQTTD_SRC = src/mqtt_main.cpp \
      src/MqttDb/mqtt_service.cpp \
      src/MqttDb/task_vms.cpp \
      src/MqttDb/task_track_display.cpp \
      src/MqttDb/task_auth.cpp

# 계정 비밀번호 도구 (작업 E 부속). task_auth 의 pbkdf2/randomHex 만 쓰지만
# 같은 번역 단위에 pqxx 를 쓰는 함수들이 있어 -lpqxx 가 함께 필요하다.
PASSWD_SRC = tools/guardx_passwd.cpp \
      src/MqttDb/task_auth.cpp

all: guardx_poller guardx_mqttd guardx_passwd

guardx_poller: $(POLLER_SRC) $(MQTT_SRC)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

guardx_mqttd: $(MQTTD_SRC) $(MQTT_SRC)
	$(CXX) $(CXXFLAGS) $^ $(MQTTD_LDLIBS) -o $@

guardx_passwd: $(PASSWD_SRC)
	$(CXX) $(CXXFLAGS) $^ $(MQTTD_LDLIBS) -o $@

clean:
	rm -f guardx_poller guardx_mqttd guardx_passwd

.PHONY: all clean
