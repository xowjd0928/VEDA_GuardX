#include "theme.h"
#include "report_chart.h"   // ReportTk — REPORT 페이지 전용 팔레트도 함께 전환
#include "zone_config.h"

#include <QApplication>
#include <QGuiApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <algorithm>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmapCache>
#include <QSettings>
#include <QStyleHints>
#include <QVariant>
#include <QWidget>

#include <iterator>

namespace {

/**
 * @brief 리소스(:/fonts)의 IBM Plex를 앱 폰트로 등록한다
 *
 * 폰트 설치를 배포 조건에서 뺀다 — 어느 PC에서 실행하든 디자인 원본과 같은
 * 글자가 나온다 (핸드오프 §Assets). 리소스가 비어 있으면 아무 일도 안 하고
 * 아래 resolve_family가 시스템 폰트로 폴백한다.
 *
 * 굵기별 **정적** face를 각각 넣어야 한다 (가변 폰트 아님). Qt가 이들을
 * typographic family 하나로 묶어주므로 호출부는 그대로 setWeight()만 하면
 * 된다 — Qt 6.11 실측: Regular+SemiBold를 등록하면 "IBM Plex Mono"에
 * styles=[Regular, SemiBold]로 잡히고 setWeight(600)이 정확히 맞는다.
 * (설치된 폰트는 GDI 레거시 이름 때문에 "IBM Plex Mono SmBld"라는 별도
 * 패밀리로 보이는데, 앱 폰트 등록은 그 함정을 피해 간다.)
 */
bool load_bundled_fonts()
{
    QStringList loaded;
    const QFileInfoList files =
        QDir(":/fonts").entryInfoList({ "*.ttf", "*.otf" }, QDir::Files);
    for (const QFileInfo &fi : files) {
        if (QFontDatabase::addApplicationFont(fi.absoluteFilePath()) >= 0)
            loaded << fi.fileName();
        else
            qWarning() << "[Theme] 번들 폰트 등록 실패:" << fi.fileName();
    }
    if (loaded.isEmpty())
        // 경고로 올린다. 08-03~08-10 동안 이 줄이 info 로 조용히 찍히는 바람에
        // 번들 폰트가 한 번도 안 실린 것을 아무도 못 봤다 (CMake 의 alias 가
        // ":/fonts/fonts/…" 를 만들고 있었다). 무엇을 확인해야 하는지도 적는다.
        qWarning() << "[Theme] 번들 폰트 없음 — 시스템 폰트로 폴백."
                   << "VMS/fonts/*.ttf 가 있는데도 이 줄이 보이면 리소스 경로"
                      "(:/fonts 바로 아래여야 한다)와 CMakeLists 의 BASE 를 확인할 것";
    else
        qInfo() << "[Theme] 번들 폰트" << loaded.size() << "개 등록:"
                << loaded.join(", ");
    return true;
}

// IBM Plex가 있으면 그걸, 없으면 시스템 폰트로 폴백 (스펙 §1.2)
QString resolve_family(const QStringList &candidates)
{
    // 번들 폰트를 등록한 뒤에 목록을 본다. 지역 static이라 최초 1회만 돌고,
    // 어떤 폰트 조회보다도 먼저 실행되는 것이 보장된다 (Theme::apply 를
    // 언제 부르는지에 의존하지 않는다).
    static const bool fonts_ready = load_bundled_fonts();
    Q_UNUSED(fonts_ready)

    const QStringList installed = QFontDatabase::families();
    for (const QString &want : candidates)
        if (installed.contains(want, Qt::CaseInsensitive))
            return want;
    return candidates.last();
}

const QString &ui_family()
{
    static const QString family =
        resolve_family({"IBM Plex Sans", "Segoe UI"});
    return family;
}

const QString &mono_family()
{
    static const QString family =
        resolve_family({"IBM Plex Mono", "Cascadia Mono", "Consolas"});
    return family;
}

/**
 * @brief 전역 글자 크기 배율 (2026-08-06)
 *
 * 디자인 원본의 px 값은 149개 호출부에 흩어져 있다. 전체를 키우는 유일하게
 * 안전한 자리가 여기다 — 호출부의 상대적 크기 관계(9 < 10 < 11 …)를 그대로
 * 두고 한 번에 확대한다. 고정 높이는 대부분 24px 이상이라 이 배율에서
 * 잘리지 않는다. HTML에 직접 박힌 font-size와 글자를 담는 고정 크기 컨트롤도
 * 같은 배율을 타도록 Theme::px()를 쓴다.
 */
constexpr double FONT_SCALE = 1.15;

/**
 * @brief 한글 대체 폰트 (2026-08-10)
 *
 * IBM Plex 에는 한글이 없다(Sans·Mono 모두 — 실측: `가`와 `안`의 렌더 결과가
 * 완전히 같다 = 둘 다 .notdef). 그래서 한글은 항상 Qt 의 대체 폰트가 그린다.
 *
 * ⚠ **그 선택을 Qt 에 맡기면 안 된다.** 고정폭(mono) 계열을 요청하면 Qt 가
 * 굴림체 계열을 고르는데, 그 폰트들은 작은 크기에 **임베디드 비트맵**을 갖고
 * 있어 1px 뼈대로 그려진다 — 획이 끊기고 `안전`이 `안석`처럼 보인다. SETTINGS
 * 의 화재 임계 라벨이 그 상태였고, 같은 이유로 `mono_font` 로 찍은 한글은
 * 화면 어디서든 같이 깨져 있었다(하단 주석 포함). `ui_font` 쪽은 맑은 고딕
 * (아웃라인)으로 떨어져서 멀쩡했다 — 그래서 한 화면 안에서 갈렸다.
 *
 * 명시적으로 지정하면 라틴·숫자는 IBM Plex 가, 한글은 맑은 고딕이 그린다.
 */
const QString &korean_family()
{
    static const QString family = [] {
        const QStringList installed = QFontDatabase::families();
        for (const QString &want : { "Malgun Gothic", "맑은 고딕", "Noto Sans KR" })
            if (installed.contains(want, Qt::CaseInsensitive))
                return want;
        return QString();   // 없으면 Qt 기본 대체에 맡긴다 (기존 동작)
    }();
    return family;
}

QFont make_font(const QString &family, double px, int weight, double tracking_em)
{
    const double scaled = px * FONT_SCALE;
    QFont f;
    // 순서가 곧 우선순위다 — 앞 폰트에 없는 글자만 뒤로 넘어간다.
    if (korean_family().isEmpty())
        f.setFamily(family);
    else
        f.setFamilies({ family, korean_family() });
    f.setPixelSize(qRound(scaled));
    f.setWeight(QFont::Weight(weight));
    if (tracking_em > 0)
        f.setLetterSpacing(QFont::AbsoluteSpacing, scaled * tracking_em);
    return f;
}

} // namespace

