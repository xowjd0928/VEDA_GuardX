#include "tracking_panel.h"
#include "floor_plan.h"
#include "wall_layout.h"
#include "calibration_store.h"
#include "crowd_page.h"
#include "panel_chrome.h"
#include "theme.h"
#include "zone_config.h"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

// ------------------------------------------------------------------- 유틸

/**
 * @brief 감지 시각을 KST 표기로
 *
 * ts는 카메라 시계다. 응답이 UTC로 오면 변환하고, 시간대 정보가 없는
 * (LocalTime) 값이면 이미 현지 시각이므로 그대로 쓴다.
 */
static QString kst_text(const QDateTime &ts)
{
    if (!ts.isValid())
        return "--:--:--";
    static QTimeZone kst = [] {
        QTimeZone tz("Asia/Seoul");
        return tz.isValid() ? tz : QTimeZone(9 * 3600);
    }();
    const QDateTime shown =
        ts.timeSpec() == Qt::LocalTime ? ts : ts.toTimeZone(kst);
    return shown.toString("HH:mm:ss");
}

/** @brief "CH1 · LOBBY EAST" -> "LOBBY EAST" (구역명만) */
static QString zone_of(int channel)
{
    const QString full = Theme::channel_name(channel);
    const int sep = full.indexOf(QString::fromUtf8(" · "));
    return sep < 0 ? full : full.mid(sep + 3);
}

