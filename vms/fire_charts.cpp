#include "fire_charts.h"
#include "theme.h"

#include <QPainter>

namespace {

/// Qt 각도는 1/16도 단위, 0 = 3시 방향, 반시계가 양수.
/// 게이지는 7시 반(225°)에서 시작해 시계방향으로 270° 돈다 — 아래쪽이
/// 트인 U자라 중앙 숫자를 넣을 자리가 생긴다.
constexpr int ARC_START = 225 * 16;
constexpr int ARC_SPAN  = -270 * 16;

} // namespace

// ------------------------------------------------------------- RiskGauge

RiskGauge::RiskGauge(int diameter, QWidget *parent)
    : QWidget(parent), m_diameter(diameter)
{
    // 캡션 한 줄이 도넛 아래로 들어갈 여유를 준다
    setFixedSize(diameter, diameter + 14);
}

void RiskGauge::set_score(double score, double threshold)
{
    m_score = score;
    m_threshold = threshold;
    update();
}

void RiskGauge::set_caption(const QString &caption)
{
    m_caption = caption;
    update();
}

void RiskGauge::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int thick = qMax(4, m_diameter / 9);
    const QRectF ring(thick / 2.0, thick / 2.0,
                      m_diameter - thick, m_diameter - thick);

    const bool frozen = m_score < 0;
    const double pct = frozen ? 0.0 : qBound(0.0, m_score / 100.0, 1.0);

    // 값이 임계를 넘었는지로 색이 갈린다 — 게이지를 흘깃 봤을 때 알아야 할
    // 것은 절대 점수가 아니라 "임계를 넘었나"이기 때문이다.
    QColor value_color = Theme::accent;
    if (frozen)
        value_color = Theme::textFaint;
    else if (m_threshold > 0 && m_score >= m_threshold)
        value_color = Theme::alarm;
    else if (m_threshold > 0 && m_score >= m_threshold * 0.7)
        value_color = Theme::amber;

    // 트랙
    p.setPen(QPen(Theme::elevated, thick, Qt::SolidLine, Qt::FlatCap));
    p.drawArc(ring, ARC_START, ARC_SPAN);

    // 값
    if (pct > 0) {
        p.setPen(QPen(value_color, thick, Qt::SolidLine, Qt::FlatCap));
        p.drawArc(ring, ARC_START, int(ARC_SPAN * pct));
    }

    // 임계 눈금 — 좌표를 삼각함수로 구하지 않고 "짧은 호 한 조각"으로 그린다
    if (m_threshold > 0 && !frozen) {
        const double tp = qBound(0.0, m_threshold / 100.0, 1.0);
        const int at = ARC_START + int(ARC_SPAN * tp);
        p.setPen(QPen(Theme::textHi, thick, Qt::SolidLine, Qt::FlatCap));
        p.drawArc(ring, at - 12, 24);   // 약 1.5도
    }

    // 가운데 숫자
    p.setPen(frozen ? Theme::textFaint : Theme::textHi);
    p.setFont(Theme::mono_font(m_diameter / 4.5, QFont::DemiBold));
    const QRectF center(0, 0, m_diameter, m_diameter);
    p.drawText(center, Qt::AlignCenter,
               frozen ? QString::fromUtf8("—")
                      : QString::number(m_score, 'f', 1));

    if (!m_caption.isEmpty()) {
        p.setPen(frozen ? Theme::textFaint : Theme::textMuted);
        p.setFont(Theme::mono_font(9));
        p.drawText(QRectF(0, m_diameter - 2, m_diameter, 16),
                   Qt::AlignHCenter | Qt::AlignTop,
                   frozen ? QString("decision frozen") : m_caption);
    }
}

// ------------------------------------------------------------- WeightPie

QColor WeightPie::slice_color(int index)
{
    // 보드(Settings.dc.html)의 파이 배색 그대로 — 의미색이다: 불꽃=적,
    // 가스=호박, 온도=청, 습도=녹, 표면온도=보라 (08-19)
    switch (index) {
    case 0:  return Theme::amber;               // 가스
    case 1:  return Theme::alarm;               // 불꽃
    case 2:  return Theme::accent;              // 온도
    case 3:  return Theme::green;               // 습도
    default: return QColor(0xB4, 0x8C, 0xFF);   // 표면온도
    }
}

