// track_plan_test.cpp - LED 좌표 변환과 실제 전송값 검증. DB·브로커·하드웨어 불필요.
//
// 빌드: cmake -B build && cmake --build build -j   ->  ./build/track_plan_test
// 단독: g++ -std=c++17 -Iinclude test/track_plan_test.cpp -o track_plan_test
//
// 확인하는 것:
//   1. 채널 -> 평면도 사분면 배치와 여백
//   2. 화면 밖 좌표의 클램프 (STM32 는 1000 초과 프레임을 통째로 거절한다)
//   3. 예측점을 그릴지 말지 (거리·direction=STOP)
//   4. RPi C 가 실제로 받는 JSON 한 글자까지
//
// 마지막에 대표 좌표의 전송값 표를 찍는다 - 현장 LED 없이 "어디에 찍히는가"를
// 눈으로 확인하기 위한 것이다.
#include "MqttDb/track_plan.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using namespace guardx::track_plan;

namespace {

struct XY { int x; int y; };

XY plan(int ch, double px, double py) {
  XY p{-1, -1};
  const bool ok = toPlan(ch, px, py, p.x, p.y);
  assert(ok);
  return p;
}

const double CX = FRAME_W / 2.0;   // 화면 정중앙
const double CY = FRAME_H / 2.0;

}  // namespace

int main() {
  // 1) 사분면 배치 — 화면 정중앙은 자기 칸의 정중앙에 찍힌다.
  //    표가 틀리면 사람이 엉뚱한 방에 표시된다. QUADRANT 만 고치면 되는
  //    자리이므로, 여기서 값이 깨지면 그 표를 의심할 것.
  assert((plan(0, CX, CY).x == 250 && plan(0, CX, CY).y == 250));   // 좌상
  assert((plan(1, CX, CY).x == 750 && plan(1, CX, CY).y == 250));   // 우상
  assert((plan(2, CX, CY).x == 250 && plan(2, CX, CY).y == 750));   // 좌하
  assert((plan(3, CX, CY).x == 750 && plan(3, CX, CY).y == 750));   // 우하
  printf("quadrant: ch0~3 centre -> (250,250) (750,250) (250,750) (750,750)\n");

  // 2) 칸 안쪽 여백 — 벽에 딱 붙지 않는다 (칸의 10~90%).
  assert((plan(0, 0, 0).x == MARGIN && plan(0, 0, 0).y == MARGIN));
  assert(plan(0, FRAME_W, FRAME_H).x == MARGIN + SPAN);
  assert(plan(3, 0, 0).x == CELL + MARGIN);
  assert(plan(3, FRAME_W, FRAME_H).y == CELL + MARGIN + SPAN);
  printf("margin: cell inset %d, span %d (max coord %d)\n",
         MARGIN, SPAN, CELL + MARGIN + SPAN);

  // 3) 화면 밖 좌표 클램프. 카메라가 프레임 밖 중심점을 줄 때가 있고,
  //    1000 을 넘긴 프레임은 STM32 가 값 범위 예외로 통째로 버린다.
  assert(plan(3, 1e9, 1e9).x <= COORD_MAX);
  assert(plan(3, 1e9, 1e9).y <= COORD_MAX);
  assert((plan(0, -5000, -5000).x == MARGIN && plan(0, -5000, -5000).y == MARGIN));
  printf("clamp: off-frame pixels stay within 0..%d\n", COORD_MAX);

  // 4) 범위 밖 채널은 변환하지 않는다 (프레임을 아예 만들지 않는다).
  {
    int x = 0, y = 0;
    assert(!toPlan(-1, CX, CY, x, y));
    assert(!toPlan(CHANNEL_COUNT, CX, CY, x, y));
  }

  // 5) 예측점 없음 -> A 만.
  {
    Frame f;
    assert(buildFrame(0, CX, CY, false, 0, 0, "UNKNOWN", f));
    assert(f.status == STATUS_A);
    assert(f.bx == 0 && f.by == 0);
  }

  // 6) 예측점이 충분히 떨어져 있으면 A|B.
  //    +200px 는 평면도로 약 31 칸이라 MIN_AB_GAP(25) 를 넘는다.
  Frame far_frame;
  {
    assert(buildFrame(0, CX, CY, true, CX + 200, CY, "MOVING", far_frame));
    assert(far_frame.status == (STATUS_A | STATUS_B));
    assert(far_frame.ax == 250 && far_frame.ay == 250);
    assert(far_frame.bx == 281 && far_frame.by == 250);
  }

  // 7) 너무 가까우면 예측점을 버린다. 카메라는 예측이 없을 때
  //    predicted 를 현재 위치로 채우므로 NULL 검사만으로는 못 거른다.
  {
    Frame f;
    assert(buildFrame(0, CX, CY, true, CX + 50, CY, "MOVING", f));
    assert(f.status == STATUS_A);
    Frame same;
    assert(buildFrame(0, CX, CY, true, CX, CY, "MOVING", same));
    assert(same.status == STATUS_A);
  }

  // 8) 멈춰 선 대상은 방향점을 그리지 않는다.
  {
    Frame f;
    assert(buildFrame(0, CX, CY, true, CX + 200, CY, "STOP", f));
    assert(f.status == STATUS_A);
  }

  // 9) 지우기 프레임 — status 0 이면 STM32 가 두 점을 즉시 지운다.
  assert(clearFrame().status == 0);

  // 10) RPi C 가 파싱하는 문자열 그대로. 필드가 하나라도 빠지거나 이름이
  //     바뀌면 matrix_link.c 가 조용히 버린다 - 그래서 문자열째 고정한다.
  {
    const std::string payload = buildPayload(1234567890123LL, 42, far_frame);
    const std::string want =
        "{\"node_id\":\"rpib\",\"timestamp\":1234567890123,\"seq\":42,"
        "\"status\":3,\"ax\":250,\"ay\":250,\"bx\":281,\"by\":250}";
    if (payload != want) {
      printf("payload mismatch\n  got : %s\n  want: %s\n",
             payload.c_str(), want.c_str());
      return 1;
    }
    printf("payload: %s\n", payload.c_str());
  }

  // 대표 좌표 표 — 하드웨어 없이 "어느 칸 어디에 찍히는가"를 눈으로 확인.
  printf("\n  ch  pixel(x,y)          -> plan(x,y)\n");
  const struct { int ch; double px; double py; } kSamples[] = {
      {0, 0, 0}, {0, CX, CY}, {0, FRAME_W, FRAME_H},
      {1, 0, 0}, {1, CX, CY}, {1, FRAME_W, FRAME_H},
      {2, CX, CY}, {3, CX, CY}, {3, FRAME_W, FRAME_H},
  };
  for (const auto& s : kSamples) {
    const XY p = plan(s.ch, s.px, s.py);
    printf("  %2d  (%7.1f,%7.1f)  -> (%4d,%4d)\n", s.ch, s.px, s.py, p.x, p.y);
  }

  printf("\nALL TRACK PLAN TESTS PASSED\n");
  return 0;
}