namespace Theme {

QFont ui_font(double px, int weight, double tracking_em)
{
    return make_font(ui_family(), px, weight, tracking_em);
}

QFont mono_font(double px, int weight, double tracking_em)
{
    return make_font(mono_family(), px, weight, tracking_em);
}

int px(double design_px)
{
    return qRound(design_px * FONT_SCALE);
}

QColor occ_color(double load)
{
    return load > 0.70 ? alarm : load > 0.45 ? amber : green;
}

QColor occ_color(double load, double warn, double critical)
{
    return load >= critical ? alarm : load >= warn ? amber : green;
}

QColor occ_bar_color(double load)
{
    return load > 0.70 ? alarm : load > 0.45 ? amber : accent;
}

// 어두운 배경에서 서로 구분되는 6색.
//
// 0번은 amber(노랑)다 — 감지 박스의 기본색이 accent(파랑)이므로 파랑을
// 0번에 두면 "골랐는데 안 골라진 것처럼" 보인다. 다중 추적 전에도 선택
// 강조가 amber였던 이유가 이것이다. 그래서 accent는 팔레트에서 빼고
// alarm(빨강)도 뺀다 (화재 전용).
static const QColor TRACK_COLORS[] = {
    {0xFF, 0xB2, 0x24},  // amber  (= 종전 선택 강조색)
    {0x2F, 0xD2, 0x7D},  // green
    {0xB4, 0x8C, 0xFF},  // violet
    {0x4F, 0xD6, 0xD6},  // cyan
    {0xFF, 0x6F, 0xB5},  // pink
    {0xFF, 0x8A, 0x3D},  // orange — amber와 가장 가까운 쌍이므로 마지막
};

int track_color_count()
{
    return int(std::size(TRACK_COLORS));
}

QColor track_color(int slot)
{
    if (slot < 0)
        return accent;
    return TRACK_COLORS[slot % track_color_count()];
}

QPixmap mascot(int width_px)
{
    if (width_px <= 0)
        return {};

    // 물리 픽셀로 뽑는다 — 논리 px로 줄여 놓고 DPR만 박으면 200% 화면에서
    // 뭉갠다. 자산이 576px인 이유가 이것이다(최대 표시 폭 220 × 2).
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const int w = qRound(width_px * dpr);
    const QString key = QStringLiteral("dodam/%1").arg(w);

    QPixmap pm;
    if (!QPixmapCache::find(key, &pm)) {
        const QPixmap src(QStringLiteral(":/img/dodam_mascot.png"));
        if (src.isNull()) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                qWarning() << "[Theme] 마스코트 자산 없음 (:/img/dodam_mascot.png)"
                           << "— vms/assets/dodam_mascot.png 와 CMakeLists의"
                              "vms_img 블록(BASE \"assets\")을 확인할 것";
            }
            return {};
        }
        // 폭만 정하고 높이는 3:4로 계산한다. 두 값을 손으로 적으면 언젠가
        // 어긋난다 — 자산 자체가 정확히 3:4(576x768)라 늘어나지 않는다.
        pm = src.scaled(w, w * 4 / 3, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QPixmapCache::insert(key, pm);
    }
    // 캐시에는 DPR 1로 넣고 꺼낸 사본에만 박는다(setDevicePixelRatio가
    // detach하므로 캐시 원본은 그대로다).
    pm.setDevicePixelRatio(dpr);
    return pm;
}

qreal mascot_opacity()
{
    return mode() == Mode::Light ? 0.85 : 1.0;
}