/** @brief mm:ss (동선 체류 시간) */
static QString duration_text(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const qint64 s = ms / 1000;
    return QString("%1:%2")
        .arg(s / 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0'));
}

/**
 * @brief 화면 채널 -> FLOOR MAP 2×2 칸 (LIVE 그리드와 **같은 배치**)
 *
 * ⚠ `channel/2, channel%2` 로 직접 계산하면 안 된다 (08-20). 드래그 앤 드롭
 *   으로 벽 배치가 움직이면 그 고정식은 옛 배치에 남는데, 엣지맵 배경은
 *   실제 타일 위치대로 합성돼 오므로(finish_edge_map) **배경과 그 위의 점·
 *   라벨이 서로 다른 채널을 가리킨다.** 배치의 진실원천은 WallOrder 하나다.
 */
static void cell_of(int channel, int &row, int &col)
{
    WallOrder::cell_of(channel, row, col);
}

// -------------------------------------------------------------- FloorMiniMap

/**
 * @brief 평면도 위에 동선을 그리는 캔버스
 *
 * ★ 두 가지 모드로 그린다.
 *
 *   ① 실측 도면 — 캘리브레이션(calibration.json)이 있을 때.
 *      네 채널을 **한 방**에 겹쳐 그린다. 대상이 CH1에서 CH2로 넘어가도
 *      같은 방 안에서 이어지는 하나의 동선으로 보인다 (개략도에서는 칸이
 *      바뀌면서 끊겨 보였다). 가구도 실제 위치에 그리므로 "테이블 옆을
 *      지나갔다"가 화면에서 그대로 읽힌다.
 *      화각(가시영역)은 여기 그리지 않는다 — 동선을 가리기만 하고,
 *      그건 CROWD 페이지가 보여 준다.
 *
 *   ② 2×2 개략도 — 캘리브레이션 전. 방 크기도 가구 위치도 모르는 상태라
 *      채널 하나를 방 하나로 친 예전 그림으로 되돌아간다. 없는 정보를
 *      지어내는 것보다 "아직 개략도"라고 보여 주는 편이 낫다.
 */
class FloorMiniMap : public QWidget
{
public:
    explicit FloorMiniMap(QWidget *parent) : QWidget(parent)
    {
        setMinimumHeight(180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        // 캘리브레이션을 새로 불러오면 개략도 <-> 실측 도면이 바뀐다
        connect(CalibrationStore::instance(), &CalibrationStore::changed,
                this, QOverload<>::of(&QWidget::update));
    }

    /** @brief 대상 하나의 동선 + 구분색 + 표기 이름 */
    struct Entry {
        QVector<TrackPoint> path;
        QColor color;
        QString label;
    };

    void set_paths(const QVector<Entry> &entries)
    {
        m_entries = entries;
        update();
    }

    void set_edge_background(const QImage &image)
    {
        m_edge_background = image;
        m_edge_scaled = QImage();   // 캐시 무효화
        update();
    }

    /** @brief 실측 도면으로 그릴 수 있나 (헤더 표기를 맞추려고 밖에서도 본다) */
    static bool has_real_plan()
    {
        auto *store = CalibrationStore::instance();
        return store->loaded() && store->room_w_cm() > 0.0 && store->room_h_cm() > 0.0;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        p.setPen(QPen(Theme::border, 1));
        p.setBrush(Theme::bg0);
        p.drawRect(QRectF(0.5, 0.5, width() - 1.0, height() - 1.0));

        // ★ 레이어 순서를 **여기 한 곳에서만** 정한다:
        //      바탕(실측 도면 / 2×2 개략도) → 엣지맵 → 동선
        //
        // ⚠ 08-12 이전에는 도면 모드가 draw_real() 안에서 동선까지 그리고 곧바로
        //   return 했다. 그래서 **캘리브레이션을 한 번 올리면 엣지맵이 영영 안
        //   그려졌다** — EDGE MAP 버튼을 눌러도 아무 일이 없어서 기능이 죽은 것처럼
        //   보였다(엣지 그리기가 개략도 경로에만 있었다).
        //   그리는 순서를 모드별 함수 밖으로 꺼내면 그 실수가 구조적으로 막힌다:
        //   모드가 하나 늘어도 동선은 언제나 맨 위다.
        const bool real = has_real_plan();

        // ★ 엣지맵을 켜면 **엣지만** 본다 — 앱 배경 + 소벨선, 그 위에 동선.
        //   도면 격자·가구와 겹쳐 놓으면 선이 두 겹이 되어 둘 다 못 읽는다.
        //   끄면 원래 바탕(도면/개략도)으로 그대로 돌아온다.
        //
        // 바탕은 순수 검정이 아니라 앱 배경색이다 — 순검정은 이 테마에서
        // 유일하게 튀는 면이라 패널 안에 구멍이 뚫린 것처럼 보였다.
        if (!m_edge_background.isNull()) {
            p.fillRect(rect().adjusted(1, 1, -1, -1), Theme::bg0);
            draw_edge_background(p, 1.0);
            // ⚠ 엣지맵 위에서는 **항상** 격자 좌표로 찍는다(캘리브레이션 무관).
            //   배경이 카메라 2×2 그림이라 도면 좌표는 아예 다른 공간이다.
            for (int i = m_entries.size() - 1; i >= 0; --i)
                draw_path(p, m_entries[i], /*on_edge=*/true);
            draw_edge_labels(p);
            return;   // 좌표계 불일치 안내는 도면 모드의 이야기다 — 여기선 무관
        }

        if (real) {
            draw_real_background(p);
        } else {
            // 주 대상(첫 원소)이 있는 방을 강조, 나머지는 "지나간 방"으로만
            const int current =
                (m_entries.isEmpty() || m_entries.first().path.isEmpty())
                    ? -1
                    : m_entries.first().path.last().channel;
            QSet<int> visited;
            for (const Entry &e : m_entries)
                for (const TrackPoint &pt : e.path)
                    visited.insert(pt.channel);

            for (int ch = 0; ch < 4; ++ch)
                draw_room(p, ch, ch == current, visited.contains(ch));
        }

        // 주 대상을 마지막에 그려 겹칠 때 위로 오게 한다
        for (int i = m_entries.size() - 1; i >= 0; --i) {
            if (real)
                draw_path_real(p, m_entries[i]);
            else
                draw_path(p, m_entries[i]);
        }

        draw_mismatch_note(p);
    }

private:
    /**
     * @brief 캡처한 엣지맵을 바탕에 깐다
     *
     * 지금은 검은 바탕 위 단독(불투명도 1.0)으로만 쓴다. 인자를 남겨 둔 이유는
     * 다른 바탕과 겹쳐 깔 자리가 다시 생기면 그때 낮춰 부르기 위해서다.
     */
    void draw_edge_background(QPainter &p, qreal opacity)
    {
        if (m_edge_background.isNull())
            return;

        const QRect target = rect().adjusted(8, 8, -8, -8);
        if (target.isEmpty())
            return;

        // ⚠ **1px 짜리 엣지선을 그냥 축소하면 선이 통째로 사라진다.** 기본
        //   변환은 최근접이라 1000px 캡처를 370px 로 줄이면 세 픽셀 중 둘을
        //   버리고, 남은 선도 반투명(alpha 150)이라 배경에 묻힌다. 08-13
        //   실측: 엣지맵을 켜도 화면에 아무 변화가 없었다.
        //   부드럽게 한 번 줄여서 **캐시**한다 — 선이 회색으로 뭉쳐 보이게 되고,
        //   250ms 갱신마다 큰 이미지를 다시 줄이던 비용도 함께 없어진다.
        if (m_edge_scaled.size() != target.size())
            m_edge_scaled = m_edge_background.scaled(target.size(),
                                                     Qt::IgnoreAspectRatio,
                                                     Qt::SmoothTransformation);

        p.save();
        p.setOpacity(opacity);
        p.drawImage(target.topLeft(), m_edge_scaled);
        p.restore();
    }

    /**
     * @brief 좌표를 못 놓는 이유를 지도 아래에 한 줄로 (없으면 아무것도 안 그린다)
     *
     * ⚠ 이게 없던 동안, 좌표계가 어긋난 캘리브레이션에서는 방과 가구만 그려지고
     *   동선은 **한 점도** 안 나왔다. 화면만 보면 추적이 죽은 것과 구분이 안 돼서
     *   엉뚱한 곳을 찾게 된다(08-13 실사고).
     */
    void draw_mismatch_note(QPainter &p)
    {
        const QString note = CalibrationStore::instance()->frame_mismatch_note();
        if (note.isEmpty())
            return;
        p.save();
        p.setOpacity(1.0);
        p.setPen(Theme::amber);
        p.setFont(Theme::mono_font(8.5));
        p.drawText(rect().adjusted(10, 0, -10, -6),
                   Qt::AlignBottom | Qt::AlignLeft | Qt::TextWordWrap, note);
        p.restore();
    }

    // ---- 실측 도면 모드 ----------------------------------------------------

    /**
     * @brief FLOOR 좌표 -> 위젯. 방 종횡비를 지킨다.
     *
     * FloorCanvas::view_transform 과 같은 규칙이다 (여백만 다르다) — 두 화면이
     * 같은 모양이어야 나란히 놓고 볼 수 있다.
     */
    QPointF floor_to_widget(const QPointF &f) const
    {
        auto *store = CalibrationStore::instance();
        const double rw = store->room_w_cm(), rh = store->room_h_cm();
        const double inset = 8;
        const double aw = width() - inset * 2, ah = height() - inset * 2;
        if (aw <= 0 || ah <= 0 || rw <= 0 || rh <= 0) return QPointF(0, 0);

        const double aspect = rw / rh;
        const double vw = qMin(aw, ah * aspect), vh = vw / aspect;
        const double ox = inset + (aw - vw) / 2.0, oy = inset + (ah - vh) / 2.0;
        return QPointF(ox + f.x() / FloorCanvas::FLOOR_W * vw,
                       oy + f.y() / FloorCanvas::FLOOR_H * vh);
    }

    /** @brief 방 실측 좌표(cm) -> 위젯 */
    QPointF cm_to_widget(const QPointF &cm) const
    {
        auto *store = CalibrationStore::instance();
        const double rw = store->room_w_cm(), rh = store->room_h_cm();
        return floor_to_widget(QPointF(FloorCanvas::FLOOR_W * (cm.x() / rw),
                                       FloorCanvas::FLOOR_H * (cm.y() / rh)));
    }

    /**
     * @brief 실측 도면의 **바탕만** — 격자·방 외곽·가구
     *
     * 동선은 여기서 그리지 않는다(paintEvent 가 엣지맵 뒤에 그린다). 예전에
     * 이 함수가 동선까지 그렸던 것이 엣지맵이 사라진 원인이다.
     */
    void draw_real_background(QPainter &p)
    {
        auto *store = CalibrationStore::instance();
        const double rw = store->room_w_cm(), rh = store->room_h_cm();

        // ---- 평면도 배경 (2026-08-24) — CROWD 와 **같은 파일**을 깐다 ----
        // 두 화면이 다른 그림을 깔면 같은 현장을 두 모습으로 말하게 된다.
        const QPixmap plan = FloorPlan::image();
        const bool has_plan = !plan.isNull() && rw > 0.0 && rh > 0.0;
        if (has_plan) {
            const QRectF room(cm_to_widget(QPointF(0, 0)),
                              cm_to_widget(QPointF(rw, rh)));
            p.drawPixmap(room, plan, QRectF(plan.rect()));
        }

        // ⚠ 1m 격자는 뺐다 (2026-08-25 지시) — CROWD 지도와 같은 이유다.
        //   도면은 "어디에 무엇이 있나"를 보여주는 그림이지 모눈종이가 아니고,
        //   좁은 패널에서는 격자가 동선·발밑점을 통째로 덮는다.

        // 방 외곽 (도면이 있으면 도면의 벽이 그 일을 한다)
        if (!has_plan) {
            QPolygonF room;
            room << cm_to_widget(QPointF(0, 0)) << cm_to_widget(QPointF(rw, 0))
                 << cm_to_widget(QPointF(rw, rh)) << cm_to_widget(QPointF(0, rh));
            p.setPen(QPen(Theme::border2, 2));
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(room);
        }

        // 가구 — CROWD 지도와 같은 규칙. 사진이 붙은 칸은 도면 위에도 그린다.
        const bool draw_plain = !has_plan || FloorPlan::show_obstacles();
        int fx_index = 0;
        p.setFont(Theme::mono_font(8, 600));
        for (const CalibrationStore::Obstacle &ob : store->obstacles()) {
            const QPixmap user_art =
                FloorPlan::prop_image(++fx_index, ob.channel, ob.name);
            if (user_art.isNull() && !draw_plain) continue;
            if (!store->channel(ob.channel).geo_usable && !store->force_draw()) continue;

            QPolygonF poly;
            for (const QPointF &cm : ob.footprint_cm) poly << cm_to_widget(cm);

            // 사진은 사용자가 지정했을 때만 (props/<번호>.png)
            if (!user_art.isNull()) {
                p.save();
                QPainterPath clip;
                clip.addPolygon(poly);
                p.setClipPath(clip, Qt::IntersectClip);
                p.drawPixmap(poly.boundingRect(), user_art,
                             QRectF(user_art.rect()));
                p.restore();
                p.setBrush(Qt::NoBrush);
            } else {
                // CROWD 지도와 **같은 기호**를 쓴다 — 두 화면이 같은 현장을
                // 같은 모습으로 말해야 한다.
                QColor ink = Theme::textMuted;
                ink.setAlpha(225);
                FloorPlan::draw_fixture(p, poly.boundingRect(), ob.name,
                                        ink, false);
            }

            // ⚠ 이름은 적지 않는다 (2026-08-25) — 이 패널은 폭 300px 라
            //   "가판대" 한 단어도 칸을 넘친다. 무엇인지는 배치가 말한다.
        }
        p.setBrush(Qt::NoBrush);
    }

    /**
     * @brief 사람이 낼 수 있는 최대 속도 (cm/s). 넘으면 변환이 튄 것으로 본다.
     *
     * 실내 보행은 ~140, 달리기가 ~500 이다. 400 은 "뛰어도 통과, 순간이동은
     * 차단"을 노린 값 — 지평선 근처에서 폭발한 점은 보통 한 틱에 방을
     * 가로지르므로 여유를 둬도 걸린다.
     */
    static constexpr double MAX_SPEED_CMS = 400.0;

    /// 연속 기각 한계. 넘으면 기준점 쪽이 틀렸다고 보고 다시 잡는다.
    static const int MAX_CONSEC_REJECT = 2;

    void draw_path_real(QPainter &p, const Entry &e)
    {
        auto *store = CalibrationStore::instance();
        const double rw = store->room_w_cm(), rh = store->room_h_cm();

        // 못 믿는 채널·지평선 근처 점은 to_floor 가 이미 걸러낸다. 여기서는
        // 남은 점 중 **직전 점에서 순간이동한 것**을 더 걸러낸다 — 변환이
        // 튀면 거리로 드러나므로, 기하를 몰라도 잡을 수 있는 마지막 그물이다.
        //
        // 기준점(anchor)은 마지막으로 **채택한** 점이다. 다만 그 기준점 자체가
        // 튄 값이면 이후 정상 점이 전부 기각되므로, 연속 기각이 이어지면
        // 기준점을 의심해 현재 점으로 다시 잡는다.
        QVector<QPointF> pts;
        QPointF anchor_cm;
        QDateTime anchor_ts;
        bool have_anchor = false;
        int consec_reject = 0;

        for (const TrackPoint &tp : e.path) {
            const QPointF f = FloorCanvas::to_floor_norm(tp.channel,
                                                         tp.cam.x(), tp.cam.y());
            if (f.x() < 0 || f.x() > FloorCanvas::FLOOR_W ||
                f.y() < 0 || f.y() > FloorCanvas::FLOOR_H) continue;

            const QPointF cm(f.x() * rw / FloorCanvas::FLOOR_W,
                             f.y() * rh / FloorCanvas::FLOOR_H);

            if (have_anchor) {
                const double dt = anchor_ts.msecsTo(tp.ts) / 1000.0;
                const double d  = std::hypot(cm.x() - anchor_cm.x(),
                                             cm.y() - anchor_cm.y());
                // dt<=0 은 위의 ts 단조 보장으로 안 오지만, 와도 0 나누기를 피한다
                if (dt > 0 && d / dt > MAX_SPEED_CMS
                    && consec_reject < MAX_CONSEC_REJECT) {
                    ++consec_reject;
                    continue;
                }
            }

            pts << floor_to_widget(f);
            anchor_cm = cm;
            anchor_ts = tp.ts;
            have_anchor = true;
            consec_reject = 0;
        }
        if (pts.isEmpty()) return;

        if (pts.size() > 1) {
            QPainterPath line;
            line.moveTo(pts.first());
            for (int i = 1; i < pts.size(); ++i) line.lineTo(pts[i]);

            QColor stroke = e.color;
            stroke.setAlphaF(0.85);
            QPen pen(stroke, 2);
            // ⚠ Qt 대시 패턴은 **펜 굵기의 배수**다 — {5,4} 는 5px 가 아니라
            //   10px 대시였다. 보드는 stroke-dasharray="4 3"(1:1 배율)이라
            //   굵기로 나눠 넣는다.
            pen.setDashPattern({ 4.0 / 2, 3.0 / 2 });
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(line);
        }

        for (int i = 0; i < pts.size(); ++i) {
            const bool last = i == pts.size() - 1;
            const double r = last ? 7 : 4;
            QColor trail = e.color;
            trail.setAlphaF(0.45);
            p.setPen(last ? QPen(Theme::textHi, 2)
                          : QPen(QColor(0x58, 0x62, 0x74, 0x88), 1));
            p.setBrush(last ? e.color : trail);
            p.drawEllipse(pts[i], r, r);
        }

        p.setPen(e.color);
        p.setFont(Theme::mono_font(8.5, 600));
        p.drawText(QPointF(pts.last().x() + 12, pts.last().y() + 3), e.label);
    }

    // ---- 2×2 개략도 모드 (캘리브레이션 전) ---------------------------------

    /** @brief 채널 칸의 사각형 (위젯 좌표) */
    QRectF cell_rect(int channel) const
    {
        const double inset = 6, gap = 6;
        const double cw = (width() - inset * 2 - gap) / 2.0;
        const double chh = (height() - inset * 2 - gap) / 2.0;
        int row = 0, col = 0;
        cell_of(channel, row, col);
        return QRectF(inset + col * (cw + gap), inset + row * (chh + gap),
                      cw, chh);
    }

    /** @brief 엣지맵 이미지가 차지하는 사각형 (draw_edge_background 와 동일) */
    QRect edge_rect() const { return rect().adjusted(8, 8, -8, -8); }

    /**
     * @brief 엣지맵 사분면에 CH 라벨 + 경계선
     *
     * 엣지선만 있으면 어느 사분면이 어느 채널인지 알 수 없다 — 도면 모드의
     * draw_room 이 하던 일을 여기서 최소한으로 대신한다.
     */
    void draw_edge_labels(QPainter &p)
    {
        const QRect er = edge_rect();
        const double qw = er.width() / 2.0, qh = er.height() / 2.0;

        p.save();
        p.setBrush(Qt::NoBrush);
        QColor div = Theme::border2;
        div.setAlpha(140);
        p.setPen(QPen(div, 1));
        p.drawLine(QPointF(er.left() + qw, er.top()),
                   QPointF(er.left() + qw, er.bottom()));
        p.drawLine(QPointF(er.left(), er.top() + qh),
                   QPointF(er.right(), er.top() + qh));

        p.setFont(Theme::mono_font(8.5, 700, 0.10));
        p.setPen(Theme::textMuted);
        for (int ch = 0; ch < 4; ++ch) {
            int row = 0, col = 0;
            cell_of(ch, row, col);
            const QRectF q(er.left() + col * qw, er.top() + row * qh, qw, qh);
            p.drawText(q.adjusted(6, 4, -4, 0), Qt::AlignLeft | Qt::AlignTop,
                       QString("CH%1").arg(ch + 1));
        }
        p.restore();
    }

    /**
     * @brief 동선 점 -> **엣지맵 위** 위젯 좌표
     *
     * 엣지맵은 LIVE 2×2 그리드를 그대로 굳힌 그림이다(finish_edge_map 이
     * m_grid_host 배치대로 합성한다). 그러니 그 위에 찍는 점도 **같은
     * 2×2 사분면**에 놓아야 한다 — 카메라 정규화 좌표(0..1)를 사분면에
     * 그대로 펴면 끝이고, **호모그래피(캘리브레이션)가 필요 없다.**
     *
     * ⚠ 예전에는 캘리브레이션이 올라와 있으면 엣지맵 위에도 평면도 좌표로
     *   (draw_path_real) 찍었다. 배경은 카메라 격자인데 점은 도면 좌표라
     *   둘이 전혀 안 맞았다 (08-19 사용자 신고).
     *
     * 타일 사이 간격(라이브 벽 2px)은 축소되면 1px 미만이라 무시한다.
     */
    QPointF to_widget_edge(const TrackPoint &pt) const
    {
        const QRect er = edge_rect();
        int row = 0, col = 0;
        cell_of(pt.channel, row, col);
        const double qw = er.width() / 2.0, qh = er.height() / 2.0;
        return QPointF(er.left() + col * qw + qw * qBound(0.0, pt.cam.x(), 1.0),
                       er.top()  + row * qh + qh * qBound(0.0, pt.cam.y(), 1.0));
    }

    /** @brief 동선 점 -> 위젯 좌표 (칸 안쪽 80% 영역에 매핑) */
    QPointF to_widget(const TrackPoint &pt) const
    {
        const QRectF cell = cell_rect(pt.channel);
        return QPointF(
            cell.left() + cell.width() * (0.10 + 0.80 * qBound(0.0, pt.cam.x(), 1.0)),
            cell.top() + cell.height() * (0.10 + 0.80 * qBound(0.0, pt.cam.y(), 1.0)));
    }

    void draw_room(QPainter &p, int channel, bool current, bool visited)
    {
        const QRectF r = cell_rect(channel);

        QColor border = current ? Theme::accent
                       : visited ? Theme::border2
                                 : Theme::border;
        if (current)
            border.setAlphaF(0.67);

        QColor fill = Theme::panel;
        if (current) {
            fill = Theme::accent;
            fill.setAlphaF(0.08);
        }
        p.setPen(QPen(border, 1));
        p.setBrush(fill);
        p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));

        p.setPen(current ? Theme::accent : Theme::textMuted);
        p.setFont(Theme::mono_font(9.5, 700, 0.10));
        p.drawText(r.adjusted(8, 5, -6, 0), Qt::AlignLeft | Qt::AlignTop,
                   QString("CH%1").arg(channel + 1));

        p.setPen(Theme::textDim);
        p.setFont(Theme::mono_font(8.5));
        p.drawText(r.adjusted(8, 19, -6, 0), Qt::AlignLeft | Qt::AlignTop,
                   zone_of(channel));
    }

    /**
     * @param on_edge 엣지맵 위에 그리는가 — 그러면 점을 **엣지 이미지가
     *        차지한 사각형**의 2×2 사분면에 놓는다(to_widget_edge).
     */
    void draw_path(QPainter &p, const Entry &e, bool on_edge = false)
    {
        const QVector<TrackPoint> &path = e.path;
        if (path.isEmpty())
            return;

        const auto to_px = [this, on_edge](const TrackPoint &tp) {
            return on_edge ? to_widget_edge(tp) : to_widget(tp);
        };

        if (path.size() > 1) {
            QPainterPath line;
            line.moveTo(to_px(path.first()));
            for (int i = 1; i < path.size(); ++i)
                line.lineTo(to_px(path[i]));

            QColor stroke = e.color;
            stroke.setAlphaF(0.85);
            QPen pen(stroke, 2);
            pen.setDashPattern({ 4.0 / 2, 3.0 / 2 });   // 보드 "4 3" (위 ⚠ 참조)
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(line);
        }

        // 경유 점 — 마지막(현재 위치)만 크게 + 흰 테두리
        for (int i = 0; i < path.size(); ++i) {
            const bool last = i == path.size() - 1;
            const QPointF c = to_px(path[i]);
            const double r = last ? 7 : 4;
            QColor trail = e.color;
            trail.setAlphaF(0.45);
            p.setPen(last ? QPen(Theme::textHi, 2)
                          : QPen(QColor(0x58, 0x62, 0x74, 0x88), 1));
            p.setBrush(last ? e.color : trail);
            p.drawEllipse(c, r, r);
        }

        // 여러 대상이 겹치면 시각이 아니라 누구인지가 먼저 필요하다
        const QPointF c = to_px(path.last());
        p.setPen(e.color);
        p.setFont(Theme::mono_font(8.5, 600));
        p.drawText(QPointF(c.x() + 12, c.y() + 3), e.label);
    }

    QVector<Entry> m_entries;
    QImage m_edge_background;
    /// 위젯 크기에 맞춰 부드럽게 줄여 둔 사본 (매 페인트마다 다시 줄이지 않는다)
    QImage m_edge_scaled;
};

