#include "image_panel.h"
#include "wall_layout.h"
#include "auth.h"
#include "channel_view.h"
#include "credentials.h"
#include "panel_chrome.h"
#include "sunapi_request.h"
#include "theme.h"

#include <QAuthenticator>
#include <QComboBox>
#include <QDebug>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QComboBox *mono_combo(const QStringList &items, int w_px, QWidget *parent)
{
    auto *c = new QComboBox(parent);
    c->addItems(items);
    c->setFixedSize(Theme::px(w_px), Theme::px(24));  // 글자와 같은 배율
    c->setFont(Theme::mono_font(10));
    c->setCursor(Qt::PointingHandCursor);
    Theme::restyle(c, [=] {
        return QString("QComboBox { background:%1; border:1px solid %2;"
                " border-radius:2px; color:%3; padding-left:8px; }"
                "QComboBox::drop-down { border:none; width:16px; }"
                "QComboBox QAbstractItemView { background:%1; color:%3;"
                " border:1px solid %2; selection-background-color:%4; }")
            .arg(Theme::elevated.name(), Theme::border2.name(),
                 Theme::textMid.name(), Theme::elevated2.name());
    });
    return c;
}

} // namespace

ImagePanel::ImagePanel(QWidget *parent)
    : QWidget(parent)
{
    m_net = new QNetworkAccessManager(this);
    Credentials::install_tls_pinning(m_net);
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [](QNetworkReply *reply, QAuthenticator *auth) {
                if (auth->user() == Credentials::camera_user()) {
                    qWarning() << "[ImagePanel] 인증 거부 — 계정 확인 필요";
                    reply->abort();
                    return;
                }
                auth->setUser(Credentials::camera_user());
                auth->setPassword(Credentials::camera_password());
            });

    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 8, 0, 0);
    col->setSpacing(8);

    m_result = new QLabel(this);
    m_result->setFont(Theme::mono_font(10));
    m_result->hide();
    col->addWidget(PanelChrome::header(
        QStringLiteral("Image / ISP"),
        QStringLiteral("image.cgi · changes hit the live picture at once"), this,
        m_result));

    auto *row = new QHBoxLayout;
    row->setSpacing(10);
    auto add_labeled = [&](const QString &label, QWidget *field) {
        auto *box = new QVBoxLayout;
        box->setSpacing(3);
        auto *l = new QLabel(label, this);
        l->setFont(Theme::ui_font(9, 600, 0.10));
        Theme::restyle(l, [=] {
            return QString("color:%1;").arg(Theme::textDim.name());
        });
        box->addWidget(l);
        box->addWidget(field);
        row->addLayout(box);
    };

    m_ch = mono_combo({"CH1", "CH2", "CH3", "CH4"}, 70, this);
    add_labeled("Channel", m_ch);
    connect(m_ch, &QComboBox::currentIndexChanged, this,
            [this](int) { fetch_all(); });

    m_comp = mono_combo({"Off", "BLC", "HLC", "WDR"}, 80, this);
    add_labeled("Backlight comp.", m_comp);
    connect(m_comp, &QComboBox::textActivated, this, [this](const QString &v) {
        if (!m_loading)
            send_set("camera", "CompensationMode", v);
    });

    m_wb = mono_combo({"ATW", "AWC", "Indoor", "Outdoor", "Manual"}, 90, this);
    add_labeled("White balance", m_wb);
    connect(m_wb, &QComboBox::textActivated, this, [this](const QString &v) {
        if (!m_loading)
            send_set("whitebalance", "WhiteBalanceMode", v);
    });

    // 값 콤보 3개는 고르는 순간 카메라에 쓴다 → 권한에 묶는다.
    // 채널 콤보(m_ch)는 조회 대상 선택이라 잠그지 않는다(§5: 조회는 둘 다 허용).
    Auth::bind(m_comp, Auth::Action::CameraTuning);
    Auth::bind(m_wb, Auth::Action::CameraTuning);

    m_ir = mono_combo({"Off", "DayNight", "Sensor", "Manual", "Schedule"}, 100, this);
    add_labeled(QString::fromUtf8("IR LED"), m_ir);
    connect(m_ir, &QComboBox::textActivated, this, [this](const QString &v) {
        if (!m_loading)
            send_set("irled", "Mode", v);
    });
    Auth::bind(m_ir, Auth::Action::CameraTuning);

    auto *af = new QPushButton("Run Simple Focus", this);
    af->setFixedSize(Theme::px(140), Theme::px(24));
    af->setFont(Theme::ui_font(11, 600));
    af->setCursor(Qt::PointingHandCursor);
    Theme::restyle(af, [=] {
        return QString("QPushButton { background:transparent; border:1px solid %1;"
                " border-radius:2px; color:%1; }"
                "QPushButton:hover { background:%2; }")
            .arg(Theme::accent.name(), Theme::elevated2.name());
    });
    af->setToolTip(
        "One autofocus pass (supported models only - others refuse)");
    Auth::bind(af, Auth::Action::CameraTuning);
    connect(af, &QPushButton::clicked, this, [this] {
        run_simple_focus(m_ch->currentIndex(), false);
    });
    auto *af_box = new QVBoxLayout;
    af_box->setSpacing(3);
    af_box->addSpacing(15);
    af_box->addWidget(af);
    row->addLayout(af_box);

    row->addStretch(1);
    col->addLayout(row);

    // 안전 타이머 — 창이 바뀌거나 released 를 놓쳐도 렌즈가 끝단까지
    // 밀리지 않게 한다. 6초면 전 구간(100°→53°)을 훑고도 남는다.
    m_zoom_guard = new QTimer(this);
    m_zoom_guard->setSingleShot(true);
    m_zoom_guard->setInterval(6000);
    connect(m_zoom_guard, &QTimer::timeout, this, [this] {
        if (m_zoom_ch < 0)
            return;
        stop_zoom(true);
        show_result("zoom stopped by the 6 s safety limit", true);
    });

    col->addWidget(build_zoom_grid(), 1);

    auto *zoom_note = new QLabel(QString::fromUtf8(
        "Each channel has its own lens - the buttons under a tile drive that "
        "tile. The camera cannot report its zoom position, so take a "
        "screenshot before you change it. Privacy masks and motion zones are "
        "tied to the field of view and will need re-checking."), this);
    zoom_note->setFont(Theme::mono_font(10));
    zoom_note->setWordWrap(true);
    Theme::restyle(zoom_note, [=] {
        return QString("color:%1;").arg(Theme::textFaint.name());
    });
    col->addWidget(zoom_note);

    auto *note = new QLabel(
        "SSDR · flip · exposure · presets - out of scope (T3)", this);
    note->setFont(Theme::mono_font(10));
    Theme::restyle(note, [=] {
        return QString("color:%1; padding-top:6px;")
                            .arg(Theme::textFaint.name());
    });
    col->addWidget(note);
}

