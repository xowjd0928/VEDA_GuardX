#include "crowd_page.h"
#include "floor_plan.h"

#include <QPainterPath>
#include "calibration_store.h"
#include "mqtt_link.h"
#include "theme.h"

#include <QButtonGroup>
#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <cmath>

// schema.sql: category 1 = Human. 실측상 2·3도 들어오므로(얼굴/기타) 토글로 둔다.
static const int CATEGORY_HUMAN = 1;

// 오버레이(DetectionFeed)와 같은 신뢰도 컷을 써야 두 화면 수치가 어긋나지 않는다
static const double MIN_LIKELIHOOD = 0.30;

// SQL에서 1차 집계할 카메라 화면 격자 (px). 이 단위로 뭉쳐 전송량을 고정한다.
static const int CAM_CELL = 60;

/**
 * @brief 데이터가 있는 날짜 목록 (retained)
 *
 * 우측 날짜 목록을 채우는 근거. 거의 안 바뀌는 상태값이고 유실되면 목록이
 * 영영 비므로 QoS 1 + retained — zones(v2)와 같은 논리다.
 */
static const QString DATES_TOPIC = "guardx/db/rpib/dates";
static const int DATES_QOS = 1;

/**
 * @brief 하루치 집계 요청 토픽
 *
 * MQTT에는 요청-응답이 없다. 요청 토픽 하나와 응답 토픽 하나를 두고 payload의
 * req_id로 짝을 맞춘다. MQTT 5의 ResponseTopic/CorrelationData 속성 대신
 * payload에 넣는 이유는 RPi B의 mosquitto 버전 의존을 없애기 위해서다
 * (1.6.x는 MQTT 5 미지원).
 *
 * QoS 1 — 요청이 유실되면 그 날짜가 영영 안 그려지고, 중복 요청은 같은 결과를
 * 두 번 받을 뿐이라 무해하다.
 */
static const QString HEATDAY_REQ_TOPIC = "guardx/db/rpib/query/heatday";

namespace {

// H(행 우선 3x3, "채널 픽셀 → 방 cm")를 점에 적용한다. 실시간 발밑점 변환
// (to_floor)에만 쓴다 — 가시영역은 이제 웹 UI가 미리 계산해 보낸 다각형을
// 그대로 읽으므로, 여기서 역행렬을 구할 필요가 없다 (아래 draw_coverage 참고).
QPointF apply_h(const double H[9], double x, double y)
{
    const double w = H[6] * x + H[7] * y + H[8];
    if (std::fabs(w) < 1e-12) return QPointF(0, 0);
    return QPointF((H[0] * x + H[1] * y + H[2]) / w,
                   (H[3] * x + H[4] * y + H[5]) / w);
}

/**
 * @brief 이 발밑점의 거리를 믿을 수 있나 — 지평선 여유 판정.
 *
 * `w = H6·x + H7·y + H8` 이 0 이 되는 선이 **지평선**이고, 결과는 w 로
 * 나뉜다. 그래서 지평선에 가까워질수록 **픽셀 한 칸이 몇 미터가 된다** —
 * 검출 박스가 조금만 흔들려도 평면도에서 방 끝까지 튄다. `apply_h` 의
 * `1e-12` 가드는 0 으로 나누기만 막을 뿐 이 증폭은 못 막는다.
 *
 * 절대값으로 자를 수 없으므로(H 의 배율은 임의다) **화면 맨 아래 중앙**
 * — 카메라에 가장 가까운, 가장 믿을 만한 점 — 의 w 와 비교한다.
 * 민감도는 1/w² 로 커지므로, 비율 r 을 요구하면 증폭이 1/r² 로 묶인다.
 *
 * 부호가 다르면 **지평선 너머**다(카메라 뒤쪽 = 물리적으로 불가능).
 */
bool depth_trustworthy(const double H[9], double x, double y,
                       double frame_w, double frame_h)
{
    // 증폭 상한 ≈ 1/0.1² = 100배. 이보다 조이면 먼 쪽 정상 점까지 잘려
    // 동선이 카메라 근처에서만 남는다 (실측 후 조정할 자리).
    static const double MIN_W_RATIO = 0.10;

    const double w     = H[6] * x + H[7] * y + H[8];
    const double w_ref = H[6] * (frame_w / 2.0) + H[7] * frame_h + H[8];
    if (std::fabs(w_ref) < 1e-12) return false;   // H 가 깨졌다

    if ((w > 0) != (w_ref > 0)) return false;     // 지평선 너머
    return std::fabs(w) >= MIN_W_RATIO * std::fabs(w_ref);
}

}  // namespace

/**
 * @brief 슬라이더 한 칸의 크기 — 전부 하루 안쪽이다
 *
 * 여러 날을 보는 것은 프리셋이 아니라 날짜 다중 선택으로 한다. payload는 항상
 * 10분 슬롯이므로 30분·1시간은 클라이언트에서 묶어 올릴 뿐 재요청이 없다.
 */
const CrowdPage::Preset CrowdPage::PRESETS[] = {
    { "10 min",  10 },
    { "30 min",  30 },
    { "1 hour",  60 },
    { "3 hours", 180 },
};
const int CrowdPage::PRESET_COUNT = int(sizeof(PRESETS) / sizeof(PRESETS[0]));

// ---------------------------------------------------------------- FloorCanvas

FloorCanvas::FloorCanvas(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(480, 300);
    setAttribute(Qt::WA_StyledBackground);
    setObjectName("Panel");
}

void FloorCanvas::set_fan_visible(int channel, bool visible)
{
    // 채널은 0-based(카메라 축) — 이 파일의 다른 API(render_heat·to_floor·
    // draw_coverage)와 같다. 2026-08-22 이전엔 이것만 1-based 였다.
    const int idx = channel;
    if (idx < 0 || idx >= 4) return;
    if (m_fan_visible[idx] == visible) return;
    m_fan_visible[idx] = visible;
    update();
}

// 호출부(render_heat)가 "이 점은 모른다"로 읽고 건너뛸 값. 정상 좌표는
// 항상 0 근방 ~ FLOOR_W/H 안쪽이라 이 값과 헷갈릴 일이 없다.
static const QPointF kUnknownFloorPt(-1e9, -1e9);