// ------------------------------------------------------------------ TrackRow

/**
 * @brief ACTIVE 목록의 탭 가능한 행 하나
 *
 * NavButton과 같은 방식 — QAbstractButton을 직접 그려 clicked()를 그대로 쓴다.
 */
class TrackRow : public QAbstractButton
{
public:
    /** @param color 선택 중이면 그 대상의 구분색, 아니면 무효 QColor */
    TrackRow(const TrackId &track, int channel, const QString &right,
             const QColor &color, QWidget *parent)
        : QAbstractButton(parent), m_id(track), m_channel(channel),
          m_right(right), m_color(color), m_selected(color.isValid())
    {
        setFixedHeight(24);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QString::fromUtf8(
                       "Click to add this target to tracking - click again to "
                       "release it.\n"
                       "Each tracked target keeps its own colour, shown on the "
                       "CH%1 view and the floor map.")
                       .arg(channel + 1));
    }

    const TrackId &track_id() const { return m_id; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (m_selected || underMouse()) {
            QColor bg = m_selected ? m_color : Theme::elevated;
            if (m_selected)
                bg.setAlphaF(0.12);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRect(rect());
        }

        const QColor dot = m_selected ? m_color : Theme::textFaint;
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(QPointF(10, height() / 2.0), 3.5, 3.5);

        p.setFont(Theme::mono_font(10, m_selected ? 700 : 400));
        p.setPen(m_selected ? m_color : Theme::textMid);
        p.drawText(QRect(22, 0, 62, height()), Qt::AlignVCenter | Qt::AlignLeft,
                   TrackHistory::label(m_id));

        p.setFont(Theme::mono_font(9.5));
        p.setPen(Theme::textMuted);
        p.drawText(QRect(88, 0, 34, height()), Qt::AlignVCenter | Qt::AlignLeft,
                   QString("CH%1").arg(m_channel + 1));

        p.setPen(Theme::textDim);
        p.drawText(QRect(126, 0, width() - 134, height()),
                   Qt::AlignVCenter | Qt::AlignLeft, zone_of(m_channel));

        p.setPen(Theme::textFaint);
        p.drawText(QRect(0, 0, width() - 8, height()),
                   Qt::AlignVCenter | Qt::AlignRight, m_right);
    }

    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    TrackId m_id;
    int m_channel;
    QString m_right;
    QColor m_color;
    bool m_selected;
};

