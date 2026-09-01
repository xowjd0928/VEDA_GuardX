#include "network_panel.h"
#include "credentials.h"
#include "panel_chrome.h"
#include "sunapi_request.h"
#include "theme.h"

#include <QAuthenticator>
#include <QDebug>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QVBoxLayout>

NetworkPanel::NetworkPanel(QWidget *parent)
    : QWidget(parent)
{
    m_net = new QNetworkAccessManager(this);
    Credentials::install_tls_pinning(m_net);
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [](QNetworkReply *reply, QAuthenticator *auth) {
                if (auth->user() == Credentials::camera_user()) {
                    qWarning() << "[NetworkPanel] 인증 거부 — 계정 확인 필요";
                    reply->abort();
                    return;
                }
                auth->setUser(Credentials::camera_user());
                auth->setPassword(Credentials::camera_password());
            });

    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 8, 0, 0);
    col->setSpacing(14);

    struct { const char *title; const char *caption; const char *submenu; } CARDS[] = {
        {"Interface", "network.cgi interface · IP/DNS/MAC", "interface"},
        {"RTSP", "network.cgi rtsp · port/auth", "rtsp"},
        {"QoS", "network.cgi qos", "qos"},
    };
    for (const auto &c : CARDS) {
        auto *w = new QWidget(this);
        auto *card = new QVBoxLayout(w);
        card->setContentsMargins(0, 0, 0, 0);
        card->setSpacing(0);
        card->addWidget(PanelChrome::header(QString::fromUtf8(c.title),
                                            QString::fromUtf8(c.caption), w));
        auto *body = new QLabel("fetching...", w);
        body->setFont(Theme::mono_font(10));
        Theme::restyle(body, [=] {
            return QString("color:%1; padding:8px 2px;")
                                .arg(Theme::textMid.name());
        });
        body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        card->addWidget(body);
        col->addWidget(w);
        m_cards.append({QLatin1String(c.submenu), body});
    }

    auto *note = new QLabel(
        "Read only - changing network settings is deliberately not offered "
        "(a remote change risks losing the connection)", this);
    note->setFont(Theme::mono_font(10));
    Theme::restyle(note, [=] {
        return QString("color:%1;").arg(Theme::textFaint.name());
    });
    col->addWidget(note);
    col->addStretch(1);
}

void NetworkPanel::showEvent(QShowEvent *ev)
{
    QWidget::showEvent(ev);
    if (m_loaded)
        return;  // 거의 안 변한다 — 재진입 재조회는 불필요
    m_loaded = true;
    for (const auto &[submenu, body] : std::as_const(m_cards))
        fetch(submenu, body);
}

void NetworkPanel::fetch(const QString &submenu, QLabel *into)
{
    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/network.cgi");
    url.setQuery(QString("msubmenu=%1&action=view").arg(submenu));
    // 텍스트 원문 표시 (조회 전용 §3-F) — Accept: json을 붙이지 않는다
    QNetworkRequest req = sunapi_request(url);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, into] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const int http = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            into->setText(http == 608 || http == 404
                              ? QString("not supported by this device")
                              : QString("fetch failed (%1)")
                                    .arg(reply->errorString()));
            return;
        }
        QStringList lines = QString::fromUtf8(reply->readAll())
                                .split('\n', Qt::SkipEmptyParts);
        if (lines.size() > 30) {
            const int dropped = lines.size() - 30;
            lines = lines.mid(0, 30);
            lines << QString("... (+%1 more lines)").arg(dropped);
        }
        into->setText(lines.isEmpty() ? QString("no response")
                                      : lines.join('\n'));
    });
}
