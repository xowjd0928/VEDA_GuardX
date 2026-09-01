#include "alert_feed.h"
#include "auth.h"
#include "camera_status.h"
#include "credentials.h"
#include "mainwindow.h"
#include "mqtt_link.h"
#include "site_config.h"
#include "theme.h"
#include "zone_config.h"
#include "zone_display_link.h"

#include <QApplication>
#include <QHash>
#include <QSettings>
#include <QDebug>
#include <QMessageBox>
#include <QScreen>

/**
 * @brief 화면 배율 반올림 정책 — **QApplication 생성 전에** 정해야 한다
 *
 * ⚠ 2026-08-10 실사고. 노트북 내장 패널(125%)에서 **글자가 뭉개져 다른 글자로
 * 보였다** — `안전`이 `안석`처럼. 외부 모니터(100%)에서는 멀쩡했다.
 *
 * 원인은 폰트가 아니라 **분수 배율**이다. devicePixelRatio 가 1.25 면 위젯이
 * 놓인 위치가 device pixel 경계에 딱 떨어지지 않고, 어긋난 위젯의 글자만
 * 안티에일리어싱 없이 하드 글리프로 그려진다. 그래서 **같은 화면 안에서**
 * 어떤 라벨은 멀쩡하고 어떤 라벨은 깨지는, 폰트로는 설명이 안 되는 모양이 된다.
 * (실측: 깨진 글자 최대밝기 205·밝은픽셀 88 vs 멀쩡한 글자 118·847 —
 *  전자가 AA 없는 글리프의 지문이다.)
 *
 * 배율을 정수로 반올림하면 그 어긋남이 사라진다. 1.25 → 1.0.
 *
 * ⚠ 대가: 125% 화면에서 앱이 그만큼 **작아진다**. 크게 쓰고 싶으면 레지스트리
 * `dpi_rounding` 을 `ceil`(1.25→1.5)로 두면 된다. 화면마다 취향이 갈리는
 * 값이라 재빌드 없이 바꾸도록 열어뒀다 — 이 리포의 다른 A/B 스위치와 같은 방식.
 *
 * 값: round(기본) · ceil · floor · roundpreferfloor · passthrough(Qt 기본, 분수 허용)
 */
static void apply_dpi_rounding_policy()
{
    // QApplication 이전이라 QSettings 에 조직/앱 이름을 직접 준다.
    const QString mode = QSettings("GuardX", "VMS")
                             .value("dpi_rounding", "round").toString().toLower();

    using P = Qt::HighDpiScaleFactorRoundingPolicy;
    const QHash<QString, P> table = {
        { "round",            P::Round },
        { "ceil",             P::Ceil },
        { "floor",            P::Floor },
        { "roundpreferfloor", P::RoundPreferFloor },
        { "passthrough",      P::PassThrough },
    };
    const auto it = table.constFind(mode);
    if (it == table.cend()) {
        qWarning().noquote()
            << QString("[main] dpi_rounding=\"%1\" 은 모르는 값 — round 로 진행")
                   .arg(mode);
        QGuiApplication::setHighDpiScaleFactorRoundingPolicy(P::Round);
        return;
    }
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(it.value());
    qInfo().noquote() << QString("[main] 화면 배율 반올림: %1").arg(mode);
}

