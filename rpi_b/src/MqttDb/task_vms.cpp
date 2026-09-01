#include "MqttDb/task_vms.hpp"
#include "Mqtt/mqtt_pub.hpp"
#include "Mqtt/topics.hpp"
#include "MqttDb/task_auth.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>   // ensureConn 이 던지는 std::runtime_error
#include <string>

// payload는 Postgres가 json_build_object로 직접 만든다. C++에서 문자열을
// 조립하지 않으므로 이스케이프 실수가 원천적으로 없고, VMS가 받는 형식이
// SQL 한 곳에만 적혀 있어 규격이 흩어지지 않는다.

namespace {

const char* TOPIC_ZONES    = "guardx/db/rpib/zones";
const char* TOPIC_DATES    = "guardx/db/rpib/dates";
const char* TOPIC_INCIDENTS = "guardx/db/rpib/incidents";

// 노드 간 주소 (Database/migration_endpoints.sql). 지금은 RPi C 의 RTP 방송
// 수신 주소 한 줄뿐 — 컴파일 상수(shared/broadcast_protocol.h)를 대체한다.
// zones 와 같은 상태 규약: 값이 바뀔 때만 retained 발행, VMS 는 구독만.
// 카메라 IP 는 여기 안 실린다 (검토 끝에 ini 유지로 확정).
const char* TOPIC_ENDPOINTS = "guardx/db/rpib/endpoints";
const char* TOPIC_HEATDAY  = "guardx/db/rpib/query/heatday";
const char* TOPIC_OCCSERIES = "guardx/db/rpib/query/occseries";
const char* TOPIC_QINCIDENTS = "guardx/db/rpib/query/incidents";
const char* TOPIC_TRAJECTORY = "guardx/db/rpib/query/trajectory";
const char* TOPIC_SETZONE  = "guardx/db/rpib/cmd/set_zone";

// 화재 판단 임계 (fire_schema.sql fire_threshold). 구역 정원과 성격이 같아
// 같은 규약을 쓴다 — 상태는 retained, 편집은 cmd + 응답.
const char* TOPIC_FIRE     = "guardx/db/rpib/fire_threshold";
const char* TOPIC_SETFIRE  = "guardx/db/rpib/cmd/set_fire_threshold";

// RPi A 환경센서 최신값 (fire_schema.sql sensor_reading/sensor_value).
// zones와 같은 상태 규약: 값이 바뀔 때만 retained 발행, VMS는 구독만.
// SENSOR_HISTORY는 VMS 켜질 때 최근 N분을 한 번 채우는 백필용 요청-응답 —
// 그 이후 실시간 갱신은 TOPIC_SENSORS 구독만으로 충분하다(1Hz 발행).
const char* TOPIC_SENSORS        = "guardx/db/rpib/sensors";
const char* TOPIC_SENSOR_HISTORY = "guardx/db/rpib/query/sensor_history";

// 로그인·세션 (작업 E — Database/migration_vms_auth.sql · MqttDb/task_auth.hpp).
// cmd 규약은 set_zone 계열과 같다(req_id·reply_to → reply_to 로 응답).
// ⚠ 비밀번호가 payload 에 평문으로 실린다 — mTLS(작업 A) 이후에만 유효한 설계.
const char* TOPIC_LOGIN         = "guardx/db/rpib/cmd/login";
const char* TOPIC_SESSION_CHECK = "guardx/db/rpib/cmd/session_check";
const char* TOPIC_LOGOUT        = "guardx/db/rpib/cmd/logout";

// 작업 G (§5b) — 본인 비밀번호 변경 · 계정 생성.
// 관리자가 남의 비밀번호를 재설정하는 경로는 일부러 없다(사용자 결정) —
// 잊어버린 계정은 서버에서 guardx_passwd 로 처리한다.
const char* TOPIC_CHANGE_PW  = "guardx/db/rpib/cmd/change_password";
const char* TOPIC_CREATE_USER = "guardx/db/rpib/cmd/create_user";

// 08-12 — 계정 비활성/재활성. "계정 삭제" 요구의 구현이다(진짜 DELETE 가 아니라
// enabled 플립 — 근거는 task_auth.hpp setUserEnabled 주석). 같은 cmd 규약.
const char* TOPIC_SET_USER_ENABLED = "guardx/db/rpib/cmd/set_user_enabled";

// 08-12 — 전역 설정 (SITE 문구·캘리브레이션). fire_threshold 와 같은 규약 —
// 상태는 retained, 편집은 cmd + 응답. 저장은 site_config 테이블
// (migration_site_config.sql, key-value·JSONB)이라 브로커 재시작에도 남고,
// 기동 시 DB → retained 재발행된다. 서버는 calibration 내용을 해석하지
// 않는다 — 스키마 정본은 VMS(calibration_store)다.
const char* TOPIC_SITECFG    = "guardx/db/rpib/site_config";
const char* TOPIC_SETSITECFG = "guardx/db/rpib/cmd/set_site_config";

// 액추에이터 수동 제어 (VMS -> B -> C, fire_schema.sql manual_command 주석의
// "설계 확정: VMS→B→C" 유일한 쓰기 경로). SET_ZONE/SET_FIRE와 같은 cmd 규약.
const char* TOPIC_SETACTUATOR = "guardx/db/rpib/cmd/set_actuator";

// RPi B -> RPi C 실제 제어 명령. guardx_protocol.h GUARDX_TOPIC_ACTUATOR_FMT("rpic")
// 전개형 — 이 파일은 rpi_a/common을 include하지 않으므로(빌드 의존 최소화) 문자열을
// 그대로 옮겨 적는다. 바뀌면 양쪽을 함께 고칠 것.
const char* TOPIC_ACTUATOR_RPIC = "guardx/actuator/rpic";
// TOPIC_ACTUATOR_RPIC이 향하는 노드 ID(위 토픽 문자열의 마지막 세그먼트와 동일).
// handleSetActuator()가 manual_command.zone_id를 채울 때 fire_zone.rpic_node_id로
// 역조회하는 키로 쓴다 — 여기서만 바꿔도 두 용례가 같이 맞는다.
const char* ACTUATOR_RPIC_NODE_ID = "rpic";

// 화재 상태 재접속 복원용 retained 스냅샷 — congestion의 guardx/db/rpib/incidents와
// 같은 역할. 전이 순간(edge)은 rpib_engine이 guardx/alert/fire로 직접 쏘고
// (이 프로세스를 거치지 않음), 여기선 "지금 화재가 열려있는가"만 주기 발행한다.
// 버튼(guardx/alert/button)은 상태가 아니라 사건이라 여기 대응 항목이 없다.
const char* TOPIC_FIRE_INCIDENT = "guardx/db/rpib/fire_incident";

// 화재 엔진(rpib_app) 핫리로드 신호. 페이로드는 보지 않고 "오면 DB를 다시
// 읽는다"가 규약이다 (rpib_app/app/src/main.c on_config).
// 이걸 안 쏘면 DB만 바뀌고 엔진은 옛 임계로 계속 판단한다.
const char* TOPIC_CONFIG_RELOAD = "guardx/config/rpib";

// TOPIC_ZONE_CHANGED(폴러에 보내는 재판정 신호)는 Mqtt/topics.hpp 에 있다 —
// 받는 쪽(Poller/realert_signal.cpp)과 같은 문자열을 봐야 하기 때문.

// 정원 허용 범위. 0 이하가 들어가면 VMS에서 OCC n/0 이 되어 나눗셈이 깨진다.
// 상한은 오타 방어용 — 실내 구역에 만 명이 들어갈 일은 없다.
const int CAP_MIN = 1;
const int CAP_MAX = 10000;

// 구역 이름 길이 상한 (바이트). 한글은 UTF-8에서 3바이트라 120이면 40자쯤이다.
// 화면(LIVE 타일 좌상단·REPORT 축 라벨)이 감당할 수 있는 선이 실질 기준이다.
const size_t NAME_MAX_BYTES = 120;

// set_site_config 요청 payload 전체 상한 (계약 §3.3 — 08-12). calibration 은
// 수 KB 예상이라 여유를 뒀다. 초과는 reason "too_large" 로 거부한다 —
// 쓰기를 여기서 막으면 retained 상태 토픽도 자연히 이 아래에 있게 된다.
const size_t SITE_CONFIG_MAX_BYTES = 16 * 1024;

// 날짜 경계를 자르는 기준. VMS가 보는 "하루"와 같아야 한다.
// (표시 타임존이 바뀌면 여기와 VMS 양쪽을 함께 고쳐야 한다)
const char* TZ_NAME = "Asia/Seoul";

// 마지막으로 발행한 payload — 바뀐 것만 쏘기 위한 비교용.
// 메인 루프(주기 발행)와 네트워크 스레드(set_zone 직후 발행)가 함께 만지므로
// 뮤텍스로 보호한다.
std::mutex g_pub_mtx;
std::string g_last_zones;
std::string g_last_dates;
std::string g_last_incidents;
std::string g_last_endpoints;
std::string g_last_fire;
std::string g_last_sensors;
std::string g_last_fire_incident;
std::string g_last_sitecfg;

// guardx/actuator/rpic 페이로드의 seq — 프로토콜 규약(guardx_protocol.h)대로
// "발행 프로세스 기준 단조 증가, 재시작 시 리셋 허용". manual_command.published_seq에도
// 같은 값을 남겨 역추적할 수 있게 한다. g_qmtx로 함께 보호한다(네트워크 스레드 하나뿐이라
// 사실상 경합은 없지만, set_zone 등과 같은 보호 범위 관례를 따른다).
long g_actuator_seq = 0;

// 핸들러 전용 커넥션 — **읽기와 쓰기를 계정 단위로 가른다** (08-11).
//
// 조회(query/*)는 guardx_reader, 쓰기(cmd/*)는 guardx_writer 로 붙는다.
// 하나로 쓰면 조회 경로가 쓰기 권한을 계속 들고 다니게 되어, 조회 SQL 에
// 실수나 주입이 생겼을 때 DB 가 막아줄 수단이 없다. 계정을 가르면 그 경우
// permission denied 로 끝난다 — 이게 "최후 방어선"의 실체다.
//
// ⚠ 두 커넥션 모두 mosquitto 네트워크 스레드에서만 쓴다. pqxx::connection 은
//   스레드 안전하지 않으므로 뮤텍스 하나로 둘 다 보호한다(스레드가 하나뿐이라
//   경합은 사실상 없고, 나누면 두 개를 언제 함께 잡아야 하는지가 생긴다).
std::mutex g_qmtx;
std::unique_ptr<pqxx::connection> g_db_ro;
std::unique_ptr<pqxx::connection> g_db_rw;
// 재접속에 필요한 접속 문자열. 시작 시 한 번 복사해 둔다 — 핸들러는
// 네트워크 스레드에서 불려 Config를 인자로 받을 수 없다.
std::string g_pgconn_ro;
std::string g_pgconn_rw;

/**
 * @brief 커넥션 확보 (없거나 끊겼으면 다시 연다). g_qmtx 보유 상태로 호출
 *
 * 폴러에 얹혀 있을 때는 Postgres가 재시작되면 프로세스가 통째로 죽고
 * systemd가 되살려서 이 문제가 가려졌다. 상시 서비스로 독립한 뒤에는
 * 스스로 회복하지 못하면 영영 "조회 커넥션 없음"만 답하게 된다.
 *
 * 실패하면 예외를 던진다 — 호출부는 전부 try 안에 있고, 그 catch 가 요청자에게
 * 실패 응답을 보낸다. 예전처럼 bool 을 돌려주면 호출부마다 검사 3줄이 붙고,
 * 그 3줄을 빠뜨린 자리가 조용히 널 역참조가 된다.
 */
pqxx::connection& ensureConn(std::unique_ptr<pqxx::connection>& c,
                             const std::string& conninfo, const char* label) {
  if (c && c->is_open()) return *c;
  try {
    c = std::make_unique<pqxx::connection>(conninfo);
    // 접속 계정을 함께 찍는다 — 롤을 가른 뒤로는 "연결은 됐는데 permission
    // denied" 의 범인이 대개 계정이다 (실사고 2026-08-03).
    std::cout << "[vms] " << label << " 커넥션 연결됨 (user=" << c->username()
              << ")\n";
    // libpqxx 는 접속 실패 시 생성자가 던지므로 여기까지 왔으면 열려 있는 것이
    // 정상이다. 그래도 확인한다 — 안 그러면 닫힌 커넥션을 참조로 넘겨
    // 호출부가 훨씬 뒤에서 알 수 없는 오류로 죽는다.
    if (!c->is_open()) throw std::runtime_error("커넥션이 열리지 않음");
  } catch (const std::exception& e) {
    c.reset();
    std::cerr << "[vms] " << label << " 커넥션 재연결 실패: " << e.what() << "\n";
    throw std::runtime_error("DB 연결 없음 (재연결 실패)");
  }
  return *c;
}

/** @brief 조회 전용 커넥션 (guardx_reader). g_qmtx 보유 상태로 호출 */
pqxx::connection& roDb() { return ensureConn(g_db_ro, g_pgconn_ro, "조회(ro)"); }

/** @brief 쓰기 커넥션 (guardx_writer). g_qmtx 보유 상태로 호출 */
pqxx::connection& rwDb() { return ensureConn(g_db_rw, g_pgconn_rw, "쓰기(rw)"); }

/** @brief JSON 문자열에서 문자열 값 하나 뽑기 (평면 payload 전용) */
std::string jget(const std::string& s, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  size_t p = s.find(pat);
  if (p == std::string::npos) return {};
  p = s.find(':', p + pat.size());
  if (p == std::string::npos) return {};
  p = s.find('"', p);
  if (p == std::string::npos) return {};
  const size_t e = s.find('"', p + 1);
  if (e == std::string::npos) return {};
  return s.substr(p + 1, e - p - 1);
}

/**
 * @brief JSON 문자열 값 하나 뽑기 — **이스케이프를 제대로 푼다**
 *
 * 위 jget 은 `\"` 를 모른다. 첫 `"` 에서 값을 끊으므로, 따옴표나 역슬래시가
 * 든 값은 조용히 잘린다. 그래서 handleSetZone 이 zone_name 에 그 두 글자를
 * **금지**하는 식으로 우회해 왔다.
 *
 * 비밀번호에는 그 우회를 쓸 수 없다 — 사용자가 고른 글자를 금지할 수 없고,
 * 잘린 채 비교하면 "맞는 비밀번호인데 계속 틀렸다고 나온다"가 된다. 원인을
 * 못 찾는 종류의 버그다.
 *
 * ⚠ 따로 만든 이유: jget 은 기존 핸들러 8곳이 쓰고 있고 이번 세션에서
 *   빌드·실기 검증을 못 한다. 검증 없이 공용 함수의 파싱 규칙을 바꾸는 것이
 *   더 위험하다고 판단했다. **jget 을 이 구현으로 흡수하는 것이 다음 정리
 *   대상이다** (그때 zone_name 의 금지 규칙도 함께 걷어낼 수 있다).
 */
std::string jgetStr(const std::string& s, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  size_t p = s.find(pat);
  if (p == std::string::npos) return {};
  p = s.find(':', p + pat.size());
  if (p == std::string::npos) return {};
  p = s.find('"', p);
  if (p == std::string::npos) return {};

  std::string out;
  for (size_t i = p + 1; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') return out;              // 닫는 따옴표 = 끝
    if (c != '\\') { out.push_back(c); continue; }
    if (++i >= s.size()) break;            // 끊긴 payload
    switch (s[i]) {
      case '"':  out.push_back('"');  break;
      case '\\': out.push_back('\\'); break;
      case '/':  out.push_back('/');  break;
      case 'b':  out.push_back('\b'); break;
      case 'f':  out.push_back('\f'); break;
      case 'n':  out.push_back('\n'); break;
      case 'r':  out.push_back('\r'); break;
      case 't':  out.push_back('\t'); break;
      case 'u': {
        // \uXXXX → UTF-8. VMS(Qt)는 한글을 그대로 실어 보내므로 실제로는
        // 거의 안 오지만, 다른 클라이언트가 쓰면 여기서 조용히 깨진다.
        // 서러게이트 쌍(U+10000 이상)은 다루지 않는다 — 비밀번호에 이모지를
        // 쓰는 경우인데, 그건 잘못 풀리면 로그인 실패로 드러나므로
        // 조용히 틀리지 않는다.
        if (i + 4 >= s.size()) break;
        unsigned cp = 0;
        bool okhex = true;
        for (int k = 1; k <= 4; ++k) {
          const char h = s[i + k];
          int v = -1;
          if (h >= '0' && h <= '9') v = h - '0';
          else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
          else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
          if (v < 0) { okhex = false; break; }
          cp = (cp << 4) | static_cast<unsigned>(v);
        }
        if (!okhex) break;
        i += 4;
        if (cp < 0x80) {
          out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
          out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        break;
      }
      default: out.push_back(s[i]); break;  // 규격 밖 — 원문 그대로
    }
  }
  return {};   // 닫는 따옴표를 못 만났다 = 망가진 payload
}

/**
 * @brief JSON 문자열로 내보내기 위한 이스케이프
 *
 * display_name·username 은 사람이 정하는 값이라 따옴표가 들어갈 수 있다.
 * 이스케이프 없이 조립하면 payload 가 깨져 VMS 파서가 통째로 실패한다.
 */
std::string jesc(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (const unsigned char c : in) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) {   // 그 밖의 제어문자 — \u00XX
          static const char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0x0F]);
        } else {
          out.push_back(static_cast<char>(c));   // UTF-8 은 그대로 통과
        }
    }
  }
  return out;
}