// ------------------------------------------------------------- TrackingPanel

TrackingPanel::TrackingPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("Panel");
    setAttribute(Qt::WA_StyledBackground);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(1, 1, 1, 1);
    lay->setSpacing(0);

    m_live_dot = new QLabel(QString::fromUtf8("● Live"), this);
    m_live_dot->setFont(Theme::mono_font(9.5, 500, 0.10));
    Theme::restyle(m_live_dot, [] {
        return QString("color:%1;").arg(Theme::green.name());
    });

    // 제목은 "Tracking" 하나 — "· Paths"까지 붙이면 부제·LIVE 점과 합쳐
    // 400px 헤더에서 잘린다 (08-19 스크린샷 실측: "Tracking · Patl").
    // 헤더 우측: [Clear] + LIVE 점. trailing 은 위젯 하나만 받으므로 묶는다.
    auto *head_right = new QWidget(this);
    auto *head_lay = new QHBoxLayout(head_right);
    head_lay->setContentsMargins(0, 0, 0, 0);
    head_lay->setSpacing(8);

    auto *clear_btn = new QPushButton(QStringLiteral("Clear"), head_right);
    clear_btn->setObjectName("OutlineBtn");
    clear_btn->setFixedHeight(20);
    clear_btn->setFont(Theme::mono_font(9, 600, 0.08));
    clear_btn->setCursor(Qt::PointingHandCursor);
    clear_btn->setToolTip(
        QString::fromUtf8("추적 지우기 — 타일 박스·평면도 선·Path Log 가 "
                          "한 번에 사라집니다. 새 감지는 곧바로 다시 쌓입니다."));
    connect(clear_btn, &QPushButton::clicked, this, &TrackingPanel::clear_all);
    head_lay->addWidget(clear_btn);
    head_lay->addWidget(m_live_dot);

    // ⚠ 부제("floor map · re-id paths")는 뺐다 — 패널이 300px 라 [Clear] 를
    //   넣는 순간 부제·버튼·LIVE 점이 겹쳐 버튼이 "e" 로 잘렸다(08-25 실측).
    //   같은 자리에서 08-19 에 제목을 줄인 것과 같은 이유다. 무엇을 보는
    //   패널인지는 아래 FLOOR MAP·Path Log 머리글이 이미 말한다.
    lay->addWidget(PanelChrome::header(
        QStringLiteral("Tracking"), QString(), this, head_right));

    lay->addWidget(build_selected_strip());
    lay->addWidget(build_map_section(), 3);
    lay->addWidget(build_active_list());
    lay->addWidget(build_path_log(), 2);

    // LIVE 점 깜박임 — 디자인의 blink 1.6s 근사 (ChannelView와 같은 반주기)
    auto *blink = new QTimer(this);
    connect(blink, &QTimer::timeout, this, [this] {
        m_blink_on = !m_blink_on;
        QColor c = Theme::green;
        if (!m_blink_on)
            c.setAlphaF(0.25);
        m_live_dot->setStyleSheet(QString("color:%1;").arg(c.name()));
    });
    blink->start(800);

    connect(TrackHistory::instance(), &TrackHistory::updated,
            this, &TrackingPanel::refresh);
    // 구역 이름은 DB에서 바뀐다. 감지가 없으면 위 시그널이 안 와서 FLOOR MAP
    // 칸 이름과 ACTIVE 행이 옛 이름으로 남는다 — 이름 변경도 갱신 계기다.
    connect(ZoneConfig::notifier(), &ZoneConfig::Notifier::changed,
            this, &TrackingPanel::refresh);
    refresh();
}

