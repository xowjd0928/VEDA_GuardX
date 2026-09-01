#include "report_chart.h"
#include "theme.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace ReportTk {

void load_mode(bool light)
{
    if (light) {
        // 4a 핸드오프 토큰 계열 (canvas/surface/hairline + primary #35618F).
        // 채널 구분색만 4개가 필요해 primary 외 3색을 흰 배경용으로 눌렀다.
        pageBg     = {0xF4, 0xF6, 0xF8};
        cardBg     = {0xFF, 0xFF, 0xFF};
        border     = {0xE3, 0xE7, 0xEC};
        gridLine   = {0xEE, 0xF1, 0xF4};
        ctrlBg     = {0xEA, 0xF0, 0xF7};
        ctrlBorder = {0xD3, 0xD9, 0xE0};
        textHi     = {0x16, 0x19, 0x1D};
        textSec    = {0x3F, 0x46, 0x4E};
        textFaint  = {0x7D, 0x85, 0x8E};
        chartLabel = {0x5C, 0x64, 0x6D};
        nowDivider = {0xD3, 0xD9, 0xE0};
        blue       = {0x35, 0x61, 0x8F};  ///< primary
        blueLight  = {0x2B, 0x54, 0x80};  ///< 링크 — 흰 배경에선 더 진하게
        green      = {0x15, 0x7F, 0x52};
        purple     = {0x6D, 0x4A, 0xA8};
        amber      = {0xA0, 0x6A, 0x00};
        capLine    = {0xC0, 0x8A, 0x2E};
        capLabel   = {0x8A, 0x5F, 0x17};
    } else {
        // 08-19 워크스페이스: 다크는 자체 핸드오프 토큰을 버리고 **앱
        // 팔레트를 그대로** 쓴다 (Theme::load_dark 다음에 불리므로 값이 항상
        // 현재 팔레트다). 목적은 색 언어 통일 — 차트 선색이 Device 탭의
        // MiniLineChart·게이지와 같은 파랑/초록/보라/황이 된다.
        // (라이트는 4a 핸드오프 값 유지 — 라이트 accent 는 브랜드 오렌지라
        //  차트 선에 쓰면 "오렌지 = 브랜드 전용" 규칙을 깬다)
        pageBg     = Theme::bg0;
        cardBg     = Theme::panel;
        border     = Theme::border;
        gridLine   = Theme::rowDivider;
        ctrlBg     = Theme::elevated2;
        ctrlBorder = Theme::border2;
        textHi     = Theme::textHi;
        textSec    = Theme::textMuted;
        textFaint  = Theme::textDim;
        chartLabel = Theme::textDim;
        nowDivider = Theme::border2;
        blue       = Theme::accent;        ///< CH1 · 단일 차트 = 정보색 파랑
        blueLight  = Theme::accentHover;
        green      = Theme::green;         ///< CH2 — Device 게이지·차트와 동일
        purple     = {0xB4, 0x8C, 0xFF};   ///< CH3 — 추적 팔레트의 보라와 동일
        amber      = Theme::amber;         ///< CH4 · NEAR CAP
        capLine    = Theme::amber.darker(160);
        capLabel   = Theme::amber;
    }
}

} // namespace ReportTk

namespace {

// 캔버스 패딩 (핸드오프 §Chart specification — 축 라벨이 이 여백에 산다)
const double PAD_L = 30, PAD_R = 14, PAD_T = 22, PAD_B = 18;

/** @brief 픽셀 단위 점선 — Qt 대시 패턴은 펜 굵기 배수라 나눠서 넣는다 */
QPen dashed_pen(const QColor &c, double width, double dash, double gap)
{
    QPen pen(c, width);
    pen.setDashPattern({ dash / width, gap / width });
    return pen;
}

} // namespace