QWidget *ImagePanel::build_zoom_grid()
{
    auto *host = new QWidget(this);
    auto *col = new QVBoxLayout(host);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(6);

    auto *head = new QLabel(
        QString::fromUtf8("Zoom  ·  live view · hold a button to drive that lens (1.7x)"),
        host);
    head->setFont(Theme::ui_font(9, 600, 0.10));
    Theme::restyle(head, [] {
        return QString("color:%1;").arg(Theme::textDim.name());
    });
    col->addWidget(head);

    // 2×2 — LIVE 벽과 같은 배치라 "어느 타일이 어느 채널인지"를 다시 배울
    // 필요가 없다. 간격도 같은 2px.
    auto *grid_host = new QWidget(host);
    auto *grid = new QGridLayout(grid_host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(8);
    col->addWidget(grid_host, 1);

    for (int ch = 0; ch < 4; ++ch) {
        auto *cell = new QWidget(grid_host);
        cell->setObjectName("Panel");
        cell->setAttribute(Qt::WA_StyledBackground);
        auto *cell_col = new QVBoxLayout(cell);
        cell_col->setContentsMargins(6, 6, 6, 6);
        cell_col->setSpacing(6);

        // db_channel 은 LIVE 와 같은 항등 매핑이다 (live_viewer::db_channel_of)
        auto *view = new ChannelView(ch, ch, cell);
        view->setMinimumSize(Theme::px(220), Theme::px(124));
        view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        cell_col->addWidget(view, 1);
        m_previews.append(view);

        auto *bar = new QHBoxLayout();
        bar->setSpacing(6);
        auto *name = new QLabel(QString("CH%1").arg(ch + 1), cell);
        name->setFont(Theme::mono_font(11, 700));
        Theme::restyle(name, [] {
            return QString("color:%1;").arg(Theme::textHi.name());
        });
        bar->addWidget(name);
        bar->addStretch(1);

        auto hold_btn = [this, ch, cell](const QString &text, const QString &dir,
                                         const QString &tip) {
            auto *b = new QPushButton(text, cell);
            b->setFixedSize(Theme::px(62), Theme::px(22));
            b->setFont(Theme::ui_font(11, 600));
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(tip);
            Theme::restyle(b, [] {
                return QString("QPushButton { background:transparent;"
                        " border:1px solid %1; border-radius:2px; color:%2; }"
                        "QPushButton:hover { border-color:%3; color:%3; }"
                        "QPushButton:pressed { background:%4; border-color:%3;"
                        "                      color:%3; }"
                        "QPushButton:disabled { color:%5; border-color:%5; }")
                    .arg(Theme::border2.name(), Theme::textMuted.name(),
                         Theme::chromeSelText.name(), Theme::elevated2.name(),
                         Theme::textFaint.name());
            });
            Auth::bind(b, Auth::Action::CameraTuning);
            connect(b, &QPushButton::pressed, this,
                    [this, ch, dir] { zoom_continuous(ch, dir); });
            // released 는 버튼 밖에서 떼도 온다 — 정지의 1차 경로다
            connect(b, &QPushButton::released, this,
                    [this] { stop_zoom(true); });
            return b;
        };

        bar->addWidget(hold_btn(QString::fromUtf8("− Wide"), "Out",
            "Hold to widen this channel's view (up to 100°).\n"
            "The lens moves while you hold and stops when you let go."));
        bar->addWidget(hold_btn(QString::fromUtf8("+ Tele"), "In",
            "Hold to narrow this channel's view (down to 53°).\n"
            "Only 1.7x - hold a few seconds to see it move.\n"
            "Focus is re-run automatically when you let go."));

        cell_col->addLayout(bar);
        // 자리는 LIVE 벽과 같은 배치다 (08-20). 여기서 ch/2, ch%2 로 굳히면
        // 벽을 드래그로 재배치한 뒤 이 화면만 옛 배치로 남아, 같은 CH 가
        // 두 화면에서 다른 자리에 앉는다.
        int row = 0, col = 0;
        WallOrder::cell_of(ch, row, col);
        grid->addWidget(cell, row, col);
        m_zoom_cells.append(cell);
    }

    // 벽 배치가 바뀌면 이 그리드도 따라 옮긴다 — 위젯을 칸만 옮기므로
    // 미리보기 스트림(ChannelView)은 끊기지 않는다 (WallLayout 과 같은 수법).
    connect(WallOrder::notifier(), &WallOrder::Notifier::changed, grid,
            [this, grid] {
        for (int ch = 0; ch < m_zoom_cells.size(); ++ch)
            grid->removeWidget(m_zoom_cells[ch]);
        for (int ch = 0; ch < m_zoom_cells.size(); ++ch) {
            int r = 0, c = 0;
            WallOrder::cell_of(ch, r, c);
            grid->addWidget(m_zoom_cells[ch], r, c);
        }
    });

    grid->setRowStretch(0, 1);
    grid->setRowStretch(1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    return host;
}

void ImagePanel::start_previews()
{
    if (m_previews_live || m_previews.isEmpty())
        return;
    m_previews_live = true;

    // LIVE 벽과 **같은 프로파일**을 쓴다 — 같은 QSettings 키를 읽는다
    // (live_viewer.cpp sub_profile). 그리드는 profile4 고정이라는 가드레일이
    // 여기에도 그대로 적용된다: 4MP 를 네 장 더 열면 지연이 무한 누적된다.
    const QString profile =
        QSettings("GuardX", "VMS").value("grid_profile", "profile4").toString();

    for (int ch = 0; ch < m_previews.size(); ++ch) {
        QUrl u;
        u.setScheme("rtsp");
        u.setHost(Credentials::camera_host());
        u.setPort(554);
        u.setUserName(Credentials::camera_user());
        u.setPassword(Credentials::camera_password());
        u.setPath(QString("/%1/%2/media.smp").arg(ch).arg(profile));
        m_previews[ch]->play_stream(u);

        QString caption = profile;
        if (!caption.isEmpty())
            caption[0] = caption[0].toUpper();
        m_previews[ch]->set_stream_caption(
            QString::fromUtf8("%1 · H.264").arg(caption));
    }
}

void ImagePanel::stop_previews()
{
    if (!m_previews_live)
        return;
    m_previews_live = false;
    // ⚠ 반드시 세운다 — 안 세우면 이 탭을 한 번 열었다는 이유로 디코드
    //   세션 4개가 LIVE 의 4개 위에 얹힌 채 앱이 끝날 때까지 남는다.
    for (ChannelView *v : m_previews)
        v->stop_stream();
}

void ImagePanel::showEvent(QShowEvent *ev)
{
    QWidget::showEvent(ev);
    fetch_all();
    start_previews();   // 이 탭이 보이는 동안만 스트림을 연다
}

void ImagePanel::hideEvent(QHideEvent *ev)
{
    // 하위 탭을 옮기거나 창을 닫는 동안 렌즈가 돌고 있으면 그대로 끝단까지
    // 간다 — 화면을 떠나기 전에 반드시 세운다(초점은 다시 잡지 않는다.
    // 안 보이는 화면에서 추가 명령을 쏘는 것보다 세우는 게 먼저다).
    stop_zoom(false);
    stop_previews();
    QWidget::hideEvent(ev);
}

void ImagePanel::zoom_continuous(int ch, const QString &direction)
{
    if (!Auth::can(Auth::Action::CameraTuning)) {
        show_result(Auth::deny_reason(Auth::Action::CameraTuning), false);
        return;
    }

    // 다른 채널이 돌고 있었다면 먼저 세운다 — 두 렌즈가 동시에 도는 상태는
    // 버튼 하나를 떼는 것으로 못 멈춘다.
    if (m_zoom_ch >= 0 && m_zoom_ch != ch)
        stop_zoom(false);

    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/image.cgi");
    url.setQuery(QString("msubmenu=focus&action=control&Channel=%1"
                         "&ZoomContinuous=%2").arg(ch).arg(direction));

    // 먼저 "도는 중"으로 표시한다 — 응답을 기다리다 이 플래그를 늦게 세우면
    // 그 사이에 버튼을 뗀 Stop 이 무시돼 렌즈가 계속 돈다.
    m_zoom_ch = ch;
    m_zoom_guard->start();

    QNetworkReply *reply = m_net->get(sunapi_request(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, direction, ch] {
        reply->deleteLater();
        const QString body = QString::fromUtf8(reply->readAll()).trimmed();
        if (reply->error() != QNetworkReply::NoError) {
            m_zoom_ch = -1;
            m_zoom_guard->stop();
            show_result(QString("zoom request failed (%1)")
                            .arg(reply->errorString()), false);
        } else if (!body.startsWith("OK")) {
            // 608 = 이 장비에 그 기능이 없음. 여기까지 왔다는 건 인증은
            // 통과했다는 뜻이라, 경로/파라미터 문제로 좁혀서 알려 준다.
            m_zoom_ch = -1;
            m_zoom_guard->stop();
            show_result(QString("camera refused zoom: %1").arg(body.left(60)),
                        false);
        } else {
            show_result(QString("CH%1 zooming %2...")
                            .arg(ch + 1)
                            .arg(direction == "In" ? "in" : "out"), true);
        }
    });
}

void ImagePanel::stop_zoom(bool refocus)
{
    if (m_zoom_ch < 0)
        return;          // 이미 섰다 (released 와 안전 타이머가 겹칠 수 있다)
    const int ch = m_zoom_ch;
    m_zoom_ch = -1;
    m_zoom_guard->stop();

    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/image.cgi");
    url.setQuery(QString("msubmenu=focus&action=control&Channel=%1"
                         "&ZoomContinuous=Stop").arg(ch));

    // ⚠ 여기서는 권한을 다시 확인하지 **않는다.** 시작할 때 확인했고, 그
    //   사이 권한이 사라졌다고 해서 도는 렌즈를 그대로 두면 안 된다.
    QNetworkReply *reply = m_net->get(sunapi_request(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, refocus, ch] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            show_result(QString("STOP FAILED - the lens may still be moving (%1)")
                            .arg(reply->errorString()), false);
            return;
        }
        // 줌이 끝나면 초점이 틀어져 있다 — 이 장비의 초점 방식이 Simple
        // focus 라 한 번 돌려 주면 된다 (조사 노트 §2-3).
        if (refocus)
            run_simple_focus(ch, true);
    });
}