QWidget *TrackingPanel::build_selected_strip()
{
    auto *box = new QWidget(this);
    auto *outer = new QVBoxLayout(box);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *row = new QWidget(box);
    row->setFixedHeight(38);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(12, 0, 12, 0);
    lay->setSpacing(9);

    m_sel_id = new QLabel(QString::fromUtf8("—"), row);
    m_sel_id->setFont(Theme::mono_font(12, 800));
    m_sel_id->setStyleSheet(QString("color:%1;").arg((Theme::accent).name()));
    lay->addWidget(m_sel_id);

    m_sel_pill = new QLabel("Tracking", row);
    m_sel_pill->setFont(Theme::ui_font(9, 600, 0.10));
    Theme::restyle(m_sel_pill, [=] {
        return QString("color:%1; border:1px solid %2; border-radius:2px;"
                "padding:2px 6px; background:transparent;")
            .arg(Theme::accent.name(), Theme::border2.name());
    });
    lay->addWidget(m_sel_pill);

    m_sel_dwell = new QLabel(QString(), row);
    m_sel_dwell->setFont(Theme::mono_font(9.5));
    Theme::restyle(m_sel_dwell, [=] {
        return QString("color:%1;").arg((Theme::textMuted).name());
    });
    lay->addWidget(m_sel_dwell);

    // 현장 LED 매트릭스 송출 토글. 좌표는 RPi B가 DB에서 꺼내 보내므로
    // 이 버튼이 하는 일은 "누구를 볼지" 알리는 것뿐이다.
    // 라벨을 두 글자로 눌러 둔다 — 스트립은 400px 고정인데 여기에 id·필·
    // dwell·now가 이미 들어 있어, 긴 라벨을 쓰면 "TRACKING ×2"나 한 시간
    // 넘긴 dwell에서 글자가 잘린다(라벨에 elide가 없다).
    m_sel_matrix = new QPushButton("Track", row);
    m_sel_matrix->setObjectName("OutlineBtn");   // 전역 QSS (두 테마 모두 정의)
    m_sel_matrix->setCursor(Qt::PointingHandCursor);
    m_sel_matrix->setCheckable(true);
    m_sel_matrix->setFont(Theme::ui_font(9, 600, 0.10));
    m_sel_matrix->setMinimumWidth(Theme::px(52));
    m_sel_matrix->setToolTip(QString::fromUtf8(
        "Shows this target's current and predicted position on the on-site LED matrix floor plan"));
    connect(m_sel_matrix, &QPushButton::clicked, this, [this](bool on) {
        m_matrix_on = on;
        sync_matrix_output();
    });
    lay->addWidget(m_sel_matrix);

    lay->addStretch(1);

    m_sel_now = new QLabel(QString(), row);
    m_sel_now->setFont(Theme::mono_font(9.5));
    Theme::restyle(m_sel_now, [] {
        return QString("color:%1;").arg(Theme::textMuted.name());
    });
    lay->addWidget(m_sel_now);

    outer->addWidget(row);

    auto *line = new QWidget(box);
    line->setObjectName("PanelHeaderLine");
    line->setAttribute(Qt::WA_StyledBackground);
    line->setFixedHeight(1);
    outer->addWidget(line);
    return box;
}

