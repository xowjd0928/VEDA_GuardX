#pragma once

#include <QPixmap>
#include <QRectF>

class QPainter;
#include <QString>

/**
 * @brief 평면도 배경 그림 — CROWD·LIVE 두 지도가 같은 파일을 깐다 (2026-08-24)
 *
 * 지금까지 바탕은 1m 격자 + 방 외곽선뿐이라 "어디가 입구고 어디가 벽인지"를
 * 도면으로 읽을 수 없었다. 실제 도면 이미지를 깔면 히트맵·동선이 그 위에
 * 얹혀 위치가 한눈에 읽힌다.
 *
 * ⭐ **리소스에 굽지 않고 파일에서 읽는다.** 현장마다 도면이 다르고, 사용자가
 *   직접 갈아 끼울 수 있어야 한다는 요구(08-24)라 실행 파일에 넣으면 안 된다.
 *   교체 방법은 docs/FLOOR_PLAN_IMAGE.md.
 *
 * 좌표 규약: 그림은 **방 전체**(0,0)-(room_w_cm, room_h_cm)에 딱 맞게 늘린다.
 * 즉 도면의 네 모서리가 방의 네 모서리다 — 여백이 있는 도면을 넣으면 그
 * 여백만큼 히트맵이 안쪽으로 밀린다. 잘라서 넣을 것.
 */
namespace FloorPlan {

/** @brief 도면 파일 경로 (QSettings `floorplan_path`, 기본 자격 폴더/floorplan.png) */
QString path();

/**
 * @brief 도면 그림 (없으면 null)
 *
 * 파일의 수정 시각·크기를 보고 **바뀌었으면 다시 읽는다** — 앱을 켠 채
 * 파일만 갈아 끼워도 다음 그리기에 반영된다(탭을 다시 열거나 250ms 틱).
 */
QPixmap image();

/**
 * @brief 가구 한 칸을 **기호로** 그린다 (2026-08-25)
 *
 * 전부 빈 네모로 그리면 "무엇이 있는지"가 안 읽힌다(사용자 지적). 도면
 * 관례대로 가구마다 다른 기호를 준다 — 진열장은 선반 칸, 벤치는 좌석 분할,
 * 카운터는 상판 위 물건, 의자는 등받이.
 *
 * 기호는 **이름과 모양**으로 고른다(캘리브레이션 JSON 의 name + 발자국 비율).
 * 이름을 못 알아보면 빈 네모로 떨어진다 — 새 가구가 생겨도 그림이 깨지지 않고,
 * 기호를 주고 싶으면 이 함수에 한 줄 더하면 된다.
 *
 * @param box  칸의 사각형 (위젯 좌표)
 * @param name 캘리브레이션 가구 이름
 * @param ink  선 색 (호출부가 신뢰도에 따라 흐리게 넘긴다)
 * @param dashed 좌표를 못 믿는 채널 표시 (frame mismatch)
 */
void draw_fixture(QPainter &p, const QRectF &box, const QString &name,
                  const QColor &ink, bool dashed);

/** @brief 가구 사진 폴더 (`<자격 폴더>/props`) — 사용자가 직접 채운다 */
QString props_dir();

/**
 * @brief 가구 한 칸에 입힐 **사용자 지정 사진** (없으면 null)
 *
 * 네모마다 다른 사진을 붙일 수 있어야 한다는 요구(08-25). 키를 **번호**로
 * 잡는 이유는 이름이 겹치기 때문이다 — 현장 데이터에 `가판대` 가 6개,
 * `의자` 가 2개다. 번호는 캘리브레이션 obstacles 의 순서이고, 그 대응표는
 * `props/_fixtures.txt` 에 자동으로 적힌다.
 *
 * 찾는 순서 (먼저 있는 것이 이긴다):
 *   1. `props/<번호>.png`            — 그 칸 하나 (예: `3.png`)
 *   2. `props/CH<채널>-<이름>.png`   — 채널 안의 같은 이름 전부
 *   3. `props/<이름>.png`            — 같은 이름 전부 (예: `가판대.png`)
 * 확장자는 png·jpg·jpeg·webp 순으로 본다.
 *
 * @param index 1부터 세는 가구 번호 (obstacles 순서)
 */
QPixmap prop_image(int index, int channel, const QString &name);

/** @brief 지도 위에 가구 번호를 겹쳐 보여줄까 (`floorplan_prop_keys`, 기본 꺼짐) */
bool show_prop_keys();

/**
 * @brief 현재 가구 목록을 `props/_fixtures.txt` 로 적는다 (바뀌었을 때만)
 *
 * 사용자가 폴더를 열면 "몇 번이 어느 네모인지"가 거기 있다. 캘리브레이션이
 * 바뀌면 번호도 바뀌므로 자동 갱신한다.
 */
void refresh_fixture_list();

/**
 * @brief 도면 위에 캘리브레이션 가구(obstacles)도 그릴까
 *
 * 기본은 **안 그린다** — 도면에 이미 가구가 그려져 있어 두 겹이 되면 서로
 * 싸운다. 캘리브레이션 좌표를 눈으로 검증할 때만 QSettings
 * `floorplan_show_obstacles=1` 로 켠다. 도면이 없으면 이 값과 무관하게
 * 예전처럼 그린다(그때는 가구가 유일한 지형지물이다).
 */
bool show_obstacles();

}  // namespace FloorPlan
