#include "box_source.h"
#include "detection_feed.h"

BoxSource::BoxSource(int channel, QObject *parent)
    : QObject(parent), m_channel(channel)
{
    // 공유 피드에서 내 채널 것만 걸러 넘긴다
    connect(DetectionFeed::instance(), &DetectionFeed::detections_arrived,
            this, [this](int channel, const QVector<DetectionBox> &boxes,
                         qint64 frame_utc_ms) {
                if (channel == m_channel)
                    emit boxes_updated(boxes, frame_utc_ms);
            });
}