QPointF FloorCanvas::to_floor(int channel, double cam_x, double cam_y)
{
    // ★ 채널 축은 앱 전체가 0-based(0~3, 카메라와 같은 축)다 — CalibrationStore
    //   도 이제 그 축으로 담는다(load_json 이 웹 UI 의 1-based 키를 입구에서
    //   변환한다). 예전엔 여기서만 +1 을 더했는데, 바로 아래 draw_coverage 는
    //   안 더해서 **한 파일 안에서 두 축이 섞여 있었다** (2026-08-22 정리).
    auto *store = CalibrationStore::instance();
    const auto cc = store->channel(channel);
    const double rw = store->room_w_cm(), rh = store->room_h_cm();
    if (rw <= 0.0 || rh <= 0.0) return kUnknownFloorPt;

    // usable 이면 입력이 이미 H 의 좌표계다 — 그대로 넣는다.
    // 아니면 force_draw() 를 켰을 때만, H 가 있는 채널에 한해 계산한다.
    if (!cc.usable) {
        if (!store->force_draw() || !cc.geo_usable) return kUnknownFloorPt;
    }

    // 입력은 라이브 프레임(FRAME_W/H) 픽셀인데 H 는 cc.frame_w/h 좌표계에서
    // 만들어졌다. 같으면 배율이 1 이라 usable 경로에 영향이 없고, 다르면
    // (사진 기준 H) 여기서 맞춰 준다 — 안 맞추면 방 한쪽으로 쏠려 찍힌다.
    double x = cam_x, y = cam_y;
    if (cc.frame_w > 0 && cc.frame_h > 0) {
        x *= double(cc.frame_w) / double(FRAME_W);
        y *= double(cc.frame_h) / double(FRAME_H);
    }

    // 지평선에 너무 가까우면 거리를 못 믿는다 — 방 안에 떨어지더라도 자리가
    // 틀렸을 값이라 "모른다"로 돌린다 (근거는 depth_trustworthy).
    //
    // ⚠ **usable 일 때만 판정한다.** force_draw 로 보는 중이라면 H 자체가
    // 다른 좌표계(사진)에서 만들어진 것이라 **지평선이 엉뚱한 자리**에 있다.
    // 그 지평선으로 거르면 정상 점까지 전부 떨어져 동선이 한 점도 안 남는다
    // (08-12 실측: 가드를 켜자 동선이 통째로 사라졌다). 못 믿는 H 로 "못
    // 믿는 구간"을 가려내는 것 자체가 모순이므로, 검증 모드에서는 걸지 않는다
    // — 어차피 그 화면은 위치를 믿지 말라고 띄운 것이다.
    if (cc.usable) {
        const double fw = cc.frame_w > 0 ? double(cc.frame_w) : double(FRAME_W);
        const double fh = cc.frame_h > 0 ? double(cc.frame_h) : double(FRAME_H);
        if (!depth_trustworthy(cc.H, x, y, fw, fh)) return kUnknownFloorPt;
    }

    const QPointF cm = apply_h(cc.H, x, y);
    return QPointF(FLOOR_W * (cm.x() / rw), FLOOR_H * (cm.y() / rh));
}

QPointF FloorCanvas::to_floor_norm(int channel, double nx, double ny)
{
    // 정규화 좌표는 해상도에 독립이므로 라이브 프레임으로 되돌려 to_floor 에
    // 넘긴다 — 사진 기준 H 로의 환산은 to_floor 가 알아서 한다. 여기서
    // cc.frame_w/h 로 직접 되돌리면 to_floor 가 한 번 더 나눠 이중 보정이 된다.
    return to_floor(channel, nx * FRAME_W, ny * FRAME_H);
}

void FloorCanvas::set_cells(const QHash<QPair<int, int>, double> &cells, double max_weight)
{
    m_cells = cells;
    m_max_weight = max_weight > 0.0 ? max_weight : 1.0;
    update();
}

void FloorCanvas::view_transform(double &sx, double &sy, double &ox, double &oy) const
{
    auto *store = CalibrationStore::instance();
    const double rw = store->room_w_cm(), rh = store->room_h_cm();

    double vw, vh;
    if (store->loaded() && rw > 0.0 && rh > 0.0) {
        // 방 비율대로. 위젯 안에 들어가도록 긴 쪽을 기준으로 맞춘다.
        const double aspect = rw / rh;
        vw = qMin(double(width()), double(height()) * aspect);
        vh = vw / aspect;
    } else {
        // 캘리브레이션 전 — 방 크기를 모르니 예전대로 캔버스 비율로 둔다
        const double s = qMin(double(width()) / FLOOR_W, double(height()) / FLOOR_H);
        vw = FLOOR_W * s;
        vh = FLOOR_H * s;
    }
    sx = vw / FLOOR_W;
    sy = vh / FLOOR_H;
    ox = (width() - vw) / 2.0;
    oy = (height() - vh) / 2.0;
}

QPointF FloorCanvas::to_widget(const QPointF &p) const
{
    double sx, sy, ox, oy;
    view_transform(sx, sy, ox, oy);
    return QPointF(ox + p.x() * sx, oy + p.y() * sy);
}

// 채널 색 — 웹 UI 캘리브레이션 탭(index.html CH_COL)과 맞춘다. 두 화면을
// 같이 보는 사람이 "이 색 = 이 채널"을 헷갈리지 않게.
static const char *kChColor[4] = { "#5b8dd9", "#3fd07a", "#d9a13f", "#d95f5f" };

/** @brief 방 cm 좌표 -> 위젯 좌표 */
QPointF FloorCanvas::cm_to_widget(const QPointF &cm, double rw, double rh) const
{
    return to_widget(QPointF(FLOOR_W * (cm.x() / rw), FLOOR_H * (cm.y() / rh)));
}

/**
 * @brief 채널의 실제 가시영역 — 웹 UI가 보낸 다각형을 그리기만 한다.
 *
 * ★ 여기서 호모그래피 역행렬이나 지평선 부호판정을 하지 않는다. 그 계산은
 *   웹 UI(calCoveragePoly())가 이미 검증된 자리에서 한 번 해서 꼭짓점으로
 *   실어 보냈다. 계산이 한 곳에만 있어야 두 화면이 서로 다른 답을 낼 일이 없다.
 *
 * ★ 다각형이라 경계가 정확하다 — 예전엔 20cm 격자로 받아서 계단처럼 보였고,
 *   그걸 뭉개서 감추려 했지만 애초에 경계가 직선인데 격자로 근사한 게
 *   문제였다 (가시영역 = 영상 사각형이 바닥에 투영된 모양).
 */
/**
 * @brief 가구 한 칸에 입힐 **현장 프롭 아트워크** (2026-08-24)
 *
 * 평면도의 가구는 원래 이름만 적힌 회색 상자였다. 현장 프롭의 인쇄 원본
 * (TOP_FRONT.pdf)에 각 면의 실제 그림이 있어서 그걸 입힌다 — 운영자가
 * 도면과 현장을 눈으로 맞출 때 "이 상자가 저 진열장"이 즉시 읽힌다.
 *
 * 어느 면을 쓰나: **모양이 정한다** (사용자 규칙 08-24)
 *  - 세로로 긴 가구 → FRONT (진열장 정면 — 선반과 진열품이 보인다)
 *  - 가로로 긴 가구 → TOP   (카운터 상판 — 위에서 내려다본 그림)
 *
 * 어느 품목을 쓰나: 이름 해시로 **결정적으로** 고른다. 무작위면 다시 그릴
 * 때마다 그림이 바뀌어 평면도가 흔들려 보인다. 이름↔품목을 손으로 맞추고
 * 싶어지면 여기 표만 고치면 된다.
 */
