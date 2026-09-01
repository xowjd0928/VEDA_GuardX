// task_auth.cpp — VMS 로그인·세션. 설계 배경은 MqttDb/task_auth.hpp 참조.
//
// bytea 를 libpqxx 의 바이너리 API 로 주고받지 않는다. SQL 경계에서
// encode()/decode() 로 **hex 문자열**만 오간다 — 바이너리 표현이 libpqxx
// 판올림마다 바뀌어 온 자리라(binarystring 폐기 → std::byte 계열) 버전에
// 물리지 않으려는 것이다. 이 파일의 SQL 이 payload 를 SQL 에서 만드는
// task_vms 의 관례와도 같은 결이다.
#include "MqttDb/task_auth.hpp"

#include <openssl/crypto.h>   // CRYPTO_memcmp
#include <openssl/evp.h>      // PKCS5_PBKDF2_HMAC, EVP_sha256, EVP_Digest
#include <openssl/rand.h>     // RAND_bytes

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace auth {
namespace {

// 세션 수명 (§8 확정: 30일 · 유휴 잠금 없음)
constexpr int kSessionDays = 30;

// 실패 잠금 (§5)
constexpr int kLockAfter1 = 5;    // 5회  → 60초
constexpr int kLockSecs1  = 60;
constexpr int kLockAfter2 = 10;   // 10회 → 10분
constexpr int kLockSecs2  = 600;

// 토큰 길이. 32바이트 = 256비트 — 추측이 성립하지 않는다.
constexpr int kTokenBytes = 32;

// 새로 만드는 비밀번호의 파라미터. `tools/guardx_passwd.cpp` 와 시드
// (`migration_vms_auth.sql`)가 같은 값을 쓴다 — 셋이 어긋나면 검증은 되지만
// 강도가 제각각이 된다. 기존 행은 자기 `pw_iters` 로 계속 검증되므로
// 이 값을 올려도 재해시 없이 새 비밀번호부터 적용된다.
constexpr int kIters     = 200000;
constexpr int kSaltBytes = 16;

// ISO8601 UTC Z, 초 단위. 프로젝트 규약(핸드오프 §5): `*_at` 은 전부 이 형식.
// timestamptz 를 json 에 그냥 넣으면 접속 세션의 TimeZone 설정대로 렌더돼
// (+09:00 ↔ +00:00) 서버 설정 한 줄로 payload 가 바뀐다.
constexpr const char* kIsoUtc =
    "to_char(%s AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')";

std::string isoUtc(const char* expr) {
  std::string s = kIsoUtc;
  const size_t p = s.find("%s");
  return s.replace(p, 2, expr);
}

const char kHex[] = "0123456789abcdef";

std::string toHex(const unsigned char* p, size_t n) {
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(kHex[p[i] >> 4]);
    out.push_back(kHex[p[i] & 0x0F]);
  }
  return out;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::vector<unsigned char> fromHex(const std::string& s) {
  if (s.size() % 2 != 0) throw std::runtime_error("hex 길이가 홀수");
  std::vector<unsigned char> out(s.size() / 2);
  for (size_t i = 0; i < out.size(); ++i) {
    const int hi = hexVal(s[i * 2]), lo = hexVal(s[i * 2 + 1]);
    if (hi < 0 || lo < 0) throw std::runtime_error("hex 아닌 문자");
    out[i] = static_cast<unsigned char>((hi << 4) | lo);
  }
  return out;
}

/** @brief SHA-256(원문) 을 hex 로. 토큰 → token_hash 변환에 쓴다. */
std::string sha256Hex(const std::string& in) {
  unsigned char md[32];
  unsigned int len = 0;
  if (EVP_Digest(in.data(), in.size(), md, &len, EVP_sha256(), nullptr) != 1 ||
      len != sizeof(md)) {
    throw std::runtime_error("SHA-256 실패");
  }
  return toHex(md, sizeof(md));
}

/**
 * @brief 존재하지 않는 계정에도 같은 시간을 쓰기 위한 더미 해시.
 *
 * 없는 계정에 즉시 답하면 응답 시간만으로 계정 존재 여부를 알 수 있다
 * (200,000회 PBKDF2 는 ~100-300ms 라 차이가 눈에 띄게 크다). reason 을
 * bad_credentials 하나로 덮어놓고 시간으로 새면 그 설계가 무의미해진다.
 */
void dummyPbkdf2() {
  unsigned char out[32];
  static const unsigned char salt[16] = {0};
  PKCS5_PBKDF2_HMAC("", 0, salt, sizeof(salt), 200000, EVP_sha256(),
                    sizeof(out), out);
  OPENSSL_cleanse(out, sizeof(out));
}

}  // namespace

std::string pbkdf2Hex(const std::string& password, const std::string& salt_hex,
                      int iters) {
  const std::vector<unsigned char> salt = fromHex(salt_hex);
  unsigned char out[32];
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                        salt.data(), static_cast<int>(salt.size()), iters,
                        EVP_sha256(), sizeof(out), out) != 1) {
    throw std::runtime_error("PBKDF2 실패");
  }
  const std::string hex = toHex(out, sizeof(out));
  OPENSSL_cleanse(out, sizeof(out));
  return hex;
}