namespace {

/// 범례 한 줄("● Surface temp   25%")이 안 잘리는 최소 폭 — 138이었을 때
/// FONT_SCALE 1.15 를 만나 제일 긴 라벨이 "Surface tem"으로 잘렸다 (08-19)
constexpr int LEGEND_W = 176;
/// 도넛과 범례 사이
constexpr int PIE_GAP = 16;
constexpr int PIE_MIN_D = 104;
constexpr int PIE_MAX_D = 140;

} // namespace

WeightPie::WeightPie(QWidget *parent) : QWidget(parent)
{
    // 세로는 가중치 5행 + 헤더/합계가 차지하는 높이를 따라가므로 넉넉하다.
    // 가로는 도넛+간격+범례가 안 겹칠 만큼만 요구하고, 남는 폭은 아래
    // paintEvent가 가운데 정렬로 흡수한다.
    setMinimumSize(PIE_MIN_D + PIE_GAP + LEGEND_W, PIE_MIN_D + 12);
}

void WeightPie::set_slices(const QVector<Slice> &slices)
{
    m_slices = slices;
    update();
}

void WeightPie::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    double total = 0;
    for (const Slice &s : m_slices)
        total += s.weight;
    if (total <= 0)
        return;

    // 도넛 지름은 세로 여유를 따라 키우되 상한을 둔다 — 그리드 열이 넓어질
    // 때마다 무한정 커지면 옆 입력칸들과 균형이 깨진다.
    const int d = qBound(PIE_MIN_D, height() - 12, PIE_MAX_D);
    const int thick = qMax(8, d / 5);

    // 도넛+범례를 한 덩어리로 보고 그 덩어리를 가로 가운데에 놓는다.
    // 전에는 도넛을 x=0에 붙박이로 그리고 범례 %만 오른쪽 끝에 정렬해서,
    // 열이 넓어질수록 둘 사이가 벌어지고 전체가 왼쪽으로 쏠려 보였다.
    const int content_w = d + PIE_GAP + LEGEND_W;
    const double x0 = qMax(0.0, (width() - content_w) / 2.0);
    const double y0 = (height() - d) / 2.0;

    const QRectF ring(x0 + thick / 2.0, y0 + thick / 2.0, d - thick, d - thick);

    // 12시에서 시계방향 — 표(WEIGHT_KEYS)에 적힌 순서 그대로 돌아야
    // 범례와 대응이 보인다.
    int at = 90 * 16;
    for (const Slice &s : m_slices) {
        const int span = int(-360 * 16 * (s.weight / total));
        p.setPen(QPen(s.color, thick, Qt::SolidLine, Qt::FlatCap));
        p.drawArc(ring, at, span);
        at += span;
    }

    // 범례 — 도넛 오른쪽에 색 점 + 이름 + 비율
    const double lx = x0 + d + PIE_GAP;
    const int row_h = 17;
    const double top = (height() - int(m_slices.size()) * row_h) / 2.0;
    p.setFont(Theme::mono_font(10));
    int i = 0;
    for (const Slice &s : m_slices) {
        const double y = top + i * row_h;
        p.setPen(Qt::NoPen);
        p.setBrush(s.color);
        p.drawEllipse(QRectF(lx, y + row_h / 2.0 - 3.5, 7, 7));

        p.setPen(Theme::textMuted);
        p.drawText(QRectF(lx + 14, y, LEGEND_W - 14 - 44, row_h),
                   Qt::AlignVCenter | Qt::AlignLeft, s.label);

        p.setPen(Theme::textHi);
        p.drawText(QRectF(lx + LEGEND_W - 44, y, 44, row_h),
                   Qt::AlignVCenter | Qt::AlignRight,
                   QString::number(s.weight * 100.0, 'f', 0) + "%");
        ++i;
    }
}
