#include "detection_feed.h"
#include "credentials.h"
#include "onvif_meta_source.h"
#include "sunapi_request.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QTimer>
#include <QUrlQuery>
#include <QDebug>
#include <QAuthenticator>
#include <QSet>
#include <QXmlStreamReader>

// 자격정보는 Credentials에서 (소스에 비밀번호를 두지 않는다)
//
// ---- 왜 피드가 둘인가 (2026-08-03) ----
// 사람 트랙(global_id 로 카메라를 넘어 동일인 묶기)은 **test 앱**의 /tracks 에서
// 온다. 그런데 그 응답에는 category 가 1(Human)뿐이고 parent_id 도 없다 —
// 실측으로 확인했다.
//
// 얼굴 블러는 category 2(Face)·3(Head)와 parent_id 가 있어야 동작하는데,
// 그건 **juan_application 앱**의 /detections 에만 실려 온다. 카메라는 매 프레임
// Face/Head 를 사람의 자식 객체로 송출하고 있고(앱의 det_ring_), /detections 가
// 그대로 내보낸다.
//
// /faces 는 쓸 수 없다 — BestShot 이벤트라 실측 분당 1.2건에 최신 항목이 1분
// 가까이 묵는다. 그 좌표로 블러하면 사람이 지나간 자리에 마스크가 남는다.
//
// 그래서 두 엔드포인트를 함께 읽는다. 사람은 tracks, Face/Head 는 detections.
// 나중에 tracks 가 Face/Head 를 함께 내보내게 되면 이쪽 보조 폴링을 지우면 된다.
static const QString FEED_PATH = "/opensdk/test/tracks";
static const QString FACE_PATH = "/opensdk/juan_application/detections";

// 기본 50ms (2026-08-04, 구 200→100→50): 궤적 시간축이 도착 시각이 된 뒤로는
// 폴링 주기가 곧 궤적 해상도이자 박스 신선도다. 레지스트리 detection_poll_ms
// 로 재빌드 없이 조절. ⚠하한 주의: 카메라 HTTP 응답시간(수십 ms)보다 짧으면
// 요청이 겹치고, 카메라 쪽 tracks 갱신 주기보다 짧으면 무의미 — 응답 지연·
// 타임아웃이 보이면 올릴 것.
//
// 0 = 연속 모드 (2026-08-05): 응답 도착 즉시 재요청 — 폴 주기가 타이머가
// 아니라 왕복시간(RTT)이 된다. 카메라는 GET마다 현재 상태로 즉시 응답하므로
// 유선 RTT ~15-30ms에선 30ms 타이머보다 신선하다. 실패 시엔 타이머 틱
// (500ms)까지 쉰다 — 다운된 카메라를 난타하지 않기 위해.
static int poll_interval_ms()
{
    static const int v = QSettings("GuardX", "VMS")
                             .value("detection_poll_ms", 50).toInt();
    return v <= 0 ? 0 : qBound(30, v, 1000);
}


// ⚠ likelihood 임계 필터는 2026-08-05에 **전 경로에서 폐지**했다.
// 원칙: 카메라는 항상 옳다 — WiseAI가 실어 보낸 검출을 VMS가 재검열하지
// 않는다. 0.30 컷을 걸었을 때 경계값에서 출렁이는 객체가 200ms 단위로
// 사라졌다 나타나는 "반짝임"이 됐고, 웹 UI도 필터 없이 전부 그린다.

// 카메라 시각 기준 "지금 화면에 있는 사람" 판정 창
static const int LIVE_WINDOW_MS = 1000;

// 커서를 조금 뒤로 물려 응답 생성 직전 데이터 유실 방지
static const int CURSOR_OVERLAP_MS = 1000;

// "촬영 시각이 미래" 허용치는 box_source.h 의 DETECTION_FUTURE_TOLERANCE_MS.
// 근거(served_utc 초 단위 절삭)는 거기 적어뒀다 — 같은 판정을 하는 곳이
// 여기 말고도 있어서(ONVIF 경로·TrackHistory) 숫자를 한 곳에 모았다.

// ONVIF 푸시가 이 시간 안에 왔던 채널은 HTTP 박스 발행을 멈춘다.
// WiseAI 5Hz 기준 10프레임 여유 — 서브스트림이 죽으면 자동으로 HTTP 복귀.
static const qint64 ONVIF_FRESH_MS = 2000;