QPixmap mascot_avatar(int diameter_px)
{
    if (diameter_px <= 0)
        return {};

    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const int d = qRound(diameter_px * dpr);
    // 배경 원·테두리가 팔레트를 타므로 키에 모드를 넣는다. 그림은 같지만
    // 캐시된 픽스맵에는 이미 그 색이 구워져 있다.
    const QString key = QStringLiteral("dodam-avatar/%1/%2")
                            .arg(d)
                            .arg(mode() == Mode::Light ? QLatin1Char('l')
                                                       : QLatin1Char('d'));

    QPixmap out;
    if (!QPixmapCache::find(key, &out)) {
        const QPixmap src(QStringLiteral(":/img/dodam_mascot.png"));
        if (src.isNull())
            return {};

        // 자산 안에서 얼굴이 있는 원. 정규화해 두는 이유는 자산 크기가 바뀌어도
        // 따라가게 하려는 것이다 — ⚠ 자산을 **다시 그리면** 이 셋을 다시 잰다
        // (공유폴더 make_mascot_asset.py 로 뽑은 576x768 기준).
        constexpr qreal FACE_CX = 0.380;   ///< 폭 대비 중심 x
        constexpr qreal FACE_CY = 0.205;   ///< 높이 대비 중심 y
        constexpr qreal FACE_R  = 0.300;   ///< 폭 대비 반지름 — 귀가 원에 안 닿게

        out = QPixmap(d, d);
        out.fill(Qt::transparent);

        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);

        // 반 픽셀 안쪽으로 — 그래야 테두리가 잘리지 않는다.
        const QRectF circle(0.5, 0.5, d - 1.0, d - 1.0);
        p.setPen(Qt::NoPen);
        p.setBrush(elevated);
        p.drawEllipse(circle);

        QPainterPath clip;
        clip.addEllipse(circle);
        p.setClipPath(clip);

        // 얼굴 원의 지름이 아바타 지름이 되도록 키우고, 그 중심을 가운데로.
        const qreal s = d / (2.0 * FACE_R * src.width());
        p.drawPixmap(QRectF(d / 2.0 - FACE_CX * src.width() * s,
                            d / 2.0 - FACE_CY * src.height() * s,
                            src.width() * s, src.height() * s),
                     src, QRectF(src.rect()));

        p.setClipping(false);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(border2, 1));
        p.drawEllipse(circle);
        p.end();

        QPixmapCache::insert(key, out);
    }
    out.setDevicePixelRatio(dpr);
    return out;
}

QString channel_name(int ch)
{
    // 구역 이름의 단일 진실원천은 DB(zones.zone_name)다. 여기는 "CH번호 · "
    // 접두만 붙인다 — 채널 번호는 화면 배치의 문제라 DB가 알 바 아니다.
    //
    // 아직 못 받았으면(첫 수신 전·브로커 정지) 번호만 보여준다. 예전엔 상수
    // 배열을 폴백으로 뒀지만, 그러면 운영자가 이름을 바꾼 뒤 브로커가 끊겼을
    // 때 화면이 *틀린 이름을 확신에 차서* 표시하게 된다. 비어 보이는 편이
    // 거짓말보다 낫다. 소비자(split_channel_name·zone_of)는 구분자가 없는
    // 문자열을 이미 처리한다.
    if (ch < 0 || ch >= 4)
        return QString("CH%1").arg(ch + 1);

    const QString zone = ZoneConfig::name(ch);
    return zone.isEmpty() ? QString("CH%1").arg(ch + 1)
                          : QString::fromUtf8("CH%1 · %2").arg(ch + 1).arg(zone);
}

int channel_cap(int ch)
{
    static const int caps[] = {60, 80, 40, 30};
    return (ch >= 0 && ch < 4) ? caps[ch] : 50;
}

// ---------------------------------------------------------------- 테마 전환