void ImagePanel::run_simple_focus(int ch, bool quiet)
{
    if (!Auth::can(Auth::Action::CameraTuning)) {
        if (!quiet)
            show_result(Auth::deny_reason(Auth::Action::CameraTuning), false);
        return;
    }

    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/image.cgi");
    url.setQuery(QString("msubmenu=focus&action=control&Channel=%1"
                         "&Mode=SimpleFocus").arg(ch));
    QNetworkReply *reply = m_net->get(sunapi_request(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, quiet] {
        reply->deleteLater();
        const QString body = QString::fromUtf8(reply->readAll()).trimmed();
        if (reply->error() != QNetworkReply::NoError)
            show_result(QString("focus request failed (%1)")
                            .arg(reply->errorString()), false);
        else if (!body.startsWith("OK"))
            show_result(QString("camera refused - focus may be unsupported (%1)")
                            .arg(body.left(50)), false);
        else
            show_result(quiet ? "zoom stopped - refocused"
                              : "Simple Focus ran", true);
    });
}

void ImagePanel::fetch_all()
{
    fetch_value("camera", "CompensationMode", m_comp);
    fetch_value("whitebalance", "WhiteBalanceMode", m_wb);
    fetch_value("irled", "Mode", m_ir);
}

void ImagePanel::fetch_value(const QString &submenu, const QString &key,
                             QComboBox *into)
{
    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/image.cgi");
    url.setQuery(QString("msubmenu=%1&action=view&Channel=%2")
                     .arg(submenu).arg(m_ch->currentIndex()));
    // 텍스트 응답에서 키 한 줄만 찾는다 (Accept: json을 붙이지 않는 이유)
    QNetworkRequest req = sunapi_request(url);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, into] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;  // 상태줄 도배 방지 — 조회 실패는 조용히 (콤보는 이전값)
        // "…CompensationMode=WDR" — 접두(Channel.N. / ImagePresetMode.…)가
        // 붙을 수 있어 줄 끝 키만 본다. 첫 매치 = 현재 모드.
        const QStringList lines = QString::fromUtf8(reply->readAll()).split('\n');
        const QString needle = key + QLatin1Char('=');
        for (const QString &line : lines) {
            const QString t = line.trimmed();
            const int at = t.indexOf(needle);
            if (at < 0 || (at > 0 && t.at(at - 1) != QLatin1Char('.')
                           && at != 0))
                continue;
            const QString value = t.mid(at + needle.size()).trimmed();
            m_loading = true;
            if (into->findText(value) < 0 && !value.isEmpty())
                into->addItem(value);  // 카메라가 아는 값이 목록에 없으면 추가
            into->setCurrentText(value);
            m_loading = false;
            return;
        }
    });
}

