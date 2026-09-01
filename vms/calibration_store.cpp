#include "calibration_store.h"

#include "crowd_page.h"

#include <QFile>
#include <QJsonArray>
#include <QDebug>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

CalibrationStore *CalibrationStore::instance()
{
    static CalibrationStore inst;
    return &inst;
}

CalibrationStore::CalibrationStore(QObject *parent) : QObject(parent)
{
    // 검증 모드는 **재시작해도 남는다**. 예전에는 런타임 전용이라, 좌표계가
    // 어긋난 현장에서 체크를 켜 두고 다음 날 앱을 켜면 동선이 도로 사라졌다 —
    // 그러면 "어제는 됐는데"가 되고 원인을 다시 찾아야 한다.
    // ⚠ 기본값이 **켜짐**이다 (08-13 결정). 현장에 올라오는 전역 캘리브레이션은
    //   웹 UI 에서 사진으로 만든 것이라 라이브 프레임과 해상도가 거의 항상
    //   다르다 — 엄격히 걸면 관리자가 파일을 올려도 CROWD 히트맵과 동선이
    //   **한 점도** 안 그려지고, 개인이 SETTINGS 에서 파일을 따로 넣어야만
    //   보였다. 그건 못 쓰는 화면이다.
    //   ⚠ 대신 그 좌표는 **근사치**다. 화면이 그렇게 말한다(frame_mismatch_note).
    //   정확한 위치가 필요하면 캘리브레이션을 라이브 프레임으로 다시 만들어야
    //   한다 — 이 스위치는 그때까지의 다리지, 정답이 아니다.
    m_force_draw = QSettings("GuardX", "VMS")
                       .value("site/calibration_force_draw", true).toBool();
}

int CalibrationStore::live_usable_count() const
{
    int n = 0;
    for (auto it = m_channels.cbegin(); it != m_channels.cend(); ++it)
        if (it->usable) ++n;
    return n;
}

QString CalibrationStore::frame_mismatch_note() const
{
    if (!m_loaded || m_room_w_cm <= 0.0 || m_room_h_cm <= 0.0)
        return QString();
    if (live_usable_count() > 0)
        return QString();

    // 어긋난 프레임 하나를 예로 든다 — 네 채널을 다 적으면 한 줄에 안 들어가고,
    // 사람이 확인해야 할 것은 "무엇으로 만들었나" 하나다.
    for (auto it = m_channels.cbegin(); it != m_channels.cend(); ++it) {
        if (!it->geo_usable || it->frame_w <= 0 || it->frame_h <= 0)
            continue;
        // 그린다(force_draw 기본 켜짐)와 안 그린다에 따라 할 말이 다르다.
        return m_force_draw
                   ? QString("positions are approximate - calibration frame "
                             "%1x%2 != live %3x%4")
                         .arg(it->frame_w).arg(it->frame_h)
                         .arg(FloorCanvas::FRAME_W).arg(FloorCanvas::FRAME_H)
                   : QString("calibration frame %1x%2 != live %3x%4 - positions "
                             "cannot be placed")
                         .arg(it->frame_w).arg(it->frame_h)
                         .arg(FloorCanvas::FRAME_W).arg(FloorCanvas::FRAME_H);
    }
    return QStringLiteral("calibration has no camera geometry - positions "
                          "cannot be placed");
}

CalibrationStore::ChannelCalib CalibrationStore::channel(int ch) const
{
    return m_channels.value(ch, ChannelCalib{});
}

bool CalibrationStore::load_file(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = QString("Cannot open the file: %1").arg(f.errorString());
        return false;
    }
    const QByteArray raw = f.readAll();

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        m_error = QString("JSON parse failed: %1").arg(perr.errorString());
        return false;
    }
    return load_json(doc.object(), path);
}

bool CalibrationStore::is_valid(const QJsonObject &root, QString *err)
{
    // 거절 조건은 이것 하나다 — 나머지 필드는 없으면 없는 대로 그린다
    // (채널·장애물은 부분적으로 와도 유효한 상태다). 방 크기는 평면도의
    // 좌표계 자체라 없으면 아무것도 그릴 수 없다.
    const QJsonObject room = root.value("room").toObject();
    const double w = room.value("width_cm").toDouble(0.0);
    const double h = room.value("height_cm").toDouble(0.0);
    if (w > 0.0 && h > 0.0)
        return true;
    if (err)
        *err = QStringLiteral("room.width_cm / height_cm is missing or zero");
    return false;
}

/**
 * @brief calibration.json 의 채널 키가 1-based 인가 (웹 UI 규약)
 *
 * ⚠⚠ **카메라는 채널을 0..3 으로, 캘리브레이션 웹 UI 는 1..4 로 센다**
 *    (2026-08-22 사용자 확인). 그대로 읽으면 cam1 의 호모그래피가 CH2 에,
 *    cam2 가 CH3 에 붙고 CH1·CH4 는 비어 버린다 — 화면에는 "캘리브레이션이
 *    있는데 위치가 이상한" 모습으로만 나타나 원인을 찾기 어렵다.
 *
 * 그래서 **파일을 보고 판정한다**: 키에 0 이 있으면 0-based 내보내기이므로
 * 그대로 쓰고, 아니면 1-based 로 보고 한 칸 당긴다. 두 규약이 실제로 공존
 * 하는 상황이라(카메라 축 · 웹 UI 축) 무조건 −1 은 0-based 파일의 ch0 을
 * 조용히 버린다.
 */