std::string randomHex(int n_bytes) {
  std::vector<unsigned char> buf(static_cast<size_t>(n_bytes));
  // ⚠ rand() 계열을 쓰면 안 된다. 토큰이 예측 가능해지면 세션이 통째로 뚫린다.
  if (RAND_bytes(buf.data(), n_bytes) != 1)
    throw std::runtime_error("난수 생성 실패 (RAND_bytes)");
  const std::string hex = toHex(buf.data(), buf.size());
  OPENSSL_cleanse(buf.data(), buf.size());
  return hex;
}

Result login(pqxx::work& tx, const std::string& username,
             const std::string& password, const std::string& device) {
  Result r;

  // 계정 조회. bytea 는 hex 로 받는다(파일 머리말 참조).
  // locked_until 이 미래인지도 DB 시계로 판단한다 — 프로세스 시계를 믿으면
  // 두 시계가 어긋났을 때 잠금이 일찍 풀리거나 영영 안 풀린다.
  pqxx::result u = tx.exec(
      "SELECT user_id, display_name, role, enabled, pw_iters,"
      "       encode(pw_salt,'hex'), encode(pw_hash,'hex'),"
      "       (locked_until IS NOT NULL AND locked_until > now()),"
      "       greatest(0, ceil(extract(epoch FROM"
      "                 coalesce(locked_until, now()) - now())))::int,"
      "       failed_count, must_change_pw"
      "  FROM vms_user WHERE username = $1::text",
      pqxx::params{username});

  if (u.empty()) {
    // 없는 계정 — 있는 계정과 같은 시간을 쓰고 같은 답을 준다.
    dummyPbkdf2();
    r.reason = reason::kBadCredentials;
    return r;
  }

  const int         user_id  = u[0][0].as<int>();
  const std::string display  = u[0][1].as<std::string>();
  const std::string role     = u[0][2].as<std::string>();
  const bool        enabled  = u[0][3].as<bool>();
  const int         iters    = u[0][4].as<int>();
  const std::string salt_hex = u[0][5].as<std::string>();
  const std::string hash_hex = u[0][6].as<std::string>();
  const bool        locked   = u[0][7].as<bool>();
  const int         retry_s  = u[0][8].as<int>();
  const int         failed   = u[0][9].as<int>();
  const bool        mustchg  = u[0][10].as<bool>();

  // 잠금·비활성은 비밀번호를 보기 전에 답한다. 잠긴 계정에 대고 해시를
  // 계속 계산해 주면 잠금이 CPU 소모 통로가 된다.
  if (locked) {
    r.reason = reason::kLocked;
    r.retry_after_s = retry_s;
    return r;
  }
  if (!enabled) {
    r.reason = reason::kDisabled;
    return r;
  }

  const std::string got = pbkdf2Hex(password, salt_hex, iters);

  // ⚠ 상수 시간 비교. `==` 는 첫 다른 바이트에서 끝나므로 응답 시간으로
  //   해시를 한 바이트씩 맞춰갈 수 있다(전형적인 타이밍 공격).
  bool match = got.size() == hash_hex.size() &&
               CRYPTO_memcmp(got.data(), hash_hex.data(), got.size()) == 0;

  if (!match) {
    const int next = failed + 1;
    // 5회 → 60초, 10회 → 10분. 임계에 **도달한 시점에만** 잠근다 —
    // 매번 갱신하면 6·7·8회째에도 60초가 새로 걸려 사실상 영구 잠금이 된다.
    int lock_s = 0;
    if (next >= kLockAfter2)      lock_s = kLockSecs2;
    else if (next >= kLockAfter1) lock_s = kLockSecs1;

    tx.exec(
        "UPDATE vms_user"
        "   SET failed_count = $2::int,"
        "       locked_until = CASE WHEN $3::int > 0"
        "                           THEN now() + ($3::int * interval '1 second')"
        "                           ELSE locked_until END"
        " WHERE user_id = $1::int",
        pqxx::params{user_id, next, lock_s});

    if (lock_s > 0) {
      r.reason = reason::kLocked;
      r.retry_after_s = lock_s;
    } else {
      r.reason = reason::kBadCredentials;
    }
    return r;
  }

  // ── 성공 ──
  tx.exec(
      "UPDATE vms_user"
      "   SET failed_count = 0, locked_until = NULL, last_login_at = now()"
      " WHERE user_id = $1::int",
      pqxx::params{user_id});

  r.token = randomHex(kTokenBytes);

  // 토큰 원문은 안 남긴다 — DB 가 새도 남의 세션을 못 쓰게.
  pqxx::result s = tx.exec(
      "INSERT INTO vms_session (token_hash, user_id, expires_at, device)"
      " VALUES (decode($1::text,'hex'), $2::int,"
      "         now() + ($3::int * interval '1 day'), $4::text)"
      " RETURNING " + isoUtc("expires_at"),
      pqxx::params{sha256Hex(r.token), user_id, kSessionDays, device});

  r.ok           = true;
  r.username     = username;
  r.display_name = display;
  r.role         = role;
  r.must_change  = mustchg;
  r.expires_at   = s[0][0].as<std::string>();
  return r;
}