namespace {

Theme::Mode g_mode = Theme::Mode::Dark;
Theme::Pref g_pref = Theme::Pref::System;

/**
 * @brief 다크 = **워크스페이스** 디자인 (08-19 확정 — 디자인 캔버스 Option A)
 *
 * 출처: `GuardX Live Redesign` 캔버스 "Workspace Build-out" 페이지. 참조한
 * 전문 VMS 계열의 평평한 무채색 회색조로 전환 — 이전의 푸른 다크
 * (#0A0D12 계열)를 대체한다. 상태색(green/amber/alarm)과 정보색(accent
 * 파랑)은 유지, 브랜드·선택 표시는 라이트(3a)와 같은 **한화 오렌지**로
 * 통일한다 — 이제 두 테마 모두 "오렌지 = 브랜드/활성 탭, 파랑 = 정보,
 * 신호등 = 건강"으로 읽는다.
 */
void load_dark()
{
    Theme::bg0        = {0x0B, 0x0B, 0x0B};  ///< 페이지/월 배경
    Theme::panel      = {0x16, 0x16, 0x16};  ///< 카드
    Theme::elevated   = {0x1B, 0x1B, 0x1B};  ///< 사이드 패널·입력
    Theme::elevated2  = {0x24, 0x24, 0x24};  ///< 트랙·행 호버·선택
    Theme::border     = {0x26, 0x26, 0x26};
    Theme::border2    = {0x2E, 0x2E, 0x2E};
    Theme::borderDim  = {0x22, 0x22, 0x22};
    Theme::rowDivider = {0x22, 0x22, 0x22};
    Theme::textHi     = {0xEC, 0xEC, 0xEC};
    Theme::textMid    = {0xC9, 0xCD, 0xD2};
    // 하위 3단계의 계층(muted > dim > faint)과 대비 하한은 08-06 규칙 유지 —
    // 무채색으로 옮기며 각 단계의 명도는 비슷하게 맞췄다.
    Theme::textMuted  = {0x9A, 0x9F, 0xA6};
    Theme::textDim    = {0x7C, 0x82, 0x8A};
    Theme::textFaint  = {0x5F, 0x65, 0x6C};
    Theme::accent     = {0x4E, 0xA1, 0xFF};
    Theme::accentHover= {0x7D, 0xBB, 0xFF};
    Theme::alarm      = {0xFF, 0x4A, 0x36};
    Theme::green      = {0x2F, 0xD2, 0x7D};
    Theme::amber      = {0xFF, 0xB2, 0x24};

    // 크롬 = 본문보다 한 단계 어두운 면 + 오렌지 선택 (워크스페이스 타이틀 바)
    Theme::chromeBg       = {0x0C, 0x0C, 0x0C};
    Theme::chromeBorder   = {0x26, 0x26, 0x26};
    Theme::chromeElevated = {0x1B, 0x1B, 0x1B};
    Theme::chromeText     = {0xEC, 0xEC, 0xEC};
    Theme::chromeTextDim  = {0x8F, 0x95, 0x9C};
    Theme::chromeSelBg    = {0x1E, 0x1E, 0x1E};  ///< 활성 탭 면
    Theme::chromeSelText  = {0xE8, 0x87, 0x3A};  ///< 활성 탭 표시 (라이트와 동일)
}

/**
 * @brief 라이트 = 디자인 탐색안 **3a** (하이브리드 크롬 + 한화 오렌지)
 *
 * 출처: `VMS Light Mode.dc.html` §TURN 3 — "1c base + Hanwha orange sub color".
 * 4a(전면 라이트)와 다른 점은 **크롬을 어둡게 남긴다**는 것이다: 상단바와
 * 내비는 다크로 방향감·제품 정체성을 지키고, 작업 캔버스만 밝게 연다.
 *
 * ⚠ 오렌지는 **브랜드 표식·선택된 내비·활성 탭에만** 쓴다. 상태(건강)는
 * 초록/황/적 그대로다 — 오렌지가 상태색 자리를 침범하면 "주의"로 오독된다.
 */
void load_light()
{
    // 본문 면·선 (light canvas)
    Theme::bg0        = {0xEE, 0xF1, 0xF5};  ///< canvas
    Theme::panel      = {0xFF, 0xFF, 0xFF};  ///< 카드
    Theme::elevated   = {0xF8, 0xFA, 0xFC};  ///< 대체 행·입력
    Theme::elevated2  = {0xE9, 0xED, 0xF2};  ///< 트랙·행 호버
    Theme::border     = {0xD6, 0xDC, 0xE3};
    Theme::border2    = {0xC9, 0xD1, 0xD9};
    Theme::borderDim  = {0xE9, 0xED, 0xF2};  ///< 게이지 트랙
    Theme::rowDivider = {0xEE, 0xF1, 0xF5};

    // 글자 계층
    Theme::textHi     = {0x12, 0x17, 0x1D};
    Theme::textMid    = {0x44, 0x50, 0x5C};
    Theme::textMuted  = {0x5C, 0x67, 0x73};
    Theme::textDim    = {0x7D, 0x87, 0x94};
    Theme::textFaint  = {0xA9, 0xB2, 0xBC};

    // 브랜드 = 한화 오렌지 (절제해서 쓴다) · 상태색은 건강 전용
    Theme::accent     = {0xC8, 0x6A, 0x22};
    Theme::accentHover= {0xA2, 0x4A, 0x0D};
    Theme::alarm      = {0xB3, 0x2D, 0x1F};
    Theme::green      = {0x14, 0x7D, 0x55};
    Theme::amber      = {0xA8, 0x64, 0x00};

    // 크롬은 어둡게 유지 (3a의 핵심)
    Theme::chromeBg       = {0x12, 0x17, 0x1D};
    Theme::chromeBorder   = {0x2A, 0x32, 0x3B};
    Theme::chromeElevated = {0x1A, 0x20, 0x28};
    Theme::chromeText     = {0xFF, 0xFF, 0xFF};
    Theme::chromeTextDim  = {0x9A, 0xA4, 0xB1};
    Theme::chromeSelBg    = {0x1F, 0x19, 0x13};  ///< 선택된 내비 (오렌지 그늘)
    Theme::chromeSelText  = {0xE8, 0x87, 0x3A};  ///< 어두운 면에서 읽히는 오렌지
}

/**
 * @brief 전역 QSS — 색은 전부 팔레트에서 뽑는다 (모드마다 다시 만든다)
 *
 * ⚠ `%1` 식 번호 자리표시자를 쓰지 않는다. 토큰이 10개를 넘으면 `%14` 같은
 * 두 자리 표시자가 `%1` + "4"로 잘못 치환돼(그래서 색이 조용히 엉뚱해진다)
 * 원인을 찾기 어려운 버그가 된다 — 08-06에 실제로 겪었다. 이름 토큰으로 쓴다.
 */
QString build_qss()
{
    QString qss = R"(
        QToolTip {
            background: @elevated2; color: @textMid;
            border: 1px solid @border2; padding: 4px 8px;
        }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
        QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; }
        QScrollBar::handle {
            background: @border2; border-radius: 4px; min-height: 24px;
        }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

