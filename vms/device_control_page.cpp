#include "device_control_page.h"
#include "auth.h"
#include "broadcast_control_row.h"
#include "fire_alert_feed.h"
#include "fire_charts.h"
#include "fire_zone_map.h"
#include "mqtt_link.h"
#include "panel_chrome.h"
#include "sensor_fields.h"
#include "theme.h"
#include "zone_sensor_store.h"

#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {

const QString SETACT_TOPIC   = "guardx/db/rpib/cmd/set_actuator";
// RPi C -> VMS 팬 상태(retained). 규약은 shared/fan_protocol.h.
const QString FANSTATE_TOPIC = "guardx/state/rpic/fan";
const QString CLEARFIRE_TOPIC = "guardx/cmd/rpib/clear_fire";
// RPi C가 직접 발행하는 edge 토픽 — VMS→B→C 설계 확정은 "명령이 나가는
// 방향"에만 적용된다. 텔레메트리/ACK는 fire_alert_feed.cpp의 guardx/alert/*와
// 같은 방식으로 VMS가 브로커에서 바로 받는다. "rpic" 고정은 set_actuator와
// 같은 이유(ACTUATOR_RPIC_NODE_ID) — zone이 하나뿐인 지금 단계의 임시값이다.
const QString ACTUATOR_ACK_TOPIC = "guardx/actuator/rpic/ack";
// LWT 생존 신호 — 같은 이유로 rpic 고정, 같은 이유로 VMS가 직접 구독한다.
const QString RPIC_STATUS_TOPIC = "guardx/status/rpic";
const int QOS = 1;


/** @brief 채널 하나의 안전/위험 기준값. has_threshold=false면 fallback 범위 */
struct Scale {
    double safe = 0, danger = 1;
    bool has_threshold = false;
};

/** @brief fire_threshold에서 이 채널의 safe/danger를 뽑는다. 없으면 fallback */
Scale resolve_scale(const SensorField &f, const QJsonObject &threshold)
{
    Scale s;
    if (f.thr_safe_key && f.thr_danger_key
        && threshold.contains(QLatin1String(f.thr_safe_key))
        && threshold.contains(QLatin1String(f.thr_danger_key))) {
        s.safe   = threshold.value(QLatin1String(f.thr_safe_key)).toDouble();
        s.danger = threshold.value(QLatin1String(f.thr_danger_key)).toDouble();
        s.has_threshold = !qFuzzyCompare(s.safe, s.danger);
    }
    if (!s.has_threshold) {
        s.safe = f.fallback_min;
        s.danger = f.fallback_max;
    }
    return s;
}

/**
 * @brief safe~danger 구간 기준 위험도 (부호 있는 원값, 클램프 안 함)
 *
 * <= 0 : safe 쪽(그 이상 안전) · >= 1 : danger 쪽(그 이상 위험) · 사이 : 주의.
 * safe/danger의 대소 관계가 채널마다 달라도(spark_raw는 safe > danger)
 * 이 식이 방향을 알아서 흡수한다 — value가 safe와 같으면 항상 0, danger와
 * 같으면 항상 1이 나온다.
 */
double risk_raw(double value, const Scale &s)
{
    const double span = s.danger - s.safe;
    if (qFuzzyIsNull(span))
        return 0.0;
    return (value - s.safe) / span;
}

/** @brief threshold 없는 채널(irtemp_ambient)은 예전 방식(파랑/회색) 그대로 */
/**
 * @brief 위험도 → 색 **슬롯 포인터**
 *
 * 값(QColor)이 아니라 팔레트 슬롯을 돌려준다. 이 색은 GaugeBar·MiniLineChart가
 * 멤버로 들고 있다가 paintEvent에서 쓰는데, 값을 복사해 두면 테마가 바뀌어도
 * 그 순간의 색이 굳는다 — 특히 "수신 없음/무응답"으로 갱신 경로를 건너뛴
 * 타일은 영영 옛 팔레트 색을 그린다. 포인터면 그릴 때마다 현재 팔레트를 읽는다.
 */
const QColor *risk_color(double raw, bool has_threshold, bool valid)
{
    if (!valid) return &Theme::textFaint;
    // 08-19: 안전 = 초록 (디자인 보드의 스파크라인과 동일 — 파랑은 정보색으로
    // 남기고, 센서 선색은 건강 신호등을 그대로 쓴다)
    if (!has_threshold) return &Theme::green;
    if (raw <= 0.0) return &Theme::green;    // 초록 — 안전
    if (raw >= 1.0) return &Theme::alarm;    // 빨강 — 위험
    return &Theme::amber;                    // 노랑 — 주의
}

/**
 * @brief COMPARE에서 구역을 구분하는 선 색
 *
 * 위험도 색(파랑/노랑/빨강)과 겹치면 안 된다 — 비교 화면에서는 색이
 * "어느 구역인가"를 뜻하지 "얼마나 위험한가"를 뜻하지 않기 때문이다.
 * 그래서 REPORT의 채널 색과 같은 계열(청/녹/보라/호박)을 쓰되, 위험도
 * 팔레트와 헷갈리지 않게 구역 라벨(Z1…)을 항상 함께 붙인다.
 *
 * 이 네 색은 **테마를 따르지 않는다** — "몇 번 구역인가"를 뜻하는 범주색이라
 * 다크/라이트에서 같은 색이어야 선과 칩이 계속 짝지어 읽힌다
 * (Theme::track_color·WeightPie::slice_color와 같은 성격의 고정 팔레트).
 */
QColor zone_color(int zone_id)
{
    switch (zone_id % 4) {
    case 1:  return QColor(0x5B, 0x9C, 0xF6);   // 청
    case 2:  return QColor(0x3E, 0xCF, 0x8E);   // 녹
    case 3:  return QColor(0xB5, 0x8C, 0xF6);   // 보라
    default: return QColor(0xE8, 0xA3, 0x3D);   // 호박
    }
}

/**
 * @brief 액추에이터 명령 카탈로그 — fire_schema.sql actuator_command 시드와 1:1
 *
 * VMS에 박아둔 고정 목록이다 — 카탈로그 조회 토픽까지 만들 정도로 자주
 * 바뀌지 않는다. **DB(actuator_command.command_key)와 정확히 같은 6개여야
 * 한다** — RPi B `handleSetActuator`가 이 카탈로그로 command_key를
 * 검증하므로, 여기 없는 key를 보내면 "command_key 없음"으로 거부당한다.
 *
 * kind: "onoff"(ON/OFF만) · "set"(value만) · "both"(토글=ON/OFF,
 *       스핀박스=SET) · "shutter"(OPEN/CLOSE/STOP 3버튼, value 없음).
 */
struct ActuatorField {
    const char *key;
    const char *label;
    const char *kind;
    double set_min;
    double set_max;
    double set_step;
    const char *suffix;
};

const ActuatorField ACTUATOR_FIELDS[] = {
    { "water_pump",  "Water pump",     "onoff",   0, 0,   0, ""    },
    { "sound",       "Alarm horn (AMP)", "onoff", 0, 0,   0, ""    },
    { "fan",         "Ventilation fan","both",    0, 100, 5, " %"  },
    { "servo_1",     "Servo (valve)",  "set",     0, 180, 1, " °"  },
    { "shutter",     "Fire shutter",   "shutter", 0, 0,   0, ""    },
    // led_matrix 는 뺐다 — STM32 브릿지가 구현되지 않아 ON/OFF 를 눌러도
    // RPi C 가 무시만 했다. 눌리는데 아무 일도 안 나는 버튼은 "명령이
    // 씹혔다"로 읽혀서 다른 액추에이터까지 못 믿게 만든다.
    //
    // ⚠ LED 매트릭스 **표시**(온습도·화재·트래킹)와는 무관하다. 그쪽은
    //   guardx/display/rpic/... 로 RPi B 가 직접 쏘는 별도 경로다.
};

/** @brief command_key로 ACTUATOR_FIELDS 행을 찾는다 — ACK의 SET 값에 단위를 붙일 때 씀 */
const ActuatorField *find_actuator_field(const QString &key)
{
    for (const ActuatorField &f : ACTUATOR_FIELDS)
        if (key == QLatin1String(f.key))
            return &f;
    return nullptr;
}

/**
 * @brief 0..1 값 하나를 채우는 얇은 바 — live_viewer.cpp OccBar와 동일한 그리기 방식
 */
class GaugeBar : public QWidget
{
public:
    explicit GaugeBar(QWidget *parent) : QWidget(parent)
    {
        setFixedHeight(8);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    /** @param slot 팔레트 **슬롯 포인터** (risk_color 참조) — 값 복사 금지 */
    void set_pct(double pct, const QColor *slot)
    {
        m_pct = qBound(0.0, pct, 1.0);
        m_color = slot;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::elevated);
        p.drawRoundedRect(rect(), 4, 4);
        if (m_pct > 0) {
            p.setBrush(m_color ? *m_color : Theme::accent);
            p.drawRoundedRect(QRectF(0, 0, width() * m_pct, height()), 4, 4);
        }
    }

private:
    double m_pct = 0;
    const QColor *m_color = &Theme::accent;   ///< 슬롯 포인터 (그릴 때 읽는다)
};

/**
 * @brief 센서 이력 꺾은선 — 단일 계열(ZONE) / 다계열 오버레이(COMPARE) 겸용
 *
 * REPORT의 ReportChart를 안 쓰는 이유: 그쪽은 X축이 이력 62% / 예측 38%로
 * 갈리고 예측 밴드·CAP 마커가 구조의 핵심인데, 센서 데이터엔 예측도 정원도
 * 없다. 그 요소를 전부 끄는 플래그를 붙이느니 이쪽을 다계열로 넓히는 편이
 * 단순하다(색 팔레트도 ReportTk에 의도적으로 가둬져 있다).
 */
class MiniLineChart : public QWidget
{
public:
    struct Series {
        /// COMPARE의 구역 범주색처럼 **테마를 타지 않는** 고정색만 값으로 둔다
        QColor color;
        /// 팔레트에서 온 색이면 슬롯 포인터를 쓴다 — 값으로 복사하면 테마가
        /// 바뀌어도 옛 색이 굳는다 (risk_color 주석 참조). 널이면 color 사용.
        const QColor *slot = nullptr;
        QVector<QPointF> pts;   ///< 시간순(오래된→최신). x=epoch ms, y=값
    };

