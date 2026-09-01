#pragma once

#include <QString>

class QWidget;

/**
 * @brief 패널 공통 크롬 (헤더 행)
 *
 * live_viewer.cpp 안의 static 함수였다. TRACKING 패널이 같은 헤더를 쓰면서
 * 두 번째 호출자가 생겼으므로 여기로 옮겼다 — 복사해두면 두 파일에서
 * 헤더 높이·색이 따로 흘러간다.
 */
namespace PanelChrome {

/**
 * @brief 패널 헤더 행 (29px + 1px 밑줄 borderDim)
 * @param caption  제목 오른쪽 모노 캡션 (비우면 생략)
 * @param trailing 우측 정렬로 붙일 위젯 (없으면 nullptr) — LIVE 점 등
 */
QWidget *header(const QString &title, const QString &caption, QWidget *parent,
                QWidget *trailing = nullptr);

} // namespace PanelChrome
