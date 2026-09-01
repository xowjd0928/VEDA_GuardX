#pragma once
// task_auth — VMS 로그인·세션 검증. Database/migration_vms_auth.sql 의 짝.
//
// ── 이 모듈이 커넥션을 갖지 않는 이유 ──
// 전부 `pqxx::work&`(트랜잭션)를 받는 순수 함수다. 커넥션 소유는 task_vms 에
// 그대로 남긴다 — 그래야 ①로그인 실패 카운트 증가와 세션 발급이 호출부의
// 트랜잭션 하나에 묶이고 ②커넥션·뮤텍스가 두 모듈에 흩어지지 않는다.
// 여기 있는 함수는 전부 **쓰기 커넥션(rwDb)의 트랜잭션**으로 불러야 한다.
// vms_user·vms_session 은 guardx_reader 에서 회수돼 있어 조회 커넥션으로는
// 아예 못 읽는다(그게 설계다 — 비밀번호 해시가 읽기 롤에 안 보이게).
//
// ── 계약 ──
// 토픽·payload·reason 문자열의 정본은 Obsidian
// 「0811_로그인·권한 RPi B 핸드오프 (전달용)」 §5 다. 바꿔야 하면 문서를 먼저.
//
//   guardx/db/rpib/cmd/login          {username, password}
//     -> {ok, role, display_name, token, expires_at}
//   guardx/db/rpib/cmd/session_check  {token}
//     -> {ok, username, role, expires_at}
//   guardx/db/rpib/cmd/logout         {token}   -> {ok}
//
// 실패는 {ok:false, reason:"..."} 다. reason 은 아래 고정 목록이며 **기계가
// 분기하는 값**이다. 기존 질의 규약의 `error`(예외 메시지 원문, 자유 형식)와
// 성격이 다르므로 섞지 않는다.
//
// ⚠ 비밀번호가 payload 에 평문으로 실린다. **mTLS(작업 A) 이후에만 유효한
//   설계다.** 1883 평문 리스너가 열려 있는 동안 로그인을 쓰면 같은 LAN 에서
//   비밀번호를 그대로 주울 수 있다.
#include <pqxx/pqxx>

#include <string>

namespace auth {

/**
 * @brief 실패 사유 — §5 고정 목록. 이 문자열이 그대로 payload 의 reason 이 된다.
 *
 * `bad_credentials` 하나가 "계정 없음"과 "비밀번호 틀림"을 모두 덮는다.
 * 둘을 구분해 주면 로그인 화면이 계정 존재 여부를 알려주는 조회 도구가 된다.
 */
namespace reason {
inline constexpr const char* kBadCredentials = "bad_credentials";
inline constexpr const char* kLocked         = "locked";     // + retry_after_s
inline constexpr const char* kDisabled       = "disabled";
inline constexpr const char* kExpired        = "expired";
inline constexpr const char* kForbidden      = "forbidden";  // 작업 F (역할 부족)
// ── 작업 G (§5b) ──
inline constexpr const char* kWeakPassword   = "weak_password";
inline constexpr const char* kDuplicate      = "duplicate";   // 이미 있는 username
inline constexpr const char* kBadRole        = "bad_role";
/**
 * @brief ⚠ 서버측 백스톱 — §5b 에 없는 값이다 (RPi B 가 추가)
 *
 * `must_change_pw` 인 계정의 토큰으로 쓰기 명령이 오면 이걸 돌려준다.
 * VMS 는 그 상태에서 명령을 안 보내도록 짜여 있으므로 정상 흐름에서는 안 뜬다.
 *
 * 왜 넣었나: §5b 가 존재하는 이유가 "시드 비밀번호가 저장소에 공개돼 있다"인데,
 * 강제 변경을 UI 로만 막으면 **공개된 비밀번호로 로그인해 액추에이터를 그대로
 * 조작할 수 있다.** 그러면 §5b 는 보안이 아니라 화면 연출이 된다.
 * §0 의 "UI 잠금은 실수 방지, 진짜 방어선은 서버"가 여기에도 적용된다.
 */
inline constexpr const char* kMustChangePw   = "must_change_password";
// ── 08-12 (계정 비활성/재활성 — cmd/set_user_enabled) ──
inline constexpr const char* kSelfTarget = "self_target";  // 대상이 요청자 자신
inline constexpr const char* kLastAdmin  = "last_admin";   // 마지막 활성 admin 보호
inline constexpr const char* kNotFound   = "not_found";    // 그런 username 없음
}  // namespace reason

// 비밀번호 길이 정책은 08-12 에 폐지됐다(전 세션 합의 ②) — 1글자부터 유효하다.
// 서버가 거부하는 것은 빈 비밀번호("") 하나뿐이고 그때 kWeakPassword 를 준다.
// 빈 값을 막는 근거: 로그인 핸들러가 빈 password 를 bad_credentials 로
// 거절하므로, 허용하면 "만들 수는 있는데 로그인은 안 되는 계정"이 생긴다.
// trim 은 어디에도 없다 — 공백만으로 된 비밀번호도 유효하다 (계약 §3.3 명문화).

/** @brief login·session_check 공통 결과 */
struct Result {
  bool ok = false;

  std::string reason;      ///< 실패 시에만. reason:: 의 값 중 하나
  int retry_after_s = 0;   ///< reason == kLocked 일 때만 (초)

  std::string username;
  std::string display_name;
  std::string role;        ///< "admin" | "operator"
  std::string token;       ///< login·change_password 성공 시. **원문**(DB엔 SHA-256만)
  std::string expires_at;  ///< ISO8601 UTC Z — "2026-09-10T01:22:31Z"

