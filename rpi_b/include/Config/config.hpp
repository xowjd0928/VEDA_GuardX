#pragma once
// config.hpp — 설정 로드: config.env 파일 → 환경변수 오버라이드 순
//
// config.env 형식 (셸 스타일과 호환 — source 해도 되고 파일로 읽어도 됨):
//   # 주석
//   export CAM_HOST=172.20.33.201
//   CAM_PASS='pw'          (따옴표 허용)
// 우선순위: 환경변수 > config.env > 기본값
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

struct Config {
  std::string host, user, pass, pgconn, state_path;
  // 조회 전용 접속 문자열 (guardx_reader). guardx_mqttd 의 query/* 핸들러와
  // 상태 발행이 쓴다 — 조회 경로가 쓰기 권한을 들고 다니지 않게 하려는 것.
  //
  // ⚠ 미설정이면 pgconn 으로 폴백한다. **필수로 두면 안 된다**:
  //   sync_to_rpi.sh 는 config.env 를 exclude 한다(장비별 설정이라 일부러).
  //   코드를 배포해도 Pi 의 config.env 에는 이 키가 안 생기므로, 필수로 두면
  //   그 순간 mqttd 가 못 뜬다. 대신 폴백을 **조용히** 하지 않는다 —
  //   아래 플래그를 보고 mqtt_service 가 기동 시 경고를 찍는다. 롤 분리가
  //   안 된 채 "된 줄 알고" 넘어가는 것이 이 작업의 유일한 실패 모드다.
  std::string pgconn_ro;
  bool pgconn_ro_is_fallback = false;
  // 작업 F 과도기 스위치. false = 토큰 **없는** 쓰기 명령을 경고와 함께 허용.
  // VMS 전 대수 배포를 확인한 뒤 1 로 올린다. 기본을 1 로 두면 배포 순서상
  // 서버가 먼저 올라간 순간 구버전 VMS 의 설정 변경이 전부 막힌다.
  // ⚠ 관대해지는 것은 "토큰 없음"뿐이다 — 틀린 토큰은 이 값과 무관하게 거부.
  bool require_token = false;
  std::string cainfo;       // CAM_CAINFO: 카메라 인증서(PEM) 경로 — 지정 시 고정
  std::string pinned_key;   // CAM_PINNED_KEY: "sha256//BASE64" 공개키 핀.
                            // 자체서명 + IP 접속(호스트명 불일치)용 — cainfo보다 우선
  bool insecure = false;
  // pred 60s: 모델이 1분 해상도 — 더 자주 읽으면 같은 값만 중복 적재 (v13)
  // face 10s: 이벤트성 소량 피드 (링 보존 60분 — 여유 큼), 지연 최소화용 (v15)
  // alert 10s: 판정을 pred에서 분리 (07-31) — 60초에 묶여 있던 것이 "임계
  //   바꿨는데 반응 없음"의 원인이었다. /occupancy는 가볍고 zone_occupancy
  //   upsert는 멱등이라 10초로 당겨도 안전 (VMS_CODE_MAP.md 부록 B)
  int det_interval_s = 2, pred_interval_s = 60, cfg_interval_s = 30;
  int face_interval_s = 10, alert_interval_s = 10;
  int trajectory_interval_s = 5;
  // sensor 1s: RPi A가 1Hz로 보내는 값을 zones/dates(30초 틱)에 묶으면 VMS
  // 실시간 그래프가 30초에 한 번만 갱신된다 — 별도 빠른 틱으로 분리.
  int sensor_interval_s = 1;
  int camera_id = 1, channel = 1;
  // MQTT (통신 규약): 브로커는 RPi B 자신 — 경보 시 액추에이터 명령 발행
  std::string mqtt_host = "localhost";
  int mqtt_port = 1883;
  // v14: 폴링 대상 채널 목록 (CHANNELS="1,2,3" 형식). 기본은 CHANNEL 단일 —
  // 카메라 v14(?channel=) 배포 후 실측된 채널 번호로 확장한다.
  std::vector<int> channels;

