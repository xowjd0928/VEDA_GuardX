#include "camera_page.h"
#include "app_card.h"
#include "camera_control.h"
#include "credentials.h"
#include "image_panel.h"
#include "network_panel.h"
#include "panel_chrome.h"
#include "profile_panel.h"
#include "sunapi_request.h"
#include "system_panel.h"
#include "theme.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QTimer>

#include <QAuthenticator>
#include <QButtonGroup>
#include <QDateTime>
#include <QDebug>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace {

/** @brief 자원 % → 색 (top_bar와 같은 언어 — >70 alarm · >45 amber) */
QColor metric_color(double v, bool npu)
{
    if (v > 70) return Theme::alarm;
    if (v > 45) return Theme::amber;
    return npu ? Theme::accent : Theme::green;
}

/**
 * @brief 앱별 의존성 경고 (기획서 §3-A — "이 앱을 끄면 무엇이 죽는가")
 *
 * 빈 문자열 = 의존성 없음(정지 안전). 앱 목록은 실측 4개 기준이고,
 * 모르는 앱은 보수적으로 일반 경고를 준다.
 */
QString dependency_warning(const QString &app_id)
{
    if (app_id == QLatin1String("test"))
        return "Stopping this removes the live detection boxes - it is the "
               "source of the /tracks (re-id) and ONVIF box pipelines.";
    if (app_id == QLatin1String("juan_application"))
        return "Stopping this cuts the poller, the forecast and all DB writes.";
    if (app_id == QLatin1String("WiseAI"))
        return "All AI analytics stop - this is the source of every detection "
               "and every metadata stream.";
    if (app_id == QLatin1String("test_calibration"))
        return QString();  // 실험용 — 정지 안전 (Start/Stop 검증은 이 앱으로만)
    return "The dependencies of this app are unverified - stop it with care.";
}

/** @brief Stop 확인 다이얼로그 — 위험색 테두리 + amber 의존성 박스 (§5-D) */
bool confirm_stop(QWidget *parent, const QString &app_id)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QString("Stop app - %1").arg(app_id));
    dlg.setModal(true);
    dlg.setStyleSheet(QString("QDialog { background:%1; border:2px solid %2; }")
                          .arg(Theme::bg0.name(), Theme::alarm.name()));

    auto *col = new QVBoxLayout(&dlg);
    col->setContentsMargins(24, 20, 24, 16);
    col->setSpacing(14);

    auto *title = new QLabel(
        QString("Stop <b>%1</b>?").arg(app_id), &dlg);
    title->setFont(Theme::ui_font(13));
    Theme::restyle(title, [=] {
        return QString("color:%1;").arg(Theme::textHi.name());
    });
    col->addWidget(title);

    const QString warning = dependency_warning(app_id);
    auto *warn = new QLabel(&dlg);
    warn->setWordWrap(true);
    warn->setFont(Theme::ui_font(11));
    if (warning.isEmpty()) {
        warn->setText("No dependencies - an experimental app, safe to stop.");
        Theme::restyle(warn, [=] {
            return QString("color:%1; padding:8px;").arg(Theme::textDim.name());
        });
    } else {
        warn->setText(QString("⚠ %1").arg(warning));
        Theme::restyle(warn, [=] {
            return QString("color:%1; background:%2; border:1px solid %1;"
                    " border-radius:2px; padding:10px;")
                .arg(Theme::amber.name(), Theme::elevated.name());
        });
    }
    col->addWidget(warn);

    auto *btns = new QDialogButtonBox(&dlg);
    auto *cancel = btns->addButton("Cancel", QDialogButtonBox::RejectRole);
    auto *stop = btns->addButton("■ Stop", QDialogButtonBox::AcceptRole);
    cancel->setDefault(true);  // 기본 = 취소 (엔터로 사고 안 나게)
    Theme::restyle(stop, [=] {
        return QString("QPushButton { border:1px solid %1; color:%1;"
                                " background:transparent; padding:5px 14px; }")
                            .arg(Theme::alarm.name());
    });
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    col->addWidget(btns);

    return dlg.exec() == QDialog::Accepted;
}

} // namespace

/**
 * @brief 원형 아크 게이지 + 숫자 롤링 + 스파크라인 (기획서 §5-D)
 *
 * 정적 막대 금지 — 값 전이는 550ms 보간으로 굴러가고, 아래 스파크라인이
 * 최근 60초의 흐름("치솟는 중인가")을 보여준다. 탭이 숨겨져 있는 동안엔
 * 애니메이션을 돌리지 않고 값만 쌓는다.
 */