        /* 크롬(상단바·내비) — 본문과 다른 면일 수 있다 (3a 하이브리드) */
        #TopBar { background: @chromeBg; border-bottom: 1px solid @chromeBorder; }
        /* 08-19 워크스페이스: 탭 줄은 타이틀 바(TopBar) 안에 산다 — 면·경계는
           바가 그린다 */
        #NavRail { background: transparent; }
        #TopDivider { background: @chromeBorder; }
        /* 브랜드 마크 = 오렌지 (08-19 워크스페이스 — 두 테마 공통 chromeSelText) */
        #Logo {
            background: @chromeSelText; color: @chromeBg; border-radius: 3px;
        }
        #ChromePill {
            background: @chromeElevated; border: 1px solid @chromeBorder;
            border-radius: 3px; color: @chromeTextDim; padding: 0 10px;
        }
        #ChromeBtn {
            background: @chromeElevated; border: 1px solid @chromeBorder;
            border-radius: 3px; color: @chromeText; padding: 0 14px;
        }
        #ChromeBtn:hover { border-color: @chromeText; }

        /* 본문 크롬 */
        #Pill {
            background: @elevated; border: 1px solid @border; border-radius: 3px;
            color: @textMuted; padding: 0 10px;
        }
        /* 이름 없는 QPushButton 의 기본 모양 (08-12).
           맨 버튼은 windowsvista 스타일이 **바탕을 시스템 색**으로 그리고
           글자는 **앱 팔레트**로 그린다 — Windows 라이트 + 앱 다크 조합에서
           흰 글자 on 흰 바탕이 된다(캘리브레이션 [불러오기] 버튼 실사고).
           개별 위젯을 고치지 않고 여기서 한 번에 거는 이유는 QMessageBox 와
           같다 — 앞으로 생길 맨 버튼도 자동으로 따라온다.
           이름 붙은 규칙(#OutlineBtn 등)은 ID 특이성이 높아 안 바뀐다.
           ⚠ :checked 를 빼먹으면 안 된다 — CROWD 의 CH·프리셋 버튼이
           이름 없이 checkable 이라, native 체크 표시가 사라진 자리를
           여기가 대신 채워야 한다. */
        QPushButton {
            background: @elevated; border: 1px solid @border2;
            border-radius: 3px; color: @textMuted; padding: 4px 12px;
        }
        QPushButton:hover { border-color: @textFaint; color: @textHi; }
        QPushButton:pressed { background: @border2; }
        QPushButton:checked {
            background: @border2; color: @textHi; border-color: @textFaint;
        }
        QPushButton:disabled { color: @textFaint; border-color: @borderDim; }

        #OutlineBtn {
            background: transparent; border: 1px solid @border2;
            border-radius: 3px; color: @textMuted; padding: 0 14px;
        }
        #OutlineBtn:hover { border-color: @textFaint; color: @textHi; }
        /* QSS 규칙이 한 번 걸리면 QPalette::Disabled 는 더 이상 먹지 않는다 —
           비활성 표시를 여기서 직접 해 주지 않으면 꺼진 버튼이 켜진 것과
           똑같아 보인다(방송 ON/OFF·전송방식 버튼에서 실제로 그랬다). */
        #OutlineBtn:disabled { color: @textFaint; border-color: @borderDim; }
        /* 숫자 입력칸 — 스타일이 없으면 OS 기본 모양이라 라이트 테마의 흰 카드
           위에서 칸 경계가 거의 사라진다(QPalette::Base가 카드와 거의 같은 밝기).
           버튼(#OutlineBtn)과 같은 언어로 맞춘다: 한 단 위 면 + border2 테두리
           + 3px(4a 컨트롤 반경). 위/아래 화살표는 손대지 않는다 — subcontrol을
           건드리기 시작하면 이미지까지 직접 넣어야 해서 스타일마다 깨진다. */
        QAbstractSpinBox {
            background: @elevated; border: 1px solid @border2; border-radius: 3px;
            color: @textMid; padding: 2px 6px;
            selection-background-color: @accent; selection-color: @bg0;
        }
        /* 포커스 = 오렌지 (08-19 보드 — 활성/선택 강조는 앱 전체에서 오렌지) */
        QAbstractSpinBox:focus { border-color: @chromeSelText; }
        QAbstractSpinBox:disabled { color: @textFaint; border-color: @borderDim; }
        /* ⚠ 위/아래 **버튼(subcontrol)은 건드리지 않는다.** 한 번이라도
           스타일을 주면 Qt는 그 자리에 기본 화살표를 더 이상 그리지 않아
           화살표가 통째로 사라진다(실측). CSS 삼각형(폭·높이 0 + 테두리)도
           Qt에서는 삼각형이 아니라 사각 블록으로 그려져 못 쓴다. 이미지로
           넣으면 색이 테마를 못 따라온다. 그래서 칸(면·테두리)만 우리 것으로
           맞추고, 화살표는 OS 스타일에 맡긴다. */
        #Segmented {
            background: @elevated; border: 1px solid @border; border-radius: 3px;
        }
        /* 08-19 워크스페이스: 선택된 세그(존 칩 등)는 오렌지 테두리로 —
           투명 테두리를 기본에 깔아 두어 체크 시 크기가 변하지 않는다 */
        #SegBtn {
            background: transparent; border: 1px solid transparent;
            border-radius: 2px; color: @textDim; padding: 0 12px;
        }
        #SegBtn:checked {
            background: @elevated2; color: @textHi;
            border: 1px solid @chromeSelText;
        }
        /* ⚠ :disabled 를 반드시 함께 정의한다 — QSS 를 걸면 QPalette::Disabled 가
           안 먹는다(#PrimaryBtn·#OutlineBtn 과 같은 이유). 08-12 실측: 이게 없어서
           권한으로 잠근 SETTINGS ▸ Accounts 탭이 **안 잠긴 탭과 픽셀상 동일**했다.
           눌러도 아무 일이 없는데 이유는 툴팁을 얹어야만 보이는 화면이 된다. */
        #SegBtn:disabled { color: @textFaint; }
        /* 주 버튼 — accent 채움. 한 화면에 **하나만** 둔다(로그인의 [로그인]).
           글자색은 로고와 같은 @logoText 라 accent 가 밝은 테마(3a 오렌지)에서
           흰 글자가 뜨는 일이 없다. #OutlineBtn 과 같은 이유로 :disabled 를
           반드시 함께 정의한다 — QSS 를 걸면 QPalette::Disabled 는 안 먹는다. */
        #PrimaryBtn {
            background: @accent; border: 1px solid @accent; border-radius: 3px;
            color: @logoText; padding: 0 14px;
        }
        #PrimaryBtn:hover { background: @accentHover; border-color: @accentHover; }
        #PrimaryBtn:disabled {
            background: @elevated; border-color: @borderDim; color: @textFaint;
        }
        /* 입력칸 — **전역**으로 건다 (08-19).
           예전엔 `#LoginCard` 아래로만 한정했는데, 그러면 나머지 QLineEdit 이
           스타일 없이 남아 **OS 팔레트**로 그려진다. Windows 가 라이트 테마인
           PC 에서는 그게 흰 상자가 되어, 우리 어두운 카드 위에 흰 입력칸이
           뜬다(08-19 팀원 화면 실측: SETTINGS 의 Site label·Create Account).
           맨 QPushButton·QMenu·QComboBox 를 전역으로 칠한 것과 **같은 병**이다.
           ⚠ zone_settings_page 의 "바뀐 칸" 표시는 QPalette 를 쓰고 있었는데
             QSS 가 팔레트를 이기므로, 그쪽은 인라인 스타일시트로 옮겼다
             (mark_changed 참조 — 인라인이 전역보다 우선한다). */
        QLineEdit {
            background: @elevated; border: 1px solid @border2; border-radius: 3px;
            color: @textHi; padding: 4px 8px;
            selection-background-color: @accent; selection-color: @bg0;
        }
        QLineEdit:focus { border-color: @chromeSelText; }
        QLineEdit:disabled { color: @textFaint; border-color: @borderDim; }
        QLineEdit:read-only { color: @textMuted; }
        /* 여러 줄 입력(장비 로그 뷰어 등)도 같은 병을 앓는다 */
        QPlainTextEdit, QTextEdit {
            background: @elevated; border: 1px solid @border2; border-radius: 3px;
            color: @textMid;
            selection-background-color: @accent; selection-color: @bg0;
        }
        /* 로그인 카드는 조금 더 큰 여백 — 위 전역 규칙을 덮어쓴다 */
        #LoginCard QLineEdit {
            background: @elevated; border: 1px solid @border2; border-radius: 3px;
            color: @textHi; padding: 6px 10px;
            selection-background-color: @accent; selection-color: @bg0;
        }
        #LoginCard QLineEdit:focus { border-color: @chromeSelText; }
        #LoginCard QLineEdit:disabled { color: @textFaint; border-color: @borderDim; }
        #Panel, #LoginCard {
            background: @panel; border: 1px solid @border; border-radius: 4px;
        }
        #PanelHeaderLine { background: @borderDim; }
        #TimelineScroll, #TimelineScroll > QWidget > QWidget {
            background: transparent; border: none;
        }
        /* 페이지 스크롤 (REPORT·CAMERA) — 뷰포트가 카드 뒤에 회색 판을 깔지
           않게. REPORT는 자기 팔레트로 같은 규칙을 한 번 더 덮어쓴다. */
        QScrollArea#RScroll { background: transparent; border: none; }
        #RScroll > QWidget > QWidget { background: transparent; }
        /* 전체 페이지를 감싸는 스크롤 (DEVICE·SETTINGS — mainwindow 08-12).
           ⚠ RScroll 처럼 투명으로 두면 안 된다 — 페이지 뒤는 어두운 크롬이라
           라이트 테마에서 페이지만 검게 남는다(실측). 앱 배경색으로 칠한다. */
        QScrollArea#PageScroll { background: @bg0; border: none; }
        #PageScroll > QWidget > QWidget { background: @bg0; }
        /* 08-19: TimelineRow 의 :hover 강조를 없앴다 — 클릭되지 않는 행이
           호버에 반응하면 "클릭할 수 있는 것"처럼 읽힌다 (클릭 가능/불가
           구분 규칙). 호버 배경은 실제 버튼(#SegBtn 등)에만 남긴다. */

        /* 대화창 — 기본 QMessageBox는 OS 크롬(밝은 배경·시스템 버튼)이라
           앱 안에서 혼자 튄다. 패널과 같은 배경·테두리, OutlineBtn과 같은
           버튼 모양으로 맞춘다. 개별 호출부가 아니라 여기서 한 번에 거는
           이유는 앞으로 추가될 대화창도 자동으로 따라오게 하기 위함이다.
           (VEDA-174가 hex로 넣은 것을 @토큰으로 옮겼다 — 안 그러면 라이트
           테마에서 대화창만 검게 남는다.) */
        /* 메뉴 — QMessageBox 와 같은 병(08-12 실사고: 상단바 로그아웃 메뉴).
           규칙이 없으면 시스템 색 바탕에 앱 팔레트 글자가 얹혀, Windows
           라이트 + 앱 다크에서 흰 글자 on 흰 바탕이 된다. ::item 은 표준
           subcontrol 이라 스핀박스 화살표 함정(위 ⚠)과 달리 안전하다. */
        QMenu {
            background: @panel; border: 1px solid @border2;
            padding: 4px; color: @textMid;
        }
        QMenu::item {
            background: transparent; padding: 6px 24px 6px 12px;
            border-radius: 2px;
        }
        QMenu::item:selected { background: @elevated2; color: @textHi; }
        QMenu::item:disabled { color: @textFaint; }
        QMenu::separator { height: 1px; background: @borderDim; margin: 4px 8px; }

        /* 콤보박스 — 같은 병의 세 번째 얼굴. 칸은 QAbstractSpinBox 와 같은
           언어로, 펼친 목록은 QMenu 와 같은 언어로. ::drop-down subcontrol 은
           건드리지 않는다(화살표가 사라지는 스핀박스 함정과 같다). */
        QComboBox {
            background: @elevated; border: 1px solid @border2; border-radius: 3px;
            color: @textMid; padding: 2px 8px;
        }
        QComboBox:focus { border-color: @chromeSelText; }
        QComboBox:disabled { color: @textFaint; border-color: @borderDim; }
        QComboBox QAbstractItemView {
            background: @panel; border: 1px solid @border2;
            color: @textMid;
            selection-background-color: @elevated2; selection-color: @textHi;
        }

        QMessageBox, QDialog {
            background: @panel; border: 1px solid @border;
        }
        QMessageBox QLabel, QDialog QLabel { color: @textMid; }
        QMessageBox QPushButton, QDialog QPushButton {
            background: transparent; border: 1px solid @border2;
            border-radius: 2px; color: @textMuted;
            padding: 6px 18px; min-width: 68px;
        }
        QMessageBox QPushButton:hover, QDialog QPushButton:hover {
            border-color: @textFaint; color: @textHi;
        }
        /* 기본 버튼(엔터로 눌리는 쪽)만 강조색 — 어느 쪽이 안전한 선택인지
           눈으로 구분되게 한다 */
        QMessageBox QPushButton:default, QDialog QPushButton:default {
            border-color: @accent; color: @textHi;
        }
    )";

    const QHash<QString, QColor> tok = {
        { "@bg0", Theme::bg0 },               { "@panel", Theme::panel },
        { "@elevated", Theme::elevated },     { "@elevated2", Theme::elevated2 },
        { "@border2", Theme::border2 },       { "@borderDim", Theme::borderDim },
        { "@border", Theme::border },
        { "@textHi", Theme::textHi },         { "@textMid", Theme::textMid },
        { "@textMuted", Theme::textMuted },   { "@textDim", Theme::textDim },
        { "@textFaint", Theme::textFaint },   { "@accent", Theme::accent },
        { "@accentHover", Theme::accentHover },   { "@alarm", Theme::alarm },
        { "@chromeBg", Theme::chromeBg },     { "@chromeBorder", Theme::chromeBorder },
        { "@chromeElevated", Theme::chromeElevated },
        { "@chromeText", Theme::chromeText }, { "@chromeTextDim", Theme::chromeTextDim },
        // ⚠ QSS 에서 쓰는 토큰은 **반드시 여기 등록**해야 한다 — 미등록 토큰은
        //   "@이름" 문자열이 그대로 남아 그 규칙만 조용히 죽는다 (08-19 실사고:
        //   #Logo 가 @chromeSelText 를 썼는데 미등록이라 G 마크가 투명했다).
        { "@chromeSelBg", Theme::chromeSelBg },
        { "@chromeSelText", Theme::chromeSelText },
        // 로고 글자 — accent가 어두우면 흰 글자, 밝으면 어두운 글자
        { "@logoText", Theme::accent.lightness() < 140 ? QColor(Qt::white)
                                                       : Theme::bg0 },
    };
    // 긴 이름부터 치환 — "@border"가 "@borderDim"을 잘라먹지 않게
    QStringList names = tok.keys();
    std::sort(names.begin(), names.end(),
              [](const QString &a, const QString &b) { return a.size() > b.size(); });
    for (const QString &n : names)
        qss.replace(n, tok.value(n).name());
    return qss;
}

