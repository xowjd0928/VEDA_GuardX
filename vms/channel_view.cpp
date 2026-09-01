#include "channel_view.h"
#include "alert_feed.h"
#include "theme.h"
#include "track_history.h"
#include "video_backend.h"
#include "zone_config.h"

#include <QApplication>
#include <QDateTime>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QPixmap>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <algorithm>

// 보간 갱신 주기(ms). 약 33fps.
static const int ANIM_INTERVAL_MS = 30;

// LIVE 점 깜박임 반주기(ms). 디자인의 gx-blink 1.6s 주기 근사.
static const int BLINK_INTERVAL_MS = 800;

// ---- 웹 UI 동형 프레임 매칭 (2026-08-05) ----
// 카메라 웹 UI(metaManager)의 구조를 그대로 가져왔다:
//   overlayList(≤50) 에 메타 프레임을 쌓고, 그릴 때 "영상의 현재 프레임
//   시각"에 최근접(±250ms)인 메타 프레임을 골라 통째로 그린다. 매칭되는
//   프레임이 없으면 아무것도 안 그린다 — 사람이 떠나면 메타가 끊기고,
//   영상 시각이 마지막 메타+250ms를 지나는 순간 박스가 저절로 사라진다.
// 궤적 재생·보간·dead-reckoning은 이 구조엔 없다 (박스가 영상과 록스텝인
// ONVIF 푸시라 필요가 없어졌다). 상수 값도 웹 UI와 동일.
static const int META_MATCH_MS = 250;     // metaTimeStampFilterTime
static const int OVERLAY_LIST_MAX = 50;   // limitLength

// 도착 후 이 이상 묵은 항목 청소 (웹 UI 청소 주기와 동일 — ONVIF가 죽으면
// 3초 안에 HTTP 폴백으로 전환된다)
static const int OVERLAY_KEEP_MS = 3000;
// 매칭 공백을 이 시간까지는 직전 그림으로 버틴다 (깜빡임·재도색 폭주 방지)
static const int BLANK_HOLD_MS = 400;

// ---- 기본 모드: 도착 즉시 (2026-08-24) ----
// 08-12 결론대로 **박스는 받는 대로 그리고, 정합은 영상을 늦춰서 맞춘다**
// (+/- 키 → VideoBackend::set_video_delay_ms). 그때 백엔드(delayq)만 만들고
// 화면 배선이 빠져 있어 손잡이가 HUD 숫자만 바꾸고 있었다 — 그 절반을 채운다.
//
// ⚠ 이 모드에선 "가장 최근 메타 프레임"을 그리므로, 메타가 멎으면 마지막
//   그림이 남는다. 그래서 **도착 나이로 반드시 늙힌다** (5Hz = 200ms 간격이라
//   두 프레임을 놓친 값). 이게 없으면 08-10 리뷰가 지적한 m_fallback_boxes 와
//   같은 병 — "몇 분 전 박스가 살아있는 영상 위에 박제"가 된다.
static const int LATEST_MAX_AGE_MS = 400;

/**
 * @brief 영상 시각에 맞춰 그리는 옛 모드로 되돌리는 스위치 (기본 꺼짐)
 *
 * QSettings `box_match_to_video=1` 이면 08-05 웹 UI 동형 매칭(±250ms)으로
 * 돌아간다. 두 방식은 **함께 쓸 수 없다** — 매칭이 살아 있으면 영상을 늦춰도
 * 매처가 그만큼 옛 메타를 골라 자동 보정하므로 +/- 손잡이가 무의미해진다.
 */
static bool match_to_video()
{
    static const bool on =
        QSettings("GuardX", "VMS").value("box_match_to_video", false).toBool();
    return on;
}

/** @brief OSD 텍스트를 그림자와 함께 그린다 (영상 위 가독성 확보) */
static void draw_osd_text(QPainter &p, const QPointF &pos, const QString &text,
                          const QColor &color)
{
    p.setPen(QColor(0, 0, 0, 200));
    p.drawText(pos + QPointF(0, 1), text);
    p.setPen(color);
    p.drawText(pos, text);
}

/**
 * @brief 사방 1px 외곽선을 두른 OSD 텍스트 (어떤 장면 위에서도 읽힌다)
 *
 * 08-24 에 상단 검은 띠를 없애면서 필요해졌다 — 그 띠의 존재 이유가 바로
 * **밝은 장면 가독성**이었기 때문이다(흰 대리석 바닥 위 흰 글자). 아래로
 * 한 번 깐 그림자로는 밝은 바닥에서 흰 글자가 묻힌다. 상자를 다시 만들지
 * 않으면서 대비를 내는 방법은 글자 자체에 테를 두르는 것뿐이다.
 */
static void draw_osd_outlined(QPainter &p, const QPointF &pos,
                              const QString &text, const QColor &color)
{
    p.setPen(QColor(0, 0, 0, 190));
    for (const QPointF &d : { QPointF(-1, 0), QPointF(1, 0),
                              QPointF(0, -1), QPointF(0, 1),
                              QPointF(1, 1), QPointF(-1, 1) })
        p.drawText(pos + d, text);
    p.setPen(color);
    p.drawText(pos, text);
}

/** @brief 반투명 배지(칩) 배경을 그리고 텍스트 시작 x를 돌려준다 */
static QRectF draw_badge_bg(QPainter &p, const QPointF &bottom_left,
                            double text_width, double h)
{
    const QRectF r(bottom_left.x(), bottom_left.y() - h,
                   text_width + 16, h);
    p.setPen(QColor(255, 255, 255, 31));
    p.setBrush(QColor(10, 13, 18, 199));
    p.drawRoundedRect(r, 2, 2);
    p.setBrush(Qt::NoBrush);
    return r;
}

// ---------------------------------------------------------------- BoxOverlay

BoxOverlay::BoxOverlay(ChannelView *view) : QWidget(view), m_view(view)
{
    // 영상 위에 얹히는 투명 레이어. 클릭은 아래(ChannelView)로 넘긴다.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
}

void BoxOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    m_view->paint_chrome(painter, size());
}

/**
 * 크롬 페인트 본체 — BoxOverlay(위젯)와 direct 경로(QImage→GPU 합성)가 공유.
 * 그리기 내용은 두 경로에서 완전히 동일해야 한다: 이 함수가 유일한 진실원천.
 */