    explicit MiniLineChart(QWidget *parent) : QWidget(parent)
    {
        setMinimumHeight(70);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    /** @param axis_decimals >=0이면 그래프 상/하단에 축 값(m_hi/m_lo)을 그 소수
     *         자리수로 적어 넣는다. 음수(기본값)면 안 그린다 — ZONE 모드 6타일은
     *         숫자가 이미 위 value_label에 크게 있어 축 라벨까지 더하면 과함. */
    void set_range(double lo, double hi, int axis_decimals = -1)
    {
        m_lo = lo;
        m_hi = hi;
        m_axis_decimals = axis_decimals;
    }
    /** @brief true면 축을 뒤집는다 — SensorField::graph_reversed 참조 */
    void set_reversed(bool r) { m_reversed = r; }

    /** @brief 단일 계열 (ZONE 모드) — 색은 팔레트 슬롯 포인터로 받는다 */
    void set_points(const QVector<QPointF> &pts, const QColor *slot, qint64 window_ms)
    {
        m_series.clear();
        m_series.append({ QColor(), slot, pts });
        m_window_ms = window_ms;
        update();
    }

    /** @brief 다계열 오버레이 (COMPARE 모드) */
    void set_series(const QVector<Series> &list, qint64 window_ms)
    {
        m_series = list;
        m_window_ms = window_ms;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // 배경 상자를 그리지 않는다 (08-19 보드와 동일 — 스파크라인은 카드
        // 면 위에 바로 얹힌다)

        if (m_window_ms <= 0)
            return;

        // 시간 기준(t_end)은 계열마다 따로 잡지 않는다 — 계열별로 잡으면
        // 마지막 수신 시각이 다른 구역들이 서로 다른 x축을 갖게 돼 비교가
        // 무의미해진다. 전 계열의 최신 시각 하나로 통일한다.
        qint64 t_end = 0;
        for (const Series &s : m_series)
            if (!s.pts.isEmpty())
                t_end = qMax(t_end, qint64(s.pts.last().x()));
        if (t_end == 0)
            return;

        const qint64 t_cut = t_end - m_window_ms;
        const double range = qMax(1e-6, m_hi - m_lo);

        // x축은 **가진 데이터를 카드 폭에 꽉 채운다** (08-19). 보드의
        // 스파크라인(polyline "0,34 … 220,26")은 언제나 카드 폭 전체를
        // 지나는데, 창 전체(10분)를 x에 그대로 매핑하면 기동 직후엔 오른쪽
        // 끝 몇 %만 그려져 "구석의 짧은 실"로 보였다(사용자 스크린샷).
        // t_start/span 은 **전 계열 공통**이라 COMPARE 겹쳐보기의 비교
        // 가능성은 그대로다(계열마다 축이 달라지지 않는다).
        qint64 t_start = t_end;
        for (const Series &s : m_series)
            for (const QPointF &pt : s.pts)
                if (qint64(pt.x()) >= t_cut) {
                    t_start = qMin(t_start, qint64(pt.x()));
                    break;   // 계열별 첫 유효점만 보면 된다 (시간순 정렬)
                }
        const double span = double(qMax<qint64>(1, t_end - t_start));

        // 굵기: 보드는 viewBox 220×46 을 preserveAspectRatio="none" 으로
        // 카드에 늘린다 — stroke-width 1.5 가 세로 배율(svg높이/46)만큼
        // 같이 늘어나 실제로는 "카드 높이의 약 3.3%짜리 굵은 띠"다
        // (1600×900 보드 실측 ≒ 10px). 그래서 상수를 박지 않고 높이에
        // 비례시킨다. ⚠ COMPARE(다계열)는 예외 — 구역 5개를 10px로 겹치면
        // 서로를 덮어 아무것도 안 보인다.
        const double pen_w = m_series.size() > 1
            ? 2.5
            : qBound(3.0, height() * (1.5 / 46.0), 12.0);

        for (const Series &s : m_series) {
            if (s.pts.size() < 2)
                continue;
            QPainterPath path;
            bool started = false;
            for (const QPointF &pt : s.pts) {
                if (qint64(pt.x()) < t_cut)
                    continue;
                const double x = width() * double(qint64(pt.x()) - t_start) / span;
                const double yv = qBound(m_lo, pt.y(), m_hi);
                const double frac = (yv - m_lo) / range;
                // 굵은 선이 축 끝에 붙으면 절반이 잘려 나가므로 위아래로
                // 펜 굵기의 절반씩 물려 둔다
                const double half = pen_w / 2.0;
                const double h_eff = qMax(1.0, height() - pen_w);
                const double y = half + (m_reversed ? frac : 1.0 - frac) * h_eff;
                if (!started) { path.moveTo(x, y); started = true; }
                else path.lineTo(x, y);
            }
            p.setPen(QPen(s.slot ? *s.slot : s.color, pen_w,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }

        // 축 상/하단 값 — COMPARE는 min/max로 range를 잡아 축이 매번
        // 바뀌므로, 지금 이 그래프의 위/아래가 정확히 얼마인지 숫자로
        // 박아 둬야 "선이 좁아졌다"가 아니라 "실제로 이만큼 흔들렸다"로
        // 읽힌다. 선 위에 그리므로 마지막에 그린다.
        if (m_axis_decimals >= 0) {
            QFont fnt = p.font();
            fnt.setPixelSize(9);
            p.setFont(fnt);
            p.setPen(Theme::textFaint);
            p.drawText(QRectF(4, 2, width() - 8, 12),
                       Qt::AlignLeft | Qt::AlignTop,
                       QString::number(m_hi, 'f', m_axis_decimals));
            p.drawText(QRectF(4, height() - 14, width() - 8, 12),
                       Qt::AlignLeft | Qt::AlignBottom,
                       QString::number(m_lo, 'f', m_axis_decimals));
        }
    }

private:
    QVector<Series> m_series;
    qint64 m_window_ms = 0;
    double m_lo = 0, m_hi = 1;
    bool m_reversed = false;
    int m_axis_decimals = -1;
};

QLabel *make_dot(QWidget *parent)
{
    auto *dot = new QLabel(parent);
    dot->setFixedSize(8, 8);
    // 생성 시 1회지만 restyle로 묶지 않는다 — 이 회색은 곧 상태색으로 덮이므로
    // 테마 전환 때 여기가 다시 돌면 살아 있는 점이 "모름"으로 되돌아간다.
    // 점의 테마 추종은 마지막 **의미**를 아는 paint_dot이 맡는다.
    dot->setStyleSheet(QString("background:%1; border-radius:4px;")
                            .arg(Theme::textFaint.name()));
    return dot;
}

/// 1초 타이머·브로커 상태 변화로 **반복 호출**된다 — restyle을 쓰면 notifier
/// 연결이 호출마다 쌓인다. 평범한 setStyleSheet으로 두고 테마 전환은 호출부
/// (DeviceControlPage::paint_dot)가 다시 부르는 것으로 해결한다.
void set_dot_color(QLabel *dot, const QColor &c)
{
    dot->setStyleSheet(QString("background:%1; border-radius:4px;").arg(c.name()));
}

} // namespace

DeviceControlPage::DeviceControlPage(QWidget *parent) : QWidget(parent)
{
    // 첫 선택 구역 = 실 하드웨어가 있는 첫 구역. 더미부터 보여주면 "값이
    // 도는데 왜 가짜냐"는 혼동이 생긴다.
    int n = 0;
    const FireZoneInfo *zones = fire_zone_table(&n);
    for (int i = 0; i < n; ++i) {
        m_sel.insert(zones[i].zone_id, true);   // COMPARE 기본값: 전 구역 선택
        if (!zones[i].dummy && m_zone_id == 1)
            m_zone_id = zones[i].zone_id;
    }

    build_ui();

    ZoneSensorStore *store = ZoneSensorStore::instance();
    connect(store, &ZoneSensorStore::updated,
            this, &DeviceControlPage::on_zone_updated);
    connect(store, &ZoneSensorStore::threshold_changed, this, [this] {
        refresh_single();
        refresh_compare();
    });

    connect(MqttLink::instance(), &MqttLink::online_changed,
            this, &DeviceControlPage::on_mqtt_online);
    on_mqtt_online(MqttLink::instance()->online());

    m_stale_timer = new QTimer(this);
    connect(m_stale_timer, &QTimer::timeout, this, &DeviceControlPage::check_staleness);
    m_stale_timer->start(1000);

    // 화재 여부는 FireAlertFeed가 이미 구독·정리해 두고 있다 — 이 화면이
    // 토픽을 또 구독하지 않고 그 상태만 빌려 쓴다.
    connect(FireAlertFeed::instance(), &FireAlertFeed::state_changed,
            this, &DeviceControlPage::on_fire_state_changed);
    // 권한이 바뀌면 잠금도 다시 계산한다. 잠금 계산이 이 함수 하나에 모여
    // 있으므로(화재 조건과 AND) 여기에 걸면 두 이유가 어긋나지 않는다.
    connect(Auth::instance(), &Auth::state_changed,
            this, [this] { on_fire_state_changed(); });
    connect(Auth::instance(), &Auth::verification_changed,
            this, [this] { on_fire_state_changed(); });
    on_fire_state_changed();

    // 팬 상태(retained) — 구독하는 순간 현재 AUTO 여부가 온다.
    MqttLink::instance()->subscribe(FANSTATE_TOPIC,
                                    [this](const QByteArray &p) { on_fan_state(p); });

    // RPi C 액추에이터 ACK — state_label을 "—"에서 실제 값으로 채운다
    // (RPIC_ENABLE_ACK, guardx_protocol.h 4-4).
    MqttLink::instance()->subscribe(
        ACTUATOR_ACK_TOPIC,
        [this](const QByteArray &p) { on_actuator_ack(p); },
        QOS);

    MqttLink::instance()->subscribe(
        RPIC_STATUS_TOPIC,
        [this](const QByteArray &p) { on_rpic_status(p); },
        QOS);

    refresh_single();
    refresh_compare();
    set_status("waiting for sensor data...");

    // 테마 전환 — 스타일시트나 QColor에 색이 굳는 자리를 "지금 화면 상태
    // 그대로" 다시 칠한다. 한 번만 등록한다(등록된 함수들 안에서 restyle을
    // 쓰지 않는 이유이기도 하다 — 그쪽은 매 호출마다 연결이 쌓인다).
    //
    // 여기 매다는 것은 전부 읽기 전용이다. send_* 처럼 MQTT가 나가는 함수를
    // 걸면 사용자가 테마를 바꿀 때마다 장비에 명령이 나간다.
    Theme::on_theme_changed(this, [this] {
        // ⚠ refresh_*는 끝에서 set_status로 **문구까지** 새로 만든다. 테마를
        //   바꿨을 뿐인데 상태줄 문장이 갈리면 안 되므로 지금 문구를 잡아
        //   두었다가 되돌린다 (색만 새 팔레트로 다시 칠해진다).
        const QString keep_status = m_status->text();

        // 값·게이지·그래프 색은 현재 데이터로 다시 계산해야 나온다. 지금 보고
        // 있는 모드만 갱신한다(on_zone_updated와 같은 규칙) — 반대편 뷰는
        // COMPARE 토글이 전환하면서 어차피 다시 그린다.
        if (m_compare)
            refresh_compare();
        else
            refresh_single();
        on_fire_state_changed();   // 화재 배너 글자색(alarm/green)
        paint_dot(m_dot_a, m_node_a);
        paint_dot(m_dot_b, m_node_b);
        paint_dot(m_dot_c, m_node_c);
        // 액추에이터 상태줄은 문구가 명령 결과라 다시 만들 수 없다. 색만
        // 마지막 성격(정상/실패)으로 되칠한다 — 문구는 라벨이 이미 들고 있다.
        set_actuator_status(m_actuator_status->text(), m_act_error);

        set_status(keep_status, m_status_error);   // 문구 원복 + 색 재적용
    });
}

void DeviceControlPage::show_zone(int zone_id)
{
    if (!m_zone_btns.contains(zone_id))
        return;

    // 특정 구역을 보러 온 것이므로 COMPARE는 끈다 — 경보를 보고 넘어온
    // 사람에게 여러 구역이 겹친 그래프를 보여주면 목적과 어긋난다.
    if (m_compare)
        m_btn_compare->setChecked(false);

    m_zone_btns[zone_id]->setChecked(true);   // toggled 시그널이 나머지를 처리
}

// ------------------------------------------------------------------ UI

void DeviceControlPage::build_ui()
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(10);

    lay->addWidget(build_header());
    lay->addWidget(build_node_strip());

    // 노드 상태 스트립과 본문 사이 — 맨 아래 두면 화면 아래로 밀려나
    // 액추에이터 명령 실패 같은 즉각 확인이 필요한 메시지가 잘 안 보였다.
    m_status = new QLabel(this);
    m_status->setFont(Theme::mono_font(10));
    lay->addWidget(m_status);

    // 화재 배너 — 스택 **바깥**이라 ZONE/COMPARE 어느 모드에서도 보인다.
    // 예전에는 액추에이터 패널 안에 있었는데, COMPARE가 그 패널이 든 뷰를
    // 통째로 내리는 바람에 정작 불이 났을 때 안 보였다.
    m_fire_state = new QLabel(this);
    m_fire_state->setFont(Theme::mono_font(11, QFont::DemiBold));
    m_fire_state->setWordWrap(true);
    lay->addWidget(m_fire_state);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(build_single_view());    // 0
    m_stack->addWidget(build_compare_view());   // 1
    lay->addWidget(m_stack, 1);
}

QWidget *DeviceControlPage::build_header()
{
    auto *host = new QWidget(this);
    auto *row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    // 화면 제목 — 워크스페이스 헤더 행 (디자인 보드와 동일 위치)
    auto *title = new QLabel("Device Control", host);
    title->setFont(Theme::ui_font(13, 700, 0.08));
    Theme::restyle(title, [] {
        return QString("color:%1;").arg(Theme::textHi.name());
    });
    row->addWidget(title);
    row->addSpacing(6);

    auto *lb = new QLabel(QString::fromUtf8("Zone"), host);
    lb->setFont(Theme::mono_font(9, 400, 1.0 / 9));
    Theme::restyle(lb, [] {
        return QString("color:%1;").arg(Theme::textFaint.name());
    });
    row->addWidget(lb);

    // ZONE 셀렉터 (단일 선택) — COMPARE가 켜지면 칩 쪽으로 역할이 넘어간다
    int n = 0;
    const FireZoneInfo *zones = fire_zone_table(&n);
    for (int i = 0; i < n; ++i) {
        const int zid = zones[i].zone_id;
        auto *b = new QPushButton(
            zones[i].dummy ? QString("Z%1 (dummy)").arg(zid)
                           : QString::fromUtf8("Z%1").arg(zid),
            host);
        b->setObjectName("SegBtn");
        b->setFont(Theme::mono_font(11));
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setChecked(zid == m_zone_id);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::toggled, this, [this, zid](bool on) {
            if (!on)
                return;
            m_zone_id = zid;
            refresh_single();
            on_fire_state_changed();   // 해제 버튼의 대상 구역이 바뀌었다
        });
        m_zone_btns.insert(zid, b);
        row->addWidget(b);
    }