QPalette build_palette()
{
    QPalette pal;
    pal.setColor(QPalette::Window, Theme::bg0);
    pal.setColor(QPalette::WindowText, Theme::textHi);
    pal.setColor(QPalette::Base, Theme::elevated);
    pal.setColor(QPalette::AlternateBase, Theme::elevated2);
    pal.setColor(QPalette::Text, Theme::textHi);
    pal.setColor(QPalette::Button, Theme::elevated);
    pal.setColor(QPalette::ButtonText, Theme::textMid);
    pal.setColor(QPalette::Highlight, Theme::accent);
    pal.setColor(QPalette::HighlightedText, Theme::bg0);
    pal.setColor(QPalette::ToolTipBase, Theme::elevated2);
    pal.setColor(QPalette::ToolTipText, Theme::textMid);
    pal.setColor(QPalette::PlaceholderText, Theme::textDim);
    pal.setColor(QPalette::Disabled, QPalette::Text, Theme::textFaint);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, Theme::textFaint);
    return pal;
}

/** @brief 팔레트 값만 갈아끼운다 (적용·알림은 호출부) */
void load_palette(Theme::Mode m)
{
    if (m == Theme::Mode::Light)
        load_light();
    else
        load_dark();
    ReportTk::load_mode(m == Theme::Mode::Light);
}

} // namespace