void ChannelView::paint_chrome(QPainter &painter, const QSize &canvas) const
{
    painter.setRenderHint(QPainter::Antialiasing);

    // ★ 영상이 없는 동안(접속 중·재접속 중)에는 빈 타일에 마스코트를 세운다.
    //   기획서(DODAM_MASCOT_PLACEMENT §3)의 "기다리는 화면·비어 있는 화면"이다.
    //
    // ⚠ **영상 위에는 절대 안 그린다.** 조건이 has_frame() 하나뿐이라 프레임이
    //   한 장이라도 오면 이 블록 자체가 안 돈다 — 감시 대상 위에 그림을 얹는
    //   것은 같은 기획서의 금지 자리 1번이다(박스·블러를 가리면 화면이 증거로
    //   못 쓰인다). 겹쳐 그렸다 지우는 방식이 아니라 **아예 다른 상태**다.
    // ⚠ 정적이다. 움직이는 마스코트는 만들지 않는다(금지 4번) — LIVE 는 프레임
    //   예산이 빠듯하고, 애니메이션은 "멈춘 화면"과 "도는 화면"의 구분을 흐린다.
    if (m_backend && !m_backend->has_frame()) {
        // 타일 높이의 55%. 폭은 3:4 로 계산한다 — 두 값을 손으로 적으면 언젠가
        // 어긋난다(기획서 §5). 타일이 너무 작으면 아예 안 그린다.
        const int mh = qRound(canvas.height() * 0.55);
        const int mw = qRound(mh * 3.0 / 4.0);
        if (mw >= 48) {
            const QPixmap dodam = Theme::mascot(mw);
            if (!dodam.isNull()) {
                painter.save();
                // 빈 상태의 불투명도 규약(0.25)에 테마 보정을 곱한다.
                painter.setOpacity(0.25 * Theme::mascot_opacity());
                painter.drawPixmap(QRect((canvas.width() - mw) / 2,
                                         (canvas.height() - mh) / 2, mw, mh),
                                   dodam);
                painter.restore();
            }
        }
    }

    painter.setBrush(Qt::NoBrush);

    // 타일 테두리 — 영상 위젯이 타일 전체를 덮으므로 오버레이에서 그린다.
    // 혼잡 경보가 걸리면 여기 색이 바뀐다: warn은 굵은 amber(정적),
    // critical은 alarm이 LIVE 점과 같은 위상으로 점멸한다. 색만으로 판단하게
    // 두지 않으려고 좌상단에 문구 칩도 함께 띄운다 (아래).
    const int sev = m_alert_severity;
    if (sev > 0) {
        QColor c = (sev >= 2) ? Theme::OnVideo::alarm : Theme::OnVideo::amber;
        const double w = (sev >= 2) ? 3.0 : 2.0;
        if (sev >= 2 && !m_blink_on)
            c.setAlpha(110);
        painter.setPen(QPen(c, w));
        const double h = w / 2.0;
        painter.drawRect(
            QRectF(0, 0, canvas.width(), canvas.height()).adjusted(h, h, -h, -h));
    } else {
        // ⚠ 여기 있던 OnVideo::border2(#232B38)는 워크스페이스 전환 이전
        //   팔레트의 **푸른 회색** 잔재라, 검은 타일 위에서 혼자 파랗게
        //   떴다. 보드는 평상시 타일에 테두리를 두지 않는다 — 경계는 벽
        //   격자(2px 간격)가 이미 만든다. 아주 흐린 중립선만 남긴다.
        QColor idle = Theme::OnVideo::textHi;
        idle.setAlpha(20);
        painter.setPen(QPen(idle, 1));
        painter.drawRect(QRectF(0, 0, canvas.width(), canvas.height())
                             .adjusted(0.5, 0.5, -0.5, -0.5));
    }

    // ---- 얼굴 가림 (영역 계산은 애니메이션 틱, 여기선 그리기만) ----
    // ffmpeg 경로는 영상 픽셀 자체를 모자이크해서 그리므로 여기선 아무것도
    // 안 한다. GPU 상주 경로(GStreamer)만 픽셀 접근이 없어 불투명 마스크로
    // 대신 가린다. 박스 표시와 무관 (프라이버시 마스크는 추적 선택과 별개).
    const QVector<AnimatedBox> &boxes = m_anim_boxes;

    // ⚠ **영상이 없으면 박스도 블러도 그리지 않는다.** 메타데이터는 영상과
    //   별개 경로로 계속 들어오기 때문에, 재접속 중인 검은 타일 위에 박스만
    //   둥둥 떠 있었다 — 무엇을 감싼 사각형인지 화면에 없는 상태다. 그건
    //   "여기 사람이 있다"가 아니라 "여기 있었다는 기록"이라, 관제 화면에서
    //   둘을 같은 모양으로 보여주면 안 된다.
    //   같은 조건(has_frame)으로 마스코트가 대신 서 있다(위).
    const bool live = !m_backend || m_backend->has_frame();

    if (live && m_face_blur && !pixel_mosaic()) {
        for (const BlurRegion &region : m_blur_rects) {
            // 단색 마스크 대신 모자이크 타일 패턴 (2026-08-04). 픽셀 접근이
            // 없는 경로라 실제 영상 픽셀화는 아니지만 가림 효과는 동일하고,
            // 시각적으로 "모자이크 처리"로 읽힌다. 블록 톤은 위치·객체 기반
            // 결정적 의사난수 — 프레임마다 안 바뀌어 눈이 편하다.
            const QRectF r = region.rect;
            const int cols = qMax(3, int(r.width() / 9));
            const int rows = qMax(3, int(r.height() / 9));
            const double bw = r.width() / cols;
            const double bh = r.height() / rows;
            painter.setPen(Qt::NoPen);
            for (int by = 0; by < rows; ++by) {
                for (int bx = 0; bx < cols; ++bx) {
                    const uint h = uint(bx) * 73856093u ^ uint(by) * 19349663u
                                   ^ uint(region.object_id) * 83492791u;
                    const int tone = 0x38 + int(h % 0x48);
                    painter.setBrush(QColor(tone, tone + 4, tone + 8, 242));
                    painter.drawRect(QRectF(r.x() + bx * bw, r.y() + by * bh,
                                            bw + 0.5, bh + 0.5));
                }
            }
            painter.setBrush(Qt::NoBrush);
        }
    }

    // ---- 감지 박스 ----
    // 기본은 **추적 중인 사람에게만** (08-19 디자인 개편): 박스는 "감시자가
    // 우클릭으로 지목한 대상"의 표시이지 감지 자체의 표시가 아니다 — 평상시
    // 화면은 비워 두고 지목된 사람만 구분색으로 도드라진다.
    //
    // 08-24: 그래도 "전부 보고 싶다"는 요구가 있어 **All Boxes 토글**을 뒀다
    // (LIVE 좌측 Tools). 켜면 추적 안 하는 사람도 박스가 붙는데, **모양을
    // 다르게 한다** — 추적 대상은 굵은 구분색 + P-태그, 나머지는 가는 중립선에
    // 태그 없음. 같은 굵기·같은 색으로 그리면 "지목한 사람"이 군중에 묻혀
    // 08-19 에 이걸 지웠던 이유가 그대로 돌아온다.
    if (live) {
        const QFont tag_font = Theme::mono_font(9, 600);
        const QFontMetricsF tag_fm(tag_font);
        for (const AnimatedBox &box : boxes) {
            // 박스는 **사람만** 그린다 (2026-08-05 확정). Face/Head는 블러
            // 마스크 전용이다 — 블러 스위치가 꺼져 있어도 박스로 승격하지
            // 않는다 (같은 사람에 박스가 2~3겹 얹혀 화면만 어지럽다).
            if (box.category != 1)
                continue;

            // 색은 동선 패널이 배정한 그 대상의 구분색 (여러 대상을 동시에
            // 추적하므로 "선택 = 노랑" 하나로는 누가 누군지 구분되지 않는다)
            const QColor stroke = m_selected.value(box.object_id);
            const bool tracked = stroke.isValid();
            if (!tracked && !m_show_all_boxes)
                continue;   // 추적 대상이 아니고 토글도 꺼져 있으면 박스 없음

            if (!tracked) {
                // 감지만 표시 — 가는 중립선. 파랑(정보색)은 영상 위에서
                // 경보색(amber/alarm)과 헷갈리지 않는 유일한 자리다.
                QColor plain = Theme::OnVideo::accent;
                plain.setAlpha(170);
                painter.setPen(QPen(plain, 1));
                painter.drawRect(box.current);
                continue;                      // 태그는 추적 대상에게만
            }

            painter.setPen(QPen(stroke, 2));   // 보드 박스 굵기 2px
            painter.drawRect(box.current);

            // 태그: 박스 좌상단 위, 채운 배경 + 어두운 모노 텍스트
            const QString tag = QString("P-%1").arg(box.object_id);
            const QRectF tag_rect(box.current.left() - 0.75,
                                  box.current.top() - 17,
                                  tag_fm.horizontalAdvance(tag) + 10, 15);
            painter.setPen(Qt::NoPen);
            painter.setBrush(stroke);
            painter.drawRect(tag_rect);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(Theme::OnVideo::bg0);
            painter.setFont(tag_font);
            painter.drawText(tag_rect, Qt::AlignCenter, tag);
        }
    }

    // ---- 좌상단: 채널명 (2026-08-24 — 영상 위를 비운다) ----
    // 전에는 폭 전체에 반투명 검은 띠(.osdstrip)를 깔고 그 위에 LIVE 점·
    // 채널명·프로파일 캡션을 얹었다. 밝은 장면에서 글자가 안 읽히는 걸 띠로
    // 해결한 것이었는데, 사용자 판단은 **띠 자체가 화면을 더 해친다**는 것
    // (08-24 지시: "검정색으로 보이는 거 없애고 채널명만 흰색으로").
    //
    // 대비는 띠 대신 **글자 그림자**로 낸다(draw_osd_text 가 검은 1px 그림자를
    // 깐다) — 밝은 대리석 바닥 같은 장면에서도 흰 글자가 읽힌다.
    // ⚠ 함께 사라진 것: LIVE 깜박임 점 · 우상단 "Profile4 · H.264" 캡션.
    //   깜박임 점은 상단바 CAM pill 과 좌측 트리 점이 이미 같은 정보를 준다.
    {
        const QFont name_font = Theme::mono_font(11, 600, 0.08);
        painter.setFont(name_font);
        const QString name = Theme::channel_name(m_channel);
        draw_osd_outlined(painter, QPointF(10, 20), name, Qt::white);

        // 경보 칩만 남긴다 — 이건 장식이 아니라 **경보 표시**다. 색 단독으로
        // 판단하게 두지 않으려고 문구까지 넣은 자리라(테두리 색과 한 쌍),
        // 화면을 비우자고 지울 수 있는 것이 아니다.
        const QString atext = m_alert_text;
        if (!atext.isEmpty()) {
            const QColor c = (m_alert_severity >= 2) ? Theme::OnVideo::alarm
                                                     : Theme::OnVideo::amber;
            const QFont cf = Theme::mono_font(9, 600, 0.08);
            const QFontMetricsF cfm(cf);
            const double x = 10 + QFontMetricsF(name_font).horizontalAdvance(name) + 8;
            const QRectF chip(x, 7, cfm.horizontalAdvance(atext) + 12, 15);
            painter.setPen(Qt::NoPen);
            painter.setBrush(c);
            painter.drawRoundedRect(chip, 2, 2);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(Theme::OnVideo::bg0);
            painter.setFont(cf);
            painter.drawText(chip, Qt::AlignCenter, atext);
        }
    }

    // ---- 좌하단: OCC 배지 + 유입 속도 배지 ----
    {
        const QFont f = Theme::mono_font(10.5);
        const QFontMetricsF fm(f);
        painter.setFont(f);

        const int ch = m_channel;
        const int occ = occupancy();
        const int cap = ZoneConfig::capacity(ch);   // zone_thresholds가 진실원천
        const QColor occ_col = Theme::OnVideo::occ_color(double(occ) / cap,
                                                ZoneConfig::warn_ratio(ch),
                                                ZoneConfig::critical_ratio(ch));

        const QString lead = "OCC ";
        const QString num = QString::number(occ);
        const QString tail = QString("/%1").arg(cap);
        const double text_w = fm.horizontalAdvance(lead + num + tail);

        const QRectF badge = draw_badge_bg(
            painter, QPointF(10, canvas.height() - 8), text_w, 21);
        double x = badge.left() + 8;
        const double base_y = badge.center().y() + fm.ascent() / 2 - 1;
        painter.setPen(Theme::OnVideo::textHi);
        painter.drawText(QPointF(x, base_y), lead);
        x += fm.horizontalAdvance(lead);
        painter.setPen(occ_col);
        painter.drawText(QPointF(x, base_y), num);
        x += fm.horizontalAdvance(num);
        painter.setPen(Theme::OnVideo::textHi);
        painter.drawText(QPointF(x, base_y), tail);

        const QString flow = QString("%1%2/min")
                                 .arg(m_flow_per_min >= 0 ? "+" : "")
                                 .arg(m_flow_per_min, 0, 'f', 1);
        const QRectF flow_badge = draw_badge_bg(
            painter, QPointF(badge.right() + 6, canvas.height() - 8),
            fm.horizontalAdvance(flow), 21);
        painter.setPen(Theme::OnVideo::textMuted);
        painter.drawText(QPointF(flow_badge.left() + 8, base_y), flow);
    }

    // ---- 우하단: 타임스탬프 + 지연 HUD (기존 싱크 측정 정보 유지) ----
    // (08-19 오후: 한 번 시각을 뺐다가 사용자 요청으로 되살렸다 — 빼야 할
    //  것은 이게 아니라 하단 눈금자·티커였다)
    {
        QString ts;
        if (!boxes.isEmpty()) {
            QDateTime newest = boxes.first().ts;
            for (const AnimatedBox &box : boxes)
                if (box.ts > newest)
                    newest = box.ts;
            ts = newest.toString("HH:mm:ss.zzz");
        } else {
            ts = QTime::currentTime().toString("HH:mm:ss");
        }

        QString hud = QString::fromUtf8("%1 · delay %2ms")
                          .arg(ts).arg(m_playback_delay_ms);
        const qint64 latency = glass_latency_ms();
        if (latency >= 0)
            hud += QString::fromUtf8(" · video %1ms").arg(latency);

        const QFont f = Theme::mono_font(10);
        painter.setFont(f);
        const double w = QFontMetricsF(f).horizontalAdvance(hud);
        draw_osd_text(painter,
                      QPointF(canvas.width() - 10 - w, canvas.height() - 13),
                      hud, QColor(255, 255, 255, 153));
    }

    // ---- 상태 문구 — 스트림이 끊겼을 때 원인을 반드시 알린다 ----
    const QString status = status_text();
    if (!status.isEmpty()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(10, 13, 18, 210));
        painter.drawRect(QRect(1, 30, canvas.width() - 2, 26));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(Theme::OnVideo::amber);
        painter.setFont(Theme::ui_font(11, 700));
        painter.drawText(QRect(12, 30, canvas.width() - 24, 26),
                         Qt::AlignLeft | Qt::AlignVCenter, status);
    }

    // ---- 드롭 후보 하이라이트 — 끌던 타일이 여기 놓이려 한다 (08-20) ----
    // 크롬 맨 위에 얹는다: "여기에 놓인다"는 표시라 어떤 영상·배지 위에서도
    // 읽혀야 한다. 경보 테두리(amber/alarm)와 혼동되지 않게 accent 색이다.
    if (m_drop_hint) {
        QColor veil = Theme::OnVideo::accent;
        veil.setAlpha(26);
        painter.setPen(Qt::NoPen);
        painter.setBrush(veil);
        painter.drawRect(QRectF(0, 0, canvas.width(), canvas.height()));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Theme::OnVideo::accent, 3));
        painter.drawRect(QRectF(1.5, 1.5,
                                canvas.width() - 3, canvas.height() - 3));
    }
}