    row->addStretch(1);

    m_btn_compare = new QPushButton("Compare: Off", host);
    m_btn_compare->setObjectName("OutlineBtn");
    m_btn_compare->setFont(Theme::mono_font(11));
    m_btn_compare->setFixedHeight(28);
    m_btn_compare->setCheckable(true);
    m_btn_compare->setCursor(Qt::PointingHandCursor);
    connect(m_btn_compare, &QPushButton::toggled, this, [this](bool on) {
        m_compare = on;
        m_btn_compare->setText(on ? "Compare: On" : "Compare: Off");
        m_stack->setCurrentIndex(on ? 1 : 0);
        // ZONE 셀렉터는 COMPARE에서 의미가 없다 — 칩이 그 역할을 대신한다
        for (QPushButton *b : m_zone_btns)
            b->setEnabled(!on);
        if (on) refresh_compare();
        else    refresh_single();
    });
    row->addWidget(m_btn_compare);

    return host;
}

QWidget *DeviceControlPage::build_node_strip()
{
    struct NodeInfo { const char *name; const char *role; QLabel **dot; };
    auto *host = new QWidget(this);
    auto *lay = new QHBoxLayout(host);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);
    host->setFixedHeight(70);

    const NodeInfo nodes[] = {
        { "RPi-A", "environment sensors · gpio chrdev", &m_dot_a },
        { "RPi-B", "MQTT broker · decision logic", &m_dot_b },
        { "RPi-C / STM32", "actuators · uart/spi · LWT liveness", &m_dot_c },
    };

    for (const NodeInfo &n : nodes) {
        auto *card = new QWidget(host);
        card->setObjectName("Panel");
        card->setAttribute(Qt::WA_StyledBackground);
        auto *card_lay = new QVBoxLayout(card);
        card_lay->setContentsMargins(12, 10, 12, 10);

        auto *row = new QHBoxLayout();
        *n.dot = make_dot(card);
        row->addWidget(*n.dot);
        auto *name = new QLabel(QString::fromUtf8(n.name), card);
        name->setFont(Theme::mono_font(12, QFont::DemiBold));
        Theme::restyle(name, [] {
            return QString("color:%1;").arg(Theme::textHi.name());
        });
        row->addWidget(name);
        row->addStretch(1);
        card_lay->addLayout(row);

        auto *role = new QLabel(QString::fromUtf8(n.role), card);
        role->setFont(Theme::ui_font(10));
        Theme::restyle(role, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        role->setWordWrap(true);
        card_lay->addWidget(role);

        lay->addWidget(card, 1);
    }
    return host;
}

QWidget *DeviceControlPage::build_single_view()
{
    auto *host = new QWidget(this);
    auto *lay = new QHBoxLayout(host);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);
    lay->addWidget(build_sensor_panel(), 1);
    m_actuator_panel = build_actuator_panel();
    // 보드의 우측 고정 열. 디자인은 400px 이지만 방송 행(레벨 바+음량
    // 슬라이더+버튼)의 최소 폭이 그보다 넓어 창을 밀어낸다(티커에서 겪은
    // 병리) — 내용이 들어가는 최소선인 520px 로 고정한다.
    m_actuator_panel->setFixedWidth(520);
    lay->addWidget(m_actuator_panel);
    return host;
}