/** @brief JSON 숫자 값 하나 뽑기. 없으면 def */
double jnum(const std::string& s, const std::string& key, double def) {
  const std::string pat = "\"" + key + "\"";
  size_t p = s.find(pat);
  if (p == std::string::npos) return def;
  p = s.find(':', p + pat.size());
  if (p == std::string::npos) return def;
  try {
    return std::stod(s.substr(p + 1));
  } catch (...) {
    return def;
  }
}

/**
 * @brief JSON 불리언 값 하나 뽑기 — 3상태. 1=true, 0=false, -1=없거나 규격 밖
 *
 * jnum 으로 대신할 수 없다 — true/false 는 숫자가 아니라 stod 가 실패해
 * "없음"과 "false"를 구분 못 한다. set_user_enabled 는 그 구분이 필요하다
 * (enabled 누락을 조용히 false 로 읽으면 오타 하나로 계정이 꺼진다).
 */
int jboolTri(const std::string& s, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  size_t p = s.find(pat);
  if (p == std::string::npos) return -1;
  p = s.find(':', p + pat.size());
  if (p == std::string::npos) return -1;
  ++p;
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
  if (p >= s.size()) return -1;   // compare 는 pos > size 에서 던진다
  if (s.compare(p, 4, "true") == 0)  return 1;
  if (s.compare(p, 5, "false") == 0) return 0;
  return -1;
}

/**
 * @brief JSON 객체 값 하나를 통짜로 뽑는다 — `"key": { … }` 의 중괄호 균형 스캔
 *
 * jget/jgetStr 는 문자열 값 전용이라 객체를 못 뽑는다. 문자열 내부의
 * 중괄호·이스케이프(\")를 건너뛰며 짝을 맞춘다. 성공 시 값("{...}")을
 * 돌려주고 out_begin/out_end 에 **키 시작 ~ 값 끝** 구간을 적는다 —
 * 호출부가 그 구간을 지워 "그 키가 없는 payload"를 만들 수 있게
 * (handleSetSiteConfig 가 봉투 필드 오염을 막는 데 쓴다).
 *
 * 값이 객체가 아니거나 짝이 안 맞으면(끊긴 payload) 빈 문자열.
 */
