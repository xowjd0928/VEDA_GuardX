#pragma once

#include <QObject>

#include <atomic>

#include <gst/gst.h>

class QTimer;
class DetectionFeed;
typedef struct _GstAppSink GstAppSink;

/**
 * @brief 채널 하나의 ONVIF 메타데이터 전용 RTSP 세션
 *
 * 웹 UI처럼 카메라의 vnd.onvif.metadata 트랙에서 박스를 푸시로 받는다.
 * 단, 영상 세션에 얹지 않고 **메타데이터만 구독하는 별도 경량 세션**을 쓴다
 * (rtspsrc select-stream으로 application 트랙만 SETUP — 영상 스트림은
 * 요청 자체를 안 하므로 대역폭은 XML 몇 KB/s뿐).
 *
 * 왜 별도 세션인가 (2026-08-05 실사고): 영상 세션의 메타 pad를 분기하는
 * in-band 방식은 rtspsrc의 pad 노출이 세션마다 비결정적이었다 — 실행마다
 * 무작위 채널이 video만/meta만/둘 다 없음으로 절름발이가 됐다(12s 무프레임
 * 재접속 루프). 메타 전용 세션은 같은 날 파이썬 프로브로 전 실행 안정을
 * 실증했고, 영상 파이프라인은 검증된 원형 그대로 남는다.
 *
 * 수명: DetectionFeed가 채널당 하나 소유. 받은 XML은 도착 즉시
 * DetectionFeed::ingest_onvif(GUI 스레드)로 넘어간다.
 */
class OnvifMetaSource : public QObject
{
    Q_OBJECT

public:
    explicit OnvifMetaSource(int channel, DetectionFeed *feed,
                             QObject *parent = nullptr);
    ~OnvifMetaSource() override;

    /** @brief 세션 시작 (요소 부재·설정 꺼짐이면 조용히 무시) */
    void start();

    /**
     * @brief 외부 증거 기반 재시작 요청 (DetectionFeed의 교차 검증)
     *
     * "HTTP는 이 채널에서 사람을 보는데 ONVIF는 침묵" = 빈 방이 아니라 죽은
     * 세션이다. 첫 8초 워치독이 못 잡는 두 경우 — ①수렴 후 조용한 사망
     * ②이벤트 버스트만 주고 프레임은 안 주는 반죽음 — 를 이 경로가 잡는다.
     * 기동 전(스태거 대기)·재시작 대기 중이면 무시한다.
     */
    void poke_restart(const QString &reason);

private:
    static GstFlowReturn s_sample(GstAppSink *sink, gpointer user);
    static gboolean s_select_stream(GstElement *src, guint num, GstCaps *caps,
                                    gpointer user);

    /**
     * @brief 파이프라인 버스를 주기적으로 비운다 (ERROR/EOS → 재시작).
     *
     * `gst_bus_add_watch` 를 쓰면 안 된다 — 그건 GLib 메인루프가 돌아야
     * 콜백이 뜨는데 Qt 는 Windows 에서 GLib 루프를 돌리지 않는다
     * (`QT_FEATURE_glib=OFF`). 워치를 걸면 콜백이 **한 번도 안 불리고**
     * 메시지만 버스에 쌓인다. 방송 송신기와 같은 폴링 방식으로 간다.
     */
    void poll_bus();

    void teardown();
    void schedule_restart(const QString &reason);

    int m_channel = -1;
    DetectionFeed *m_feed = nullptr;
    GstElement *m_pipeline = nullptr;
    QTimer *m_bus_poll = nullptr;
    QTimer *m_retry = nullptr;
    int m_retry_count = 0;

    /**
     * 무데이터 워치독: 건강한 세션은 PLAY 직후 이벤트 초기화 버스트가
     * 반드시 온다 (전 실측 공통). 8초 무데이터 = 카메라가 이 세션을 굶기는
     * 것 — 재시작이 새 세션으로 재추첨한다. 오류 없이 조용히 굶는 게 이
     * 증상의 특징이라 버스 워치만으론 못 잡는다.
     */
    QTimer *m_data_watch = nullptr;
    std::atomic<bool> m_got_data{false};
    /// 이 세션이 살면서 한 번이라도 문서를 받아봤는가 (기동 굶주림 구분용)
    std::atomic<bool> m_ever_had_data{false};
};