// --------------------------------------------------------------- ChannelView

ChannelView::ChannelView(int channel, int db_channel, QWidget *parent)
    : QWidget(parent), m_channel(channel), m_db_channel(db_channel)
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setMinimumSize(160, 120);

    // 타일 좌상단 이름은 DB(zones.zone_name)에서 온다. 영상이 흐르면 오버레이가
    // 알아서 다시 그려지지만, 스트림이 끊긴 채널은 멈춰 있으므로 명시적으로 친다.
    connect(ZoneConfig::notifier(), &ZoneConfig::Notifier::changed,
            this, &ChannelView::update_overlay);
    setContextMenuPolicy(Qt::NoContextMenu);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);

    m_backend = create_video_backend(channel, this);
    m_video = m_backend->create_widget(this);
    m_video->move(0, 0);

    // 사람 위 커서 힌트용 (mouseMoveEvent). 영상 위젯이 타일을 덮으므로
    // 거기도 켠다 — 자식이 무시한 move는 부모로 전파된다.
    setMouseTracking(true);
    m_video->setMouseTracking(true);

    m_overlay = new BoxOverlay(this);
    m_overlay->move(0, 0);
    m_overlay->raise();

    // direct 경로: 영상이 네이티브 HWND라 투명 위젯을 위에 못 얹는다(airspace).
    // 크롬은 update_overlay가 QImage로 그려 sink의 GPU 합성으로 보낸다.
    m_gpu_overlay = m_backend->gpu_composition();
    if (m_gpu_overlay) {
        m_overlay->hide();
        // 첫 프레임 전까지는 네이티브 창을 띄우지 않는다 — 띄워두면 sink가
        // 그리기 전까지 그 자리에 있던 픽셀이 그대로 보인다.
        m_video->hide();
    }

    connect(m_backend, &VideoBackend::status_changed, this, [this] {
        // 상태가 바뀌는 시점 = 세션 시작 / 첫 프레임 / 끊김. 영상 유무가
        // 갈리는 지점이 정확히 여기라 여기서 창 표시를 다시 판단한다.
        update_video_visibility();
        update_overlay();
        emit stream_event(m_channel, status_text()); // 이벤트 타임라인용
    });
    connect(m_backend, &VideoBackend::session_started, this,
            [this] { emit session_started(m_channel); });

    // LIVE 점 깜박임 — 오버레이 저빈도(1.25Hz) 갱신. 시계/배지도 함께 갱신된다.
    m_blink_timer = new QTimer(this);
    connect(m_blink_timer, &QTimer::timeout, this, [this] {
        m_blink_on = !m_blink_on;
        update_overlay();
    });
    m_blink_timer->start(BLINK_INTERVAL_MS);

    m_box_source = new BoxSource(db_channel, this);
    connect(m_box_source, &BoxSource::boxes_updated,
            this, &ChannelView::on_boxes_updated);

    // 혼잡 경보 — 박스 피드와 같이 타일이 스스로 구독한다 (LiveViewer를
    // 거치지 않아 배치가 바뀌어도 배선이 끊기지 않는다)
    connect(AlertFeed::instance(), &AlertFeed::state_changed,
            this, &ChannelView::refresh_alert);
    refresh_alert();

    // 그리기 틱 — 웹 UI가 영상 시각 콜백마다 getOverlayData를 부르는 것의
    // 등가물. 표시 중 프레임의 카메라 UTC에 맞는 메타 프레임을 고른다.
    m_anim_timer = new QTimer(this);
    connect(m_anim_timer, &QTimer::timeout, this, [this] {
        // 도착 후 오래 묵은 메타 프레임 청소 (웹 UI someClearOverlayData 역할)
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        while (!m_overlay_list.isEmpty()
               && now_ms - m_overlay_list.first().arrival_ms > OVERLAY_KEEP_MS)
            m_overlay_list.removeFirst();

        // 프레임 선택 — 웹 UI metaManager.getOverlayData와 동일 규칙
        const QVector<DetectionBox> *chosen = nullptr;
        static const QVector<DetectionBox> NONE;
        if (!m_overlay_list.isEmpty() && !match_to_video()) {
            // ---- 기본: 도착 즉시 ----
            // 가장 최근에 받은 메타 프레임을 그린다. 영상과의 정합은 여기서
            // 계산하지 않는다 — 운영자가 +/- 로 **영상을 늦춰** 맞춘다.
            const OverlayFrame &newest = m_overlay_list.last();
            if (now_ms - newest.arrival_ms <= LATEST_MAX_AGE_MS) {
                chosen = &newest.boxes;
                m_last_match_ms = now_ms;
                ++m_diag_latest;
            } else if (now_ms - m_last_match_ms < BLANK_HOLD_MS) {
                chosen = nullptr;          // 짧은 공백은 직전 그림 유지
                ++m_diag_hold;
            } else {
                chosen = &NONE;            // 메타가 멎었다 — 지운다
                ++m_diag_none;
            }
        } else if (!m_overlay_list.isEmpty()) {
            const qint64 e =
                m_backend ? m_backend->current_frame_utc_ms() : -1;
            if (e > 0) {
                if (m_diag_prev_e > 0 && e < m_diag_prev_e)
                    ++m_diag_eback;
                m_diag_prev_e = e;

                qint64 best = META_MATCH_MS + 1;
                int best_i = -1;
                for (int i = m_overlay_list.size() - 1; i >= 0; --i) {
                    const qint64 d = qAbs(m_overlay_list[i].utc_ms - e);
                    if (d < best) {
                        best = d;
                        best_i = i;
                    }
                }
                // ±250ms 안에 없으면 "그 프레임의 메타는 없다".
                // ⚠ 다만 **즉시 지우지는 않는다** (2026-08-05 성능 수리):
                // 매칭이 있다/없다를 오갈 때마다 전체 캔버스를 새로 칠하게 되고,
                // 4채널 × 6MB 캔버스 × 30Hz면 메모리 대역만 1.6GB/s다. 실제로
                // 그 부하가 소프트웨어 디코더를 굶겨 **영상 지연이 무한정
                // 누적**됐다(−450ms → 33초, 4분 실측). 짧은 공백은 직전 박스를
                // 유지해 깜빡임과 재도색을 함께 없앤다. 웹 UI도 영상이 늦어
                // 이 공백을 겪지 않으므로 동형성은 유지된다.
                if (best_i >= 0) {
                    chosen = &m_overlay_list[best_i].boxes;
                    m_last_match_ms = now_ms;
                    ++m_diag_matched;
                    m_diag_offsets.append(int(best));
                } else if (now_ms - m_last_match_ms < BLANK_HOLD_MS) {
                    chosen = nullptr;          // 직전 그림 유지 (재도색 없음)
                    ++m_diag_hold;
                } else {
                    chosen = &NONE;
                    ++m_diag_none;
                }
            } else {
                // 영상 시각을 모르는 백엔드(appsink 경로 등) = 웹 UI의
                // 비매칭 모드와 동일하게 최신 프레임을 그린다
                chosen = &m_overlay_list.last().boxes;
                ++m_diag_latest;
            }
        } else {
            chosen = &m_fallback_boxes;   // ONVIF 부재 — HTTP 스냅샷
            if (!m_fallback_boxes.isEmpty())
                ++m_diag_fallback;
        }

        // 채널별 매칭 통계 5초 로그 — "ch2만 버벅"이 매칭 공백인지, e 역행
        // 인지, 폴백 전환인지 숫자로 가른다. 활동이 있었던 채널만 찍는다.
        if (now_ms - m_diag_log_ms > 5000) {
            m_diag_log_ms = now_ms;
            if (m_diag_matched + m_diag_none + m_diag_latest
                    + m_diag_fallback > 0) {
                std::sort(m_diag_offsets.begin(), m_diag_offsets.end());
                const int p50 = m_diag_offsets.isEmpty()
                                    ? -1
                                    : m_diag_offsets[m_diag_offsets.size() / 2];
                const int pmax = m_diag_offsets.isEmpty()
                                     ? -1
                                     : m_diag_offsets.last();
                qDebug().noquote()
                    << QString("[Match] ch%1 5s: 매칭 %2 · 유지 %3 · 공백 %4 "
                               "· 최신 %5 · 폴백 %6 · **재도색 %7** "
                               "· |e-utc| p50 %8/max %9 ms · e역행 %10")
                           .arg(m_channel).arg(m_diag_matched)
                           .arg(m_diag_hold).arg(m_diag_none)
                           .arg(m_diag_latest).arg(m_diag_fallback)
                           .arg(m_diag_repaint).arg(p50).arg(pmax)
                           .arg(m_diag_eback);
            }
            m_diag_matched = m_diag_none = m_diag_latest = m_diag_fallback = 0;
            m_diag_hold = m_diag_repaint = 0;
            m_diag_eback = 0;
            m_diag_offsets.clear();
        }

        if (!chosen)
            return;    // 공백 유지 — 이번 틱은 아무것도 다시 계산·도색하지 않는다

        m_anim_boxes.clear();
        for (const DetectionBox &box : *chosen)
            m_anim_boxes.append({ box.object_id, to_view_rect(box), QRectF(),
                                  box.ts, box.category, box.parent_id });

        // ---- 얼굴 가림 영역 = Face·Head 박스 그 자체 (2026-08-05 최종) ----
        // Face(2)와 Head(3)를 **같은 대상**으로 본다: 같은 사람의 것이면 하나로
        // 합쳐 한 덩어리로 가린다 (둘 다 그리면 모자이크가 겹쳐 얼룩진다).
        // 사람 박스와 완전히 같은 경로에서 나온 좌표다 — 같은 ONVIF 프레임,
        // 같은 뷰 변환. 그래서 서로 어긋날 수가 없다.
        //
        // 잠시 "사람 박스 윗부분을 통째로 가리는" 방식을 썼다가 되돌렸다:
        // 얼굴 검출이 프레임마다 빠지던 HTTP 폴링 시절의 대증요법이었는데,
        // ONVIF 전환으로 Face/Head가 사람 박스와 록스텝으로 오게 되면서
        // 그 전제가 사라졌다. 어깨·배경까지 덮는 대가만 남아 되돌린다.
        m_blur_rects.clear();
        if (m_face_blur) {
            QHash<int, QRectF> merged;   // 사람(부모) 단위로 합친 가림 영역
            for (const AnimatedBox &b : m_anim_boxes) {
                if (b.category != 2 && b.category != 3)
                    continue;
                // 부모를 모르면 자기 자신을 키로 (음수라 부모 id와 안 겹친다)
                const int key = b.parent_id > 0 ? b.parent_id : -b.object_id;
                const auto it = merged.find(key);
                if (it == merged.end())
                    merged.insert(key, b.current);
                else
                    *it = it->united(b.current);
            }
            for (auto it = merged.cbegin(); it != merged.cend(); ++it) {
                // 받은 박스보다 10% 크게 (중심 유지) — 검출 박스가 얼굴에
                // 딱 맞아서 머리카락·턱 끝이 삐져나오는 것을 여유폭으로 덮는다
                const QRectF r = it.value();
                m_blur_rects.append(
                    { qAbs(it.key()),
                      r.adjusted(-r.width() * 0.05, -r.height() * 0.05,
                                 r.width() * 0.05, r.height() * 0.05) });
            }
        }

        // 실픽셀 모자이크 지원 백엔드(ffmpeg 등)에는 영역을 직접 넘긴다 —
        // 영상 자체가 뭉개져 그려지고, 오버레이는 마스크를 그리지 않는다
        if (m_backend && m_backend->supports_pixel_mosaic()) {
            QVector<QRectF> masks;
            masks.reserve(m_blur_rects.size());
            for (const BlurRegion &region : m_blur_rects)
                masks.append(region.rect);
            if (masks != m_last_masks) {
                m_backend->set_mosaic_rects(masks);
                m_last_masks = masks;
            }
        }

        // 실제로 달라졌을 때만 다시 그린다.
        // 오버레이는 반투명 위젯이라 한 번 그릴 때마다 창 전체가 재합성된다.
        // 무조건 33Hz로 갱신하면 박스가 없는 채널까지 GPU/CPU를 계속 먹는다.
        if (boxes_changed()) {
            ++m_diag_repaint;   // 재도색 횟수 = 오버레이 CPU 비용의 직접 지표
            update_overlay();
        }
    });
    m_anim_timer->start(ANIM_INTERVAL_MS);
}