QWidget *TrackingPanel::build_map_section()
{
    auto *box = new QWidget(this);
    auto *lay = new QVBoxLayout(box);
    lay->setContentsMargins(14, 10, 14, 8);
    lay->setSpacing(8);

    auto *cap_row = new QWidget(box);
    auto *cap_lay = new QHBoxLayout(cap_row);
    cap_lay->setContentsMargins(0, 0, 0, 0);

    auto *cap = new QLabel(QString::fromUtf8("Floor Map"), cap_row);
    cap->setFont(Theme::ui_font(9.5, 500, 0.14));
    Theme::restyle(cap, [=] {
        return QString("color:%1;").arg((Theme::textDim).name());
    });
    cap_lay->addWidget(cap);
    cap_lay->addStretch(1);

    // 개략도인지 실측 도면인지는 캘리브레이션 유무로 갈린다 — 어느 쪽을
    // 보고 있는지 헤더에 적어 준다 (아래 refresh_map_caption 이 갱신).
    m_map_mode = new QLabel(cap_row);
    m_map_mode->setFont(Theme::mono_font(9));
    Theme::restyle(m_map_mode, [=] {
        return QString("color:%1;").arg((Theme::textFaint).name());
    });
    cap_lay->addWidget(m_map_mode);
    lay->addWidget(cap_row);

    m_map = new FloorMiniMap(box);
    lay->addWidget(m_map, 1);

    refresh_map_caption();
    connect(CalibrationStore::instance(), &CalibrationStore::changed,
            this, &TrackingPanel::refresh_map_caption);
    // LIVE 벽을 드래그로 재배치하면 엣지맵·개략도의 칸도 따라와야 한다 —
    // 배경(엣지맵)은 이미 새 배치로 합성돼 오므로, 안 따라오면 배경과 점이
    // 서로 다른 채널을 가리킨다 (08-20).
    connect(WallOrder::notifier(), &WallOrder::Notifier::changed, m_map,
            QOverload<>::of(&QWidget::update));
    return box;
}

QWidget *TrackingPanel::build_active_list()
{
    auto *box = new QWidget(this);
    box->setFixedHeight(104);
    auto *lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *line = new QWidget(box);
    line->setObjectName("PanelHeaderLine");
    line->setAttribute(Qt::WA_StyledBackground);
    line->setFixedHeight(1);
    lay->addWidget(line);

    auto *cap_row = new QWidget(box);
    auto *cap_lay = new QHBoxLayout(cap_row);
    cap_lay->setContentsMargins(14, 8, 14, 4);
    cap_lay->setSpacing(8);

    auto *cap = new QLabel(QString::fromUtf8("Active"), cap_row);
    cap->setFont(Theme::ui_font(9.5, 500, 0.14));
    Theme::restyle(cap, [=] {
        return QString("color:%1;").arg((Theme::textDim).name());
    });
    cap->setToolTip(QString::fromUtf8(
        "Everyone currently detected. Click a row to track that person - "
        "click again to release. Multiple people can be tracked at once."));
    cap_lay->addWidget(cap);
    cap_lay->addStretch(1);

    m_active_caption = new QLabel(QString(), cap_row);
    m_active_caption->setFont(Theme::mono_font(9));
    Theme::restyle(m_active_caption, [=] {
        return QString("color:%1;").arg((Theme::textFaint).name());
    });
    cap_lay->addWidget(m_active_caption);
    lay->addWidget(cap_row);

    auto *scroll = new QScrollArea(box);
    scroll->setObjectName("TimelineScroll");
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_active_host = new QWidget(scroll);
    m_active_lay = new QVBoxLayout(m_active_host);
    m_active_lay->setContentsMargins(2, 0, 2, 4);
    m_active_lay->setSpacing(0);
    m_active_lay->addStretch(1);
    scroll->setWidget(m_active_host);
    lay->addWidget(scroll, 1);
    return box;
}

QWidget *TrackingPanel::build_path_log()
{
    auto *box = new QWidget(this);
    auto *lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *line = new QWidget(box);
    line->setObjectName("PanelHeaderLine");
    line->setAttribute(Qt::WA_StyledBackground);
    line->setFixedHeight(1);
    lay->addWidget(line);

    lay->addWidget(PanelChrome::header(
        "Path Log", QString::fromUtf8("re-id · 200ms poll"), box));

    auto *scroll = new QScrollArea(box);
    scroll->setObjectName("TimelineScroll");
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_log_host = new QWidget(scroll);
    m_log_lay = new QVBoxLayout(m_log_host);
    m_log_lay->setContentsMargins(14, 6, 14, 8);
    m_log_lay->setSpacing(6);
    m_log_lay->addStretch(1);
    scroll->setWidget(m_log_host);
    lay->addWidget(scroll, 1);
    return box;
}

int TrackingPanel::max_targets()
{
    return Theme::track_color_count();
}

void TrackingPanel::set_edge_map(const QImage &image)
{
    if (!m_map)
        return;
    m_map->set_edge_background(image);
}