ReportChart::ReportChart(int min_height, QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(min_height);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void ReportChart::set_single(const Series &s, int cap, const QString &range_label)
{
    m_series = { s };
    m_compare = false;
    m_cap = cap;
    m_range_label = range_label;
    update();
}

void ReportChart::set_compare(const QVector<Series> &list, const QString &range_label)
{
    m_series = list;
    m_compare = true;
    m_cap = 0;
    m_range_label = range_label;
    update();
}

void ReportChart::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const double W = width(), H = height();
    const double iw = W - PAD_L - PAD_R;
    const double ih = H - PAD_T - PAD_B;
    if (iw < 20 || ih < 20)
        return;

    const double split = PAD_L + iw * 0.62;
    const auto fx = [&](double minutes) {
        return split + (iw * 0.38 - 8) * std::sqrt(minutes / 180.0);
    };

    // Y 스케일 — 그려질 모든 값(이력 + p50/p90)의 최대. 최소 3 → 빈 방에서도
    // 축이 0~4로 서 있어 선이 바닥에 붙어 보이지 않는다.
    double vmax = 3.0;
    for (const Series &s : m_series) {
        for (double v : s.hist)
            vmax = qMax(vmax, v);
        for (const FcPt &f : s.fc) {
            vmax = qMax(vmax, f.p50);
            vmax = qMax(vmax, f.p90);
        }
    }
    const double y_max = std::ceil(vmax * 1.25);
    const auto Y = [&](double v) { return PAD_T + ih * (1.0 - v / y_max); };

    const QFont f9 = Theme::mono_font(9);
    const QFontMetricsF fm(f9);
    p.setFont(f9);

    // ── 격자 3줄 (0 · 중간 · 최대) + 좌측 라벨 ──
    for (double t : { 0.0, 0.5, 1.0 }) {
        const double gy = PAD_T + ih * t;
        p.setPen(QPen(ReportTk::gridLine, 1));
        p.drawLine(QPointF(PAD_L, gy), QPointF(W - PAD_R, gy));

        const QString lb = QString::number(qRound(y_max * (1.0 - t)));
        p.setPen(ReportTk::chartLabel);
        p.drawText(QPointF(PAD_L - 6 - fm.horizontalAdvance(lb), gy + 3), lb);
    }

    // ── now 분할선 + X축 라벨 ──
    p.setPen(dashed_pen(ReportTk::nowDivider, 1, 2, 3));
    p.drawLine(QPointF(split, PAD_T - 4), QPointF(split, PAD_T + ih));

    p.setPen(ReportTk::chartLabel);
    p.drawText(QPointF(split - fm.horizontalAdvance("now") / 2, H - 4), "now");
    p.drawText(QPointF(PAD_L, H - 4), m_range_label);
    const struct { double m; const char *lb; } ticks[] = {
        { 5, "+5" }, { 30, "+30" }, { 60, "+60" }, { 180, "+180m" },
    };
    for (const auto &t : ticks) {
        const QString lb = QString::fromLatin1(t.lb);
        p.drawText(QPointF(fx(t.m) - fm.horizontalAdvance(lb) / 2, H - 4), lb);
    }

    // ── 시리즈 ──
    // 선 굵기: 보드(Report.dc.html)는 viewBox 640×150 을
    // preserveAspectRatio="none" 로 카드에 늘린다 — stroke-width="2" 가
    // 세로 배율(플롯높이/150 ≒ 2)만큼 같이 늘어나 실제로는 약 4px 이다.
    // DEVICE 스파크라인과 **같은 함정**이라(속성값을 그대로 픽셀로 읽음)
    // 같은 방식으로 높이에 비례시킨다.
    const double line_w = qBound(2.0, ih * (2.0 / 150.0), 6.0);
    for (const Series &s : m_series) {
        const int n = s.hist.size();
        const auto hx = [&](int k) {
            return n > 1 ? PAD_L + (split - PAD_L) * double(k) / (n - 1) : split;
        };

        QPainterPath hist_path;
        if (n > 0) {
            hist_path.moveTo(hx(0), Y(s.hist[0]));
            for (int k = 1; k < n; ++k)
                hist_path.lineTo(hx(k), Y(s.hist[k]));
        }

        if (!m_compare && n > 0) {
            QPainterPath area = hist_path;
            area.lineTo(split, Y(0));
            area.lineTo(PAD_L, Y(0));
            area.closeSubpath();
            // 면 채움은 **시리즈색**의 10% — 주석과 달리 blue 고정이었다.
            // REPORT 단일 카드는 항상 blue 시리즈라 결과가 같고, ANALYTICS
            // 구역 카드(초록/보라/황 시리즈)에서만 차이가 드러난다.
            QColor fill = s.color;
            fill.setAlpha(26);
            p.fillPath(area, fill);
        }
        if (n > 0) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(s.color, line_w, Qt::SolidLine, Qt::RoundCap,
                          Qt::RoundJoin));
            p.drawPath(hist_path);
        }

        if (s.fc.isEmpty())
            continue;

        // 예측은 마지막 실측점에서 이어 그린다 (이력이 없으면 첫 예측점부터)
        const double x0 = split;
        const double y0 = n > 0 ? Y(s.hist.last()) : Y(qMax(0.0, s.fc.first().p50));

        bool has_band = !m_compare;
        for (const FcPt &f : s.fc)
            has_band = has_band && f.p10 >= 0 && f.p90 >= 0;
        if (has_band) {
            QPainterPath band;
            band.moveTo(x0, y0);
            for (const FcPt &f : s.fc)
                band.lineTo(fx(f.minutes), Y(f.p90));
            for (int k = s.fc.size() - 1; k >= 0; --k)
                band.lineTo(fx(s.fc[k].minutes), Y(s.fc[k].p10));
            band.closeSubpath();
            // p10–p90 밴드 10% — 시리즈가 밴드색을 주면 그것, 아니면 기존값
            QColor band_fill = s.band.isValid() ? s.band : ReportTk::blueLight;
            band_fill.setAlpha(26);
            p.fillPath(band, band_fill);
        }

        QPainterPath fc_path;
        fc_path.moveTo(x0, y0);
        for (const FcPt &f : s.fc)
            fc_path.lineTo(fx(f.minutes), Y(f.p50));
        QColor fc_color = s.color;
        fc_color.setAlphaF(0.9);
        p.setBrush(Qt::NoBrush);
        p.setPen(dashed_pen(fc_color, line_w, 4, 3));
        p.drawPath(fc_path);

        p.setPen(Qt::NoPen);
        p.setBrush(s.color);
        for (const FcPt &f : s.fc)
            p.drawEllipse(QPointF(fx(f.minutes), Y(f.p50)), 2.4, 2.4);
    }

    // ── CAP 마커 (단일 모드) — Y를 정원까지 늘리는 대신 상단 고정 + ⌁ 축절단 ──
    if (!m_compare && m_cap > 0) {
        p.setPen(dashed_pen(ReportTk::capLine, 1, 5, 4));
        p.drawLine(QPointF(PAD_L, PAD_T + 2), QPointF(W - PAD_R, PAD_T + 2));

        const QString lb =
            QString::fromUtf8("CAP %1 ⌁").arg(m_cap);   // U+2301 ⌁
        p.setPen(ReportTk::capLabel);
        p.drawText(QPointF(W - PAD_R - fm.horizontalAdvance(lb), PAD_T - 5), lb);
    }
}