// ---- 교차검증 재시작 (2026-08-05 오후, 즉시 재조정) ----
// ⚠ 최초 구현은 "HTTP엔 사람이 있는데 **객체 프레임**이 2초 없다"를 사망으로
// 봤다가 재시작 폭풍을 만들었다(4분에 22회). 오진 원인 둘:
//   ① tt:Frame은 **검출된 객체가 있을 때만** 온다 — HTTP /tracks는 추적기가
//      물고 있어 검출이 잠깐 끊겨도 사람을 계속 보고한다. 정상 세션도 침묵한다.
//   ② 재시작한 세션이 8초 안에 데이터를 못 받으면 무데이터 워치독이 또 죽여
//      되먹임 고리가 된다. 게다가 세션 churn 자체가 카메라를 굶겨 영상까지 흔든다.
// 그래서 생존 신호를 **객체 프레임이 아니라 "아무 문서라도"(이벤트 포함)** 로
// 바꾸고, 임계도 20초로 늘렸다. 이건 기동 굶주림을 못 잡은 세션을 뒤늦게
// 건지는 최후의 그물이지 상시 감시가 아니다.
// ⚠ 실측으로 드러난 진짜 고장 모드는 **반죽음 세션**이다 (08-05 3.6분 관측):
// 연결도 되고 접속 시 이벤트 버스트도 받는데 **분석 메타가 영영 안 흐른다**.
// ch1은 43개 창 중 37개에서 25f/5s 완벽, ch0은 버스트 뒤 2분 침묵 → 사용자가
// 본 "한쪽 채널만 잘 됨"의 정체. 버스트가 생존 판정을 속이므로, 사람이 있는데
// **아무 문서도** 12초 이상 없으면 반죽음으로 보고 되살린다.
// (사람이 없으면 정상 세션도 조용하므로 절대 건드리지 않는다 — 그게 오후의
//  재시작 폭풍을 만든 오진이었다)
static const qint64 ONVIF_DOC_SILENCE_MS = 12000;
static const qint64 ONVIF_POKE_COOLDOWN_MS = 20000;

// 채널별 진단 카운터 (5s 로그) — docs는 이벤트 포함 전체 문서 = 세션 생존
// 지표, frames는 tt:Frame(박스 흐름), unmapped는 대조표에 없던 Human raw id
static QHash<int, int> g_onvif_unmapped;
static QHash<int, int> g_onvif_docs;
static QHash<int, int> g_onvif_frames;
static QHash<int, int> g_onvif_objects;

DetectionFeed *DetectionFeed::instance()
{
    static DetectionFeed feed;
    return &feed;
}

DetectionFeed::DetectionFeed(QObject *parent)
    : QObject(parent)
{
    m_net = new QNetworkAccessManager(this);
    // ⚠ 여기를 다시 끄지 말 것. 이 피드는 200ms마다 카메라 자격증명을 실어
    // 보내는 **가장 트래픽이 많은 경로**다 — 검증을 우회하면 중간자가 그 자격을
    // 그대로 가져간다. (2026-08-03 복구: "SSL errors ignored for test" 로
    // 무조건 통과시키고 있었다)
    //
    // 인증서는 리포에 동봉돼 있어(:/certs/*.pem) 각자 설정할 것이 없다.
    // 카메라를 교체·초기화했으면 PEM 을 갱신해 커밋한다 — docs/SECURITY_SETUP.md §2.
    Credentials::install_tls_pinning(m_net);

    // 카메라가 Digest 인증을 요구한다. 401이 오면 이 시그널이 발생하고,
    // 여기서 자격을 채워주면 Qt가 알아서 재요청한다.
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [](QNetworkReply *reply, QAuthenticator *auth) {
                // 같은 요청에 두 번 불렸다면 자격이 틀린 것 — 무한 재시도 방지
                if (auth->user() == Credentials::camera_user()) {
                    qWarning() << "[DetectionFeed] 인증 거부 — 계정/비밀번호 확인 필요";
                    reply->abort();
                    return;
                }
                auth->setUser(Credentials::camera_user());
                auth->setPassword(Credentials::camera_password());
            });

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this] {
        request_detections();
        request_faces();
    });
    // 연속 모드에선 타이머는 실패 후 재기동용 워치독으로만 남는다
    m_timer->start(poll_interval_ms() > 0 ? poll_interval_ms() : 500);

    request_detections();
    request_faces();

    // ONVIF 메타데이터 전용 세션 (채널당 1개) — 박스의 주 공급원.
    // 실패·부재 시엔 위 HTTP 폴링이 자동으로 계속 공급한다 (emit 루프의
    // ONVIF_FRESH_MS 판정).
    //
    // ⚠기동 스태거 (08-05 실사고): 동시 SETUP+digest 버스트는 카메라 제어
    // 채널에서 확률적으로 세션을 굶긴다 (영상 백엔드의 250ms 스태거와 같은
    // 근거 — 메타 4개를 동시에 띄우자 무작위 채널이 무데이터로 잠겼고,
    // 단독 실행으로는 전 채널 정상 수신 확인). 영상 4세션이 자리 잡는
    // +1.5s 이후 400ms 간격으로 순차 기동한다.
    // 어느 채널에 메타 세션을 열 것인가 — 레지스트리 `onvif_meta_channels`
    // (예: "0,1"). 비우면 4채널 전부.
    //
    // 왜 조정 키가 필요한가 (08-05 실측): 카메라 앞에 사람이 아예 없는 채널이
    // 있다 — 이 현장은 ch2·ch3 센서가 테이블을 향해 **4.4시간 누적 분석 프레임이
    // 24·25개**(ch0·ch1은 3만·4만)다. 그런 채널의 메타 세션은 얻는 것 없이
    // 카메라 세션 자원만 먹고, 그 압박이 다른 채널의 **반죽음 세션**(연결·이벤트
    // 버스트는 오는데 분석 메타가 안 흐름)으로 이어지는 것으로 의심된다.
    const QString ch_csv =
        QSettings("GuardX", "VMS").value("onvif_meta_channels").toString();
    QVector<int> meta_channels;
    for (const QString &tok : ch_csv.split(',', Qt::SkipEmptyParts)) {
        bool ok = false;
        const int v = tok.trimmed().toInt(&ok);
        if (ok && v >= 0 && v < 4)
            meta_channels.append(v);
    }
    if (meta_channels.isEmpty())
        meta_channels = { 0, 1, 2, 3 };

    int slot = 0;
    for (int ch = 0; ch < 4; ++ch) {
        if (!meta_channels.contains(ch)) {
            m_meta_sources.append(nullptr);   // 인덱스 정렬 유지
            continue;
        }
        auto *src = new OnvifMetaSource(ch, this, this);
        m_meta_sources.append(src);   // 교차 검증 재시작용 (handle_reply)
        // ⚠ 6초 뒤부터 600ms 간격 (08-05 실측 조정): 영상 4세션이 먼저
        // 자리를 잡아야 한다. 1.5초에 붙이던 때는 메타 SETUP이 영상 재접속과
        // 겹쳐 ch3 영상이 12초 무프레임 ×4회 끝에야 떴다(검은 타일). 영상이
        // 사용자가 제일 먼저 보는 것이므로 경합에서 우선권을 준다.
        QTimer::singleShot(6000 + slot * 600, src, [src] { src->start(); });
        ++slot;
    }
    qInfo().noquote() << QString("[DetectionFeed] ONVIF 메타 세션 채널: %1")
                             .arg(ch_csv.isEmpty() ? QStringLiteral("0,1,2,3 (기본)")
                                                   : ch_csv);
}

