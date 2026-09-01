#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

/**
 * @brief REPORT 탭 디자인 토큰 — design_handoff_reports_tab/README.md §Design Tokens
 *
 * Theme:: 전역 팔레트와 미세하게 다른 값들이다 (카드 #0d1219 vs Theme::panel
 * #0d1118 등). 핸드오프가 "High-fidelity, 픽셀 단위 재현"을 요구하므로 이
 * 페이지에 한해 핸드오프 원본 값을 쓰고, 다른 화면으로 새지 않게 여기 가둔다.
 */
namespace ReportTk {

// ⚠ const가 아니다 — Theme::set_mode()가 load_mode()로 값을 갈아끼운다.
// 색을 캡처해 두지 말고 쓸 때마다 읽을 것.
inline QColor pageBg    {0x06, 0x08, 0x0c};
inline QColor cardBg    {0x0d, 0x12, 0x19};
inline QColor border    {0x1c, 0x24, 0x30};
inline QColor gridLine  {0x1a, 0x21, 0x2c};
inline QColor ctrlBg    {0x1d, 0x26, 0x35};  ///< 활성 세그먼트/토글 배경
inline QColor ctrlBorder{0x2c, 0x3a, 0x4f};
inline QColor textHi    {0xdf, 0xe6, 0xef};
inline QColor textSec   {0x66, 0x71, 0x7f};
inline QColor textFaint {0x4d, 0x58, 0x66};
inline QColor chartLabel{0x5a, 0x65, 0x72};
inline QColor nowDivider{0x2b, 0x36, 0x48};
inline QColor blue      {0x5b, 0x9c, 0xf6};  ///< CH1 · 단일 차트 기본색
inline QColor blueLight {0x8a, 0xb8, 0xff};  ///< REFRESH 링크, horizon 열
inline QColor green     {0x3e, 0xcf, 0x8e};  ///< CH2 · HEADROOM · P(초과)
inline QColor purple    {0xb5, 0x8c, 0xf6};  ///< CH3
inline QColor amber     {0xe8, 0xa3, 0x3d};  ///< CH4 · NEAR CAP
inline QColor capLine   {0x8a, 0x6a, 0x2f};
inline QColor capLabel  {0xc9, 0x97, 0x4a};

/**
 * @brief 팔레트를 다크/라이트로 갈아끼운다 (Theme::set_mode가 부른다)
 *
 * REPORT 페이지는 핸드오프 HTML에서 온 자체 토큰을 쓴다. 앱 팔레트와 별개라
 * 여기도 두 벌이 필요하다 — 채널 구분색(blue/green/purple/amber)은 흰 배경
 * 대비를 위해 어둡게 누른 값을 쓴다.
 */
void load_mode(bool light);

inline QColor channel_color(int ch)
{
    switch (ch) {
    case 0: return blue;
    case 1: return green;
    case 2: return purple;
    default: return amber;
    }
}

} // namespace ReportTk

/**
 * @brief 이력 + 예측 분할 차트 (핸드오프 HTML의 chart() 수식을 QPainter로 포팅)
 *
 * - X축 분할: 좌 62% = 이력(선형), 우 38% = 예측(√스케일 — 가까운 horizon에
 *   폭을 몰아준다). 분할선에 "now" 라벨.
 * - Y축: 0 → ceil(1.25 × 최대값, 최소 3). 정원(30~80)이 실측(0~5)을 압도하므로
 *   Y를 정원까지 늘리지 않고, 상단에 고정 CAP 점선 + "CAP n ⌁"(축 절단 기호)로
 *   표시한다 — 핸드오프 명세 그대로.
 * - 단일 모드: 이력 면 채움 + p10–p90 밴드 + CAP 마커.
 *   비교 모드: 채널색 선만 (채움·밴드·CAP 없음).
 */
class ReportChart : public QWidget
{
    Q_OBJECT

public:
    struct FcPt {
        int minutes = 0;      ///< 5 / 30 / 60 / 180
        double p50 = -1;
        double p10 = -1;      ///< -1 = 미제공 (밴드 생략)
        double p90 = -1;
    };
    struct Series {
        QColor color;
        QVector<double> hist; ///< 과거 → 현재 순
        QVector<FcPt> fc;     ///< 비어 있으면 예측 미표시 (warmup 등)
        /**
         * @brief p10–p90 밴드 색 — 비우면 ReportTk::blueLight (기존 동작)
         *
         * REPORT 단일 카드는 늘 파랑 시리즈라 밴드도 파랑이면 맞았다.
         * ANALYTICS 는 구역색(초록·보라·황)으로 그리는데 밴드만 파랑으로
         * 남으면 카드 안에 설명 없는 두 번째 색이 생긴다.
         */
        QColor band;
    };

    explicit ReportChart(int min_height, QWidget *parent = nullptr);

    /** @brief 단일 채널 (그리드 카드) — cap > 0 이면 CAP 마커를 그린다 */
    void set_single(const Series &s, int cap, const QString &range_label);
    /** @brief 다채널 오버레이 (COMPARE) */
    void set_compare(const QVector<Series> &list, const QString &range_label);

protected:
    void paintEvent(QPaintEvent *ev) override;

private:
    QVector<Series> m_series;
    bool m_compare = false;
    int m_cap = 0;
    QString m_range_label;   ///< "-60m" / "-6h" / "-24h"
};
