// fan_level_test.cpp — 팬 단계가 VMS 혼잡 경보 단계와 항상 같은지 검증.
//                      DB·브로커·팬 불필요.
//
// 빌드: cmake -B build && cmake --build build -j   ->  ./build/fan_level_test
// 단독: g++ -std=c++17 -Iinclude -I../shared test/fan_level_test.cpp
//        src/Poller/task_fan_level.cpp -o fan_level_test
//
// 왜 이 테스트가 필요한가: 혼잡 경보에는 히스테리시스가 걸려 있다.
//   critical -> warn 강등은 경보에 반영되지 않고(플랩 방지),
//   해제는 warn_ratio * 0.9 밴드를 내려가야 일어난다.
// 그래서 sevOf()의 **순간값**을 팬에 쓰면 화면은 critical 인데 팬은 75%로
// 내려가 있는, 서로 다른 두 단계가 생긴다. task_alert 은 그걸 피하려고
// mqttPublishAlert 에 넘기는 것과 **같은 문자열**로 팬 단계를 뽑는다.
// 여기서 그 규약을 못 박는다.
#include "Poller/task_fan_level.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

// ---------------------------------------------------------------- 스텁
// task_fan_level 이 쓰는 발행 함수만 대신한다. 브로커를 띄우지 않는다.
static std::vector<int> g_published;

void mqttPublishRetained(const std::string& topic, const std::string& payload) {
  (void)topic;
  const size_t p = payload.find("\"level\":");
  if (p == std::string::npos) return;
  g_published.push_back(std::atoi(payload.c_str() + p + 8));
}

namespace {

int last_published() { return g_published.empty() ? -1 : g_published.back(); }

// task_alert 의 한 판정을 흉내낸다: 경보 severity 문자열이 정해지면 팬도
// 그 문자열에서 단계를 뽑는다 — 실제 코드가 하는 것과 같은 호출이다.
void tick(int zone_id, const char* alert_severity) {
  reportZoneLevel(zone_id, fanLevelFromSeverityName(alert_severity));
  publishFanLevel();
}

}  // namespace

int main() {
  // 1) 문자열 -> 단계. mqttPublishAlert 이 실제로 쓰는 값 전부.
  assert(fanLevelFromSeverityName("clear") == 0);
  assert(fanLevelFromSeverityName("warn") == 1);
  assert(fanLevelFromSeverityName("critical") == 2);
  // 모르는 값은 정상으로. 팬을 올리는 쪽으로 추측하면 소음이 된다.
  assert(fanLevelFromSeverityName("") == 0);
  assert(fanLevelFromSeverityName("wArN") == 0);
  printf("severity -> level: clear=0 warn=1 critical=2\n");

  // 2) 히스테리시스 시나리오. 왼쪽이 VMS 가 보는 경보 단계,
  //    오른쪽이 팬에 실제로 나간 단계 — 항상 같아야 한다.
  //
  //    ratio 는 참고용이다. 경보가 어떤 문자열을 내보내는지는 task_alert 의
  //    상태머신이 정하고, 여기서는 그 결과를 그대로 받는다.
  struct Step {
    const char* alert;   // 그 시점 VMS 가 표시하는 단계
    int expect;          // 팬에 나가야 하는 단계
    const char* why;
  };
  const Step steps[] = {
    { "clear",    0, "평상시" },
    { "warn",     1, "warn 진입 -> 75%" },
    { "critical", 2, "critical 승급 -> 90%" },
    // ↓ 여기가 핵심. 순간값은 warn 으로 떨어졌지만 경보는 critical 을
    //   유지한다(강등 플랩 방지). 팬도 90% 를 유지해야 한다.
    { "critical", 2, "순간값 강등 - 경보는 critical 유지 -> 팬도 90%" },
    { "critical", 2, "해제 밴드(warn*0.9) 위 - 여전히 critical" },
    { "clear",    0, "해제 밴드 아래 -> resolved -> 40%" },
  };

  for (const Step& s : steps) {
    tick(1, s.alert);
    if (last_published() != s.expect) {
      printf("  실패: %s — 경보 %s 인데 팬 단계 %d (기대 %d)\n",
             s.why, s.alert, last_published(), s.expect);
      return 1;
    }
    printf("  경보 %-9s -> 팬 단계 %d   (%s)\n", s.alert, s.expect, s.why);
  }

  // 3) 존이 여럿이면 가장 나쁜 단계를 따른다. 팬은 노드에 하나뿐이라
  //    존별로 다른 듀티를 낼 수 없다.
  g_published.clear();
  reportZoneLevel(1, fanLevelFromSeverityName("clear"));
  reportZoneLevel(2, fanLevelFromSeverityName("critical"));
  publishFanLevel();
  assert(last_published() == 2);
  reportZoneLevel(2, fanLevelFromSeverityName("clear"));
  publishFanLevel();
  assert(last_published() == 0);
  printf("여러 존: 가장 나쁜 단계를 따름 (2 -> 0)\n");

  // 4) 판단 불가(-1)는 정상(0)과 다르다. 정원을 아직 안 넣은 존만 있으면
  //    아무것도 보내지 않는다 — 0 을 보내면 AUTO 켜자마자 40% 로 돈다.
  g_published.clear();
  reportZoneLevel(1, -1);
  reportZoneLevel(2, -1);
  publishFanLevel();
  if (!g_published.empty()) {
    printf("  실패: 전 존 판단 불가인데 %d 를 발행함\n", last_published());
    return 1;
  }
  printf("전 존 판단 불가: 아무것도 발행하지 않음\n");

  // 5) 판단 가능한 존이 하나라도 있으면 그 값으로 간다.
  reportZoneLevel(2, fanLevelFromSeverityName("warn"));
  publishFanLevel();
  assert(last_published() == 1);
  printf("일부만 판단 가능: 그 존의 단계를 따름\n");

  printf("\nALL FAN LEVEL TESTS PASSED\n");
  return 0;
}