void DetectionFeed::request_faces()
{
    if (m_pending_faces)
        return;

    // 커서가 잡히기 전엔 부르지 않는다. `since` 없이 부르면 카메라가 **링 전체**
    // (10분치·최대 4000행)를 내려주는데, 그게 느린 링크에서 수 초씩 걸려
    // 폴링이 사실상 멈춘다. 커서는 /tracks 응답의 카메라 시계로 씨딩한다
    // (handle_reply 참조) — 우리 PC 시계를 쓰면 카메라와 어긋나 첫 창을 통째로
    // 놓치거나 또 전체를 받는다.
    if (!m_faces_since.isValid())
        return;

    QUrl url = Credentials::camera_base_url();
    url.setPath(FACE_PATH);
    QUrlQuery q;
    q.addQueryItem("since", m_faces_since.toString(Qt::ISODateWithMs));
    url.setQuery(q);

    m_pending_faces = m_net->get(sunapi_request(url));
    connect(m_pending_faces, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_pending_faces;
        m_pending_faces = nullptr;
        const bool ok = (reply->error() == QNetworkReply::NoError);
        handle_faces_reply(reply);
        reply->deleteLater();
        if (poll_interval_ms() == 0 && ok)
            QTimer::singleShot(0, this, [this] { request_faces(); });
    });
}