Notifier *notifier()
{
    static Notifier n;
    return &n;
}

Mode mode()
{
    return g_mode;
}

Pref preference()
{
    return g_pref;
}

Mode system_mode()
{
    // Qt 6.8+ — OS의 밝기 설정. Unknown이면 제품 기본값(다크)으로 본다.
    if (auto *hints = QGuiApplication::styleHints())
        return hints->colorScheme() == Qt::ColorScheme::Light ? Mode::Light
                                                              : Mode::Dark;
    return Mode::Dark;
}

namespace {

/** @brief 선택 → 실제 모드 (System이면 OS에 물어본다) */
Theme::Mode effective(Theme::Pref p)
{
    return p == Theme::Pref::System ? Theme::system_mode()
         : p == Theme::Pref::Light  ? Theme::Mode::Light
                                    : Theme::Mode::Dark;
}

/** @brief 팔레트를 실제로 갈아끼우고 화면 전체에 알린다 */
void switch_to(Theme::Mode m)
{
    if (m == g_mode)
        return;
    g_mode = m;
    load_palette(m);

    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        app->setPalette(build_palette());
        app->setStyleSheet(build_qss());
    }

    // 스타일시트에 색을 구워 넣은 위젯들에게 다시 만들라고 알린다
    Theme::notifier()->notify();