bool ChannelView::boxes_changed()
{
    // 위치가 1px 미만으로 움직인 것은 화면상 차이가 없다 — 다시 그리지 않는다.
    static const double EPS = 1.0;

    bool changed = (m_anim_boxes.size() != m_drawn_boxes.size());
    if (!changed) {
        for (int i = 0; i < m_anim_boxes.size(); ++i) {
            const AnimatedBox &a = m_anim_boxes[i];
            const AnimatedBox &b = m_drawn_boxes[i];
            if (a.object_id != b.object_id
                || qAbs(a.current.x() - b.current.x()) > EPS
                || qAbs(a.current.y() - b.current.y()) > EPS
                || qAbs(a.current.width() - b.current.width()) > EPS
                || qAbs(a.current.height() - b.current.height()) > EPS) {
                changed = true;
                break;
            }
        }
    }

    // 가림 영역도 따로 봐야 한다 — 서 있는 사람의 얼굴 검출이 깜빡이면
    // 사람 박스는 그대로인데 마스크 높이만 바뀐다 (그때 재도색이 없으면
    // 마스크가 옛 자리에 박제된다)
    if (!changed) {
        if (m_blur_rects.size() != m_drawn_blur.size()) {
            changed = true;
        } else {
            for (int i = 0; i < m_blur_rects.size(); ++i) {
                const QRectF &a = m_blur_rects[i].rect;
                const QRectF &b = m_drawn_blur[i].rect;
                if (m_blur_rects[i].object_id != m_drawn_blur[i].object_id
                    || qAbs(a.x() - b.x()) > EPS || qAbs(a.y() - b.y()) > EPS
                    || qAbs(a.width() - b.width()) > EPS
                    || qAbs(a.height() - b.height()) > EPS) {
                    changed = true;
                    break;
                }
            }
        }
    }

    // 상태 문구(재접속 등)나 선택 대상이 바뀌어도 다시 그려야 한다
    const QString status = status_text();
    if (status != m_drawn_status || m_selected != m_drawn_selected)
        changed = true;

    if (changed) {
        m_drawn_boxes = m_anim_boxes;
        m_drawn_blur = m_blur_rects;
        m_drawn_status = status;
        m_drawn_selected = m_selected;
    }
    return changed;
}