void DetectionFeed::handle_faces_reply(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "[DetectionFeed] Face 피드 실패:" << reply->errorString();
        return;
    }

    // 파싱 실패를 조용히 삼키지 않는다. 응답이 깨지거나 잘리면 Face/Head 가
    // 통째로 안 들어오는데, 로그가 없으면 "블러가 왜 안 되지"로만 보인다.
    const QByteArray body = reply->readAll();
    QJsonParseError perr{};
    const QJsonObject root = QJsonDocument::fromJson(body, &perr).object();
    if (perr.error != QJsonParseError::NoError) {
        qWarning().noquote()
            << QString("[DetectionFeed] Face 피드 JSON 파싱 실패: %1 "
                       "(offset %2 / %3바이트) — 응답이 잘렸을 수 있다")
                   .arg(perr.errorString()).arg(perr.offset).arg(body.size());
        return;
    }

    const QDateTime served =
        QDateTime::fromString(root["served_utc"].toString(), Qt::ISODateWithMs);
    if (!served.isValid()) {
        qWarning() << "[DetectionFeed] Face 피드 served_utc 없음/이상 —"
                   << body.left(120);
        return;
    }

    const QJsonArray arr = root["detections"].toArray();

    // 시계는 유효한 ts 중 최신값 — /tracks 쪽과 같은 규칙
    QDateTime clock = served;
    for (const QJsonValue &v : arr) {
        const QDateTime ts =
            QDateTime::fromString(v.toObject()["ts"].toString(), Qt::ISODateWithMs);
        if (ts.isValid() && ts <= served.addMSecs(DETECTION_FUTURE_TOLERANCE_MS) && ts > clock)
            clock = ts;
    }

    for (const QJsonValue &v : arr) {
        const QJsonObject obj = v.toObject();

        // 사람(1)은 /tracks 가 준다. 여기선 Face(2)·Head(3)만 취한다 —
        // 둘 다 받으면 같은 사람이 두 박스로 그려진다.
        const int category = obj["category"].toInt(1);
        if (category == 1)
            continue;

        if (!is_valid(obj))
            continue;

        const QDateTime ts =
            QDateTime::fromString(obj["ts"].toString(), Qt::ISODateWithMs);
        if (!ts.isValid() || ts > served.addMSecs(DETECTION_FUTURE_TOLERANCE_MS))
            continue;

        DetectionBox box;
        box.object_id = obj["object_id"].toInt();
        box.sx = obj["rect_sx"].toInt();
        box.sy = obj["rect_sy"].toInt();
        box.ex = obj["rect_ex"].toInt();
        box.ey = obj["rect_ey"].toInt();
        box.ts = ts;
        box.category = category;
        box.parent_id = obj["parent_id"].toInt();
        box.global_id = 0;   // Face/Head 는 동선 추적 대상이 아니다

        // ⚠ 두 앱의 채널 번호 규약이 다르다 (2026-08-03 실측).
        // 같은 사람·같은 순간·같은 좌표를 두 피드가 이렇게 본다:
        //   test 앱 /tracks            : channel 2, raw_channel 1
        //   juan_application /detections: channel 1        (raw 필드 없음)
        // 즉 **detections 의 channel 이 tracks 의 raw_channel 과 같은 축**이다.
        // 여기에 -1 을 하면(옛 /detections 단독 시절 규약) 얼굴만 한 타일 앞으로
        // 밀려, 사람은 타일 1인데 마스크는 타일 0에 그려진다 = "블러가 안 됨".
        const int raw_channel = obj.contains("raw_channel")
                                    ? obj["raw_channel"].toInt()
                                    : obj["channel"].toInt();

        // parent_id 를 사람 박스와 같은 id 공간(display_id)으로 옮긴다.
        // 못 찾으면 원본을 유지한다 — 그 사람이 아직 /tracks 에 안 잡혔거나
        // 방금 사라진 경우다. 그대로 두면 매칭만 실패할 뿐 오작동은 없다.
        if (const auto ch_map = m_raw_to_display.constFind(raw_channel);
            ch_map != m_raw_to_display.cend()) {
            if (const auto it = ch_map->constFind(box.parent_id);
                it != ch_map->cend()) {
                box.parent_id = it.value();
            }
        }

        QHash<int, DetectionBox> &ch_map = m_faces[raw_channel];
        if (!ch_map.contains(box.object_id) || ch_map[box.object_id].ts < ts)
            ch_map[box.object_id] = box;
    }

    // 창을 벗어난 얼굴 제거 — 사람이 사라지면 마스크도 사라져야 한다
    const QDateTime cutoff = clock.addMSecs(-LIVE_WINDOW_MS);
    for (auto ch_it = m_faces.begin(); ch_it != m_faces.end(); ++ch_it) {
        QHash<int, DetectionBox> &ch_map = ch_it.value();
        for (auto it = ch_map.begin(); it != ch_map.end();) {
            if (it.value().ts < cutoff)
                it = ch_map.erase(it);
            else
                ++it;
        }
    }

    m_faces_since = clock.addMSecs(-CURSOR_OVERLAP_MS);

    // 블러가 안 걸릴 때 "카메라가 안 주는 것"인지 "우리가 못 받는 것"인지
    // 구분할 수 있어야 한다. 5초에 한 줄, 채널별 Face/Head 생존 수.
    static qint64 last_log_ms = 0;
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (now_ms - last_log_ms > 5000) {
        last_log_ms = now_ms;
        int face = 0, head = 0;
        for (auto ch_it = m_faces.cbegin(); ch_it != m_faces.cend(); ++ch_it)
            for (const DetectionBox &b : ch_it.value())
                (b.category == 2 ? face : head)++;
        // 사람 수를 같이 찍는다 — 이게 없으면 "얼굴 0"이 빈 화면 탓인지
        // 두 피드가 어긋난 탓인지 로그만 보고는 구분할 수 없다.
        int people = 0;
        for (auto ch_it = m_live.cbegin(); ch_it != m_live.cend(); ++ch_it)
            people += ch_it.value().size();
        qDebug().noquote()
            << QString("[DetectionFeed] people %1 · face %2 head %3 "
                       "(detections 응답 %4행, 커서 %5)")
                   .arg(people).arg(face).arg(head).arg(arr.size())
                   .arg(m_faces_since.toString("HH:mm:ss.zzz"));
    }
}

void DetectionFeed::request_detections()
{
    if (m_pending)
        return;

    // camera_base_url() 을 쓴다 — scheme 을 "http" 로 박아두면 카메라가 매번
    // 301 로 https 에 넘겨서, 200ms 폴링마다 왕복이 한 번씩 더 붙는다
    // (초당 5회 낭비). 핀이 있으면 처음부터 https 로 나간다.
    QUrl url = Credentials::camera_base_url();
    url.setPath(FEED_PATH);

    // 타임아웃이 있어야 m_pending 가드가 반드시 풀린다 — 매달린 reply는
    // finished를 내지 않고, 가드는 그 핸들러에서만 풀린다 (2026-08-10).
    // 오류로 끝나면 연속 모드 자가 재요청은 멈추지만 500ms 워치독 타이머가
    // 폴링을 다시 띄운다 (생성자 §워치독).
    m_pending = m_net->get(sunapi_request(url));
    connect(m_pending, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_pending;
        m_pending = nullptr;
        const bool ok = (reply->error() == QNetworkReply::NoError);
        handle_reply(reply);
        reply->deleteLater();
        if (poll_interval_ms() == 0 && ok)
            QTimer::singleShot(0, this, [this] { request_detections(); });
    });
}

