# VMS 코드 지도 — 각 클래스가 무엇을 하고, 무엇을 고치면 되는가

> 대상: VMS(Qt) 작업자 · 기능을 추가하려는 사람
> 기준: 태그 `mqtt-v9`
> 관련: `DB_LINK_AND_MQTT_MIGRATION.md`(MQTT 설계) · `REMAINING_STEPS.md`(남은 일)
> 작성: 2026-07-29

---

## 0. 먼저 알아야 할 것 — 데이터 경로가 둘이다

VMS 화면의 숫자는 **출처가 두 군데**다. 이걸 모르면 "어디를 고쳐야 하나"를 못 찾는다.

```
① 카메라 직결 (실시간, DB 무관)
   [카메라 OpenSDK] --HTTP 200ms--> DetectionFeed --> BoxSource --> ChannelView
                                                                      └─ 박스 그림
                                                                      └─ OCC 분자(인원수)

② MQTT (RPi B 경유)
   [Postgres] --> [RPi B 폴러] --MQTT--> MqttLink --> ZoneConfig   (OCC 분모=정원)
                                                  --> CrowdPage    (히트맵·날짜)
                                                  --> ZoneSettings (정원 편집)
```

| 화면 요소 | 출처 | 파일 |
|---|---|---|
| 바운딩 박스 | **① 카메라** | `detection_feed.cpp` |
| `OCC 3/30` 의 **3** (현재 인원) | **① 카메라** | `channel_view.h:occupancy()` |
| `OCC 3/30` 의 **30** (정원) | **② MQTT** | `zone_config.cpp` |
| **+5분 예측 틱 (실모델)** | **① 카메라** | `prediction_feed.cpp` (07-30 — 선형외삽 자리표시자 대체) |
| **REPORT 페이지 전체** | **① 카메라** | `report_page.cpp` (07-30 — placeholder 실구현) |
| 히트맵 | **② MQTT** | `crowd_page.cpp` |
| **혼잡 경보 (LIVE 색·ALERT·REPORT 예측 이벤트)** | **② MQTT** | `alert_feed.cpp` (부록 B) |
| 영상 | RTSP 직결 | `live_viewer.cpp:stream_url()` |

**VMS는 Postgres에 직접 붙지 않는다** (v6에서 제거). DB가 필요한 건 전부 ②를 탄다.

---

## 1. 자주 찾는 것 — 바로가기

| 궁금한 것 | 어디 |
|---|---|
| **박스 좌표를 어디서 받아오나** | `detection_feed.cpp:15` `FEED_PATH` |
| **박스 폴링 주기** | `detection_feed.cpp:17` `POLL_INTERVAL_MS = 200` |
| **RTSP 영상 URL** | `live_viewer.cpp` `stream_url()` |
| **profile5(해상도)를 어디서 정하나** | `live_viewer.cpp` `sub_profile()` / `main_profile()` |
| **화면 속 인구수 세는 로직** | `channel_view.h` `occupancy()` |
| **정원(분모)** | `zone_config.cpp` — MQTT `guardx/db/rpib/zones` |
| **채널 이름 (CH1 · LOBBY EAST)** | `theme.cpp:72` `channel_name()` |
| **정원 기본값 (MQTT 수신 전)** | `theme.cpp:82` `channel_cap()` = `{60,80,40,30}` |
| **카메라 IP·계정** | `credentials.cpp` — `%LOCALAPPDATA%\GuardX\credentials.ini` |
| **TLS 인증서 핀 설정 (PC마다 1회)** | **[`SECURITY_SETUP.md`](SECURITY_SETUP.md)** — `SSL handshake failed` 뜨면 여기 |
| **브로커 주소** | 같은 파일 `[mqtt] host` |
| **원본 해상도 2592×1520** | `channel_view.h:171` / `crowd_page.h` `FRAME_W/H` |
| **예측(p50/p10/p90/P초과)을 어디서 받나** | `prediction_feed.cpp:14` `PRED_PATH` — 카메라 직결, 60초 |
| **예측에 capacity 주입 (v16)** | `prediction_feed.cpp` `set_capacity()` — `?capacity=N` |
| **REPORT 페이지 데이터 원천** | `report_page.cpp` `refresh_all()` — /prediction·/occupancy·/events·/stats |
| **로그인·역할·세션** | `auth.cpp` — 화면은 `login_page.cpp`, 게이트는 `mainwindow.cpp` `go_to()` (부록 G) |
| **"이 버튼이 왜 꺼져 있나"** | `Auth::can(Action)` — 사유 문구는 `Auth::deny_reason()` |
| **브로커 mTLS 설정** | `credentials.ini [mqtt] tls_*` · 배선은 `mqtt_link.cpp` (부록 G-6) |
| **RTP 방송 목적지** | `broadcast_controller.cpp` `resolve_rtp_host()` — endpoints→캐시→상수 |

---

## 2. 클래스별 역할

### 2-1. 데이터 공급 (화면 아님)

#### `DetectionFeed` — 카메라 감지 수신기 (싱글턴)
`detection_feed.{h,cpp}` · 46+211줄

카메라 OpenSDK 엔드포인트 **하나가 전 채널 감지를 함께 내려준다.** 그래서
채널마다 따로 받지 않고 여기서 한 번만 받아 나눠 뿌린다.

```cpp
static const QString FEED_PATH = "/opensdk/juan_application/detections";
static const int POLL_INTERVAL_MS = 200;     // 5Hz
static const double MIN_LIKELIHOOD = 0.30;   // 신뢰도 컷
static const int LIVE_WINDOW_MS = 1000;      // 이보다 오래된 객체는 제거 = 박스 소멸
```

| 하는 일 | 설명 |
|---|---|
| HTTP Digest 인증 | 401이 오면 `authenticationRequired` 시그널에서 자격 주입 |
| 커서 전진 | `?since=` — **카메라 시계 기준**, 1초 겹침 |
| 노이즈 필터 | rect 전부 0(BestShot)·면적 0 만 제외. ⚠ **`likelihood` 컷은 없다** — 2026-08-05에 전 경로 폐지("카메라는 항상 옳다"). 0.30 컷은 경계값 객체를 200ms 단위로 깜빡이게 만들던 주범이었고 웹 UI도 필터 없이 전부 그린다 (`detection_feed.cpp:57` 원칙 주석) |
| 미래 시각 방어 | 감지 `ts` > `served_utc` + 5초면 그 행을 버린다. **카메라끼리 비교**라 좁게 잡을 수 있다 — ONVIF 경로는 PC 시계와 대조해야 해서 허용치가 다르다(부록 F-2) |
| 생존창 | 최신 감지 -1000ms 보다 오래된 객체 삭제 → 사라진 사람의 박스가 지워진다 |
| 빈 채널도 emit | 안 그러면 마지막 박스가 화면에 박제된다 |

```cpp
signals:
    void detections_arrived(int channel, const QVector<DetectionBox> &boxes);
```

> **이 경로는 MQTT 전환 범위 밖이었다.** DB가 죽어도 박스는 그대로 나온다.

#### `BoxSource` — 채널 필터
`box_source.{h,cpp}` · 47+12줄

`DetectionFeed` 의 전 채널 신호에서 **자기 채널만 골라** 다시 emit 한다.
12줄짜리 얇은 어댑터. `ChannelView` 가 채널 번호를 신경 쓰지 않게 하는 역할.

`DetectionBox` 구조체가 여기 정의돼 있다:
```cpp
struct DetectionBox {
    int object_id;              // ★ 추적 기능의 열쇠
    int sx, sy, ex, ey;         // 카메라 원본(2592x1520) 좌표
    QDateTime ts;
    int category = 1;           // 1=Human, 2=Face, 3=Head
    int parent_id = 0;          // Face/Head가 속한 사람의 object_id
};
```

#### `MqttLink` — 브로커 연결 (싱글턴)
`mqtt_link.{h,cpp}` · 101+302줄

```cpp
void subscribe(const QString &topic, Handler handler, int qos = 1);
bool publish(const QString &topic, const QByteArray &payload, int qos = 1, bool retain = false);
QString client_id() const;    // "vms-{hostname}"
```

**스레드를 만들지 않는다.** `QTimer` 20ms로 `mosquitto_loop()` 를 GUI 스레드에서
논블로킹 호출하므로 **콜백도 GUI 스레드에 떨어진다** — 위젯을 건드리는 핸들러를
마샬링 없이 그대로 쓸 수 있다.

같은 토픽에 핸들러를 **여러 개 등록해도 된다** — 전부 호출된다
(`ZoneConfig` 와 `ZoneSettingsPage` 가 `zones` 를 함께 구독한다).

#### `ZoneConfig` — 정원/임계 캐시 (네임스페이스)
`zone_config.{h,cpp}` · 49+130줄

`guardx/db/rpib/zones` 를 구독해 채널별로 캐시한다.

```cpp
void init();                        // 구독 등록만. 값은 나중에 온다
int capacity(int channel);          // OCC 분모
double warn_ratio(int channel);
double critical_ratio(int channel);
```

메시지가 안 오면 `Theme::channel_cap()` 기본값으로 폴백한다(fail-soft).

#### `Credentials` — 설정 로드 (네임스페이스)
`credentials.{h,cpp}` · 76+328줄

`%LOCALAPPDATA%\GuardX\credentials.ini` 또는 환경변수에서 읽는다.
**`[database]` 는 v6에서 제거됐다.** 지금은 `[camera]` 와 `[mqtt]` 뿐.

Windows DPAPI 암호화(`dpapi:` 접두), 카메라 TLS 인증서 지문 고정도 여기.

---

### 2-2. 화면 (페이지)

`MainWindow` 가 `QStackedWidget` 에 6개를 넣고 `NavRail` 이 전환한다.

| # | 메뉴 | 클래스 | 상태 |
|---|---|---|---|
| 0 | LIVE | `LiveViewer` | 실제 (경보 배지가 붙는 버튼) |
| 1 | CROWD | `CrowdPage` | 실제 |
| 2 | REPORT | `ReportPage` | **실제 (07-30)** — 부록 A 참조 |
| 3 | DEVICE | `DeviceControlPage` | **실제 (08-06 VEDA-174)** — 부록 E 참조 |
| 4 | CAMERA | `CameraPage` | **실제 (08-06)** — 부록 C 참조 |
| 5 | SETTINGS | `ZoneSettingsPage` | 실제 — 화면 테마 + 구역 정원/임계 + 화재 임계 |

⚠ 인덱스는 `nav_rail.cpp`의 `items[]`와 `mainwindow.cpp`의 `addWidget` 순서가
**같아야** 성립한다. 한쪽만 고치면 버튼과 화면이 어긋난다.
CAMERA 인덱스는 `NavRail::CAMERA_INDEX` 상수로 고정돼 있다(top_bar의 APPS pill
클릭 점프가 참조).

> **표 정정 이력**: TRACK 탭은 07-30 병합으로 사라졌다(동선은 LIVE 우측
> `TrackingPanel`로 들어감). 08-06에 CAMERA가 DEVICE와 ZONES 사이로 들어오며
> ZONES가 4→5로 밀렸다. 08-07 VEDA-176 머지에서 DEVICE가 placeholder에서
> 실물 `DeviceControlPage`로 바뀌고, ZONES는 화재 임계까지 품으면서
> **SETTINGS**로 이름이 바뀌었다.

혼잡 경보는 **화면(탭)이 아니다** — 떠 있는 `AlertPopup` + LIVE 타일 색이
맡는다 (부록 B). 어느 화면을 보고 있든 알려야 하는 것이라 탭에 넣으면
정작 봐야 할 때 못 본다.

#### `LiveViewer` — 4채널 그리드
`live_viewer.{h,cpp}` · 97+698줄

- `ChannelView` 4개 배치 — **고정 2×2 그리드** (08-19: FOCUS/전체화면 모드·타일 클릭 확대 삭제)
- **RTSP URL 생성** ← 아래 3-2 참조
- 재생 지연 보정 (`+`/`-` 키, 0~3000ms)
- `update_occupancy_stats()` — 60초 샘플로 유입 속도 계산. +5분 예측은
  **실모델 우선**(`PredictionFeed`, 07-30) / warmup·피드다운 시 외삽 폴백
- `CameraTuner` 로 인코더를 저지연 설정으로 강제

#### `CrowdPage` — 평면도 히트맵
`crowd_page.{h,cpp}` · 193+696줄 (가장 큼)

**날짜 단위 캐싱** 구조 (v5):
```cpp
QHash<QDate, QVector<DayCell>> m_day_cache;   // 날짜 → 하루치 집계
QHash<QString, QDate> m_inflight;             // req_id → 요청 중인 날짜
```

- 날짜 클릭 → `query/heatday` 요청 1회 → 캐시
- 슬라이더·프리셋·다중선택 → **전부 캐시 위 계산, 네트워크 0**
- `FloorCanvas` 가 카메라 화면 좌표를 평면도로 투영(`to_floor`)

#### `ZoneSettingsPage` — 정원 편집 (v9)
`zone_settings_page.{h,cpp}` · 67+305줄

`cmd/set_zone` 을 발행해 RPi B가 DB를 갱신하게 한다.
**요청-응답 + 편집 중 보호**의 참고 구현으로 쓸 만하다.

---

### 2-3. 채널 하나 (`ChannelView`)

`channel_view.{h,cpp}` · 205+538줄

한 채널의 **영상 + 오버레이 + OSD** 를 담당한다.

```
ChannelView
├─ VideoBackend (영상)   ← GStreamer / FFmpeg / QMediaPlayer 중 택1
└─ BoxOverlay (그 위)    ← 박스·태그·OCC 배지·블러
```