void ChannelView::set_playback_delay(int ms)
{
    m_playback_delay_ms = ms;   // HUD 표기용

    // ⭐ **여기가 08-13 에 빠져 있던 배선이다.** `set_video_delay_ms` 를 만든
    //   커밋(7a0f03f)은 백엔드 3파일만 건드려서, +/- 키가 HUD 숫자만 바꾸고
    //   영상은 1ms 도 안 늦춰지고 있었다(2026-08-24 확인). 이 값이 곧 영상
    //   지연이고, 박스는 도착 즉시 그리므로 **정합을 만드는 유일한 손잡이**다.
    if (m_backend)
        m_backend->set_video_delay_ms(ms);
}

void ChannelView::set_show_all_boxes(bool on)
{
    if (m_show_all_boxes == on)
        return;
    m_show_all_boxes = on;
    // 박스 목록은 그대로고 **그리는 규칙만** 바뀐다 — boxes_changed() 는
    // 좌표·선택만 보므로 여기서 직접 다시 그린다 (refresh_alert 와 같은 규칙).
    update_overlay();
}

void ChannelView::set_face_blur(bool on)
{
    if (m_face_blur == on)
        return;
    m_face_blur = on;
    // 백엔드 모자이크는 다음 애니메이션 틱(≤30ms)에 갱신된다
    update_overlay();
}

