#pragma once

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;

/**
 * @brief 평면도 위에 누적 히트맵을 그리는 캔버스
 *
 * 좌표계는 FLOOR_W x FLOOR_H 가상 평면도이고, 위젯 크기에 맞춰 비율을 유지한
 * 채 스케일된다. 카메라 화면 좌표(2592x1520)를 평면도로 옮기는 변환이
 * 이 클래스의 핵심이다 (to_floor).
 *
 * ★ 부채꼴 근사(옛 ChannelFan/FANS[])는 없앴다. 카메라가 실제로 어디를
 *   보는지는 CalibrationStore 의 H(호모그래피)만으로 정확히 안다 — 방위각을
 *   손으로 추측해 적어둘 이유가 없다. 그래서 지금은:
 *     - 채널에 usable 한 H 가 없으면 그 채널은 **아무것도 안 그린다**
 *       (틀릴 게 뻔한 근사보다 빈 화면이 낫다는 원칙을 여기도 그대로 적용)
 *     - 있으면 H⁻¹ 로 "이 바닥 지점이 화면 안에 들어오나"를 픽셀 단위로
 *       판정해 실제 가시영역을 칠한다 (웹 UI 캘리브레이션 탭의 「전체 지도」
 *       와 같은 알고리즘 — calDrawMap() 참고)
 */
class FloorCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit FloorCanvas(QWidget *parent = nullptr);

    /** @brief 격자 누적값 교체 (키: 평면도 셀 좌표) */
    void set_cells(const QHash<QPair<int, int>, double> &cells, double max_weight);

    /**
     * @brief 채널(1~4)의 가시영역 표시 on/off — CROWD 상단 토글이 부른다.
     *
     * usable 한 H 가 없는 채널은 어차피 아무것도 안 그리므로, 이 토글은
     * "있는데 화면이 복잡해서 잠깐 끄고 싶을 때"를 위한 것이다.
     */
    void set_fan_visible(int channel, bool visible);

    /**
     * @brief 카메라 화면 좌표(라이브 프레임 픽셀) -> 평면도 좌표.
     *
     * CalibrationStore 에 그 채널의 usable 한 H 가 있으면 그걸로 정확히
     * 계산한다. 없으면 **모른다는 뜻으로 화면 밖 좌표**를 돌려준다 —
     * 호출부(render_heat)가 이 경우 그 점을 건너뛴다. 예전처럼 부정확한
     * 부채꼴 근사로 대충 찍지 않는다.
     *
     * force_draw() 가 켜져 있으면 usable=false 인 채널도 geo_usable(H 만
     * 있으면 true)이면 계산한다. 이때 입력 픽셀을 H 를 만든 좌표계로
     * 환산하므로 **해상도 차이는 사라지고 시점 차이만 남는다** — 위치는
     * 여전히 못 믿는다. 호출부가 그걸 화면에 드러내야 한다.
     */
    static QPointF to_floor(int channel, double cam_x, double cam_y);

    /**
     * @brief 축소판 모드 — 레일 카드처럼 폭 300px 미만으로 그릴 때
     *
     * 지도 위 문장(캘리브레이션 경고)·가구 이름·히트 범례를 숨긴다. 축소판
     * 에서는 어차피 못 읽고 겹쳐서 "깨진 화면"으로만 보인다(08-21 지적).
     * 큰 캔버스(기본 false)는 종전과 완전히 동일하다.
     */
    void set_compact(bool on) { m_compact = on; update(); }

    /**
     * @brief 정규화 좌표(0..1) -> 평면도 좌표.
     *
     * TrackPoint::cam 처럼 프레임 대비 비율로 오는 입력용. 어느 해상도로
     * 되돌릴지는 **그 채널의 H 가 정한다**(cc.frame_w/h) — 라이브 프레임
     * 상수로 되돌리면 사진 기준 H 에 엉뚱한 크기의 픽셀을 먹이게 된다.
     * 그 환산을 호출부마다 다시 쓰지 않도록 여기 한 곳에 둔다.
     */
    static QPointF to_floor_norm(int channel, double nx, double ny);

    // 가상 평면도 크기 (실측 방 비율과 다를 수 있다 — 장애물·가시영역은
    // room_w_cm/room_h_cm 로 정규화해 이 캔버스에 얹으므로 값 자체는
    // 상관없다. 위젯 종횡비 기준일 뿐)
    static const int FLOOR_W = 1000;
    static const int FLOOR_H = 620;

    // 격자 한 칸 (평면도 단위). 작을수록 조밀하지만 노이즈에 민감해진다.
    static const int CELL = 16;

    // 카메라 원본 해상도 — ChannelView/detections.geom과 동일해야 한다.
    // CalibrationStore 가 이 값과 frame_w/h 를 대조해 usable 을 정한다.
    static const int FRAME_W = 2592;
    static const int FRAME_H = 1520;

