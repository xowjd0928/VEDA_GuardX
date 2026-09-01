#pragma once

#include "theme.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

/**
 * @brief 미구현 화면 자리표시 페이지
 *
 * 남은 것은 DEVICE 하나다 — RPi/STM32 제어 배선이 VMS에 아직 없어
 * 내비게이션만 되게 스텁으로 둔다 (스펙 §2).
 * 전체 디자인은 docs/GuardX VMS.dc.html에 보존돼 있다.
 */
class PlaceholderPage : public QWidget
{
public:
    explicit PlaceholderPage(const QString &title, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        lay->addStretch(3);

        auto *heading = new QLabel(title.toUpper(), this);
        heading->setFont(Theme::ui_font(15, 700, 0.14));
        heading->setAlignment(Qt::AlignCenter);
        Theme::restyle(heading, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        lay->addWidget(heading);

        lay->addSpacing(10);

        auto *sub = new QLabel(
            QStringLiteral("screen pending · pending backend data"), this);
        sub->setFont(Theme::mono_font(11));
        sub->setAlignment(Qt::AlignCenter);
        Theme::restyle(sub, [] {
            return QString("color:%1;").arg(Theme::textDim.name());
        });
        lay->addWidget(sub);

        lay->addStretch(4);
    }
};