#### 인구수 세는 로직 ★

```cpp
// channel_view.h:101
int occupancy() const
{
    int n = 0;
    for (const AnimatedBox &box : m_anim_boxes)
        n += (box.category == 1);      // ★ Human 만 센다
    return n;
}
```

**`category == 1`(Human)만 센다.** Face(2)·Head(3)를 함께 세면 같은 사람이
2~3번 세어져 점유율이 증폭된다.

`m_anim_boxes` 는 **보간된 표시용 박스** 목록이다 — 실제 감지(`m_tracks`)를
재생 지연만큼 과거 시점으로 되돌려 부드럽게 움직이게 만든 결과.

#### 그 밖에

| 기능 | 설명 |
|---|---|
| 재생 지연 보간 | 감지가 5Hz라 그대로 그리면 툭툭 끊긴다. 프레임마다 목표로 미끄러뜨림 |
| Face/Head 중복 제거 | 같은 사람에 둘 다 있으면 더 넓은 Head를 우선 |
| 얼굴 블러 | `category` 2·3 영역을 가림 |
| 박스 클릭 | `object_selected(db_channel, object_id, cam_rect)` emit ← **추적의 진입점** |

---

### 2-4. 영상 백엔드 (교체 가능)

`video_backend.h` 가 인터페이스, 세 구현이 있다.

```cpp
class VideoBackend {
    virtual QWidget *create_widget(QWidget *parent) = 0;
    virtual void play(const QUrl &url) = 0;
    virtual qint64 current_frame_pts_ms() const = 0;
    virtual qint64 glass_latency_ms() const;
};
```

| 구현 | 파일 | 특징 |
|---|---|---|
| `GStreamerBackend` | `gstreamer_backend.*` | GPU 상주. `RhiVideoWidget` + NV12 셰이더 |
| `FfmpegBackend` | `ffmpeg_backend.*` | libav 직접 디코드 — **미사용, 손대지 말 것** (아래) |
| `QMediaPlayerBackend` | `qmediaplayer_backend.*` | 안전망. 지연 1초+ |

> 🚫 **`video_backend=ffmpeg` 를 켜지 마라 (2026-08-03 코드 확인).** 데뮉스 옵션은
> GStreamer 경로와 동급인데(`max_delay=0`·`nobuffer`·`low_delay`·`reorder_queue_size=0`)
> 그 뒤가 완전히 다른 세대다. 현재 워크로드(800×448)는 버티지만 1080p에서 무너진다.
> 실제로 아무도 안 쓰므로 **고치지 않고 남겨둔다** — 쓰지 않는 것이 유일한 대책이다.
>
> | 문제 | 위치 | 비용 |
> |---|---|---|
> | 하드웨어 디코딩 없음 — `hw_device_ctx`·`get_format` 둘 다 없음 | `:150-159` | 순수 SW |
> | `sws_scale` CPU 색변환(BGRA) | `:277` | 1080p당 3~6ms × 4ch |
> | `rgb.copy()` 프레임마다 전체 딥카피 | `:287` | 1080p: 8.3MB×30×4 ≈ **1 GB/s memcpy** |
> | `stimeout` — 최신 FFmpeg의 RTSP 키는 `timeout` | `:126` | 5초 타임아웃이 무효일 수 있음 |
> | `terminate()` 폴백 | `:53` | `AVIOInterruptCB` 부재의 결과. 스레드 강제 종료 = 자원 누수 위험 |
>
> 게다가 이 경로엔 `add-reference-timestamp-meta` 가 없어 **지연 측정 자체가 안 된다** —
> 문제가 생겨도 숫자가 안 나온다. `[Pipeline]`·`[Trace]` 로그도 GStreamer 전용이다.

선택은 레지스트리 `HKCU\Software\GuardX\VMS\video_backend` (현재 `gstreamer`).

⚠️ `glass_latency_ms()` 는 이름과 달리 **appsink 도착까지**다 (화면 아님).
로그 문자열은 2026-08-03에 `glass-to-arrival` 로 바꿨지만 C++ 심볼은 그대로 두었다
(`ChannelView`·`VideoBackend` 전부에 걸쳐 있어 별도 작업).

### 2-4-1. 지연 계측 3종 (GStreamer 경로 전용)

셋 다 `FrameQueue` 안에 산다 — 스트리밍 스레드와 UI 스레드가 **둘 다 들고 있는
유일한 객체**라서, 별도로 만들면 배선만 늘어난다.

| 파일 | 클래스 | 무엇 |
|---|---|---|
| `frame_queue.h` | `FrameQueue` | mailbox 큐. 싱크 켜지면 capacity 확장 |
| `pipeline_stats.h` | `PipelineStats` | 30초 창 구간별 p50/p95/max — `[Pipeline]` 로그 |
| `frame_trace.h` | `FrameTrace` | 프레임 **한 장**을 촬영→렌더 제출까지 — `[Trace]` 로그 |

구간 정의: `촬영 ─net─> 디코더입구 ─decode─> appsink ─queue─> 표시선택 ─render─> 제출`.
`render` 는 CPU 제출까지고 GPU 완료·vsync·DWM 은 **여전히 미계측**이다.

`net`·`TOTAL` 은 카메라 시계 오프셋(현장 −1.04초)을 뺀 **상대값**이다 —
`PipelineStats::observe_net()` 주석 참조. 절대 지연은 카메라·PC NTP 동기가 선행 조건.

추적 주기는 `HKCU\Software\GuardX\VMS\trace_interval_ms` (기본 5000, `0`이면 끔).

---

## 3. 자주 묻는 것 — 정확한 위치

### 3-1. 박스 좌표를 어디서 받아오나

**카메라 OpenSDK HTTP** 다. DB가 아니다.

```cpp
// detection_feed.cpp:15
static const QString FEED_PATH = "/opensdk/juan_application/detections";
```

전체 URL은 `Credentials::camera_base_url()` + 이 경로:
```
http://192.168.0.3/opensdk/juan_application/detections?since=...
```

호스트는 `credentials.ini` 의 `[camera] host`. `https=true` + 인증서 지문이
설정돼 있으면 HTTPS로 나간다.

### 3-2. RTSP 영상 URL / profile5는 어디서

```cpp
// live_viewer.cpp
QUrl LiveViewer::stream_url(int ch, const QString &profile) const
{
    QUrl u;
    u.setScheme("rtsp");
    u.setHost(Credentials::camera_host());
    u.setPort(CAM_PORT);                    // 554
    u.setUserName(Credentials::camera_user());
    u.setPassword(Credentials::camera_password());
    u.setPath(QString("/%1/%2/media.smp").arg(ch).arg(profile));
    return u;
}
```

→ `rtsp://admin:****@192.168.0.3:554/0/profile5/media.smp`

**프로파일(해상도)은 레지스트리에서 온다:**

```cpp
static QString sub_profile()   // 그리드용 (08-19: 전체화면 삭제로 이것 하나만 남았다)
{ return QSettings("GuardX","VMS").value("grid_profile", "profile5").toString(); }

```

| 위치 | 값 |
|---|---|
| `HKCU\Software\GuardX\VMS\grid_profile` | 없으면 기본 `profile5` |
| ~~`fullscreen_profile`~~ | 08-19 폐기 — 전체화면 모드가 사라져 읽는 코드가 없다 |

바꾸려면:
```bash
reg add "HKCU\Software\GuardX\VMS" /v grid_profile /t REG_SZ /d profile4 /f
```

**해상도 자체는 카메라가 정한다** — VMS는 프로파일 번호만 고른다.
카메라의 프로파일 설정은 `CameraTuner`(`camera_tuner.cpp`)가 SUNAPI로 만진다.

### 3-3. 화면 속 인구수 (OCC의 분자)

`channel_view.h:101` `occupancy()` — 위 2-3 참조.
**카메라 감지 박스 중 `category==1` 개수**다. DB·MQTT와 무관하다.

`LiveViewer::update_occupancy_stats()` 가 1초마다 이걸 읽어 유입 속도
(명/분)를 계산한다. **+5분 예측은 계산하지 않는다** — `PredictionFeed`
(카메라 `/prediction` 실모델 p50)가 원천이고, 스냅샷이 없거나 3분 이상
낡았을 때만(워밍업·피드 다운) 유입 속도 외삽으로 폴백한다 (07-30 배선).

### 3-4. 정원 (OCC의 분모)

`ZoneConfig::capacity(channel)` — MQTT `guardx/db/rpib/zones` 수신값.
못 받았으면 `Theme::channel_cap()` = `{60, 80, 40, 30}`.

⚠ **`Theme::channel_cap()` 을 직접 부르지 말 것** (08-10). 폴백은 이미
`ZoneConfig::capacity()` **안에** 있다. 밖에서 직접 부르면 값을 못 받은 동안엔
같아 보이지만, 값이 도착한 뒤 **같은 화면의 두 숫자가 갈린다** — 실제로 타일 OCC
배지는 `ZoneConfig` 를, 하단 점유/예측 패널과 `PredictionFeed` 는 `Theme` 를 쓰고
있었다. 값이 바뀔 수 있는 소비자는 `ZoneConfig::Notifier::changed` 에 재적용을
걸 것(`live_viewer.cpp` 생성자가 그 예).

### 3-5. 좌표계

| 이름 | 값 | 어디 |
|---|---|---|
| 카메라 원본 | 2592 × 1520 | `channel_view.h:171`, `crowd_page.h` |
| 히트맵 격자 | 60px (`CAM_CELL`) | `crowd_page.cpp` |
| 가상 평면도 | 1000 × 620 | `crowd_page.h` `FLOOR_W/H` |

`FloorCanvas::to_floor(channel, cam_x, cam_y)` 가 카메라 좌표 → 평면도 변환.
채널별 방위·화각은 `FloorCanvas::FANS[4]` 표. **실측이 나오면 이 표만 고치면 된다.**

---

## 4. 객체 추적(objective tracing)을 붙이려면

### 4-1. 이미 있는 것

| 조각 | 어디 | 상태 |
|---|---|---|
| `object_id` | `DetectionBox` / `AnimatedBox` | ✅ |
| 박스 클릭 이벤트 | `ChannelView::object_selected(db_channel, object_id, cam_rect)` | ✅ |
| 선택 강조 | `ChannelView::set_selected_object(id)` — 전 채널 동시 강조 | ✅ |
| DB 궤적 테이블 | `tracks`, `track_path` (스키마에 있음, 미사용) | ⚠ 적재 여부 확인 필요 |
| TRACK 화면 자리 | `MainWindow` 인덱스 2 (placeholder) | ⬜ |

**`LiveViewer::on_object_selected()` 가 현재 하이라이트만 하고 끝난다.**
v6 이전에는 여기서 DB를 조회했는데 그 코드는 제거됐다 — **추적 조회를 붙일
자리가 바로 여기다.**

### 4-2. 어느 부류인가 — B(질의)

> **판별 기준**: 사용자가 뭘 고르느냐에 따라 답이 달라지는가?
> 추적은 "클릭한 `object_id`" 에 따라 달라진다 → **B**

따라서 **요청-응답**이 필요하고, `CrowdPage` 의 히트맵 구조를 그대로 베끼면 된다.

### 4-3. 고칠 곳 (파일별)

#### ① RPi B — `rpi_b/src/MqttDb/task_vms.cpp`

`handleHeatday` 를 복사해 SQL만 바꾼다.

```cpp
const char* TOPIC_TRACK = "guardx/db/rpib/query/track";

void handleTrack(const std::string& req) {
  const int object_id = (int)jnum(req, "object_id", -1);
  // SELECT ts, ST_X(geom), ST_Y(geom), channel
  //   FROM detections
  //  WHERE object_id = $1 AND ts >= $2 AND ts < $3
  //  ORDER BY ts
}
```

`startVmsQueryService` 에 `mqttSubscribe(TOPIC_TRACK, handleTrack);` 한 줄 추가.

**확인됨 (07-31)**: `object_id` 는 채널 간 유지되지 **않는다**. 채널 넘는
re-id 로직은 팀장 담당으로 진행 중 — 이 리포에서 손대지 말 것.
TRACK 화면(궤적 조회·렌더)도 팀장 + 병규 담당이다.

#### ② VMS — 새 파일 `track_page.{h,cpp}`

부기 코드는 `MqttLink::request()`가 다 한다 (§7) — 이것만 쓰면 된다:

```cpp
QJsonObject params;
params["query"]     = "track";
params["object_id"] = id;

MqttLink::instance()->request(
    "guardx/db/rpib/query/track", params,
    [this](const QJsonObject &reply) { draw_path(reply["path"].toArray()); },
    [this](const QString &reason)    { set_status("궤적 조회 실패 — " + reason, true); });
```

`req_id`·`reply_to`·타임아웃·응답 토픽 구독은 전부 자동이다.

#### ③ VMS — `mainwindow.cpp` 한 줄

```cpp
m_pages->addWidget(new PlaceholderPage("Target Tracking", this)); // 2 track
                            ↓
m_pages->addWidget(new TrackPage(this));                          // 2 track
```

#### ④ VMS — `live_viewer.cpp` (선택)

박스를 클릭하면 TRACK 화면으로 넘어가게 하려면 `on_object_selected()` 에서
시그널을 하나 더 올리면 된다.

#### ⑤ VMS — `CMakeLists.txt`

```cmake
qt_add_executable(gstream_VMS
    ...
    track_page.h track_page.cpp
)
```

### 4-4. 궤적을 어디에 그릴 것인가 — 두 선택지

| 방식 | 재사용할 것 | 특징 |
|---|---|---|
| **평면도 위 선** | `FloorCanvas::to_floor()` | 공간 이동이 직관적. `CrowdPage` 캔버스 재사용 가능 |
| **영상 위 잔상** | `BoxOverlay` | 실제 화면과 대조. 지나간 경로를 흐리게 |