static QPixmap prop_art(const QString &name, bool tall)
{
    static const char *FRONT[] = { ":/img/props/front_beauty.png",
                                   ":/img/props/front_fragrance.png",
                                   ":/img/props/front_accessory.png" };
    static const char *TOP[]   = { ":/img/props/top_info.png",
                                   ":/img/props/top_jewelry.png",
                                   ":/img/props/top_giftwrap.png" };
    const char *const *set = tall ? FRONT : TOP;
    const QString path =
        QString::fromLatin1(set[int(qHash(name, 0x9E37) % 3u)]);

    // 리소스 디코딩은 한 번만 — 평면도는 250ms 마다 다시 그린다.
    static QHash<QString, QPixmap> cache;
    auto it = cache.find(path);
    if (it == cache.end())
        it = cache.insert(path, QPixmap(path));
    return it.value();
}

void FloorCanvas::draw_coverage(QPainter &p, int channel) const
{
    auto *store = CalibrationStore::instance();
    const auto cc = store->channel(channel);
    const double rw = store->room_w_cm(), rh = store->room_h_cm();
    // 가시영역은 정적 기하라 usable(라이브 프레임 해상도 일치)이 아니라
    // geo_usable 로 거른다 — calibration_store.h 참고.
    if (!cc.geo_usable || rw <= 0.0 || rh <= 0.0 || cc.coverage_poly.size() < 3) return;

    QPolygonF poly;
    poly.reserve(cc.coverage_poly.size());
    for (const QPointF &cm : cc.coverage_poly) poly << cm_to_widget(cm, rw, rh);

    // 웹 UI 지도(calDrawMap)와 같은 톤 — 옅은 채움 + 같은 색 실선 테두리.
    const QColor col(kChColor[channel]);   // 채널 0-based, 색표는 그 순서
    QColor fill = col;
    fill.setAlpha(64);
    p.setBrush(fill);
    p.setPen(QPen(col, 1.4));
    p.drawPolygon(poly);
    p.setBrush(Qt::NoBrush);

    p.setPen(Theme::textHi);
    p.setFont(Theme::mono_font(9.5, 700));
    // 사람에게 보이는 이름만 1-based 다 (CH1..CH4)
    p.drawText(poly.boundingRect(), Qt::AlignCenter,
               QString("CH%1").arg(channel + 1));
}

void FloorCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), Theme::bg0);

    // ⚠ 좌표를 못 놓는 캘리브레이션이면 **이유를 지도 위에 적는다.** 방과 가구는
    //   그려지는데 히트 셀만 한 칸도 안 나오면(to_floor 가 전부 "모름"을 돌린다)
    //   화면만 봐서는 감지가 죽은 것과 구분이 안 된다 — 08-13 실사고: 관리자가
    //   올린 전역 캘리브레이션이 세로 휴대폰 사진으로 만들어져 있었다.
    {
        const QString note =
            CalibrationStore::instance()->frame_mismatch_note();
        if (!note.isEmpty() && !m_compact) {
            p.save();
            p.setPen(Theme::amber);
            p.setFont(Theme::mono_font(9.5));
            // 안내는 **지금 안 그리고 있을 때만** 붙인다 — 이미 그리는 중인데
            // "이렇게 하면 보인다"고 적으면 화면이 자기 상태를 잘못 말한다.
            QString line = note;
            if (!CalibrationStore::instance()->force_draw())
                line += QString::fromUtf8("  ·  Settings > Floor calibration > "
                        "\"draw even if the coordinate frame mismatches\" "
                        "shows them anyway");
            p.drawText(rect().adjusted(12, 8, -12, -8),
                       Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, line);
            p.restore();
        }
    }

    double sx, sy, ox, oy;
    view_transform(sx, sy, ox, oy);

    // ---- 평면도 배경 (2026-08-24) ----
    // 파일이 있으면 방 사각형에 딱 맞춰 깐다. 히트 셀·가시영역·동선은 그
    // **위에** 그려지므로 표시 방식은 예전 그대로다.
    // ⚠ 도면이 있으면 1m 격자는 그리지 않는다 — 도면에 이미 벽·치수가
    //   있어서 격자를 겹치면 선이 두 겹이 되어 둘 다 못 읽는다.
    bool has_plan = false;
    {
        auto *store = CalibrationStore::instance();
        const double rw = store->room_w_cm(), rh = store->room_h_cm();
        const QPixmap plan = FloorPlan::image();
        if (!plan.isNull() && rw > 0.0 && rh > 0.0) {
            const QRectF room(cm_to_widget(QPointF(0, 0), rw, rh),
                              cm_to_widget(QPointF(rw, rh), rw, rh));
            p.drawPixmap(room, plan, QRectF(plan.rect()));
            has_plan = true;
        }
    }

    // ⚠ 1m 격자는 뺐다 (2026-08-25 지시). 도면은 "어디에 무엇이 있나"를
    //   보여주는 그림이지 모눈종이가 아니다 — 격자가 깔리면 히트 셀·동선과
    //   선이 뒤섞여 배치가 안 읽힌다. 거리 감각은 좌하단 축척 막대가 준다.

    // ---- 채널 가시영역 — geo_usable 한 채널만, 웹 UI가 보낸 다각형을 그린다.
    //      (상단 토글로 채널별 on/off. 히트 셀보다 먼저 그려야 셀이 위에 보인다) ----
    for (int ch = 0; ch < 4; ++ch) {
        if (!m_fan_visible[ch]) continue;
        draw_coverage(p, ch);
    }

    // ---- 히트 셀 ----
    // 방 비율을 살리면 x/y 배율이 달라서 칸도 정사각형이 아니다 — 셀 하나가
    // 덮는 실제 넓이는 그대로이므로 배율만 각각 곱해 준다.
    const double cell_w = CELL * sx, cell_h = CELL * sy;
    for (auto it = m_cells.constBegin(); it != m_cells.constEnd(); ++it) {
        double t = it.value() / m_max_weight;
        t = qBound(0.0, t, 1.0);

        // 보드(Crowd.dc.html)의 히트 램프: green -> amber -> alarm 3단.
        // 전엔 accent(파랑) -> alarm 2단이라 "여유"가 파랑이었는데, 이 앱의
        // 다른 모든 화면에서 파랑은 정보색이고 **여유는 초록**이다(경보
        // 3색 규약). 중간값이 호박색을 지나야 "슬슬 붐빈다"가 읽힌다.
        const QColor lo  = Theme::green;
        const QColor mid = Theme::amber;
        const QColor hi  = Theme::alarm;
        const QColor &a = t < 0.5 ? lo  : mid;
        const QColor &b = t < 0.5 ? mid : hi;
        const double u = t < 0.5 ? t * 2.0 : (t - 0.5) * 2.0;
        QColor c(int(a.red()   + (b.red()   - a.red())   * u),
                 int(a.green() + (b.green() - a.green()) * u),
                 int(a.blue()  + (b.blue()  - a.blue())  * u));
        c.setAlphaF(0.16 + 0.64 * t);

        const QPointF tl = to_widget(QPointF(it.key().first * CELL, it.key().second * CELL));
        p.fillRect(QRectF(tl.x(), tl.y(), cell_w, cell_h), c);
    }

    // ---- 캘리브레이션 웹 UI가 찍은 장애물 (SETTINGS 에서 불러온 것) ----
    //
    // ★ 하드코딩 스케치(ITEMS[])는 없앴다 — 이제 이게 유일한 장애물 소스다.
    //
    // 색은 채널과 무관하게 **고정**이다. 가구는 방에 붙박인 것이지 특정
    // 카메라의 소유물이 아니라서, 채널 색으로 칠하면 "이 가구는 CH1 것"처럼
    // 읽힌다. 중립색(textMuted)을 쓰는 이유는 하나 더 있다 — CH3 색(#d9a13f)이
    // 호박색이라, 예전처럼 amber 로 칠하면 CH3 화각과 구분이 안 된다.
    //
    // geo_usable=false 채널은 기본적으로 건너뛴다 — force_draw() 를 켜면
    // 그것도 그리되 **점선**으로 "이건 못 믿는 좌표"라는 표시를 남긴다.
    p.setFont(Theme::mono_font(9, 600));
    {
        auto *store = CalibrationStore::instance();
        const double rw = store->room_w_cm(), rh = store->room_h_cm();
        if (store->loaded() && rw > 0.0 && rh > 0.0) {
            // 도면을 깔았으면 가구는 기본적으로 그리지 않는다 — 도면에 이미
            // 있어서 두 겹이 되면 서로 싸운다. 캘리브레이션 좌표를 눈으로
            // 검증할 때만 QSettings `floorplan_show_obstacles=1` 로 켠다.
            // 사진이 붙은 칸은 도면 위에도 **항상** 그린다 — 사용자가 그
            // 칸에 사진을 지정했다는 건 보고 싶다는 뜻이다. 사진이 없는 칸만
            // 예전 규칙을 따른다(도면이 있으면 생략, 없으면 옛 상자).
            const bool draw_plain = !has_plan || FloorPlan::show_obstacles();
            FloorPlan::refresh_fixture_list();
            int fx_index = 0;
            for (const CalibrationStore::Obstacle &ob : store->obstacles()) {
                const QPixmap user_art =
                    FloorPlan::prop_image(++fx_index, ob.channel, ob.name);
                if (user_art.isNull() && !draw_plain) continue;
                // 장애물 좌표는 정적 기하(사진 좌표계에서 이미 cm 로 계산됨) —
                // 라이브 프레임 해상도와 무관하므로 usable 이 아니라
                // geo_usable 로 거른다 (calibration_store.h 참고).
                const bool geo_usable = store->channel(ob.channel).geo_usable;
                if (!geo_usable && !store->force_draw()) continue;

                // ---- 가구 한 칸 — 도형만 (2026-08-25) ----
                // 그림도 글자도 넣지 않는다. 이 지도가 답하는 질문은 "어느
                // 자리에 무엇이 있나" 하나뿐이고, 그건 **모양과 위치**로 이미
                // 답이 된다. 이름을 칸마다 적으면 좁은 칸에서 글자가 칸을
                // 넘치고, 사진을 넣으면 그 위의 히트 셀이 안 읽힌다.
                QPolygonF poly;
                for (const QPointF &cm : ob.footprint_cm) poly << cm_to_widget(cm, rw, rh);

                // 사진은 사용자가 그 칸에 지정했을 때만 (props/<번호>.png).
                // 기본은 아무것도 없어서 아래 도형만 그려진다.
                if (!user_art.isNull()) {
                    p.save();
                    QPainterPath clip;
                    clip.addPolygon(poly);
                    p.setClipPath(clip, Qt::IntersectClip);
                    p.setOpacity(geo_usable ? 0.90 : 0.45);
                    p.drawPixmap(poly.boundingRect(), user_art,
                                 QRectF(user_art.rect()));
                    p.restore();
                    p.setBrush(Qt::NoBrush);
                } else {
                    // 기호로 그린다 — 빈 네모만 늘어놓으면 "무엇이 있는지"가
                    // 안 읽힌다(08-25 지적). 기호 표는 FloorPlan::draw_fixture.
                    QColor ink = Theme::textMuted;
                    ink.setAlpha(geo_usable ? 225 : 110);
                    FloorPlan::draw_fixture(p, poly.boundingRect(), ob.name,
                                            ink, !geo_usable);
                }
            }
            p.setBrush(Qt::NoBrush);
        }
    }

    // ---- 방 외곽 ----
    p.setPen(QPen(Theme::border2, 2));
    p.setBrush(Qt::NoBrush);
    const QPointF tl = to_widget(QPointF(0, 0));
    p.drawRect(QRectF(tl.x(), tl.y(), FLOOR_W * sx, FLOOR_H * sy));

    // ★ "카메라 표식(CCTV 점)"은 뺐다 — 예전 위치(CAM_X_RATIO/Y_RATIO)는
    //   스케치 추측값이었다. H 만으로는 카메라 위치를 못 구한다(내부
    //   파라미터가 있어야 분해된다 — plan_view.py 의 같은 주석 참고).
    //   calibration.json 에 cam_x_cm/cam_y_cm 이 추가되면 그때 다시 넣는다.
    //   채널 라벨은 draw_coverage() 가 실제 가시영역 위에 그린다.

    // ---- 히트 범례 (보드: 지도 우상단 고정 칩 "low ▮▮▮ high") ----
    // 색이 무엇을 뜻하는지 화면 안에서 말해 준다 — 초록/호박/빨강 3단이
    // 위 히트 셀의 램프와 같은 순서다.
    if (!m_compact) {
        const int chip_h = 22, sw = 13, gap = 3;
        p.setFont(Theme::mono_font(9));
        const QFontMetricsF fm(p.font());
        const double low_w  = fm.horizontalAdvance("low");
        const double high_w = fm.horizontalAdvance("high");
        const double chip_w = 10 + low_w + 6 + sw * 3 + gap * 2 + 6 + high_w + 10;
        const QRectF chip(width() - chip_w - 14, 12, chip_w, chip_h);

        p.setPen(QPen(Theme::border2, 1));
        p.setBrush(QColor(0, 0, 0, 153));   // 보드 rgba(0,0,0,0.6)
        p.drawRect(chip);

        p.setPen(Theme::textMuted);
        p.setBrush(Qt::NoBrush);
        double x = chip.left() + 10;
        p.drawText(QRectF(x, chip.top(), low_w, chip_h),
                   Qt::AlignVCenter | Qt::AlignLeft, "low");
        x += low_w + 6;

        const QColor ramp[3] = { Theme::green, Theme::amber, Theme::alarm };
        for (const QColor &c : ramp) {
            p.fillRect(QRectF(x, chip.center().y() - 4, sw, 8), c);
            x += sw + gap;
        }
        x += 6 - gap;
        p.setPen(Theme::textMuted);
        p.drawText(QRectF(x, chip.top(), high_w, chip_h),
                   Qt::AlignVCenter | Qt::AlignLeft, "high");
    }
}

// ------------------------------------------------------------------ CrowdPage

CrowdPage::CrowdPage(QWidget *parent) : QWidget(parent)
{
    build_ui();

    // 요청이 "날짜당 1회"라 v4의 순서 역전 문제가 거의 사라졌다. 남은 것은
    // 여러 날짜를 동시에 요청했을 때인데, MqttLink::request()가 요청마다
    // 콜백을 들고 있으므로 각 응답이 자기 날짜 캐시로 들어간다 (순서 무관).
    // 응답 토픽·req_id·타임아웃 부기도 전부 거기로 옮겼다.
    MqttLink::instance()->subscribe(
        DATES_TOPIC,
        [this](const QByteArray &payload) { on_dates(payload); },
        DATES_QOS);

    rebuild_slider();
    set_status("waiting for the date list...");
}