bool DetectionFeed::is_valid(const QJsonObject &obj) const
{
    // BestShot 노이즈: rect가 전부 0인 행 (수신부가 걸러주던 것)
    const int sx = obj["rect_sx"].toInt();
    const int sy = obj["rect_sy"].toInt();
    const int ex = obj["rect_ex"].toInt();
    const int ey = obj["rect_ey"].toInt();
    if (sx == 0 && sy == 0 && ex == 0 && ey == 0)
        return false;

    // 면적이 없는 rect — 판단이 아니라 기하학적 퇴화(그릴 수 없음 + OCC 오염)
    if (ex <= sx || ey <= sy)
        return false;

    return true;
}

void DetectionFeed::handle_reply(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "[DetectionFeed] 요청 실패:" << reply->errorString();
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();

    // 카메라 시계 — 창 판정과 커서 전진의 유일한 기준
    const QDateTime served =
        QDateTime::fromString(root["served_utc"].toString(), Qt::ISODateWithMs);
    if (!served.isValid()) {
        qDebug() << "[DetectionFeed] served_utc 파싱 실패";
        return;
    }

    const QJsonArray arr = root["tracks"].toArray();
    // ★★★ 여기 ★★★ — 유효한 ts 중 최신값을 시계로 삼는다
    QDateTime clock = served;
    for (const QJsonValue &v : arr) {
        const QDateTime ts =
            QDateTime::fromString(v.toObject()["ts"].toString(), Qt::ISODateWithMs);
        if (ts.isValid() && ts <= served.addMSecs(DETECTION_FUTURE_TOLERANCE_MS) && ts > clock)
            clock = ts;
    }

    int bogus = 0;

    for (const QJsonValue &v : arr) {
        const QJsonObject obj = v.toObject();
        if (!is_valid(obj))
            continue;

        const QDateTime ts =
            QDateTime::fromString(obj["ts"].toString(), Qt::ISODateWithMs);
        if (!ts.isValid())
            continue;

        // 감지 시각이 응답 시각보다 미래일 수 없다.
        // 손상된 행을 여기서 끊어야 커서와 화면이 오염되지 않는다.
        if (ts > served.addMSecs(DETECTION_FUTURE_TOLERANCE_MS)) {
            ++bogus;
            continue;
        }

        DetectionBox box;
        // VMS 내부 표시/선택 기준을 display_id로 통일.
        // 즉 화면에 보이는 P-123의 123은 CAP display_id.
        const int display_id = obj.contains("display_id")
                                   ? obj["display_id"].toInt()
                                   : obj["global_id"].toInt();

        box.object_id = display_id;
        box.sx = obj["rect_sx"].toInt();
        box.sy = obj["rect_sy"].toInt();
        box.ex = obj["rect_ex"].toInt();
        box.ey = obj["rect_ey"].toInt();
        box.ts = ts;
        box.category = obj.contains("category") ? obj["category"].toInt() : 1;
        box.parent_id = 0;
        box.global_id = display_id;
        box.vx = obj["velocity_x"].toDouble();
        box.vy = obj["velocity_y"].toDouble();
        box.lost_ms = qMax(0, obj["lost_ms"].toInt());

        const int raw_channel = obj.contains("raw_channel")
                                    ? obj["raw_channel"].toInt()
                                    : obj["channel"].toInt() - 1;

        // Face/Head 의 parent_id(카메라 원본 object_id)를 display_id 로 옮기기
        // 위한 대조표. 두 피드의 id 공간이 다른 것을 여기서 이어 붙인다.
        if (const int raw_id = obj["object_id"].toInt(); raw_id > 0)
            m_raw_to_display[raw_channel][raw_id] = display_id;

        QHash<int, DetectionBox> &ch_map = m_live[raw_channel];
        if (!ch_map.contains(box.object_id) || ch_map[box.object_id].ts < ts)
            ch_map[box.object_id] = box;
    }

    // 창을 벗어난 객체 제거 = 사라진 사람의 박스 소멸
    const QDateTime cutoff = clock.addMSecs(-LIVE_WINDOW_MS);
    for (auto ch_it = m_live.begin(); ch_it != m_live.end(); ++ch_it) {
        QHash<int, DetectionBox> &ch_map = ch_it.value();
        for (auto it = ch_map.begin(); it != ch_map.end();) {
            if (it.value().ts < cutoff)
                it = ch_map.erase(it);
            else
                ++it;
        }
    }

    // 사라진 사람의 대조표 항목도 정리한다 — 안 그러면 raw id 가 재사용될 때
    // 옛 display_id 로 잘못 이어붙고, 장시간 돌면 계속 커진다.
    for (auto ch_it = m_raw_to_display.begin(); ch_it != m_raw_to_display.end();
         ++ch_it) {
        const QHash<int, DetectionBox> &live = m_live.value(ch_it.key());
        QSet<int> alive;
        for (const DetectionBox &b : live)
            alive.insert(b.object_id);
        QHash<int, int> &map = ch_it.value();
        for (auto it = map.begin(); it != map.end();) {
            if (!alive.contains(it.value()))
                it = map.erase(it);
            else
                ++it;
        }
    }

    // 커서는 데이터가 아닌 카메라 시계로 전진시킨다
    m_since = clock.addMSecs(-CURSOR_OVERLAP_MS);

    // 실효 폴 속도 — 연속 모드(detection_poll_ms=0) A/B의 판정 지표.
    // 타이머 모드면 ~1000/주기 req/s, 연속 모드면 1000/RTT req/s가 나와야 한다.
    {
        static qint64 rate_t0 = 0;
        static int rate_n = 0;
        ++rate_n;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        if (rate_t0 == 0)
            rate_t0 = now_ms;
        if (now_ms - rate_t0 >= 5000) {
            qDebug().noquote()
                << QString("[DetectionFeed] tracks 폴 %1 req/s (연속모드 %2)")
                       .arg(rate_n * 1000.0 / (now_ms - rate_t0), 0, 'f', 1)
                       .arg(poll_interval_ms() == 0 ? "ON" : "OFF");
            rate_t0 = now_ms;
            rate_n = 0;
        }
    }

    // Face 피드 커서 씨딩 — 카메라 시계 기준이라 여기서만 잡을 수 있다.
    // 이게 없으면 첫 /detections 요청이 링 전체를 끌어온다 (request_faces 주석).
    if (!m_faces_since.isValid())
        m_faces_since = clock.addMSecs(-LIVE_WINDOW_MS);
    /*
    qDebug() << "[DetectionFeed] 수신" << arr.size()
             << "| 손상" << bogus
             << "| served" << served.toString("HH:mm:ss.zzz")
             << "| 생존" << m_live.value(1).size() << "(ch1)";
*/
    // 빈 채널도 emit해야 마지막 박스가 화면에 남지 않는다.
    // 사람(/tracks)과 Face/Head(/detections)를 한 벡터로 합쳐 내보낸다 —
    // 수신부(channel_view)는 category 로만 구분하므로 출처를 알 필요가 없다.
    // 단 ONVIF 서브스트림이 신선한 채널은 건너뛴다 — 그쪽이 록스텝(-4ms)이고
    // 이쪽은 300-400ms 묵은 좌표라, 섞이면 박스가 앞뒤로 널뛴다.
    const qint64 emit_now = QDateTime::currentMSecsSinceEpoch();
    for (int ch = 0; ch < 4; ++ch) {
        if (emit_now - m_onvif_last_ms.value(ch, 0) < ONVIF_FRESH_MS)
            continue;

        // 교차 검증 — 상단 주석의 재조정판. 판정 기준은 **아무 문서도 안 옴**
        // (이벤트 포함). 사람이 없어 객체 프레임만 없는 정상 세션은 건드리지
        // 않는다. 한 번도 문서를 못 받은 채널(doc_ms==0)은 기동 워치독 소관이라
        // 여기선 제외 — 두 그물이 같은 세션을 번갈아 죽이던 고리를 끊는다.
        const qint64 doc_ms = m_onvif_doc_ms.value(ch, 0);
        if (doc_ms > 0 && !m_live.value(ch).isEmpty()
            && emit_now - doc_ms > ONVIF_DOC_SILENCE_MS
            && emit_now - m_onvif_poke_ms.value(ch, 0) > ONVIF_POKE_COOLDOWN_MS
            && ch < m_meta_sources.size() && m_meta_sources[ch]) {
            m_onvif_poke_ms[ch] = emit_now;
            m_meta_sources[ch]->poke_restart(
                QString("문서 %1초 두절 (사람은 있음)")
                    .arg((emit_now - doc_ms) / 1000));
        }

        QVector<DetectionBox> boxes = m_live.value(ch).values().toVector();
        boxes += m_faces.value(ch).values().toVector();
        emit detections_arrived(ch, boxes, -1);
    }

    // handle_reply 끝부분
    static QDateTime prev_newest;
    QDateTime newest;
    for (const QJsonValue &v : arr) {
        const QDateTime ts = QDateTime::fromString(v.toObject()["ts"].toString(), Qt::ISODateWithMs);
        if (ts.isValid() && ts <= served && (!newest.isValid() || ts > newest))
            newest = ts;
    }
    /*
   if (prev_newest.isValid() && newest.isValid()) {
        qDebug() << "[Feed] 소스간격" << prev_newest.msecsTo(newest) << "ms"
                 << "| lag" << newest.msecsTo(served) << "ms";
    }
*/
    prev_newest = newest;
}

