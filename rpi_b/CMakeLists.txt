# CMakeLists.txt — GuardX rpi_b (guardx_poller + guardx_mqttd)
# 의존: sudo apt install cmake libcurl4-openssl-dev nlohmann-json3-dev libpqxx-dev libmosquitto-dev
# 빌드:
#   cmake -B build && cmake --build build -j
# 산출물: build/guardx_poller  build/guardx_mqttd
#         build/db_smoke  build/det_smoke  build/test_parsing
# 실행은 rpi_b 루트에서 (config.env·state/·test/fixtures 상대경로):
#   ./build/guardx_poller   ·   ./build/guardx_mqttd   ·   ./build/test_parsing
#
# 구조 (v16 정리): include/<모듈>/*.hpp + src/<모듈>/*.cpp — include 경로는
# "모듈/…hpp" 형태 그대로 (include/ 루트 하나만 -I).
#
# 모듈 (v2, VEDA-155):
#   Config·Database·Storage  설정·DB 헬퍼·커서
#   Mqtt      브로커 전송 계층 (양쪽 공용)
#   Poller    카메라 → DB 적재 + 경보 판정      -> guardx_poller
#   MqttDb    DB ↔ VMS 질의응답 + 상태 발행     -> guardx_mqttd
# Poller 와 MqttDb 는 서로를 include 하지 않는다. 상세: MQTT_SERVICE_SPLIT.md
cmake_minimum_required(VERSION 3.16)
project(guardx_poller CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wformat-truncation=2 -O2)

find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
find_library(MOSQUITTO_LIB mosquitto REQUIRED)   # sudo apt install libmosquitto-dev

# ── OpenSSL (libcrypto) — 작업 E 로그인 (2026-08-11 추가) ──
# PBKDF2-HMAC-SHA256 · SHA-256 · RAND_bytes 용. **새 의존이다** —
# mqttdb_core 는 지금까지 pqxx + mosquitto 만 링크했다(핸드오프 §5 의
# "폴러가 이미 libcrypto 링크라 새 의존성 0" 은 guardx_poller 얘기고,
# 로그인이 들어가는 곳은 guardx_mqttd 다).
#
# 헤더 패키지가 필요하다: sudo apt install libssl-dev
# (libcurl4-openssl-dev 가 이미 끌어오므로 대개 깔려 있다. 없으면 여기서
#  cmake 가 멈춘다 — 빌드 도중이 아니라 설정 단계에서 멈추는 편이 낫다.)
find_package(OpenSSL REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)
# 노드 간 공유 규약 헤더(shared/). 팬 단계 토픽처럼 RPi C·VMS 와 같은
# 문자열을 써야 하는 것들이 여기 있다.
include_directories(${CMAKE_SOURCE_DIR}/../shared)

# ── Mqtt: 브로커 전송 계층 (양쪽 공용) ──
# 두 실행 파일이 모두 의존하는 유일한 지점. 여기에 카메라도 DB도 안 들어온다.
add_library(mqtt_core STATIC
  src/Mqtt/mqtt_pub.cpp
)
target_link_libraries(mqtt_core PUBLIC ${MOSQUITTO_LIB})

# ── Poller: 카메라 → DB 적재 + 경보 판정 ──
add_library(camera_core STATIC
  src/Poller/http_client.cpp
  src/Poller/task_detections.cpp
  src/Poller/task_prediction.cpp
  src/Poller/task_occupancy.cpp
  src/Poller/task_fan_level.cpp
  src/Poller/task_faces.cpp
  src/Poller/task_flow.cpp
  src/Poller/task_alert.cpp
  src/Poller/task_config_sync.cpp
  src/Poller/realert_signal.cpp
  src/Poller/task_tracks.cpp
  src/Poller/task_analytics_trajectories.cpp
  src/Database/db.cpp
  src/Storage/state.cpp
  src/Config/credentials.cpp
)
target_link_libraries(camera_core PUBLIC CURL::libcurl PkgConfig::PQXX mqtt_core)

# ── MqttDb: DB ↔ VMS 질의응답 + 상태 발행 ──
# curl 을 링크하지 않는다 — 이 모듈은 카메라에 HTTP 로 접근할 일이 없다.
add_library(mqttdb_core STATIC
  src/MqttDb/task_vms.cpp
  src/MqttDb/task_track_display.cpp
  src/MqttDb/task_auth.cpp
  src/MqttDb/mqtt_service.cpp
)
# OpenSSL::Crypto 만 — TLS(OpenSSL::SSL)는 안 쓴다. 브로커 TLS 는 libmosquitto
# 안쪽 일이고 이 모듈이 직접 소켓을 열지 않는다.
target_link_libraries(mqttdb_core PUBLIC PkgConfig::PQXX mqtt_core OpenSSL::Crypto)

# camera_core 와 mqttdb_core 는 서로를 참조하지 않는다 (v2). 둘 사이의 유일한
# 대화는 브로커를 지나는 TOPIC_ZONE_CHANGED 뿐이다 — 링크 그래프가 그 사실을
# 그대로 보여줘야 나중에 누가 실수로 다시 붙이지 않는다.

# ── 실행 파일 ──
add_executable(guardx_poller
  src/main.cpp
  src/Poller/poller_main.cpp
)
target_link_libraries(guardx_poller PRIVATE camera_core)

add_executable(guardx_mqttd
  src/mqtt_main.cpp
)
target_link_libraries(guardx_mqttd PRIVATE mqttdb_core)

# ── 계정 비밀번호 도구 (작업 E 부속) ──
# 이게 없으면 migration_vms_auth.sql 이 심어 둔 **git 에 공개된 시드
# 비밀번호를 바꿀 방법이 없다** — Postgres 에 PBKDF2-SHA256 이 없기 때문이다.
add_executable(guardx_passwd
  tools/guardx_passwd.cpp
)
target_link_libraries(guardx_passwd PRIVATE mqttdb_core)

# ── 스모크/테스트 (파일 있을 때만 타깃 생성) ──
if(EXISTS ${CMAKE_SOURCE_DIR}/test/db_smoke.cpp)
  add_executable(db_smoke test/db_smoke.cpp)
  target_link_libraries(db_smoke PRIVATE camera_core)   # lookupZoneId
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/test/det_smoke.cpp)
  add_executable(det_smoke test/det_smoke.cpp)
  target_link_libraries(det_smoke PRIVATE PkgConfig::PQXX)
endif()

# 좌표 변환은 DB·브로커·하드웨어 없이 검증한다 — 링크할 라이브러리가 없다.
if(EXISTS ${CMAKE_SOURCE_DIR}/test/track_plan_test.cpp)
  add_executable(track_plan_test test/track_plan_test.cpp)
endif()

# 팬 단계가 VMS 혼잡 경보 단계와 항상 같은지 — 히스테리시스 포함.
if(EXISTS ${CMAKE_SOURCE_DIR}/test/fan_level_test.cpp)
  add_executable(fan_level_test
    test/fan_level_test.cpp src/Poller/task_fan_level.cpp)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/test/test_parsing.cpp)
  add_executable(test_parsing test/test_parsing.cpp)
  # 픽스처가 "test/fixtures/…" 상대경로 → rpi_b 루트에서 실행:
  #   ./build/test_parsing
endif()