bool ChannelView::pixel_mosaic() const
{
    return m_backend && m_backend->supports_pixel_mosaic();
}

QString ChannelView::status_text() const
{
    return m_backend ? m_backend->status_text() : QString();
}

qint64 ChannelView::glass_latency_ms() const
{
    return m_backend ? m_backend->glass_latency_ms() : -1;
}

void ChannelView::update_video_visibility()
{
    // appsink/ffmpeg 경로의 영상 위젯은 평범한 Qt 위젯이라 스스로 칠한다 —
    // 이 처리가 필요한 건 네이티브 창을 쓰는 direct 경로뿐이다.
    if (!m_gpu_overlay || !m_video || !m_backend)
        return;
    const bool live = m_backend->has_frame();
    if (m_video->isVisible() == live)
        return;
    m_video->setVisible(live);
    if (!live)
        update();   // 창이 비켰으니 Qt가 배경 + 크롬을 그린다
}

void ChannelView::showEvent(QShowEvent *ev)
{
    QWidget::showEvent(ev);
    // 탭 복귀. 영상이 흐르는 중이면 마지막 프레임을 다시 제시하게 한다 —
    // 안 그러면 다음 버퍼가 올 때까지(끊겨 있으면 영영) 낡은 픽셀이 남는다.
    if (m_backend)
        m_backend->expose();
    update_video_visibility();
}

