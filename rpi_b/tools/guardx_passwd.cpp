// guardx_passwd — VMS 계정 비밀번호 해시 생성기 (작업 E 부속)
//
// 왜 필요한가: 비밀번호는 PBKDF2 해시로만 저장되므로 psql 만으로는 계정을
// 만들 수도, 비밀번호를 바꿀 수도 없다. Postgres 에 PBKDF2-SHA256 이 없다
// (pgcrypto 의 crypt() 는 다른 알고리즘이다). 이 도구가 없으면
// migration_vms_auth.sql 이 심어 둔 **git 에 공개된 시드 비밀번호를 영영 못
// 바꾼다.**
//
// 왜 DB 에 직접 쓰지 않고 SQL 을 출력하나:
//   ① 이 도구에 DB 자격을 주지 않아도 된다
//   ② 관리자가 실행 전에 문장을 눈으로 확인할 수 있다
//   ③ 원격 DB·다른 장비에도 같은 출력을 그대로 쓸 수 있다
//
// 사용:
//   ./build/guardx_passwd admin                    # 기존 계정 비밀번호 변경
//   ./build/guardx_passwd kim --create --role operator --name "김운영"
//
//   출력된 SQL 을 확인한 뒤:
//     sudo -u postgres psql -v ON_ERROR_STOP=1 -d guardx
//   에 붙여넣는다.
//
// ⚠ 비밀번호를 명령행 인자로 받지 않는다 — `ps` 와 셸 히스토리에 그대로 남는다.
#include "MqttDb/task_auth.hpp"

#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <string>

namespace {

// 시드와 같은 값(§migration_vms_auth.sql). Pi 실측으로 1회 100~300ms 나오는
// 값이라야 한다 — 너무 낮으면 무차별 대입이 쉬워지고, 너무 높으면 로그인이
// 체감될 만큼 느려진다. 바꾸면 이 도구로 만든 행만 새 값이 되고 기존 행은
// 자기 pw_iters 로 계속 검증된다(그래서 컬럼에 함께 저장한다).
constexpr int kIters    = 200000;
constexpr int kSaltBytes = 16;

/** @brief 에코 없이 한 줄 읽기. 파이프 입력이면 그냥 읽는다 */
std::string readSecret(const char* prompt) {
  const bool tty = isatty(STDIN_FILENO);
  termios old{};
  if (tty) {
    std::fputs(prompt, stderr);
    tcgetattr(STDIN_FILENO, &old);
    termios noecho = old;
    noecho.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho);
  }
  std::string line;
  std::getline(std::cin, line);
  if (tty) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    std::fputs("\n", stderr);
  }
  return line;
}

/** @brief SQL 문자열 리터럴용 이스케이프 (작은따옴표 중복) */
std::string sq(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (c == '\'') out += "''";
    else out.push_back(c);
  }
  return out;
}

void usage() {
  std::cerr <<
      "사용: guardx_passwd <username> [--create] [--role admin|operator]\n"
      "                    [--name \"표시이름\"]\n"
      "\n"
      "  기본(--create 없음) = 기존 계정의 비밀번호 변경 UPDATE 문 출력\n"
      "  --create            = 새 계정 INSERT 문 출력 (--role 필수)\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { usage(); return 2; }

  const std::string username = argv[1];
  if (username.empty() || username[0] == '-') { usage(); return 2; }

  bool create = false;
  std::string role, display;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--create") { create = true; }
    else if (a == "--role" && i + 1 < argc) { role = argv[++i]; }
    else if (a == "--name" && i + 1 < argc) { display = argv[++i]; }
    else { std::cerr << "알 수 없는 인자: " << a << "\n"; usage(); return 2; }
  }

  if (create) {
    if (role != "admin" && role != "operator") {
      std::cerr << "--create 에는 --role admin|operator 가 필요하다\n";
      return 2;
    }
    if (display.empty()) display = username;
  } else if (!role.empty() || !display.empty()) {
    std::cerr << "--role·--name 은 --create 와 함께 쓴다\n";
    return 2;
  }

  const std::string pw1 = readSecret("새 비밀번호: ");
  if (pw1.empty()) { std::cerr << "비밀번호가 비어 있다\n"; return 1; }
  const std::string pw2 = readSecret("한 번 더: ");
  if (pw1 != pw2) { std::cerr << "두 입력이 다르다\n"; return 1; }

  try {
    // salt 는 계정마다 새로 뽑는다. 같은 salt 를 재사용하면 두 계정의
    // 비밀번호가 같은지 해시만 보고 알 수 있고, 사전 계산 공격도 쉬워진다.
    const std::string salt = auth::randomHex(kSaltBytes);
    const std::string hash = auth::pbkdf2Hex(pw1, salt, kIters);

    std::cout << "-- guardx_passwd 출력 — 확인 후 실행할 것\n";
    if (create) {
      std::cout <<
          "INSERT INTO vms_user (username, display_name, pw_algo, pw_iters,\n"
          "                      pw_salt, pw_hash, role)\n"
          "VALUES ('" << sq(username) << "', '" << sq(display) << "',\n"
          "        'pbkdf2-sha256', " << kIters << ",\n"
          "        '\\x" << salt << "'::bytea,\n"
          "        '\\x" << hash << "'::bytea,\n"
          "        '" << role << "');\n";
    } else {
      std::cout <<
          "UPDATE vms_user\n"
          "   SET pw_algo = 'pbkdf2-sha256', pw_iters = " << kIters << ",\n"
          "       pw_salt = '\\x" << salt << "'::bytea,\n"
          "       pw_hash = '\\x" << hash << "'::bytea,\n"
          // 비밀번호를 바꿨으면 잠금도 함께 푼다. 잠긴 계정의 비밀번호를
          // 바꾸는 상황이 대개 "잠겨서 못 들어간다"라서, 안 풀면 바꾸고도
          // 못 들어간다.
          "       failed_count = 0, locked_until = NULL\n"
          " WHERE username = '" << sq(username) << "';\n"
          "\n"
          "-- 비밀번호를 바꿨으면 기존 세션도 끊는 편이 맞다\n"
          "DELETE FROM vms_session\n"
          " WHERE user_id = (SELECT user_id FROM vms_user"
          " WHERE username = '" << sq(username) << "');\n";
    }
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "실패: " << e.what() << "\n";
    return 1;
  }
}