// -------------------------------------------------- ONVIF 메타데이터 서브스트림

void DetectionFeed::ingest_onvif(int channel, const QByteArray &xml)
{
    // depay가 문서를 조각내 줄 가능성 방어 — 문서 종료 태그 기준으로 조립.
    // (실측에선 marker 단위 완성 문서가 왔지만, 가정하지 않는다)
    static const QByteArray END_TAG = "</tt:MetadataStream>";
    // 채널별 첫 수신 확인 — pad 연결→depay→appsink→GUI 전달의 관통 증거
    static QSet<int> s_seen;
    if (!s_seen.contains(channel)) {
        s_seen.insert(channel);
        qInfo() << "[DetectionFeed] ch" << channel << "첫 ONVIF 문서"
                << xml.size() << "B";
    }
    // 생존 신호 — 이벤트 문서든 객체 프레임이든 "왔다"는 사실만 기록한다
    m_onvif_doc_ms[channel] = QDateTime::currentMSecsSinceEpoch();

    QByteArray &acc = m_onvif_buf[channel];
    acc += xml;
    report_onvif_stats();
    for (;;) {
        const int end = acc.indexOf(END_TAG);
        if (end < 0) {
            if (acc.size() > (1 << 20)) {  // 종료 태그 없이 1MB = 오염 — 리셋
                qWarning() << "[DetectionFeed] ch" << channel
                           << "ONVIF 조립 버퍼 오염 — 버림";
                acc.clear();
            }
            return;
        }
        const int cut = end + END_TAG.size();
        ++g_onvif_docs[channel];
        process_onvif_doc(channel, acc.left(cut));
        acc.remove(0, cut);
    }
}