void ChannelView::paintEvent(QPaintEvent *ev)
{
    QWidget::paintEvent(ev);   // autoFillBackground 배경
    // 영상이 안 흐르는 동안의 크롬은 여기서 그린다. sink 합성 경로는 버퍼에
    // 얹혀 나가므로 프레임이 없으면 상태 문구까지 통째로 안 보인다 —
    // "재접속 중인지 화면에 반드시 알린다"(VideoBackend::status_text)가
    // direct 경로에서 지켜지지 않던 구멍이다.
    if (!m_gpu_overlay)
        return;                       // appsink 경로는 BoxOverlay가 담당
    if (m_video && m_video->isVisible())
        return;                       // 영상이 그리는 중 — 겹쳐 그리지 않는다
    QPainter p(this);
    paint_chrome(p, size());
}

void ChannelView::update_overlay()
{
    if (m_gpu_overlay) {
        // 영상이 없으면 합성 경로가 죽어 있다 — Qt paintEvent 로 돌린다
        if (m_video && !m_video->isVisible()) {
            update();
            return;
        }
        // 캔버스는 영상 프레임 비율로 — sink가 프레임 픽셀 공간에 합성한 뒤
        // 프레임과 함께 위젯 크기로 늘리므로, 뷰 좌표로 계산된 박스에
        // 뷰→캔버스 스케일만 걸면 화면에 1:1로 얹힌다.
        const QSize vid = m_backend->video_size();
        if (vid.isEmpty() || width() <= 0 || height() <= 0)
            return;
        // 4MP에서 풀해상도 캔버스는 틱당 ~16MB 페인트가 된다 — 폭 1600으로
        // 상한 (크롬은 어차피 벡터라 업스케일 열화가 크지 않다)
        QSize canvas = vid;
        if (canvas.width() > 1600)
            canvas = QSize(1600,
                           qMax(1, int(1600.0 * vid.height() / vid.width())));
        if (m_overlay_img.size() != canvas)
            m_overlay_img = QImage(canvas, QImage::Format_ARGB32_Premultiplied);
        m_overlay_img.fill(Qt::transparent);
        {
            QPainter p(&m_overlay_img);
            p.scale(double(canvas.width()) / width(),
                    double(canvas.height()) / height());
            paint_chrome(p, size());
        }
        m_backend->set_overlay_image(m_overlay_img);
        return;
    }
    if (m_overlay)
        m_overlay->update();
}

void ChannelView::refresh_alert()
{
    // 경보의 channel은 zones.channel(DB 축)이라 db_channel로 조회한다.
    // 지금은 표시 채널과 같지만(live_viewer의 db_channel_of), 갈라지면
    // 여기가 맞는 쪽이다.
    const AlertFeed::State st = AlertFeed::instance()->state(m_db_channel);

    const int sev = int(st.severity);
    QString text;
    if (sev > 0) {
        text = (sev >= 2) ? QStringLiteral("Critical") : QStringLiteral("Warn");
        if (st.count >= 0 && st.capacity > 0)
            text += QString(" %1/%2").arg(st.count).arg(st.capacity);
        if (st.predicted)
            text += QString(" Forecast");
    }

    if (sev == m_alert_severity && text == m_alert_text)
        return;
    m_alert_severity = sev;
    m_alert_text = text;
    update_overlay();
}

void ChannelView::play_stream(const QUrl &url)
{
    m_backend->play(url);
}

void ChannelView::stop_stream()
{
    m_backend->stop();
}

void ChannelView::resizeEvent(QResizeEvent *ev)
{
    QWidget::resizeEvent(ev);
    // 영상과 오버레이가 뷰 전체를 정확히 덮어야 박스 변환이 단순 비례가 된다
    if (m_video)
        m_video->resize(size());
    if (m_overlay) {
        m_overlay->resize(size());
        m_overlay->raise();
    }
    // direct 경로는 캔버스 스케일이 뷰 크기에 걸려 있어 즉시 재합성해야
    // 박스·배지가 새 크기에 맞는다
    if (m_gpu_overlay)
        update_overlay();
}