Result check(pqxx::work& tx, const std::string& token) {
  Result r;
  if (token.empty()) {
    r.reason = reason::kExpired;
    return r;
  }

  pqxx::result s = tx.exec(
      "SELECT u.username, u.display_name, u.role, u.enabled,"
      "       (v.expires_at <= now()) AS is_expired,"
      "       " + isoUtc("v.expires_at") + ", u.must_change_pw"
      "  FROM vms_session v JOIN vms_user u USING (user_id)"
      " WHERE v.token_hash = decode($1::text,'hex')",
      pqxx::params{sha256Hex(token)});

  // 없는 토큰과 만료된 토큰을 같은 답으로 덮는다 — 구분해 주면 토큰 존재
  // 여부를 떠보는 조회 도구가 된다.
  if (s.empty()) {
    r.reason = reason::kExpired;
    return r;
  }

  if (s[0][4].as<bool>()) {
    // 만료된 세션은 이 자리에서 지운다. 따로 청소 작업을 두지 않아도
    // 쓰이는 토큰은 여기서 걸러지고, 안 쓰이는 토큰은 남아도 무해하다
    // (그래도 대량으로 쌓이면 idx_vms_session_expires 로 일괄 삭제 가능).
    tx.exec("DELETE FROM vms_session WHERE token_hash = decode($1::text,'hex')",
            pqxx::params{sha256Hex(token)});
    r.reason = reason::kExpired;
    return r;
  }

  if (!s[0][3].as<bool>()) {
    r.reason = reason::kDisabled;
    return r;
  }

  r.ok           = true;
  r.username     = s[0][0].as<std::string>();
  r.display_name = s[0][1].as<std::string>();
  r.role         = s[0][2].as<std::string>();
  r.expires_at   = s[0][5].as<std::string>();
  r.must_change  = s[0][6].as<bool>();
  return r;
}