void DetectionFeed::report_onvif_stats()
{
    // 5초에 한 줄, 채널별. d=전체 문서(이벤트 포함 — 세션이 살아있다는 증거),
    // f=박스 프레임, b=박스 수. "한 채널만 잘 된다"는 비대칭은 여기서
    // d가 0인 채널(세션 사망)과 f만 0인 채널(사람 없음)로 갈린다.
    static qint64 s_log_ms = 0;
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (now_ms - s_log_ms < 5000)
        return;
    s_log_ms = now_ms;
    QStringList parts;
    for (int ch = 0; ch < 4; ++ch)
        if (g_onvif_docs.value(ch) || g_onvif_frames.value(ch))
            parts << QString("ch%1 %2d/%3f/%4b(미매핑 %5)")
                         .arg(ch).arg(g_onvif_docs.value(ch))
                         .arg(g_onvif_frames.value(ch))
                         .arg(g_onvif_objects.value(ch))
                         .arg(g_onvif_unmapped.value(ch));
    if (!parts.isEmpty())
        qDebug().noquote()
            << QString("[DetectionFeed] ONVIF 5s: %1")
                   .arg(parts.join(QStringLiteral(" · ")));
    g_onvif_docs.clear();
    g_onvif_frames.clear();
    g_onvif_objects.clear();
    g_onvif_unmapped.clear();
}