std::string jgetObj(const std::string& s, const std::string& key,
                    size_t* out_begin, size_t* out_end) {
  const std::string pat = "\"" + key + "\"";
  const size_t k = s.find(pat);
  if (k == std::string::npos) return {};
  size_t p = s.find(':', k + pat.size());
  if (p == std::string::npos) return {};
  ++p;
  while (p < s.size() &&
         (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r'))
    ++p;
  if (p >= s.size() || s[p] != '{') return {};

  int  depth  = 0;
  bool in_str = false;
  for (size_t i = p; i < s.size(); ++i) {
    const char c = s[i];
    if (in_str) {
      if (c == '\\') { ++i; continue; }   // 이스케이프 다음 글자는 통과
      if (c == '"') in_str = false;
      continue;
    }
    if (c == '"') { in_str = true; continue; }
    if (c == '{') { ++depth; continue; }
    if (c == '}' && --depth == 0) {
      if (out_begin) *out_begin = k;
      if (out_end)   *out_end   = i + 1;
      return s.substr(p, i + 1 - p);
    }
  }
  return {};
}

/** @brief 값이 바뀐 경우에만 retained 발행 */
void publishIfChanged(const char* topic, const std::string& payload,
                      std::string& last) {
  std::lock_guard<std::mutex> lk(g_pub_mtx);
  if (payload.empty() || payload == last) return;
  mqttPublishRetained(topic, payload);
  last = payload;
  std::cout << "[vms] " << topic << " 발행 (" << payload.size() << "B)\n";
}

/** @brief zones 조회 SQL — 주기 발행과 set_zone 직후 발행이 공유한다 */
std::string queryZones(pqxx::work& tx) {
  // zone_id와 channel은 1:1이 아니다 (실측: zone1->ch1, zone2->ch0).
  // VMS는 channel로만 판단하므로 zones를 조인해 channel을 실어야 한다.
  // capacity_limit은 스키마상 NULL 가능이며 그대로 null로 내보낸다
  // (VMS는 "값 없음"으로 보고 기본값을 유지한다).
  //
  // zone_name은 구역명만 담는다 ("LOBBY EAST"). "CH1 · " 접두는 VMS가 붙인다 —
  // 채널 번호는 DB가 아니라 화면 배치의 문제고, 이름에 섞어두면 이름을 고칠
  // 때마다 접두까지 손으로 맞춰야 한다.
  pqxx::result r = tx.exec(
      "SELECT json_build_object("
      "  'node_id', 'rpib',"
      "  'timestamp', (extract(epoch from now()) * 1000)::bigint,"
      "  'zones', coalesce(json_agg(json_build_object("
      "      'zone_id', z.zone_id, 'channel', z.channel,"
      "      'zone_name', z.zone_name,"
      "      'capacity_limit', t.capacity_limit,"
      "      'warn_ratio', t.warn_ratio,"
      "      'critical_ratio', t.critical_ratio) ORDER BY z.channel),"
      "    '[]'::json))::text"
      " FROM zones z JOIN zone_thresholds t USING (zone_id)");
  return r.empty() ? std::string() : r[0][0].as<std::string>();
}

/**
 * @brief 노드 간 주소 조회 SQL — 주기 발행이 쓴다 (Database/migration_endpoints.sql)
 *
 * payload 형태:
 * ```json
 * {"node_id":"rpib","rpic_rtp_host":"172.20.33.114",
 *  "updated_at":"2026-08-11T10:22:31+09:00"}
 * ```
 * 표의 key 가 최상위 필드명이 된다 — 주소가 하나 늘어도 이 SQL 도 VMS 파서도
 * 안 바뀌고 INSERT 한 줄로 끝난다. 봉투 필드와의 이름 충돌은 테이블 CHECK 이
 * 막는다(예약어 node_id/timestamp/updated_at).
 *
 * ⚠ 시각은 **UTC + `Z` 접미 + 초 단위**로 못 박는다(프로젝트 규약, 08-11 확정).
 *   timestamptz 를 json 에 그냥 넣으면 **접속 세션의 TimeZone 설정대로** 렌더돼
 *   (`+09:00` vs `+00:00`) 서버 설정 한 줄로 payload 가 바뀐다. 그러면 내용이
 *   그대로인데도 dedup 비교가 어긋나 재발행이 일어난다.
 *
 * ⚠ 다른 상태 토픽과 달리 `timestamp`(now())를 싣지 않는다. 이 값은 사이트를
 *   옮기지 않는 한 영원히 안 바뀌는데, now() 를 넣으면 payload 가 매 틱 달라져
 *   publishIfChanged 의 비교가 항상 실패하고 30초마다 재발행하게 된다.
 *   대신 표의 max(updated_at) 을 싣는다 — "값이 언제 바뀌었나"라는 질문에는
 *   그쪽이 정확한 답이기도 하다.
 *
 * 값이 없으면 빈 문자열을 돌려준다 — publishIfChanged 가 알아서 건너뛴다.
 * (빈 표에 대고 발행하면 VMS 가 "주소 없음"을 받아 캐시를 지울 수 있다)
 */
std::string queryEndpoints(pqxx::work& tx) {
  // queryFireThreshold 와 같은 배포 순서 가드. Postgres 는 없는 테이블을 파스
  // 단계에서 거부하므로 CASE 로 감싸도 소용없고, 쿼리를 아예 안 보내야 한다.
  // 이 가드가 없으면 마이그레이션 전에 코드가 먼저 올라갔을 때 zones·dates
  // 발행까지 같은 트랜잭션에서 통째로 죽는다.
  const bool has_endpoints =
      tx.exec("SELECT to_regclass('public.endpoints') IS NOT NULL")[0][0]
          .as<bool>();
  if (!has_endpoints) return {};

  // jsonb 의 || 로 봉투와 kv 를 합친다. jsonb 는 키 순서를 정규화하므로
  // (길이→바이트순) 같은 내용이면 항상 같은 문자열이 나온다 — dedup 비교가
  // 성립하는 근거다. JSON 은 순서 없는 형식이라 수신 측에는 영향이 없다.
  //
  // 컬럼을 별칭으로 한정한다 — `key`·`value` 는 Postgres 비예약어라 그냥 써도
  // 통하지만, 표준 SQL 에서는 예약어라 버전·확장에 따라 물릴 여지가 있다.
  pqxx::result r = tx.exec(
      "SELECT CASE WHEN count(*) = 0 THEN NULL ELSE"
      "  (json_build_object('node_id', 'rpib')::jsonb"
      "   || json_object_agg(e.key, e.value)::jsonb"
      "   || json_build_object('updated_at',"
      "        to_char(max(e.updated_at) AT TIME ZONE 'UTC',"
      "                'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'))::jsonb)::text"
      " END FROM endpoints e");
  if (r.empty() || r[0][0].is_null()) return {};
  return r[0][0].as<std::string>();
}

/**
 * @brief 전역 설정(site_config) 조회 — 주기 발행이 쓴다 (08-12 신설)
 *
 * endpoints 와 같은 규약이다: 표의 key 가 최상위 필드가 되고(`site_name`·
 * `calibration`), `timestamp` 대신 max(updated_at) 을 싣는다 — now() 를 넣으면
 * 매 틱 payload 가 달라져 publishIfChanged 비교가 항상 실패한다(queryEndpoints
 * 주석 참조). jsonb 의 키 정규화가 dedup 비교의 근거인 것도 같다.
 *
 * 값은 jsonb 통짜로 나간다 — calibration 내용을 서버는 해석하지 않는다.
 * 저장된 키가 없으면 빈 문자열 — publishIfChanged 가 알아서 건너뛴다.
 */
std::string querySiteConfig(pqxx::work& tx) {
  // queryEndpoints 와 같은 배포 순서 가드 — migration_site_config.sql 이
  // 아직 안 돈 DB 에서 이 조회가 트랜잭션을 통째로 오염시키지 않게.
  const bool has_tbl =
      tx.exec("SELECT to_regclass('public.site_config') IS NOT NULL")[0][0]
          .as<bool>();
  if (!has_tbl) return {};

  pqxx::result r = tx.exec(
      "SELECT CASE WHEN count(*) = 0 THEN NULL ELSE"
      "  (json_build_object('node_id', 'rpib')::jsonb"
      "   || json_object_agg(s.key, s.value)::jsonb"
      "   || json_build_object('updated_at',"
      "        to_char(max(s.updated_at) AT TIME ZONE 'UTC',"
      "                'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'))::jsonb)::text"
      " END FROM site_config s");
  if (r.empty() || r[0][0].is_null()) return {};
  return r[0][0].as<std::string>();
}

/**
 * @brief 열려 있는 혼잡 incident 조회 SQL — 주기 발행이 공유한다
 *
 * **왜 필요한가**: 경보(`guardx/alert/rpib`)는 상태 *전이* 시점에만
 * retain=false 로 발행된다. 그래서 VMS가 경보 발생 뒤에 켜지거나 재접속하면
 * 이미 열린 incident를 영영 모른다 — 화면은 평온한데 현장은 critical.
 * 이 토픽이 그 구멍을 막는다 (retained = 접속 즉시 현재 상태 수신).
 *
 * incidents에는 인원/정원이 없다 (경보 문구에만 있다). VMS는 정원을
 * ZoneConfig로, 현재 인원을 카메라 직결로 이미 알고 있으므로 여기서는
 * "어느 채널이 어느 단계인가"만 실어 보내면 충분하다.
 */
std::string queryOpenIncidents(pqxx::work& tx) {
  // capacity/count 포함 (07-31): 없으면 VMS가 재시작 복원 시 "CRITICAL"만
  // 띄우고 "2/1" 같은 숫자를 못 쓴다. count는 zone_occupancy 최신 분 중앙값
  // (라이브 경보의 채택값과 다를 수 있으나 복원 표시용으로 충분).
  // LEFT JOIN — 임계 미설정 존의 incident도 목록에서 빠지면 안 된다.
  pqxx::result r = tx.exec(
      "SELECT json_build_object("
      "  'node_id', 'rpib',"
      "  'timestamp', (extract(epoch from now()) * 1000)::bigint,"
      "  'incidents', coalesce(json_agg(json_build_object("
      "      'incident_id', i.incident_id,"
      "      'zone_id', i.zone_id,"
      "      'channel', z.channel,"
      "      'severity', i.severity,"
      "      'source_type', i.source_type,"
      "      'capacity', t.capacity_limit,"
      "      'count', (SELECT o.person_count FROM zone_occupancy o"
      "                 WHERE o.zone_id = i.zone_id"
      "                 ORDER BY o.bucket_ts DESC LIMIT 1),"
      "      'detected_at', to_char(i.detected_at AT TIME ZONE 'UTC',"
      "                             'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'))"
      "    ORDER BY i.detected_at), '[]'::json))::text"
      " FROM incidents i JOIN zones z USING (zone_id)"
      " LEFT JOIN zone_thresholds t USING (zone_id)"
      " WHERE i.incident_type = 'congestion' AND i.status = 'open'");
  return r.empty() ? std::string() : r[0][0].as<std::string>();
}

/**
 * @brief 활성 화재 임계 1행 조회 — 주기 발행이 쓴다
 *
 * fire_threshold 는 "새 행 INSERT + is_active 플립"으로 버전을 넘기는
 * 이력 보존형이라(fire_schema.sql 2절), 지금 유효한 값은 is_active 한 행뿐이다.
 * 그 행을 통째로 실어 보낸다 — VMS의 편집 폼이 22개 컬럼을 다 채워야 하고,
 * 저장도 스냅샷 단위라 부분 전송이 의미가 없다.
 *
 * 활성 행이 없으면 threshold: null 로 나간다 (VMS가 "설정 없음"으로 표시).
 */
std::string queryFireThreshold(pqxx::work& tx) {
  // season_threshold 는 나중에 추가된 테이블이라(migration_season_threshold.sql,
  // 2026-08-04) 아직 없는 DB 가 있을 수 있다. Postgres 는 없는 테이블을 파스
  // 단계에서 거부하므로 CASE 로 감싸도 소용없다 — 쿼리 문자열 자체를 갈라야 한다.
  // 이 가드가 없으면 마이그레이션 전에 코드가 먼저 올라갔을 때 발행 쿼리가
  // 통째로 죽어 SETTINGS 화면이 비어버린다 (배포 순서 의존을 없애려는 것).
  const bool has_seasons =
      tx.exec("SELECT to_regclass('public.season_threshold') IS NOT NULL")[0][0]
          .as<bool>();

  // to_jsonb(t) 로 행을 통째로 옮긴다 — 컬럼을 손으로 나열하면 22개를 두 번
  // 적어야 하고, 스키마에 컬럼이 추가될 때 여기만 빠뜨리기 쉽다.
  // is_active 만 뺀다: 어느 행이 활성인지는 키 이름(threshold/default)이
  // 이미 말하고 있어서, 실어 보내면 VMS 폼이 그 값을 편집 대상으로 오해한다.
  //
  // default = threshold_id 최소 행 = fire_schema.sql 시드(공장 기본값).
  // VMS의 [기본값 불러오기]가 이걸 쓴다 — 따로 요청하면 왕복이 생기고,
  // 어차피 한 번 만들 payload 라 같이 싣는 편이 단순하다.
  std::string sql =
      "SELECT json_build_object("
      "  'node_id', 'rpib',"
      "  'timestamp', (extract(epoch from now()) * 1000)::bigint,"
      "  'threshold', (SELECT to_jsonb(t) - 'is_active'"
      "                  FROM fire_threshold t WHERE t.is_active LIMIT 1),"
      "  'default',   (SELECT to_jsonb(t) - 'is_active'"
      "                  FROM fire_threshold t ORDER BY t.threshold_id LIMIT 1)";

  // seasons = VMS 우측에 버튼으로 낼 계절 프리셋. sort_order 로 정렬해 보내므로
  // VMS 는 받은 순서대로 그리기만 한다 — 계절을 늘리거나 이름을 바꾸는 것이
  // DB 만의 일이 되도록. 'default' 행은 뺀다: [기본값 불러오기]가 fire_threshold
  // 시드를 직접 쓰므로 버튼으로 낼 것이 아니고, 그 행은 계절값을 정할 때
  // 비교용 기준선으로만 둔 것이다.
  if (has_seasons)
    sql +=
        ",  'seasons',  (SELECT jsonb_agg(to_jsonb(s) ORDER BY s.sort_order)"
        "                  FROM season_threshold s"
        "                 WHERE s.season_key <> 'default')";

  sql += ")::text";

  pqxx::result r = tx.exec(sql);
  return r.empty() ? std::string() : r[0][0].as<std::string>();
}


/**
 * @brief zone별 최신 센서 사이클 조회 — 주기 발행이 쓴다
 *
 * DEVICE CONTROL 화면(VMS)의 게이지·실시간 그래프 소스. RPi A의 raw 정책
 * 그대로 값·유효성만 옮긴다 — ppm 환산·위험 판정은 여기서 하지 않는다
 * (decision.c 책임, fire_schema.sql 주석 "raw 정책" 참조).
 *
 * payload 형태 (PHASE 7에서 zones 배열로 바뀜 — 아래 ⚠ 참조):
 * ```json
 * {"node_id":"rpib","timestamp":1785...,
 *  "zones":[{"zone_id":1,"zone_name":"1구역","sensor_seq":812,
 *            "composite_score":12.4,
 *            "channels":{"gas_raw":{"value":530,"is_valid":true}, ...}}]}
 * ```
 * channels는 channel_key를 키로 쓰는 object다 — VMS가 배열 인덱스가 아니라
 * 이름으로 접근하게 해서, 채널 순서가 바뀌거나 하나 빠져도 안 깨진다.
 *
 * ⚠ 호환성 없는 변경이다. 예전 payload는 최상위에 sensor_seq/composite_score/
 *   channels가 바로 있었다. VMS의 DeviceControlPage와 반드시 함께 배포할 것 —
 *   한쪽만 올리면 센서 화면이 통째로 빈다.
 *
 * fire_zone이 비어 있으면 zones가 []로 나가고, 데이터가 아직 없는 zone은
 * 행은 있되 값이 null이다("존재하지만 수신 대기"를 VMS가 구분해야 한다).
 */
std::string querySensors(pqxx::work& tx) {
  // PHASE 7: zone마다 최신 사이클을 하나씩 낸다.
  //
  // 전에는 zone 구분 없이 "가장 최근 1행"만 뽑았다. zone이 하나뿐일 땐
  // 우연히 맞았지만, 둘 이상이면 두 zone의 값이 번갈아 나와 VMS가 엉뚱한
  // 구역 값을 그리게 된다 — 조용히 틀리는 종류의 버그다.
  //
  // fire_zone을 왼쪽에 두고 LATERAL로 zone당 1행씩 당겨온다:
  //   - DISTINCT ON (zone_id)보다 낫다. 그쪽은 sensor_reading 전체를 훑고
  //     정렬해야 하는데, 이 표는 1Hz로 쌓여 하루 8.6만 행이다. LATERAL은
  //     idx_sensor_reading_zone_latest로 zone당 인덱스 조회 1번이면 끝난다.
  //   - LEFT JOIN이라 아직 데이터가 없는 zone도 행이 남는다(값은 null).
  //     VMS가 "그 zone은 존재하지만 수신 대기"를 구분할 수 있어야 한다.
  //
  // zone_name도 같이 싣는다 — VMS의 구역 선택 UI가 이름을 하드코딩하지
  // 않게(구역명의 진실원천은 DB다).
  pqxx::result r = tx.exec(
      "SELECT json_build_object("
      "  'node_id', 'rpib',"
      "  'timestamp', (extract(epoch from now()) * 1000)::bigint,"
      "  'zones', coalesce(json_agg(json_build_object("
      "      'zone_id', z.zone_id,"
      "      'zone_name', z.zone_name,"
      "      'sensor_seq', l.sensor_seq,"
      "      'composite_score', l.composite_score,"
      "      'channels', coalesce(("
      "          SELECT json_object_agg(c.channel_key,"
      "                   json_build_object('value', v.value, 'is_valid', v.is_valid))"
      "          FROM sensor_value v JOIN sensor_channel c USING (channel_id)"
      "          WHERE v.reading_id = l.reading_id"
      "      ), '{}'::json)"
      "    ) ORDER BY z.zone_id), '[]'::json)"
      ")::text"
      " FROM fire_zone z"
      " LEFT JOIN LATERAL ("
      "   SELECT r.reading_id, r.sensor_seq, r.composite_score"
      "   FROM sensor_reading r"
      "   WHERE r.zone_id = z.zone_id"
      "   ORDER BY r.reading_id DESC LIMIT 1"
      " ) l ON true");
  return r.empty() ? std::string() : r[0][0].as<std::string>();
}

/**
 * @brief 화재 상태 스냅샷 — 주기 발행이 쓴다 (재접속 복원용)
 *
 * fire_event의 최신 행 하나만 본다 — 그게 'fire_confirmed'면 지금 화재가
 * 진행 중이고, 'recovered'거나 행 자체가 없으면(엔진 최초 기동 전 등) 평상시.
 * incidents(congestion)와 달리 "열린 사건 목록"이 아니라 "지금 활성인가
 * 아닌가" 단일 불리언인 이유는 decision.c(zone마다 독립 판정, PHASE 6)가
 * 있어도 VMS가 아직 zone을 하나만 다루기 때문 — fire_event.zone_id는
 * PHASE 6 이후 항상 있지만, 이 쿼리는 zone 구분 없이 "가장 최근 사건
 * 하나"만 본다. zone이 실제로 2개 이상이 되면 이 LIMIT 1이 다른 zone의
 * 사건을 덮어써 섞일 수 있다 — zone별 스냅샷이 필요해지면 그때 zone_id로
 * GROUP BY/파티션해야 한다(지금은 VMS가 zone 1개만 알므로 범위 밖).
 *
 * cause는 fire_confirmed일 때만 채운다 — recovered 사건은 cause_channel_id가
 * NULL이라(db_writer_pg.c 참조) 자연히 NULL로 나간다.
 */
std::string queryFireIncident(pqxx::work& tx) {
  pqxx::result r = tx.exec(
      "WITH latest AS ("
      "  SELECT e.event_type, e.occurred_at, e.zone_id, c.channel_key AS cause"
      "  FROM fire_event e"
      "  LEFT JOIN sensor_channel c ON c.channel_id = e.cause_channel_id"
      "  ORDER BY e.occurred_at DESC LIMIT 1"
      ")"
      "SELECT json_build_object("
      "  'node_id', 'rpib',"
      "  'timestamp', (extract(epoch from now()) * 1000)::bigint,"
      "  'active', coalesce((SELECT event_type = 'fire_confirmed' FROM latest), false),"
      "  'zone_id', (SELECT zone_id FROM latest),"
      "  'cause', (SELECT cause FROM latest),"
      "  'occurred_at', (SELECT to_char(occurred_at AT TIME ZONE 'UTC',"
      "                                 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') FROM latest)"
      ")::text");
  return r.empty() ? std::string() : r[0][0].as<std::string>();
}

/**
 * @brief 히트맵 요청 1건 처리
 *
 * 실패해도 반드시 응답을 보낸다 — VMS는 "응답 없음"과 "에러"를 다른 상태로
 * 표시하고, 침묵하면 타임아웃까지 기다리게 된다.
 */
void handleHeatday(const std::string& req) {
  const std::string req_id = jget(req, "req_id");
  const std::string reply  = jget(req, "reply_to");
  const std::string date   = jget(req, "date");
  if (req_id.empty() || reply.empty() || date.empty()) {
    std::cerr << "[vms] heatday: req_id/reply_to/date 없는 요청 무시\n";
    return;
  }

  const int    cell     = (int)jnum(req, "cell", 60);
  const int    slot_min = (int)jnum(req, "slot_min", 10);
  const int    category = (int)jnum(req, "category", 1);
  const double min_lk   = jnum(req, "min_likelihood", 0.30);

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(roDb());

    // 하루를 slot_min 단위로 나눈 인덱스(0~143)와 카메라 격자로 집계.
    // ts 절대범위 조건은 파티션 프루닝을 살리기 위해 필수 (schema.sql 참조).
    // 캐스트를 전부 명시한다 — AT TIME ZONE·나눗셈의 파라미터는 Postgres가
    // 타입을 못 정하고 "could not determine data type of parameter"로 죽는다.
    //
    // ⚠ geom 이 아니라 **발밑점**(박스 아랫변 중앙)으로 집계한다.
    // geom 은 카메라가 준 x,y 를 그대로 넣은 것이라 **박스 무게중심**이다
    // (CAMERA_API_v15 예시로 확인: rect 1830~2050 × 700~1300 → x,y = 1940,1000).
    // 무게중심은 가슴 높이라 VMS 가 바닥평면 호모그래피에 넣으면 그 점이 바닥에
    // 있다고 가정되어 **사람이 카메라에서 멀어지는 쪽으로 계통적으로 밀린다**
    // (docs/FLOOR_CALIBRATION_RESEARCH.md §2.4 가 규칙을 못 박아 뒀다).
    //
    // detections 는 rect_* 원본을 같이 저장하므로 **읽는 쪽에서 고치면 이미
    // 쌓인 과거 데이터에도 그대로 적용된다** — 재수집이 필요 없다.
    // rect_* 는 nullable 이라 없으면 geom 으로 폴백한다(옛 동작과 동일).
    pqxx::result r = tx.exec(
        "WITH d AS (SELECT $1::date AS day)"
        " SELECT coalesce(json_agg(json_build_array(s, channel, gx, gy, w)),"
        "                 '[]'::json)::text"
        " FROM ("
        "   SELECT floor(extract(epoch FROM (ts AT TIME ZONE $2::text)"
        "                        - (SELECT day FROM d)::timestamp)"
        "                / ($3::int * 60))::int AS s,"
        "          channel,"
        "          floor(coalesce((rect_sx + rect_ex) / 2.0, ST_X(geom))"
        "                / $4::int)::int AS gx,"
        "          floor(coalesce(rect_ey::double precision, ST_Y(geom))"
        "                / $4::int)::int AS gy,"
        "          count(*) AS w"
        "   FROM detections"
        "   WHERE ts >= ((SELECT day FROM d)::timestamp AT TIME ZONE $2::text)"
        "     AND ts <  (((SELECT day FROM d) + 1)::timestamp AT TIME ZONE $2::text)"
        "     AND category = $5::int"
        "     AND likelihood >= $6::real"
        "   GROUP BY 1, 2, 3, 4"
        " ) t",
        pqxx::params{date, std::string(TZ_NAME), slot_min, cell, category, min_lk});
    tx.commit();

    const std::string cells = r.empty() ? "[]" : r[0][0].as<std::string>();
    const std::string out =
        "{\"node_id\":\"rpib\",\"req_id\":\"" + req_id + "\",\"date\":\"" +
        date + "\",\"ok\":true,\"cells\":" + cells + "}";
    mqttPublishReply(reply, out);
    std::cout << "[vms] heatday " << date << " 응답 " << out.size() << "B\n";

  } catch (const std::exception& e) {
    std::string msg = e.what();
    // 따옴표·줄바꿈이 들어가면 VMS의 JSON 파싱이 깨진다
    for (char& c : msg)
      if (c == '"' || c == '\n' || c == '\r') c = ' ';
    if (msg.size() > 200) msg.resize(200);

    const std::string out =
        "{\"node_id\":\"rpib\",\"req_id\":\"" + req_id + "\",\"date\":\"" +
        date + "\",\"ok\":false,\"error\":\"" + msg + "\"}";
    mqttPublishReply(reply, out);
    std::cerr << "[vms] heatday " << date << " 실패: " << msg << "\n";
  }
}

/** @brief 응답 발행 (성공/실패 공통) */
void replyOk(const std::string& topic, const std::string& req_id) {
  mqttPublishReply(topic,
      "{\"node_id\":\"rpib\",\"req_id\":\"" + req_id + "\",\"ok\":true}");
}

void replyErr(const std::string& topic, const std::string& req_id,
              std::string msg) {
  for (char& c : msg)
    if (c == '"' || c == '\n' || c == '\r') c = ' ';
  if (msg.size() > 200) msg.resize(200);
  mqttPublishReply(topic,
      "{\"node_id\":\"rpib\",\"req_id\":\"" + req_id +
      "\",\"ok\":false,\"error\":\"" + msg + "\"}");
}

// ────────────────────────────────────────────────────────────────────
// 작업 E·F — 로그인·세션·권한
// ────────────────────────────────────────────────────────────────────

// 과도기 스위치 (§6). false = 토큰 **없는** 요청을 경고 로그와 함께 허용한다.
// VMS 배포가 전 대수 끝난 것을 확인한 뒤 config.env 에서 REQUIRE_TOKEN=1 로
// 올린다. 시작 시 한 번 복사해 둔다 — 핸들러는 Config 를 못 받는다.
bool g_require_token = false;

/**
 * @brief 로그인 계열 실패 응답 — **reason 만 내보낸다**
 *
 * ⚠ replyErr 를 재사용하면 안 된다. 그쪽은 `e.what()` 원문을 그대로 싣는데,
 *   로그인 경로에서 그러면 ①DB 오류 문구로 스키마·테이블 존재가 새고
 *   ②"계정 존재 여부를 흘리지 않는다"는 설계가 무너진다(bad_credentials
 *   하나로 덮기로 한 이유가 그것이다).
 *
 * `error`(진단 문자열)와 `reason`(기계가 분기하는 열거값)은 이름만 비슷할 뿐
 * 성격이 다르다. 여기서는 reason 만 쓴다.
 */
void replyAuthFail(const std::string& topic, const std::string& req_id,
                   const std::string& reason, int retry_after_s = 0) {
  std::string out = "{\"node_id\":\"rpib\",\"req_id\":\"" + jesc(req_id) +
                    "\",\"ok\":false,\"reason\":\"" + reason + "\"";
  if (retry_after_s > 0)
    out += ",\"retry_after_s\":" + std::to_string(retry_after_s);
  out += "}";
  mqttPublishReply(topic, out);
}

/** @brief cmd/login — {username, password} → {ok, role, display_name, token, expires_at} */
void handleLogin(const std::string& req) {
  const std::string req_id = jgetStr(req, "req_id");
  const std::string reply  = jgetStr(req, "reply_to");
  if (req_id.empty() || reply.empty()) {
    std::cerr << "[auth] login: req_id/reply_to 없는 요청 무시\n";
    return;
  }

  // ⚠ jgetStr 을 쓴다 — 비밀번호에 따옴표·역슬래시가 들어갈 수 있다.
  const std::string username = jgetStr(req, "username");
  const std::string password = jgetStr(req, "password");
  // 감사용. VMS 가 안 실어 보내도 동작해야 하므로 없으면 빈 값.
  const std::string device   = jgetStr(req, "device");

  if (username.empty() || password.empty())
    return replyAuthFail(reply, req_id, auth::reason::kBadCredentials);

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(rwDb());
    const auth::Result r = auth::login(tx, username, password, device);
    // ⚠ 실패해도 commit 한다 — abort 하면 방금 올린 실패 카운트가 함께
    //   사라져 잠금이 영원히 안 걸린다.
    tx.commit();

    if (!r.ok) {
      replyAuthFail(reply, req_id, r.reason, r.retry_after_s);
      // 비밀번호·토큰은 절대 로그에 남기지 않는다. 사용자명까지가 한계다.
      std::cout << "[auth] login 실패 " << username << " — " << r.reason << "\n";
      return;
    }

    mqttPublishReply(reply,
        "{\"node_id\":\"rpib\",\"req_id\":\"" + jesc(req_id) +
        "\",\"ok\":true,\"role\":\"" + r.role +
        "\",\"display_name\":\"" + jesc(r.display_name) +
        "\",\"token\":\"" + r.token +
        "\",\"expires_at\":\"" + r.expires_at +
        "\",\"must_change\":" + (r.must_change ? "true" : "false") + "}");
    std::cout << "[auth] login " << username << " (" << r.role << ")"
              << (r.must_change ? " ⚠비밀번호 변경 필요" : "") << " 만료 "
              << r.expires_at << "\n";

  } catch (const std::exception& e) {
    // 예외 문구를 요청자에게 주지 않는다(위 replyAuthFail 주석). 로그에만.
    replyAuthFail(reply, req_id, auth::reason::kBadCredentials);
    std::cerr << "[auth] login 처리 실패: " << e.what() << "\n";
  }
}