Result changePassword(pqxx::work& tx, const std::string& token,
                      const std::string& old_password,
                      const std::string& new_password,
                      const std::string& device) {
  Result r;
  if (token.empty()) {
    r.reason = reason::kExpired;
    return r;
  }

  // 세션 + 비밀번호 재료를 한 번에. check() 를 부르고 다시 조회하면 왕복이
  // 둘이 되고, 그 사이에 계정이 바뀔 여지가 생긴다.
  pqxx::result s = tx.exec(
      "SELECT u.user_id, u.username, u.display_name, u.role, u.enabled,"
      "       u.pw_iters, encode(u.pw_salt,'hex'), encode(u.pw_hash,'hex'),"
      "       (v.expires_at <= now())"
      "  FROM vms_session v JOIN vms_user u USING (user_id)"
      " WHERE v.token_hash = decode($1::text,'hex')",
      pqxx::params{sha256Hex(token)});

  // 없는 토큰과 만료를 같은 답으로 덮는다 (check() 와 같은 이유)
  if (s.empty() || s[0][8].as<bool>()) {
    r.reason = reason::kExpired;
    return r;
  }
  if (!s[0][4].as<bool>()) {
    r.reason = reason::kDisabled;
    return r;
  }

  const int         user_id  = s[0][0].as<int>();
  const std::string username = s[0][1].as<std::string>();
  const std::string display  = s[0][2].as<std::string>();
  const std::string role     = s[0][3].as<std::string>();
  const int         iters    = s[0][5].as<int>();
  const std::string salt_hex = s[0][6].as<std::string>();
  const std::string hash_hex = s[0][7].as<std::string>();

  // ⚠ 현재 비밀번호 확인이 먼저다. 정책 검사를 앞에 두면, 토큰만 훔친 사람이
  //   현재 비밀번호를 모르고도 "어떤 새 비밀번호가 통과하는지"를 떠볼 수 있다.
  const std::string got = pbkdf2Hex(old_password, salt_hex, iters);
  const bool match = got.size() == hash_hex.size() &&
                     CRYPTO_memcmp(got.data(), hash_hex.data(), got.size()) == 0;
  if (!match) {
    // 여기서는 실패 카운트를 올리지 않는다. 이미 유효한 세션을 가진 본인이고,
    // 올리면 오타 몇 번으로 자기 계정이 잠긴다(로그인과 성격이 다르다).
    r.reason = reason::kBadCredentials;
    return r;
  }

  // 길이 정책은 08-12 에 폐지됐다 — 빈 값만 막는다 (task_auth.hpp 주석 참조).
  if (new_password.empty()) {
    r.reason = reason::kWeakPassword;
    return r;
  }

  const std::string salt = randomHex(kSaltBytes);
  const std::string hash = pbkdf2Hex(new_password, salt, kIters);

  tx.exec(
      "UPDATE vms_user"
      "   SET pw_algo = 'pbkdf2-sha256', pw_iters = $2::int,"
      "       pw_salt = decode($3::text,'hex'),"
      "       pw_hash = decode($4::text,'hex'),"
      "       must_change_pw = FALSE,"
      // 비밀번호를 바꿨으면 잠금도 푼다 — 잠긴 상태로 바꾸는 상황이 대개
      // "잠겨서 못 들어간다"라서, 안 풀면 바꾸고도 못 들어간다.
      "       failed_count = 0, locked_until = NULL"
      " WHERE user_id = $1::int",
      pqxx::params{user_id, kIters, salt, hash});

  // ⭐ 기존 세션 전부 무효화. 비밀번호를 바꾸는 이유의 절반이 "샜을지도
  //    모른다"라서, 다른 기기에 남은 세션이 살아 있으면 바꾼 의미가 없다.
  //    방금 쓴 그 토큰도 함께 사라진다.
  tx.exec("DELETE FROM vms_session WHERE user_id = $1::int",
          pqxx::params{user_id});

  // 그래서 새 세션을 발급해 응답에 실어 준다 — 안 그러면 방금 바꾼 본인이
  // 그 자리에서 튕긴다.
  r.token = randomHex(kTokenBytes);
  pqxx::result ns = tx.exec(
      "INSERT INTO vms_session (token_hash, user_id, expires_at, device)"
      " VALUES (decode($1::text,'hex'), $2::int,"
      "         now() + ($3::int * interval '1 day'), $4::text)"
      " RETURNING " + isoUtc("expires_at"),
      pqxx::params{sha256Hex(r.token), user_id, kSessionDays, device});

  r.ok           = true;
  r.username     = username;
  r.display_name = display;
  r.role         = role;
  r.must_change  = false;
  r.expires_at   = ns[0][0].as<std::string>();
  return r;
}

