#pragma once

#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

/**
 * @brief 비밀정보 보관/로드 + TLS 설정
 *
 * 소스에 비밀번호를 박아두면 공유폴더·git·백업으로 그대로 새어나간다.
 * 여기서는 실행 환경에서 읽어온다. 우선순위:
 *   1) 환경변수 (GUARDX_CAM_PASSWORD 등) — CI/일회성 실행용
 *   2) %APPDATA%/GuardX/credentials.ini — 평소 경로 (공유폴더 밖!)
 *      값이 "dpapi:..." 로 시작하면 Windows DPAPI로 복호화한다.
 *      DPAPI 암호문은 그 사용자 계정에서만 풀리므로 파일이 유출돼도 못 쓴다.
 *
 * 파일이 없으면 실패를 감추지 않는다(fail closed). last_error()에 사유가 남고
 * 호출부가 사용자에게 알린다.
 */
namespace Credentials {

/** @brief 설정 로드. 앱 시작 시 1회 호출 */
bool load();

/** @brief 로드 실패 사유 (성공 시 빈 문자열) */
QString last_error();

/** @brief 설정 파일 경로 (사용자 안내용) */
QString config_path();

QString camera_host();
QString camera_user();
QString camera_password();

// [database] 섹션은 v6에서 제거됐다. VMS는 더 이상 Postgres에 직접 붙지 않고
// 필요한 값은 전부 MQTT로 받는다 (docs/DB_LINK_AND_MQTT_MIGRATION.md).
// DB 조회는 RPi B 폴러가 대신하며, 자격은 그쪽에만 있다.

// MQTT 브로커 (RPi B). [mqtt] 섹션이 없으면 기본값으로 동작한다.
// 기본값은 172.20.33.251:8883(mTLS) — 인증서 3종만 두면 ini 없이 붙는다.
QString mqtt_host();
int     mqtt_port();
QString mqtt_user();
QString mqtt_password();

/**
 * @brief `[mqtt] client_id` — MQTT 세션 id 오버라이드 (비면 "vms-{hostname}")
 *
 * 계정은 언제나 인증서 CN 이다(브로커 use_identity_as_username) — 이 값은
 * **세션 식별자만** 바꾸고, 인증서 파일명(`vms-{hostname}.crt`)은 안 바뀐다.
 * 한 PC 에서 앱 두 개를 병행할 때(개발) 같은 id 로 서로 세션을 끊는 것을
 * 피하는 용도다. 예: `client_id = vms-3-11-b`
 */
QString mqtt_client_id();

/**
 * @brief mTLS 인증서를 두는 표준 자리 — `<앱 로컬 데이터>/certs`
 *
 * 실제 경로는 `C:/Users/<계정>/AppData/Local/GuardX/certs` 다.
 * **자격 파일(credentials.ini)과 같은 폴더 아래**로 두는 것이 요점이다 —
 * 그 자리는 이미 OneDrive 밖이고, 자격과 수명이 같아 한쪽만 남는 일이 없다.
 *
 * ⚠ **개인키는 리포·공유폴더·OneDrive 에 두지 않는다.** 경로를 사람이 적지
 * 않으므로 "옮겼는데 ini 를 안 고쳐 접속이 죽는" 사고도 사라진다(08-11 실사고).
 */
QString certs_dir();

/**
 * @brief mTLS 파일 경로 — 기본은 `certs_dir()` 안의 규약 파일명이다
 *
 *   ca   : `ca.crt`
 *   cert : `<cn>.crt`   (예: `vms-3-11.crt` — 파일명 = CN = 계정)
 *   key  : `<cn>.key`
 *
 * ⚠ 인자는 **인증서 이름(CN = "vms-{hostname}")**이다. `[mqtt] client_id` 로
 * 세션 id 를 바꿔도 여기 넘기는 이름은 안 바뀐다 — 계정은 CN 이 정한다.
 *
 * `[mqtt] tls_ca/tls_cert/tls_key` 를 적으면 그 값이 이긴다(다른 자리에 둘 때의
 * 탈출구). 세 파일이 다 있으면 TLS 로 붙는다 — `mqtt_tls_enabled()`.
 * `[mqtt] tls=0` 은 인증서가 있어도 쓰지 않는다(1883 롤백용).
 */
QString mqtt_tls_ca(const QString &cn);
QString mqtt_tls_cert(const QString &cn);
QString mqtt_tls_key(const QString &cn);
/**
 * @brief 평문으로 붙겠다고 **명시**했는가 (`[mqtt] tls=0`) — 1883 롤백용
 *
 * ⚠ **"인증서가 없다"는 여기 해당하지 않는다.** 파일이 없으면 TLS 를 포기하는
 * 게 아니라 **접속을 포기한다**(MqttLink). 자동 강등은 보안 설정이 조용히
 * 사라지는 가장 흔한 방식이고, 평문 8883 은 어차피 브로커가 끊는다.
 */
bool    mqtt_tls_opt_out();

/** @brief SUNAPI/메타데이터 요청에 HTTPS를 쓸지 */
bool camera_https();

/** @brief 고정된 카메라 인증서 지문 (SHA-256, hex). 비어 있으면 미설정 */
QString camera_cert_pin();

/** @brief scheme/포트를 설정에 맞춰 만든 카메라 기본 URL */
QUrl camera_base_url();

/**
 * @brief 바닥 캘리브레이션 웹 UI(test_calibration OpenApp) 주소
 *
 * 웹뷰어의 "앱 화면 가기" 링크와 같은 규칙이다 —
 *   https://<host>/home/setup/opensdk/html/<AppName>/index.html?AppName=<AppName>
 * `/opensdk/<app_id>/` 형태(OpenAPI 엔드포인트, 설치마다 바뀌는 id)와는
 * 다른 경로이니 혼동하지 말 것. AppName은 고정값("test_calibration")이라
 * 여기 하드코딩해도 설치가 바뀌어도 안 틀어진다.
 */
QUrl calibration_ui_url();

/**
 * @brief NAM에 인증서 고정 검증을 건다
 *
 * 카메라는 기기 자체 서명 인증서를 쓰므로 CA 검증이 통과할 수 없다.
 * 그렇다고 오류를 전부 무시하면 중간자 공격에 무방비다. 대신 지문을
 * 고정(pinning)해 "그 카메라의 그 인증서"만 허용한다.
 * 지문이 설정돼 있지 않으면 아무것도 수락하지 않는다.
 */
void install_tls_pinning(QNetworkAccessManager *nam);

/** @brief 신뢰 개시(TOFU): 현재 카메라 인증서 지문을 조회해 설정에 저장 */
bool pin_current_camera_cert(QString *out_fingerprint, QString *out_error);

/** @brief 평문 credentials.ini의 비밀값들을 DPAPI로 암호화해 다시 쓴다 */
bool encrypt_config_file(QString *out_error);

/** @brief 문자열 DPAPI 암/복호 (Windows 전용, 그 외 플랫폼은 그대로 반환) */
QString protect(const QString &plain);
QString unprotect(const QString &stored);

} // namespace Credentials