protected:
    void paintEvent(QPaintEvent *ev) override;

private:
    /** @brief 평면도 좌표 -> 위젯 좌표 (레터박스 유지) */
    QPointF to_widget(const QPointF &floor_pt) const;
    /** @brief 방 실측 좌표(cm) -> 위젯 좌표. rw/rh = 방 크기 cm */
    QPointF cm_to_widget(const QPointF &cm, double rw, double rh) const;

    /**
     * @brief FLOOR 좌표 -> 위젯 좌표 배율·여백. 방 종횡비를 되살린다.
     *
     * FLOOR_W/FLOOR_H 는 내부 정규화 좌표일 뿐 방의 실제 비율이 아니다.
     * 등비로 그리면 1300x700 방(1.86)이 1000x620 상자(1.61)에 눌려 가로가
     * 13% 짧아진다 — 지도에서 거리를 눈으로 재면 틀린다. 그래서 방 크기를
     * 알면 x/y 배율을 따로 잡아 실제 비율로 그린다 (웹 UI 지도와 같은 모양).
     */
    void view_transform(double &sx, double &sy, double &ox, double &oy) const;

    /** @brief 채널(1~4)의 실제 가시영역을 H⁻¹ 로 판정해 칠한다 (usable 아니면 no-op) */
    void draw_coverage(class QPainter &p, int channel) const;

    QHash<QPair<int, int>, double> m_cells;
    double m_max_weight = 1.0;
    bool m_compact = false;   ///< set_compact() — 축소판에서 텍스트류 숨김
    bool m_fan_visible[4] = { true, true, true, true };   ///< index = 채널(0-based)
};

/**
 * @brief 하루치 히트맵 한 칸 — (10분 슬롯, 채널, 카메라 격자 좌표) 별 검출 수
 *
 * 서버가 하루를 통째로 이 형태로 집계해 보내고, 화면 합성은 전부 클라이언트가
 * 한다. 슬라이더를 움직이거나 날짜를 더 고르는 것은 이 배열을 다시 훑는
 * 일이므로 네트워크가 발생하지 않는다.
 */
struct DayCell {
    qint16 slot;      ///< 0~143 (하루를 10분으로 나눈 인덱스)
    qint16 channel;
    qint16 gx, gy;    ///< 카메라 화면 격자 좌표 (CAM_CELL 단위)
    qint32 count;
};

/**
 * @brief CROWD 화면 — 평면도 통합 히트맵
 *
 * 데이터는 *날짜 단위로 한 번씩* 받아 캐시한다. 서버는 하루를 10분 슬롯으로
 * 집계해 주고, 슬라이더·누적·날짜 다중선택은 전부 그 캐시 위에서 계산한다.
 *
 * 이 구조를 고른 이유:
 *  - 슬라이더를 움직일 때마다 조회하면 요청이 폭주하고, 응답 순서가 뒤바뀌어
 *    화면이 튄다. 날짜당 1회로 줄이면 그 문제가 대부분 사라진다.
 *  - 사람은 데이터를 하루 단위로 본다. 접근 패턴과 전송 단위가 맞는다.
 *  - 보존 기간(14일) 전체를 10분 단위로 받으면 수십 MB지만, 하루치는 1~2MB다.
 *
 * 날짜를 여러 개 고르면 *같은 시각끼리* 합산한다 — "이 날들의 오후 2시엔
 * 어디가 붐볐나". 히트맵 색은 화면의 최댓값 기준으로 정규화되므로 날짜 수가
 * 늘어도 색 분포는 유지된다 (평균을 따로 낼 필요가 없다).
 */
class CrowdPage : public QWidget
{
    Q_OBJECT

public:
    explicit CrowdPage(QWidget *parent = nullptr);

private:
    void build_ui();