void ChannelView::on_boxes_updated(const QVector<DetectionBox> &boxes,
                                   qint64 frame_utc_ms)
{
    // 웹 UI metaManager.setOverlayData의 등가물 (2026-08-05 동형 전환).
    // 수신은 쌓기만 하고, 어떤 프레임을 그릴지는 그리기 틱이 영상 시각으로
    // 고른다 — 웹 UI도 수신(파서)과 선택(getOverlayData)이 분리돼 있다.
    for (const DetectionBox &box : boxes)
        if (box.category == 1)
            TrackHistory::instance()->add(m_channel, box);

    if (frame_utc_ms > 0) {
        // ONVIF 메타 프레임 — 프레임 귀속이 있는 박스 묶음
        m_overlay_list.append(
            { frame_utc_ms, QDateTime::currentMSecsSinceEpoch(), boxes });
        while (m_overlay_list.size() > OVERLAY_LIST_MAX)
            m_overlay_list.removeFirst();
    } else {
        // HTTP 스냅샷 (프레임 귀속 없음) — ONVIF가 죽었을 때의 폴백 공급원.
        // DetectionFeed가 ONVIF 신선 채널에선 발행을 멈추므로 평시엔 안 온다.
        m_fallback_boxes = boxes;
    }
}

QRectF ChannelView::to_view_rect(const DetectionBox &box) const
{
    const double rx = double(width())  / FRAME_W;
    const double ry = double(height()) / FRAME_H;

    return QRectF(box.sx * rx,
                  box.sy * ry,
                  (box.ex - box.sx) * rx,
                  (box.ey - box.sy) * ry);
}

/**
 * @brief pos 아래의 사람(Human) 박스 — 없으면 nullptr
 *
 * 겹친 박스 중 가장 작은 것 = 사용자가 노린 대상일 확률이 높다.
 * Face/Head(v15)는 추적 대상이 아니므로 제외 — 안 그러면 얼굴 박스가
 * 항상 더 작아서 사람 선택을 가로챈다.
 */
const AnimatedBox *ChannelView::human_box_at(const QPointF &pos) const
{
    const AnimatedBox *hit = nullptr;
    for (const AnimatedBox &box : m_anim_boxes) {
        if (box.category != 1)
            continue;
        if (!box.current.contains(pos))
            continue;
        if (!hit || box.current.width() * box.current.height()
                        < hit->current.width() * hit->current.height())
            hit = &box;
    }
    return hit;
}

void ChannelView::mousePressEvent(QMouseEvent *ev)
{
    // 우클릭: 추적 토글. 좌클릭은 아무 일도 하지 않는다 — 타일 클릭 전체화면
    // 확대는 08-19에 삭제됐다 (화면은 항상 2×2 그리드).
    if (ev->button() == Qt::RightButton) {
        if (const AnimatedBox *hit = human_box_at(ev->position())) {
            // 화면 좌표 -> 카메라 원본 좌표 역변환 (DB rect와 동일 좌표계)
            const double rx = double(FRAME_W) / qMax(1, width());
            const double ry = double(FRAME_H) / qMax(1, height());
            const QRectF cam_rect(hit->current.x() * rx,
                                  hit->current.y() * ry,
                                  hit->current.width() * rx,
                                  hit->current.height() * ry);
            // 우클릭마다 더하고/빼는 토글 — Ctrl 조합 없이 여러 명을 쌓는다
            emit object_selected(m_db_channel, hit->object_id, cam_rect);
        }
        return;
    }

    // 좌클릭: 타일 드래그(배치 이동)의 시작 후보. 여기서 accept 해 둬야 Qt가
    // 이 위젯을 그랩 대상으로 삼아 이후 move/release를 이리로 보낸다 —
    // 임계 거리를 넘기 전에는 아무 일도 하지 않는다 (좌클릭 단독 기능은
    // 08-19에 전체화면 확대와 함께 삭제돼 비어 있다).
    if (ev->button() == Qt::LeftButton) {
        m_press_pos = ev->position().toPoint();
        ev->accept();
        return;
    }

    QWidget::mousePressEvent(ev);
}

void ChannelView::mouseReleaseEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton && m_press_pos.x() >= 0) {
        const bool was_drag = m_dragging;
        m_press_pos = QPoint(-1, -1);
        m_dragging = false;
        if (was_drag) {
            emit tile_drag_finished(m_channel, ev->globalPosition().toPoint());
            return;
        }
    }
    QWidget::mouseReleaseEvent(ev);
}

void ChannelView::mouseMoveEvent(QMouseEvent *ev)
{
    // 좌클릭 드래그 — 임계 거리를 넘으면 배치 이동 모드 (08-20).
    // ⚠ QDrag(OLE)를 쓰지 않는 이유: direct 경로의 sink는 자기 **자식 HWND**에
    //   그리는데 그 창은 OLE 드롭 타깃으로 등록돼 있지 않아 영상 위 드롭이
    //   막힐 수 있다. 버튼을 누른 동안은 Qt가 마우스를 캡처하므로 move는
    //   여기로 온다 — 아래 hover 힌트의 소실 조건(캡처 없는 이동)과 다르다.
    if ((ev->buttons() & Qt::LeftButton) && m_press_pos.x() >= 0) {
        if (!m_dragging
            && (ev->position().toPoint() - m_press_pos).manhattanLength()
                   >= QApplication::startDragDistance())
            m_dragging = true;
        if (m_dragging) {
            emit tile_drag_moved(m_channel, ev->globalPosition().toPoint());
            return;
        }
    }

    // "여기는 우클릭이 먹힌다"의 유일한 단서 — 박스가 기본 표시되지 않으므로
    // (추적 대상만 그린다) 사람 위에서 커서 모양으로 알려 준다.
    // direct 경로(네이티브 sink 창)에서는 move가 안 올 수 있다 — 그 경우
    // 커서 힌트만 빠지고 우클릭 동작 자체는 그대로다.
    setCursor(human_box_at(ev->position()) ? Qt::PointingHandCursor
                                           : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(ev);
}

void ChannelView::set_drop_hint(bool on)
{
    if (m_drop_hint == on)
        return;
    m_drop_hint = on;
    update_overlay();
}