int main(int argc, char *argv[])
{
    // ⚠ 반드시 QApplication 보다 먼저. 뒤에서 부르면 Qt 가 무시한다.
    apply_dpi_rounding_policy();

    QApplication a(argc, argv);

    const QStringList args = a.arguments();

    // 자격 파일의 비밀값을 DPAPI로 암호화하고 종료 (설치 시 1회)
    if (args.contains("--encrypt-credentials")) {
        QString error;
        const bool ok = Credentials::encrypt_config_file(&error);
        if (!ok)
            qCritical().noquote() << error;
        return ok ? 0 : 1;
    }

    // 카메라 인증서 지문을 조회해 고정하고 종료 (신뢰 개시, 1회)
    if (args.contains("--pin-camera-cert")) {
        if (!Credentials::load())
            qWarning().noquote() << Credentials::last_error();
        QString fingerprint, error;
        if (!Credentials::pin_current_camera_cert(&fingerprint, &error)) {
            qCritical().noquote() << error;
            return 1;
        }
        qInfo().noquote() << "카메라 인증서 고정 완료 (SHA-256):" << fingerprint;
        qInfo().noquote() << "이제 SUNAPI/메타데이터 요청이 HTTPS로 나갑니다.";
        return 0;
    }

    // 로그인 경로 자가시험 후 종료 (화면 없이 — 개발/CI용)
    if (args.contains("--auth-selftest")) {
        // 실서버 경로로 시험할 때만 브로커가 필요하다. 스텁이면 자격 파일이
        // 없어도 돌아야 한다 — 화면을 만들기 전에 쓰는 도구다.
        if (!Auth::stub_enabled()) {
            if (!Credentials::load())
                qWarning().noquote() << Credentials::last_error();
            MqttLink::instance()->start();
        }
        return Auth::selftest();
    }

    if (!Credentials::load()) {
        // 비밀번호가 없으면 조용히 실패시키지 않는다 — 원인을 명확히 알린다
        qCritical().noquote() << "[Credentials]" << Credentials::last_error();
        QMessageBox::critical(nullptr, "GuardX VMS — 설정 필요",
                              Credentials::last_error());
        return 1;
    }

    // MQTT 브로커 접속 시작 (비동기 — 브로커가 죽어 있어도 여기서 안 멈춘다)
    MqttLink::instance()->start();

    // 구역 정원/임계 구독 등록. 값은 여기서 오지 않고 retained 메시지로
    // 곧 도착한다 — 그때까지는 Theme 기본값으로 그려진다 (fail-soft).
    ZoneConfig::init();

    // 현장 전역 설정(SITE 문구·캘리브레이션) — 같은 이유로 위젯 생성 전에.
    // 캐시를 먼저 복원하므로 브로커가 죽어 있어도 마지막 값으로 그려진다.
    SiteConfig::instance()->init();

    // 혼잡 경보 구독도 위젯 생성 전에 걸어둔다 — retained 스냅샷이 첫 화면이
    // 그려지기 전에 도착해야 이미 열린 critical을 놓치지 않는다.
    AlertFeed::instance();

    // Feed the field LED floor plan with the synthetic readings the dummy
    // zones already produce. Wired before the widgets because this path
    // depends on ZoneSensorStore alone - the LED fills in even if the
    // operator never opens the DEVICE page.
    ZoneDisplayLink::instance()->start();

    // 카메라 자원·앱 상태 폴링 개시 — top_bar 전역 표시용이라 위젯이 아니라
    // 여기서 깨운다 (탭 가시성과 무관하게 상시).
    CameraStatus::instance();

    // 디자인 시스템 적용 (팔레트 + QSS + 폰트) — 위젯 생성 전에
    Theme::apply(a);

    // 저장된 세션으로 자동 로그인 시도 (§4a). **MainWindow 보다 먼저** 불러야
    // 로그인 화면이 폼 대신 스플래시로 뜬다 — 뒤에서 부르면 폼이 한 프레임
    // 그려졌다가 사라져 "깜빡이지 않게"(§6a) 가 깨진다.
    Auth::instance()->resume();

    MainWindow w;
    w.show();
    // 배율·화면 진단 한 줄 — "창이 화면보다 크다/작다" 신고가 오면 이 줄부터
    // 본다. 창 최소 크기는 각 페이지 레이아웃의 합이라 코드만 봐선 모른다.
    if (QScreen *s = w.screen())
        qInfo().noquote()
            << QString("[main] 화면 %1x%2 · 작업영역 %3x%4 · 배율 %5 · 창 %6x%7 · 최소 %8x%9")
                   .arg(s->geometry().width()).arg(s->geometry().height())
                   .arg(s->availableGeometry().width())
                   .arg(s->availableGeometry().height())
                   .arg(s->devicePixelRatio())
                   .arg(w.width()).arg(w.height())
                   .arg(w.minimumSizeHint().width())
                   .arg(w.minimumSizeHint().height());
    return QCoreApplication::exec();
}