  static std::map<std::string, std::string> parseEnvFile(const std::string& path) {
    std::map<std::string, std::string> kv;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
      // trim leading spaces
      size_t b = line.find_first_not_of(" \t");
      if (b == std::string::npos) continue;
      line = line.substr(b);
      if (line.empty() || line[0] == '#') continue;
      if (line.rfind("export ", 0) == 0) line = line.substr(7);
      size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string k = line.substr(0, eq);
      std::string v = line.substr(eq + 1);
      // strip trailing spaces/CR and surrounding quotes
      while (!v.empty() && (v.back() == '\r' || v.back() == ' ' || v.back() == '\t')) v.pop_back();
      if (v.size() >= 2 && ((v.front() == '\'' && v.back() == '\'') ||
                            (v.front() == '"' && v.back() == '"'))) {
        v = v.substr(1, v.size() - 2);
      }
      kv[k] = v;
    }
    return kv;
  }

  // 우선순위: 환경변수 > 파일 > 기본값
  static std::string pick(const std::map<std::string, std::string>& file,
                          const char* key, const std::string& def) {
    const char* e = std::getenv(key);
    if (e && *e) return e;
    auto it = file.find(key);
    if (it != file.end()) return it->second;
    return def;
  }

  static Config load(const std::string& env_path = "config.env") {
    auto file = parseEnvFile(env_path);
    Config c;
    c.host       = pick(file, "CAM_HOST", "192.168.0.3");
    c.user       = pick(file, "CAM_USER", "admin");
    c.pass       = pick(file, "CAM_PASS", "");
    c.pgconn     = pick(file, "PGCONN", "host=localhost dbname=guardx user=guardx_writer");
    // 미설정이면 pgconn 폴백 + 플래그. 판단(경고 출력)은 여기가 아니라
    // 쓰는 쪽에서 한다 — Config 는 파서일 뿐이고 헤더에서 로그를 찍지 않는다.
    c.pgconn_ro  = pick(file, "PGCONN_RO", "");
    c.pgconn_ro_is_fallback = c.pgconn_ro.empty();
    if (c.pgconn_ro_is_fallback) c.pgconn_ro = c.pgconn;
    c.require_token = pick(file, "REQUIRE_TOKEN", "0") == "1";
    c.state_path = pick(file, "STATE_PATH", "./state/poller_state.json");
    c.insecure   = pick(file, "CAM_INSECURE", "0") == "1";
    c.cainfo     = pick(file, "CAM_CAINFO", "");
    c.pinned_key = pick(file, "CAM_PINNED_KEY", "");
    c.det_interval_s  = atoi(pick(file, "DET_INTERVAL_S",  "2").c_str());
    c.pred_interval_s = atoi(pick(file, "PRED_INTERVAL_S", "60").c_str());
    c.cfg_interval_s  = atoi(pick(file, "CFG_INTERVAL_S",  "30").c_str());
    c.face_interval_s = atoi(pick(file, "FACE_INTERVAL_S", "10").c_str());
    c.alert_interval_s = atoi(pick(file, "ALERT_INTERVAL_S", "10").c_str());
    c.trajectory_interval_s = atoi(pick(file, "TRAJECTORY_INTERVAL_S", "5").c_str());
    c.sensor_interval_s = atoi(pick(file, "SENSOR_INTERVAL_S", "1").c_str());
    c.camera_id       = atoi(pick(file, "CAMERA_ID", "1").c_str());
    c.mqtt_host       = pick(file, "MQTT_HOST", "localhost");
    c.mqtt_port       = atoi(pick(file, "MQTT_PORT", "1883").c_str());
    c.channel         = atoi(pick(file, "CHANNEL",   "1").c_str());
    // CHANNELS 파싱 (쉼표 구분). 미지정이면 CHANNEL 단일 — v13 거동 그대로.
    const std::string chs = pick(file, "CHANNELS", "");
    if (chs.empty()) {
      c.channels.push_back(c.channel);
    } else {
      size_t b = 0;
      while (b < chs.size()) {
        size_t e = chs.find(',', b);
        if (e == std::string::npos) { e = chs.size(); }
        const std::string tok = chs.substr(b, e - b);
        if (!tok.empty()) { c.channels.push_back(atoi(tok.c_str())); }
        b = e + 1;
      }
      if (c.channels.empty()) { c.channels.push_back(c.channel); }
    }
    return c;
  }

  std::string base()     const { return "https://" + host + "/opensdk/juan_application"; }
  std::string wiseai()   const { return "https://" + host + "/opensdk/WiseAI/configuration"; }
  // 동선 추적(global_id) 스냅샷 — base() 와 다른 앱이라 별도 헬퍼가 필요하다.
  // https 인 이유: 카메라가 http 를 301 로 넘겨서, http 로 두면 폴링마다 왕복이
  // 하나씩 더 붙는다 (핀 검증 자체는 리다이렉트 후에도 그대로 적용된다).
  std::string tracks()   const { return "https://" + host + "/opensdk/test/tracks"; }
  std::string trajectories() const { return "https://" + host + "/opensdk/test/analytics/trajectories"; }
  std::string userpass() const { return user + ":" + pass; }
};