/** @brief cmd/session_check — {token} → {ok, username, role, expires_at} */
void handleSessionCheck(const std::string& req) {
  const std::string req_id = jgetStr(req, "req_id");
  const std::string reply  = jgetStr(req, "reply_to");
  if (req_id.empty() || reply.empty()) {
    std::cerr << "[auth] session_check: req_id/reply_to 없는 요청 무시\n";
    return;
  }

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(rwDb());
    // 쓰기 커넥션인 이유 둘: vms_user·vms_session 은 guardx_reader 에서
    // 회수돼 있어 조회 커넥션으로는 못 읽고, check 가 만료 세션을 지운다.
    const auth::Result r = auth::check(tx, jgetStr(req, "token"));
    tx.commit();

    if (!r.ok) return replyAuthFail(reply, req_id, r.reason);

    mqttPublishReply(reply,
        "{\"node_id\":\"rpib\",\"req_id\":\"" + jesc(req_id) +
        "\",\"ok\":true,\"username\":\"" + jesc(r.username) +
        "\",\"display_name\":\"" + jesc(r.display_name) +
        "\",\"role\":\"" + r.role +
        "\",\"expires_at\":\"" + r.expires_at +
        "\",\"must_change\":" + (r.must_change ? "true" : "false") + "}");

  } catch (const std::exception& e) {
    replyAuthFail(reply, req_id, auth::reason::kExpired);
    std::cerr << "[auth] session_check 처리 실패: " << e.what() << "\n";
  }
}

/** @brief cmd/logout — {token} → {ok} (없는 토큰이어도 성공) */
void handleLogout(const std::string& req) {
  const std::string req_id = jgetStr(req, "req_id");
  const std::string reply  = jgetStr(req, "reply_to");
  if (req_id.empty() || reply.empty()) {
    std::cerr << "[auth] logout: req_id/reply_to 없는 요청 무시\n";
    return;
  }

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(rwDb());
    auth::logout(tx, jgetStr(req, "token"));
    tx.commit();
    replyOk(reply, req_id);
  } catch (const std::exception& e) {
    // 로그아웃 실패를 사용자에게 되돌려도 할 일이 없다 — 클라이언트는
    // 어차피 로컬 토큰을 버린다. 서버 세션이 남는 것은 만료가 처리한다.
    replyOk(reply, req_id);
    std::cerr << "[auth] logout 처리 실패(무시): " << e.what() << "\n";
  }
}

// requireAdmin 의 정의는 아래 작업 F 절에 있다. create_user 가 그걸 쓰는데
// 정의 순서상 여기가 앞이라 선언만 먼저 둔다 — 가드를 쓰기 명령 핸들러들과
// 같은 자리에 두는 편이 읽기 좋아서 옮기지 않았다.
bool requireAdmin(pqxx::work& tx, const std::string& req,
                  const std::string& reply, const std::string& req_id,
                  std::string& out_user);

/** @brief cmd/change_password — {token, old_password, new_password} → {ok, token, expires_at} */
void handleChangePassword(const std::string& req) {
  const std::string req_id = jgetStr(req, "req_id");
  const std::string reply  = jgetStr(req, "reply_to");
  if (req_id.empty() || reply.empty()) {
    std::cerr << "[auth] change_password: req_id/reply_to 없는 요청 무시\n";
    return;
  }

  // ⚠ jgetStr — 비밀번호에 따옴표·역슬래시가 들어갈 수 있다
  const std::string old_pw = jgetStr(req, "old_password");
  const std::string new_pw = jgetStr(req, "new_password");
  const std::string device = jgetStr(req, "device");

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(rwDb());
    const auth::Result r =
        auth::changePassword(tx, jgetStr(req, "token"), old_pw, new_pw, device);
    tx.commit();

    if (!r.ok) {
      replyAuthFail(reply, req_id, r.reason);
      std::cout << "[auth] change_password 실패 — " << r.reason << "\n";
      return;
    }

    // ⭐ 새 토큰을 반드시 실어 준다. 기존 세션을 전부 지웠으므로, 안 주면
    //    방금 바꾼 본인이 그 자리에서 튕긴다.
    mqttPublishReply(reply,
        "{\"node_id\":\"rpib\",\"req_id\":\"" + jesc(req_id) +
        "\",\"ok\":true,\"token\":\"" + r.token +
        "\",\"expires_at\":\"" + r.expires_at + "\"}");
    std::cout << "[auth] change_password " << r.username
              << " — 기존 세션 전부 무효화, 새 토큰 발급\n";

  } catch (const std::exception& e) {
    // 예외 문구를 요청자에게 주지 않는다 (로그인 3종과 같은 이유)
    replyAuthFail(reply, req_id, auth::reason::kBadCredentials);
    std::cerr << "[auth] change_password 처리 실패: " << e.what() << "\n";
  }
}

/** @brief cmd/create_user — admin 만. {token, username, display_name, role, password} */
void handleCreateUser(const std::string& req) {
  const std::string req_id = jgetStr(req, "req_id");
  const std::string reply  = jgetStr(req, "reply_to");
  if (req_id.empty() || reply.empty()) {
    std::cerr << "[auth] create_user: req_id/reply_to 없는 요청 무시\n";
    return;
  }

  const std::string username = jgetStr(req, "username");
  const std::string display  = jgetStr(req, "display_name");
  const std::string role     = jgetStr(req, "role");
  const std::string password = jgetStr(req, "password");

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(rwDb());

    // admin 확인. requireAdmin 이 거부 응답까지 보낸다.
    // ⚠ 과도기(REQUIRE_TOKEN=0)에도 토큰 없는 계정 생성은 허용되지만, 이건
    //   VMS 가 admin 화면에서만 부르는 명령이라 실질 위험이 낮다. 그래도
    //   REQUIRE_TOKEN=1 로 넘어가면 자동으로 막힌다.
    std::string actor;
    if (!requireAdmin(tx, req, reply, req_id, actor)) { tx.abort(); return; }

    const auth::Result r = auth::createUser(tx, username, display, role, password);
    tx.commit();

    if (!r.ok) {
      replyAuthFail(reply, req_id, r.reason);
      std::cout << "[auth] create_user 실패 " << username << " — " << r.reason << "\n";
      return;
    }

    mqttPublishReply(reply,
        "{\"node_id\":\"rpib\",\"req_id\":\"" + jesc(req_id) +
        "\",\"ok\":true,\"user_id\":" + std::to_string(r.user_id) + "}");
    // 비밀번호는 로그에 남기지 않는다. 누가 누구를 만들었는지까지가 한계다.
    std::cout << "[auth] create_user " << r.username << " (" << r.role
              << ") by " << (actor.empty() ? "(토큰없음)" : actor) << "\n";

  } catch (const std::exception& e) {
    replyAuthFail(reply, req_id, auth::reason::kDuplicate);
    std::cerr << "[auth] create_user 처리 실패: " << e.what() << "\n";
  }
}

/** @brief cmd/set_user_enabled — admin 만. {token, username, enabled} → {ok, username, enabled} */
void handleSetUserEnabled(const std::string& req) {
  const std::string req_id = jgetStr(req, "req_id");
  const std::string reply  = jgetStr(req, "reply_to");
  if (req_id.empty() || reply.empty()) {
    std::cerr << "[auth] set_user_enabled: req_id/reply_to 없는 요청 무시\n";
    return;
  }

  const std::string username = jgetStr(req, "username");
  const int en_tri = jboolTri(req, "enabled");
  // enabled 누락·비불리언은 규격 위반 — 기계 분기 대상이 아니라 개발 실수라
  // reason 이 아니라 error 로 답한다 (set_fire_threshold 의 검증과 같은 결).
  // 조용히 false 로 읽으면 오타 하나로 계정이 꺼진다.
  if (en_tri < 0)
    return replyErr(reply, req_id, "enabled 필드는 true/false 여야 함");
  if (username.empty())
    return replyErr(reply, req_id, "username 없음");

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(rwDb());

    // admin 확인. requireAdmin 이 거부 응답까지 보낸다.
    std::string actor;
    if (!requireAdmin(tx, req, reply, req_id, actor)) { tx.abort(); return; }

    const auth::Result r =
        auth::setUserEnabled(tx, actor, username, en_tri == 1);
    tx.commit();

    if (!r.ok) {
      replyAuthFail(reply, req_id, r.reason);
      std::cout << "[auth] set_user_enabled 실패 " << username << " — "
                << r.reason << "\n";
      return;
    }

    mqttPublishReply(reply,
        "{\"node_id\":\"rpib\",\"req_id\":\"" + jesc(req_id) +
        "\",\"ok\":true,\"username\":\"" + jesc(r.username) +
        "\",\"enabled\":" + (r.enabled ? "true" : "false") + "}");
    std::cout << "[auth] set_user_enabled " << username << " -> "
              << (r.enabled ? "활성" : "비활성") << " by "
              << (actor.empty() ? "(토큰없음)" : actor) << "\n";

  } catch (const std::exception& e) {
    // 예외 문구를 요청자에게 주지 않는다 (로그인 계열과 같은 이유). 로그에만.
    replyAuthFail(reply, req_id, auth::reason::kNotFound);
    std::cerr << "[auth] set_user_enabled 처리 실패: " << e.what() << "\n";
  }
}

