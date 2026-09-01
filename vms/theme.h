#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <QPixmap>
#include <QString>

#include <functional>

class QApplication;
class QWidget;

/**
 * @brief GuardX VMS 디자인 시스템 — docs/GuardX VMS.dc.html 원본에서 추출
 *
 * 색·폰트·화면 상수를 한 곳에 모은다. 모든 위젯은 여기 상수만 참조한다
 * (hex를 위젯 코드에 흩뿌리지 않는다 — 팔레트 교체가 한 파일로 끝나게).
 *
 * **팔레트는 상수가 아니다 (2026-08-06)**. `set_mode()`가 다크/라이트 두 벌
 * 사이를 오간다. `paintEvent`에서 읽는 곳은 다시 그려지기만 하면 자동으로
 * 따라오고, **스타일시트에 색을 구워 넣은 곳**은 `restyle()`로 등록해야
 * 테마 변경 때 다시 만들어진다 (§restyle 주석 참조).
 */
namespace Theme {

// ---- 팔레트 (UI_REDESIGN_SPEC §1.1) ----------------------------------------
// ⚠ const가 아니다 — set_mode()가 값을 갈아끼운다. 색을 캡처해 두지 말고
//    쓸 때마다 읽을 것 (그래야 테마 전환이 반영된다).
inline QColor bg0        {0x0A, 0x0D, 0x12};  ///< 앱 배경 (가장 어두움)
inline QColor panel      {0x0D, 0x11, 0x18};  ///< 패널, 상단바, 내비 레일
inline QColor elevated   {0x12, 0x16, 0x1F};  ///< 필, 입력, 진행 트랙
inline QColor elevated2  {0x16, 0x1C, 0x25};  ///< 활성 내비 배경, 행 호버
inline QColor border     {0x1C, 0x23, 0x30};  ///< 기본 테두리
inline QColor border2    {0x23, 0x2B, 0x38};  ///< 구분선, 컨트롤 테두리
inline QColor borderDim  {0x16, 0x1C, 0x25};  ///< 패널 헤더 밑줄
inline QColor rowDivider {0x10, 0x14, 0x1B};  ///< 리스트 행 구분선
inline QColor textHi     {0xE2, 0xE8, 0xF2};
inline QColor textMid    {0xC6, 0xCE, 0xDB};
inline QColor textMuted  {0x8B, 0x96, 0xA8};
inline QColor textDim    {0x5A, 0x65, 0x77};
inline QColor textFaint  {0x3A, 0x45, 0x57};
inline QColor accent     {0x4E, 0xA1, 0xFF};  ///< 링크, 트랙, 감지 박스
inline QColor accentHover{0x7D, 0xBB, 0xFF};
inline QColor alarm      {0xFF, 0x4A, 0x36};  ///< 화재/경보
inline QColor green      {0x2F, 0xD2, 0x7D};  ///< OK / 온라인
inline QColor amber      {0xFF, 0xB2, 0x24};  ///< 경고

// ---- 크롬 (상단바·내비 레일) -----------------------------------------------
/**
 * 본문과 **다른 면**일 수 있어 따로 둔다. 3a 같은 하이브리드 테마는 크롬을
 * 어둡게 두고 작업 캔버스만 밝게 가져간다 — 그때 상단바 글자를 본문 팔레트로
 * 칠하면 검은 글자가 검은 바에 얹힌다.
 * 크롬과 본문이 같은 면인 테마(다크·4a)에서는 두 값이 같아 차이가 없다.
 */
inline QColor chromeBg;        ///< 상단바·내비 배경
inline QColor chromeBorder;    ///< 크롬 구분선·테두리
inline QColor chromeElevated;  ///< 크롬 위의 칩·버튼 배경
inline QColor chromeText;      ///< 크롬 위 주 글자
inline QColor chromeTextDim;   ///< 크롬 위 보조 글자
inline QColor chromeSelBg;     ///< 내비 선택 배경
inline QColor chromeSelText;   ///< 내비 선택 글자·좌측 마커

/**
 * @brief 크롬이 어두운 면인가
 *
 * 어두우면 크롬 위의 상태색은 **밝은 쪽(OnVideo 팔레트)** 을 써야 한다 —
 * 흰 배경용으로 눌러 놓은 초록/황은 검은 바 위에서 읽히지 않는다.
 */
bool chrome_is_dark();

// ---- 화면 테마 (다크 / 라이트) ---------------------------------------------

enum class Mode { Dark, Light };

/**
 * @brief 사용자의 선택 — System이면 OS 설정을 그대로 따른다
 *
 * 기본값은 System이다. Windows가 다크면 다크, 라이트면 라이트로 뜨고
 * 사용자가 OS 설정을 바꾸면 앱도 따라 바뀐다. 사용자가 다크/라이트를
 * 직접 고르면 그 선택이 OS보다 우선한다(고른 것을 되돌리지 않는다).
 */
enum class Pref { System, Dark, Light };

/** @brief 지금 실제로 그려지고 있는 모드 */
Mode mode();

/** @brief 저장된 선택 (레지스트리 `theme_mode`: system|dark|light) */
Pref preference();

/** @brief OS가 알려주는 현재 색 구성 (알 수 없으면 Dark) */
Mode system_mode();

/**
 * @brief 선택 저장 + 즉시 적용
 *
 * 재시작이 필요 없다. 앱 팔레트/전역 QSS를 다시 적용하고 `notifier()`로
 * 알린 뒤, 열려 있는 모든 최상위 창을 다시 그리게 한다.
 */
void set_preference(Pref p);

/**
 * @brief 테마가 바뀌었다는 신호원 (ZoneConfig::Notifier와 같은 패턴)
 *
 * Theme은 함수 묶음이라 시그널을 낼 수 없어 이것만 QObject다.
 * `paintEvent`에서 색을 읽는 위젯은 구독할 필요가 없다(재도색으로 충분).
 * **이미 만들어진 스타일시트**를 든 위젯만 구독하면 된다.
 */
class Notifier : public QObject
{
    Q_OBJECT

public:
    void notify() { emit changed(); }

signals:
    void changed();
};

/** @brief connect(Theme::notifier(), &Theme::Notifier::changed, ...) */
Notifier *notifier();

/**
 * @brief 스타일시트를 테마에 묶는다 — 지금 한 번 적용하고, 바뀔 때마다 다시
 *
 * 색을 구워 넣은 `setStyleSheet(...)` 호출을 이걸로 감싸면 테마 전환이
 * 반영된다. 위젯을 context로 연결하므로 위젯이 죽으면 자동 해제된다.
 *
 * **위젯당 1개만 남는다** — 같은 위젯에 다시 걸면 앞의 등록을 대체한다.
 * 그래서 갱신 함수 안(추적 스트립·토스트·결과 라벨)에서 매번 불러도 안전하다.
 *
 * ```cpp
 * Theme::restyle(label, [label] {
 *     return QString("color:%1;").arg(Theme::textDim.name());
 * });
 * ```
 */
void restyle(QWidget *w, std::function<QString()> make_qss);

/**
 * @brief 테마 변경 때 다시 부를 갱신 함수를 등록한다 (스타일시트 밖의 것)
 *
 * 위젯이 이미 "전부 다시 그리는" 메서드를 갖고 있으면 그걸 걸어두는 편이
 * 호출부마다 restyle()을 다는 것보다 싸다.
 */
void on_theme_changed(QWidget *context, std::function<void()> fn);

/**
 * @brief 영상 위에 그리는 크롬 전용 색 — **테마를 따르지 않는다**
 *
 * 영상 타일의 배경은 임의의 장면이라 밝기가 제각각이다. 라이트 테마의
 * 어두운 글자를 얹으면 밝은 장면에서 사라지므로, 타일 안쪽(타임스탬프·
 * OCC 배지·감지 박스·채널명)은 어느 모드에서든 다크 팔레트 값을 쓴다.
 */
namespace OnVideo {

inline const QColor bg0      {0x0A, 0x0D, 0x12};  ///< 배지 위 글자색(어두운 글자)
inline const QColor textHi   {0xE2, 0xE8, 0xF2};
inline const QColor textMuted{0x8B, 0x96, 0xA8};
inline const QColor border2  {0x23, 0x2B, 0x38};
inline const QColor accent   {0x4E, 0xA1, 0xFF};
inline const QColor alarm    {0xFF, 0x4A, 0x36};
inline const QColor green    {0x2F, 0xD2, 0x7D};
inline const QColor amber    {0xFF, 0xB2, 0x24};

/** @brief 영상 위 OCC 배지용 — Theme::occ_color의 고정 팔레트 판 */
QColor occ_color(double load, double warn, double critical);

} // namespace OnVideo

// ---- 폰트 ------------------------------------------------------------------
/** @brief UI 폰트 (IBM Plex Sans → Segoe UI 폴백). tracking_em = letter-spacing */
QFont ui_font(double px, int weight = 400, double tracking_em = 0.0);
/** @brief 데이터/모노 폰트 — 숫자·코드·타임스탬프·채널명·엔드포인트 전부 */
QFont mono_font(double px, int weight = 400, double tracking_em = 0.0);

/**
 * @brief 디자인 원본 px → 실제 화면 px (글자 배율과 동일)
 *
 * `ui_font`/`mono_font`가 안에서 쓰는 것과 **같은 배율**이다. 두 군데서 쓴다.
 * - HTML 조각의 `font-size:10px` — 안 쓰면 위젯만 커지고 그 안의 span은 작게 남는다.
 * - **글자를 담는 고정 크기 컨트롤**(콤보·스핀·버튼 폭) — 글자만 키우면 잘린다.
 *
 * 화면 골격(상단바 48px·레일 84px·카드 radius)에는 쓰지 않는다 — 4a 지오메트리
 * 규격은 글자 크기와 무관하게 유지한다.
 */
int px(double design_px);

// ---- 점유율 -> 색 규칙 -----------------------------------------------------
/** @brief 타일 OCC 배지용: load > .70 alarm · > .45 amber · else green */
QColor occ_color(double load);
/** @brief 구역별 임계(zone_thresholds)를 쓰는 버전 */
QColor occ_color(double load, double warn, double critical);
/** @brief 예측 바용: load > .70 alarm · > .45 amber · else accent */
QColor occ_bar_color(double load);

// ---- 다중 추적 색 ----------------------------------------------------------
/**
 * @brief 동시 추적 대상 구분색 (slot 0..track_color_count()-1)
 *
 * 동선 패널의 선과 영상 타일의 감지 박스가 같은 색을 써야 "저 선이 저 사람"이
 * 읽힌다. 그래서 팔레트를 여기 한 곳에 둔다.
 *
 * accent(파랑)와 alarm(빨강)은 넣지 않는다 — 파랑은 미선택 감지 박스의
 * 기본색이라 선택 표시가 안 되고, 빨강은 화재/경보 전용이라 오인을 부른다.
 * 0번은 amber로, 다중 추적 전 선택 강조색과 같다.
 */
QColor track_color(int slot);
int track_color_count();

// ---- 마스코트 도담 ---------------------------------------------------------
/**
 * @brief 도담 픽스맵 — **폭만 주면 높이는 3:4 로 따라온다**
 *
 * 화면 배율(devicePixelRatio)만큼 크게 뽑아 DPR을 박아서 돌려주므로 호출부는
 * 논리 px만 준다 (`label->setPixmap(Theme::mascot(220))`). 같은 폭은
 * QPixmapCache에 남아 두 번 축소하지 않는다 — 다섯 자리가 각자
 * `QPixmap(":/img/…").scaled(…)`를 부르면 테마를 바꿀 때마다 다시 줄인다.
 *
 * 리소스가 없으면 **null 픽스맵**을 돌려준다. 자리가 안 나오면 안 넣는 것이
 * 배치 기획이므로, 호출부는 `isNull()`이면 위젯을 숨기고 레이아웃에서 뺀다.
 *
 * ⚠ 상태 표시에 쓰지 않는다 — 정상/경보를 그림으로 구분하는 변형은 만들지
 * 않는다(색·아이콘·문구가 이미 말한다). 경보 팝업·영상 타일·화재 임계·
 * DEVICE 조작 화면에는 넣지 않는다.
 */
QPixmap mascot(int width_px);

/**
 * @brief 라이트 테마에서만 0.85 (다크는 1.0)
 *
 * 흰 면 위에서는 방패의 어두운 금속이 무겁게 떨어진다. 자리별 불투명도
 * (빈 상태 0.25 등)에 **곱해서** 쓴다: `p.setOpacity(0.25 * Theme::mascot_opacity())`.
 * 그림 자체는 건드리지 않으므로 mascot()의 캐시는 테마가 바뀌어도 유효하다.
 */
qreal mascot_opacity();

/**
 * @brief 얼굴만 원형으로 오려낸 아바타 (로그인 카드 브랜드 줄)
 *
 * 전신은 카드 옆에 세울 자리가 없어 카드 **안**으로 들어갔다(08-13 결정 —
 * 배치 기획 §3① 의 "카드 안에 넣지 않는다"를 뒤집은 것이다. 방패의 `guardX`
 * 로고가 워드마크와 겹치는 문제는 얼굴만 오려내 방패를 거의 빼면서 사라진다).
 *
 * 자산은 전신 한 장뿐이고 여기서 오려낸다 — 아바타용 두 번째 PNG 를 만들면
 * 둘이 언젠가 갈린다. 배경 원과 테두리는 팔레트를 타므로 캐시 키에 테마가
 * 들어간다(그림 자체는 같다).
 */
QPixmap mascot_avatar(int diameter_px);

// ---- 채널 상수 (디자인 목업의 채널명/수용 인원) ----------------------------
QString channel_name(int ch);   ///< "CH1 · LOBBY EAST" 등
int channel_cap(int ch);        ///< 채널별 수용 인원 (OCC n/cap의 cap)

// ---- 적용 ------------------------------------------------------------------
/** @brief 앱 전역 팔레트 + QSS + 기본 폰트 적용. main()에서 show 전에 호출 */
void apply(QApplication &app);

} // namespace Theme