    /** @brief 캐시만으로 히트맵을 다시 그린다 (네트워크 없음) */
    void render_heat();

    /** @brief 날짜 선택이 바뀌었을 때 — 필요한 날짜를 받고 다시 그린다 */
    void on_selection_changed();

    /** @brief 선택된 날짜 중 캐시에 없는 것을 요청 */
    void ensure_days_loaded();

    /** @brief 날짜 목록 수신 (guardx/db/rpib/dates) — 우측 목록을 채운다 */
    void on_dates(const QByteArray &payload);

    /** @brief 하루치 집계 수신 (MqttLink::request 콜백 — 날짜는 캡처로 온다) */
    void on_day_result(const QJsonObject &reply, const QDate &date);

    /** @brief 슬라이더 눈금 갱신 (프리셋에 따라 칸 수만 바뀐다) */
    void rebuild_slider();

    /**
     * @brief 슬롯 통계 4줄을 만든다 (좌측 View 패널 아래)
     *
     * 보드는 우측 날짜 열 밑에 두지만, 우리 날짜 목록은 14일치를 세로로 다
     * 채워서 그 아래가 창 바닥에 눌린다 — 좌측이 비어 있어 훨씬 잘 보인다.
     */
    QWidget *build_slot_stats(QWidget *parent);

    /**
     * @brief 슬롯 통계 4줄 갱신
     *
     * 값은 render_heat() 가 이미 도는 루프에서 같이 모은다 — 통계를 위해
     * 캐시를 한 번 더 훑지 않는다.
     *
     * @param per_channel 채널 → 검출 수 (**데이터가 온 채널만** 들어 있다)
     * @param busiest     평면도 격자에서 가장 많이 쌓인 칸
     * @param busiest_w   그 칸의 누적값 (0 이면 표시할 것이 없다)
     */
    void update_slot_stats(const QHash<int, qint64> &per_channel,
                           qint64 total,
                           const QPair<int, int> &busiest,
                           double busiest_w,
                           const QString &span);

    void set_status(const QString &text, bool error = false);

    FloorCanvas *m_canvas = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_range_label = nullptr;
    QSlider *m_slider = nullptr;
    QListWidget *m_date_list = nullptr;
    QPushButton *m_cumulative_btn = nullptr;

    // ---- 슬롯 통계 (우측 열 하단) ----
    QLabel *m_slot_title = nullptr;   ///< "Slot 14:30 → 14:40"
    QLabel *m_stat_peak = nullptr;    ///< 가장 붐빈 채널
    QLabel *m_stat_total = nullptr;   ///< 총 검출 수
    QLabel *m_stat_cell = nullptr;    ///< 최다 격자 칸
    QLabel *m_stat_quiet = nullptr;   ///< 가장 한산한 채널

    /// true = 자정부터 현재 슬라이더까지 누적, false = 그 칸 하나만.
    /// 다중 선택 + 누적 + 슬라이더 끝 = "고른 날들의 하루 전체"가 된다.
    bool m_cumulative = false;

    /// 슬라이더 한 칸 = 몇 분. payload는 항상 10분 슬롯이고, 더 큰 값은
    /// 클라이언트에서 묶어 올린다 (재요청 없음).
    int m_step_min = 10;

    QHash<QDate, QVector<DayCell>> m_day_cache;  ///< 날짜 → 하루치 집계
    QHash<QDate, QString> m_inflight;            ///< 요청 중인 날짜 → req_id
    QSet<QDate> m_selected;                      ///< 현재 선택된 날짜들

    /// payload 슬롯 크기. 서버와 맞춰야 한다.
    static const int SLOT_MIN = 10;
    static const int SLOTS_PER_DAY = 24 * 60 / SLOT_MIN;   // 144

    // 응답 대기 한도는 MqttLink::request()가 관리한다 (DEFAULT_TIMEOUT_MS).
    // MQTT는 publish가 항상 성공하고 응답이 없으면 조용하므로, 타임아웃이
    // 없으면 "느림"과 "죽음"을 구분할 수 없다 — 그 판단이 거기로 옮겨갔다.

    /** @brief 슬라이더 칸 크기 프리셋 — 전부 클라이언트 재집계라 재요청 없음 */
    struct Preset { const char *label; int minutes; };
    static const Preset PRESETS[];
    static const int PRESET_COUNT;
};