/**
 * @brief cmd/set_site_config — admin 만. {token, site_name?, calibration?} → {ok}
 *
 * 전역 설정 부분 갱신(08-12, 계약 §3.3): 실린 키만 저장하고, 저장 후 새 상태를
 * TOPIC_SITECFG 로 retained 발행한다. calibration 은 서버가 내용을 해석하지
 * 않는 통짜 JSON 객체다 — 문법 검증은 $1::jsonb 캐스트가 한다(틀리면 예외).
 *
 * ⚠ calibration 을 먼저 떼어낸 나머지(stripped)로 봉투 필드를 뽑는다 —
 *   calibration 내부에 "token"·"reply_to" 같은 키가 있으면 평면 파서
 *   (jget/jgetStr)가 그쪽을 먼저 집을 수 있기 때문이다. requireAdmin 에도
 *   stripped 를 준다.
 */
void handleSetSiteConfig(const std::string& req) {
  size_t cal_b = 0, cal_e = 0;
  const std::string calibration = jgetObj(req, "calibration", &cal_b, &cal_e);
  std::string stripped = req;
  if (!calibration.empty()) stripped.erase(cal_b, cal_e - cal_b);

  const std::string req_id = jgetStr(stripped, "req_id");
  const std::string reply  = jgetStr(stripped, "reply_to");
  if (req_id.empty() || reply.empty()) {
    std::cerr << "[vms] set_site_config: req_id/reply_to 없는 요청 무시\n";
    return;
  }

  // 상한은 원본 전체 기준 (계약: 16 KB, reason 은 기계 분기 값이라 고정).
  if (req.size() > SITE_CONFIG_MAX_BYTES)
    return replyAuthFail(reply, req_id, "too_large");

  // "calibration" 키는 있는데 객체로 안 뽑혔다 = 값이 객체가 아니거나 끊긴
  // payload. 조용히 무시하면 "보냈는데 안 바뀐다"가 되므로 여기서 답한다.
  if (calibration.empty() && req.find("\"calibration\"") != std::string::npos)
    return replyErr(reply, req_id, "calibration 은 JSON 객체여야 함");

  const bool has_name = stripped.find("\"site_name\"") != std::string::npos;
  const std::string site_name =
      has_name ? jgetStr(stripped, "site_name") : std::string();
  if (!has_name && calibration.empty())
    return replyErr(reply, req_id, "site_name·calibration 둘 다 없음");

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(rwDb());

    // 작업 F
    std::string actor;
    if (!requireAdmin(tx, stripped, reply, req_id, actor)) { tx.abort(); return; }

    // 감사 표기는 set_fire_threshold 와 같은 규약 — 토큰이 있으면 사용자명,
    // 과도기 무토큰이면 VMS 가 보낸 기기명, 그마저 없으면 "vms".
    const std::string by = jget(stripped, "updated_by");
    const std::string by_final =
        !actor.empty() ? actor : (by.empty() ? std::string("vms") : by);

    if (has_name) {
      tx.exec(
          "INSERT INTO site_config (key, value, updated_by)"
          " VALUES ('site_name', to_jsonb($1::text), $2::text)"
          " ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value,"
          "   updated_at = now(), updated_by = EXCLUDED.updated_by",
          pqxx::params{site_name, by_final});
    }
    if (!calibration.empty()) {
      tx.exec(
          "INSERT INTO site_config (key, value, updated_by)"
          " VALUES ('calibration', $1::jsonb, $2::text)"
          " ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value,"
          "   updated_at = now(), updated_by = EXCLUDED.updated_by",
          pqxx::params{calibration, by_final});
    }

    // 새 값을 같은 트랜잭션에서 읽어 발행 payload 를 만든다 (set_zone 과 동일)
    const std::string cfg = querySiteConfig(tx);
    tx.commit();

    replyOk(reply, req_id);
    std::cout << "[vms] set_site_config"
              << (has_name ? " site_name" : "")
              << (!calibration.empty()
                      ? " calibration(" + std::to_string(calibration.size()) + "B)"
                      : "")
              << " by " << by_final << "\n";

    publishIfChanged(TOPIC_SITECFG, cfg, g_last_sitecfg);

  } catch (const std::exception& e) {
    // 진단이 필요한 쪽이라 set_zone 계열처럼 error 원문을 준다 — 여기는
    // 로그인 경로가 아니라서 계정 존재 여부가 샐 것이 없다.
    replyErr(reply, req_id, e.what());
    std::cerr << "[vms] set_site_config 실패: " << e.what() << "\n";
  }
}

/**
 * @brief 작업 F — 쓰기 명령의 토큰·역할 재검증. **진짜 방어선**
 *
 * VMS 의 버튼 잠금은 실수를 막을 뿐이다. UI 를 우회해 브로커에 직접 쏘는
 * 경우를 막는 것은 여기뿐이다.
 *
 * @param out_user 통과 시 사용자명 (감사 기록용). 과도기 무토큰 통과 시엔 빈 값
 * @return false 면 **이미 거부 응답을 보냈다** — 호출부는 그냥 return 하면 된다
 *
 * ── 과도기 규칙 (§6) ──
 * `REQUIRE_TOKEN=0` 일 때 관대해지는 것은 **토큰이 없는 경우뿐**이다.
 * 토큰이 실려 왔는데 틀렸다면 그건 언제나 거부다 — "없음"은 아직 배포가 안
 * 끝난 구버전 VMS 이고, "틀림"은 그렇게 설명되지 않는다.
 */
bool requireAdmin(pqxx::work& tx, const std::string& req,
                  const std::string& reply, const std::string& req_id,
                  std::string& out_user) {
  const std::string token = jgetStr(req, "token");

  if (token.empty()) {
    if (!g_require_token) {
      std::cerr << "[auth] ⚠ 토큰 없는 쓰기 명령 허용 (과도기) — "
                   "VMS 배포 확인 후 config.env 에 REQUIRE_TOKEN=1\n";
      out_user.clear();
      return true;
    }
    replyAuthFail(reply, req_id, auth::reason::kForbidden);
    return false;
  }

  const auth::Result r = auth::check(tx, token);
  if (!r.ok) {
    // 만료·비활성은 그 이유를 그대로 준다 — VMS 가 "다시 로그인" 화면으로
    // 보낼지 "권한 없음"을 띄울지 갈라야 하기 때문이다.
    replyAuthFail(reply, req_id, r.reason);
    return false;
  }

  // ⚠ 서버측 백스톱 (§5b 에 없는 값 — RPi B 가 추가). VMS 는 must_change 상태에서
  // 명령을 안 보내므로 정상 흐름에서는 여기 안 걸린다.
  //
  // 왜 넣었나: §5b 가 존재하는 이유가 "시드 비밀번호가 저장소에 공개돼 있다"인데,
  // 강제 변경을 UI 로만 막으면 **공개된 비밀번호로 로그인해 액추에이터를 그대로
  // 조작할 수 있다.** 그러면 §5b 는 보안이 아니라 화면 연출이 된다.
  // §0 의 "UI 잠금은 실수 방지, 진짜 방어선은 서버"가 여기에도 적용된다.
  if (r.must_change) {
    replyAuthFail(reply, req_id, auth::reason::kMustChangePw);
    std::cout << "[auth] 비밀번호 미변경 거부 " << r.username << "\n";
    return false;
  }

  if (r.role != "admin") {
    replyAuthFail(reply, req_id, auth::reason::kForbidden);
    std::cout << "[auth] 역할 거부 " << r.username << " (" << r.role << ")\n";
    return false;
  }

  out_user = r.username;
  return true;
}

/**
 * @brief 혼잡 경보 이력 요청 1건 처리 — VMS의 ALERT/REPORT 화면
 *
 * 요청 : {req_id, reply_to, hours(기본 24), limit(기본 100)}
 * 응답 : {node_id, req_id, ok, events: [[ts, ch, severity, source, msg, iid, status], …]}
 *
 * 원천은 alerts(발송 이력) — 전이마다 1행이라 "언제 무슨 일이 있었는가"가
 * 그대로 남는다. severity/status는 incidents의 **현재** 값이라 승급·해제가
 * 반영돼 있고, 발생 당시 표현은 message 문구가 가지고 있다.
 */
void handleIncidentHistory(const std::string& req) {
  const std::string req_id = jget(req, "req_id");
  const std::string reply  = jget(req, "reply_to");
  if (req_id.empty() || reply.empty()) {
    std::cerr << "[vms] incidents: req_id/reply_to 없는 요청 무시\n";
    return;
  }

  int hours = (int)jnum(req, "hours", 24);
  if (hours < 1)   hours = 1;
  if (hours > 720) hours = 720;      // 30일 — 그 이상은 리포트의 일이 아니다
  int limit = (int)jnum(req, "limit", 100);
  if (limit < 1)   limit = 1;
  if (limit > 500) limit = 500;      // payload 폭주 방지

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(roDb());
    pqxx::result r = tx.exec(
        "SELECT coalesce(json_agg(json_build_array("
        "         ts, ch, sev, src, msg, iid, st) ORDER BY ts DESC),"
        "       '[]'::json)::text"
        " FROM ("
        "   SELECT to_char(a.created_at AT TIME ZONE 'UTC',"
        "                  'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS ts,"
        "          z.channel AS ch, i.severity AS sev, i.source_type AS src,"
        "          a.message AS msg, i.incident_id AS iid, i.status AS st"
        "     FROM alerts a"
        "     JOIN incidents i USING (incident_id)"
        "     JOIN zones z USING (zone_id)"
        "    WHERE i.incident_type = 'congestion'"
        "      AND a.created_at > now() - make_interval(hours => $1::int)"
        "    ORDER BY a.created_at DESC"
        "    LIMIT $2::int"
        " ) t",
        pqxx::params{hours, limit});
    tx.commit();

    const std::string events = r.empty() ? "[]" : r[0][0].as<std::string>();
    const std::string out =
        "{\"node_id\":\"rpib\",\"req_id\":\"" + req_id + "\",\"ok\":true"
        ",\"hours\":" + std::to_string(hours) +
        ",\"events\":" + events + "}";
    mqttPublishReply(reply, out);
    std::cout << "[vms] incidents " << hours << "h 응답 " << out.size() << "B\n";

  } catch (const std::exception& e) {
    replyErr(reply, req_id, e.what());
    std::cerr << "[vms] incidents 실패: " << e.what() << "\n";
  }
}

/**
 * @brief 점유 시계열 요청 1건 처리 — VMS REPORT 차트의 6h/24h 이력
 *
 * 요청 : {req_id, reply_to, minutes, bucket_min}
 * 응답 : {node_id, req_id, ok, minutes, bucket_min,
 *         points: [[channel, s, v], ...]}
 *        s = "지금부터 몇 버킷 전" (0 = 최신), v = 버킷 내 person_count 평균
 *
 * 원천은 zone_occupancy (폴러가 카메라 /occupancy 분 중앙값을 60초마다
 * 멱등 upsert — task_occupancy). 빈 버킷은 행이 없다 → VMS가 0으로 채운다.
 * heatday와 같은 요청-응답 규약이며, 실패해도 반드시 응답을 보낸다.
 */
void handleOccseries(const std::string& req) {
  const std::string req_id = jget(req, "req_id");
  const std::string reply  = jget(req, "reply_to");
  if (req_id.empty() || reply.empty()) {
    std::cerr << "[vms] occseries: req_id/reply_to 없는 요청 무시\n";
    return;
  }

  // 범위 방어 — zone_occupancy 보존이 365일이라도 차트 용도는 하루가 상한
  int minutes = (int)jnum(req, "minutes", 360);
  if (minutes < 10)   minutes = 10;
  if (minutes > 1440) minutes = 1440;
  int bucket = (int)jnum(req, "bucket_min", 5);
  if (bucket < 1)  bucket = 1;
  if (bucket > 60) bucket = 60;

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(roDb());

    // zone_id -> channel 은 zones 조인으로 푼다 (1:1 아님 — queryZones 참조).
    // 캐스트 전부 명시 — heatday에서 배운 파라미터 타입 추론 실패 방지.
    pqxx::result r = tx.exec(
        "SELECT coalesce(json_agg(json_build_array(ch, s, v)), '[]'::json)::text"
        " FROM ("
        "   SELECT z.channel AS ch,"
        "          floor(extract(epoch FROM (now() - o.bucket_ts))"
        "                / ($1::int * 60))::int AS s,"
        "          round(avg(o.person_count)::numeric, 1) AS v"
        "   FROM zone_occupancy o JOIN zones z USING (zone_id)"
        "   WHERE o.bucket_ts > now() - make_interval(mins => $2::int)"
        "   GROUP BY 1, 2"
        " ) t",
        pqxx::params{bucket, minutes});
    tx.commit();

    const std::string points = r.empty() ? "[]" : r[0][0].as<std::string>();
    const std::string out =
        "{\"node_id\":\"rpib\",\"req_id\":\"" + req_id + "\",\"ok\":true"
        ",\"minutes\":" + std::to_string(minutes) +
        ",\"bucket_min\":" + std::to_string(bucket) +
        ",\"points\":" + points + "}";
    mqttPublishReply(reply, out);
    std::cout << "[vms] occseries " << minutes << "m/" << bucket
              << "m 응답 " << out.size() << "B\n";

  } catch (const std::exception& e) {
    replyErr(reply, req_id, e.what());
    std::cerr << "[vms] occseries 실패: " << e.what() << "\n";
  }
}