class ResourceGauge : public QWidget
{
public:
    ResourceGauge(const QString &label, bool npu, QWidget *parent)
        : QWidget(parent), m_label(label), m_npu(npu)
    {
        setFixedSize(150, 186);
        m_anim = new QVariantAnimation(this);
        m_anim->setDuration(550);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
        m_anim->setStartValue(0.0);
        m_anim->setEndValue(1.0);
        connect(m_anim, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &v) {
                    m_shown = m_from + (m_to - m_from) * v.toDouble();
                    update();
                });
    }

    void set_value(double v)
    {
        m_to = qBound(0.0, v, 100.0);
        m_valid = v >= 0;

        m_hist.append(m_to);
        while (m_hist.size() > 60)
            m_hist.removeFirst();

        if (!isVisible()) {  // 숨김 중엔 보간 생략 — 타이머 안 돌림
            m_shown = m_to;
            return;
        }
        m_from = m_shown;
        m_anim->stop();
        m_anim->start();
    }

    /** @brief 링크 상태에 따른 흐림 정도 */
    enum Fade {
        Live,      ///< 정상 — 임계 색상 그대로
        Stale,     ///< 폴 1~2회 실패 — **색은 유지**하고 투명도만 낮춘다
        Offline,   ///< 접근 불가 — 값을 모른다는 뜻이므로 회색
    };

    /**
     * @brief 흐림 단계 설정
     *
     * Stale에서 색을 통째로 죽이면 "지금 얼마나 높은가"라는 신호가 사라진다.
     * 낡았다는 것과 모른다는 것은 다르다 — 낡은 값은 색을 유지한 채 흐리게,
     * 모르는 값만 회색으로.
     */
    void set_fade(Fade f)
    {
        if (m_fade == f)
            return;
        m_fade = f;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // ---- 아크 (180° 반원 — 08-19 보드) ----
        // 보드(Camera.dc.html)는 viewBox 120×70 에 "M12 62 A50 50 0 0 1 108 62",
        // 즉 위로 볼록한 **반원**이다. 270°(열린 쪽 아래)는 그 이전 세대 값이라
        // 같은 수치가 더 꽉 찬 것처럼 보였다.
        const QRectF arc(25, 22, 100, 100);
        const int start = 180 * 16, full = -180 * 16;
        // 트랙은 hairline-soft — 4a에서 게이지 트랙은 #EEF1F4다
        // (다크에선 elevated2와 같은 값이라 보이는 변화 없음)
        QPen track(Theme::borderDim, 9, Qt::SolidLine, Qt::RoundCap);
        p.setPen(track);
        p.drawArc(arc, start, full);

        QColor c = metric_color(m_to, m_npu);
        if (m_fade == Offline || !m_valid)
            c = Theme::textDim;
        else if (m_fade == Stale)
            c.setAlphaF(0.45);          // 색(=부하 수준)은 남기고 힘만 뺀다
        if (m_valid) {
            QPen val(c, 9, Qt::SolidLine, Qt::RoundCap);
            p.setPen(val);
            p.drawArc(arc, start, int(full * m_shown / 100.0));
        }

        // ---- 숫자 (아크 중앙) ----
        p.setPen(m_fade == Live ? Theme::textHi
                                : (m_fade == Stale ? Theme::textMuted
                                                   : Theme::textDim));
        // 보드는 값만 19/700 로 적고 % 글리프는 쓰지 않는다 — 라벨(CPU·MEM·
        // NPU)이 이미 단위를 말하고 있어 % 는 눈만 붙잡는다.
        p.setFont(Theme::mono_font(19, 700));
        const QRectF num(arc.left(), arc.center().y() - 22, arc.width(), 30);
        p.drawText(num, Qt::AlignCenter,
                   m_valid ? QString::number(qRound(m_shown))
                           : QStringLiteral("—"));

        // ---- 라벨 ----
        p.setPen(m_npu ? Theme::accent : Theme::textMuted);
        p.setFont(Theme::ui_font(10, 600, 0.1));
        p.drawText(QRect(0, 118, width(), 16), Qt::AlignCenter, m_label);

        // ---- 스파크라인 (최근 60초, 고정 0~100 스케일) ----
        if (m_hist.size() >= 2) {
            const QRectF spark(15, 146, 120, 32);
            QPainterPath path;
            const double dx = spark.width() / 59.0;
            const int n = m_hist.size();
            for (int i = 0; i < n; ++i) {
                const double x = spark.right() - (n - 1 - i) * dx;
                const double y =
                    spark.bottom() - spark.height() * m_hist[i] / 100.0;
                if (i == 0)
                    path.moveTo(x, y);
                else
                    path.lineTo(x, y);
            }
            QColor sc = c;
            sc.setAlpha(m_fade == Live ? 150 : m_fade == Stale ? 90 : 60);
            p.setPen(QPen(sc, 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
    }

private:
    QString m_label;
    bool m_npu = false;
    bool m_valid = false;
    Fade m_fade = Live;
    double m_shown = 0, m_from = 0, m_to = 0;
    QVector<double> m_hist;
    QVariantAnimation *m_anim = nullptr;
};

// ---------------------------------------------------------------------------

CameraPage::CameraPage(QWidget *parent)
    : QWidget(parent)
{
    // deviceinfo 1회 조회용 — DetectionFeed와 같은 TLS핀+digest 패턴
    m_net = new QNetworkAccessManager(this);
    Credentials::install_tls_pinning(m_net);
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [](QNetworkReply *reply, QAuthenticator *auth) {
                if (auth->user() == Credentials::camera_user()) {
                    qWarning() << "[CameraPage] 인증 거부 — 계정/비밀번호 확인 필요";
                    reply->abort();
                    return;
                }
                auth->setUser(Credentials::camera_user());
                auth->setPassword(Credentials::camera_password());
            });

    // 조작 창구 — build_subtabs()의 SystemPanel이 참조하므로 레이아웃보다 먼저
    m_control = new CameraControl(this);
    connect(m_control, &CameraControl::app_control_finished,
            this, &CameraPage::on_control_finished);
    connect(m_control, &CameraControl::app_set_finished,
            this, &CameraPage::on_set_finished);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(26, 24, 26, 24);   // 4a §Geometry
    root->setSpacing(20);

    root->addWidget(build_header());

    // 오프라인 배너 — §4d: 마지막 값 타임스탬프와 함께, 평소엔 숨김
    m_banner = new QLabel(this);
    m_banner->setFont(Theme::mono_font(11));
    m_banner->setFixedHeight(30);
    m_banner->setAlignment(Qt::AlignCenter);
    Theme::restyle(m_banner, [=] {
        return QString("background:%1; color:%2; border:1px solid %3; border-radius:2px;")
            .arg(QColor(0x2A, 0x12, 0x10).name(), Theme::alarm.name(),
                 QColor(0x4A, 0x1E, 0x18).name());
    });
    m_banner->hide();
    root->addWidget(m_banner);

    // 본문(히어로 + 앱 + 하위 탭)은 통째로 스크롤한다 — REPORT와 같은 구조.
    // 전에는 페이지 전체가 고정 스택이라, 히어로와 앱 카드가 창 높이를 다 쓰면
    // 하위 탭(시스템·프로파일·이미지·네트워크)이 몇 픽셀로 눌려 내용을 볼 수
    // 없었다. 이제 넘치는 만큼 페이지가 스크롤되고, 하위 탭은 최소 높이를 갖는다.
    auto *body = new QWidget(this);
    // ⚠ 세로로 **줄어들 수 있다고** 말하지 않는다. QScrollArea는 위젯을 줄일 수
    //   있으면 뷰포트 높이에 맞춰 줄여 버리고(qSmartMinSize가 minimumSizeHint를
    //   쓴다), 그러면 스크롤 대신 히어로 게이지(고정 186px)가 잘려 나간다.
    //   MinimumExpanding이면 자연 높이를 지키고 넘치는 만큼 스크롤바가 선다.
    body->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    auto *body_col = new QVBoxLayout(body);
    body_col->setContentsMargins(0, 0, 0, 0);
    body_col->setSpacing(20);
    body_col->addWidget(build_hero());
    // 응용 프로그램 카드는 하위 탭 스택의 System 페이지가 됐다(보드 배치)
    body_col->addWidget(build_subtabs(), 1);

    auto *body_scroll = new QScrollArea(this);
    body_scroll->setObjectName("RScroll");
    body_scroll->setWidgetResizable(true);
    body_scroll->setFrameShape(QFrame::NoFrame);
    body_scroll->setWidget(body);

    // 08-19 보드: [중앙(게이지 + 하위 탭) | 우측 360px 장비 열]
    auto *main_row = new QHBoxLayout();
    main_row->setContentsMargins(0, 0, 0, 0);
    main_row->setSpacing(14);
    main_row->addWidget(body_scroll, 1);
    main_row->addWidget(build_device_column());
    root->addLayout(main_row, 1);

    // 테마 전환 — 상태 칩·앱 카드·장비정보 표는 색을 텍스트에 구워 넣는다.
    // 폴이 곧 덮어쓰지만(1~5초) 전환 즉시 맞추기 위해 여기서 다시 그린다.
    Theme::on_theme_changed(this, [this] {
        on_link_state(CameraStatus::instance()->link_state());
        if (!m_apps.isEmpty())
            on_apps(m_apps);
        if (m_devinfo_loaded)
            fill_deviceinfo(m_devinfo);
    });

    // 재부팅 카운트다운 (§4d) — Rebooting 동안 1초마다 배너 갱신
    m_reboot_timer = new QTimer(this);
    m_reboot_timer->setInterval(1000);
    connect(m_reboot_timer, &QTimer::timeout, this, [this] {
        const qint64 s =
            (QDateTime::currentMSecsSinceEpoch() - m_reboot_started_ms) / 1000;
        m_banner->setText(
            QString("Camera rebooting - %1 s elapsed (about 60 s expected, "
                    "resumes automatically)").arg(s));
    });

    // ---- CameraStatus 구독 (폴링 원천은 하나, 이 페이지는 소비만) ----
    auto *cam = CameraStatus::instance();
    connect(cam, &CameraStatus::resources_changed, this, &CameraPage::on_resources);
    connect(cam, &CameraStatus::apps_changed, this, &CameraPage::on_apps);
    connect(cam, &CameraStatus::link_state_changed, this, &CameraPage::on_link_state);
    if (cam->resources().valid())
        on_resources(cam->resources());
    if (!cam->apps().isEmpty())
        on_apps(cam->apps());
    on_link_state(cam->link_state());
}

void CameraPage::showEvent(QShowEvent *ev)
{
    QWidget::showEvent(ev);
    if (!m_devinfo_loaded)
        fetch_deviceinfo();  // 진입 1회 (실패 시 재진입 때 재시도)
}

// ---------------------------------------------------------------- UI 구축

QWidget *CameraPage::build_header()
{
    auto *w = new QWidget(this);
    auto *row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(16);

    // 08-19 보드: 제목 줄 한 줄 — [제목 13/700] [● 상태] [모델 캡션] … [하위 탭]
    auto *title = new QLabel("Camera Control", w);
    title->setFont(Theme::ui_font(13, 700, 0.08));
    Theme::restyle(title, [=] {
        return QString("color:%1;").arg(Theme::textHi.name());
    });
    row->addWidget(title);

    // 상태 칩은 제목 바로 옆이다(보드의 "● Online") — 줄 맨 끝에 두면 제목과
    // 멀어져 무엇의 상태인지가 안 읽힌다.
    m_state_chip = new QLabel(w);
    m_state_chip->setObjectName("Pill");
    m_state_chip->setFixedHeight(Theme::px(22));
    m_state_chip->setFont(Theme::mono_font(11));
    row->addWidget(m_state_chip);

    m_dev_summary = new QLabel("fetching device info...", w);
    m_dev_summary->setFont(Theme::mono_font(10, 400, 1.0 / 10));
    Theme::restyle(m_dev_summary, [=] {
        return QString("color:%1;").arg(Theme::textMuted.name());
    });
    row->addWidget(m_dev_summary);
    row->addStretch(1);

    // 하위 탭도 보드처럼 제목 줄 오른쪽 끝에 — 스택은 build_subtabs 가
    // 만들고, 여기서 만든 버튼 그룹(m_subtab_group)에 나중에 연결한다.
    row->addWidget(build_subtab_bar(w));

    return w;
}

/**
 * @brief 하위 탭 버튼 줄 (System · Profiles · Image · Network)
 *
 * 보드는 이 줄을 화면 제목 줄 오른쪽에 둔다. 스택(m_subpages)은 아직 없으므로
 * 여기서는 버튼과 그룹만 만들고, 연결은 build_subtabs 가 한다.
 */
QWidget *CameraPage::build_subtab_bar(QWidget *parent)
{
    auto *seg = new QFrame(parent);
    seg->setObjectName("Segmented");
    auto *seg_row = new QHBoxLayout(seg);
    seg_row->setContentsMargins(3, 3, 3, 3);
    seg_row->setSpacing(2);

    m_subtab_group = new QButtonGroup(this);
    m_subtab_group->setExclusive(true);

    static const char *TABS[] = {"System", "Profiles", "Image", "Network"};
    for (int i = 0; i < 4; ++i) {
        auto *btn = new QPushButton(QString::fromUtf8(TABS[i]), seg);
        btn->setObjectName("SegBtn");
        btn->setCheckable(true);
        btn->setFixedHeight(Theme::px(22));
        btn->setFont(Theme::ui_font(11, 600));
        btn->setCursor(Qt::PointingHandCursor);
        m_subtab_group->addButton(btn, i);
        seg_row->addWidget(btn);
        if (i == 0)
            btn->setChecked(true);
    }
    return seg;
}

QWidget *CameraPage::build_hero()
{
    auto *card = new QFrame(this);
    Theme::restyle(card, [=] {
        return QString("background:%1; border:1px solid %2; border-radius:3px;")
            .arg(Theme::panel.name(), Theme::border.name());
    });
    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(24, 14, 24, 10);
    row->setSpacing(28);

    static const char *NAMES[3] = {"CPU", "Memory", "NPU"};
    for (int i = 0; i < 3; ++i) {
        m_gauges[i] = new ResourceGauge(NAMES[i], i == 2, card);
        m_gauges[i]->setStyleSheet("background:transparent; border:none;");
        row->addWidget(m_gauges[i]);
    }

    row->addStretch(1);

    // RAM·저장소 — 게이지로 만들 만큼 요동치지 않아 수치로 (§7 GB 표기)
    auto *side = new QVBoxLayout;
    side->setSpacing(8);
    side->addStretch(1);
    auto make_line = [&](QLabel *&out) {
        out = new QLabel(QStringLiteral("—"), card);
        out->setFont(Theme::mono_font(12));
        Theme::restyle(out, [=] {
            return QString("color:%1; background:transparent; border:none;")
                .arg(Theme::textMid.name());
        });
        side->addWidget(out, 0, Qt::AlignRight);
    };
    make_line(m_ram_label);
    make_line(m_sto_label);
    side->addStretch(1);
    row->addLayout(side);

    return card;
}

QWidget *CameraPage::build_apps_section()
{
    auto *w = new QWidget(this);
    auto *outer = new QVBoxLayout(w);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // 08-19: 보드의 Applications 는 **카드**다(면 + 테두리 + 머리글 + 줄).
    // 카드 없이 머리글만 페이지에 얹혀 있으니 다른 구역들과 다른 물건처럼
    // 보였다 (사용자: "applications font is a bit weird").
    auto *card = new QWidget(w);
    card->setObjectName("Panel");
    card->setAttribute(Qt::WA_StyledBackground);
    outer->addWidget(card);

    auto *col = new QVBoxLayout(card);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    // 헤더 우측 토스트 — 조작 결과가 여기 잠깐 떴다 사라진다
    m_toast = new QLabel(card);
    m_toast->setFont(Theme::mono_font(10));
    m_toast->hide();
    col->addWidget(PanelChrome::header(
        QString::fromUtf8("Applications"),
        QString::fromUtf8("opensdk apps · Start/Stop"), card, m_toast));

    auto *body = new QWidget(card);
    m_cards_lay = new QVBoxLayout(body);
    m_cards_lay->setContentsMargins(12, 10, 12, 12);
    m_cards_lay->setSpacing(6);
    auto *waiting = new QLabel("waiting for the app list...", body);
    waiting->setFont(Theme::mono_font(11));
    Theme::restyle(waiting, [=] {
        return QString("color:%1; padding:8px 2px;")
                               .arg(Theme::textDim.name());
    });
    m_cards_lay->addWidget(waiting);
    col->addWidget(body);

    // ⚠ 이 스트레치가 없으면 앱 목록이 페이지 아래로 밀려 내려간다.
    //   이 위젯은 하위 탭 스택 안에서 세로로 늘어나는데, 남는 높이를
    //   레이아웃이 카드와 목록에 나눠 주기 때문이다 (사용자: "the apps are
    //   way too below the applications"). 남는 높이는 전부 여기로 보낸다.
    outer->addStretch(1);
    return w;
}

QWidget *CameraPage::build_subtabs()
{
    auto *w = new QWidget(this);
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(10);

    // 탭 버튼 줄은 build_subtab_bar 가 만들어 **화면 제목 줄**에 들어갔다
    // (08-19 보드). 여기는 내용 스택만 만든다.

    // 탭마다 따로 스크롤하지 않는다 — 페이지 스크롤 하나만 둔다. 스크롤 영역을
    // 겹쳐 두면 안쪽은 좁은 창에서 몇 줄만 보이고 휠이 어느 쪽에 먹는지도
    // 헷갈린다. 여기서는 패널이 자기 키대로 펼쳐지고 페이지가 대신 흐른다.
    //
    // System 페이지 = 응용 프로그램 목록이다. 장비·펌웨어 정보는 보드처럼
    // **우측 상시 열**로 옮겼다(build_device_column) — 예전엔 System 탭
    // 안에 있어서 나머지 세 탭에서는 아예 보이지 않았다.
    m_subpages = new QStackedWidget(w);
    m_subpages->addWidget(build_apps_section());
    m_subpages->addWidget(new ProfilePanel(w));
    m_subpages->addWidget(new ImagePanel(w));
    m_subpages->addWidget(new NetworkPanel(w));

    // QStackedWidget은 기본적으로 **가장 큰** 페이지에 맞춰 키를 잡는다 —
    // 그러면 짧은 탭에서 그만큼 빈 공간이 남는다. 보이는 탭만 크기에 반영한다.
    auto size_to_current = [this](int cur) {
        for (int i = 0; i < m_subpages->count(); ++i)
            m_subpages->widget(i)->setSizePolicy(
                QSizePolicy::Preferred,
                i == cur ? QSizePolicy::Preferred : QSizePolicy::Ignored);
        m_subpages->widget(cur)->adjustSize();
        m_subpages->adjustSize();
    };
    size_to_current(0);
    connect(m_subpages, &QStackedWidget::currentChanged, this, size_to_current);

    connect(m_subtab_group, &QButtonGroup::idClicked,
            m_subpages, &QStackedWidget::setCurrentIndex);
    col->addWidget(m_subpages, 1);

    return w;
}

/**
 * @brief 우측 장비 열 (보드 360px) — 장비·펌웨어 + 시스템 운영
 *
 * 보드에서 이 열은 **어느 하위 탭에서도 보인다.** 예전에는 System 탭 안에
 * 있어서 Profiles·Image·Network 를 보는 동안 모델·펌웨어·업타임이 사라졌다.
 */
QWidget *CameraPage::build_device_column()
{
    auto *host = new QWidget(this);
    // ⚠ 보드는 360px 이지만 그 열에는 장비 정보 8줄 + 버튼 3개 + 로그뿐이다.
    //   우리 SystemPanel 은 시계/NTP · 프로파일 세션 · 로그 · 백업까지 들고
    //   있어서 360 에 넣으면 제목과 캡션이 잘린다(실측: "Device & Firmw",
    //   "profileaccessi"). 잘린 화면은 보드와 닮은 게 아니라 그냥 고장이라
    //   내용이 들어가는 폭으로 넓힌다 — 의도한 이탈이다.
    host->setFixedWidth(Theme::px(404));
    host->setAttribute(Qt::WA_StyledBackground);
    Theme::restyle(host, [] {
        return QString("background:%1; border-left:1px solid %2;")
            .arg(Theme::chromeElevated.name(), Theme::border.name());
    });

    auto *col = new QVBoxLayout(host);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    // 웹 UI 링크는 이 헤더 오른쪽에 붙인다 — 장비 정보를 보다가 "실물 설정을
    // 확인해야겠다"가 되는 자리가 여기다. 주소는 Credentials가 유일한 출처라
    // 카메라를 옮겨도 화면이 옛 주소를 말하지 않는다.
    col->addWidget(PanelChrome::header(
        QStringLiteral("Device & Firmware"),
        QStringLiteral("system.cgi deviceinfo"), host, build_camera_link(host)));

    auto *grid_host = new QWidget(host);
    m_dev_grid = new QGridLayout(grid_host);
    m_dev_grid->setContentsMargins(12, 10, 12, 10);
    m_dev_grid->setHorizontalSpacing(14);
    m_dev_grid->setVerticalSpacing(7);
    m_dev_loading = new QLabel("fetching...", grid_host);
    m_dev_loading->setFont(Theme::mono_font(11));
    Theme::restyle(m_dev_loading, [=] {
        return QString("color:%1;").arg(Theme::textDim.name());
    });
    m_dev_grid->addWidget(m_dev_loading, 0, 0);
    col->addWidget(grid_host);

    // 운영 기능 묶음: 시계/NTP · 프로파일 세션 · 로그 · 백업 · 재부팅
    col->addWidget(new SystemPanel(m_control, host));
    col->addStretch(1);

    auto *foot = new QLabel(
        QString::fromUtf8("SUNAPI · digest auth · TLS-pinned · admin only for writes"),
        host);
    foot->setFont(Theme::mono_font(10));
    foot->setWordWrap(true);
    Theme::restyle(foot, [] {
        return QString("color:%1; border-top:1px solid %2; padding:10px 12px;")
            .arg(Theme::textFaint.name(), Theme::border.name());
    });
    col->addWidget(foot);

    // 세로로 넘치면 이 열만 스크롤한다 — 360px 은 좁아서 SystemPanel 이
    // 창 높이를 넘길 수 있다(잘리면 안 되는 내용이다).
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("RScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(host);
    scroll->setFixedWidth(Theme::px(404));
    return scroll;
}

/**
 * @brief 카메라 웹 UI 링크 — 주소를 그대로 보여주고, 누르면 브라우저로 연다
 *
 * 주소는 **Credentials가 유일한 출처**다(`camera_base_url()` = scheme+host).
 * 화면에 IP를 박아 두면 카메라를 옮겼을 때 여기만 옛 주소를 말하게 된다.
 *
 * ⚠ `<a>`의 색은 스타일시트로 못 준다 — QSS는 리치텍스트 안의 앵커에 닿지
 *   않고 Qt가 자기 기본 링크색으로 칠해 버린다(라이트 테마에서 파란 링크가
 *   오렌지 화면에 혼자 남는다). HTML에 직접 굽고 테마가 바뀌면 다시 그린다
 *   (부록 D의 세 번째 경로).
 */
QWidget *CameraPage::build_camera_link(QWidget *parent)
{
    const QUrl url = Credentials::camera_base_url();

    auto *link = new QLabel(parent);
    link->setFont(Theme::mono_font(10));
    link->setTextFormat(Qt::RichText);
    link->setOpenExternalLinks(true);                           // 기본 브라우저
    link->setTextInteractionFlags(Qt::TextBrowserInteraction);  // 클릭 + 복사
    link->setCursor(Qt::PointingHandCursor);
    link->setToolTip(QString(
        "Opens the camera web UI in your default browser - %1\n"
        "(the device certificate is from a private CA, so the browser may warn)")
                         .arg(url.toString()));

    auto paint = [link, url] {
        link->setText(QString(
                          "<span style=\"color:%1\">web UI</span>&nbsp; "
                          "<a href=\"%2\" style=\"color:%3\">%4</a>")
                          .arg(Theme::textDim.name(), url.toString(),
                               Theme::accent.name(), url.host()));
    };
    paint();
    Theme::on_theme_changed(link, paint);
    return link;
}

// ---------------------------------------------------------------- 데이터

void CameraPage::on_resources(const CameraResources &res)
{
    m_gauges[0]->set_value(res.cpu);
    m_gauges[1]->set_value(res.mem);
    m_gauges[2]->set_value(res.npu);

    const double gb = 1024.0;
    m_ram_label->setText(
        QString("RAM used %1 / %2 GB")
            .arg((res.ram_total_mb - res.ram_free_mb) / gb, 0, 'f', 1)
            .arg(res.ram_total_mb / gb, 0, 'f', 1));
    m_sto_label->setText(
        QString("Storage free %1 / %2 MB")
            .arg(qRound(res.storage_free_mb))
            .arg(qRound(res.storage_total_mb)));
}

void CameraPage::on_apps(const QVector<CameraAppInfo> &apps)
{
    m_apps = apps;

    // 목록 구성이 바뀌었나 (설치/제거/순서) — 바뀌면 카드를 다시 짓는다
    bool same = m_cards.size() == apps.size();
    if (same)
        for (int i = 0; i < apps.size(); ++i)
            if (!m_cards.contains(apps[i].id)) { same = false; break; }

    if (!same) {
        while (QLayoutItem *it = m_cards_lay->takeAt(0)) {
            delete it->widget();
            delete it;
        }
        m_cards.clear();
        for (const CameraAppInfo &app : apps) {
            auto *card = new AppCard(app.id, this);
            connect(card, &AppCard::start_requested,
                    this, &CameraPage::on_start_requested);
            connect(card, &AppCard::stop_requested,
                    this, &CameraPage::on_stop_requested);
            connect(card, &AppCard::autostart_requested, this,
                    [this](const QString &id, bool enable) {
                        const CameraAppInfo *info = find_app(id);
                        m_control->set_app(id, enable,
                                           info ? info->priority
                                                : QStringLiteral("Low"),
                                           QStringLiteral("AutoStart"));
                    });
            connect(card, &AppCard::priority_requested, this,
                    [this](const QString &id, const QString &p) {
                        const CameraAppInfo *info = find_app(id);
                        m_control->set_app(id, info && info->auto_start, p,
                                           QStringLiteral("Priority"));
                    });
            m_cards_lay->addWidget(card);
            m_cards.insert(app.id, card);
        }
    }

    for (const CameraAppInfo &app : apps)
        m_cards.value(app.id)->set_info(app);
}

const CameraAppInfo *CameraPage::find_app(const QString &app_id) const
{
    for (const CameraAppInfo &app : m_apps)
        if (app.id == app_id)
            return &app;
    return nullptr;
}

void CameraPage::on_start_requested(const QString &app_id)
{
    // 시작은 파장이 없어 확인 없이 — 낙관적 전이 후 폴이 확정
    if (AppCard *card = m_cards.value(app_id))
        card->begin_pending(true);
    m_control->control_app(app_id, true);
}

void CameraPage::on_stop_requested(const QString &app_id)
{
    if (!confirm_stop(this, app_id))
        return;
    // 사용자 지시 정지 — "예기치 않은 죽음" 경보(§4c-2) 대상에서 제외
    CameraStatus::instance()->expect_app_stop(app_id);
    if (AppCard *card = m_cards.value(app_id))
        card->begin_pending(false);
    m_control->control_app(app_id, false);
}

void CameraPage::on_control_finished(const QString &app_id, bool start,
                                     bool ok, const QString &error)
{
    if (ok) {
        show_toast(QString("%1 %2 request accepted - confirming state...")
                       .arg(app_id, start ? QString("start") : QString("stop")),
                   true);
        // 낙관적 전이의 확정 — 즉시 1회 + 이후 5초 폴이 이어받는다
        CameraStatus::instance()->request_apps_now();
        QTimer::singleShot(1500, CameraStatus::instance(),
                           &CameraStatus::request_apps_now);
    } else {
        if (AppCard *card = m_cards.value(app_id))
            card->cancel_pending();
        show_toast(QString("%1 %2 failed: %3")
                       .arg(app_id, start ? QString("start") : QString("stop"),
                            error),
                   false);
    }
}

void CameraPage::on_set_finished(const QString &app_id, const QString &key,
                                 bool ok, const QString &error)
{
    if (ok) {
        show_toast(QString("%1 %2 change applied").arg(app_id, key), true);
    } else {
        show_toast(QString("%1 %2 change failed: %3").arg(app_id, key, error),
                   false);
        on_apps(m_apps);  // 컨트롤 표시를 진실값으로 원복
    }
    // 성공이든 실패든 즉시 재폴 — 카드가 카메라의 진실로 수렴한다
    CameraStatus::instance()->request_apps_now();
}

void CameraPage::show_toast(const QString &text, bool ok)
{
    m_toast->setText(text);
    Theme::restyle(m_toast, [=] {
        return QString("color:%1;")
                               .arg((ok ? Theme::green : Theme::alarm).name());
    });
    m_toast->show();
    QTimer::singleShot(4000, m_toast, &QLabel::hide);
}

void CameraPage::on_link_state(CameraStatus::LinkState state)
{
    using LS = CameraStatus::LinkState;

    QColor c = Theme::textDim;
    QString text = QStringLiteral("—");
    switch (state) {
    case LS::Online:    c = Theme::green;   text = "Online"; break;
    case LS::Stale:     c = Theme::amber;   text = "Refreshing"; break;
    case LS::Offline:   c = Theme::alarm;   text = "Offline"; break;
    case LS::Rebooting: c = Theme::amber;   text = "Rebooting"; break;
    }
    m_state_chip->setText(
        QString("<span style=\"font-size:8px;color:%1\">●</span>&nbsp; %2")
            .arg(c.name(), text));

    const bool offline = state == LS::Offline;
    const bool rebooting = state == LS::Rebooting;
    if (offline) {
        const qint64 last = CameraStatus::instance()->last_success_ms();
        m_banner->setText(
            last > 0 ? QString("Camera offline - last update %1")
                           .arg(QDateTime::fromMSecsSinceEpoch(last)
                                    .toString("HH:mm:ss"))
                     : QString("Camera offline - never connected"));
    }
    if (rebooting && !m_reboot_timer->isActive()) {
        m_reboot_started_ms = QDateTime::currentMSecsSinceEpoch();
        m_banner->setText("Camera rebooting - waiting for it to come back");
        m_reboot_timer->start();
    } else if (!rebooting) {
        m_reboot_timer->stop();
    }
    m_banner->setVisible(offline || rebooting);
    // Stale은 "낡음"이지 "모름"이 아니다 — 색(부하 수준)은 유지한다
    const ResourceGauge::Fade fade = state == LS::Online  ? ResourceGauge::Live
                                   : state == LS::Stale   ? ResourceGauge::Stale
                                                          : ResourceGauge::Offline;
    for (ResourceGauge *g : m_gauges)
        g->set_fade(fade);
}

void CameraPage::fetch_deviceinfo()
{
    if (m_pending)
        return;

    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/system.cgi");
    url.setQuery("msubmenu=deviceinfo&action=view");
    QNetworkRequest req = sunapi_request(url, 3000);
    req.setRawHeader("Accept", "application/json");

    m_pending = m_net->get(req);
    connect(m_pending, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_pending;
        m_pending = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            m_dev_summary->setText(
                "Device info fetch failed - retries when you re-enter the tab");
            if (m_dev_loading)
                m_dev_loading->setText(
                    QString("Fetch failed (%1) - retries when you re-enter the tab")
                        .arg(reply->errorString()));
            return;
        }
        const QJsonObject obj =
            QJsonDocument::fromJson(reply->readAll()).object();
        if (obj.isEmpty())
            return;
        m_devinfo_loaded = true;
        m_devinfo = obj;
        fill_deviceinfo(obj);
    });
}