void DetectionFeed::process_onvif_doc(int channel, const QByteArray &doc)
{
    // tt:MetadataStream 안에서 우리가 쓰는 건 VideoAnalytics의 tt:Frame뿐.
    // (tt:Event 문서 = AudioDetection 등 — 박스 아님, 신선도 판정에도 불포함)
    QVector<DetectionBox> boxes;
    bool has_frame = false;
    QDateTime frame_ts;
    double src_w = 0, src_h = 0;   // Transformation 역산 좌표공간 (0 = 미상)

    QXmlStreamReader r(doc);
    while (!r.atEnd()) {
        if (r.readNext() != QXmlStreamReader::StartElement)
            continue;
        const auto name = r.name();

        if (name == QLatin1String("Frame")) {
            has_frame = true;
            frame_ts = QDateTime::fromString(
                r.attributes().value("UtcTime").toString(), Qt::ISODateWithMs);
            // 미래로 튄 UtcTime 은 **없는 것으로 친다.** HTTP 경로는 같은
            // 판정을 카메라가 준 served_utc 로 하지만 여기엔 카메라측 기준
            // 시각이 없어 PC 시계와 대조할 수밖에 없다 — 그래서 허용치가
            // 다르다(DETECTION_PC_CLOCK_SKEW_MS, 근거는 box_source.h).
            //
            // 버리지 않고 무효화하는 이유: 아래 두 갈래가 이미 "프레임 시각
            // 모름"을 처리한다 — box.ts 는 도착 시각으로, 발행은 -1(프레임
            // 귀속 없음)로 간다. 박스 자체는 멀쩡하므로 계속 그린다
            // ("카메라는 항상 옳다" — 좌표는 믿고, 불가능한 시각만 안 믿는다).
            // 반대로 문서를 통째로 버리면 PC 시계가 뒤처진 것만으로 박스가
            // 전멸한다.
            if (frame_ts.isValid()
                && frame_ts > QDateTime::currentDateTimeUtc().addMSecs(
                                  DETECTION_PC_CLOCK_SKEW_MS)) {
                // 상시 발생하면 손상이 아니라 **시계 어긋남**이다 — 델타를
                // 찍어 둘을 구분할 수 있게 한다(초 단위 = 시계, 분 단위 = 손상).
                static QHash<int, qint64> s_warn_ms;
                const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
                if (now_ms - s_warn_ms.value(channel, 0) > 10000) {
                    s_warn_ms[channel] = now_ms;
                    qWarning().noquote()
                        << QString("[DetectionFeed] ch%1 ONVIF UtcTime 이 "
                                   "미래 (+%2ms) — 프레임 시각 무시, 도착 "
                                   "시각으로 대체. 상시 발생하면 카메라/PC "
                                   "시계를 맞출 것")
                               .arg(channel)
                               .arg(frame_ts.toMSecsSinceEpoch() - now_ms);
                }
                frame_ts = QDateTime();
            }
            continue;
        }
        if (name == QLatin1String("Scale") && has_frame) {
            // ONVIF Transformation: scale ≈ 2/프레임크기. 역산해 좌표를
            // 계약 공간(2592x1520)으로 정규화 — 웹 UI가 Translate/Scale로
            // 정규화하는 것의 등가물 (프로파일별 좌표공간 변동 방어)
            const double tx = qAbs(r.attributes().value("x").toDouble());
            const double ty = qAbs(r.attributes().value("y").toDouble());
            if (tx > 0 && ty > 0) {
                src_w = 2.0 / tx;
                src_h = 2.0 / ty;
            }
            continue;
        }
        if (name != QLatin1String("Object") || !has_frame)
            continue;

        // ---- tt:Object 하나 소비 ----
        const int raw_id = r.attributes().value("ObjectId").toInt();
        const int raw_parent = r.attributes().value("Parent").toInt();
        double sx = 0, sy = 0, ex = 0, ey = 0, likelihood = 0;
        QString type;
        int depth = 1;
        bool in_candidate = false;
        while (depth > 0 && !r.atEnd()) {
            const auto tok = r.readNext();
            if (tok == QXmlStreamReader::StartElement) {
                const auto n = r.name();
                if (n == QLatin1String("Object")) {
                    ++depth;
                } else if (n == QLatin1String("BoundingBox")) {
                    // 픽셀 좌표(2592x1520) 그대로 — HTTP 피드와 같은 공간 실측
                    const auto at = r.attributes();
                    sx = at.value("left").toDouble();
                    sy = at.value("top").toDouble();
                    ex = at.value("right").toDouble();
                    ey = at.value("bottom").toDouble();
                } else if (n == QLatin1String("ClassCandidate")) {
                    in_candidate = true;
                } else if (n == QLatin1String("Type") && !in_candidate) {
                    // 확정 분류는 ClassCandidate 밖의 tt:Type 하나뿐
                    likelihood = r.attributes().value("Likelihood").toDouble();
                    type = r.readElementText();
                }
            } else if (tok == QXmlStreamReader::EndElement) {
                const auto n = r.name();
                if (n == QLatin1String("Object"))
                    --depth;
                else if (n == QLatin1String("ClassCandidate"))
                    in_candidate = false;
            }
        }

        const int category = type == QLatin1String("Human") ? 1
                           : type == QLatin1String("Face")  ? 2
                           : type == QLatin1String("Head")  ? 3 : 0;
        if (category == 0)
            continue;                      // Vehicle 등 미표시 분류
        if (ex <= sx || ey <= sy)
            continue;                      // ImageRef 부속 행 등 무면적
        // likelihood 필터 없음 — 카메라는 항상 옳다 (파일 상단 원칙 주석)
        Q_UNUSED(likelihood);

        // 좌표공간이 계약(2592x1520)과 다르면 비례 변환 (실측 기본은 동일)
        double kx = 1.0, ky = 1.0;
        if (src_w > 1 && src_h > 1) {
            kx = 2592.0 / src_w;
            ky = 1520.0 / src_h;
        }

        DetectionBox box;
        box.sx = int(sx * kx);
        box.sy = int(sy * ky);
        box.ex = int(ex * kx);
        box.ey = int(ey * ky);
        box.ts = frame_ts.isValid() ? frame_ts
                                    : QDateTime::currentDateTimeUtc();
        box.category = category;
        // id 공간 통일: 사람은 display_id(재식별)로 옮겨야 P-태그·우클릭
        // 추적·동선 패널이 기존과 같은 축에서 돈다. 대조표는 /tracks 폴링이
        // 저주기로 계속 채운다 — 새 인물은 다음 폴까지(≤1s) raw id로 보인다.
        const auto &map = m_raw_to_display[channel];
        if (category == 1) {
            // 대조표 미비(새 인물·handover 직후 raw id 교체)면 raw로 표시 —
            // 다음 /tracks 폴(≤1s)에 display_id로 바뀐다. 이 깜빡임 빈도를
            // 5s 로그의 unmapped로 센다 (handover 이상 현상 추적용)
            if (!map.contains(raw_id))
                ++g_onvif_unmapped[channel];
            box.object_id = map.value(raw_id, raw_id);
            box.global_id = box.object_id;
            box.parent_id = 0;
        } else {
            box.object_id = raw_id;
            box.parent_id = map.value(raw_parent, raw_parent);
            box.global_id = 0;
        }
        boxes.append(box);
    }

    if (r.hasError() && !has_frame)
        return;                            // 깨진 이벤트 문서 — 조용히 무시
    if (!has_frame)
        return;                            // 이벤트 문서 — 박스 경로와 무관

    m_onvif_last_ms[channel] = QDateTime::currentMSecsSinceEpoch();
    emit detections_arrived(channel, boxes,
                            frame_ts.isValid() ? frame_ts.toMSecsSinceEpoch()
                                               : -1);

    // 채널별 카운터만 갱신 — 5초 리포트는 ingest_onvif가 담당한다
    // (이벤트 문서까지 세어 세션 생존을 보이려면 그쪽이어야 한다)
    ++g_onvif_frames[channel];
    g_onvif_objects[channel] += boxes.size();
}