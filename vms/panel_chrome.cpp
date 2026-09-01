#include "panel_chrome.h"
#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

namespace PanelChrome {

QWidget *header(const QString &title, const QString &caption, QWidget *parent,
                QWidget *trailing)
{
    auto *box = new QWidget(parent);
    auto *lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // 08-19 보드 .chead — 제목은 산세리프 12/600 textHi, 부제는 모노 10
    // textMuted. 전엔 대문자 모노 11/600 이었는데, SETTINGS 새 카드들과
    // 두 언어가 섞여 "폰트가 통일되지 않았다"는 피드백을 받았다. 카드
    // 머리글은 앱 전체가 이 함수 하나를 쓰므로 여기만 고치면 다 따라온다.
    auto *row = new QWidget(box);
    row->setFixedHeight(40);
    auto *row_lay = new QHBoxLayout(row);
    row_lay->setContentsMargins(20, 0, 20, 0);
    row_lay->setSpacing(14);

    auto *title_lbl = new QLabel(title, row);
    title_lbl->setFont(Theme::ui_font(12, 600));
    Theme::restyle(title_lbl, [] {
        return QString("color:%1;").arg(Theme::textHi.name());
    });
    row_lay->addWidget(title_lbl);

    if (!caption.isEmpty()) {
        auto *cap_lbl = new QLabel(caption, row);
        cap_lbl->setFont(Theme::mono_font(10));
        Theme::restyle(cap_lbl, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        row_lay->addWidget(cap_lbl);
    }
    row_lay->addStretch(1);
    if (trailing) {
        trailing->setParent(row);
        row_lay->addWidget(trailing);
    }
    lay->addWidget(row);

    auto *line = new QWidget(box);
    line->setObjectName("PanelHeaderLine");
    line->setAttribute(Qt::WA_StyledBackground);
    line->setFixedHeight(1);
    lay->addWidget(line);
    return box;
}

} // namespace PanelChrome