`FloorCanvas` 는 `crowd_page.h` 에 있어 `TrackPage` 에서도 include 하면 쓸 수 있다.
**셀 대신 선을 그리려면** `set_cells()` 옆에 `set_path()` 를 추가하는 정도면 된다.

### 4-5. 작업 순서 제안

| # | 작업 | 확인 |
|---|---|---|
| 1 | `object_id` 가 채널 간 유지되는지 DB로 확인 | `SELECT object_id, count(DISTINCT channel) FROM detections GROUP BY 1 HAVING count(DISTINCT channel)>1` |
| 2 | RPi B에 `query/track` 추가 | `mosquitto_pub` 로 손수 요청 → 응답 확인 |
| 3 | `TrackPage` 뼈대 + 요청-응답 | 로그에 응답 도착 |
| 4 | 궤적 렌더링 | 화면 |
| 5 | LIVE에서 클릭 → TRACK 연동 | — |

**2번을 먼저 하면** VMS 없이 폴러만으로 검증된다 —
`heatmap_query_service.sh` 를 참고해 셸로 먼저 시험해봐도 된다.

---

## 5. 고칠 만한 곳 (기술 부채)

지금 동작에는 문제없지만 손볼 여지가 있는 것들.

| 대상 | 문제 | 제안 |
|---|---|---|
| `crowd_page.cpp` | `FloorCanvas`(렌더)와 `CrowdPage`(데이터·UI)가 한 파일 | **병규 담당 등재 (07-31)** — `HANDOFF_병규_2026-07-30.md` §3 |
| `live_viewer.cpp` | 레이아웃·재생·통계·타임라인이 뭉쳐 있음 | ✅ **해소 (07-31)** — `WallLayout`(레이아웃 전환) 분리 |
| ~~MQTT 요청-응답 코드 4벌~~ | ✅ **해소 (07-30)** — `MqttLink::request()` 로 추출 | 아래 §7 참조 |
| `MIN_LIKELIHOOD = 0.30` | `detection_feed.cpp` 와 `crowd_page.cpp` 에 **중복 상수** | 한 곳으로. 어긋나면 두 화면 수치가 안 맞는다 |
| 2592×1520 | `channel_view.h` 와 `crowd_page.h` 에 중복 | payload에 `frame_w/h` 를 실어 받는 게 정석 |
| `PlaceholderPage` 3개 | REPORT·DEVICE 미구현 | — |

---

## 7. MQTT 요청-응답 — `MqttLink::request()` (07-30 추출)

MQTT엔 RPC가 없다. 규약은 "요청 토픽 + 응답 토픽 + payload의 `req_id`로 짝
맞추기"인데, 그 부기 코드가 화면마다 복사돼 **4벌**(heatday·set_zone·
occseries·incidents)까지 갔다. 한 곳으로 모았다.

```cpp
MqttLink::instance()->request(
    "guardx/db/rpib/query/heatday",
    params,                                   // 질의 고유 인자만
    [this](const QJsonObject &reply) { … },   // 성공
    [this](const QString &reason)    { … });  // 타임아웃·미연결·ok:false
```

`node_id`·`timestamp`·`req_id`·`reply_to`는 자동으로 채워진다. 응답 토픽
구독은 첫 요청 때 1회, 타임아웃 청소 타이머는 **대기 중인 요청이 있을 때만**
돈다.

설계에서 신경 쓴 것:

- **`ok:false`도 에러 경로로 보낸다.** 화면이 "응답 없음"과 "서버가 거절함"을
  다른 문구로 표시할 수 있어야 한다 — 침묵하면 원인을 못 찾는다.
- **동시 요청이 섞이지 않는다.** 요청마다 자기 콜백을 들고 있으므로
  CrowdPage가 날짜 3개를 한꺼번에 물어도 각자 제 캐시로 들어간다.
- **콜백 수명은 호출부 책임.** 위젯을 캡처하면서 요청이 살아 있는 동안
  위젯이 죽을 수 있다면 `cancel(req_id)`를 소멸자에서 부를 것.
- 스텁 빌드(libmosquitto 없음)에서도 컴파일된다 — `publish()`가 false를
  돌려주고 `on_error`가 즉시 불린다.

추적(TRACK)을 붙일 때는 이제 `query/track` 요청 한 줄이면 된다.

---

## 6. 빌드 시 주의

**헤더(`.h`)를 고쳤는데 이상하게 죽으면 클린 빌드부터.**
증분 빌드가 헤더 의존을 놓쳐 다른 `.obj` 가 옛 클래스 크기로 남으면
`new` 가 메모리를 덜 잡아 힙이 깨진다 (실제로 겪음 — `CrowdPage` 생성자에서 크래시).

```bash
cmake --build <builddir> --clean-first --parallel
```

빌드 전에 앱을 끌 것 — 실행 중이면 `LNK1104` 로 링크가 막힌다.

### 폰트는 설치가 아니라 번들이다 (2026-07-30)

`VMS/fonts/*.ttf` 를 Qt 리소스(`:/fonts`)에 넣고 `Theme::load_bundled_fonts()`
가 시작 시 `QFontDatabase::addApplicationFont()` 로 등록한다. **배포 PC에
IBM Plex를 설치할 필요가 없다.**

- 넣을 것은 **굵기별 정적(static) face** — 가변 폰트는 Qt가 중간 굵기를
  못 살린다. 코드가 쓰는 굵기: Sans 400/500/600/700 · Mono 400/600
- 파일이 없으면 그 face만 빼고 빌드되고(CMake가 STATUS로 알림), 런타임엔
  Segoe UI / Consolas 로 폴백한다. **파일을 넣은 뒤엔 CMake 재실행 필수**
- ⚠ 폰트를 *설치*하면 GDI 레거시 이름 때문에 SemiBold가
  `"IBM Plex Mono SmBld"` 라는 **별도 패밀리**로 잡혀 `setWeight(600)` 이
  헛돈다. 앱 폰트 등록은 typographic family로 병합돼 이 함정을 피한다
  (Qt 6.11 실측: `styles=[Regular, SemiBold]`, `setWeight(600)` exact match).

### 의사 재배치(pseudo relocation) 함정 — MinGW + 수학 함수

시작하자마자 이렇게 죽으면:

```
Mingw-w64 runtime failure:
32 bit pseudo relocation at 00007FF6E67209C7 out of range, targeting 00007FFF81A17A30
… terminated with exit code 3
```

**빌드는 멀쩡한데 실행만 죽고, 게다가 될 때도 있다** (ASLR이 exe와 DLL을 4GB
넘게 떨어뜨린 실행에서만 터진다 — 실측 당시 우리 셸에서는 12회 연속 정상,
사용자 Qt Creator에서는 재현).

원인: GCC가 `sqrt(x)` 를 `sqrtsd` 명령 + **"음수면 errno 설정용 라이브러리
호출"** 두 갈래로 낸다. 인자가 음수가 아님을 컴파일러가 증명하지 못하면
그 폴백 호출이 남고, 링커가 `msvcrt.dll` 의 `sqrt` 를 auto-import 하며
32비트 의사 재배치 썽크(`__fu0_sqrt` …)를 만든다. x64에서 이 32비트 변위는
exe–DLL 거리를 담지 못한다.

수정(2026-07-30, `CMakeLists.txt`): `-fno-math-errno` 로 폴백 호출 자체를
없애고, `-Wl,--disable-auto-import` 로 재발 시 **런타임 abort 대신 링크
에러**가 나게 막아뒀다.

진단 명령 — 의사 재배치가 남아 있는지 (시작 주소 == 끝 주소면 없음, 정상):

```bash
nm gstream_VMS.exe | grep RUNTIME_PSEUDO_RELOC_LIST
```

---

## 부록 A — 2026-07-30 추가 컴포넌트 (예측 배선 · REPORT)

### `PredictionFeed` — 카메라 예측 수신기 (싱글턴)
`prediction_feed.{h,cpp}` — DetectionFeed와 같은 패턴.

- 채널별 60초 `GET /prediction?channel=N&capacity=<정원>` (① 카메라 직결 —
  모델이 1분 해상도라 이보다 잦으면 같은 값만 온다)
- **capacity 주입 (카메라 v16)**: `set_capacity(ch, cap)` — p_over_capacity가
  주입된 정원 기준으로 계산됨 (미주입 시 앱 상수 20 기준이라 표시 무의미)
- `Info`: served/warmup + horizon 4종 {minutes, p50, p10, p90, p_over_capacity}
- 시그널 `prediction_arrived(ch, Info)` — LiveViewer가 +5분 p50 소비 (신선도
  3분, warmup·부재 시 종전 유입속도 외삽으로 폴백)

⚠ **라이브 예측은 MQTT를 타지 않는다** — 의도적 결정 (실시간=카메라 직결,
DB=이력·경보·MAE). 근거·경위: `rpi_b/POLLER_VMS_CHANGES.md` §3-2 인용문.

### `ReportPage` — Post-Analysis Reports (07-30 리디자인, 커스텀 페인팅)
`report_page.{h,cpp}` + `report_chart.{h,cpp}` — mainwindow 인덱스 3.
디자인 원본: `Downloads/Reports Tab Redesign Options/design_handoff_reports_tab`
(README §2a가 최종안). QTextBrowser 구현을 대체.

- 구성: 2×2 채널 카드(이름·모델 메타·현재/정원·pill + 차트 + horizon 표)
  ⇄ **COMPARE 모드**(채널 칩 → 오버레이 차트 + 미니 카드). 시간 범위
  세그먼트 60min/6h/24h. 60초 자동 + REFRESH + 진입 시 즉시 갱신
- `ReportChart`: 이력(좌 62%, 선형)/예측(우 38%, √스케일) 분할 차트.
  단일 모드는 면 채움 + p10–p90 밴드 + 상단 고정 "CAP n ⌁"(축 절단) 마커
- 데이터 원천 (실시간=카메라 직결 · 이력/설정=DB 원칙):
  - 현재 인원 + 60분 이력: 카메라 `/occupancy` (`series_1min`)
  - 예측 4-horizon + 모델 메타: 카메라 `/prediction?channel&capacity`
  - 정원·pill 임계: `ZoneConfig` (MQTT zones — 임계는 warn_ratio)
  - **6h/24h 이력: MQTT `guardx/db/rpib/query/occseries`** (신규 —
    RPi B `task_vms.cpp`가 zone_occupancy를 5분/15분 버킷 평균으로 집계.
    heatday와 같은 요청-응답, 응답 토픽 공유 + req_id 구분)
- `p_over_capacity == -1`은 **불명 → '—'** (0%로 표기 금지). P(초과) 색은
  핸드오프의 항상-초록 대신 >50% alarm · >20% amber (안전 규칙 유지)
- 페이지 한정 디자인 토큰은 `ReportTk`(report_chart.h) — Theme와 값이
  미세하게 달라 의도적으로 격리 (핸드오프 "High-fidelity" 요구)
- 구버전의 LINE FLOW·PIPELINE 섹션(/events·/stats)은 핸드오프 범위 밖이라
  **제거됨** — 필요하면 git 이력의 QTextBrowser판 참조

---

## 부록 B — 2026-07-30 혼잡 경보 배선 (팝업 · LIVE 색 · REPORT 예측 이벤트)

판정은 **전부 RPi B `task_alert.cpp`가 한다** (3신호 채택 + 히스테리시스).
VMS는 판정하지 않고 받아서 보여주기만 한다 — 임계 해석이 두 곳에 생기면
DB와 화면이 갈라진다.

### 토픽 3종

| 토픽 | 성격 | 쓰임 |
|---|---|---|
| `guardx/alert/rpib` | **edge**, retain=false | 전이 순간 (warn/critical/clear) |
| `guardx/db/rpib/incidents` | **retained** | 열린 incident 스냅샷 — 신규 |
| `guardx/db/rpib/query/incidents` | 요청-응답 | 이력 표 — 신규 |

> **왜 스냅샷이 필요한가**: 폴러는 상태가 *바뀔 때만* retain=false로 쏜다.
> 경보 발생 뒤에 VMS를 켜면 이미 열린 critical을 영영 모른다 —
> 화면은 평온한데 현장은 critical. retained 토픽이 그 구멍을 막는다.
>
> ⚠ 스냅샷은 폴러 설정 틱(기본 30초)에만 재발행되는데 edge는 즉시 온다.
> 그대로 적용하면 방금 해제된 채널이 30초간 되살아난다. `AlertFeed`가
> payload의 `timestamp`를 채널별 마지막 라이브 시각과 비교해 낡은 스냅샷은
> **그 채널만** 무시한다.

### VMS 쪽

| 파일 | 역할 |
|---|---|
| `alert_feed.{h,cpp}` | 싱글턴 허브. 위 3토픽을 합쳐 채널별 단계 + 이력 보유 |
| `alert_popup.{h,cpp}` | **critical 팝업** — 프레임 없는 모드리스 창, 테두리 점멸 |
| `channel_view.cpp` | LIVE 타일: warn=앰버 2px, critical=빨강 3px **점멸** + 문구 칩 |
| `nav_rail.cpp` | LIVE 버튼 배지 (critical 점멸 / warn 정적) |
| `report_page.cpp` | PREDICTED CONGESTION EVENTS 패널 (`predicted`만 필터) |

**왜 탭이 아니라 팝업인가**: 경보는 "보러 가는 것"이 아니라 "찾아오는 것"이다.
탭에 넣으면 다른 화면을 보는 동안 아무 일도 일어나지 않는다. DB 규약도 이미
이쪽을 가리킨다 — `alerts.broadcast_channel = 'vms_popup'`.