  /// 작업 G — TRUE 면 비밀번호를 바꾸기 전까지 쓰기 명령이 거부된다
  bool must_change = false;
  /// create_user 성공 시에만
  int user_id = 0;
  /// setUserEnabled 성공 시에만 — 반영된 값
  bool enabled = false;
};

/**
 * @brief 자격 검증 + 세션 발급. 실패 카운트·잠금까지 이 안에서 처리한다.
 *
 * @param device 감사용 표기. "vms vms-3-11" 형식(updated_by 와 같은 규약)
 *
 * 잠금 규칙(§5): 5회 → 60초, 10회 → 10분. 성공하면 카운트가 0으로 돌아간다.
 *
 * ⚠ 호출부는 **성공·실패 모두 tx.commit() 해야 한다.** 실패 시 abort 하면
 *   방금 올린 실패 카운트가 함께 사라져 잠금이 영원히 안 걸린다.
 */
Result login(pqxx::work& tx, const std::string& username,
             const std::string& password, const std::string& device);

/**
 * @brief 토큰으로 세션 조회. 만료된 세션은 이 자리에서 지운다.
 *
 * 만료·계정 비활성은 `ok:false` + reason(kExpired / kDisabled)으로 돌아온다.
 * 존재하지 않는 토큰도 kExpired 다 — "그런 토큰은 없다"와 "만료됐다"를
 * 구분해 주면 토큰 존재 여부를 떠보는 조회 도구가 된다.
 */
Result check(pqxx::work& tx, const std::string& token);

/**
 * @brief 세션 파기. 없는 토큰이어도 true (멱등 — 로그아웃은 실패할 일이 없다)
 */
bool logout(pqxx::work& tx, const std::string& token);

/**
 * @brief 작업 G — 본인 비밀번호 변경 (§5b)
 *
 * `guardx/db/rpib/cmd/change_password` — `{token, old_password, new_password}`
 * 성공 시 `{ok, token, expires_at}`. **새 토큰이 반드시 실린다.**
 *
 * ⭐ 성공하면 **그 사용자의 기존 세션을 전부 지운다.** 비밀번호를 바꾸는 이유의
 * 절반이 "샜을지도 모른다"라서, 다른 기기에 남은 세션이 살아 있으면 바꾼 의미가
 * 없다. 대신 새 세션을 발급해 응답에 실어 준다 — 그래야 방금 바꾼 본인이 그
 * 자리에서 튕기지 않는다.
 *
 * 관리자가 남의 비밀번호를 재설정하는 경로는 **일부러 두지 않았다**(사용자 결정).
 * 잊어버린 계정은 서버에서 `guardx_passwd` 로 처리한다.
 *
 * @param device 새 세션의 감사 표기 ("vms vms-3-11" 형식)
 * @return 실패 시 reason: bad_credentials(현재 비번 틀림) ·
 *         weak_password(빈 새 비밀번호) · expired · disabled
 */
Result changePassword(pqxx::work& tx, const std::string& token,
                      const std::string& old_password,
                      const std::string& new_password,
                      const std::string& device);

/**
 * @brief 작업 G — 계정 생성 (§5b). **호출부가 admin 여부를 먼저 확인해야 한다**
 *
 * `guardx/db/rpib/cmd/create_user`
 * — `{token, username, display_name, role, password}` → `{ok, user_id}`
 *
 * 새 계정은 항상 `must_change_pw = TRUE` 다 — 만든 사람이 초기 비밀번호를 알고
 * 있으므로 본인이 첫 로그인에서 바꿔야 한다.
 *
 * @param display_name 비어 있으면 username 을 쓴다
 * @return 실패 시 reason: bad_role · weak_password(빈 비밀번호) · duplicate
 */
Result createUser(pqxx::work& tx, const std::string& username,
                  const std::string& display_name, const std::string& role,
                  const std::string& password);

/**
 * @brief 08-12 — 계정 비활성/재활성. **호출부가 admin 여부를 먼저 확인해야 한다**
 *
 * `guardx/db/rpib/cmd/set_user_enabled` — `{token, username, enabled}`
 * → `{ok, username, enabled}`
 *
 * "계정 삭제" 요구의 구현이다 — 진짜 DELETE 가 아니라 enabled 플립(08-12
 * 사용자 결정). guardx_writer 에 vms_user DELETE 권한이 없고 안 주는 것이
 * 설계라("mqttd 가 뚫려도 계정을 못 지운다" — migration_vms_auth.sql), 이
 * 방식이면 그 통제가 유지되고 이력이 남고 실수 복구(재활성)가 된다.
 * 비활성 계정도 username 유니크에 걸린다 — 같은 아이디 재사용은 재활성으로.
 *
 * ⭐ 끌 때는 대상의 vms_session 을 **전부 지운다** — 안 지우면 이미 발급된
 * 토큰이 최대 30일 살아서 다른 PC 의 로그인이 유지된다.
 *
 * 멱등 — 이미 그 상태여도 ok:true (덮어쓰기 무해 원칙).
 *
 * @param actor 요청자 username. 과도기 무토큰 통과면 빈 값 — 그때는
 *              self_target 검사가 성립하지 않는다 (계약 §3.3 에 명시)
 * @return 실패 시 reason: self_target · last_admin · not_found
 */
Result setUserEnabled(pqxx::work& tx, const std::string& actor,
                      const std::string& username, bool enabled);

/**
 * @brief 비밀번호 해시 계산 — guardx_passwd 도구와 로그인이 함께 쓴다.
 * @return 32바이트 다이제스트를 소문자 hex 로 (64자)
 */
std::string pbkdf2Hex(const std::string& password, const std::string& salt_hex,
                      int iters);

/** @brief 암호학적 난수 n바이트를 소문자 hex 로 */
std::string randomHex(int n_bytes);

}  // namespace auth
