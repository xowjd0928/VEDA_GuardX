#include "camera_control.h"
#include "auth.h"
#include "credentials.h"
#include "sunapi_request.h"

#include <QAuthenticator>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>

CameraControl::CameraControl(QObject *parent)
    : QObject(parent)
{
    m_net = new QNetworkAccessManager(this);
    Credentials::install_tls_pinning(m_net);
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [](QNetworkReply *reply, QAuthenticator *auth) {
                if (auth->user() == Credentials::camera_user()) {
                    qWarning() << "[CameraControl] 인증 거부 — Admin 계정 확인 필요";
                    reply->abort();
                    return;
                }
                auth->setUser(Credentials::camera_user());
                auth->setPassword(Credentials::camera_password());
            });
}

void CameraControl::set_app(const QString &app_id, bool auto_start,
                            const QString &priority, const QString &label)
{
    // 권한 백스톱 (§5) — 카메라로 명령이 나가는 관문이다.
    if (!Auth::can(Auth::Action::CameraApps)) {
        qWarning() << "[CameraControl] 권한 없는 앱 설정 변경 차단";
        return;
    }

    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/opensdk.cgi");
    url.setQuery(QString("msubmenu=apps&action=set&AppID=%1&Priority=%2&AutoStart=%3")
                     .arg(app_id, priority,
                          auto_start ? QStringLiteral("True")
                                     : QStringLiteral("False")));

    QNetworkRequest req = sunapi_request(url);
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, app_id, label] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[CameraControl]" << app_id << label
                       << "설정 실패:" << reply->errorString();
            emit app_set_finished(app_id, label, false, reply->errorString());
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        const bool ok =
            obj.value("Response").toString().compare(QLatin1String("Success"),
                                                     Qt::CaseInsensitive) == 0;
        QString error;
        if (!ok) {
            error = obj.value("Error").toObject().value("Details").toString();
            if (error.isEmpty())
                error = QStringLiteral("unexpected response");
            qWarning() << "[CameraControl]" << app_id << label << "설정 거부:" << error;
        } else {
            qInfo() << "[CameraControl]" << app_id << label << "설정 수락됨";
        }
        emit app_set_finished(app_id, label, ok, error);
    });
}

void CameraControl::reboot_camera()
{
    // 권한 백스톱 (§5) — 카메라로 명령이 나가는 관문이다.
    if (!Auth::can(Auth::Action::CameraSystem)) {
        qWarning() << "[CameraControl] 권한 없는 카메라 재부팅 차단";
        return;
    }

    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/system.cgi");
    url.setQuery("msubmenu=power&action=control&Type=Restart");

    QNetworkRequest req = sunapi_request(url);
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            qInfo() << "[CameraControl] 재부팅 요청 수락됨";
            emit reboot_finished(true, QStringLiteral("request accepted"));
            return;
        }
        // 재부팅이 응답보다 빨리 시작되면 여기로 온다 — 실패 단정 금지
        qWarning() << "[CameraControl] 재부팅 응답 없음(재부팅 중일 수 있음):"
                   << reply->errorString();
        emit reboot_finished(true, reply->errorString());
    });
}

void CameraControl::control_app(const QString &app_id, bool start)
{
    // 권한 백스톱 (§5) — 카메라로 명령이 나가는 관문이다.
    if (!Auth::can(Auth::Action::CameraApps)) {
        qWarning() << "[CameraControl] 권한 없는 앱 시작/정지 차단";
        return;
    }

    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/opensdk.cgi");
    url.setQuery(QString("msubmenu=apps&action=control&AppID=%1&Mode=%2")
                     .arg(app_id, start ? QStringLiteral("Start")
                                        : QStringLiteral("Stop")));

    QNetworkRequest req = sunapi_request(url);
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, app_id, start] {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[CameraControl]" << app_id
                       << (start ? "Start" : "Stop")
                       << "실패:" << reply->errorString();
            emit app_control_finished(app_id, start, false, reply->errorString());
            return;
        }

        // 정상 응답 {"Response":"Success"} / 실패 {"Error":{"Code":..,"Details":..}}
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        const bool ok =
            obj.value("Response").toString().compare(QLatin1String("Success"),
                                                     Qt::CaseInsensitive) == 0;
        QString error;
        if (!ok) {
            const QJsonObject err = obj.value("Error").toObject();
            error = err.value("Details").toString();
            if (error.isEmpty())
                error = QStringLiteral("unexpected response");
            qWarning() << "[CameraControl]" << app_id << "조작 거부:" << error;
        } else {
            qInfo() << "[CameraControl]" << app_id
                    << (start ? "Start" : "Stop") << "요청 수락됨";
        }
        emit app_control_finished(app_id, start, ok, error);
    });
}