- **모드리스**다. 경보가 떴다고 조작을 막지 않는다 — 감시를 방해하면 안 된다
- **critical만** 팝업이 뜬다. warn은 타일 색만 (주의마다 창이 뜨면 경보 피로로
  정작 critical을 무시하게 된다)
- 닫아도 경보는 유효하다 — 타일은 계속 빨갛고, 새 critical 전이가 오면 다시 뜬다.
  해당 채널이 전부 해제되면 **스스로 닫힌다**
- 타일은 `LiveViewer`를 거치지 않고 **자기가 구독한다** (박스 피드와 같은 패턴)
- **색 단독 금지**: 테두리 색과 함께 `CRITICAL 52/60 예측` 문구 칩을 늘 띄운다

### ⏱ 경보 지연 — "임계를 바꿨는데 반응이 없다"의 정체

**최대 60초 걸리는 것이 정상이다.** 2026-07-30 실측으로 확인한 경로:

| 단계 | 지연 |
|---|---|
| ZONES 저장 → `cmd/set_zone` → DB UPDATE → `zones` 즉시 재발행 → VMS 분모 변경 | **~1초** |
| **혼잡 판정(`pollAlert`) — 다음 틱까지 대기** | **0~60초** ← 병목 |
| 판정 → `guardx/alert/rpib` 발행 → 팝업·타일 | **<0.1초** |

원인은 `poller_main.cpp`가 `pollAlert`를 `pred_interval_s`(기본 **60초**,
`Config/config.hpp`) 틱에 묶어둔 것이다. 임계는 즉시 반영되는데 판정만
안 따라오니 "분모는 1로 바뀌었는데 경보가 없네?"로 보인다.

또 하나 헷갈리기 쉬운 것: 경보는 **전이 시점에만** 발행된다. 이미 열려 있는
incident는 `guardx/alert/rpib`를 아무리 구독해도 다시 오지 않는다 —
그래서 retained `incidents` 스냅샷이 필요하다(위 참조).

**개선안** (미적용 — 폴러 수정 + 재배포 필요):
1. `handleSetZone` 직후 해당 존만 **즉시 재판정** — 위 시나리오를 직접 없앤다
2. `pollAlert`+`pollOccupancy`를 예측에서 떼어내 `alert_interval_s`(신설,
   10초 권장)로 분리 — 예측은 모델이 1분 해상도라 60초 유지.
   `/occupancy`는 가볍고 `zone_occupancy` upsert는 멱등이라 안전하다
3. retained 스냅샷에 `capacity`(+가능하면 count) 추가 — 지금은 재시작 복원
   시 칩이 `CRITICAL`만 뜨고 `2/1` 같은 숫자가 안 나온다

### 검증 방법 — `tools/alert_test.ps1`

폴러 60초 틱을 기다리지 않고 경보 UI를 직접 검증한다. 8개 시나리오:
critical · warn · 다중 · 잘못된 입력 · clear · 스냅샷 복원 · **낡은 스냅샷
무시** · 스냅샷 비우기.

```bash
powershell -ExecutionPolicy Bypass -File vms/tools/alert_test.ps1 -Case all -Shot
```

기본 대상은 **격리 브로커 `127.0.0.1:11883`**이다. 먼저 띄울 것:

```bash
mosquitto -p 11883
```

VMS는 `GUARDX_CREDENTIALS`로 `[mqtt] host=127.0.0.1 port=11883` 짜리 임시
ini를 물려 실행한다 (실설정을 건드리지 않는다).

⚠ 실브로커(`guardx/alert/rpib`)에 가짜 경보를 쏘면 transmission layer의
**실제 액추에이터가 반응할 수 있다**. 실브로커로 시험하려면 담당자 동의를
받고, 끝나면 반드시 같은 incident_id로 `-Case clear`를 쏴서 되돌린다.

⚠ payload는 **반드시 파일로** 넘긴다 (`mosquitto_pub -f`) — PowerShell이
네이티브 인자의 큰따옴표를 먹어서 `-m '{"a":1}'`은 `{a:1}`로 도착해 JSON
파싱에 실패하고 조용히 무시된다 (앱 버그로 오인함, 실제로 겪음).
스크립트가 이미 파일 방식으로 처리한다.

⚠ `.ps1`은 **UTF-8 BOM**으로 저장할 것. Windows PowerShell 5.1은 BOM 없는
스크립트를 ANSI로 읽어 한글 주석이 깨지고 파싱이 죽는다 (역시 실제로 겪음).

### CMakeLists 주의 (07-30 수리 반영)
GStreamer를 pkg-config로 찾는 환경에서 FFmpeg 백엔드가 조용히 빠지던
비결정성 2건(탐색 스킵 + 링크 디렉터리 누락) 수리됨 — **pull 후 반드시
"CMake 다시 실행"** (WINDOWS_SETUP.md §3-3과 동일 요령).

---

## 부록 C — 2026-08-06 카메라 제어 (전역 자원 모니터 + CAMERA 탭)

VMS가 카메라를 **보기만 하던 것에서 읽고 조작하는 것으로** 넓어졌다.
신규 8쌍(3,081줄) + 기존 확장. 브랜치 `VEDA-175-Camera_Manipulation_Logic`.

### C-1. 두 개의 창구 — 읽기 하나, 쓰기 하나

| 파일 | 클래스 | 역할 |
|---|---|---|
| `camera_status.{h,cpp}` · 137+401 | `CameraStatus` | **읽기 1원천 싱글턴.** `opensdk.cgi appstatus` 1초 + `apps view` 5초. `main.cpp`에서 위젯보다 먼저 기동(전역 표시라 탭 무관 상시) |
| `camera_control.{h,cpp}` · 53+123 | `CameraControl` | **쓰기 창구.** 앱 control/set, 재부팅. 상태 확정은 안 한다 — 응답 후 호출부가 재폴 |

```cpp
// 소비자는 구독만 한다 — 화면이 늘어도 카메라 부하는 그대로
connect(CameraStatus::instance(), &CameraStatus::resources_changed, …);
connect(CameraStatus::instance(), &CameraStatus::apps_changed,      …);
connect(CameraStatus::instance(), &CameraStatus::link_state_changed,…);
```

**상태기계**(`LinkState`): `Online` → 1~2회 실패 `Stale` → 3회+ `Offline`.
재부팅 버튼은 발사 **전에** `enter_reboot_mode()`를 불러 `Rebooting`으로
바꾼다 — 안 그러면 자기가 없앤 카메라를 오프라인으로 오해한다(2분 상한).

- 폴 겹침 방지(`m_pending_*`) · 타임아웃 2.5s · 실패 로그는 **전이당 1회**.
- **킬스위치**: 레지스트리 `camera_status_poll=0` → 폴러 자체를 안 만든다.
  카메라 제어 채널 반죽음이 재발하면 이걸 먼저 꺼서 용의선상에서 제외할 것.
- 제어 채널 순증 부하 ≈ **1.2 req/s** (read-only GET).

### C-2. 화면

| 파일 | 클래스 | 역할 |
|---|---|---|
| `camera_page.{h,cpp}` · 75+680 | `CameraPage`, `ResourceGauge` | CAMERA 탭 본체. 아크 게이지 3개(270°·550ms 롤링·60초 스파크라인) + 오프라인/재부팅 배너 + deviceinfo 표 + 하위탭 스택 |
| `app_card.{h,cpp}` · 54+237 | `AppCard` | 앱 한 장 — 4px 상태바·호흡점·앱별 자원·Start/Stop·AutoStart·Priority |
| `system_panel.{h,cpp}` · 48+436 | `SystemPanel` | 시계/NTP(보일 때만 30s)·프로파일 세션·로그 3종·설정 백업·**재부팅(2단 확인)** |
| `profile_panel.{h,cpp}` · 49+412 | `ProfilePanel` | 인코더 표·인라인 편집(update)·키프레임 강제·저지연 프리셋(`CameraTuner` 흡수) |
| `image_panel.{h,cpp}` · 38+219 | `ImagePanel` | 역광/WB/IR LED 콤보 + Simple Focus |
| `network_panel.{h,cpp}` · 24+95 | `NetworkPanel` | interface/rtsp/qos **조회 전용** (설정 변경 버튼 없음 — 의도) |

`top_bar.{h,cpp}`도 확장됐다: 자원 pill(400ms 롤링) + APPS pill(클릭 → CAMERA
탭) + 기존 정적 pill 실배선(CAM은 `LiveViewer::stream_health_changed` 신규
시그널, DB·RPI B는 `MqttLink::online_changed`). **RPI A·C는 신호원이 없어
dim 유지** — 거짓 초록 금지.

### C-3. 조작의 규칙 — 낙관적 전이 + 폴 확정

```
클릭 → 즉시 "정지 중…" + 버튼 잠금        (UI는 바로 반응)
     → CameraControl 발사
     → 응답 후 request_apps_now() + 1.5초 뒤 한 번 더
     → 목표 상태 도달 시 확정 / 20초 무소식이면 포기(폴이 말하는 진실로)
     → 실패 응답이면 즉시 원복 + 토스트
```

안전장치: `IsDefault`(WiseAI)는 정지 불가 · `ControlForbidden` 비어 있지 않으면
조작 전부 잠금 · Stop은 **의존성 경고 다이얼로그**(기본 버튼=취소).

> ⚠ **Start/Stop 실험은 `test_calibration` 으로만.** `test`(박스 원천)·
> `juan_application`(폴러·DB)·`WiseAI`(AI 전면)를 끄면 실제로 파이프라인이 죽는다.

### C-4. AlertFeed 확장 (카메라/앱 이상 → 기존 경보 인프라)

채널 스키마를 침해하지 않고 **음수 특수 키**를 더했다 (`alert_feed.h`):

```cpp
static const int DEV_LINK = -1;  // 오프라인            → Critical
static const int DEV_APP  = -2;  // 앱 죽음/AutoStart 구멍 → Critical/Warn
static const int DEV_RES  = -3;  // 자원 >90% 10초+      → Warn
void raise_device_alert(int key, Severity sev, const QString &message);
```

- 전이 때만 발행(같은 단계·메시지면 무시) — 1초 폴러가 초당 경보를 쏘지 않는다.
- 사용자가 누른 Stop은 `CameraStatus::expect_app_stop()`으로 등록해 경보에서
  제외 — 자기가 만든 이벤트로 자기가 놀라지 않게.
- 소비: `AlertPopup`이 장비 critical을 같은 팝업에 합류, `NavRail` LIVE 배지가
  건수를 합산.

### C-5. ⚠ SUNAPI 함정 3종 (08-06 실측)

1. **`apps&action=set`은 Priority와 AutoStart를 함께** 보내야 한다. 한 키만
   보내면 `Invalid Input Value(s)`. 값은 대문자 `True/False`(조회 JSON은
   소문자 bool — 비대칭 주의). → `set_app(id, auto, prio, label)`로 강제.
2. **`BitrateControlType`도 코덱 접두 키**다 —
   `Channel.N.Profile.M.H264.BitrateControlType`. 최상위로 찾으면 영영 `—`.
3. **`irled` Mode에 문서 enum 밖 값이 온다**(실측 `Auto1`) → 콤보에 없으면
   자동 추가. "카메라가 아는 값이 우리 목록보다 진실이다."

부수 확인: `profileaccessinfo`는 **지원된다**(전 채널×프로파일 덤프에서
`ConcurrentUserCount>0`만 추려 RTSP 부하 가시화) · 앱 조작에 `Channel`
파라미터 불필요(`ChannelType=Multiple`) · `ethstatus`는 NVR 전용.

### C-6. 검증 도구 함정

- **`tools/shot.ps1`은 DPI 비인식** — 125% 스케일에서 창 오른쪽 ~25%가 안 찍혀
  멀쩡한 레이아웃을 깨진 것처럼 보여준다(실사고). `SetProcessDPIAware()` 판을
  쓸 것.
- Qt GUI 앱 콘솔 로그는 `QT_LOGGING_TO_CONSOLE=1` 없이는 증발한다.
- 실행 검증 종료는 **`CloseMainWindow()`** — 강제종료는 RTSP 세션을 오염시킨다
  (08-04 정량 실증).

> 상세 설계·실측 전문은 Obsidian 「VMS 카메라 제어 — 구현 정본 (08-06)」.


---

## 부록 D — 2026-08-06 화면 테마 (다크 / 라이트 / 시스템)

팔레트가 **상수에서 런타임 값**이 됐다. 색을 만지는 코드는 이 규칙을 따른다.

### D-1. 색을 쓰는 세 가지 방법

| 방법 | 테마 전환에 따라오나 | 해야 할 일 |
|---|---|---|
| `paintEvent`에서 `Theme::xxx` 읽기 | ✅ 자동 | 없음 (재도색이면 끝) |
| `setStyleSheet`에 색을 구움 | ❌ | `Theme::restyle(위젯, [] { return QString(...); })` |
| `setText`의 HTML에 색을 구움 | ❌ | 클래스의 render를 `Theme::on_theme_changed`에 한 번 연결 |

```cpp
// 스타일시트 — 테마가 바뀌면 람다를 다시 불러 붙인다
Theme::restyle(label, [] {
    return QString("color:%1;").arg(Theme::textDim.name());
});

// HTML에 색을 굽는 위젯 — 통째로 다시 그린다
Theme::on_theme_changed(this, [this] { render(); });
```

