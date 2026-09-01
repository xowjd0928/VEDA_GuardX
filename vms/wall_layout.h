#pragma once

#include <QList>
#include <QObject>

class ChannelView;
class QGridLayout;

/**
 * @brief 2×2 벽의 **자리 순서** — 채널이 어느 칸에 놓이는지의 단일 진실원천
 *
 * 08-20 드래그 앤 드롭으로 배치가 움직이게 되면서, 같은 2×2 를 그리는 화면이
 * 셋이 됐다: LIVE 벽 · TRACKING 엣지맵/개략도 · CAMERA ▸ Image 줌 그리드.
 * 각자 `ch/2, ch%2` 로 자리를 계산하면 벽만 움직이고 나머지는 옛 배치에 남아
 * **같은 화면이 서로 다른 배치를 주장한다** (실제로 엣지맵은 배경이 벽 배치로
 * 합성되는데 그 위 점·라벨은 고정 배치라 스왑 후 어긋났다 — 사용자 신고).
 *
 * 그래서 순서를 여기 한 곳에 두고 셋이 모두 읽는다. 저장은
 * QSettings("wall_order") — "2,0,1,3" = 좌상단부터 행 우선.
 */
namespace WallOrder {

/** @brief 2×2 = 채널 4개 */
static const int COUNT = 4;

/** @brief 자리(행 우선) -> 채널. 저장값이 유효한 순열이 아니면 기본 순서 */
QList<int> order();

/** @brief 채널 -> 자리. 못 찾으면 채널 번호를 그대로 (기본 배치와 같다) */
int position_of(int channel);

/** @brief 채널 -> 2×2 행/열 (position_of 의 행/열 표현) */
void cell_of(int channel, int &row, int &col);

/** @brief 순서 교체 + 저장 + 알림 (드래그 앤 드롭이 부른다) */
void set_order(const QList<int> &order);

/** @brief 배치 변경 알림 — 화면들이 구독한다 (ZoneConfig::Notifier 와 같은 꼴) */
class Notifier : public QObject
{
    Q_OBJECT

public:
    /** @brief 순서를 바꾼 쪽이 부른다 (signals 는 밖에서 emit 할 수 없다) */
    void notify() { emit changed(); }

signals:
    void changed();
};

/** @brief connect(WallOrder::notifier(), &WallOrder::Notifier::changed, ...) */
Notifier *notifier();

} // namespace WallOrder

/**
 * @brief 4채널 벽 배치 (고정 2×2 그리드)
 *
 * live_viewer.cpp에 레이아웃·재생·통계·타임라인이 뭉쳐 있던 것에서
 * **배치만** 분리했다 (VMS_CODE_MAP §5 부채, 07-31). 전체화면(FOCUS) 전환은
 * 08-19 디자인 개편으로 삭제 — 배치는 2×2 그리드 하나뿐이다.
 *
 * 08-20 드래그 앤 드롭 배치 이동: 자리 순서는 WallOrder 가 들고, 스왑은 두
 * 타일의 그리드 칸만 맞바꾼다 — 위젯 재부모화가 없으므로 direct 경로의
 * 네이티브 영상 창(HWND)과 RTSP 세션은 건드리지 않는다.
 */
class WallLayout : public QObject
{
    Q_OBJECT

public:
    WallLayout(QGridLayout *grid, const QList<ChannelView *> &views,
               QObject *parent = nullptr);

    /** @brief 2×2 그리드 배치 (초기 배치 겸용) — 저장된 자리 순서를 따른다 */
    void show_grid();

    /** @brief 두 채널의 타일 자리를 맞바꾼다 (드래그 앤 드롭) + 순서 저장 */
    void swap_channels(int ch_a, int ch_b);

private:
    QGridLayout *m_grid;
    QList<ChannelView *> m_views;
};