Result createUser(pqxx::work& tx, const std::string& username,
                  const std::string& display_name, const std::string& role,
                  const std::string& password) {
  Result r;

  // 역할 화이트리스트. vms_user 의 CHECK 이 DB 단에서도 막지만, 여기서 걸러야
  // 사용자에게 뜻이 통하는 reason 이 나간다(CHECK 위반은 예외로 튄다).
  if (role != "admin" && role != "operator") {
    r.reason = reason::kBadRole;
    return r;
  }
  // 빈 username 은 규격 위반이다. §5b 에 따로 reason 이 없어 가장 가까운
  // bad_role(= 요청 필드가 규격 밖)로 답한다. VMS 폼은 이 값을 못 만든다.
  if (username.empty()) {
    r.reason = reason::kBadRole;
    return r;
  }
  // 길이 정책은 08-12 에 폐지됐다 — 빈 값만 막는다 (task_auth.hpp 주석 참조).
  if (password.empty()) {
    r.reason = reason::kWeakPassword;
    return r;
  }

  const std::string salt = randomHex(kSaltBytes);
  const std::string hash = pbkdf2Hex(password, salt, kIters);
  const std::string disp = display_name.empty() ? username : display_name;

  // ON CONFLICT DO NOTHING + RETURNING — 중복이면 빈 결과가 온다.
  // 미리 SELECT 로 확인하면 그 사이에 낄 여지가 생기고(TOCTOU), 예외로 잡으면
  // 트랜잭션이 오염돼 뒤따르는 문장이 전부 죽는다.
  //
  // ⚠ 새 계정은 항상 must_change_pw = TRUE 다. 만든 사람이 초기 비밀번호를
  //   알고 있으므로 본인이 첫 로그인에서 바꿔야 한다 (§5b).
  pqxx::result ins = tx.exec(
      "INSERT INTO vms_user (username, display_name, pw_algo, pw_iters,"
      "                      pw_salt, pw_hash, role, must_change_pw)"
      " VALUES ($1::text, $2::text, 'pbkdf2-sha256', $3::int,"
      "         decode($4::text,'hex'), decode($5::text,'hex'),"
      "         $6::text, TRUE)"
      " ON CONFLICT (username) DO NOTHING"
      " RETURNING user_id",
      pqxx::params{username, disp, kIters, salt, hash, role});

  if (ins.empty()) {
    r.reason = reason::kDuplicate;
    return r;
  }

  r.ok           = true;
  r.user_id      = ins[0][0].as<int>();
  r.username     = username;
  r.display_name = disp;
  r.role         = role;
  r.must_change  = true;
  return r;
}

Result setUserEnabled(pqxx::work& tx, const std::string& actor,
                      const std::string& username, bool enabled) {
  Result r;

  // 자기 자신 금지 — 끄는 것은 물론 켜는 것도 성립하지 않는다(꺼진 계정은
  // 명령을 못 보낸다). 과도기 무토큰 통과(actor 빈 값)면 이 검사가 성립하지
  // 않는다 — 계약 §3.3 에 그렇게 적었다 (REQUIRE_TOKEN=1 이후는 항상 성립).
  if (!actor.empty() && actor == username) {
    r.reason = reason::kSelfTarget;
    return r;
  }

  pqxx::result u = tx.exec(
      "SELECT user_id, enabled, role FROM vms_user WHERE username = $1::text",
      pqxx::params{username});
  if (u.empty()) {
    r.reason = reason::kNotFound;
    return r;
  }
  const int         user_id     = u[0][0].as<int>();
  const bool        cur_enabled = u[0][1].as<bool>();
  const std::string role        = u[0][2].as<std::string>();

  // 마지막 활성 admin 보호 — admin 0명이 되면 복구 수단이 psql 뿐이다.
  // 이미 꺼진 admin 을 다시 끄는 멱등 경로는 여기 안 걸린다(활성 수가 안 준다).
  if (!enabled && cur_enabled && role == "admin") {
    const int others = tx.exec(
        "SELECT count(*)::int FROM vms_user"
        " WHERE role = 'admin' AND enabled AND user_id <> $1::int",
        pqxx::params{user_id})[0][0].as<int>();
    if (others == 0) {
      r.reason = reason::kLastAdmin;
      return r;
    }
  }

  tx.exec("UPDATE vms_user SET enabled = $2::bool WHERE user_id = $1::int",
          pqxx::params{user_id, enabled});

  // ⭐ 끌 때는 세션도 전부 지운다 — 안 지우면 발급된 토큰이 최대 30일 살아서
  //    다른 PC 의 로그인이 유지된다. 멱등 경로(이미 꺼짐)에서도 지운다 —
  //    psql 로 직접 끈 계정에는 세션이 남아 있을 수 있다.
  if (!enabled) {
    tx.exec("DELETE FROM vms_session WHERE user_id = $1::int",
            pqxx::params{user_id});
  }

  r.ok       = true;
  r.username = username;
  r.role     = role;
  r.enabled  = enabled;
  return r;
}

bool logout(pqxx::work& tx, const std::string& token) {
  if (token.empty()) return true;
  tx.exec("DELETE FROM vms_session WHERE token_hash = decode($1::text,'hex')",
          pqxx::params{sha256Hex(token)});
  // 없는 토큰이어도 성공이다. "그 토큰은 없다"를 알려줄 이유가 없고,
  // 로그아웃은 재시도해도 같은 결과여야 한다(멱등).
  return true;
}

}  // namespace auth