void CrowdPage::on_dates(const QByteArray &payload)
{
    QJsonParseError err{};
    const QJsonObject o = QJsonDocument::fromJson(payload, &err).object();
    if (err.error != QJsonParseError::NoError) {
        qDebug() << "[CrowdPage] 날짜 목록 파싱 실패:" << err.errorString();
        return;
    }

    const QJsonArray arr = o.value("dates").toArray();
    if (arr.isEmpty()) {
        set_status("no dates with data", true);
        return;
    }

    // 최근 날짜가 위로 오게 — 보통 최근을 먼저 본다
    QVector<QDate> dates;
    for (const QJsonValue &v : arr) {
        const QDate d = QDate::fromString(v.toString(), Qt::ISODate);
        if (d.isValid())
            dates.append(d);
    }
    std::sort(dates.begin(), dates.end(), [](const QDate &a, const QDate &b) {
        return a > b;
    });

    const QSet<QDate> keep = m_selected;   // 목록을 다시 채워도 선택은 유지
    {
        QSignalBlocker block(m_date_list);
        m_date_list->clear();
        for (const QDate &d : dates) {
            auto *item = new QListWidgetItem(d.toString("yyyy-MM-dd  (ddd)"), m_date_list);
            item->setData(Qt::UserRole, d);
            if (keep.contains(d))
                item->setSelected(true);
        }
        // 처음 받았고 아직 고른 게 없으면 가장 최근 날짜를 자동 선택
        if (keep.isEmpty() && m_date_list->count() > 0)
            m_date_list->item(0)->setSelected(true);
    }

    on_selection_changed();
}

void CrowdPage::on_selection_changed()
{
    m_selected.clear();
    const auto items = m_date_list->selectedItems();
    for (const QListWidgetItem *it : items)
        m_selected.insert(it->data(Qt::UserRole).toDate());

    ensure_days_loaded();
    render_heat();
}

void CrowdPage::ensure_days_loaded()
{
    QVector<QDate> need;
    for (const QDate &d : m_selected) {
        if (m_day_cache.contains(d))
            continue;                       // 캐시 적중 — 네트워크 없음
        if (m_inflight.contains(d))
            continue;                       // 이미 요청 중
        need.append(d);
    }
    if (need.isEmpty())
        return;

    for (const QDate &d : need) {
        QJsonObject params;
        params["query"]          = "heatday";
        params["date"]           = d.toString(Qt::ISODate);
        params["cell"]           = CAM_CELL;
        params["slot_min"]       = SLOT_MIN;
        params["category"]       = CATEGORY_HUMAN;
        params["min_likelihood"] = MIN_LIKELIHOOD;

        const QString id = MqttLink::instance()->request(
            HEATDAY_REQ_TOPIC, params,
            [this, d](const QJsonObject &reply) { on_day_result(reply, d); },
            [this, d](const QString &reason) {
                m_inflight.remove(d);
                qDebug() << "[CrowdPage] 집계 실패" << d << reason;
                set_status(QString("%1 aggregation failed - %2")
                               .arg(d.toString("MM-dd"), reason), true);
            });

        if (id.isEmpty())
            return;   // 미연결 — on_error가 이미 사유를 표시했다
        m_inflight.insert(d, id);
    }

    set_status(QString("requesting aggregation... (%1 days)")
                   .arg(m_inflight.size()));
}

void CrowdPage::on_day_result(const QJsonObject &o, const QDate &date)
{
    m_inflight.remove(date);

    // cells: [[slot, channel, gx, gy, count], ...] — 하루치라 수만 행까지 온다.
    // 키 있는 객체 대신 배열이라 전송량이 1/3 수준이다.
    const QJsonArray arr = o.value("cells").toArray();
    QVector<DayCell> day;
    day.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonArray row = v.toArray();
        if (row.size() < 5)
            continue;
        DayCell c;
        c.slot    = qint16(row.at(0).toInt());
        c.channel = qint16(row.at(1).toInt());
        c.gx      = qint16(row.at(2).toInt());
        c.gy      = qint16(row.at(3).toInt());
        c.count   = row.at(4).toInt();
        day.append(c);
    }

    m_day_cache.insert(date, day);
    qInfo() << "[CrowdPage]" << date.toString(Qt::ISODate) << "집계 수신 —"
            << day.size() << "셀";

    render_heat();
}