static bool keys_are_one_based(const QJsonObject &channels)
{
    for (auto it = channels.begin(); it != channels.end(); ++it) {
        bool ok = false;
        const int k = it.key().toInt(&ok);
        if (ok && k == 0)
            return false;   // 0 이 있다 = 카메라 축 그대로
    }
    return !channels.isEmpty();
}

bool CalibrationStore::load_json(const QJsonObject &root, const QString &source)
{
    if (!is_valid(root, &m_error))
        return false;

    const QJsonObject room = root.value("room").toObject();
    const double room_w = room.value("width_cm").toDouble(0.0);
    const double room_h = room.value("height_cm").toDouble(0.0);

    QHash<int, ChannelCalib> channels;
    const QJsonObject chs = root.value("channels").toObject();
    // 웹 UI(1..4) -> 카메라·VMS(0..3). 판정 근거는 keys_are_one_based 참고.
    const int key_offset = keys_are_one_based(chs) ? -1 : 0;
    qInfo().noquote()
        << QString("[Calibration] 채널 키 해석: %1 (오프셋 %2)")
               .arg(key_offset ? QStringLiteral("웹 UI 1-based")
                               : QStringLiteral("카메라 0-based"))
               .arg(key_offset);
    for (auto it = chs.begin(); it != chs.end(); ++it) {
        bool ok = false;
        const int ch = it.key().toInt(&ok) + key_offset;
        if (!ok || ch < 0) continue;
        const QJsonObject c = it.value().toObject();

        ChannelCalib cc;
        cc.present = true;
        cc.frame_w = c.value("frame_w").toInt(0);
        cc.frame_h = c.value("frame_h").toInt(0);
        cc.source = c.value("source").toString();

        const QJsonArray h = c.value("H").toArray();
        const bool has_h = (h.size() == 9);
        if (has_h) {
            for (int i = 0; i < 9; ++i) cc.H[i] = h.at(i).toDouble();
        }

        // 가시영역 다각형 — 웹 UI가 이미 계산해 보낸 꼭짓점(방 좌표 cm).
        // 여기선 그대로 옮겨 담기만 한다(재계산 없음).
        for (const QJsonValue &cv : c.value("coverage_poly").toArray()) {
            const QJsonArray p = cv.toArray();
            if (p.size() >= 2) {
                cc.coverage_poly << QPointF(p.at(0).toDouble(), p.at(1).toDouble());
            }
        }

        // ★ 웹 UI에서 만든 H 가 메타데이터 좌표계(FloorCanvas::FRAME_W/H, 즉
        //   "라이브 카메라 프레임")와 같은 해상도이고 H 행렬 9개가 다 왔을
        //   때만 신뢰한다 — 실시간 발밑점 변환(to_floor)에만 쓰는 조건이다.
        //   하나라도 빠지면 카메라가 다르거나(폰 사진) 데이터가 덜 온 것.
        cc.usable = has_h && cc.frame_w == FloorCanvas::FRAME_W &&
                   cc.frame_h == FloorCanvas::FRAME_H;

        // 가시영역·장애물(정적 기하)은 웹 UI가 "그 사진 좌표계"에서 이미
        // cm 로 계산해 보낸 결과라 라이브 프레임 해상도와 무관하다 — H 만
        // 있으면 믿는다. (usable 하나로 같이 거르면 폰 사진처럼 해상도가
        // 다른 채널의 정확한 가시영역까지 안 그려진다)
        cc.geo_usable = has_h;
        channels.insert(ch, cc);
    }

    // ★ 여기서는 usable 로 거르지 않는다 — force_draw() 로 나중에 봐야 할
    //   수도 있어서다. "그릴지 말지"는 전부 crowd_page(그리는 쪽)의 판단이다.
    QVector<Obstacle> obstacles;
    const QJsonArray obs = root.value("obstacles").toArray();
    for (const QJsonValue &v : obs) {
        const QJsonObject o = v.toObject();
        // 장애물의 channel 도 같은 축이다 — 여기만 빼면 가구가 옆 카메라에 붙는다
        const int ch = o.value("channel").toInt(0) + key_offset;
        if (!channels.contains(ch)) continue;   // channels 에 없는 채널 번호는 오염 데이터

        Obstacle ob;
        ob.channel = ch;
        ob.name = o.value("name").toString();
        for (const QJsonValue &pv : o.value("footprint").toArray()) {
            const QJsonArray p = pv.toArray();
            if (p.size() >= 2) {
                ob.footprint_cm.append(QPointF(p.at(0).toDouble(), p.at(1).toDouble()));
            }
        }
        if (ob.footprint_cm.size() >= 2) obstacles.append(ob);
    }

    // 여기까지 왔으면 유효 — 상태를 통째로 교체한다
    m_room_w_cm = room_w;
    m_room_h_cm = room_h;
    m_channels = channels;
    m_obstacles = obstacles;
    m_last_path = source;
    m_raw = root;
    m_loaded = true;
    m_error.clear();

    emit changed();
    return true;
}

void CalibrationStore::set_force_draw(bool on)
{
    if (m_force_draw == on) return;
    m_force_draw = on;
    QSettings("GuardX", "VMS").setValue("site/calibration_force_draw", on);
    emit changed();   // crowd_page 의 repaint 연결을 그대로 재사용한다
}
