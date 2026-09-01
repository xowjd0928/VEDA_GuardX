#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QVector>

#include "box_source.h"

class OnvifMetaSource;
class QJsonObject;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/**
 * @brief 카메라 OpenSDK 메타데이터 엔드포인트 공유 수신기
 *
 * 엔드포인트 하나가 전 채널 감지를 함께 내려주므로, 채널마다 따로 받지 않고
 * 여기서 한 번만 받아 채널별로 나눠 뿌린다.
 */
class DetectionFeed : public QObject
{
    Q_OBJECT

public:
    static DetectionFeed *instance();

    /**
     * @brief RTSP ONVIF 메타데이터 XML 인입 (direct 백엔드 → GUI 스레드)
     *
     * 같은 RTSP 세션의 m=application 트랙에서 온 tt:MetadataStream 문서.
     * 실측(2026-08-05): 박스가 영상 프레임과 록스텝(-4ms/+32ms)으로 도착 —
     * 이게 신선한 채널에선 HTTP 폴링 박스 발행이 자동 침묵한다(이중 공급
     * 방지). ONVIF가 끊기면 2초 뒤 HTTP가 자동 복귀 — 상태 전환 코드 없음.
     */
    void ingest_onvif(int channel, const QByteArray &xml);

signals:
    /**
     * @brief 새 감지 도착 (채널별)
     * @param frame_utc_ms ONVIF tt:Frame UtcTime (epoch ms) — 이 박스들이
     *        어느 영상 프레임의 것인지. HTTP 스냅샷(프레임 귀속 없음)은 -1.
     *        수신부는 이 값으로 웹 UI식 프레임 매칭을 한다 (channel_view).
     */
    void detections_arrived(int channel, const QVector<DetectionBox> &boxes,
                            qint64 frame_utc_ms);

private:
    explicit DetectionFeed(QObject *parent = nullptr);

    void request_detections();
    void handle_reply(QNetworkReply *reply);

    /**
     * @brief Face/Head 전용 보조 폴링 (juan_application/detections)
     *
     * 사람 트랙은 test 앱의 /tracks 에서 오지만 그쪽엔 Face/Head 가 없다.
     * 얼굴 블러에 필요한 category 2·3 + parent_id 는 juan_application 의
     * /detections 에만 실려 오므로 두 피드를 함께 읽는다. 자세한 근거는
     * detection_feed.cpp 상단 주석 참조.
     */
    void request_faces();
    void handle_faces_reply(QNetworkReply *reply);

    /** @brief 계약서 필터 이식 — 수신부가 하던 검증을 여기서 수행 */
    bool is_valid(const QJsonObject &obj) const;

    /** @brief 완성된 tt:MetadataStream 문서 하나 파싱·발행 (ingest_onvif 하위) */
    void process_onvif_doc(int channel, const QByteArray &doc);

    /** @brief 채널별 ONVIF 수신 통계 5초 로그 (문서/프레임/박스/미매핑) */
    void report_onvif_stats();

    QNetworkAccessManager *m_net = nullptr;
    QTimer *m_timer = nullptr;
    QNetworkReply *m_pending = nullptr;       ///< /tracks 요청 중복 방지
    QNetworkReply *m_pending_faces = nullptr; ///< /detections 요청 중복 방지

    QHash<int, QHash<int, DetectionBox>> m_live;  ///< channel -> object_id -> 최신 박스
    QDateTime m_since;                            ///< 다음 요청 커서 (카메라 시계 기준)

    /// channel -> face object_id -> 최신 Face/Head 박스 (m_live 와 별도로 만료)
    QHash<int, QHash<int, DetectionBox>> m_faces;
    QDateTime m_faces_since;                      ///< /detections 커서

    /**
     * @brief channel -> raw object_id -> display_id
     *
     * 두 피드의 id 공간이 다르다. /tracks 의 사람 박스는 display_id(예: 435)를
     * object_id 로 쓰는데, /detections 의 Face/Head 는 parent_id 에 카메라
     * 원본 object_id(예: 2500)를 싣는다. 그대로 두면 "이 얼굴이 저 사람 것"이
     * 연결되지 않아 중복 얼굴 억제(channel_view)가 오동작한다.
     */
    QHash<int, QHash<int, int>> m_raw_to_display;

    // ---- ONVIF 메타데이터 서브스트림 (2026-08-05) ----
    QHash<int, QByteArray> m_onvif_buf;    ///< channel -> 문서 조각 조립 버퍼
    QHash<int, qint64> m_onvif_last_ms;    ///< channel -> 마지막 **박스 프레임** 수신
    QHash<int, qint64> m_onvif_doc_ms;     ///< channel -> 마지막 **아무 문서** 수신 (생존 신호)
    QVector<OnvifMetaSource *> m_meta_sources;  ///< 채널별 메타 전용 세션
    QHash<int, qint64> m_onvif_poke_ms;    ///< channel -> 마지막 교차검증 재시작
};