void CrowdPage::render_heat()
{
    if (m_selected.isEmpty()) {
        m_canvas->set_cells({}, 1.0);
        update_slot_stats({}, 0, {}, 0.0, "-");
        set_status("select a date");
        return;
    }

    const int bucket = m_slider->value();          // 표시 단위(m_step_min) 기준
    const int per_bucket = m_step_min / SLOT_MIN;  // 한 칸에 들어가는 10분 슬롯 수

    QHash<QPair<int, int>, double> cells;
    double max_w = 0.0;
    qint64 total = 0;
    QSet<int> seen_channels;
    // 슬롯 통계용 — 어차피 도는 루프라 여기서 같이 모은다(추가 순회 없음)
    QHash<int, qint64> per_channel;
    QPair<int, int> busiest{ 0, 0 };
    QSet<int> uncalibrated;   ///< 데이터는 왔지만 usable 한 H 가 없어 못 그린 채널
    int loaded_days = 0;

    for (const QDate &d : m_selected) {
        const auto it = m_day_cache.constFind(d);
        if (it == m_day_cache.constEnd())
            continue;                              // 아직 도착 안 한 날짜
        ++loaded_days;

        for (const DayCell &c : *it) {
            const int b = c.slot / per_bucket;     // 10분 슬롯 -> 표시 칸
            // 누적이면 자정부터 현재 칸까지, 아니면 그 칸 하나만.
            // 다중 선택 + 누적 + 슬라이더 끝 = "고른 날들의 하루 전체".
            const bool in_view = m_cumulative ? (b <= bucket) : (b == bucket);
            if (!in_view)
                continue;

            // 셀 중심을 대표점으로 평면도에 투영
            const double cam_x = (double(c.gx) + 0.5) * CAM_CELL;
            const double cam_y = (double(c.gy) + 0.5) * CAM_CELL;
            total += c.count;
            seen_channels.insert(c.channel);
            per_channel[c.channel] += c.count;

            // ★ 이 채널에 usable 한 H 가 없으면 to_floor() 가 "모른다"는
            //   좌표(kUnknownFloorPt)를 준다. qBound 로 clamp 하면 화면 구석에
            //   거짓으로 쌓이니, 반드시 여기서 건너뛴다.
            const QPointF f = FloorCanvas::to_floor(c.channel - 1, cam_x, cam_y);
            if (f.x() < 0 || f.x() > FloorCanvas::FLOOR_W ||
                f.y() < 0 || f.y() > FloorCanvas::FLOOR_H) {
                uncalibrated.insert(c.channel);
                continue;
            }
            const int cx = qBound(0, int(f.x()) / FloorCanvas::CELL,
                                  FloorCanvas::FLOOR_W / FloorCanvas::CELL - 1);
            const int cy = qBound(0, int(f.y()) / FloorCanvas::CELL,
                                  FloorCanvas::FLOOR_H / FloorCanvas::CELL - 1);

            const double acc = cells.value(qMakePair(cx, cy), 0.0) + c.count;
            cells[qMakePair(cx, cy)] = acc;
            if (acc > max_w)
                busiest = qMakePair(cx, cy);   // 최댓값 셀을 같이 기억한다
            max_w = qMax(max_w, acc);
        }
    }

    m_canvas->set_cells(cells, max_w);

    // 시각 표기 — 슬라이더는 하루 안의 시각이다
    const int from_min = bucket * m_step_min;
    const int to_min = qMin(from_min + m_step_min, 24 * 60);
    const QString span =
        m_cumulative
            ? QString::fromUtf8("00:00 → %1").arg(QTime(0, 0).addSecs(to_min * 60).toString("HH:mm"))
            : QString("%1 → %2")
                  .arg(QTime(0, 0).addSecs(from_min * 60).toString("HH:mm"),
                       QTime(0, 0).addSecs(to_min * 60).toString("HH:mm"));

    // 08-19: "interval 00:00 → 00:10 (1/144 slots)" 는 슬롯 개념을 이미 아는
    // 사람만 읽을 수 있었다. 지금 화면이 무엇인지를 문장으로 말한다.
    m_range_label->setText(
        m_cumulative
            ? QString::fromUtf8("Showing midnight → %1").arg(
                  QTime(0, 0).addSecs(to_min * 60).toString("HH:mm"))
            : QString::fromUtf8("Showing %1").arg(span));

    update_slot_stats(per_channel, total, busiest, max_w, span);

    if (loaded_days == 0) {
        set_status("waiting for aggregation...");
        return;
    }

    // 데이터가 없는 채널과, 데이터는 있지만 캘리브레이션이 없어 못 그린
    // 채널을 구분해서 드러낸다 — 지도의 빈 구역이 매핑 탓인지 수집 탓인지
    // 화면에서 바로 구분되게 하려는 것
    QStringList missing, uncal;
    for (int ch = 0; ch < 4; ++ch) {
        if (!seen_channels.contains(ch)) missing << QString("CH%1").arg(ch + 1);
        else if (uncalibrated.contains(ch)) uncal << QString("CH%1").arg(ch + 1);
    }

    QString msg = QString("%1 days · samples %2 · cells %3")
                      .arg(loaded_days).arg(total).arg(cells.size());
    if (!missing.isEmpty())
        msg += QString("  ·  no data: %1").arg(missing.join(", "));
    if (!uncal.isEmpty())
        msg += QString("  ·  no calibration: %1").arg(uncal.join(", "));
    set_status(msg, !missing.isEmpty() || !uncal.isEmpty());
}

void CrowdPage::rebuild_slider()
{
    const int count = 24 * 60 / m_step_min;
    QSignalBlocker block(m_slider);
    m_slider->setRange(0, count - 1);
    if (m_slider->value() > count - 1)
        m_slider->setValue(count - 1);
}