    // paintEvent에서 색을 읽는 위젯(내비·타일·차트·게이지)은 재도색이면 충분.
    // 전역 QSS 재적용이 대부분 유발하지만, 커스텀 페인터까지 확실히 훑는다.
    for (QWidget *w : QApplication::allWidgets())
        w->update();

    qInfo() << "[Theme] 화면 테마 →" << (m == Theme::Mode::Light ? "light" : "dark");
}

} // namespace

void set_preference(Pref p)
{
    g_pref = p;
    QSettings("GuardX", "VMS")
        .setValue("theme_mode", p == Pref::Light  ? "light"
                              : p == Pref::Dark   ? "dark"
                                                  : "system");
    switch_to(effective(p));
    notifier()->notify();  // 선택 자체가 바뀌었을 수 있다 (설정 화면 체크 상태)
}

void restyle(QWidget *w, std::function<QString()> make_qss)
{
    if (!w)
        return;
    w->setStyleSheet(make_qss());

    // 같은 위젯에 restyle을 다시 걸면 **앞의 연결을 끊고** 새로 건다 (2026-08-10).
    //
    // 갱신 경로에서 부르는 곳이 있다: TrackingPanel::refresh_strip()은 추적 중
    // 250ms마다, CameraPage::show_toast()/ProfilePanel·ImagePanel::show_result()
    // 는 메시지가 뜰 때마다 같은 라벨에 다시 건다. 그냥 connect만 하면 위젯
    // 하나에 연결과 캡처된 std::function이 무한히 쌓이고(추적 1시간 ≈ 1.4만 개),
    // 테마를 한 번 토글하는 순간 그게 전부 동기로 실행돼 GUI가 멎는다.
    // 고치는 자리는 호출부가 아니라 여기다 — 호출부를 하나씩 손보면 다음에
    // 새로 생기는 갱신 경로에서 같은 사고가 또 난다.
    //
    // 장부는 QHash<QWidget*, Connection>이 아니라 **위젯의 동적 속성**에 둔다.
    // 위젯이 언제 죽든 속성도 함께 죽으므로 dangling 키도 stale 연결도 원천적으로
    // 없다. QHash면 destroyed()를 따로 걸어 지워야 하고, 그 연결 자체가 또 관리할
    // 장부가 된다. 연결도 w를 context로 잡으므로 위젯 소멸 시 Qt가 알아서 끊는다.
    static constexpr const char *CONN_PROP = "theme_restyle_conn";

    const QVariant prev = w->property(CONN_PROP);
    if (prev.isValid())
        QObject::disconnect(prev.value<QMetaObject::Connection>());

    const QMetaObject::Connection conn =
        QObject::connect(notifier(), &Notifier::changed, w,
                         [w, make_qss = std::move(make_qss)] {
                             w->setStyleSheet(make_qss());
                         });
    w->setProperty(CONN_PROP, QVariant::fromValue(conn));
}

void on_theme_changed(QWidget *context, std::function<void()> fn)
{
    if (!context)
        return;
    QObject::connect(notifier(), &Notifier::changed, context,
                     [fn = std::move(fn)] { fn(); });
}

bool chrome_is_dark()
{
    // 밝기 판정 하나로 "크롬 위에 어떤 상태색을 쓸지"가 갈린다
    return chromeBg.lightness() < 128;
}

namespace OnVideo {

QColor occ_color(double load, double warn, double critical)
{
    return load >= critical ? alarm : load >= warn ? amber : green;
}

} // namespace OnVideo

void apply(QApplication &app)
{
    // 08-19 저녁: **워크스페이스 다크 고정** (사용자 확정). 테마 선택 카드
    // 삭제와 함께 레지스트리 theme_mode 도 더 이상 읽지 않는다 — 어떤 값이
    // 저장돼 있든 다크로 뜬다. 라이트 팔레트 코드는 남겨 둔다(재도입 대비);
    // 아래 OS colorScheme 추종 연결은 g_pref 가 System 일 수 없어 죽은
    // 배선이지만, 재도입 시 그대로 살아나므로 두 줄 아끼자고 지우지 않는다.
    g_pref = Pref::Dark;
    g_mode = Theme::Mode::Dark;
    load_palette(g_mode);

    // OS 설정이 바뀌면 따라간다 — 단 "시스템 따름"일 때만.
    // 사용자가 직접 고른 것을 OS가 뒤엎지 않는다.
    QObject::connect(app.styleHints(), &QStyleHints::colorSchemeChanged,
                     notifier(), [](Qt::ColorScheme) {
                         if (g_pref == Pref::System)
                             switch_to(system_mode());
                     });

    app.setPalette(build_palette());
    app.setFont(ui_font(12));
    app.setStyleSheet(build_qss());
}

} // namespace Theme