void CameraPage::fill_deviceinfo(const QJsonObject &obj)
{
    const QString model = obj.value("Model").toString();
    const QString fw = obj.value("FirmwareVersion").toString();
    const QString onvif = obj.value("ONVIFVersion").toString();
    m_dev_summary->setText(QString::fromUtf8("%1 · FW %2 · ONVIF %3")
                               .arg(model, fw, onvif));

    // 표: 모델 크게(§3-0 "모델명이 데모 정체성"), 나머지는 필드명 순서대로
    while (QLayoutItem *it = m_dev_grid->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    m_dev_loading = nullptr;  // 방금 지워졌다 — 매달린 포인터 방지
    static const struct { const char *key; const char *label; } FIELDS[] = {
        {"Model", "Model"},
        {"FirmwareVersion", "Firmware"},
        {"FirmwareBuildDate", "Build Date"},
        {"SerialNumber", "Serial"},
        {"ConnectedMACAddress", "MAC"},
        {"WisenetPlatformVersion", "Wisenet Platform"},
        {"CGIVersion", "CGI"},
        {"ONVIFVersion", "ONVIF"},
        {"OpenSDKVersion", "OpenSDK"},
        {"DeviceName", "Device Name"},
        {"DeviceLocation", "Location"},
    };
    int row = 0;
    for (const auto &f : FIELDS) {
        QString v = obj.value(QLatin1String(f.key)).toVariant().toString();
        if (qstrcmp(f.key, "FirmwareBuildDate") == 0 && v.isEmpty())
            v = obj.value("BuildDate").toVariant().toString();
        if (v.isEmpty())
            continue;
        auto *k = new QLabel(QLatin1String(f.label), this);
        k->setFont(Theme::ui_font(10, 600, 0.10));
        Theme::restyle(k, [=] {
            return QString("color:%1;").arg(Theme::textDim.name());
        });
        auto *val = new QLabel(v, this);
        const bool is_model = qstrcmp(f.key, "Model") == 0;
        val->setFont(Theme::mono_font(is_model ? 14 : 11, is_model ? 600 : 400));
        Theme::restyle(val, [=] {
            return QString("color:%1;")
                               .arg(is_model ? Theme::textHi.name()
                                             : Theme::textMid.name());
        });
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_dev_grid->addWidget(k, row, 0, Qt::AlignLeft | Qt::AlignVCenter);
        m_dev_grid->addWidget(val, row, 1, Qt::AlignLeft | Qt::AlignVCenter);
        ++row;
    }
    m_dev_grid->setColumnStretch(2, 1);
}