void ImagePanel::send_set(const QString &submenu, const QString &key,
                          const QString &value)
{
    // ISP 쓰기는 전부 이 함수를 지난다 — 백스톱을 여기 하나만 두면 된다(§5).
    if (!Auth::can(Auth::Action::CameraTuning)) {
        show_result(Auth::deny_reason(Auth::Action::CameraTuning), false);
        return;
    }

    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/image.cgi");
    url.setQuery(QString("msubmenu=%1&action=set&Channel=%2&%3=%4")
                     .arg(submenu).arg(m_ch->currentIndex()).arg(key, value));
    QNetworkRequest req = sunapi_request(url);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, key, value] {
                reply->deleteLater();
                const QString body =
                    QString::fromUtf8(reply->readAll()).trimmed();
                if (reply->error() != QNetworkReply::NoError)
                    show_result(QString("%1 apply failed (%2)")
                                    .arg(key, reply->errorString()), false);
                else if (!body.startsWith("OK"))
                    show_result(QString("camera refused: %1")
                                    .arg(body.left(60)), false);
                else
                    show_result(QString("%1 = %2 applied")
                                    .arg(key, value), true);
                fetch_all();  // 진실로 수렴
            });
}

void ImagePanel::show_result(const QString &text, bool ok)
{
    m_result->setText(text);
    Theme::restyle(m_result, [=] {
        return QString("color:%1;")
                                .arg((ok ? Theme::green : Theme::alarm).name());
    });
    m_result->show();
    QTimer::singleShot(4000, m_result, &QLabel::hide);
}