/**
 * @brief 센서 이력 백필 — VMS의 실시간 그래프가 켜지자마자 최근 N분을 채운다
 *
 * 요청 : {req_id, reply_to, minutes(기본 10, 1~60)}
 * 응답 : {node_id, req_id, ok, minutes, points: [[channel, s, v, ok], ...]}
 *        s = 지금부터 몇 초 전(내림차순, 0=최신), ok = is_valid.
 *
 * occseries와 같은 요청-응답 규약이지만 버킷 평균을 안 낸다 — 센서는 이미
 * ~1Hz라 원시 포인트를 그대로 내보내도 양이 많지 않다(10분×6채널≈3600행).
 * 이 시점 이후의 갱신은 TOPIC_SENSORS 구독만으로 충분하다 — 이 토픽은
 * "VMS 켜지기 전 이력"만 메운다.
 */
// Handles VMS business-flow analytics requests.
// Source table: trajectory_segments inserted by guardx_poller from
// CAP /analytics/trajectories.
void handleTrajectoryAnalytics(const std::string& req) {
  const std::string req_id = jget(req, "req_id");
  const std::string reply  = jget(req, "reply_to");
  if (req_id.empty() || reply.empty()) {
    std::cerr << "[vms] trajectory: req_id/reply_to 없는 요청 무시\n";
    return;
  }

  int hours = (int)jnum(req, "hours", 24);
  if (hours < 1)   hours = 1;
  if (hours > 720) hours = 720;

  int limit = (int)jnum(req, "limit", 8);
  if (limit < 1)  limit = 1;
  if (limit > 20) limit = 20;

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(roDb());

    const pqxx::result coverage_result = tx.exec(
        "SELECT json_build_object("
        "  'reliable_count', count(*) FILTER (WHERE is_reliable),"
        "  'total_count', count(*),"
        "  'avg_confidence', coalesce(round(avg(confidence)::numeric, 3), 0)"
        ")::text"
        " FROM trajectory_segments"
        " WHERE segment_ts > now() - make_interval(hours => $1::int)",
        pqxx::params{hours});

    const pqxx::result dwell_result = tx.exec(
        "SELECT coalesce(json_agg(json_build_object("
        "  'zone_id', zone_id,"
        "  'visit_count', visit_count,"
        "  'avg_dwell_ms', avg_dwell_ms,"
        "  'total_dwell_ms', total_dwell_ms,"
        "  'avg_confidence', avg_confidence"
        ") ORDER BY total_dwell_ms DESC), '[]'::json)::text"
        " FROM ("
        "   SELECT zone_id,"
        "          count(*) AS visit_count,"
        "          round(avg(dwell_ms)::numeric, 1) AS avg_dwell_ms,"
        "          sum(dwell_ms) AS total_dwell_ms,"
        "          round(avg(confidence)::numeric, 3) AS avg_confidence"
        "   FROM trajectory_segments"
        "   WHERE is_reliable = true"
        "     AND zone_id IS NOT NULL"
        "     AND segment_ts > now() - make_interval(hours => $1::int)"
        "   GROUP BY zone_id"
        "   ORDER BY total_dwell_ms DESC"
        "   LIMIT $2::int"
        " ) t",
        pqxx::params{hours, limit});

    const pqxx::result transition_result = tx.exec(
        "WITH base_segments AS ("
        "   SELECT camera_id,"
        "          segment_id,"
        "          global_id,"
        "          zone_id,"
        "          start_ms,"
        "          end_ms,"
        "          confidence,"
        "          segment_ts"
        "   FROM trajectory_segments"
        "   WHERE is_reliable = true"
        "     AND zone_id IS NOT NULL"
        "     AND segment_ts > now() - make_interval(hours => $1::int)"
        " ), ordered_segments AS ("
        "   SELECT *,"
        "          lead(zone_id) OVER ("
        "              PARTITION BY camera_id, global_id"
        "              ORDER BY start_ms"
        "          ) AS next_zone_id"
        "   FROM base_segments"
        " ), confirmed_summary AS ("
        "   SELECT zone_id AS from_zone_id,"
        "          next_zone_id AS to_zone_id,"
        "          count(*) AS confirmed_count"
        "   FROM ordered_segments"
        "   WHERE next_zone_id IS NOT NULL"
        "     AND zone_id <> next_zone_id"
        "   GROUP BY zone_id, next_zone_id"
        " ), estimated_candidates AS ("
        "   SELECT a.camera_id,"
        "          a.segment_id AS from_segment_id,"
        "          a.start_ms AS from_start_ms,"
        "          b.segment_id AS to_segment_id,"
        "          b.start_ms AS to_start_ms,"
        "          a.zone_id AS from_zone_id,"
        "          b.zone_id AS to_zone_id,"
        "          (b.start_ms - a.end_ms) AS gap_ms,"
        "          round(("
        "              least(a.confidence, b.confidence)::numeric * 0.70"
        "              + greatest(0.0, 1.0 - ((b.start_ms - a.end_ms)::double precision / 5000.0))::numeric * 0.30"
        "          ), 3) AS match_score,"
        "          row_number() OVER ("
        "              PARTITION BY a.camera_id, a.segment_id, a.start_ms"
        "              ORDER BY "
        "                  (least(a.confidence, b.confidence) * 0.70"
        "                   + greatest(0.0, 1.0 - ((b.start_ms - a.end_ms)::double precision / 5000.0)) * 0.30) DESC,"
        "                  (b.start_ms - a.end_ms) ASC"
        "          ) AS rn"
        "   FROM base_segments a"
        "   JOIN base_segments b"
        "     ON b.camera_id = a.camera_id"
        "    AND b.global_id <> a.global_id"
        "    AND b.zone_id <> a.zone_id"
        "    AND b.start_ms >= a.end_ms"
        "    AND b.start_ms - a.end_ms BETWEEN 0 AND 5000"
        "   WHERE NOT EXISTS ("
        "       SELECT 1"
        "       FROM ordered_segments os"
        "       WHERE os.camera_id = a.camera_id"
        "         AND os.segment_id = a.segment_id"
        "         AND os.start_ms = a.start_ms"
        "         AND os.next_zone_id IS NOT NULL"
        "         AND os.next_zone_id <> os.zone_id"
        "   )"
        " ), estimated_source_best AS ("
        "   SELECT *"
        "   FROM estimated_candidates"
        "   WHERE rn = 1"
        "     AND match_score >= 0.60"
        " ), estimated_unique AS ("
        "   SELECT *,"
        "          row_number() OVER ("
        "              PARTITION BY camera_id, to_segment_id, to_start_ms"
        "              ORDER BY match_score DESC, gap_ms ASC"
        "          ) AS target_rn"
        "   FROM estimated_source_best"
        " ), estimated_summary AS ("
        "   SELECT from_zone_id,"
        "          to_zone_id,"
        "          count(*) AS estimated_count,"
        "          round(avg(match_score)::numeric, 3) AS avg_score"
        "   FROM estimated_unique"
        "   WHERE target_rn = 1"
        "   GROUP BY from_zone_id, to_zone_id"
        " ), summarized AS ("
        "   SELECT coalesce(c.from_zone_id, e.from_zone_id) AS from_zone_id,"
        "          coalesce(c.to_zone_id, e.to_zone_id) AS to_zone_id,"
        "          coalesce(c.confirmed_count, 0) AS confirmed_count,"
        "          coalesce(e.estimated_count, 0) AS estimated_count,"
        "          coalesce(c.confirmed_count, 0) + coalesce(e.estimated_count, 0) AS transition_count,"
        "          coalesce(e.avg_score, 0) AS avg_score"
        "   FROM confirmed_summary c"
        "   FULL OUTER JOIN estimated_summary e"
        "     ON e.from_zone_id = c.from_zone_id"
        "    AND e.to_zone_id = c.to_zone_id"
        "   ORDER BY transition_count DESC"
        "   LIMIT $2::int"
        " )"
        " SELECT coalesce(json_agg(json_build_object("
        "   'from_zone_id', from_zone_id,"
        "   'to_zone_id', to_zone_id,"
        "   'transition_count', transition_count,"
        "   'confirmed_count', confirmed_count,"
        "   'estimated_count', estimated_count,"
        "   'avg_score', avg_score"
        " ) ORDER BY transition_count DESC), '[]'::json)::text"
        " FROM summarized",
        pqxx::params{hours, limit});

    tx.commit();

    const std::string coverage = coverage_result.empty()
        ? "{\"reliable_count\":0,\"total_count\":0,\"avg_confidence\":0}"
        : coverage_result[0][0].as<std::string>();
    const std::string dwell = dwell_result.empty()
        ? "[]"
        : dwell_result[0][0].as<std::string>();
    const std::string transitions = transition_result.empty()
        ? "[]"
        : transition_result[0][0].as<std::string>();

    const std::string out =
        "{\"node_id\":\"rpib\",\"req_id\":\"" + req_id + "\",\"ok\":true"
        ",\"hours\":" + std::to_string(hours) +
        ",\"coverage\":" + coverage +
        ",\"dwell_summary\":" + dwell +
        ",\"transition_summary\":" + transitions + "}";
    mqttPublishReply(reply, out);
    std::cout << "[vms] trajectory " << hours << "h 응답 "
              << out.size() << "B\n";

  } catch (const std::exception& e) {
    replyErr(reply, req_id, e.what());
    std::cerr << "[vms] trajectory 실패: " << e.what() << "\n";
  }
}

void handleSensorHistory(const std::string& req) {
  const std::string req_id = jget(req, "req_id");
  const std::string reply  = jget(req, "reply_to");
  if (req_id.empty() || reply.empty()) {
    std::cerr << "[vms] sensor_history: req_id/reply_to 없는 요청 무시\n";
    return;
  }

  int minutes = (int)jnum(req, "minutes", 10);
  if (minutes < 1)  minutes = 1;
  if (minutes > 60) minutes = 60;

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(roDb());
    // PHASE 7: 각 점에 zone_id를 앞세운다 — [zone, ch, s, v, ok].
    // COMPARE 화면이 여러 zone의 그래프를 한 번에 백필해야 해서, zone마다
    // 요청을 나누지 않고 한 응답에 담는다(요청 N번이면 응답 순서가 엇갈려
    // 그래프가 뒤섞일 수 있다 — heatday를 하루 단위 통짜로 준 것과 같은 이유).
    pqxx::result r = tx.exec(
        "SELECT coalesce(json_agg(json_build_array(zone, ch, s, v, ok) ORDER BY s DESC),"
        "                 '[]'::json)::text"
        " FROM ("
        "   SELECT sr.zone_id AS zone,"
        "          c.channel_key AS ch,"
        "          extract(epoch FROM (now() - sr.received_at))::int AS s,"
        "          sv.value AS v,"
        "          sv.is_valid AS ok"
        "   FROM sensor_value sv"
        "   JOIN sensor_reading sr USING (reading_id)"
        "   JOIN sensor_channel c USING (channel_id)"
        "   WHERE sr.received_at > now() - make_interval(mins => $1::int)"
        " ) t",
        pqxx::params{minutes});
    tx.commit();

    const std::string points = r.empty() ? "[]" : r[0][0].as<std::string>();
    const std::string out =
        "{\"node_id\":\"rpib\",\"req_id\":\"" + req_id + "\",\"ok\":true"
        ",\"minutes\":" + std::to_string(minutes) +
        ",\"points\":" + points + "}";
    mqttPublishReply(reply, out);
    std::cout << "[vms] sensor_history " << minutes << "m 응답 " << out.size() << "B\n";

  } catch (const std::exception& e) {
    replyErr(reply, req_id, e.what());
    std::cerr << "[vms] sensor_history 실패: " << e.what() << "\n";
  }
}

/**
 * @brief 구역 임계 변경 (VMS -> DB)
 *
 * VMS가 DB에 직접 붙지 않고도 정원을 고칠 수 있게 하는 유일한 경로다.
 * UPDATE 직후 zones를 *즉시* 재발행하므로 VMS는 다음 틱(기본 30초)을
 * 기다리지 않고 1초 안에 화면이 바뀐다 — 그 자체가 성공 확인이 된다.
 *
 * ⚠ 쓰기는 읽기보다 위험하다. 브로커가 현재 평문·익명 허용이라 같은 LAN의
 * 누구든 이 토픽에 쏠 수 있다. 최소한의 값 검증을 여기서 반드시 한다.
 * (브로커 ACL 또는 mTLS 2단계 전환 전까지의 유일한 방어선)
 */
void handleSetZone(const std::string& req) {
  const std::string req_id = jget(req, "req_id");
  const std::string reply  = jget(req, "reply_to");
  if (reply.empty()) {
    std::cerr << "[vms] set_zone: reply_to 없는 요청 무시\n";
    return;
  }

  const int    zone_id = (int)jnum(req, "zone_id", -1);
  const int    cap     = (int)jnum(req, "capacity_limit", -1);
  const double warn    = jnum(req, "warn_ratio", -1);
  const double crit    = jnum(req, "critical_ratio", -1);
  // 선택 항목 — 없으면 이름은 건드리지 않는다. 이름 필드를 모르는 옛 VMS가
  // 정원만 고치러 와도 이름이 지워지지 않아야 한다.
  const std::string zname = jget(req, "zone_name");

  // ── 검증 ──
  if (zone_id <= 0)
    return replyErr(reply, req_id, "zone_id 누락 또는 잘못됨");
  if (cap < CAP_MIN || cap > CAP_MAX)
    return replyErr(reply, req_id,
                    "capacity_limit 범위 밖 (" + std::to_string(CAP_MIN) +
                    "~" + std::to_string(CAP_MAX) + ")");
  if (warn <= 0.0 || warn >= 1.0)
    return replyErr(reply, req_id, "warn_ratio 는 0 초과 1 미만이어야 함");
  if (crit <= warn || crit > 1.0)
    return replyErr(reply, req_id, "critical_ratio 는 warn_ratio 초과 1 이하여야 함");
  if (!zname.empty()) {
    if (zname.size() > NAME_MAX_BYTES)
      return replyErr(reply, req_id,
                      "zone_name 이 너무 김 (최대 " +
                      std::to_string(NAME_MAX_BYTES) + " 바이트)");
    // jget은 첫 따옴표까지만 읽는 평면 파서다 — 따옴표가 든 이름은 여기 닿기
    // 전에 이미 잘려 있다. 역슬래시도 같은 이유로 막는다. VMS가 먼저 거르지만
    // 브로커가 익명 허용이라 아무나 쏠 수 있으므로 여기가 실질 방어선이다.
    if (zname.find('"') != std::string::npos ||
        zname.find('\\') != std::string::npos)
      return replyErr(reply, req_id, "zone_name 에 따옴표·역슬래시는 쓸 수 없음");
  }

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(rwDb());

    // 작업 F — UI 잠금을 우회한 요청은 여기서 막힌다
    std::string actor;
    if (!requireAdmin(tx, req, reply, req_id, actor)) { tx.abort(); return; }

    pqxx::result up = tx.exec(
        "UPDATE zone_thresholds"
        "   SET capacity_limit = $2::int,"
        "       warn_ratio     = $3::real,"
        "       critical_ratio = $4::real,"
        "       updated_at     = now()"
        " WHERE zone_id = $1::int",
        pqxx::params{zone_id, cap, warn, crit});

    if (up.affected_rows() == 0) {
      tx.abort();
      return replyErr(reply, req_id,
                      "zone_id " + std::to_string(zone_id) + " 없음");
    }

    // 이름은 zone_thresholds가 아니라 zones에 있다 — 같은 트랜잭션의 두 번째
    // UPDATE다. 하나만 성공하고 끝나는 상태가 없어야 한다.
    if (!zname.empty())
      tx.exec("UPDATE zones SET zone_name = $2::text WHERE zone_id = $1::int",
              pqxx::params{zone_id, zname});

    // 새 값을 같은 트랜잭션에서 읽어 발행 payload를 만든다 — commit 전에
    // 읽으므로 방금 쓴 값이 그대로 반영된다.
    const std::string zones = queryZones(tx);
    // 정원이 바뀌면 같은 인원도 다른 단계가 된다 — 열린 incident 목록도 함께
    // 다시 내보내 VMS가 옛 단계를 붙들고 있지 않게 한다.
    const std::string incidents = queryOpenIncidents(tx);
    tx.commit();

    replyOk(reply, req_id);
    std::cout << "[vms] set_zone zone " << zone_id << " -> cap " << cap
              << " warn " << warn << " crit " << crit << "\n";

    // 다음 틱을 기다리지 않고 즉시 재발행 — VMS 화면이 바로 바뀐다
    publishIfChanged(TOPIC_ZONES, zones, g_last_zones);
    publishIfChanged(TOPIC_INCIDENTS, incidents, g_last_incidents);

    // 새 임계로 즉시 재판정하라고 폴러에 알린다 — 폴러가 다음 1초 틱에
    // 소화한다. 없으면 다음 alert 틱까지 옛 단계가 남는다 ("임계 바꿨는데
    // 반응 없음"). 폴러가 꺼져 있으면 이 신호는 그냥 사라지는데, 그때는
    // 재판정할 대상 자체가 없으므로 유실돼도 무해하다.
    mqttPublishEvent(TOPIC_ZONE_CHANGED,
        "{\"node_id\":\"rpib\",\"zone_id\":" + std::to_string(zone_id) + "}");

  } catch (const std::exception& e) {
    replyErr(reply, req_id, e.what());
    std::cerr << "[vms] set_zone 실패: " << e.what() << "\n";
  }
}