void TrackingPanel::select(int channel, int object_id)
{
    // 우클릭이 넘겨준 것은 (채널, object_id)뿐이다. global_id가 실리면
    // TrackHistory가 그 키로 저장하므로, 같은 대상의 global 트랙을 찾아본다.
    TrackHistory *hist = TrackHistory::instance();
    TrackId candidate;
    candidate.channel = channel;
    candidate.object_id = object_id;

    for (const TrackId &id : hist->active()) {
        if (id.is_global() && hist->current_channel(id) == channel
            && hist->current_object_id(id) == object_id) {
            candidate = id;
            break;
        }
    }
    toggle(candidate);
}

int TrackingPanel::free_slot() const
{
    for (int slot = 0; slot < max_targets(); ++slot) {
        bool used = false;
        for (const Target &t : m_sel)
            used = used || t.slot == slot;
        if (!used)
            return slot;
    }
    return -1;
}

QVector<TrackId> TrackingPanel::selected_ids() const
{
    QVector<TrackId> ids;
    ids.reserve(m_sel.size());
    for (const Target &t : m_sel)
        ids.append(t.id);
    return ids;
}

void TrackingPanel::toggle(const TrackId &id)
{
    if (!id.is_valid())
        return;

    const auto found = std::find_if(m_sel.cbegin(), m_sel.cend(),
                                    [&id](const Target &t) { return t.id == id; });

    if (found != m_sel.cend()) {
        // 이미 추적 중인 대상을 다시 누르면 해제 — 박스가 사라진다.
        m_sel.removeAt(int(found - m_sel.cbegin()));
    } else {
        // 클릭은 항상 "더하기"다 (08-19: Ctrl 조합 폐지 — 단일 선택 모드 없음).
        // 상한을 넘으면 가장 오래된 것을 밀어낸다 — 클릭을 무시하면
        // 사용자는 왜 안 되는지 알 수 없다
        if (m_sel.size() >= max_targets())
            m_sel.removeFirst();
        m_sel.append({ id, free_slot() });
    }

    refresh();
}

void TrackingPanel::clear_all()
{
    // ① 선택 해제 — 타일 박스와 평면도 선이 여기서 나온다
    m_sel.clear();
    // ② 궤적 저장소 비우기 — Path Log 와 "다시 고르면 되살아나는 옛 선"의 원천
    TrackHistory::instance()->clear();
    // ③ 매트릭스 송출은 대상이 없으면 의미가 없다. refresh() 안의
    //    sync_matrix_output() 이 주 대상 부재를 보고 STOP 을 보낸다.
    refresh();

    qInfo() << "[Tracking] 추적 지움 — 선택·궤적·로그 전부";
}

QColor TrackingPanel::color_of(const TrackId &id) const
{
    for (const Target &t : m_sel)
        if (t.id == id)
            return Theme::track_color(t.slot);
    return QColor();
}

void TrackingPanel::prune_selection()
{
    // 이력에서 지워진(10분 경과) 대상은 색만 잡아먹으므로 뺀다.
    //
    // 비어도 아무것도 채우지 않는다. 시작 직후와 전부 해제한 뒤는 같은
    // 상태여야 한다 — 감시자가 고르지 않은 사람을 골라준 것처럼 노랗게
    // 칠해두면 "누가 추적 중인지"를 화면이 거짓으로 말하게 된다.
    TrackHistory *hist = TrackHistory::instance();
    for (int i = m_sel.size() - 1; i >= 0; --i)
        if (!hist->contains(m_sel[i].id))
            m_sel.removeAt(i);
}

void TrackingPanel::refresh()
{
    prune_selection();
    refresh_strip();
    refresh_active();
    refresh_log();

    TrackHistory *hist = TrackHistory::instance();
    QVector<FloorMiniMap::Entry> entries;
    for (const Target &t : m_sel)
        entries.append({ hist->path(t.id), Theme::track_color(t.slot),
                         TrackHistory::label(t.id) });
    m_map->set_paths(entries);

    // 선택 목록이 그대로여도 매번 알린다 — 대상이 다른 카메라로 넘어가면
    // 강조해야 할 채널·object_id가 바뀐다. 내용이 같으면 ChannelView가
    // 조기 반환하므로 재도색은 없다.
    emit selection_changed(selected_ids());

    // 선택이 비거나 주 대상이 바뀌었을 수 있다. 여기서 한 번 맞춘다.
    sync_matrix_output();
}

void TrackingPanel::sync_matrix_output()
{
    const TrackId primary = m_sel.isEmpty() ? TrackId() : m_sel.first().id;
    const bool want = m_matrix_on && primary.is_valid();

    if (!want) {
        if (!m_matrix_id.is_valid())
            return;   // 이미 꺼져 있다 — 같은 STOP을 되풀이하지 않는다

        m_matrix_id = TrackId();
        // 대상이 사라져서 꺼진 경우까지 토글을 눌러둔 상태로 남기면, 다음에
        // 다른 사람을 고르는 순간 의도치 않게 송출이 시작된다.
        m_matrix_on = false;
        m_sel_matrix->setChecked(false);
        m_sel_matrix->setText("Track");
        emit matrix_output_changed(TrackId(), false);
        return;
    }

    if (m_matrix_id == primary)
        return;   // 같은 대상 — 재전송은 TrackDisplayLink의 주기가 맡는다

    m_matrix_id = primary;
    emit matrix_output_changed(primary, true);
}

void TrackingPanel::refresh_strip()
{
    TrackHistory *hist = TrackHistory::instance();

    if (m_sel.isEmpty()) {
        m_sel_id->setText(QString::fromUtf8("—"));
        m_sel_pill->setVisible(false);
        m_sel_dwell->setText("no target");
        m_sel_now->clear();
        m_sel_matrix->setEnabled(false);
        m_sel_matrix->setChecked(false);
        return;
    }

    // 대상이 여럿이어도 LED는 점 한 쌍(현재점·예측점)만 그린다 — 주 대상이
    // 나간다는 사실을 버튼에 적어 두어야 "왜 두 명인데 하나만 뜨지"가 안 된다.
    m_sel_matrix->setEnabled(true);
    m_sel_matrix->setChecked(m_matrix_on);
    m_sel_matrix->setText(m_matrix_on ? QString("Tracking") : QString("Track"));

    // 스트립은 주 대상(첫 원소) 기준이다. 여러 개면 개수만 덧붙인다 —
    // dwell/now를 N개 늘어놓으면 400px에 들어가지 않는다.
    const TrackId primary = m_sel.first().id;
    const QColor color = Theme::track_color(m_sel.first().slot);

    m_sel_id->setText(TrackHistory::label(primary));
    m_sel_id->setStyleSheet(QString("color:%1;").arg(color.name()));

    m_sel_pill->setVisible(true);
    m_sel_pill->setText(m_sel.size() > 1
                            ? QString::fromUtf8("Tracking ×%1").arg(m_sel.size())
                            : QString("Tracking"));
    Theme::restyle(m_sel_pill, [=] {
        return QString("color:%1; border:1px solid %2; border-radius:2px;"
                "padding:2px 6px; background:transparent;")
            .arg(color.name(), Theme::border2.name());
    });

    const QDateTime first = hist->first_seen(primary);
    const QDateTime last = hist->last_seen(primary);
    m_sel_dwell->setText(
        QString::fromUtf8("dwell %1").arg(duration_text(first.msecsTo(last))));

    const int ch = hist->current_channel(primary);
    m_sel_now->setText(ch < 0 ? QString()
                              : QString("now CH%1").arg(ch + 1));
}