QWidget *DeviceControlPage::build_sensor_panel()
{
    // 08-19 워크스페이스 (디자인 "Device Control" 보드): 패널 하나에 담지
    // 않고 센서마다 **카드**다 — 3열 그리드, 종합 위험도(화재 점수)는
    // 마지막 카드. 위젯 포인터(m_sensors·m_risk)와 갱신 경로는 그대로다.
    auto *panel = new QWidget(this);
    auto *grid = new QGridLayout(panel);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(10);

    // 보드 순서 그대로: 온도 · 습도 · 가스 · 불꽃 · 표면온도 (+ 화재 점수).
    // Ambient temp 는 화면에서 뺀다 (08-19 사용자 요청 — 보드의 6칸 구성).
    // 카탈로그(SENSOR_FIELDS)에서 지우지는 않는다: COMPARE·더미 값 생성이
    // 여전히 쓴다. 표시 여부는 이 목록이 정한다.
    static const char *TILE_ORDER[] = { "temperature", "humidity", "gas_raw",
                                        "spark_raw", "irtemp_object" };
    int idx = 0;
    for (const char *tile_key : TILE_ORDER) {
        const SensorField *field_ptr =
            find_sensor_field(QLatin1String(tile_key));
        if (!field_ptr)
            continue;
        const SensorField &f = *field_ptr;
        auto *tile = new QWidget(panel);
        tile->setObjectName("Panel");
        tile->setAttribute(Qt::WA_StyledBackground);
        auto *tile_lay = new QVBoxLayout(tile);
        tile_lay->setContentsMargins(12, 10, 12, 10);
        tile_lay->setSpacing(6);

        auto *top = new QHBoxLayout();
        // 보드의 캡션 형식: "TEMPERATURE · SHT30" — 라벨과 장치명을 한 줄로
        auto *label = new QLabel(
            QString::fromUtf8("%1 · %2")
                .arg(QString::fromUtf8(f.label).toUpper(),
                     QString::fromUtf8(f.device)),
            tile);
        label->setFont(Theme::ui_font(10, QFont::DemiBold, 0.06));
        Theme::restyle(label, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        top->addWidget(label);
        top->addStretch(1);
        SensorTile &st = m_sensors[QString::fromLatin1(f.key)];
        st.state_label = new QLabel("—", tile);
        st.state_label->setFont(Theme::mono_font(9, QFont::DemiBold));
        top->addWidget(st.state_label);
        tile_lay->addLayout(top);

        // 값 + 게이지를 한 줄에 — 게이지가 전체 폭 한 줄을 안 차지하게 해서
        // 그 세로 공간을 그래프로 돌린다.
        auto *value_row = new QHBoxLayout();
        st.value_label = new QLabel("—", tile);
        st.value_label->setFont(Theme::mono_font(20, QFont::DemiBold));
        // 값(숫자)만 바뀌고 색은 고정이다 — state_label과 달리 여기는 restyle
        Theme::restyle(st.value_label, [] {
            return QString("color:%1;").arg(Theme::textHi.name());
        });
        value_row->addWidget(st.value_label);
        value_row->addStretch(1);
        // 게이지 바는 뺐다 (08-19 보드와 동일) — 위험도는 선 색과 state
        // 칩이 이미 말하고, refresh 쪽은 st.gauge 널 가드로 지나간다.
        tile_lay->addLayout(value_row);

        auto *chart = new MiniLineChart(tile);
        chart->set_range(f.fallback_min, f.fallback_max);
        chart->set_reversed(f.graph_reversed);
        st.chart = chart;
        tile_lay->addWidget(chart, 1);

        grid->addWidget(tile, idx / 3, idx % 3);
        ++idx;
    }

    // 종합 위험도 카드 — 위 센서들의 결과물(decision.c 합산)이므로 같은
    // 그리드의 마지막 칸이 제자리다 (보드의 FIRE SCORE 카드).
    {
        auto *risk_card = new QWidget(panel);
        risk_card->setObjectName("Panel");
        risk_card->setAttribute(Qt::WA_StyledBackground);
        auto *rr = new QHBoxLayout(risk_card);
        rr->setContentsMargins(12, 10, 12, 10);
        rr->setSpacing(14);

        m_risk = new RiskGauge(84, risk_card);
        rr->addWidget(m_risk, 0, Qt::AlignVCenter);

        m_risk_note = new QLabel(risk_card);
        m_risk_note->setFont(Theme::mono_font(10));
        // 문구는 refresh_single이 바꾸지만 색은 여기서 한 번 정해진다 —
        // 그래서 이 자리는 restyle로 묶어도 연결이 쌓이지 않는다.
        Theme::restyle(m_risk_note, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        m_risk_note->setWordWrap(true);
        rr->addWidget(m_risk_note, 1);

        grid->addWidget(risk_card, idx / 3, idx % 3);
    }

    return panel;
}

QWidget *DeviceControlPage::build_compare_view()
{
    auto *panel = new QWidget(this);
    panel->setObjectName("Panel");
    panel->setAttribute(Qt::WA_StyledBackground);
    auto *lay = new QVBoxLayout(panel);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(PanelChrome::header(
        "Environment Sensors · Zone Comparison",
        "actuator control lives in Zone mode", panel));

    auto *body = new QWidget(panel);
    auto *col = new QVBoxLayout(body);
    col->setContentsMargins(14, 12, 14, 14);
    col->setSpacing(12);

    // 구역 칩 (다중 선택, 최소 1개)
    auto *chips = new QHBoxLayout();
    chips->setSpacing(8);
    auto *lb = new QLabel(QString::fromUtf8("Zones"), body);
    lb->setFont(Theme::mono_font(9, 400, 1.0 / 9));
    Theme::restyle(lb, [] {
        return QString("color:%1;").arg(Theme::textFaint.name());
    });
    chips->addWidget(lb);

    int n = 0;
    const FireZoneInfo *zones = fire_zone_table(&n);
    for (int i = 0; i < n; ++i) {
        const int zid = zones[i].zone_id;
        auto *b = new QPushButton(QString::fromUtf8("Z%1").arg(zid), body);
        b->setObjectName("SegBtn");
        b->setFont(Theme::mono_font(11));
        b->setCheckable(true);
        b->setChecked(true);
        b->setCursor(Qt::PointingHandCursor);
        // 선 색과 칩 색을 맞춘다 — 범례를 따로 읽지 않아도 대응이 보인다.
        // restyle로 묶지 않는다: 여기서 굽는 것은 테마를 타지 않는 범주색
        // (zone_color) 하나뿐이고, 선택 배경 등 나머지 #SegBtn 규칙은 전역
        // QSS에 있어 테마가 바뀌면 그쪽이 통째로 다시 만들어진다.
        b->setStyleSheet(QString("#SegBtn:checked { color:%1; }")
                             .arg(zone_color(zid).name()));
        connect(b, &QPushButton::toggled, this, [this, zid](bool on) {
            if (!on) {
                // 불변식: 최소 1개 선택 — 마지막 하나는 끌 수 없다
                bool any = false;
                for (auto it = m_sel.constBegin(); it != m_sel.constEnd(); ++it)
                    any = any || (it.key() != zid && it.value());
                if (!any) {
                    QSignalBlocker block(m_chips[zid]);
                    m_chips[zid]->setChecked(true);
                    return;
                }
            }
            m_sel[zid] = on;
            refresh_compare();
        });
        m_chips.insert(zid, b);
        chips->addWidget(b);
    }
    chips->addStretch(1);

    // 구역별 위험도 게이지 — 비교 화면에서 제일 먼저 알고 싶은 것이
    // "그래서 어느 구역이 위험한가"라, 그래프를 훑기 전에 답이 보이게 한다.
    for (int i = 0; i < n; ++i) {
        const int zid = zones[i].zone_id;
        auto *g = new RiskGauge(52, body);
        g->set_caption(QString::fromUtf8("Z%1").arg(zid));
        m_risk_mini.insert(zid, g);
        chips->addWidget(g, 0, Qt::AlignVCenter);
    }

    col->addLayout(chips);

    auto *grid = new QGridLayout();
    grid->setSpacing(12);
    int idx = 0;
    for (const SensorField &f : SENSOR_FIELDS) {
        auto *tile = new QWidget(body);
        auto *tl = new QVBoxLayout(tile);
        tl->setContentsMargins(0, 0, 0, 0);
        tl->setSpacing(4);

        auto *label = new QLabel(QString::fromUtf8(f.label), tile);
        label->setFont(Theme::ui_font(10, QFont::DemiBold, 0.02));
        Theme::restyle(label, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        tl->addWidget(label);

        CompareTile &ct = m_compares[QString::fromLatin1(f.key)];
        auto *chart = new MiniLineChart(tile);
        chart->set_range(f.fallback_min, f.fallback_max);
        chart->set_reversed(f.graph_reversed);
        ct.chart = chart;
        tl->addWidget(chart, 1);

        ct.legend = new QLabel("—", tile);
        ct.legend->setFont(Theme::mono_font(9));
        // 범례 HTML은 구역색(범주색)만 span으로 굽는다 — 바탕 글자색은 여기
        // 한 번 정해지므로 restyle이 맞는 자리다.
        Theme::restyle(ct.legend, [] {
            return QString("color:%1;").arg(Theme::textDim.name());
        });
        tl->addWidget(ct.legend);

        grid->addWidget(tile, idx / 3, idx % 3);
        ++idx;
    }
    col->addLayout(grid, 1);
    lay->addWidget(body, 1);
    return panel;
}

QWidget *DeviceControlPage::build_actuator_panel()
{
    auto *panel = new QWidget(this);
    panel->setObjectName("Panel");
    panel->setAttribute(Qt::WA_StyledBackground);
    auto *lay = new QVBoxLayout(panel);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(PanelChrome::header(
        QString::fromUtf8("Actuators · RPi-C / STM32"),
        "command send only (no ACK)", panel));

    m_actuator_status = new QLabel(panel);
    m_actuator_status->setFont(Theme::mono_font(10));
    m_actuator_status->setContentsMargins(10, 6, 10, 0);
    lay->addWidget(m_actuator_status);

    // 수동 화재 해제 — PHASE 7. 개별 액추에이터 목록 위에 따로 둔다.
    // 이건 액추에이터 하나를 만지는 게 아니라 구역의 화재 상태 자체를
    // 되돌리는 상위 조작이라(펌프·팬·경보가 한꺼번에 꺼진다) 같은 줄에
    // 섞으면 무게가 안 맞는다.
    //
    // 화재 상태 배너는 여기 없다 — 페이지 레벨(build_ui)로 올라갔다.
    // 여기 남는 것은 "조작"뿐이다: 해제는 대상 구역이 명확해야 하므로
    // 구역이 하나로 정해지는 ZONE 모드에서만 노출된다.
    {
        auto *clear_row = new QWidget(panel);
        auto *btn_row = new QHBoxLayout(clear_row);
        btn_row->setContentsMargins(10, 8, 10, 8);
        btn_row->setSpacing(8);

        auto *note = new QLabel(
            "Fire state is never cleared automatically - check the site, then clear it here.",
            clear_row);
        note->setFont(Theme::ui_font(10));
        Theme::restyle(note, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        note->setWordWrap(true);
        btn_row->addWidget(note, 1);

        m_btn_clear_fire = new QPushButton("Clear fire", clear_row);
        m_btn_clear_fire->setFont(Theme::ui_font(11, QFont::DemiBold, 0.04));
        m_btn_clear_fire->setCursor(Qt::PointingHandCursor);
        // 활성/비활성 색이 QSS 안에 둘 다 들어 있어 상태가 바뀌어도 다시 굽지
        // 않는다 — 그래서 생성 시 restyle 한 번으로 테마까지 따라온다.
        Theme::restyle(m_btn_clear_fire, [] {
            return QString(
                       "QPushButton { color:%1; border:1px solid %1; border-radius:3px;"
                       " padding:5px 14px; }"
                       "QPushButton:disabled { color:%2; border-color:%2; }")
                .arg(Theme::alarm.name(), Theme::textFaint.name());
        });
        connect(m_btn_clear_fire, &QPushButton::clicked,
                this, &DeviceControlPage::send_clear_fire);
        btn_row->addWidget(m_btn_clear_fire);

        lay->addWidget(clear_row);
    }

    auto *list_host = new QWidget(panel);
    auto *list_lay = new QVBoxLayout(list_host);
    list_lay->setContentsMargins(4, 4, 4, 4);
    list_lay->setSpacing(0);

    for (const ActuatorField &f : ACTUATOR_FIELDS) {
        auto *row_w = new QWidget(list_host);
        Theme::restyle(row_w, [] {
            return QString("border-bottom:1px solid %1;")
                .arg(Theme::rowDivider.name());
        });
        auto *row = new QHBoxLayout(row_w);
        row->setContentsMargins(10, 10, 10, 10);
        row->setSpacing(10);

        ActuatorRow &ar = m_actuators[QString::fromLatin1(f.key)];
        ar.command_key = QString::fromLatin1(f.key);
        ar.kind = QString::fromLatin1(f.kind);

        auto *label = new QLabel(QString::fromUtf8(f.label), row_w);
        label->setFont(Theme::ui_font(12, QFont::DemiBold));
        row->addWidget(label, 1);

        // "상태 미상"으로 시작한다 — RPi C가 ACK를 안 보내(guardx_protocol.h
        // RPIC_ENABLE_ACK 미사용) VMS는 이 액추에이터가 실제로 켜져 있는지
        // 알 방법이 없다. "OFF"로 시작하면 실제로 켜져 있어도 화면은 계속
        // OFF라고 거짓말하게 된다 — 모르는 걸 안다고 하느니 안 다는 게 낫다.
        // ACK가 배선되면(TODO #2) 그때 실제 값으로 갱신한다.
        ar.state_label = new QLabel("—", row_w);
        ar.state_label->setFont(Theme::mono_font(10));
        Theme::restyle(ar.state_label, [] {
            return QString("color:%1;").arg(Theme::textDim.name());
        });
        row->addWidget(ar.state_label);

        if (ar.kind == "set" || ar.kind == "both") {
            auto *box = new QDoubleSpinBox(row_w);
            box->setRange(f.set_min, f.set_max);
            box->setSingleStep(f.set_step);
            box->setSuffix(QString::fromUtf8(f.suffix));
            box->setDecimals(0);
            ar.value_box = box;
            row->addWidget(box);

            auto *apply = new QPushButton(QString::fromUtf8("Set"), row_w);
            // 스타일 없는 맨 버튼은 OS 기본 모양이라 라이트 테마의 흰 카드
            // 위에서 통째로 사라진다 — 디자인 시스템의 #OutlineBtn을 입힌다
            // (전역 QSS가 두 테마 모두를 정의한다).
            apply->setObjectName("OutlineBtn");
            apply->setCursor(Qt::PointingHandCursor);
            ar.apply = apply;
            const QString key = ar.command_key;
            connect(apply, &QPushButton::clicked, this, [this, key] {
                ActuatorRow &r = m_actuators[key];
                send_actuator(key, "SET", int(r.value_box->value()));
            });
            row->addWidget(apply);
        }

        if (ar.kind == "onoff" || ar.kind == "both") {
            // ON/OFF를 토글 하나가 아니라 독립 버튼 둘로 둔다 — ACK가 없어
            // VMS는 실제 상태를 몰라서(§ActuatorRow), 토글이면 "다음 클릭이
            // ON인지 OFF인지" 화면 혼자 추측해야 했다. 각 버튼이 자기 명령만
            // 보내면 그 추측 자체가 필요 없다.
            const QString key = ar.command_key;

            auto *btn_on = new QPushButton(QString::fromUtf8("On"), row_w);
            btn_on->setObjectName("OutlineBtn");   // SET 버튼과 같은 이유
            btn_on->setCursor(Qt::PointingHandCursor);
            btn_on->setMinimumWidth(Theme::px(56));  // 고정폭 금지 — OutlineBtn 좌우 패딩 14px까지 더해지면 글자가 잘린다
            ar.btn_on = btn_on;
            connect(btn_on, &QPushButton::clicked, this, [this, key] {
                send_actuator(key, "ON");
            });
            row->addWidget(btn_on);

            // 화재 중엔 btn_on과 함께 잠긴다(on_fire_state_changed) — 이유는
            // btn_on 쪽 주석 참조.
            auto *btn_off = new QPushButton(QString::fromUtf8("Off"), row_w);
            btn_off->setObjectName("OutlineBtn");
            btn_off->setCursor(Qt::PointingHandCursor);
            btn_off->setMinimumWidth(Theme::px(56));
            ar.btn_off = btn_off;
            connect(btn_off, &QPushButton::clicked, this, [this, key] {
                send_actuator(key, "OFF");
            });
            row->addWidget(btn_off);
        }

        // 팬만 자동 제어를 갖는다. 임계는 zone_thresholds(정원·warn·critical)
        // 를 그대로 쓰고 단계별 듀티는 RPi C 가 정한다 — 화면은 켜고 끄기만
        // 하고 결과를 받아 표시한다.
        if (ar.command_key == QLatin1String("fan")) {
            auto *btn_auto = new QPushButton(QString::fromUtf8("AUTO"), row_w);
            btn_auto->setObjectName("OutlineBtn");
            btn_auto->setCursor(Qt::PointingHandCursor);
            btn_auto->setCheckable(true);
            btn_auto->setMinimumWidth(Theme::px(64));
            btn_auto->setToolTip(
                "Automatic ventilation from the congestion level "
                "(normal 40%, warning 75%, danger 90%). Manual controls are "
                "disabled while this is on. A fire forces 0% either way.");
            ar.btn_auto = btn_auto;
            connect(btn_auto, &QPushButton::clicked, this, [this] {
                // 눌린 상태가 아니라 **지금 RPi C 가 알려준 상태**의
                // 반대를 보낸다. 체크 표시는 응답(retained)이 오면
                // 맞춰지므로, 여기서 먼저 뒤집으면 명령이 실패했을 때
                // 화면만 켜진 채로 남는다.
                send_actuator("fan_auto", m_fan_auto ? "OFF" : "ON");
            });
            row->addWidget(btn_auto);
        }

        // shutter는 ON/OFF/SET이 아니라 OPEN/CLOSE/STOP 3버튼 — 모터 상태가
        // 이진(on)이 아니라 3상태라 토글 하나로 표현이 안 된다.
        if (ar.kind == "shutter") {
            const QString key = ar.command_key;
            for (const char *action : {"OPEN", "CLOSE", "STOP"}) {
                // 라벨은 Title Case, 전송 payload는 프로토콜 그대로(대문자)
                const QString cmd = QString::fromUtf8(action);
                auto *btn = new QPushButton(
                    cmd.left(1) + cmd.mid(1).toLower(), row_w);
                btn->setObjectName("OutlineBtn");   // SET 버튼과 같은 이유
                btn->setCursor(Qt::PointingHandCursor);
                btn->setMinimumWidth(Theme::px(56));   // "Close"가 잘리지 않게 최소폭만
                connect(btn, &QPushButton::clicked, this, [this, key, action] {
                    send_actuator(key, QString::fromUtf8(action));
                });
                // 화재 잠금이 잡을 수 있게 반드시 보관한다 — 로컬로 두면
                // on_fire_state_changed 가 손댈 방법이 없다 (08-10 수정)
                ar.btns_shutter.append(btn);
                row->addWidget(btn);
            }
        }

        list_lay->addWidget(row_w);
    }
    list_lay->addWidget(new BroadcastControlRow(list_host));
    list_lay->addStretch(1);
    lay->addWidget(list_host, 1);
    return panel;
}

// --------------------------------------------------------------- 갱신

void DeviceControlPage::on_zone_updated(int zone_id)
{
    if (m_compare)
        refresh_compare();
    else if (zone_id == m_zone_id)
        refresh_single();
}

void DeviceControlPage::refresh_single()
{
    ZoneSensorStore *store = ZoneSensorStore::instance();
    const ZoneSensorStore::Snapshot snap = store->snapshot(m_zone_id);
    const QJsonObject threshold = store->threshold();

    // 종합 위험도 게이지 — fire_score_threshold를 눈금으로 함께 그린다.
    // 값 자체보다 "임계까지 얼마나 남았나"가 알고 싶은 것이기 때문이다.
    const double fire_thr =
        threshold.value(QStringLiteral("fire_score_threshold")).toDouble(0.0);
    m_risk->set_score(snap.has_data ? snap.composite_score : -1.0, fire_thr);
    m_risk->set_caption(QString::fromUtf8("Zone %1").arg(m_zone_id));
    if (!snap.has_data) {
        m_risk_note->setText("Composite risk - waiting for data");
    } else if (snap.composite_score < 0) {
        // decision.h 규약: 음수는 0점이 아니라 "계산 안 함"이다
        m_risk_note->setText(
            "Decision frozen - the sensors were invalid, so risk was not "
            "computed this cycle.\n"
            "Fire state is held, and can only be cleared by hand.");
    } else if (fire_thr > 0) {
        m_risk_note->setText(QString(
            "Composite risk %1 / threshold %2\n"
            "A weighted sum of 5 sensors (weights live in Settings).")
                .arg(snap.composite_score, 0, 'f', 1).arg(fire_thr, 0, 'f', 0));
    } else {
        m_risk_note->setText(QString("Composite risk %1 - waiting for the threshold")
                                 .arg(snap.composite_score, 0, 'f', 1));
    }

    if (!snap.has_data) {
        for (const SensorField &f : SENSOR_FIELDS) {
            // 타일 없는 채널(irtemp_ambient)은 건너뛴다 — operator[]로 빈
            // 타일이 생기면 널 포인터를 만진다
            const auto st_it = m_sensors.find(QString::fromLatin1(f.key));
            if (st_it == m_sensors.end())
                continue;
            SensorTile &st = st_it.value();
            st.value_label->setText("—");
            st.state_label->setText("waiting");
            // 이 함수는 센서 수신마다(초당) 불린다 — 여기서 Theme::restyle을
            // 쓰면 notifier 연결이 무한히 쌓인다. 색은 평범하게 굽고, 테마
            // 전환은 생성자가 이 함수를 다시 부르는 것으로 해결한다.
            st.state_label->setStyleSheet(
                QString("color:%1;").arg(Theme::textFaint.name()));
        }
        set_status(QString("Zone %1 - waiting for sensor data...").arg(m_zone_id));
        return;
    }

    for (const SensorField &f : SENSOR_FIELDS) {
        const QString key = QString::fromLatin1(f.key);
        const auto st_it = m_sensors.find(key);
        if (st_it == m_sensors.end())   // irtemp_ambient — 타일 없음
            continue;
        SensorTile &st = st_it.value();
        const ZoneSensorStore::Sample sm = snap.channels.value(key);

        if (!sm.present) {
            st.value_label->setText("—");
            st.state_label->setText("no reply");
            st.state_label->setStyleSheet(
                QString("color:%1;").arg(Theme::textFaint.name()));
            continue;
        }

        st.value_label->setText(QString::number(sm.value, 'f', f.decimals)
                                 + QString::fromUtf8(f.suffix));
        st.state_label->setText(sm.valid ? QString("normal") : QString("abnormal"));
        st.state_label->setStyleSheet(
            QString("color:%1;").arg((sm.valid ? Theme::green : Theme::amber).name()));

        // THRESHOLD(SETTINGS 화면에서 편집) 기준 파랑/노랑/빨강 — resolve_scale이
        // 채널별 방향(오름/내림차순)을 흡수하므로 여기선 그냥 넘기기만 하면 된다.
        const Scale scale = resolve_scale(f, threshold);
        const double raw = risk_raw(sm.value, scale);
        const QColor *color = risk_color(raw, scale.has_threshold, sm.valid);
        if (st.gauge)   // ZONE 타일은 08-19 보드 전환으로 게이지가 없다
            static_cast<GaugeBar *>(st.gauge)->set_pct(qBound(0.0, raw, 1.0),
                                                       color);

        // 축 범위는 손대지 않는다 — 만들 때 f.fallback_min/max로 고정해뒀다.
        // threshold 구간(화재 판정용이라 정상 변동보다 훨씬 넓음)도, 버퍼
        // min/max 자동 스케일(작은 노이즈를 그래프 전체로 확대)도 둘 다
        // 시도했다가 더 안 좋아져서 뺐다 — 색만 threshold로 바꾼다.
        QVector<QPointF> pts;
        const QVector<ZoneSensorStore::Point> buf = store->history(m_zone_id, key);
        pts.reserve(buf.size());
        for (const ZoneSensorStore::Point &pt : buf)
            pts.append(QPointF(double(pt.t_ms), pt.value));
        static_cast<MiniLineChart *>(st.chart)
            ->set_points(pts, color, ZoneSensorStore::HISTORY_WINDOW_MS);
    }

    const QString score = snap.composite_score < 0
        ? QString("decision frozen")
        : QString("risk %1").arg(snap.composite_score, 0, 'f', 1);
    set_status(QString::fromUtf8("Zone %1 (%2) · %3")
                   .arg(m_zone_id).arg(snap.zone_name, score));
}

void DeviceControlPage::refresh_compare()
{
    ZoneSensorStore *store = ZoneSensorStore::instance();
    const double fire_thr = store->threshold()
                                .value(QStringLiteral("fire_score_threshold"))
                                .toDouble(0.0);

    // 구역별 위험도 — 선택 안 된 구역은 숨긴다(칩과 상태를 일치시킨다)
    for (auto it = m_risk_mini.constBegin(); it != m_risk_mini.constEnd(); ++it) {
        const int zid = it.key();
        const bool on = m_sel.value(zid);
        it.value()->setVisible(on);
        if (!on)
            continue;
        const ZoneSensorStore::Snapshot s = store->snapshot(zid);
        it.value()->set_score(s.has_data ? s.composite_score : -1.0, fire_thr);
    }

    for (const SensorField &f : SENSOR_FIELDS) {
        const QString key = QString::fromLatin1(f.key);
        CompareTile &ct = m_compares[key];

        QVector<MiniLineChart::Series> series;
        QStringList legend;

        // fallback_min/max(화재 판정용 넓은 구간) 그대로 축을 고정해 두면
        // 정상 변동폭이 바닥 근처 실선 한 줄로 뭉개져 "비교"가 안 됐다.
        // 지금 그리는 구역들의 실측값 범위로 축을 좁힌다.
        bool any_pt = false;
        double range_lo = 0, range_hi = 0;

        for (auto it = m_sel.constBegin(); it != m_sel.constEnd(); ++it) {
            if (!it.value())
                continue;
            const int zid = it.key();

            MiniLineChart::Series s;
            s.color = zone_color(zid);
            const QVector<ZoneSensorStore::Point> buf = store->history(zid, key);
            s.pts.reserve(buf.size());
            double z_lo = 0, z_hi = 0;
            bool z_any = false;
            for (const ZoneSensorStore::Point &pt : buf) {
                s.pts.append(QPointF(double(pt.t_ms), pt.value));
                if (!z_any) { z_lo = z_hi = pt.value; z_any = true; }
                else { z_lo = qMin(z_lo, pt.value); z_hi = qMax(z_hi, pt.value); }
            }
            series.append(s);

            if (z_any) {
                range_lo = any_pt ? qMin(range_lo, z_lo) : z_lo;
                range_hi = any_pt ? qMax(range_hi, z_hi) : z_hi;
                any_pt = true;
            }

            const ZoneSensorStore::Sample sm =
                store->snapshot(zid).channels.value(key);
            // 구역별 min~max — "현재값이 얼마나 흔들렸는지"는 그래프 굵기보다
            // 이 숫자가 더 정확히 말해준다(그래프는 축이 좁아져도 개형만 보임).
            const QString minmax = z_any
                ? QString::fromUtf8(" (%1~%2)")
                      .arg(QString::number(z_lo, 'f', f.decimals),
                           QString::number(z_hi, 'f', f.decimals))
                : QString();
            legend << QString::fromUtf8("<span style='color:%1'>Z%2</span> %3%4")
                          .arg(zone_color(zid).name())
                          .arg(zid)
                          .arg(sm.present ? QString::number(sm.value, 'f', f.decimals)
                                          : QString::fromUtf8("—"))
                          .arg(minmax);
        }

        // 위아래 10% 여백 — 값이 축 상/하단에 딱 붙으면 그 지점에서 꺾이는
        // 것처럼 보인다. 데이터가 아직 없으면(any_pt false) fallback 유지.
        if (any_pt) {
            const double span = range_hi - range_lo;
            const double pad = span > 1e-9 ? span * 0.1 : qMax(1.0, qAbs(range_hi) * 0.1);
            static_cast<MiniLineChart *>(ct.chart)
                ->set_range(range_lo - pad, range_hi + pad, f.decimals);
        } else {
            static_cast<MiniLineChart *>(ct.chart)
                ->set_range(f.fallback_min, f.fallback_max, f.decimals);
        }

        static_cast<MiniLineChart *>(ct.chart)
            ->set_series(series, ZoneSensorStore::HISTORY_WINDOW_MS);
        ct.legend->setText(legend.join(QString::fromUtf8(" · ")));
    }

    QStringList on;
    for (auto it = m_sel.constBegin(); it != m_sel.constEnd(); ++it)
        if (it.value())
            on << QString::number(it.key());
    std::sort(on.begin(), on.end());
    set_status(QString("Zone comparison - Zone %1").arg(on.join(", ")));
}

QList<int> DeviceControlPage::watched_zones() const
{
    if (!m_compare)
        return { m_zone_id };

    QList<int> on;
    for (auto it = m_sel.constBegin(); it != m_sel.constEnd(); ++it)
        if (it.value())
            on << it.key();
    std::sort(on.begin(), on.end());
    return on;
}

void DeviceControlPage::check_staleness()
{
    // 판정은 ZoneSensorStore::node_a_health() 한 곳에만 있다 (2026-08-20).
    // 여기서 zone 을 직접 훑던 코드는 지웠다 — 규칙이 세 개(더미 제외 ·
    // 재발행 배제 · 연속 조건)로 늘어난 이상, 두 벌로 두면 반드시 갈라진다.
    // 실제로 2026-08-10 에 더미 가드가 한쪽에만 있어 상단바와 이 점이 서로
    // 모순된 상태를 표시한 적이 있다.
    const ZoneSensorStore::NodeAHealth ah =
        ZoneSensorStore::instance()->node_a_health();

    if (!ah.seen) {
        m_stale_streak = 0;
        return;   // 전부 대기 중이면 색을 바꾸지 않는다
    }

    // 초록은 "꾸준히 오는 중"일 때만. 값이 신선해도 연속 조건을 못 채웠으면
    // (막 돌아왔거나 띄엄띄엄) 아직 정상이라고 말하지 않는다.
    const bool healthy = ah.streaming;
    const NodeState prev_a = m_node_a;
    m_node_a = healthy ? NodeState::Online : NodeState::Offline;
    paint_dot(m_dot_a, m_node_a);   // 점은 즉시 (상단바와 같은 기준)
    if (m_node_a != prev_a)
        qInfo().noquote()
            << QString("[DeviceControl] RPi A 점: %1 → %2 (사유: %3)")
                   .arg(node_state_text(prev_a), node_state_text(m_node_a),
                        healthy
                            ? QString("streaming (%1 cycles in a row)").arg(ah.streak)
                        : ah.age_ms > ZoneSensorStore::STALE_MS
                            ? QString("no new sensor values for %1 ms").arg(ah.age_ms)
                            : QString("unsteady (%1 of %2 cycles)")
                                  .arg(ah.streak).arg(ZoneSensorStore::STREAM_STREAK));

    // ---- 팝업으로 가는 "장애다"라는 주장은 지속 조건을 요구한다 ----
    // 점과 달리 팝업은 걸쇠(pending)라 운영자가 손으로 닫아야 한다. 1Hz 발행에서
    // 5초 공백 한 번에 띄우면 망이 조금 출렁일 때마다 팝업이 뜬다.
    // 복구는 즉시 알린다 — 그건 놓쳐서 손해 볼 게 없다.
    m_stale_streak = healthy ? 0 : m_stale_streak + 1;
    const bool claim_offline = m_stale_streak >= STALE_STREAK_FOR_ALERT;
    if (claim_offline != m_alerted_a_offline) {
        m_alerted_a_offline = claim_offline;
        qInfo().noquote()
            << QString("[DeviceControl] RPi A 경보: %1 (연속 stale %2회)")
                   .arg(claim_offline ? QString::fromUtf8("끊김 알림")
                                      : QString::fromUtf8("복구 알림"))
                   .arg(m_stale_streak);
        emit node_state_changed(Node::A, !claim_offline);
    }
}

void DeviceControlPage::on_mqtt_online(bool online)
{
    const NodeState prev_b = m_node_b;
    m_node_b = online ? NodeState::Online : NodeState::Offline;
    paint_dot(m_dot_b, m_node_b);
    if (m_node_b != prev_b)
        emit node_state_changed(Node::B, online);

    // 브로커가 끊기면 RPi A 신선도 판정 자체가 의미 없다 — 경로가 죽은 것이지
    // A 가 죽었는지는 **알 수 없다.**
    //
    // ⚠ 2026-08-10: 예전엔 여기서 Offline 으로 두고 node_state_changed(A,false)
    // 까지 쏘았다. 그러면 RpiAlertPopup 이 걸쇠를 걸고 "RPI-A · 환경센서 — 끊김"
    // 을 띄운다 — **RPi A 는 멀쩡한데 브로커가 끊긴 것**이라 사실이 아닌 주장이다.
    // 오탐 팝업의 주 원인이었다.
    //
    // 이제 Unknown 으로 둔다. 점은 2색 규칙대로 여전히 빨강이고(paint_dot 이
    // Unknown 도 빨강으로 칠한다 — UI 에 "모름" 색을 새로 만들지 않는다),
    // 다만 **알 수 없는 것을 안다고 주장하지 않는다.** 브로커가 돌아오면
    // check_staleness 가 실데이터로 다시 판정한다.
    if (!online) {
        const NodeState prev_a = m_node_a;
        m_node_a = NodeState::Unknown;
        paint_dot(m_dot_a, m_node_a);
        m_stale_streak = 0;   // 브로커 없는 동안의 공백은 A 탓이 아니다
        if (m_node_a != prev_a)
            qInfo().noquote()
                << QString("[DeviceControl] RPi A 점: %1 → 미확인 "
                           "(사유: 브로커 끊김 — A 생사 판단 불가)")
                       .arg(node_state_text(prev_a));
    }
}

void DeviceControlPage::on_rpic_status(const QByteArray &payload)
{
    // retain=true라 구독 직후 마지막 상태를 바로 받는다(LWT, guardx_protocol.h
    // GUARDX_STATUS_ONLINE/OFFLINE — VMS는 C 헤더를 안 쓰니 문자열로 비교).
    const NodeState prev_c = m_node_c;
    m_node_c = (payload == "online") ? NodeState::Online : NodeState::Offline;
    paint_dot(m_dot_c, m_node_c);
    if (m_node_c != prev_c)
        emit node_state_changed(Node::C, m_node_c == NodeState::Online);
}

/*
 * 점을 "의미"로 칠한다.
 *
 * 색이 아니라 상태를 인자로 받는 이유: 마지막 QColor를 기억해 두면 테마를
 * 바꿨을 때 옛 팔레트의 값이 그대로 굳는다. 상태만 들고 있다가 칠할 때마다
 * 팔레트를 새로 읽어야 같은 뜻이 새 테마의 색으로 나온다.
 *
 * 2색 규칙: 연결 확인(Online)만 초록, 그 외(Offline·Unknown — "아직 한 번도
 * 못 받음"도 포함)는 전부 빨강. 회색 "모름"을 없앤 이유: 부팅 직후 몇 초는
 * 다 빨갛게 뜨지만, "모름"과 "끊김"을 색으로 구분해봐야 운영자가 할 수 있는
 * 조치는 똑같다(둘 다 "아직 못 믿는다") — 색 종류를 줄이는 편이 더 명확하다.
 */
QString DeviceControlPage::node_state_text(NodeState s)
{
    switch (s) {
    case NodeState::Online:  return QString("healthy");
    case NodeState::Offline: return QString("offline");
    default:                 return QString("unknown");
    }
}

void DeviceControlPage::paint_dot(QLabel *dot, NodeState state)
{
    set_dot_color(dot, state == NodeState::Online ? Theme::green : Theme::alarm);
}

// --------------------------------------------------------------- 명령

void DeviceControlPage::send_actuator(const QString &command_key,
                                      const QString &action, int value)
{
    // 권한 백스톱 (§5) — 아래 화재 백스톱과 같은 이유로 여기 둔다.
    // 화면 잠금은 표면이다: 잠금 갱신보다 클릭이 먼저 올 수 있고, 잠긴
    // 위젯도 코드로는 clicked 를 낼 수 있다.
    if (!Auth::can(Auth::Action::ActuatorControl)) {
        qWarning().noquote()
            << QString("[Device] 권한 없는 액추에이터 명령 차단: %1 %2 — %3")
                   .arg(command_key, action,
                        Auth::deny_reason(Auth::Action::ActuatorControl));
        return;
    }

    // 화재 중 수동 명령 차단 — **백스톱**이다 (08-10).
    //
    // on_fire_state_changed 가 위젯을 비활성화하지만 그건 표면일 뿐이다:
    // 잠금 갱신보다 클릭이 먼저 처리될 수 있고(신호 순서), 새로 추가되는
    // 명령 경로가 잠금 목록에 빠질 수 있고(실제로 셔터·SET 이 그렇게 빠져
    // 있었다), 잠긴 위젯도 코드로는 clicked 를 낼 수 있다. 명령이 실제로
    // 나가는 **여기**서 한 번 더 막아야 "화재 중엔 수동 명령이 안 나간다"가
    // 위젯 목록 관리와 무관하게 성립한다.
    //
    // 판정 기준은 on_fire_state_changed 의 fire_here 와 같다 — 지금 보고 있는
    // 구역에 화재. 명령도 그 구역으로 나가므로 대상이 일치한다.
    {
        const FireAlertFeed::State fs = FireAlertFeed::instance()->state();
        if (fs.active && fs.zone_id == m_zone_id) {
            qWarning().noquote()
                << QString("[DeviceControl] 화재 중 수동 명령 차단 — zone %1 "
                           "%2 %3").arg(m_zone_id).arg(command_key, action);
            set_actuator_status(
                "Fire in progress - automatic control is running, so manual "
                "commands are not sent", true);
            return;
        }
    }

    if (const FireZoneInfo *z = find_fire_zone(m_zone_id)) {
        if (z->dummy) {
            // 더미 구역의 버튼이 진짜 RPi C를 움직이면 안 된다 — set_actuator
            // 경로는 구역을 구분하지 않고 무조건 rpic으로 나간다(task_vms.cpp
            // ACTUATOR_RPIC_NODE_ID). 안 막으면 Z4 화면을 만졌는데 Z1의 실제
            // 셔터가 닫힌다.
            set_actuator_status(
                "This is a dummy zone - no real command is sent", true);
            return;
        }
    }

    QJsonObject cmd;
    cmd["cmd"] = "set_actuator";
    cmd["command_key"] = command_key;
    cmd["action"] = action;
    if (action == "SET")
        cmd["value"] = value;

    Auth::attach_token(cmd);   // §6 — 서버가 role='admin' 을 다시 본다

    const QString id = MqttLink::instance()->request(
        SETACT_TOPIC, cmd,
        [this, command_key, action](const QJsonObject &) {
            set_actuator_status(
                QString("%1 %2 command accepted").arg(command_key, action));
        },
        [this, command_key, action](const QString &reason) {
            set_actuator_status(
                QString("%1 %2 command failed - %3")
                    .arg(command_key, action, reason), true);
        },
        MqttLink::DEFAULT_TIMEOUT_MS,
        // 인증 이유의 거절(`reason`)만 여기로 온다 — 값 오류(`error`)는
        // 위 on_error 가 받는다. 서버가 층을 필드 이름으로 알려준다.
        [this](const QJsonObject &reply) {
            set_actuator_status(Auth::note_write_reject(reply), true);
        });

    if (id.isEmpty())
        return;   // 미연결 — on_error가 이미 사유를 표시했다

    set_actuator_status(QString("%1 %2 requested...").arg(command_key, action));
}

void DeviceControlPage::on_actuator_ack(const QByteArray &payload)
{
    QJsonParseError err{};
    const QJsonObject o = QJsonDocument::fromJson(payload, &err).object();
    if (err.error != QJsonParseError::NoError)
        return;   // 잘못된 payload — 센서 파서들과 같은 정책으로 조용히 무시

    const QString command = o.value("command").toString();
    auto it = m_actuators.find(command);
    if (it == m_actuators.end())
        return;   // 카탈로그에 없는 command_key — 방어적으로 무시

    // 실패 ACK는 라벨을 안 바꾼다 — 화면에 남은 값이 "마지막으로 확인된
    // 실제 상태"다. 시도했다가 실패한 값으로 덮으면 그게 더 거짓말이다.
    if (o.value("result").toString() != "ok")
        return;

    const QString action = o.value("action").toString();
    QString text;
    if (action == "ON" || action == "OFF")
        text = action.left(1) + action.mid(1).toLower();   // 표시만 Title Case
    else if (action == "SET") {
        const int value = o.value("value").toInt();
        const ActuatorField *f = find_actuator_field(command);
        text = QString::number(value) + (f ? QString::fromUtf8(f->suffix) : QString());
    } else if (action == "OPEN" || action == "CLOSE" || action == "STOP") {
        text = action.left(1) + action.mid(1).toLower();
    } else {
        return;   // 모르는 action — 라벨을 함부로 바꾸지 않는다
    }

    if (it.value().state_label)
        it.value().state_label->setText(text);
}

/*
 * 선택 구역이 지금 화재 중인가를 배너와 버튼에 반영한다.
 *
 * FireAlertFeed는 사이트 전체 단일 상태 하나만 들고 있으므로(구역별 배열이
 * 아님), 그 상태가 **지금 보고 있는 구역 것일 때만** 화재로 친다. zone_id는
 * rpib_engine이 PHASE 6부터 항상 실어 보내고 스냅샷 쿼리도 함께 내보내므로
 * 비교가 성립한다.
 */
void DeviceControlPage::on_fan_state(const QByteArray &payload)
{
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    m_fan_auto = o.value("auto").toBool();
    m_fan_level = o.value("level").toInt();
    m_fan_duty = o.value("duty").toInt();
    const bool fire = o.value("fire").toBool();

    auto it = m_actuators.find(QStringLiteral("fan"));
    if (it != m_actuators.end()) {
        ActuatorRow &r = it.value();
        if (r.btn_auto)
            r.btn_auto->setChecked(m_fan_auto);
        if (r.state_label) {
            static const char *kLevel[] = { "normal", "warning", "danger" };
            const int lv = qBound(0, m_fan_level, 2);
            r.state_label->setText(
                fire ? QString("FIRE · 0 %")
                     : m_fan_auto
                           ? QString("AUTO · %1 · %2 %")
                                 .arg(QString::fromLatin1(kLevel[lv]))
                                 .arg(m_fan_duty)
                           : QString("MANUAL · %1 %").arg(m_fan_duty));
        }
    }
    // AUTO 가 켜지면 수동 위젯이 잠긴다 — 잠금 계산은 한 곳에만 둔다.
    on_fire_state_changed();
}

void DeviceControlPage::on_fire_state_changed()
{
    const FireAlertFeed::State st = FireAlertFeed::instance()->state();
    // 배너는 페이지 전체 알림이라 "어느 구역이든" 화재면 뜬다.
    // 해제 버튼만 "지금 보고 있는 구역"인지를 따진다 — 조작은 대상이
    // 명확해야 하기 때문이다. 그래서 판단이 둘로 나뉜다.
    const bool fire_anywhere = st.active;
    const bool fire_here = st.active && st.zone_id == m_zone_id;

    // 배너 색은 상태마다 달라 여기서 굽는다. FireAlertFeed 신호로 반복
    // 호출되는 자리라 restyle은 금지 — 대신 생성자가 테마 전환 때 이 함수를
    // 다시 부른다(읽기만 하는 함수라 다시 불려도 안전하다).
    if (fire_here) {
        m_fire_state->setText(
            QString("🔥 Fire in Zone %1 - confirmed %2 (cause: %3)")
                .arg(m_zone_id)
                .arg(st.since.toString("HH:mm:ss"), cause_text(st.cause)));
        m_fire_state->setStyleSheet(QString("color:%1;").arg(Theme::alarm.name()));
    } else if (fire_anywhere) {
        // 불은 났는데 다른 구역이다. 이 안내가 없으면 "불이 났다는데 왜
        // 내 화면 해제 버튼은 비활성이지?"에서 막힌다.
        m_fire_state->setText(
            QString("🔥 Fire in Zone %1 - confirmed %2 (cause: %3)  ·  "
                    "select Zone %1 to clear it")
                .arg(st.zone_id)
                .arg(st.since.toString("HH:mm:ss"), cause_text(st.cause)));
        m_fire_state->setStyleSheet(QString("color:%1;").arg(Theme::alarm.name()));
    } else {
        m_fire_state->setText("Normal - no fire in progress");
        m_fire_state->setStyleSheet(QString("color:%1;").arg(Theme::green.name()));
    }

    // 불이 안 났는데 해제 버튼이 눌리는 상황 자체를 없앤다. RPi B가 멱등이라
    // 눌려도 무해하지만, 눌리는 게 무해한 것과 누를 수 있게 보이는 것은 다르다
    // — 상태를 모르는 사용자에게 "지금은 할 일이 없다"를 버튼으로 말해준다.
    // 권한(§5)은 화재 조건과 **AND** 로 묶는다. 밖에서 따로 setEnabled 를
    // 걸면 이 함수와 서로 덮어써서 마지막에 불린 쪽이 이긴다.
    const bool may_clear = Auth::can(Auth::Action::FireClear);
    m_btn_clear_fire->setEnabled(fire_here && may_clear);
    if (!may_clear)
        m_btn_clear_fire->setToolTip(Auth::deny_reason(Auth::Action::FireClear));
    else if (fire_here)
        m_btn_clear_fire->setToolTip("Clears the fire state for this zone");
    else if (fire_anywhere)
        m_btn_clear_fire->setToolTip(
            QString("The fire is in Zone %1 - select that zone to clear it")
                .arg(st.zone_id));
    else
        m_btn_clear_fire->setToolTip("No fire in progress, nothing to clear");

    // 화재 중엔 ON/OFF 둘 다 잠근다 — rpib_engine이 화재 확정과 동시에
    // 이미 자동으로 액추에이터를 구동하는 중이라, 그 위에 사람이 수동
    // 명령을 더 얹으면 자동 제어와 충돌한다(어느 쪽이 이겼는지 VMS가 알
    // 방법도 없다). 수동 조작은 화재가 끝난 뒤에만 의미가 있다.
    // COMPARE 모드는 액추에이터 패널 자체가 숨어 있어(§ActuatorRow 주석)
    // 여기서 값을 바꿔도 화면에 안 보이지만, ZONE으로 돌아왔을 때 곧바로
    // 맞는 상태여야 하므로 모드와 무관하게 항상 갱신한다.
    // 잠그는 이유가 둘이 됐다 — 화재 중이거나(운영), 권한이 없거나(§5).
    // 화재를 먼저 말한다: 지금 화면에서 더 급한 사실이다.
    const bool may_act = Auth::can(Auth::Action::ActuatorControl);
    const QString lock_tip =
        fire_here ? QString("Fire in progress - automatic control is running. "
                            "Clear it first.")
        : !may_act ? Auth::deny_reason(Auth::Action::ActuatorControl)
                   : QString();
    //
    // ⚠ 08-10: 예전엔 btn_on/btn_off 만 잠갔다. 그런데 ACTUATOR_FIELDS 를 보면
    // 5줄 중 그 둘을 가진 건 2줄뿐이다 —
    //   servo_1  kind="set"     → value_box + SET 만  (통째로 안 잠겼다)
    //   fan      kind="both"    → ON/OFF 는 잠겼지만 SET 경로가 열려 있었다
    //   shutter  kind="shutter" → OPEN/CLOSE/STOP, 애초에 보관도 안 해서 손도 못 댔다
    // 즉 화재 중에 운영자가 셔터를 열거나 팬을 0으로 SET 할 수 있었고, 그게
    // 자동 화재 대응을 정면으로 거슬렀다. 이제 **명령을 보내는 모든 위젯**을 잠근다.
    for (auto it = m_actuators.begin(); it != m_actuators.end(); ++it) {
        ActuatorRow &r = it.value();
        // 팬은 AUTO 가 켜져 있으면 수동 위젯도 함께 잠근다. 자동이 도는
        // 옆에서 수동 값을 넣으면 다음 단계 전환에 조용히 덮이는데,
        // 운영자 입장에서는 "명령이 씹혔다"로 보인다.
        const bool auto_locked =
            (r.command_key == QLatin1String("fan")) && m_fan_auto;
        const QString tip = auto_locked && !fire_here && may_act
            ? QString("AUTO is on - the fan follows the congestion level. "
                      "Turn AUTO off for manual control.")
            : lock_tip;
        const auto lock = [&](QWidget *w) {
            if (!w)
                return;
            w->setEnabled(!fire_here && may_act && !auto_locked);
            w->setToolTip(tip);
        };
        lock(r.btn_on);
        lock(r.btn_off);
        lock(r.value_box);   // 값 입력도 막는다 — 잠긴 SET 옆에서 숫자만 바뀌면
        lock(r.apply);       //   운영자가 "적용됐다"고 오해한다
        for (QPushButton *b : std::as_const(r.btns_shutter))
            lock(b);
        // AUTO 토글 자신은 AUTO 상태로 잠그지 않는다 — 잠그면 끌 방법이
        // 없어진다. 화재와 권한으로만 잠긴다.
        if (r.btn_auto) {
            r.btn_auto->setEnabled(!fire_here && may_act);
            r.btn_auto->setToolTip(lock_tip);
        }
    }
}

/*
 * 수동 화재 해제 — PHASE 7에서 자동 해제를 없앤 뒤의 유일한 해제 경로.
 *
 * set_actuator와 달리 guardx_mqttd(cmd/…)를 거치지 않고 판단 엔진에 직접
 * 쏜다. 이건 DB에 검증할 게 있는 명령이 아니라 rpib_engine 메모리의 판정
 * 상태를 바꾸는 것이라, 중계 프로세스가 낄 자리가 없기 때문이다
 * (guardx_protocol.h GUARDX_TOPIC_CLEAR_FIRE_FMT 주석 참조).
 * 감사 기록은 해제 결과로 남는 fire_event('recovered') 행이 담당한다.
 */
void DeviceControlPage::send_clear_fire()
{
    if (!Auth::can(Auth::Action::FireClear)) {
        qWarning().noquote()
            << QString("[Device] 권한 없는 화재 해제 차단 — %1")
                   .arg(Auth::deny_reason(Auth::Action::FireClear));
        return;
    }

    const FireAlertFeed::State st = FireAlertFeed::instance()->state();
    if (!(st.active && st.zone_id == m_zone_id)) {
        // 버튼이 비활성이라 정상 경로로는 못 오지만, 상태가 방금 바뀐 경우
        // (해제 직전에 다른 곳에서 먼저 해제됨) 대비.
        set_actuator_status("No fire in progress", true);
        on_fire_state_changed();
        return;
    }

    // 되돌릴 수 없는 조작이라 한 번 묻는다 — 펌프·팬·경보가 한꺼번에 꺼지고,
    // 다시 켜려면 실제로 화재가 재확정되거나 액추에이터를 하나씩 손으로 켜야 한다.
    //
    // QMessageBox::question() 대신 직접 조립하는 이유: 표준 버튼은 문구가
    // Qt 번역에 딸려가 "Yes/No"로 뜰 수 있고, 기본 아이콘도 다크 테마에서
    // 튄다. 창 크롬 자체는 theme.cpp의 전역 QSS가 이미 맞춰준다.
    QMessageBox box(this);
    box.setIcon(QMessageBox::NoIcon);
    box.setWindowTitle("Confirm clear fire");
    box.setTextFormat(Qt::RichText);
    box.setText(QString::fromUtf8(
        "<b>Zone %1 currently has a fire in progress.</b><br><br>"
        "Confirmed at : %2<br>"
        "Cause : %3<br><br>"
        "Have you checked the site?<br>"
        "Clearing it stops the suppression pump, smoke fan and alarm.")
            .arg(m_zone_id)
            .arg(st.since.toString("HH:mm:ss"), cause_text(st.cause)));

    QPushButton *ok = box.addButton(QString("Clear fire"),
                                    QMessageBox::AcceptRole);
    QPushButton *cancel = box.addButton(QString("Cancel"),
                                        QMessageBox::RejectRole);
    // 해제 버튼만 경보색으로 — 위험한 쪽이 어느 것인지 색으로 구분된다.
    // 반복 호출 함수 안이지만 restyle이 안전한 예외다: 이 버튼은 호출마다 새로
    // 만들어져 대화창과 함께 죽으므로 연결이 쌓이지 않는다. 시스템 추종 모드에서
    // 대화창을 띄운 사이 OS 테마가 바뀌어도 경보색이 따라온다.
    Theme::restyle(ok, [] {
        return QString("color:%1; border-color:%1;").arg(Theme::alarm.name());
    });
    box.setDefaultButton(cancel);   // 엔터 연타로 실수 해제되지 않게
    box.exec();

    if (box.clickedButton() != ok)
        return;

    QJsonObject req;
    req["node_id"] = "vms";
    req["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    req["zone_id"] = m_zone_id;

    MqttLink::instance()->publish(
        CLEARFIRE_TOPIC, QJsonDocument(req).toJson(QJsonDocument::Compact), QOS);

    // ACK가 없다 — 해제 성공은 fire_incident 스냅샷/alert 토픽이 곧 알려준다
    // (액추에이터 명령과 같은 낙관적 처리).
    set_actuator_status(
        QString("Sent a clear-fire request for Zone %1").arg(m_zone_id));
}

// --------------------------------------------------------------- 상태줄

void DeviceControlPage::set_actuator_status(const QString &text, bool error)
{
    // 색이 아니라 **성격**을 남긴다 — 테마 전환 때 생성자가 마지막 문구와 이
    // 값으로 다시 부른다(명령 결과 문구는 다시 만들어낼 수 없으므로).
    m_act_error = error;
    m_actuator_status->setText(text);
    // 명령을 누를 때마다 불리는 자리 — restyle을 쓰면 연결이 쌓인다
    m_actuator_status->setStyleSheet(
        QString("color:%1;").arg((error ? Theme::alarm : Theme::textDim).name()));
}

void DeviceControlPage::set_status(const QString &text, bool error)
{
    m_status_error = error;   // 테마 전환 때 같은 의미로 다시 칠하려고 남긴다
    m_status->setText(text);
    // 센서 수신마다 불리는 자리 — restyle 금지. 테마 전환 때는 refresh_single/
    // refresh_compare가 다시 돌면서 이 함수를 끝에서 부르므로 색도 따라온다.
    m_status->setStyleSheet(
        QString("color:%1;").arg((error ? Theme::alarm : Theme::textDim).name()));
}