void CrowdPage::build_ui()
{
    // 08-19 워크스페이스 (디자인 "Crowd Analytics" 보드):
    // [좌 View 패널 | 제목 행 + 평면도 + 스크럽 바 | 우 날짜 열]
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 좌: 가시영역 토글 패널 ----
    auto *left = new QWidget(this);
    // 보드(Crowd.dc.html)의 좌측 열은 264px. 전엔 190을 **생짜 px**로 줬는데,
    // 안에 들어가는 글자는 전부 FONT_SCALE(1.15)로 커지므로 글자만 15% 큰
    // 칸이 되어 라벨이 잘렸다 (08-19 사용자 신고). 글자를 담는 칸은 글자와
    // 같은 배율을 타야 한다 — 230 × 1.15 ≒ 264 = 보드 값.
    left->setFixedWidth(Theme::px(230));
    left->setAttribute(Qt::WA_StyledBackground);
    Theme::restyle(left, [] {
        return QString("background:%1; border-right:1px solid %2;")
            .arg(Theme::chromeElevated.name(), Theme::border.name());
    });
    auto *left_col = new QVBoxLayout(left);
    left_col->setContentsMargins(14, 12, 14, 12);
    left_col->setSpacing(8);

    // 보드 문구·서식 그대로: 11/600, 자간 1px(=0.09em), textMuted
    auto *fan_label = new QLabel(QString::fromUtf8("CAMERA VIEW AREAS"), left);
    fan_label->setFont(Theme::ui_font(11, 600, 0.09));
    Theme::restyle(fan_label, [] {
        return QString("color:%1;").arg(Theme::textMuted.name());
    });
    left_col->addWidget(fan_label);

    // 채널 가시영역 토글 — draw_coverage() 가 그리는 실제 영역을 켜고 끈다.
    // 4개를 늘 같이 보여주면 화면이 복잡하다. 기본은 전부 켬.
    for (int ch = 0; ch < 4; ++ch) {
        auto *btn = new QPushButton(QString("CH%1").arg(ch + 1), left);
        btn->setObjectName("SegBtn");
        btn->setFixedHeight(24);
        btn->setCheckable(true);
        btn->setChecked(true);   // FloorCanvas 기본값(m_fan_visible 전부 true)과 맞춘다
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::toggled, this, [this, ch](bool on) {
            m_canvas->set_fan_visible(ch, on);
        });
        left_col->addWidget(btn);
    }

    // 슬롯 통계는 CH 토글 바로 밑 — 이 아래가 통째로 비어 있었다
    left_col->addSpacing(10);
    left_col->addWidget(build_slot_stats(left));

    left_col->addStretch(1);
    // 08-20: "fans = calibrated / camera view areas" 각주 삭제. 각주가 있어야
    // 뜻이 통하는 라벨이면 라벨이 틀린 것이다 — 패널 제목을 뜻 그대로 고쳤다
    // ("VIEW · COVERAGE FANS" → "CAMERA VIEW AREAS"). 9px textFaint 로 두 줄
    // 적어 두는 것보다 제목 한 번 고치는 쪽이 읽는 사람에게 싸다.
    root->addWidget(left);

    // ---- 중앙: 제목 행 + 평면도 + 스크럽 바 ----
    auto *center = new QWidget(this);
    auto *mid = new QVBoxLayout(center);
    mid->setContentsMargins(14, 10, 14, 10);
    mid->setSpacing(8);

    auto *head = new QHBoxLayout();
    head->setSpacing(12);
    auto *title = new QLabel("Crowd Analytics", this);
    title->setFont(Theme::ui_font(13, 700, 0.08));
    Theme::restyle(title, [=] {
        return QString("color:%1;").arg((Theme::textHi).name());
    });
    head->addWidget(title);

    auto *sub = new QLabel("floor-plan heatmap · detections", this);
    sub->setFont(Theme::mono_font(10));
    Theme::restyle(sub, [=] {
        return QString("color:%1;").arg((Theme::textFaint).name());
    });
    head->addWidget(sub);
    head->addStretch(1);

    m_status = new QLabel(this);
    m_status->setFont(Theme::mono_font(10));
    head->addWidget(m_status);
    mid->addLayout(head);

    m_canvas = new FloorCanvas(this);
    mid->addWidget(m_canvas, 1);

    // 캘리브레이션 웹 UI에서 새로 불러오면 평면도가 즉시 다시 그려지게.
    // FloorCanvas 는 CalibrationStore 를 직접 읽으므로(파싱 상태를 따로
    // 들고 있지 않는다) repaint 트리거만 여기서 이어준다.
    connect(CalibrationStore::instance(), &CalibrationStore::changed,
            m_canvas, [this] { m_canvas->update(); });

    // ---- 스크럽 바 (보드의 타임라인 바 자리) — 컨트롤 + 시각 슬라이더 ----
    auto *bar = new QWidget(center);
    bar->setObjectName("Panel");
    bar->setAttribute(Qt::WA_StyledBackground);
    auto *bar_col = new QVBoxLayout(bar);
    bar_col->setContentsMargins(12, 8, 12, 8);
    bar_col->setSpacing(6);

    auto *ctl = new QHBoxLayout();
    ctl->setSpacing(6);

    // 08-19: "Slot" 은 이 화면을 만든 사람의 말이다. 처음 보는 사람에게
    // 필요한 것은 "이 줄이 시간을 고르는 곳"이라는 사실이다.
    auto *period = new QLabel("Show", this);
    period->setFont(Theme::ui_font(10.5, 700, 0.12));
    Theme::restyle(period, [=] {
        return QString("color:%1;").arg((Theme::textMuted).name());
    });
    period->setToolTip("How long a time block each step of the slider covers");
    ctl->addWidget(period);

    // 프리셋은 "조회 창"이 아니라 슬라이더 한 칸의 크기를 정한다
    auto *group = new QButtonGroup(this);
    group->setExclusive(true);
    for (int i = 0; i < PRESET_COUNT; ++i) {
        auto *btn = new QPushButton(PRESETS[i].label, this);
        btn->setObjectName("SegBtn");
        btn->setFixedHeight(24);
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setChecked(PRESETS[i].minutes == m_step_min);
        group->addButton(btn, i);
        ctl->addWidget(btn);
    }
    // 칸 크기만 바뀐다. payload는 10분 슬롯 그대로라 재요청이 없다.
    connect(group, &QButtonGroup::idClicked, this, [this](int id) {
        m_step_min = PRESETS[id].minutes;
        rebuild_slider();
        render_heat();
    });

    ctl->addSpacing(16);

    // 이 토글이 "지금 뭘 보고 있는지"를 정한다 — 안 드러나면 구간/누적을
    // 구분할 방법이 없어 화면을 오해하게 된다.
    // 다중 선택 + 누적 + 슬라이더 끝 = "고른 날들의 하루 전체"가 되므로
    // 별도의 '전체 보기' 모드를 두지 않았다.
    m_cumulative_btn = new QPushButton("Add up from midnight", this);
    m_cumulative_btn->setObjectName("SegBtn");
    m_cumulative_btn->setFixedHeight(24);
    m_cumulative_btn->setCheckable(true);
    m_cumulative_btn->setCursor(Qt::PointingHandCursor);
    m_cumulative_btn->setToolTip(
        "Off: show only the time block the slider is on.\n"
        "On: add up everything from midnight to that block.\n"
        "Turn it on and drag the slider to the end to see the whole day.");
    connect(m_cumulative_btn, &QPushButton::toggled, this, [this](bool on) {
        m_cumulative = on;
        render_heat();
    });
    ctl->addWidget(m_cumulative_btn);

    ctl->addStretch(1);

    // 캐시를 비우고 선택된 날짜를 다시 받아온다 (그날 데이터가 늘었을 때)
    auto *refresh = new QPushButton("Reload", this);
    refresh->setObjectName("OutlineBtn");
    refresh->setFixedHeight(24);
    refresh->setCursor(Qt::PointingHandCursor);
    connect(refresh, &QPushButton::clicked, this, [this] {
        for (const QDate &d : m_selected)
            m_day_cache.remove(d);
        ensure_days_loaded();
    });
    ctl->addWidget(refresh);
    bar_col->addLayout(ctl);

    // ---- 시각 슬라이더 (하루 안, 00:00 ~ 23:5x) ----
    // 08-19: 그냥 놓아두면 "무엇을 정하는 손잡이인지" 알 수 없다는 지적.
    // 세 가지를 더한다: ① 무엇을 하는 줄인지 말하는 라벨 ② 하루를 읽을 수
    // 있는 눈금(00:00·06:00·12:00·18:00·24:00) ③ 지금 보고 있는 시각을
    // 크게 적은 값. 손잡이 위치가 곧 하루 중 어디인지가 되게 만든다.
    auto *tl = new QHBoxLayout();
    tl->setSpacing(10);

    auto *when = new QLabel("Time of day", this);
    when->setFont(Theme::ui_font(10.5, 700, 0.12));
    Theme::restyle(when, [=] {
        return QString("color:%1;").arg((Theme::textMuted).name());
    });
    tl->addWidget(when);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, SLOTS_PER_DAY - 1);
    m_slider->setPageStep(1);
    m_slider->setCursor(Qt::PointingHandCursor);
    m_slider->setToolTip(
        "Drag to move through the day - the map shows that moment.\n"
        "Arrow keys step one block at a time.");
    connect(m_slider, &QSlider::valueChanged, this, [this] { render_heat(); });

    // 슬라이더 + 그 아래 시각 눈금을 한 덩어리로
    auto *slider_host = new QWidget(this);
    auto *slider_col = new QVBoxLayout(slider_host);
    slider_col->setContentsMargins(0, 0, 0, 0);
    slider_col->setSpacing(0);
    slider_col->addWidget(m_slider);

    auto *ticks = new QWidget(slider_host);
    auto *tick_row = new QHBoxLayout(ticks);
    // 손잡이 폭의 절반만큼 좌우를 물려야 눈금이 실제 위치와 맞는다
    tick_row->setContentsMargins(6, 0, 6, 0);
    tick_row->setSpacing(0);
    static const char *TICKS[] = { "00:00", "06:00", "12:00", "18:00", "24:00" };
    for (int i = 0; i < 5; ++i) {
        auto *t = new QLabel(QString::fromUtf8(TICKS[i]), ticks);
        t->setFont(Theme::mono_font(9));
        Theme::restyle(t, [] {
            return QString("color:%1;").arg(Theme::textFaint.name());
        });
        t->setAlignment(i == 0   ? Qt::AlignLeft
                        : i == 4 ? Qt::AlignRight
                                 : Qt::AlignHCenter);
        tick_row->addWidget(t, 1);
    }
    slider_col->addWidget(ticks);
    tl->addWidget(slider_host, 1);

    m_range_label = new QLabel(this);
    m_range_label->setFont(Theme::mono_font(11, 700));
    m_range_label->setMinimumWidth(Theme::px(230));
    Theme::restyle(m_range_label, [=] {
        return QString("color:%1;").arg((Theme::textHi).name());
    });
    tl->addWidget(m_range_label);
    bar_col->addLayout(tl);

    mid->addWidget(bar);
    root->addWidget(center, 1);

    // ---- 우: 날짜 열 ----
    // 보드의 우측 열은 280px 이고 좌측 열과 같은 면(elevated)에 왼쪽
    // 테두리가 있다 — 전엔 200px 생짜에 배경이 없어 페이지 바닥처럼 보였다.
    auto *side_host = new QWidget(this);
    side_host->setFixedWidth(Theme::px(244));   // ≒ 보드 280px
    side_host->setAttribute(Qt::WA_StyledBackground);
    Theme::restyle(side_host, [] {
        return QString("background:%1; border-left:1px solid %2;")
            .arg(Theme::chromeElevated.name(), Theme::border.name());
    });
    auto *side = new QVBoxLayout(side_host);
    side->setContentsMargins(14, 10, 14, 10);
    side->setSpacing(6);

    auto *date_title = new QLabel("DATE", side_host);
    date_title->setFont(Theme::ui_font(11, 600, 0.09));
    Theme::restyle(date_title, [=] {
        return QString("color:%1;").arg((Theme::textMuted).name());
    });
    side->addWidget(date_title);

    // 여러 날을 고르면 *같은 시각끼리* 합산된다 — "이 날들의 오후 2시".
    // 3일/7일 프리셋을 없애고 이걸로 대체했다. 연속하지 않은 날짜(월·수·금)도
    // 고를 수 있어 오히려 유연하다.
    m_date_list = new QListWidget(this);
    m_date_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_date_list->setFont(Theme::mono_font(10));
    m_date_list->setObjectName("Panel");
    m_date_list->setToolTip(
        "Ctrl+click to pick several days - the same time slots are summed");
    connect(m_date_list, &QListWidget::itemSelectionChanged,
            this, [this] { on_selection_changed(); });
    side->addWidget(m_date_list, 1);

    root->addWidget(side_host);
}