/**
 * @brief 레이아웃의 행 위젯을 비운다 (마지막 stretch는 남긴다)
 *
 * delete가 아니라 deleteLater다 — 행을 다시 만드는 계기가 그 행의 clicked()
 * 핸들러이고, 거기서 즉시 delete하면 아직 마우스 이벤트를 처리 중인 위젯을
 * 지우게 된다 (use-after-free). 레이아웃에서 빼고 숨겨두면 화면에서는
 * 즉시 사라지고 실제 소멸은 이벤트 루프로 넘어간다.
 */
static void clear_rows(QVBoxLayout *lay)
{
    while (lay->count() > 1) {
        QLayoutItem *item = lay->takeAt(0);
        if (QWidget *w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
}

void TrackingPanel::refresh_active()
{
    // 행은 매번 다시 만든다 — 최대 수십 개라 비용이 무의미하고, 상태(선택/채널)를
    // 기존 위젯에 맞춰 갱신하는 코드가 오히려 길다.
    clear_rows(m_active_lay);

    TrackHistory *hist = TrackHistory::instance();
    const QVector<TrackId> active = hist->active();
    m_active_caption->setText(QString("%1 / %2 tracked")
                                  .arg(m_sel.size())
                                  .arg(active.size()));

    if (active.isEmpty()) {
        auto *empty = new QLabel("  no detections", m_active_host);
        empty->setFont(Theme::mono_font(9.5));
        Theme::restyle(empty, [=] {
            return QString("color:%1;").arg((Theme::textFaint).name());
        });
        m_active_lay->insertWidget(0, empty);
        return;
    }

    // 이미 추적 중인 대상을 위로 — 여러 개를 고른 뒤 목록이 뒤섞이면
    // 무엇을 보고 있었는지 잃는다
    QVector<TrackId> ordered = selected_ids();
    for (const TrackId &id : active)
        if (!ordered.contains(id))
            ordered.append(id);

    int inserted = 0;
    for (const TrackId &id : ordered) {
        const int ch = hist->current_channel(id);
        if (ch < 0)
            continue;

        auto *row = new TrackRow(id, ch, kst_text(hist->last_seen(id)),
                                 color_of(id), m_active_host);
        connect(row, &QAbstractButton::clicked, this, [this, id] {
            // global_id 대기 중 임시 조작 — 패널에서 대상을 고르면 그 카메라
            // 화면에서 같은 색으로 강조된다 (박스 우클릭과 같은 결과).
            toggle(id);
        });
        m_active_lay->insertWidget(inserted++, row);
    }
}

void TrackingPanel::refresh_map_caption()
{
    if (!m_map_mode) return;

    auto *store = CalibrationStore::instance();
    if (FloorMiniMap::has_real_plan()) {
        m_map_mode->setText(QString::fromUtf8("1F · %1 × %2 cm")
                                .arg(store->room_w_cm(), 0, 'f', 0)
                                .arg(store->room_h_cm(), 0, 'f', 0));
    } else {
        m_map_mode->setText(QString::fromUtf8("1F · schematic"));
    }
}

void TrackingPanel::refresh_log()
{
    clear_rows(m_log_lay);

    TrackHistory *hist = TrackHistory::instance();

    // 점 하나하나가 아니라 "채널이 바뀐 순간"만 남긴다 — 동선의 의미 단위는
    // 방을 옮긴 사건이고, 200ms 폴링 점을 다 찍으면 로그가 스크롤만 된다.
    struct Hop { QDateTime ts; int channel; QColor color; QString label; };
    QVector<Hop> hops;
    for (const Target &t : m_sel) {
        const QVector<TrackPoint> path = hist->path(t.id);
        int prev_ch = -1;
        for (const TrackPoint &pt : path) {
            if (pt.channel == prev_ch)
                continue;
            prev_ch = pt.channel;
            hops.append({ pt.ts, pt.channel, Theme::track_color(t.slot),
                          TrackHistory::label(t.id) });
        }
    }
    if (hops.isEmpty())
        return;

    // 여러 대상의 이동을 시각순으로 섞는다 — "누가 먼저 어디로 갔나"가
    // 다중 추적에서 보고 싶은 것이다
    std::sort(hops.begin(), hops.end(), [](const Hop &a, const Hop &b) {
        return a.ts > b.ts;  // 최신이 위
    });

    const bool multi = m_sel.size() > 1;
    int inserted = 0;
    for (int i = 0; i < hops.size(); ++i) {
        const Hop &hop = hops[i];

        auto *row = new QWidget(m_log_host);
        auto *lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(9);

        auto *time = new QLabel(kst_text(hop.ts), row);
        time->setFont(Theme::mono_font(9.5));
        time->setFixedWidth(58);
        Theme::restyle(time, [=] {
            return QString("color:%1;").arg((Theme::textDim).name());
        });
        lay->addWidget(time);

        auto *dot = new QLabel(QString::fromUtf8("●"), row);
        dot->setFont(Theme::mono_font(8));
        dot->setFixedWidth(10);
        Theme::restyle(dot, [=] {
            return QString("color:%1;").arg(hop.color.name());
        });
        lay->addWidget(dot);

        // 대상이 하나면 id 열은 스트립과 중복이라 생략한다
        if (multi) {
            auto *who = new QLabel(hop.label, row);
            who->setFont(Theme::mono_font(9.5, 600));
            who->setFixedWidth(56);
            Theme::restyle(who, [=] {
                return QString("color:%1;").arg(hop.color.name());
            });
            lay->addWidget(who);
        }

        auto *ch = new QLabel(QString("CH%1").arg(hop.channel + 1), row);
        ch->setFont(Theme::mono_font(9.5, 700));
        ch->setFixedWidth(34);
        Theme::restyle(ch, [=] {
            return QString("color:%1;").arg((Theme::textMid).name());
        });
        lay->addWidget(ch);

        auto *zone = new QLabel(zone_of(hop.channel), row);
        zone->setFont(Theme::mono_font(9.5));
        Theme::restyle(zone, [=] {
            return QString("color:%1;").arg((Theme::textMuted).name());
        });
        zone->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        lay->addWidget(zone, 1);

        m_log_lay->insertWidget(inserted++, row);
    }
}