> ⚠ **`restyle`을 매번 불리는 함수 안에 두지 말 것.** 연결이 계속 쌓인다.
> `render()` 같은 자리는 평범한 `setStyleSheet`를 쓰고, 그 `render()` 자체를
> `on_theme_changed`에 한 번만 걸어라. 반대로 **매번 새 위젯을 만드는** 자리는
> 안전하다 — 연결이 위젯과 함께 사라진다.

> ⚠ **색을 값으로 넘기지 말 것.** `f(QColor c)`로 받아 캡처하면 그 순간의 색이
> 굳는다. 팔레트 슬롯 포인터를 받아라: `f(const QColor *role)` ← `&Theme::accent`.

### D-2. 예외 두 가지 — 면이 다르면 색도 다르다

- **`Theme::OnVideo`** — 영상 타일 안쪽(타임스탬프·OCC 배지·감지 박스·채널명).
  배경이 임의의 장면이라 **어느 모드에서든 다크 값 고정**. 라이트 테마의 검은
  글자는 밝은 장면에서 사라진다. (`channel_view.cpp`)
- **`chrome*` 토큰 + `chrome_is_dark()`** — 상단바·내비는 본문과 다른 면일 수
  있다(3a는 크롬만 다크). 크롬이 어두우면 그 위의 상태색은 `OnVideo`의 밝은
  값을 쓴다. QSS도 `#ChromePill`·`#ChromeBtn`으로 본문의 `#Pill`·`#OutlineBtn`과
  분리돼 있다 — 이름을 공유하면 카드 위 칩까지 다크로 칠해진다.

### D-3. 모드 결정

```cpp
enum class Pref { System, Dark, Light };   // 레지스트리 theme_mode
Theme::set_preference(Theme::Pref::System);
```
기본은 **System** — `QStyleHints::colorScheme()`(Qt 6.8+)을 읽고
`colorSchemeChanged`를 구독해 OS를 따라간다. 사용자가 직접 고르면 그 선택이
우선한다(OS가 되돌리지 않는다). 설정 UI는 SETTINGS 화면(구 ZONES) 최상단.

### D-4. ⚠ QSS 자리표시자 함정 (실사고)

`build_qss()`는 **`@이름` 토큰 치환**을 쓴다. `%1` 방식은 토큰이 10개를 넘는
순간 **`%14`가 `%1` + `"4"`로 치환**돼 `#E9EDF24` 같은 7자리 색이 만들어지고,
Qt가 앞 6자리만 읽어 **오류 없이 색만 조용히 틀린다**. 치환은 **긴 이름부터**
— `@border`가 `@borderDim`을 잘라먹지 않게.

색이 이상한데 원인을 모르겠으면 **눈이 아니라 픽셀값을 찍어라**
(`GetPixel`). 08-06에 이 방법으로 잡았다.

### D-5. 지오메트리 (디자인 핸드오프 4a §Geometry — 전 테마 공통)

상단바 48px · 사이드바 84px · 카드 radius 4px · 컨트롤 3px · 패널 헤더 40px ·
페이지 여백 24/26px · 카드 간격 20px · 화면 제목 21/700.
색과 무관하므로 다크에도 함께 적용된다.

### D-6. 글자 크기 — 전역 배율 (08-07)

디자인 원본의 px 값은 **149개 호출부**에 흩어져 있다. 전체를 키우는 자리는
`theme.cpp`의 `FONT_SCALE`(현재 **1.15**) 하나뿐이다 — `make_font()`가 곱한다.
호출부의 상대 관계(9 < 10 < 11 …)는 그대로 유지된다.

```cpp
Theme::ui_font(11, 600)   // 디자인 11px → 화면 13px
Theme::px(56)             // 디자인 px → 화면 px (같은 배율)
```

`Theme::px()`를 써야 하는 자리 두 곳:
- **HTML 조각의 `font-size:10px`** — 안 쓰면 위젯만 커지고 span만 작게 남는다.
- **글자를 담는 고정 크기 컨트롤**(콤보·스핀·버튼 폭) — 글자만 키우면 잘린다.
  ⚠ `#OutlineBtn`은 좌우 padding 14px가 더 붙는다. 고정폭(`setFixedWidth`)보다
  **최소폭**(`setMinimumWidth`)이 안전하다 — "CLOSE"·"노캔 OFF"가 실제로 잘렸다.

화면 골격(상단바 48px·레일 84px·카드 radius)에는 **쓰지 않는다** — 4a 지오메트리
규격은 글자 크기와 무관하게 유지한다.

### D-7. 다크 팔레트 글자 계층 (08-07 조정)

