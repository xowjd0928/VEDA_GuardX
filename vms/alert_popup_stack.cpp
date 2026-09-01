#include "alert_popup_stack.h"

#include <QEvent>
#include <QWidget>

#include <algorithm>

AlertPopupStack *AlertPopupStack::instance()
{
    static AlertPopupStack stack;
    return &stack;
}

AlertPopupStack::AlertPopupStack(QObject *parent) : QObject(parent) {}

void AlertPopupStack::add(QWidget *popup, Order order)
{
    if (!popup)
        return;

    m_slots.append({ popup, int(order) });
    std::stable_sort(m_slots.begin(), m_slots.end(),
                     [](const Slot &a, const Slot &b) { return a.order < b.order; });

    // 팝업이 스스로 재배치를 부르지 않아도 어긋나지 않게 직접 관찰한다.
    popup->installEventFilter(this);

    // 장부에 죽은 포인터가 남지 않게. QHash<QWidget*,...> 로 관리하면 여기서
    // 지우는 것을 잊는 순간 dangling 이 되는데, 그 실수를 없애는 쪽을 택한다.
    connect(popup, &QObject::destroyed, this, [this](QObject *obj) {
        for (int i = 0; i < m_slots.size(); ++i) {
            if (m_slots[i].widget == obj) {
                m_slots.remove(i);
                break;
            }
        }
        relayout();
    });
}

AlertPopupStack::Slot *AlertPopupStack::find(QObject *w)
{
    for (Slot &s : m_slots)
        if (s.widget == w)
            return &s;
    return nullptr;
}

void AlertPopupStack::set_gated(bool gated)
{
    if (m_gated == gated)
        return;
    m_gated = gated;

    if (gated) {
        // 이미 떠 있던 것도 숨긴다 — 로그아웃하면 화면에 남아 있으면 안 된다.
        for (Slot &s : m_slots) {
            if (!s.widget || !s.widget->isVisible())
                continue;
            s.suppressed = true;
            m_forcing_hide = true;
            s.widget->hide();
            m_forcing_hide = false;
        }
        qInfo() << "[AlertPopupStack] 경보 팝업 잠금 (로그인 전)";
        return;
    }

    int restored = 0;
    for (Slot &s : m_slots) {
        if (!s.suppressed || !s.widget)
            continue;
        s.suppressed = false;
        // 내부 상태(미확인 여부·문구)는 보류 동안 그대로였다 — 그대로 띄운다.
        s.widget->show();
        s.widget->raise();
        ++restored;
    }
    qInfo() << "[AlertPopupStack] 경보 팝업 잠금 해제 — 보류됐던 팝업" << restored << "건";
    relayout();
}

bool AlertPopupStack::try_show(QWidget *popup)
{
    Slot *s = find(popup);
    if (!m_gated) {
        popup->show();
        popup->raise();
        return true;
    }

    // 잠금 중 — 창을 만들지 않는다. 팝업의 미확인 상태는 건드리지 않는다
    // (여기서 dismiss 를 대신 해 주면 그 경보는 영영 사라진다).
    if (s && !s->suppressed) {
        s->suppressed = true;
        qInfo() << "[AlertPopupStack] 로그인 전 경보 팝업 보류 —"
                << popup->metaObject()->className();
    }
    return false;
}

bool AlertPopupStack::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::Show:
        if (m_gated) {
            // 백스톱 — try_show 를 안 거치고 뜬 팝업(새로 추가되며 빠뜨린
            // 경우)이 여기서 걸린다. 정상 경로에서는 오지 않는다.
            if (Slot *s = find(watched)) {
                s->suppressed = true;
                m_forcing_hide = true;
                s->widget->hide();
                m_forcing_hide = false;
                qWarning() << "[AlertPopupStack] try_show 를 안 거친 팝업을 숨김 —"
                           << watched->metaObject()->className();
            }
            break;
        }
        relayout();
        break;
    case QEvent::Hide:
        // 팝업이 **스스로** 닫힌 것이면 보류 표시를 지운다(혼잡 해제 등) —
        // 그래야 로그인 뒤에 이미 끝난 경보가 되살아나지 않는다.
        if (!m_forcing_hide) {
            if (Slot *s = find(watched))
                s->suppressed = false;
        }
        relayout();
        break;
    case QEvent::Resize:
        // Resize 도 본다 — 화재 팝업은 내용에 따라 높이가 변해서(adjustSize),
        // 그 아래 팝업이 밀리거나 떠야 한다.
        relayout();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void AlertPopupStack::relayout()
{
    // move()는 Move 이벤트만 내므로 위 필터로 되돌아오지 않는다. 다만 앞으로
    // 다른 이벤트를 보게 될 수도 있어 재진입만 막아둔다 — 여기서 무한루프가
    // 나면 원인을 찾기 어렵다.
    if (m_laying_out)
        return;
    m_laying_out = true;

    int y = -1;
    int orphan_y = FIRST_TOP;
    for (const Slot &slot : m_slots) {
        QWidget *w = slot.widget;
        if (!w || !w->isVisible())
            continue;

        QWidget *anchor = w->parentWidget() ? w->parentWidget()->window() : nullptr;
        if (!anchor) {
            w->move(ORPHAN_X, orphan_y);
            orphan_y += w->height() + GAP;
            continue;
        }

        const QRect g = anchor->geometry();
        if (y < 0)
            y = g.top() + FIRST_TOP;
        w->move(g.center().x() - w->width() / 2, y);
        y += w->height() + GAP;
    }

    m_laying_out = false;
}
