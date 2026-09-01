#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QVector>

/**
 * @brief 바닥 캘리브레이션 웹 UI(test_calibration)가 내보낸 calibration.json 을
 *        읽어 평면도에 쓸 수 있는 형태로 들고 있는다.
 *
 * ★ 지금은 "파일을 불러온다"까지만 한다. RPi B/MQTT 로 자동 수신하는 건
 *   다음 단계다(WORKLOG 2026-08-10 §11 참조) — 이 클래스가 그 껍데기가
 *   될 자리라, "①어디서 가져오나"만 나중에 MQTT 로 바뀌도록 ②JSON→구조체
 *   파싱과 분리해 뒀다. load_file() 만 바뀌면 된다.
 *
 * ⚠ 지금 웹 UI에서 뽑은 H 는 아직 폰 사진(3024x4032) 기준일 수 있다.
 *   `frame_w/frame_h` 가 FloorCanvas::FRAME_W/H(2592x1520) 와 다른 채널은
 *   기본적으로 **평면도에 안 그린다** — 조용히 틀린 좌표를 보여주는 것보다
 *   빠진 채로 보이는 편이 안전하다 (HANDOFF 문서의 반복된 경고와 같은 이유).
 *   `force_draw(true)` 로 검증용으로만 강제로 볼 수 있다 (다른 스타일로).
 *
 * ★ 계산은 여기서 하지 않는다. H 는 실시간 점 변환(발밑점 등)에만 쓰고,
 *   가시영역은 웹 UI가 이미 계산해 보낸 격자(`ChannelCalib::coverage`)를
 *   그대로 읽는다 — 호모그래피 역행렬·지평선 부호판정 같은, 검증하기
 *   어려운 수학을 Qt(C++) 쪽에서 다시 구현하지 않기 위해서다.
 */
class CalibrationStore : public QObject
{
    Q_OBJECT

public:
    struct Obstacle {
        int channel = 0;
        QString name;
        // 항상 4점 사각형(방 좌표 cm) — 웹 UI 의 calBoxesOf() 가 그렇게 만든다.
        QVector<QPointF> footprint_cm;
    };

    struct ChannelCalib {
        bool present = false;
        int frame_w = 0, frame_h = 0;   // 이 H 를 만든 좌표계
        QString source;
        // 3x3 호모그래피, 행 우선(H[0..2]=1행 …). "이 채널 픽셀 → 방 cm".
        // 웹 UI calHomography() 가 만드는 것과 같은 배치.
        double H[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 };
        // usable = frame_w/h 가 FloorCanvas::FRAME_W/H(라이브 카메라 프레임)와
        // 일치하고 H 도 왔을 때만 true. **실시간 발밑점 변환(to_floor)** 에만
        // 쓴다 — 그 변환은 라이브 스트림 프레임 좌표를 그대로 H 에 넣으므로
        // H 를 만든 사진의 해상도와 같아야 한다.
        bool usable = false;

        // geo_usable = H 만 있으면 true(프레임 해상도 무관). 가시영역·장애물은
        // 웹 UI가 "그 사진 좌표계"에서 이미 cm 로 계산해 보낸 정적 결과라
        // 라이브 카메라 프레임 해상도와 아무 관계가 없다 — usable 로 같이
        // 거르면 폰 사진처럼 해상도가 다른 채널의 (정확한) 가시영역까지
        // 안 그려지는 바람에 예전에 usable 을 통째로 완화하는 임시방편이
        // 필요했었다. 이제 두 조건을 분리해 그 임시방편이 필요 없다.
        bool geo_usable = false;

        // ── 가시영역 다각형 ──
        // 웹 UI가 계산해 보낸 꼭짓점 목록(방 좌표 cm). 가시영역은 영상
        // 사각형이 바닥에 투영된 모양이라 경계가 **직선**이다 — 그래서
        // 격자가 아니라 다각형으로 받는다. VMS는 그리기만 하면 되고,
        // 호모그래피 역행렬·지평선 판정을 여기서 다시 하지 않는다.
        // (예전엔 20cm 격자로 받아서 계단처럼 보였다)
        QPolygonF coverage_poly;
    };

    static CalibrationStore *instance();

    /** @brief 지금 로드돼 있나 (파일을 한 번도 안 읽었으면 false) */
    bool loaded() const { return m_loaded; }

    double room_w_cm() const { return m_room_w_cm; }
    double room_h_cm() const { return m_room_h_cm; }

    /** @brief 채널 1~4. 없는 채널은 present=false */
    ChannelCalib channel(int ch) const;

