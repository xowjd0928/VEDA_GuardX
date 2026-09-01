#include "wall_layout.h"
#include "channel_view.h"

#include <QGridLayout>
#include <QSet>
#include <QSettings>
#include <QStringList>

// 자리 순서 저장 키 — "2,0,1,3" = 좌상단부터 행 우선으로 놓일 채널 번호
static const char *ORDER_KEY = "wall_order";

namespace WallOrder {

// 메모리 캐시. ⚠ 이게 없으면 **그릴 때마다 레지스트리를 읽는다** —
// cell_of 는 동선 점 하나마다 불리는 그리기 경로(tracking_panel::to_widget_edge)
// 에 있어서, 250ms 갱신 × 점 수만큼 QSettings 를 새로 여는 꼴이었다.
// 쓰기(set_order)는 이 세션의 드래그뿐이라 캐시는 거기서만 갱신하면 된다.
static QList<int> g_cache;
static bool g_cached = false;

/** @brief QSettings 에서 읽어 검증한다 (캐시 미스일 때만) */
static QList<int> load_from_settings()
{
    QList<int> fallback;
    for (int ch = 0; ch < COUNT; ++ch)
        fallback.append(ch);

    // 저장값은 **유효한 순열일 때만** 쓴다 — 손상됐으면 조용히 기본으로
    // 되돌린다 (틀린 순서로 타일이 사라지는 것보다 배치 초기화가 낫다).
    const QStringList saved = QSettings("GuardX", "VMS")
                                  .value(ORDER_KEY).toString().split(',');
    if (saved.size() != COUNT)
        return fallback;

    QList<int> parsed;
    QSet<int> seen;
    for (const QString &s : saved) {
        bool ok = false;
        const int ch = s.toInt(&ok);
        if (!ok || ch < 0 || ch >= COUNT || seen.contains(ch))
            return fallback;
        parsed.append(ch);
        seen.insert(ch);
    }
    return parsed;
}

QList<int> order()
{
    if (!g_cached) {
        g_cache = load_from_settings();
        g_cached = true;
    }
    return g_cache;
}

int position_of(int channel)
{
    const int pos = order().indexOf(channel);
    return pos >= 0 ? pos : channel;
}

void cell_of(int channel, int &row, int &col)
{
    const int pos = position_of(channel);
    row = pos / 2;
    col = pos % 2;
}

void set_order(const QList<int> &new_order)
{
    if (new_order.size() != COUNT)
        return;
    QStringList parts;
    for (int ch : new_order)
        parts.append(QString::number(ch));
    QSettings("GuardX", "VMS").setValue(ORDER_KEY, parts.join(','));
    g_cache = new_order;      // 캐시는 쓰기 쪽에서 갱신 — 다시 읽지 않는다
    g_cached = true;
    notifier()->notify();
}

Notifier *notifier()
{
    static Notifier n;
    return &n;
}

} // namespace WallOrder

WallLayout::WallLayout(QGridLayout *grid, const QList<ChannelView *> &views,
                       QObject *parent)
    : QObject(parent), m_grid(grid), m_views(views)
{
}

void WallLayout::show_grid()
{
    const QList<int> order = WallOrder::order();
    for (int pos = 0; pos < order.size() && pos < m_views.size(); ++pos) {
        ChannelView *view = m_views[order[pos]];
        m_grid->addWidget(view, pos / 2, pos % 2);
        view->show();
    }
}

void WallLayout::swap_channels(int ch_a, int ch_b)
{
    QList<int> order = WallOrder::order();
    const int pa = order.indexOf(ch_a);
    const int pb = order.indexOf(ch_b);
    if (pa < 0 || pb < 0 || pa == pb)
        return;
    order.swapItemsAt(pa, pb);

    // 그리드 칸만 옮긴다. removeWidget이 먼저다 — 같은 위젯을 다른 칸에
    // addWidget만 하면 레이아웃 항목이 이중으로 남는다. 부모는 그대로라
    // 네이티브 창 재생성이 없고, 칸 크기가 같아 리사이즈도 없다.
    ChannelView *a = m_views[ch_a];
    ChannelView *b = m_views[ch_b];
    m_grid->removeWidget(a);
    m_grid->removeWidget(b);
    m_grid->addWidget(a, pb / 2, pb % 2);
    m_grid->addWidget(b, pa / 2, pa % 2);

    // 저장 + 알림 — TRACKING 엣지맵/개략도와 CAMERA 줌 그리드가 이 신호로
    // 같은 배치를 따라온다 (배치를 주장하는 화면이 셋이라 알림이 필요하다).
    WallOrder::set_order(order);
}