9~11px 글자가 읽히지 않아 하위 3단계를 한 칸씩 올렸다. 배경 `panel`(#0D1118)
대비 기준:

| 토큰 | 전 | 후 | 대비 |
|---|---|---|---|
| `textMuted` | #8B96A8 | **#A7B2C4** | 5.8 → 8.1 |
| `textDim` | #5A6577 | **#8B96A8** | 2.2 → 5.8 |
| `textFaint` | #3A4557 | **#6C7789** | 1.8 → 3.8 |

계층(muted > dim > faint)은 그대로 두고 각 단계를 위로 민다.
`report_chart.cpp`의 ReportTk 다크 팔레트(`textSec`·`textFaint`·`chartLabel`)도
같은 이유로 함께 올렸다 — 차트 축 라벨이 9~10px이다.

### D-8. 전역 QSS가 책임지는 컨트롤 (08-07 추가)

`build_qss()`에 있는 규칙은 **두 테마를 모두 정의**한다. 새 위젯을 만들 때는
스타일을 직접 굽기 전에 여기 이름이 있는지 먼저 본다.

| 이름 | 무엇 |
|---|---|
| `#Panel` `#PanelHeaderLine` | 카드 면·헤더 밑줄 |
| `#Pill` `#OutlineBtn` `#Segmented` `#SegBtn` | 본문 칩·버튼·세그먼트 |
| `#ChromePill` `#ChromeBtn` | **크롬 전용** (상단바·내비) |
| `QScrollArea#RScroll` | 페이지 스크롤 (REPORT·CAMERA) |
| `QMessageBox` `QDialog` | 대화창 — OS 크롬 대신 앱 면·버튼 |
| `QAbstractSpinBox` | 숫자 입력칸 |

⚠ **스타일 없는 맨 위젯을 쓰지 말 것.** `QPushButton`을 그냥 두면 OS 기본
모양이라 **라이트 테마의 흰 카드 위에서 통째로 사라진다**(08-07 실사고 —
액추에이터 ON/SET/OPEN, 방송 RTP/노캔/ON/OFF 9개). `setObjectName("OutlineBtn")`
한 줄이면 두 테마가 함께 해결된다.
(08-10: 방송의 전송방식 전환·노캔 토글 버튼은 제거됐다 — RTP 전용·노캔 상시.
 방송 행에는 ON/OFF 만 남는다. 위 "9개"는 08-07 시점 기록이다.)

⚠ **QSS 규칙이 한 번 걸리면 `QPalette::Disabled`는 더 이상 먹지 않는다.**
비활성 표시를 QSS에서 직접 해 주지 않으면 **꺼진 버튼이 켜진 것과 똑같아
보인다**. `#OutlineBtn:disabled`는 그래서 있다.

⚠ **스핀박스의 위/아래 버튼(subcontrol)은 건드리지 말 것.** 한 번이라도
스타일을 주면 Qt가 그 자리에 기본 화살표를 그리지 않아 **화살표가 사라진다**.
CSS 삼각형(width/height 0 + border)도 Qt에서는 사각 블록으로 그려진다.
칸(면·테두리)만 맞추고 화살표는 OS 스타일에 맡긴다. (둘 다 08-07 실측)

> 상세·팔레트 값 전문은 Obsidian 「VMS 화면 테마 — 다크·라이트 전환 구조 (08-06)」.

---

## 부록 E — 2026-08-07 디바이스·화재·방송 통합 (VEDA-176 머지)

`main`(VEDA-174 하드웨어 zone·화재 + VEDA-172 방송)과 VEDA-175(카메라 제어·화면
테마)를 합친 결과다. **새로 들어온 파일 20개**와 그것들이 지켜야 하는 규약을
여기에 모은다.

### E-1. 무엇이 어디로 들어왔나

| 묶음 | 파일 | 역할 |
|---|---|---|
| DEVICE 화면 | `device_control_page.{h,cpp}` | ZONE/COMPARE 두 모드 · 센서 6타일 · 액추에이터 · 화재 수동 해제 |
| 센서 원천 | `zone_sensor_store.{h,cpp}` | `guardx/db/rpib/sensors` 구독 · zone별 최신값·이력·더미 생성 |
| 화재 | `fire_alert_feed` `fire_alert_popup` `fire_charts` `fire_zone_map.h` | 화재 상태 구독 · 팝업 · 게이지/파이 · zone↔CCTV 매핑 |
| 방송 | `broadcast_controller` `broadcast_rtp_sender` `broadcast_control_row` | **Opus/RTP 전용** 송출 · 노캔 상시 · 레벨 미터 (08-10) |
| 음향 이벤트 | `audio_alert_popup` | RPi C TOIMIC(비명·총성) 팝업 |
| 필드 정의 | `sensor_fields.h` | 센서 채널의 라벨·단위·임계 키·그래프 방향 |

통신 원칙은 그대로다 — **VMS ↔ RPi B 만** 말한다. 센서도 액추에이터 명령도
RPi B를 거친다(`fire_schema.sql` "설계 확정: VMS→B→C"). ACK 토픽이 없어
액추에이터 버튼은 **낙관적 토글**이다.

### E-2. 더미 zone — "값이 온다"를 생존으로 읽을 때 주의

`fire_zone_map.h`가 유일한 출처다. zone 1만 실 하드웨어이고 2·3·4는
`dummy=true`(시연용)다. `ZoneSensorStore`가 **더미 zone에는 가짜 값을 만들어
넣는다** — 그래서 "센서가 오고 있으니 RPi A가 살아 있다"를 판정할 때는
**반드시 `dummy=false`인 zone만** 세야 한다. 그러지 않으면 하드웨어가 죽어도
상태등이 영원히 초록이다. (상단바 RPI 필이 이 규칙을 쓴다 — `top_bar.cpp`)

| 노드 | 신호원 | 표시 |
|---|---|---|
| RPi A | 환경센서 수신 신선도(B 경유, `STALE_MS` 5초) | 초록 / amber(끊김) / dim(미수신) |
| RPi B | MQTT 브로커 연결 | 초록 / 적 |
| RPi C | **없음** — 액추에이터 수신 전용, publish는 `guardx/alert/rpic`(이벤트)뿐 | dim 고정 |

RPi C가 주기 상태를 publish하기 시작하면 같은 자리(`render_db_pills`)에 배선하면
된다. 그전까지는 **모르는 것을 초록으로 칠하지 않는다**.

### E-3. 새 화면이 테마를 따라오게 한 방법 (부록 D 규약의 적용 사례)

새 화면들은 색을 **생성 시점에** 스타일시트/QColor로 구워 넣어 라이트에서
글자가 사라졌다. 부록 D 규약대로 옮겼고, 그 과정에서 나온 함정 세 가지를
남긴다 — 다음에 화면을 추가할 때 같은 자리에서 걸린다.

1. **부수효과 있는 함수를 `on_theme_changed`에 걸지 말 것.**
   `FireAlertPopup::refresh()`는 "다시 칠하기"가 아니라 팝업의 표시 상태
   자체다(hide/show/raise/펄스 시작·정지). 색을 되살리려던 등록이 **창을 띄우거나
   감추는** 부수효과를 낸다. 창틀만 굽는 `apply_frame()`을 따로 떼어 그것만 걸었다.
   같은 이유로 DEVICE 화면은 `send_*`(MQTT 발행) 계열을 절대 걸지 않는다.
2. **색을 값으로 저장하면 갱신을 건너뛴 위젯이 옛 팔레트로 굳는다.**
   `risk_color()`가 `QColor`를 돌려주고 `GaugeBar`·`MiniLineChart`가 멤버로
   들고 있었다. `refresh_single()`에는 색을 안 건드리고 빠져나가는 경로가 둘
   있다(`!snap.has_data` / `!sm.present`) — 그 타일은 테마를 바꿔도 옛 색을
   계속 그렸다. → **팔레트 슬롯 포인터**(`const QColor *`)로 바꾸고 그릴 때 읽는다.
   COMPARE의 구역 범주색(`zone_color`)은 반대로 **테마를 타지 않아야** 맞다 —
   `Series`가 값/슬롯 둘 다 받는 이유다.
3. **문구를 다시 만드는 함수라면 문구를 보존할 것.**
   `refresh_*`가 끝에서 `set_status()`를 부르며 문장을 새로 만든다. 테마만
   바꿨는데 상태줄 문장이 갈리면 안 된다 — 지금 문구를 잡아 두었다가 되돌리고,
   오류 여부는 색이 아니라 **불리언**(`m_status_error`)으로 기억한다.

### E-4. 라이트 테마 회귀를 부르는 흔한 실수 (체크리스트)

- [ ] 새 `QPushButton`에 `setObjectName("OutlineBtn")` 을 줬는가 (부록 D-8)
- [ ] 상태에 따라 색이 바뀌는 라벨을 `restyle`로 묶지 않았는가 (연결 누적)
- [ ] 그 라벨의 재도색을 생성자에서 `on_theme_changed`에 **한 번** 걸었는가
- [ ] 색이 아니라 **의미**(enum·bool)를 저장했는가
- [ ] 고정폭 컨트롤에 `Theme::px()`를 태웠는가 (글자만 커져 잘리지 않게)

> 하루 서사와 머지 충돌 판단 근거는 Obsidian
> 「VMS 디바이스 통합 — VEDA-176 머지 정본 (08-07)」.

---

## 부록 F0 — 2026-08-10 낮: 리뷰 2회·병합·구조 수정

> 부록 F(심야 4커밋)의 **앞 단계**다. 리뷰 2회를 돌리고 팀 코드와 병합하고
> 확정 결함 중 급한 것을 고쳤다. 13커밋.
> 서사는 Obsidian 「0810_… (VMS)」 **PART 0**, 진행 상황은 「08-10 VMS 코드 리뷰 수정 TODO」.

### F0-1. 새 파일 1개

| 파일 | 역할 |
|---|---|
| `sunapi_request.h` (헤더 전용) | 전송 타임아웃이 **이미 걸려 나오는** `QNetworkRequest` 팩토리. 12개 파일이 요청을 만드는데 타임아웃을 거는 건 7개뿐이었다 → 13파일 26호출부를 전부 통과시켰다. 이제 원시 `QNetworkRequest(...)` 생성이 리포에 **0건**이라 깜빡할 수 없다. ⚠ `Accept: application/json` 은 **일부러 안 넣는다** — 절반은 SUNAPI 텍스트(`Key=Value`) 응답을 파싱한다 |

### F0-2. GstBus — 워치가 아니라 폴링이다 (⚠ 되돌리지 말 것)

`gst_bus_add_watch` 는 **GLib 메인루프가 돌아야** 콜백이 뜬다. Qt 는 Windows 에서
안 돌린다(`QT_FEATURE_glib=OFF`, `build/CMakeCache.txt` 확인). 워치를 걸면 **콜백이
한 번도 안 불리고** ERROR/EOS 복구 경로가 통째로 죽는다 — 컴파일 되고, 경고 없고,
watch id 도 정상 반환되며, **그냥 아무 일도 안 일어난다.**

| 파일 | 상태 |
|---|---|
| `broadcast_rtp_sender.cpp` | 원래부터 폴링 (헤더에 이유가 적혀 있었다) |
| `onvif_meta_source.cpp` · `direct_sink_backend.cpp` · `gstreamer_backend.cpp` | 08-10 에 폴링으로 통일 (`poll_bus()`, 100ms) |

⚠ `gst_bus_pop_filtered` 는 워치와 **소유권 규약이 다르다** — 꺼낸 메시지를
`gst_message_unref` 해야 한다. 워치 코드를 그대로 옮기면 메시지마다 누수.
실증: `[OnvifMeta] ch 2 "EOS"` 가 로그에 찍혔다(고치기 전엔 나올 수 없던 문구).
**감지 8초 → 0.1초.**

### F0-3. 스레드 경계 — 앱 코드가 GUI 스레드 밖에서 도는 곳은 3곳뿐이다

| 콜백 | 파일 | 경로 |
|---|---|---|
| `s_sink_in` (pad probe) | `direct_sink_backend.cpp` | **direct (현행)** |
| `s_sample` | `onvif_meta_source.cpp` | **메타 (현행)** |
| `s_new_sample` | `gstreamer_backend.cpp` | appsink (휴면) |

리포 전체에 `QThread` 파생은 `FFmpegWorker` 하나뿐이고(휴면), `moveToThread`·
`QtConcurrent`·`std::thread` 는 0건이다. **감사해야 할 동시성 표면이 이 3개가 전부다.**
`ChannelSync` 는 그 경계에서 락 없이 공유되고 있었다 → `QMutex` 추가(`recompute_locked`
로 개명해 계약을 이름에 박았다. 이탈/복귀 `qInfo()` 는 **락 밖으로** — 락 안 로그 I/O 가
표시 스레드를 세운다).

### F0-4. `Theme::restyle()` 은 연결을 **교체**한다 (누적 아님)

호출될 때마다 `Notifier::changed` 에 연결을 새로 추가하고 해제하지 않았다.
`TrackingPanel::refresh_strip()` 은 250ms 주기라 **추적 1시간에 약 1.4만 개**가 쌓이고,
테마 토글 시 그게 전부 동기 실행된다. 장부는 `QHash<QWidget*,Connection>` 이 아니라
**위젯의 동적 속성**(`theme_restyle_conn`)에 둔다 — 위젯이 죽으면 장부도 함께 죽어
dangling 키도 stale 연결도 원천적으로 없다.

### F0-5. direct 경로의 표시 계약 — 프레임이 없으면 창을 숨긴다

영상 위젯은 네이티브 HWND 이고 `WA_NoSystemBackground`+`WA_OpaquePaintEvent` 라
**Qt 가 그 영역을 절대 안 칠한다.** sink 는 새 버퍼가 올 때만 그린다. 그래서:

| 신설 | 하는 일 |
|---|---|
| `VideoBackend::expose()` | 탭 복귀 시 `gst_video_overlay_expose()` 로 마지막 프레임 재제시 |
| `VideoBackend::has_frame()` | 프레임이 없는 동안 `ChannelView` 가 **네이티브 창을 숨긴다** |
| `ChannelView::paintEvent` | 창이 비킨 자리에 배경 + `paint_chrome()` 을 직접 그린다 (GPU 합성과 **같은 그림 함수**) |

이걸로 두 가지가 해결됐다 — 탭 잔상, 그리고 **프레임이 없으면 "재접속 중" 문구도
안 보였던 문제**(`video_backend.h` 가 명시한 설계 목표 위반이었다).
⚠ `teardown()` 에서 `m_had_frame=false` 를 내려야 한다 — `stop()` 경로에 구멍이 있었다.

### F0-6. 세션 시작 시 키프레임 강제

`VideoBackend::session_started` → `ChannelView` → `LiveViewer::on_session_started` →
SUNAPI `setsynchronizationpoint`. 재접속마다 GOP 한 바퀴를 기다리던 것을 없앤다.
`status_changed` 로는 이 시점을 못 고른다 — "연결 중…"과 "재접속 중"이 둘 다
비어있지 않은 문구인데 후자는 **세션이 없다.** 채널당 2초 하한.

### F0-7. ffmpeg 종료 — `terminate()` 금지, `AVIOInterruptCB` 사용

`QThread::terminate()` 는 정리 코드를 통째로 건너뛰어 libav 컨텍스트를 전부 누수시키고
최악엔 CRT 힙 락을 쥔 채 죽어 **프로세스 전체**를 망가뜨린다. 근본 원인은 terminate 가
아니라 **중단 수단의 부재**였다 — 리포에 `AVIOInterruptCB` 가 0건이라 블로킹 중
`m_stop` 을 볼 방법이 없었다. 콜백을 `avformat_open_input` **前에** 걸고, 타임아웃을
넘겨도 죽이지 않고 스레드에 소유권을 넘긴다(`finished` → `deleteLater`).
실측: 망이 느려 RTSP 열기 실패인 상태에서 **종료 543ms.**

### F0-8. 방송 — RTP 전용 (MQTT/PCM 경로 제거, −319줄)

RPi C 담당자 합의. 전송방식 전환 버튼과 노캔 토글을 없앴다 — **노캔은 항상 켬**
(`denoise()` 기본값이 원래 `true` 였고 UI 만 껐다 켤 수 있었다. 탈출구로 레지스트리
`broadcast/denoise` 는 남김). 방송 행에는 **ON/OFF 만** 남는다.
MQTT 경로에만 있던 use-after-free(발행 실패 시 `QIODevice` 자기 신호 안에서 소유자
`QAudioSource` 를 파괴)가 **원인째 사라졌다.**
⚠ `shared/broadcast_protocol.h` 의 MQTT 상수 제거는 RPi C 정리 후에.

### F0-9. 노드 점과 경보 팝업은 **다른 기준**을 쓴다 (합치지 말 것)

| | 기준 | 이유 |
|---|---|---|
| 점 | 5초 즉시 | "지금 신선한가". `top_bar.cpp` 가 같은 기준이라 두 지시기가 어긋나지 않는다 |
| 팝업 | **연속 3회 ≈ 15초** | "장애다"라는 주장이고 걸쇠라 운영자를 붙잡는다 |

그리고 **브로커가 끊기면 RPi A 를 `Unknown` 으로 둔다**(Offline 아님) — 경로가 죽은
것이지 A 가 죽었는지는 알 수 없다. 점은 2색 규칙대로 여전히 빨강이라 **UI 에 "모름"
색을 새로 만들지 않는다.** 예전엔 여기서 `node_state_changed(A,false)` 를 쏴서
"RPI-A 끊김" 팝업이 **오탐으로** 떴다.

⚠ **더미 zone 을 신선도 판정에 넣지 말 것.** `tick_dummy()` 가 매초 가짜 타임스탬프를
찍으므로 세면 **하드웨어가 죽어도 영원히 초록**이다. `top_bar` 에는 이 가드가 있었고
`device_control_page` 에는 없어서 **두 지시기가 모순된 상태를 표시**하고 있었다.

### F0-10. 화재 중 액추에이터 잠금은 **UI 가 아니라 명령 지점**에서 막는다

`ACTUATOR_FIELDS` 5줄 중 `btn_on`/`btn_off` 를 가진 건 2줄뿐이다. 셔터는 버튼을
로컬 변수로 만들어 **잠금이 잡을 손잡이가 없었고**, `set`/`both` 의 SET 경로는 통째로
열려 있었다. 이제 셔터 버튼을 `ActuatorRow::btns_shutter` 에 보관하고 명령 위젯 전부를
잠근다. **그리고 `send_actuator()` 최상단에 백스톱을 둔다** — UI 잠금은 표면이다:
잠금 갱신보다 클릭이 먼저 올 수 있고, 새 명령 경로가 목록에서 빠질 수 있고(실제로
그랬다), 잠긴 위젯도 코드로는 `clicked` 를 낼 수 있다.

### F0-11. ⚠ `MqttLink::handle_message` 의 스냅샷을 지우지 말 것

`m_subs` 를 복사 없이 순회하면 핸들러가 `subscribe()` 를 다시 불러 append 할 수 있고
(`request()` → `ensure_reply_subscription()` 경로), `QList` 재할당으로 순회 중인 참조가
해제된 메모리를 가리킨다. `QList` 는 COW 라 스냅샷 비용은 **refcount 증가 1회**다.

---

## 부록 F — 2026-08-10 리뷰 수정 (시계·세션 회복·경보·핀)

> 4커밋: `6bc4c37` 시계 오염 · `47cd3e3` 세션 회복 · `977bb6e` 경보 · `159d9fe` 핀·정합.
> 서사와 근거는 Obsidian 「0810_리뷰 잔여 4묶음 수정과 카메라 경로 우회 규명 (VMS)」.

### F-1. 신규 파일 2개

| 파일 | 역할 |
|---|---|
| `alert_time.h` (헤더 전용) | 경보 payload 의 **사건 시각**과 신선도 판정 단일 원천. `alert_event_time()` · `alert_momentary_is_fresh()` |
| `alert_popup_stack.{h,cpp}` | 경보 팝업 세로 배치자(싱글턴). 팝업은 `add(this, Order)` 로 **등록만** 한다 |

### F-2. 시계 — 미래 타임스탬프를 어디서 거르나

`box_source.h` 에 허용치가 **둘** 있다. 헷갈리면 안 된다 — **기준 시계가 다르다.**

| 상수 | 값 | 무엇과 무엇을 비교하나 |
|---|---|---|
| `DETECTION_FUTURE_TOLERANCE_MS` | 5 s | 감지 `ts` ↔ 같은 응답의 `served_utc` (**카메라끼리**) |
| `DETECTION_PC_CLOCK_SKEW_MS` | 60 s | 카메라 시각 ↔ **PC 시계** (ONVIF 경로 · `TrackHistory`) |

⚠ 하나로 합치면 안 된다. **실측 카메라가 PC보다 3.3초 앞선다** — 5초로 묶으면 여유가
1.7초뿐이라 정상 ONVIF 박스가 전멸(록스텝 경로가 통째로 HTTP 폴백으로 강등)한다.

| 자리 | 동작 |
|---|---|
| `detection_feed.cpp` HTTP 경로 | 미래면 그 행을 버린다 (카메라끼리 비교라 안전) |
| `detection_feed.cpp` ONVIF 경로 | 미래면 **`UtcTime` 만 무효화** — 문서는 안 버린다. 기존 폴백 2개(도착 시각 대체 · 프레임 귀속 −1)가 받는다 |
| `track_history.cpp` `add()` | 문 앞에서 **기각**. 모든 공급원이 지나는 문이라 여기 백스톱이 있어야 새 경로가 생겨도 안 샌다 |

**왜 저장소에도 두는가**: `m_clock` 은 "본 적 있는 최대 시각"이라 되돌아오지 않는다.
미래 점 하나가 들어오면 ①`prune()` 컷오프가 밀려 **나머지 동선이 전부 삭제**되고
②그 트랙의 `last().ts` 가 미래가 되어 **이후 정상 점이 전부 기각**된다.

### F-3. 세션 회복 — 회복이 4겹이 됐다 (`direct_sink_backend.cpp`)

1. 버스 ERROR/EOS 폴링 · 2. 12초 무프레임 워치독 · 3. 지수 백오프 ·
**4. 지연 누적 가드 (신규)**

- **재접속 예약 취소는 `start_pipeline()` 맨 위**에 있다(`play()` 아님).
  세션이 시작되는 지점이 그 함수 하나뿐이라 새 호출부가 생겨도 저절로 지켜진다.
- **지연 누적은 절대 임계로 재지 않는다.** 원시 `glass` 값에는 카메라-PC 시계차가
  섞여 있어 ⓐ지금 방향이면 항상 음수라 **가드가 죽고**(07-31 재현) ⓑ반대 방향이면
  **상시 초과로 재시작 폭주**가 된다. → **세션 최솟값 대비 초과분**(`m_glass_floor_ms`)
  **+ 연속 3틱(9초)**(`m_creep_ticks`). 잡음(한 채널 폭 6.9초)이 임계(5초)보다 커서
  연속 조건이 필수다.
- 셋 다 `start_pipeline()` 에서 **리셋**한다. 안 하면 새 세션이 첫 프레임 전에 임계를
  넘어 재기동 루프가 된다.

### F-4. 경보 — 사건 시각과 팝업 자리

- **사건 시각**: `alert_time.h` 하나를 지난다. 캡은 **종류별로 다르다** —
  상태 경보(화재 확정/해제·혼잡 단계)는 **버리지 않고** 사건 시각만 정확히 싣고,
  점 사건(비명·총성·비상 버튼)만 60초 상한으로 버린다. *"1분 전에 확정된 화재는
  지금도 화재"* 이고, 지나간 총성을 지금 일로 띄우면 운영자가 빈 현장으로 달려간다.
  발행자 시계가 앞선 경우(나이 음수)는 **통과**시킨다 — 경보 억제는 위험한 쪽 오류다.
- `AlertFeed::audio_alert` 시그널에 **`const QDateTime &ts` 가 추가**됐다.
  `AudioAlertPopup::show_alert` 도 같이 바뀌었다.
- **팝업 자리는 각자 정하지 않는다.** 예전엔 팝업마다 y 오프셋 상수를 들고 있었고
  (혼잡 90 · 화재 160 · RPi 230), `rpi_alert_popup.cpp` 주석이 *"셋 다 고정 오프셋"*
  이라 적어뒀는데 **실제 팝업은 넷**이었다 — 빠진 음향 팝업이 화재와 **같은
  자리(160)** 에 떠 서로를 가렸다.
  → 생성자에서 `AlertPopupStack::instance()->add(this, Order)` 로 등록만 하고,
  `reposition()` 은 `relayout()` 호출로 축약. show/hide/**resize** 를 이벤트 필터로
  관찰하므로 재배치 호출을 잊어도 어긋나지 않는다. 순서는 `Order` enum
  (Congestion 10 · Fire 20 · Audio 30 · Device 40) — 08-10 이전 화면 순서 그대로다.

### F-5. TLS 핀 — 판정은 하나, 배선은 둘

`credentials.cpp` 의 `peer_pin_matches()` 가 **유일한 판정**이고,
`install_tls_pinning()` 이 그것을 `encrypted`(핸드셰이크 성공)와 `sslErrors`(검증
실패) **양쪽**에 건다. 예전엔 `sslErrors` 안에만 있어 **체인 검증을 통과한 인증서는
핀을 안 봤다.** 자세한 배경은 `SECURITY_SETUP.md` §0.

### F-6. 화면 정합 (`live_viewer.cpp`)

- `WallLayout::mode_changed` 핸들러가 `if / else if` 라 **풀스크린 → 다른 채널
  풀스크린** 전환에서 이전 채널이 고해상에 남았다(그리드 4MP 금지 위반).
  떠나는 채널을 **먼저** 내리고 새 채널을 올린다.
- 정원은 `ZoneConfig::capacity()` 로 통일 + `Notifier::changed` 재적용 (§3-4).

### F-7. 인수 시험 하네스 — `tools/acceptance.py` (⚠ 미커밋)

`net` / `devices` / `vms` / `latency` 4모드. 사용법과 함정은
`LAN_TEST_CHECKLIST.md` §3b. **리포에 커밋하지 않았다**(08-10 사용자 지시) —
공유폴더에만 있다.

---

## 부록 G — 2026-08-11 로그인·권한·mTLS (VEDA-186)

> 기획서 §7 로드맵 3·4·5 + 0c·0d + 핸드오프 §6 을 한 브랜치에서 끝냈다.
> 계약(토픽·payload·reason)의 정본은 Obsidian 「0811_로그인·권한 RPi B 핸드오프」,
> 리포 안의 정본은 `DB_LINK_AND_MQTT_MIGRATION.md` 의 로그인 3종·`endpoints` 절이다.
> 서버측 구현은 `origin/VEDA-185-Security_Procedures`.

### G-1. 신규 파일 2개

| 파일 | 역할 |
|---|---|
| `auth.{h,cpp}` | 로그인 상태·역할의 **단일 진실원천**(싱글턴). 스텁 백엔드와 자가시험을 함께 들고 있다 |
| `login_page.{h,cpp}` | 로그인 화면. 별도 창이 아니라 `MainWindow` 최상위 스택의 0페이지 |

### G-2. 화면 게이트는 두 겹이다 — 스택 + `go_to()`

`MainWindow` 에 최상위 `QStackedWidget` 이 하나 더 생겼다(0=로그인, 1=본 화면).
**본 화면은 로그인 전에 미리 만든다** — 늦게 만들면 ①이미 열려 있던 retained
경보를 팝업이 놓치고(`main.cpp` 의 구독 순서가 지키려던 것) ②진입할 때 RTSP
재연결을 기다린다.

미리 만들기 때문에 **경보 팝업이 로그인 화면 위로 뜰 수 있다**. 그래서 화면
이동을 `MainWindow::go_to()` 한 곳으로 모으고 거기서 잠근다 — 팝업 5곳이
각자 `setCurrentIndex` 를 부르고 있었고, 그게 전부 로그인 우회로였다.

### G-3. ⚠ 경보 팝업은 로그인 전에 뜨지 않는다 (`AlertPopupStack`)

보이는 것만의 문제가 아니다. **팝업의 `[확인]` 은 미확인 경보를 지우는 조작**이라
(화재·버튼 팝업은 `m_fire_pending` 이 확인 전까지 안 꺼진다), 로그인 안 한
사람이 누르면 **진짜 운영자는 그 경보를 영영 못 본다.**

- `set_gated(bool)` — 잠그면 이미 떠 있던 것도 숨긴다. **내부 상태는 안 건드린다**
- `try_show(QWidget*)` — 팝업은 `show(); raise();` 대신 이것을 부른다
- 잠금이 풀리면 **그때까지도 떠 있어야 하는 것만** 다시 뜬다(스스로 닫힌 것은 제외)

⚠ **뜬 뒤에 숨기는 방식으로는 안 된다.** 팝업들이 `if (!isVisible()) show()` 형태라
다음 갱신마다 다시 뜨려 한다 — 실측 12초에 34번, 창이 떴다 사라지며 로그인
화면에 잔상이 남았다. **뜨기 전에** 판정한다. `eventFilter` 의 Show 분기는
`try_show` 를 안 거친 팝업을 잡는 백스톱으로 남겼다(경고 로그).

### G-4. 권한 — `Auth::can(Action)` 하나로 판정한다

셋을 함께 본다: 로그인했는가 · **서버가 확인해 준 세션인가**(§4b 오프라인 유예는
읽기 전용) · 역할이 관리자인가. 기획서 §5 표의 모든 항목이 오늘은 관리자
전용이라 판정은 하나지만, `Action` 열거형은 호출부가 무엇을 묻는지 남기려고 둔다.

⚠ **잠금은 각 화면의 기존 활성 계산에 AND 로 넣는다.** 밖에서 `setEnabled` 를
따로 걸면 화재 잠금·전이 중 잠금·데이터 도착 여부와 서로 덮어써 **나중에 부른
쪽이 이긴다**. 자체 조건이 없는 버튼만 `Auth::bind()` 로 묶고, 그 계약을
`auth.h` 에 적어 뒀다. SETTINGS 는 활성 계산이 네 군데로 흩어져 있어
`refresh_write_enable()` 한 곳으로 모았다.

그리고 **명령이 나가는 지점 11곳에 백스톱**을 뒀다(`send_actuator` ·
`send_clear_fire` · `apply_row` · `apply_fire` · `BroadcastController::start` ·
`CameraControl` 3종 · `ImagePanel::send_set` · `SystemPanel` 백업/재부팅 ·
`ProfilePanel::apply_edit`). UI 잠금은 표면이다 — 부록 F0-10 과 같은 이유.

### G-5. 세션 — 저장·자동 로그인·오프라인 유예 72h

- 토큰은 `Credentials::protect()`(DPAPI)로 감싸 레지스트리에. 아이디·역할은
  비밀이 아니라 그대로 둔다(고쳐 봐야 오프라인 진입은 읽기 전용이고, 온라인이면
  서버 응답이 덮는다)
- 기동 시 `cmd/session_check`. `Auth::resume()` 을 **MainWindow 보다 먼저** 부른다 —
  나중에 부르면 로그인 폼이 한 프레임 그려졌다 사라져 §6a 의 "깜빡이지 않게"가 깨진다
- ⚠ **MqttLink 접속은 비동기라 기동 직후엔 항상 미연결이다.** 거기서 바로 유예로
  넘기면 브로커가 멀쩡한 날에도 매번 읽기 전용이 된다 → 6초 기다렸다 판단
- 유예 통과는 **읽기 전용**(`verified()==false`) + 상단 배너. 브로커가 돌아오면
  자동 재검증해 푼다 — 유예는 임시 상태여야 한다
- ⚠ **서버가 거절한 세션(`reason` 이 온 응답)은 유예로 살리지 않는다.** 만료된
  토큰도 마찬가지 — 그건 30일 정책을 우회하는 것이다

### G-6. mTLS (0c) — SAN 에 있는 이름/IP 로 붙어야 한다

**기본값이 곧 정상 설정이다 (08-12): `172.20.33.251:8883`, `[mqtt]` 섹션 불필요.**
기본 포트가 1883 이던 동안, 인증서를 받은 팀원 전원이 평문 리스너에 TLS 를
던지며 무한 재시도만 했다(화면엔 "연결 안 됨"뿐) — 그래서 기본값을 뒤집었다.
host 는 LAN IP 기본 — 서버 인증서 SAN 에 `DNS:rpib` 와 `IP:172.20.33.251` 이
둘 다 있어 검증을 통과한다(08-12 실측, `CONNACK 0`).

인증서 경로는 **ini 에 안 적는다** — `Credentials::certs_dir()`
(= 자격 파일 폴더 `/certs` = `AppData/Local/GuardX/certs`)에서
`ca.crt` · `<cn>.crt` · `<cn>.key` 를 찾는다 (`<cn>` = `vms-{hostname}`).
⚠ `QStandardPaths::AppLocalDataLocation` 을 그대로 쓰지 않았다 — 이 앱은
organization/application 이름을 세팅하지 않아 그 값이 실행 파일 이름으로 풀린다.
자격 파일과 같은 뿌리를 쓰면 경로가 한 곳에서 정해지고 자격과 인증서가 늘 같이 있다.
탈출구: `tls_ca/tls_cert/tls_key`(다른 자리) · `tls=0`(1883 롤백 — 포트도 함께).
⚠ **인증서가 없으면 평문으로 내려가지 않고 접속을 포기한다.** TLS 를 안 쓰는
경우는 `tls=0` 으로 **명시했을 때뿐**이다. 08-11 리팩터에서 잠깐 "파일 없으면
평문"이 됐다가 되돌렸다 — 그 상태는 8883 에 평문으로 붙으려다 rc=7 재시도만
돌아서, 화면이 비는데 원인이 안 보인다.

**⚠ client_id ≠ CN (08-12 분리).** 신원은 둘로 갈린다:
- **CN**(`MqttLink::cert_name()`) = `vms-{hostname}` 고정 = 인증서 파일명 = 계정.
  브로커가 `use_identity_as_username` 이라 계정·ACL 은 언제나 여기서 나온다
- **client_id**(`MqttLink::client_id()`) = MQTT **세션** id. 기본은 CN 과 같고
  `[mqtt] client_id` 로 오버라이드된다. 같은 PC 에서 앱 두 개를 병행(개발)할 때
  한쪽만 `vms-3-11-b` 처럼 주면 서로 세션을 안 끊는다 — 브로커는 CN 만 보므로
  계정은 안 바뀐다(08-12 실서버 확인). 두 인스턴스는 같은 ini 를 읽으니
  둘째는 `GUARDX_CREDENTIALS` 환경변수로 별도 ini 를 준다.
  `reply_topic()` 도 client_id 기반이라 응답이 안 섞인다

**접속 실패 이유는 화면이 말한다 (08-12).** `MqttLink::fault()` + `fault_changed()`
— 설정 수준의 오류(인증서 없음 · tls=0/8883 조합 · CONNACK 거절)만 담고, 로그인
카드 상태줄이 빨강으로 표시한다. "재시도 중"(노랑)과 다른 사건이라 색도 다르다.
⚠ 인증서 없음은 `start()` 가 조기 return 하는 경로라 `online_changed` 가 영영
안 온다 — 화면은 반드시 `fault_changed` 도 들어야 한다(`login_page.cpp`).

- `100.73.217.52`(Tailscale IP)·`rpib.tail…ts.net` 은 SAN 에 없어 실패한다
- `mosquitto_tls_insecure_set()` 을 쓰지 않는다 — 그걸 켜면 hostname 검증이 꺼져
  mTLS 의 절반이 사라진다
- ⚠ **실패해도 평문으로 폴백하지 않는다.** 자동 강등은 보안 설정이 조용히
  사라지는 가장 흔한 방식이다. 파일 존재를 먼저 확인한다 — `tls_set` 은 없는
  파일에도 성공하고 **접속할 때** 알 수 없는 오류로 죽는다
- TLS 면 username/password 를 보내지 않는다. ini 로 `tls_cert` 경로를 바꿔
  파일명이 CN 규약과 다르면 경고를 남긴다(ACL 이 엉뚱한 계정으로 걸리는 사고)
- ⚠ **개인키는 리포·공유폴더(OneDrive) 금지.** ini 엔 경로만. 설정 방법은
  `SECURITY_SETUP.md` §1

### G-7. 쓰기 명령의 토큰 (핸드오프 §6)

`set_zone` · `set_fire_threshold` · `set_actuator` 가 `token` 을 싣는다
(`Auth::attach_token`). `cmd/track_display` 는 면제다.

- ⚠ **토큰이 없으면 필드를 아예 넣지 않는다.** 서버는 "없음"만 과도기
  (`REQUIRE_TOKEN=0`)에 관대하고 **틀린 토큰은 언제나 거부**한다
- 거절은 **필드 이름으로 층이 갈린다**: `reason` 이면 권한·세션(`on_fail`),
  `error` 면 값 검증(`on_error`). 서버가 그렇게 답하도록 설계돼 있다
- `expired`·`disabled` 면 세션을 무효화해 로그인 화면으로 보낸다
  (`Auth::note_write_reject`)

그래서 `MqttLink::request()` 에 선택 인자 **`on_fail(QJsonObject)`** 가 생겼다 —
`ok:false` 를 문자열 하나로 줄이면 `reason`·`retry_after_s` 가 사라져 잠금
카운트다운을 만들 수 없다. 기존 호출부는 그대로다.

### G-8. `endpoints` 3단 폴백 (0d)

`guardx/db/rpib/endpoints`(retained) → `rpic_rtp_host`.
①이번 실행 수신값 → ②레지스트리 캐시 → ③컴파일 상수.

②가 있는 이유는 브로커가 죽어도 방송은 나가야 하기 때문이고(08-10 원칙),
①이 ②를 덮는 이유는 주소를 DB 로 옮긴 목적이 "IP 가 바뀌면 양쪽 재빌드"를
없애는 것이라 DB 가 정본이기 때문이다. 탈출구는 `broadcast/rtp_host_pin=1`.

⭐ 붙이자마자 실버그를 하나 잡았다 — 이 PC 의 캐시가 **브로커 주소**였다.
교정 코드가 `start()` 안에만 있어 방송 ON 을 누른 적 없는 PC 에서는 한 번도
돌지 않았다. 이제 기동만으로 바로잡힌다.

### G-9. 전역 QSS 추가분 (부록 D-8 의 연장)

| 이름 | 무엇 | 비고 |
|---|---|---|
| `#PrimaryBtn` | accent 채움 주 버튼 | 한 화면에 하나만. 글자색은 `@logoText` |
| `#LoginCard QLineEdit` | 로그인 입력칸 | ⚠ **카드 아래로 한정**한다 |

⚠ 전역 `QLineEdit` 에 걸면 SETTINGS 구역 이름 칸의 "바뀐 칸" **QPalette 표시가
조용히 죽는다**(QSS 가 걸리면 팔레트가 안 먹는 D-8 함정의 같은 얼굴).
토큰 `@accentHover`·`@alarm` 도 추가했다 — 치환은 길이 내림차순이라 안전하다.

### G-10. 검증 도구 — `gstream_VMS --auth-selftest`

GUI 없이 도는 자가시험(다른 1회성 플래그와 같은 자리). 스텁 모드 **46검사** ·
`auth/stub_offline=1` 모드 **9검사**(08-11 실측).

- ⚠ **`auth/stub` 의 기본값은 꺼짐이다**(08-11 mTLS 절체 후 뒤집었다). 그러니
  자가시험을 돌리려면 **먼저 켜야 한다**:
  `reg add HKCU\Software\GuardX\VMS\auth /v stub /t REG_DWORD /d 1 /f`
  안 켜고 돌리면 실경로로 판단하는데, 이 프로세스는 몇 초 살다 죽어서 브로커
  연결이 서기 전에 로그인이 나간다 → `unreachable` 로 **실패 5건 · 통과 3건**.
  시험이 깨진 게 아니라 스텁을 안 켠 것이다(첫 줄이 "스텁 미사용(실서버)")
- 스텁은 **계약과 같은 모양의 JSON** 을 만들어 같은 파서를 지난다 — 붙이는 날
  바뀌는 것은 전송 한 줄이다
- `auth/stub_offline` 은 오프라인 유예를 **72시간 기다리지 않고** 시험하는
  유일한 방법이다(세션을 심고 "마지막 확인 시각"을 밀어 본다)
- ⚠ 시험 순서 주의 — 잠금 시험이 `operator` 계정을 60초 잠그므로 운영자 권한
  시험을 그 **앞**에 둬야 한다(뒤에 뒀다가 거짓 통과할 뻔했다)

⚠ MQTT 를 손으로 시험할 때 **PowerShell 인자로 JSON 을 넘기지 말 것** — 따옴표가
먹혀 `{node_id:vms-3-11,…}` 이 도착하고, 폴러는 규약대로 조용히 무시한다.
`mosquitto_pub -f 파일` 을 쓴다. 이걸로 "서버가 죽었다"고 오진한 적이 있다.

### G-11. 비밀번호 변경 · 계정 관리 (§5b — 08-11 저녁, 08-12 확장)

계약은 `DB_LINK_AND_MQTT_MIGRATION.md` 의 `cmd/change_password`·`cmd/create_user` 절.
사용자 결정 2건: **시드 비밀번호는 강제로 바꾸게 한다 · 관리자가 남의 비밀번호를
재설정하는 경로는 두지 않는다.**

**강제 변경은 상태로 표현한다** — `Auth::State::MustChangePassword`.
`logged_in()` 이 false 라서 셸 진입·경보 팝업·`can()` 이 **한꺼번에** 막힌다.
"로그인은 됐지만 아직 아무것도 못 한다"를 불리언으로 두면 그 조건을 곳곳에
흩뿌려야 하고, 반드시 한 군데를 빠뜨린다.

| 자리 | 무엇 |
|---|---|
| `login_page.cpp` `build_change_card()` | 변경 폼. **강제·자발적이 같은 폼**이다(차이는 취소 버튼뿐) |
| `LoginPage::begin_change(forced)` | 자발적 진입점. `MainWindow` 가 칩 메뉴 신호를 받아 부른다 |
| `account_settings_card.*` | **SETTINGS ▸ Accounts 탭** 전체 — 만들기 · 목록 · 사용 중지/재개 (08-12 분리) |
| `zone_settings_page.cpp` `build_subtabs()` | General \| Accounts 세그먼트. `QTabWidget` 이 아니라 `#Segmented`+`QStackedWidget` |
| `auth.cpp` `change_password/create_user/list_users/set_user_enabled` | 명령 + 스텁 |

⚠ **변경 성공 시 서버가 기존 세션을 전부 무효화하므로, 응답의 새 토큰으로 갈아
끼워야 한다** — 안 하면 방금 바꾼 본인이 튕긴다.
⚠ `must_change` 는 **없으면 false 로 읽는다** — 서버가 아직 안 실어도 안 깨진다.
⚠ **비밀번호 정책은 폐지됐다 (08-12).** `password_policy_error()`·
`PASSWORD_MIN_BYTES` 와 호출부 5곳을 전부 지웠다. 서버가 거부하는 것은 **빈 값**
하나뿐이고 그때만 `weak_password` 가 온다. 화면이 자체 규칙을 하나라도 되살리면
"서버는 받아 주는데 화면만 거부"가 다시 생긴다.
⚠ **비밀번호 경로에 `trimmed()` 를 걸지 않는다.** 공백도 비밀번호이고 개수까지
보존된다(실기 확인). 자가시험 `⑦-3b` 가 이걸 지킨다 — 공백 3개로 바꾼 뒤 3개로는
들어가지고 1개로는 못 들어간다. 아이디·표시 이름은 반대로 깎는 것이 정상이다.
⚠ **초기 비밀번호는 고정**이다(`Auth::initial_password()`). 생성 폼에 비밀번호
칸이 없다 — 관리자가 값을 정하면 그 사람이 남의 비밀번호를 아는 상태가 된다.
소스에 박혀 git 에 공개되지만, 서버가 `must_change_pw=TRUE` 로 만들어 **그 값으로
할 수 있는 일은 비밀번호를 바꾸는 것뿐**이라 창이 좁다(그 전제가 깨지면 재검토).

⚠ **권한 잠금 규약** — 탭 버튼처럼 자체 활성 조건이 없는 위젯만 `Auth::bind()`,
[Create]·[Disable] 처럼 자체 조건이 있는 것은 `refresh_write_enable()` 안에서
`AND can()`. 밖에서 `setEnabled` 로 걸면 응답이 버튼을 되살리는 순간 권한 잠금이
함께 풀린다.
⚠ 08-12 실측: `#SegBtn` 에 `:disabled` 규칙이 없어 **잠근 탭이 안 잠긴 탭과
픽셀상 동일**했다(`theme.cpp` 에 한 줄 추가). QSS 를 건 위젯은 `QPalette::Disabled`
가 안 먹는다는 이 리포의 규칙이 세그먼트에서만 빠져 있었다.

⚠ 계정 목록(`cmd/list_users`)은 **서버 배포 대기**다(08-12). 스텁은 완성돼 있어
화면·자가시험이 전부 돌고, 실경로는 배포 전까지 "The server did not answer" 를
그대로 보여준다.
⚠ 서버가 `must_change_password` 로 쓰기를 거절하면(강제 변경 대상이 명령을 보낸
경우) 일반 실패로 흘리지 않고 **상태를 되돌려 변경 화면으로 보낸다** —
`note_write_reject()`. 그 응답이 왔다는 건 화면이 서버 판정과 어긋났다는 뜻이다.
**08-11 저녁 실서버 확인**: 시드 `admin` 로그인 응답에 `must_change:true` 가 실려
오고 화면이 곧바로 변경 폼으로 간다. 틀린 현재 비밀번호는 서버가
`bad_credentials` 로 거절한다. (실제 비밀번호 교체는 사용자 몫 — 값을 정하지 않았다.)

⚠ 사용자 칩은 **아이디를 항상 보여준다.** 실서버 시드 계정의 `display_name` 이
"관리자"라 표시 이름만 쓰면 칩이 `관리자 · 관리자` 가 되어 **어느 계정인지 알 수
없었다.** 표시 이름은 아이디·역할과 다를 때만 덧붙인다.

### G-12. 현장 전역 설정 — SITE 문구 · 캘리브레이션 (08-12)

계약은 `DB_LINK_AND_MQTT_MIGRATION.md` 의 `site_config`·`cmd/set_site_config` 절.
사용자 결정 ④: **배포 시 모든 사용자가 같은 환경을 본다** — 서버가 단일
진실원천이고 retained 로 뿌린다.

| 자리 | 무엇 |
|---|---|
| `site_config.{h,cpp}` | 수신·캐시·발행. `site_name()` 은 서버값 → 캐시 → 설계 기본값 순 |
| `site_settings_card.{h,cpp}` | SETTINGS 카드 (문구 편집 + 캘리브레이션). `zone_settings_page` 는 생성 한 줄만 |
| `calibration_store.cpp` `load_json()` | 파일/MQTT 공통 진입점. `load_file()` 은 읽어서 여기로 넘긴다 |
| `top_bar.cpp` · `login_page.cpp` · `report_page.cpp` | 문구를 읽는 세 곳. `site_name_changed()` 로 함께 바뀐다 |

**캘리브레이션에는 축이 둘이다.** 관리자 [Apply] = 전역(서버 발행 → retained →
전 VMS), 운영자 [Apply] = 이 PC·이 계정만(`QSettings` 오버라이드, 서버 무접촉).
오버라이드가 있는 동안 전역 수신은 캐시만 갱신하고 화면에는 안 올라간다.

- **[Load file...] 은 적용하지 않는다** — 파싱 검증까지만 하고 대기(staged).
  [Apply] 가 실제 적용이다. 잘못 고른 파일이 곧바로 평면도를 덮던 것을 막는다
- 캐시(`site/name`·`site/calibration_global`·`site/calibration_local_<계정>`)를
  기동 시 먼저 복원한다 — 브로커가 죽어 있어도 마지막 값으로 그린다
- ⚠ **검증 전에는 캐시에 쓰지 않는다.** 서버는 calibration 을 해석하지 않고 통짜
  보관하므로(계약) 여기 오는 값이 캘리브레이션이 아닐 수 있다 — 실제로 다른
  세션의 계약 시험 payload(`{"reply_to":…,"token":…}`)가 캐시에 굳어 재시작마다
  파싱 실패를 뱉었다. `CalibrationStore::is_valid()` 를 통과한 것만 남긴다
- ⚠ 검증은 **적용해 보는 것으로 대신할 수 없다** — 화면에 운영자 오버라이드가
  떠 있으면 그걸 밀어낸다. `is_valid()` 와 `load_json()` 이 **같은 규칙**을 쓴다
- ⚠ 없는 키 = "설정 안 됨"(계약). `site_name` 만 저장된 상태면 `calibration`
  필드 자체가 안 온다 — `contains()` 로만 판단하고 지우지 않는다

**08-12 실서버 확인**: 다른 세션이 발행한 retained 문구가 상단바·로그인 카드·
REPORT 에 재시작 없이 반영됐다. 운영자 [Apply] → `calibration_local_operator` 에
저장 → **앱 재시작 후에도 CROWD 평면도에 그대로** (전역 캐시는 안 건드림).

⚠ **스텁 로그인 + 실브로커 조합의 함정**: 스텁 토큰을 실서버가 `expired` 로
거절하고, `note_write_reject()` 가 그걸 세션 만료로 읽어 **로그아웃시킨다.**
쓰기 경로를 스텁으로 시험하면 매번 튕긴다 — 운영자 로컬 경로처럼 서버를 안 타는
쪽으로 시험하거나 실계정으로 로그인해야 한다.