    /**
     * @brief 불러온 전 채널의 장애물 (usable 여부 무관).
     *
     * 채널별로 믿을 수 있는지는 channel(ch).usable 로 따로 물어야 한다 —
     * 여기서 미리 걸러버리면 force_draw() 로 "그래도 보기"를 켰을 때
     * 다시 불러올 방법이 없다.
     */
    const QVector<Obstacle> &obstacles() const { return m_obstacles; }

    /**
     * @brief true 면 usable=false 채널도 그린다 (검증용).
     *
     * 기본은 false — 좌표계가 다른 H 를 조용히 그리는 게 더 위험하다는
     * 원칙은 그대로다. 켜면 **호출부(crowd_page)가 다른 스타일로** 그려서
     * "이건 못 믿는 좌표"라는 걸 계속 알린다. 끄면 다시 원래대로 걸러진다.
     */
    bool force_draw() const { return m_force_draw; }
    void set_force_draw(bool on);

    /**
     * @brief 라이브 좌표를 놓을 수 있는 채널 수 (`usable` 인 채널)
     *
     * **0 이면 동선도 히트맵도 한 점 안 그려진다.** 방·가구는 그대로 보이므로
     * (그쪽은 `geo_usable` 만 본다) 화면만 봐서는 고장과 구분이 안 된다 —
     * 그래서 그리는 쪽이 이 값을 보고 이유를 적을 수 있게 노출한다.
     */
    int live_usable_count() const;

    /**
     * @brief 왜 좌표가 안 놓이는지 한 줄로 (문제 없으면 빈 문자열)
     *
     * 08-13 실사고: 관리자가 올린 전역 캘리브레이션이 **세로로 찍은 휴대폰
     * 사진**(1050×1400)으로 만들어져 라이브 프레임(2592×1520)과 좌표계가 달랐다.
     * 방과 가구는 멀쩡히 그려지는데 동선·히트맵만 통째로 사라져서, 화면을 보는
     * 사람은 추적이 고장 났다고 읽었다. 실제로 다른 건 **H 의 출처**뿐이다.
     */
    QString frame_mismatch_note() const;

    /** @brief 마지막으로 불러온 파일 경로 (다시 열기 편의용) */
    QString last_path() const { return m_last_path; }

    /**
     * @brief calibration.json 을 읽어 상태를 교체한다.
     * @return 성공하면 true. 실패하면 error() 에 사유가 남고 이전 상태는 유지된다.
     */
    bool load_file(const QString &path);

    /**
     * @brief 파싱·검증·적용의 본체 — 파일이 아니라 JSON 객체를 받는다 (08-12)
     *
     * 헤더 머리말의 "①어디서 가져오나"가 site_config(MQTT 전역 설정)로
     * 확장되면서 분리했다. load_file() 은 이제 읽어서 여기로 넘길 뿐이다.
     * @param source 어디서 온 값인지 사람이 읽을 표기 (파일 경로·"site_config" 등)
     */
    bool load_json(const QJsonObject &root, const QString &source);

    /**
     * @brief 적용하지 않고 유효성만 본다 (load_json 과 **같은 규칙**)
     *
     * 원격에서 온 값을 캐시에 굳히기 전에 거르는 용도. 화면에 이미 다른
     * 캘리브레이션(운영자 로컬 오버라이드)이 떠 있을 수 있어, 검증하려고
     * 실제로 적용해 버리면 그 화면을 밀어낸다.
     * @param err 실패 사유 (선택)
     */
    static bool is_valid(const QJsonObject &root, QString *err = nullptr);

    /**
     * @brief 마지막으로 **성공 적용된** 원본 JSON (관리자 [적용]→전역 발행용)
     *
     * 서버는 calibration 을 해석하지 않고 통짜 보관한다(계약) — 그래서
     * 파싱된 구조체가 아니라 원본을 그대로 다시 내보낼 수 있어야 한다.
     */
    QJsonObject raw_json() const { return m_raw; }

    QString error() const { return m_error; }

signals:
    /** @brief 성공적으로 새로 로드됐다 — 평면도가 이걸 받아 다시 그린다 */
    void changed();

private:
    explicit CalibrationStore(QObject *parent = nullptr);

    bool m_loaded = false;
    bool m_force_draw = false;
    double m_room_w_cm = 0, m_room_h_cm = 0;
    QHash<int, ChannelCalib> m_channels;
    QVector<Obstacle> m_obstacles;
    QString m_last_path;
    QString m_error;
    QJsonObject m_raw;   ///< 마지막 성공 적용분의 원본 (전역 발행용)
};