/**
 * @brief 슬롯 통계 4줄 — 좌측 View 패널 아래쪽
 *
 * 보드는 이 블록을 우측(날짜) 열 밑에 두지만, 우리 날짜 목록은 보존 기간
 * 전체(14일)를 세로로 채우기 때문에 그 아래는 창 바닥에 눌려 읽을 수가
 * 없다(08-19 사용자 신고). 좌측 패널은 CH 토글 넷 아래가 통째로 비어
 * 있어서 같은 내용이 훨씬 잘 보인다.
 */
QWidget *CrowdPage::build_slot_stats(QWidget *parent)
{
    auto *host = new QWidget(parent);
    auto *col = new QVBoxLayout(host);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(6);

    // 지도는 "어디가 붐볐나"를 보여주지만 숫자를 읽어 주지는 않는다. 보드가
    // 이 네 줄을 둔 이유가 그거다 — 지금 보고 있는 칸의 요약.
    // 값은 전부 render_heat() 가 이미 순회하는 캐시에서 나온다(추가 조회 없음).
    m_slot_title = new QLabel("Slot -", host);
    m_slot_title->setFont(Theme::ui_font(11, 600));
    m_slot_title->setWordWrap(true);
    Theme::restyle(m_slot_title, [] {
        return QString("color:%1; border-top:1px solid %2;"
                       "border-bottom:1px solid %2; padding:7px 0;")
            .arg(Theme::textHi.name(), Theme::border.name());
    });
    col->addWidget(m_slot_title);

    // 좁은 열(264px)이라 라벨·값을 **두 줄로** 쌓는다 — 한 줄에 세우면
    // "total detections" 만으로 폭을 다 먹고 값이 잘린다.
    auto stat_row = [&](const QString &name, QLabel *&value_out) {
        auto *box = new QVBoxLayout();
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(1);
        auto *l = new QLabel(name, host);
        l->setFont(Theme::mono_font(9.5));
        Theme::restyle(l, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        box->addWidget(l);

        value_out = new QLabel(QString::fromUtf8("—"), host);
        value_out->setFont(Theme::mono_font(11, 700));
        // 색은 값의 **역할**이라 팔레트에서 그때그때 고른다(테마 전환 대응).
        // 붐빔=호박 · 최다 셀=경보색 · 한산=초록 — 앱 전역의 3색 규약 그대로.
        Theme::restyle(value_out, [name] {
            const QColor c = name.startsWith("peak")    ? Theme::amber
                           : name.startsWith("busiest") ? Theme::alarm
                           : name.startsWith("quietest") ? Theme::green
                                                         : Theme::textHi;
            return QString("color:%1;").arg(c.name());
        });
        box->addWidget(value_out);
        col->addLayout(box);
    };

    stat_row("peak zone",        m_stat_peak);
    stat_row("total detections", m_stat_total);
    stat_row("busiest cell",     m_stat_cell);
    stat_row("quietest zone",    m_stat_quiet);

    return host;
}

void CrowdPage::update_slot_stats(const QHash<int, qint64> &per_channel,
                                  qint64 total,
                                  const QPair<int, int> &busiest,
                                  double busiest_w,
                                  const QString &span)
{
    m_slot_title->setText(QString("Slot %1").arg(span));

    if (total <= 0) {
        const QString dash = QString::fromUtf8("—");
        m_stat_peak->setText(dash);
        m_stat_total->setText(dash);
        m_stat_cell->setText(dash);
        m_stat_quiet->setText(dash);
        return;
    }

    // 붐빈 채널 / 한산한 채널. **데이터가 온 채널만** 비교한다 — 아예 값이
    // 없는 채널을 0 으로 세면 "가장 한산한 곳"이 늘 꺼진 카메라가 된다.
    int peak_ch = -1, quiet_ch = -1;
    qint64 peak_v = -1, quiet_v = -1;
    for (auto it = per_channel.constBegin(); it != per_channel.constEnd(); ++it) {
        if (peak_v < 0 || it.value() > peak_v) { peak_v = it.value(); peak_ch = it.key(); }
        if (quiet_v < 0 || it.value() < quiet_v) { quiet_v = it.value(); quiet_ch = it.key(); }
    }

    const QString dash = QString::fromUtf8("—");
    m_stat_peak->setText(peak_ch > 0
        ? QString::fromUtf8("CH%1 · %2").arg(peak_ch).arg(peak_v) : dash);
    m_stat_quiet->setText(quiet_ch > 0 && per_channel.size() > 1
        ? QString::fromUtf8("CH%1 · %2").arg(quiet_ch).arg(quiet_v) : dash);

    // 천 단위 구분 — 네 자리가 넘어가면 눈으로 자릿수를 못 센다
    m_stat_total->setText(QLocale().toString(qlonglong(total)));

    // 최다 셀은 **평면도 격자** 좌표다(카메라 격자가 아니다) — 지도에서
    // 바로 짚을 수 있어야 의미가 있다.
    m_stat_cell->setText(busiest_w > 0
        ? QString::fromUtf8("(%1, %2) · %3")
              .arg(busiest.first).arg(busiest.second).arg(qRound(busiest_w))
        : dash);
}

void CrowdPage::set_status(const QString &text, bool error)
{
    m_status->setStyleSheet(QString("color:%1;").arg((error ? Theme::alarm : Theme::textDim).name()));
    m_status->setText(text);
}