/**
 * @brief 액추에이터 수동 제어 (VMS -> RPi C, RPi B 경유) — device_control_page.cpp
 *
 * 요청 : {req_id, reply_to, command_key, action, value?}
 * 응답 : {node_id, req_id, ok, error?}
 *
 * fire_schema.sql manual_command 주석의 "설계 확정: VMS→B→C" 원칙의 유일한
 * 쓰기 경로다. set_zone과 순서가 다르다 — 거긴 발행이 성공 확인 그 자체라
 * commit 후 곧장 재발행하지만, 여긴 **manual_command 기록(감사 로그)이
 * "명령을 접수했다"의 정의**라 DB commit을 먼저 하고 그 다음에 실제
 * guardx/actuator/rpic로 내보낸다. RPi C에 ACK 토픽이 없어(guardx_protocol.h,
 * "미사용") VMS는 "RPi B가 접수했다"까지만 확인할 수 있다 — 실제 동작
 * 성공 여부는 모른다.
 *
 * command_key 검증은 actuator_command 카탈로그 조회로 한다(하드코딩 목록을
 * 여기 또 두지 않는다 — VMS의 ACTUATOR_FIELDS와 두 곳이 따로 놀면 어긋난다).
 * kind별 action 검증은 onoff/set만 좁히고, both는 DB CHECK가 허용하는 6개
 * 전부를 통과시킨다 — shutter처럼 kind='both'라도 실제로는 OPEN/CLOSE/STOP만
 * 유효한 경우가 있어(fire_schema.sql 주석 "미확정") 그 이상의 세분화는
 * 카탈로그에 열이 추가되기 전까지 여기서 판단할 근거가 없다.
 *
 * PHASE 6: manual_command.zone_id가 NOT NULL이 됐다(fire_schema.sql). 이
 * VMS는 아직 zone을 하나만 다룬다(TOPIC_ACTUATOR_RPIC이 "rpic"에 고정) —
 * 그렇다고 zone_id를 리터럴 1로 박으면 fire_zone 시드가 바뀌는 순간 조용히
 * 어긋난다. 대신 TOPIC_ACTUATOR_RPIC이 향하는 노드("rpic")로 fire_zone을
 * 역조회해 zone_id를 구한다 — rpib_engine의 main.c가 node_id로 zone을 찾는
 * 것과 같은 원칙. zone별 UI(요청 안에 zone_id를 싣는 것)는 여전히 범위 밖.
 *
 * ⚠ 브로커가 익명 허용이라 여기가 실질 방어선이다 (set_zone과 같은 이유).
 *   이 경로는 물리 액추에이터를 직접 움직이므로 set_zone·set_fire보다
 *   위험이 크다.
 */
void handleSetActuator(const std::string& req) {
  const std::string req_id = jget(req, "req_id");
  const std::string reply  = jget(req, "reply_to");
  if (reply.empty()) {
    std::cerr << "[vms] set_actuator: reply_to 없는 요청 무시\n";
    return;
  }

  const std::string command_key = jget(req, "command_key");
  const std::string action      = jget(req, "action");
  // jget은 문자열 값 전용이라 value(숫자)는 jnum으로 따로 뽑되, SET이 아닌
  // action에 실수로 value가 섞여 들어와도 무시하도록 존재 여부만 별도로 본다.
  const bool has_value = req.find("\"value\"") != std::string::npos;
  const int value = (int)jnum(req, "value", 0);

  if (command_key.empty())
    return replyErr(reply, req_id, "command_key 누락");
  if (action != "ON" && action != "OFF" && action != "SET" &&
      action != "OPEN" && action != "CLOSE" && action != "STOP")
    return replyErr(reply, req_id, "action 값이 올바르지 않음");
  if (action == "SET" && !has_value)
    return replyErr(reply, req_id, "SET은 value가 필요함");

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(rwDb());

    // 작업 F — 액추에이터는 실동작(펌프·셔터·경보음)이 걸린 명령이다.
    // 넷 중 오남용 대가가 가장 크므로 여기가 가장 중요한 자리다.
    std::string actor;
    if (!requireAdmin(tx, req, reply, req_id, actor)) { tx.abort(); return; }

    pqxx::result cat = tx.exec(
        "SELECT command_id, kind FROM actuator_command"
        " WHERE command_key = $1::text",
        pqxx::params{command_key});
    if (cat.empty()) {
      tx.abort();
      return replyErr(reply, req_id,
                      "command_key '" + command_key + "' 없음 (카탈로그 미등록)");
    }
    const int command_id = cat[0][0].as<int>();
    const std::string kind = cat[0][1].as<std::string>();

    if (kind == "onoff" && action != "ON" && action != "OFF") {
      tx.abort();
      return replyErr(reply, req_id, "'" + command_key + "'는 ON/OFF만 가능");
    }
    if (kind == "set" && action != "SET") {
      tx.abort();
      return replyErr(reply, req_id, "'" + command_key + "'는 SET만 가능");
    }

    // manual_command.zone_id(NOT NULL) — ACTUATOR_RPIC_NODE_ID가 실제로 향하는
    // zone을 fire_zone에서 찾는다. 하드코딩 리터럴 대신 역조회하는 이유는 위
    // 함수 docblock 참조.
    pqxx::result zr = tx.exec(
        "SELECT zone_id FROM fire_zone WHERE rpic_node_id = $1::text",
        pqxx::params{std::string(ACTUATOR_RPIC_NODE_ID)});
    if (zr.empty()) {
      tx.abort();
      return replyErr(reply, req_id,
                      "fire_zone에 rpic_node_id='" +
                          std::string(ACTUATOR_RPIC_NODE_ID) + "' 없음 (설정 오류)");
    }
    const int zone_id = zr[0][0].as<int>();

    const long seq = ++g_actuator_seq;
    const std::optional<int> db_value =
        action == "SET" ? std::optional<int>(value) : std::nullopt;

    tx.exec(
        "INSERT INTO manual_command"
        "  (zone_id, command_id, action, value, source, published_seq)"
        " VALUES ($1::smallint, $2::smallint, $3::text, $4::int, 'vms', $5::bigint)",
        pqxx::params{zone_id, command_id, action, db_value, seq});
    tx.commit();

    replyOk(reply, req_id);
    std::cout << "[vms] set_actuator " << command_key << " " << action
              << (action == "SET" ? (" value=" + std::to_string(value)) : std::string())
              << " seq=" << seq << "\n";

    // DB 기록이 끝난 뒤에만 실제로 내보낸다 — 감사 로그 없이 물리 명령이
    // 나가는 경우가 없어야 한다.
    using namespace std::chrono;
    const int64_t now_ms =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    std::string payload =
        "{\"node_id\":\"" + std::string(ACTUATOR_RPIC_NODE_ID) + "\",\"timestamp\":" + std::to_string(now_ms) +
        ",\"seq\":" + std::to_string(seq) +
        ",\"command\":\"" + command_key + "\",\"action\":\"" + action + "\"";
    if (action == "SET") payload += ",\"value\":" + std::to_string(value);
    payload += "}";
    mqttPublishEvent(TOPIC_ACTUATOR_RPIC, payload);

  } catch (const std::exception& e) {
    replyErr(reply, req_id, e.what());
    std::cerr << "[vms] set_actuator 실패: " << e.what() << "\n";
  }
}


/**
 * @brief 화재 임계 변경 (VMS -> DB) — 스냅샷 통째로 새 행
 *
 * 스키마가 "1행 = 설정 스냅샷 전체" 라(fire_schema.sql) 부분 수정 개념이
 * 없다. 22개 값을 다 받아 새 행을 넣고 is_active 를 넘긴다. is_active 부분
 * 유니크 인덱스 때문에 비활성화와 삽입이 **한 트랜잭션**이어야 한다.
 *
 * 검증을 여기서 먼저 하는 이유: DB CHECK 에 걸리면 Postgres 원문 에러
 * ("new row violates check constraint chk_weight_sum")가 그대로 VMS 화면에
 * 뜬다. 어느 값이 왜 틀렸는지 사람이 읽을 수 있어야 한다.
 *
 * ⚠ 브로커가 익명 허용이라 여기가 실질 방어선이다 (set_zone 과 같은 이유).
 *   게다가 이 값은 화재 판단 기준이라 잘못 들어가면 경보가 안 울린다.
 */
void handleSetFireThreshold(const std::string& req) {
  const std::string req_id = jget(req, "req_id");
  const std::string reply  = jget(req, "reply_to");
  if (reply.empty()) {
    std::cerr << "[vms] set_fire_threshold: reply_to 없는 요청 무시\n";
    return;
  }

  // 없으면 -1 로 떨어져 아래 범위 검증에 걸린다 — 누락과 오입력을 같은
  // 경로로 처리한다 (기본값을 몰래 채워 넣으면 절반만 바뀐 설정이 활성화된다)
  const double gas_min   = jnum(req, "gas_raw_min", -1);
  const double gas_max   = jnum(req, "gas_raw_max", -1);
  const double spark_safe   = jnum(req, "spark_raw_safe", -1);
  const double spark_danger = jnum(req, "spark_raw_danger", -1);
  const double temp_min  = jnum(req, "temp_min_c", -999);
  const double temp_max  = jnum(req, "temp_max_c", -999);
  const double humi_safe   = jnum(req, "humi_safe_percent", -1);
  const double humi_danger = jnum(req, "humi_danger_percent", -1);
  const double ir_min    = jnum(req, "irtemp_min_c", -999);
  const double ir_max    = jnum(req, "irtemp_max_c", -999);
  const double w_gas     = jnum(req, "weight_gas", -1);
  const double w_spark   = jnum(req, "weight_spark", -1);
  const double w_temp    = jnum(req, "weight_temp", -1);
  const double w_humi    = jnum(req, "weight_humi", -1);
  const double w_ir      = jnum(req, "weight_irtemp", -1);
  const double score     = jnum(req, "fire_score_threshold", -1);
  const int    n_confirm = (int)jnum(req, "n_confirm", -1);
  const int    n_recover = (int)jnum(req, "n_recover", -1);
  const int    freeze    = (int)jnum(req, "freeze_relax_cycles", -1);
  const double min_valid = jnum(req, "min_valid_weight", -1);
  const double ov_spark  = jnum(req, "override_spark_score", -1);
  const double ov_ir     = jnum(req, "override_irtemp_score", -1);
  const std::string by   = jget(req, "updated_by");

  // ── 검증: DB CHECK 와 같은 조건을 사람 말로 ──
  if (gas_min >= gas_max)
    return replyErr(reply, req_id, "gas_raw_min 은 gas_raw_max 보다 작아야 함");
  // 스파크는 내림차순 퍼지화 — safe 가 더 크다 (fire_schema.sql 주석)
  if (spark_safe <= spark_danger)
    return replyErr(reply, req_id, "spark_raw_safe 는 spark_raw_danger 보다 커야 함");
  if (temp_min >= temp_max)
    return replyErr(reply, req_id, "temp_min_c 는 temp_max_c 보다 작아야 함");
  if (ir_min >= ir_max)
    return replyErr(reply, req_id, "irtemp_min_c 는 irtemp_max_c 보다 작아야 함");
  // 습도도 내림차순 — 습하면 안전
  if (humi_safe <= humi_danger)
    return replyErr(reply, req_id,
                    "humi_safe_percent 는 humi_danger_percent 보다 커야 함");

  const double w_sum = w_gas + w_spark + w_temp + w_humi + w_ir;
  if (w_gas < 0 || w_spark < 0 || w_temp < 0 || w_humi < 0 || w_ir < 0)
    return replyErr(reply, req_id, "가중치는 음수일 수 없음");
  if (std::fabs(w_sum - 1.0) >= 0.001)
    return replyErr(reply, req_id,
                    "가중치 합이 1.0 이어야 함 (현재 " +
                    std::to_string(w_sum) + ")");

  if (score < 0 || score > 100)
    return replyErr(reply, req_id, "fire_score_threshold 는 0~100");
  if (ov_spark < 0 || ov_spark > 100 || ov_ir < 0 || ov_ir > 100)
    return replyErr(reply, req_id, "override_* 점수는 0~100");
  if (min_valid <= 0 || min_valid > 1)
    return replyErr(reply, req_id, "min_valid_weight 는 0 초과 1 이하");
  if (n_confirm <= 0 || n_recover <= 0 || freeze <= 0)
    return replyErr(reply, req_id,
                    "n_confirm · n_recover · freeze_relax_cycles 는 1 이상");

  try {
    std::lock_guard<std::mutex> lk(g_qmtx);
    pqxx::work tx(rwDb());

    // 작업 F
    std::string actor;
    if (!requireAdmin(tx, req, reply, req_id, actor)) { tx.abort(); return; }

    // 감사(§6): updated_by 에 **사용자명**을 남긴다. 지금까지는 VMS 가 보낸
    // 기기명뿐이라 "어느 PC 에서"까지만 알 수 있었다. 과도기 무토큰 통과일
    // 때는 actor 가 비므로 기존 값(기기명)을 그대로 쓴다.
    const std::string by_final = actor.empty() ? by : actor;

    // is_active 부분 유니크 인덱스(uq_fire_threshold_active) 때문에 순서가
    // 중요하다 — 먼저 비활성화하지 않고 INSERT 하면 유니크 위반으로 죽는다.
    tx.exec("UPDATE fire_threshold SET is_active = FALSE WHERE is_active");

    tx.exec(
        "INSERT INTO fire_threshold ("
        "  gas_raw_min, gas_raw_max, spark_raw_safe, spark_raw_danger,"
        "  temp_min_c, temp_max_c, humi_safe_percent, humi_danger_percent,"
        "  irtemp_min_c, irtemp_max_c,"
        "  weight_gas, weight_spark, weight_temp, weight_humi, weight_irtemp,"
        "  fire_score_threshold, n_confirm, n_recover, freeze_relax_cycles,"
        "  min_valid_weight, override_spark_score, override_irtemp_score,"
        "  is_active, updated_by)"
        " VALUES ($1::real,$2::real,$3::real,$4::real,"
        "         $5::real,$6::real,$7::real,$8::real,"
        "         $9::real,$10::real,"
        "         $11::real,$12::real,$13::real,$14::real,$15::real,"
        "         $16::real,$17::int,$18::int,$19::int,"
        "         $20::real,$21::real,$22::real,"
        "         TRUE, $23::text)",
        pqxx::params{gas_min, gas_max, spark_safe, spark_danger,
                     temp_min, temp_max, humi_safe, humi_danger,
                     ir_min, ir_max,
                     w_gas, w_spark, w_temp, w_humi, w_ir,
                     score, n_confirm, n_recover, freeze,
                     min_valid, ov_spark, ov_ir,
                     by_final.empty() ? std::string("vms") : by_final});

    // 새 값을 같은 트랜잭션에서 읽어 발행 payload 를 만든다 (set_zone 과 동일)
    const std::string fire = queryFireThreshold(tx);
    tx.commit();

    replyOk(reply, req_id);
    std::cout << "[vms] set_fire_threshold score " << score
              << " n_confirm " << n_confirm << " weights "
              << w_gas << "/" << w_spark << "/" << w_temp << "/"
              << w_humi << "/" << w_ir << "\n";

    publishIfChanged(TOPIC_FIRE, fire, g_last_fire);

    // ⚠ 이 신호가 없으면 DB만 바뀌고 화재 엔진은 옛 임계로 계속 판단한다.
    // 엔진은 페이로드를 보지 않고 "오면 다시 읽는다"만 한다.
    mqttPublishEvent(TOPIC_CONFIG_RELOAD, "reload");
    std::cout << "[vms] 화재 엔진 리로드 신호 발행 — " << TOPIC_CONFIG_RELOAD << "\n";

  } catch (const std::exception& e) {
    replyErr(reply, req_id, e.what());
    std::cerr << "[vms] set_fire_threshold 실패: " << e.what() << "\n";
  }
}

}  // namespace

void publishVmsState(pqxx::connection& db) {
  try {
    pqxx::work tx(db);

    // ── zones: OCC 배지의 분모(정원)와 색 임계 ──
    const std::string zones = queryZones(tx);

    // ── incidents: 열려 있는 혼잡 경보 (VMS 재접속 시 현재 단계 복원) ──
    const std::string incidents = queryOpenIncidents(tx);

    // ── endpoints: 노드 간 주소 (RPi C RTP 방송 목적지) ──
    // fire_threshold 처럼 트랜잭션을 따로 열지 않는다 — queryEndpoints 가
    // to_regclass 로 먼저 걸러서 없는 테이블에 쿼리를 보내지 않기 때문이다.
    const std::string endpoints = queryEndpoints(tx);

    // ── site_config: 전역 설정 (SITE 문구·캘리브레이션, 08-12) ──
    // endpoints 와 같은 이유로 본 트랜잭션을 같이 쓴다 (to_regclass 가드가
    // 미적용 DB 에서 쿼리 자체를 안 보낸다). 기동 첫 틱의 이 발행이
    // "DB → retained 복원"이다 — 브로커가 재시작돼도 값이 살아나는 근거.
    const std::string sitecfg = querySiteConfig(tx);

    // ── dates: CROWD 화면 우측 날짜 목록 ──
    //
    // 날짜 목록은 일 파티션 이름이 이미 갖고 있다 — detections 본체를 스캔할
    // 이유가 없다 (실측 2026-07-31, 폴러 가동 중 3회 중앙값: 전 행 스캔
    // 1,694ms → 3.5ms, planning 포함 1,708ms → 10.2ms.
    // 근거·재현법은 DATES_QUERY_OPTIMIZATION.md).
    //
    // ⚠ 파티션 명명 규약 'detections_pYYYYMMDD'에 의존한다. schema.sql의
    //   detections_ensure_partitions·detections_drop_old와 같은 규약이라
    //   바꾸려면 세 곳을 함께 고쳐야 한다. 어긋나면 에러 없이 빈 목록이 된다.
    //
    // day <= current_date — 파티션은 7일치가 비어 있는 채로 미리 생성된다.
    //   거르지 않으면 VMS가 데이터 없는 미래 날짜를 자동 선택해 빈 히트맵을 띄운다.
    //
    // EXISTS — 폴러가 멈춰 있던 날의 빈 파티션 제외 (실측 07-23~26 4일).
    //   파티션 프루닝 + idx_detections_ts로 파티션당 O(log n).
    pqxx::result d = tx.exec(
        "SELECT json_build_object("
        "  'node_id', 'rpib',"
        "  'timestamp', (extract(epoch from now()) * 1000)::bigint,"
        "  'dates', coalesce(json_agg(day ORDER BY day), '[]'::json))::text"
        " FROM ("
        "   SELECT to_date(substring(c.relname FROM '\\d{8}$'), 'YYYYMMDD') AS day"
        "   FROM pg_inherits i"
        "   JOIN pg_class c ON c.oid = i.inhrelid"
        "   JOIN pg_class p ON p.oid = i.inhparent"
        "   WHERE p.relname = 'detections'"
        "     AND c.relname ~ '^detections_p\\d{8}$'"
        " ) s"
        " WHERE day <= current_date"
        "   AND EXISTS (SELECT 1 FROM detections"
        "               WHERE ts >= day::timestamptz"
        "                 AND ts < (day + 1)::timestamptz)");

    tx.commit();

    publishIfChanged(TOPIC_ZONES, zones, g_last_zones);
    publishIfChanged(TOPIC_INCIDENTS, incidents, g_last_incidents);
    publishIfChanged(TOPIC_ENDPOINTS, endpoints, g_last_endpoints);
    publishIfChanged(TOPIC_SITECFG, sitecfg, g_last_sitecfg);
    if (!d.empty()) publishIfChanged(TOPIC_DATES, d[0][0].as<std::string>(), g_last_dates);

  } catch (const std::exception& e) {
    // 폴러는 계속 간다 — VMS는 마지막 retained 값을 계속 보여준다
    std::cerr << "[vms] 상태 발행 실패: " << e.what() << "\n";
  }

  // ── fire_threshold: SETTINGS 화면의 화재 임계 폼 ──
  //
  // 트랜잭션을 따로 여는 이유: fire_schema.sql 은 schema.sql 과 별개 파일이라
  // 아직 적용 안 된 DB가 있을 수 있다. 같은 트랜잭션에서 조회하면 "테이블 없음"
  // 하나로 그 트랜잭션이 통째로 오염돼 zones·dates 발행까지 함께 죽는다.
  try {
    pqxx::work tx(db);
    const std::string fire = queryFireThreshold(tx);
    tx.commit();
    publishIfChanged(TOPIC_FIRE, fire, g_last_fire);
  } catch (const std::exception& e) {
    // 매 틱 같은 줄을 찍지 않는다 — 30초마다 영원히 반복될 수 있다
    static bool warned = false;
    if (!warned) {
      warned = true;
      // 원인을 단정하지 않는다 — 테이블 없음(fire_schema.sql 미적용)과
      // 권한 없음(GRANT 누락 또는 접속 계정이 다름)이 둘 다 여기로 온다.
      std::cerr << "[vms] fire_threshold 조회 실패: " << e.what()
                << " — 이 토픽만 건너뛴다 (테이블·권한·접속 계정 확인)\n";
    }
  }

  // ── fire_incident: Device Control 화면의 화재 경보 팝업 재접속 복원용 ──
  // fire_threshold와 같은 이유로 트랜잭션을 분리한다(fire_event도 fire_schema.sql 소속).
  // 전이 순간(edge)은 rpib_engine이 guardx/alert/fire로 직접 쏘므로, 여기선
  // "지금 화재가 열려있는가" 스냅샷만 30초 틱으로 재발행한다.
  try {
    pqxx::work tx(db);
    const std::string incident = queryFireIncident(tx);
    tx.commit();
    if (!incident.empty())
      publishIfChanged(TOPIC_FIRE_INCIDENT, incident, g_last_fire_incident);
  } catch (const std::exception& e) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      std::cerr << "[vms] fire_incident 조회 실패: " << e.what()
                << " — 이 토픽만 건너뛴다 (테이블·권한·접속 계정 확인)\n";
    }
  }
}

void publishSensorState(pqxx::connection& db) {
  // publishVmsState()와 별도 함수로 분리한 이유는 헤더 문서 참조 — zones
  // (30초 틱)에 묶으면 실시간 그래프가 죽는다. 트랜잭션도 별도로 열어서
  // fire_threshold와 같은 이유로(sensor_reading도 fire_schema.sql 소속)
  // 미적용 DB에서도 이 토픽만 건너뛰게 한다.
  try {
    pqxx::work tx(db);
    const std::string sensors = querySensors(tx);
    tx.commit();
    if (!sensors.empty()) publishIfChanged(TOPIC_SENSORS, sensors, g_last_sensors);
  } catch (const std::exception& e) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      std::cerr << "[vms] sensors 조회 실패: " << e.what()
                << " — 이 토픽만 건너뛴다 (테이블·권한·접속 계정 확인)\n";
    }
  }
}

void startVmsQueryService(const Config& cfg) {
  {
    std::lock_guard<std::mutex> lk(g_qmtx);
    g_pgconn_rw = cfg.pgconn;
    g_pgconn_ro = cfg.pgconn_ro;
    g_require_token = cfg.require_token;

    // 기동 시 한 번 붙여 본다. 실패해도 계속 간다 — 요청마다 재연결을
    // 시도하므로 DB 가 늦게 떠도 회복한다. 여기서 던지면 서비스가 죽는다.
    //
    // ⚠ 성공 로그의 `user=` 를 반드시 볼 것. 롤 분리가 실제로 걸렸는지는
    //   그 줄이 유일한 증거다 — PGCONN_RO 미설정 시 아래 경고와 함께
    //   쓰기 계정으로 폴백하므로, 조용히 "분리한 줄 알고" 넘어갈 수 있다.
    try { roDb(); } catch (const std::exception&) {}
    try { rwDb(); } catch (const std::exception&) {}
  }

  // DB가 아직 안 붙었어도 구독은 건다. 침묵하면 VMS는 타임아웃까지
  // 기다리기만 하지만, 구독해 두면 최소한 "왜 실패했는지"를 답할 수 있다.
  mqttSubscribe(TOPIC_HEATDAY, handleHeatday);
  mqttSubscribe(TOPIC_OCCSERIES, handleOccseries);
  mqttSubscribe(TOPIC_QINCIDENTS, handleIncidentHistory);
  mqttSubscribe(TOPIC_TRAJECTORY, handleTrajectoryAnalytics);
  mqttSubscribe(TOPIC_SENSOR_HISTORY, handleSensorHistory);
  mqttSubscribe(TOPIC_SETZONE, handleSetZone);
  mqttSubscribe(TOPIC_SETFIRE, handleSetFireThreshold);
  mqttSubscribe(TOPIC_SETACTUATOR, handleSetActuator);

  // 로그인 3종 (작업 E). ⚠ 비밀번호가 평문으로 실리므로 브로커가 mTLS 로
  // 넘어간 뒤에만 실제로 써야 한다 — 구독 자체는 걸어 둔다(안 걸면 VMS 가
  // 타임아웃까지 기다리기만 하고 이유를 모른다).
  mqttSubscribe(TOPIC_LOGIN, handleLogin);
  mqttSubscribe(TOPIC_SESSION_CHECK, handleSessionCheck);
  mqttSubscribe(TOPIC_LOGOUT, handleLogout);

  // 작업 G (§5b) — 비밀번호 변경·계정 생성
  mqttSubscribe(TOPIC_CHANGE_PW, handleChangePassword);
  mqttSubscribe(TOPIC_CREATE_USER, handleCreateUser);
  std::cout << "[auth] 계정 관리 수신 — " << TOPIC_CHANGE_PW << ", "
            << TOPIC_CREATE_USER << "\n";

  // 08-12 — 계정 비활성/재활성 · 전역 설정 (계약 §3.3)
  mqttSubscribe(TOPIC_SET_USER_ENABLED, handleSetUserEnabled);
  mqttSubscribe(TOPIC_SETSITECFG, handleSetSiteConfig);
  std::cout << "[auth] 계정 상태 수신 — " << TOPIC_SET_USER_ENABLED << "\n";
  std::cout << "[vms] 전역 설정 수신 — " << TOPIC_SETSITECFG << "\n";
  std::cout << "[vms] 조회 서비스 시작 — " << TOPIC_HEATDAY << ", "
            << TOPIC_OCCSERIES << ", " << TOPIC_QINCIDENTS << ", "
            << TOPIC_TRAJECTORY << ", " << TOPIC_SENSOR_HISTORY << "\n";
  std::cout << "[vms] 설정 변경 수신 — " << TOPIC_SETZONE << ", "
            << TOPIC_SETFIRE << ", " << TOPIC_SETACTUATOR << "\n";
}
