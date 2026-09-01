#include "credentials.h"
#include "sunapi_request.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace {

struct Store {
    QString error;
    QString path;

    QString cam_host = "192.168.0.3";
    QString cam_user;
    QString cam_pass;
    bool    cam_https = false;
    QString cam_pin;

    // MQTT — DB 직결을 대체한 통로 (docs/DB_LINK_AND_MQTT_MIGRATION.md).
    // 브로커는 RPi B(mosquitto). 카메라(192.168.0.3)와 다른 기계다.
    // 주소는 비밀이 아니고 팀이 같은 브로커를 쓰므로 기본값으로 둔다 —
    // 새 PC에서 [mqtt] 섹션 없이도 동작하게 하려는 것.
    //
    // ⚠ 기본 포트는 **8883(mTLS)** 다 (08-12). 1883 이던 동안에는 인증서를
    //    받은 팀원 PC 가 평문 리스너에 TLS 핸드셰이크를 던지며 무한 재시도만
    //    했다 — ini 없이도 "인증서만 두면 붙는" 상태가 목표다.
    //    host 는 LAN IP 로 둔다. 서버 인증서 SAN 에 DNS:rpib 와
    //    IP:172.20.33.251 이 둘 다 있어 hostname 검증을 통과한다(08-12 실측).
    //    1883 롤백은 [mqtt] tls=0 + port=1883 둘 다 명시해야 한다.
    QString mqtt_host = "172.20.33.251";
    QString mqtt_tls_ca, mqtt_tls_cert, mqtt_tls_key;
    QString mqtt_client_id;   // [mqtt] client_id — 비우면 "vms-{hostname}"
    int     mqtt_port = 8883;
    QString mqtt_user;
    QString mqtt_pass;

    bool loaded = false;
};

Store &store()
{
    static Store s;
    return s;
}

const char *DPAPI_PREFIX = "dpapi:";

/**
 * @brief 리포에 동봉된 신뢰 인증서들의 SHA-256 지문 (`:/certs/*.pem`)
 *
 * **왜 파일로 두는가:** 예전엔 PC마다 `--pin-camera-cert` 를 돌려 지문을
 * `credentials.ini` 에 적었다. 그러면 (1) 새 팀원마다 수동 절차가 생기고
 * (2) 지문을 메신저로 옮겨 적다 틀릴 수 있고 (3) 각자 TOFU 라 아무도 대조를
 * 안 하며 (4) 카메라 교체 시 전원이 다시 해야 했다. 인증서를 git 에 두면
 * 배포가 곧 pull 이고, 바뀌면 PR diff 로 보인다.
 *
 * 인증서는 **공개 정보**다 — 카메라가 접속자 누구에게나 건네주는 값이라
 * 리포에 있어도 새는 비밀이 없다.
 *
 * 신뢰 강도는 지문 고정과 동일하다(인증서 완전 일치). 사설 루트 CA 를
 * 신뢰하는 방식이 아니므로, 같은 CA 가 발급한 **다른** 한화 장비까지
 * 덩달아 신뢰하는 일은 없다. (애초에 이 카메라는 체인에 리프 1장만 보내서
 * CA 검증은 선택지도 아니었다 — 2026-08-03 확인)
 */
QStringList bundled_pins()
{
    static const QStringList pins = [] {
        QStringList out;
        QDirIterator it(QStringLiteral(":/certs"), { "*.pem", "*.crt" },
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            // 한 파일에 여러 장이 들어 있어도 전부 받는다 (교체 과도기)
            const QList<QSslCertificate> certs =
                QSslCertificate::fromPath(path, QSsl::Pem);
            if (certs.isEmpty()) {
                qWarning() << "[Credentials] 동봉 인증서를 못 읽음:" << path;
                continue;
            }
            for (const QSslCertificate &c : certs) {
                out << QString::fromLatin1(
                    c.digest(QCryptographicHash::Sha256).toHex()).toLower();
            }
        }
        return out;
    }();
    return pins;
}

/** @brief 환경변수가 있으면 그것을, 없으면 ini 값을 (DPAPI면 풀어서) 반환 */
QString read_secret(QSettings &ini, const QString &key, const char *env_name)
{
    const QByteArray env = qgetenv(env_name);
    if (!env.isEmpty())
        return QString::fromUtf8(env);
    return Credentials::unprotect(ini.value(key).toString());
}

} // namespace

QString Credentials::protect(const QString &plain)
{
#ifdef Q_OS_WIN
    if (plain.isEmpty() || plain.startsWith(DPAPI_PREFIX))
        return plain;

    const QByteArray raw = plain.toUtf8();
    DATA_BLOB in{ DWORD(raw.size()), reinterpret_cast<BYTE *>(const_cast<char *>(raw.constData())) };
    DATA_BLOB out{ 0, nullptr };
    if (!CryptProtectData(&in, L"GuardX VMS", nullptr, nullptr, nullptr, 0, &out)) {
        qWarning() << "[Credentials] DPAPI 암호화 실패 — 평문 유지";
        return plain;
    }
    const QByteArray enc(reinterpret_cast<char *>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return QString::fromLatin1(DPAPI_PREFIX) + QString::fromLatin1(enc.toBase64());
#else
    return plain;
#endif
}

QString Credentials::unprotect(const QString &stored)
{
#ifdef Q_OS_WIN
    if (!stored.startsWith(DPAPI_PREFIX))
        return stored;

    QByteArray enc = QByteArray::fromBase64(
        stored.mid(int(qstrlen(DPAPI_PREFIX))).toLatin1());
    DATA_BLOB in{ DWORD(enc.size()), reinterpret_cast<BYTE *>(enc.data()) };
    DATA_BLOB out{ 0, nullptr };
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        qWarning() << "[Credentials] DPAPI 복호화 실패 — 다른 계정에서 암호화된 값?";
        return QString();
    }
    const QString plain =
        QString::fromUtf8(reinterpret_cast<char *>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return plain;
#else
    return stored;
#endif
}

QString Credentials::config_path()
{
    if (!store().path.isEmpty())
        return store().path;

    const QByteArray override_path = qgetenv("GUARDX_CREDENTIALS");
    if (!override_path.isEmpty())
        return QString::fromUtf8(override_path);

    // 공유폴더/리포 밖에 둔다 — 소스와 함께 새어나가지 않도록.
    // GenericConfigLocation = %APPDATA% (앱 이름이 끼지 않아 경로가 예측 가능)
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir(dir).filePath("GuardX/credentials.ini");
}

bool Credentials::load()
{
    Store &s = store();
    s.path = config_path();
    s.error.clear();

    if (!QFile::exists(s.path)) {
        s.error = QString("자격 파일이 없습니다: %1\n"
                          "[camera] 섹션에 host·user·password를 넣고 "
                          "다시 실행하세요.").arg(s.path);
        return false;
    }

    QSettings ini(s.path, QSettings::IniFormat);

    ini.beginGroup("camera");
    s.cam_host  = ini.value("host", s.cam_host).toString();
    s.cam_user  = read_secret(ini, "user", "GUARDX_CAM_USER");
    s.cam_pass  = read_secret(ini, "password", "GUARDX_CAM_PASSWORD");
    s.cam_https = ini.value("https", false).toBool();
    s.cam_pin   = ini.value("cert_sha256").toString().remove(':').toLower();
    ini.endGroup();

    // [database]는 v6에서 읽지 않는다. 남아 있어도 무시되므로 기존 파일을
    // 고칠 필요는 없지만, 쓰이지 않는 비밀값을 계속 두는 것은 위험만 남기니
    // 지우는 편이 낫다.

    // [mqtt]는 없어도 된다 — 없으면 기본값으로 붙어보고,
    // 실패하면 MqttLink가 재시도만 한다. DB 경로는 그대로 살아 있으므로
    // 이 섹션이 비었다고 앱을 막지 않는다.
    ini.beginGroup("mqtt");
    s.mqtt_host = ini.value("host", s.mqtt_host).toString();
    s.mqtt_port = ini.value("port", s.mqtt_port).toInt();
    // 세션 id 만 바꾼다 — 계정(CN)·인증서 파일명은 그대로다. 한 PC 에서
    // 앱을 두 개 띄우면 같은 id 라 서로 세션을 끊는데(개발 병행), 그때
    // 한쪽만 client_id=vms-3-11-b 처럼 달리 주면 된다.
    s.mqtt_client_id = ini.value("client_id").toString().trimmed();
    s.mqtt_user = read_secret(ini, "user", "GUARDX_MQTT_USER");
    s.mqtt_pass = read_secret(ini, "password", "GUARDX_MQTT_PASSWORD");
    // mTLS 파일 경로. 비밀번호가 아니라 **경로**라 DPAPI 대상이 아니다 —
    // 지켜야 할 것은 키 파일 자체이고, 그건 기기 로컬에 둔다(리포 금지).
    s.mqtt_tls_ca   = ini.value("tls_ca").toString();
    s.mqtt_tls_cert = ini.value("tls_cert").toString();
    s.mqtt_tls_key  = ini.value("tls_key").toString();
    ini.endGroup();

    if (s.cam_user.isEmpty() || s.cam_pass.isEmpty()) {
        s.error = QString("%1 의 [camera] user/password가 비어 있습니다.").arg(s.path);
        return false;
    }

    // 검증할 수단이 전혀 없을 때만 HTTP로 내려간다. 동봉 인증서(:/certs)가
    // 있으면 ini 에 핀이 없어도 정상이다 — 그게 기본 경로다.
    if (s.cam_https && s.cam_pin.isEmpty() && bundled_pins().isEmpty()) {
        qWarning() << "[Credentials] https=true 인데 신뢰할 인증서가 없습니다 — "
                      "검증 불가로 HTTP로 내려갑니다. certs/ 에 카메라 PEM 을 "
                      "넣거나 --pin-camera-cert 를 쓰세요 (docs/SECURITY_SETUP.md).";
        s.cam_https = false;
    }

    s.loaded = true;
    return true;
}

QString Credentials::last_error()   { return store().error; }
QString Credentials::camera_host()  { return store().cam_host; }
QString Credentials::camera_user()  { return store().cam_user; }
QString Credentials::camera_password() { return store().cam_pass; }
bool    Credentials::camera_https() { return store().cam_https; }
QString Credentials::camera_cert_pin() { return store().cam_pin; }
QString Credentials::certs_dir()
{
    // 자격 파일이 있는 폴더 아래로 둔다 = C:/Users/<계정>/AppData/Local/GuardX/certs.
    // QStandardPaths::AppLocalDataLocation 을 그대로 쓰지 않는 이유: 이 앱은
    // organizationName/applicationName 을 세팅하지 않아 그 값이 실행 파일 이름
    // (gstream_VMS)으로 풀린다. 자격 파일과 같은 뿌리를 쓰면 경로가 한 곳에서
    // 정해지고, 자격과 인증서가 늘 같이 있다.
    return QFileInfo(config_path()).absolutePath() + "/certs";
}

QString Credentials::mqtt_tls_ca(const QString &)
{
    const QString ini = store().mqtt_tls_ca;
    return ini.isEmpty() ? certs_dir() + "/ca.crt" : ini;
}

QString Credentials::mqtt_tls_cert(const QString &cn)
{
    const QString ini = store().mqtt_tls_cert;
    return ini.isEmpty() ? certs_dir() + "/" + cn + ".crt" : ini;
}

QString Credentials::mqtt_tls_key(const QString &cn)
{
    const QString ini = store().mqtt_tls_key;
    return ini.isEmpty() ? certs_dir() + "/" + cn + ".key" : ini;
}

bool Credentials::mqtt_tls_opt_out()
{
    // 인증서를 지우게 만들지 않는 롤백 스위치. **이 값이 0일 때만** 평문이다.
    return !QSettings(config_path(), QSettings::IniFormat)
                .value("mqtt/tls", true).toBool();
}

QString Credentials::mqtt_host()    { return store().mqtt_host; }
int     Credentials::mqtt_port()    { return store().mqtt_port; }
QString Credentials::mqtt_client_id() { return store().mqtt_client_id; }
QString Credentials::mqtt_user()    { return store().mqtt_user; }
QString Credentials::mqtt_password() { return store().mqtt_pass; }

QUrl Credentials::camera_base_url()
{
    QUrl url;
    url.setScheme(camera_https() ? "https" : "http");
    url.setHost(camera_host());
    return url;
}

QUrl Credentials::calibration_ui_url()
{
    QUrl url = camera_base_url();
    url.setPath("/home/setup/opensdk/html/test_calibration/index.html");
    url.setQuery("AppName=test_calibration");
    return url;
}

/**
 * @brief 피어 인증서가 허용 핀과 일치하는가 (불일치·부재면 경고 후 false)
 *
 * 허용 지문 = 리포 동봉 인증서(:/certs/*.pem) + credentials.ini 재정의.
 * 보통은 동봉본만으로 충분하고, ini 항목은 다른 카메라를 붙이는 사람용
 * 탈출구다 (리포를 고치지 않고 자기 PC만 예외 처리).
 *
 * ⚠ 이 판정은 **두 곳**에서 불린다(sslErrors · encrypted). 한 곳에만 두면
 * 안 되는 이유는 install_tls_pinning() 주석에 적어뒀다. 판정을 함수 하나로
 * 묶어두는 것이 그 두 자리가 어긋나지 않게 하는 유일한 방법이다.
 */
static bool peer_pin_matches(QNetworkReply *reply)
{
    QStringList allowed = bundled_pins();
    if (const QString ini_pin = Credentials::camera_cert_pin(); !ini_pin.isEmpty())
        allowed << ini_pin;

    if (allowed.isEmpty()) {
        qWarning() << "[Credentials] 신뢰 인증서 없음 — TLS 연결 거부. "
                      "certs/ 가 리소스에 포함됐는지 확인 "
                      "(docs/SECURITY_SETUP.md)";
        return false;
    }

    const QSslCertificate peer = reply->sslConfiguration().peerCertificate();
    if (peer.isNull()) {
        qWarning() << "[Credentials] 피어 인증서 없음 — 거부";
        return false;
    }

    const QString actual = QString::fromLatin1(
        peer.digest(QCryptographicHash::Sha256).toHex()).toLower();
    if (!allowed.contains(actual)) {
        qWarning() << "[Credentials] 인증서 지문 불일치 — 거부. 실제:" << actual
                   << "· 허용:" << allowed.join(", ")
                   << "— 카메라를 교체·초기화했다면 certs/ 의 PEM 을 갱신할 것"
                      " (docs/SECURITY_SETUP.md §1)";
        return false;
    }
    return true;
}

void Credentials::install_tls_pinning(QNetworkAccessManager *nam)
{
    // ---- ① 핸드셰이크 성공 경로 (2026-08-10 추가) ----
    // 원래 핀 대조가 sslErrors **안에만** 있었다. 그러면 검증을 통과한
    // 인증서는 핀을 아예 안 본다 — 즉 시스템 신뢰 저장소에 있는 CA가 발급한
    // 아무 인증서나 통과한다. 사설 CA가 어떤 PC의 저장소에 들어가 있거나,
    // 공인 CA로 발급받은 인증서를 내미는 중간자가 있으면 그대로 뚫린다.
    //
    // 지금 이 현장의 카메라는 사설 인증서라 **항상** sslErrors 를 거치므로
    // 실제로 뚫리고 있는 건 아니다. 고치는 이유는 그 사실이 카메라·PC 설정에
    // 달려 있기 때문이다 — 방어가 우연히 성립하고 있는 상태다.
    QObject::connect(nam, &QNetworkAccessManager::encrypted, nam,
                     [](QNetworkReply *reply) {
        if (peer_pin_matches(reply))
            return;
        // 오류가 하나도 없었는데 핀이 틀렸다 = 검증은 통과했지만 우리 카메라가
        // 아니다. 요청 본문(Digest 자격 포함)이 나가기 전에 끊는다.
        qWarning() << "[Credentials] 체인 검증은 통과했으나 핀 불일치 — 연결 중단";
        reply->abort();
    });

    // ---- ② 검증 실패 경로 (원래 있던 것) ----
    QObject::connect(nam, &QNetworkAccessManager::sslErrors, nam,
                     [](QNetworkReply *reply, const QList<QSslError> &errors) {
        if (!peer_pin_matches(reply))
            return;  // ignoreSslErrors를 부르지 않음 = 연결 중단

        // 지문이 일치하면 피어 신원은 **이미 암호학적으로 확인된 것**이다.
        // 체인 검증 실패는 그 다음 문제가 아니라 이 구성에서 **당연한 상태**다:
        //  - 이 카메라 인증서는 자체 서명이 아니라 "Hanwha Techwin Private Root
        //    CA 2" 발급이고, 그 루트는 Windows 신뢰 저장소에 없다
        //    → UnableToGetIssuerCertificate ("issuer certificate could not be found")
        //  - IP로 접속하므로 CN(*.hanwha-security.com) 불일치도 항상 난다
        //
        // ⚠ 2026-08-03: 예전엔 자체서명 3종만 예상 목록에 있어 위 오류가 매 요청
        // 경고로 찍혔다. DetectionFeed 가 200ms 폴링이라 초당 수십 줄이 쌓여
        // 정작 봐야 할 [Pipeline]·glass-to-arrival 로그를 덮었다.
        static const QSslError::SslError expected[] = {
            QSslError::SelfSignedCertificate,
            QSslError::SelfSignedCertificateInChain,
            QSslError::HostNameMismatch,
            QSslError::UnableToGetIssuerCertificate,
            QSslError::UnableToGetLocalIssuerCertificate,
            QSslError::UnableToVerifyFirstCertificate,
            QSslError::CertificateUntrusted,
        };

        // 예상 밖 오류(만료·폐기 등)는 핀과 무관하게 알아야 한다 — 핀은 신원만
        // 보증하지 인증서가 아직 유효한지는 말해주지 않는다. 다만 **종류별 1회**만
        // 찍는다. 같은 이유로 로그를 채우는 것이 바로 위에서 고친 문제다.
        static QList<QSslError::SslError> reported;
        for (const QSslError &e : errors) {
            if (std::find(std::begin(expected), std::end(expected), e.error())
                != std::end(expected))
                continue;
            if (reported.contains(e.error()))
                continue;
            reported.append(e.error());
            qWarning() << "[Credentials] 핀은 일치하나 예상 밖 오류 (이 종류는 "
                          "최초 1회만 보고):" << e.errorString();
        }
        reply->ignoreSslErrors(errors);
    });
}

bool Credentials::pin_current_camera_cert(QString *out_fingerprint, QString *out_error)
{
    if (!QSslSocket::supportsSsl()) {
        if (out_error)
            *out_error = QString("이 빌드에 TLS 지원이 없습니다 (OpenSSL DLL 필요). "
                                 "빌드 라이브러리: %1")
                             .arg(QSslSocket::sslLibraryBuildVersionString());
        return false;
    }

    QNetworkAccessManager nam;
    QUrl url;
    url.setScheme("https");
    url.setHost(camera_host());
    url.setPath("/stw-cgi/system.cgi");
    url.setQuery("msubmenu=deviceinfo&action=view");

    // 아래 QEventLoop가 15초 뒤 스스로 빠져나오지만, reply까지 끊어야 소켓이
    // 남지 않는다 — 같은 15초를 전송 타임아웃으로도 건다. 지문은 sslErrors에서
    // (핸드셰이크 시점에) 이미 잡히므로 타임아웃이 TOFU를 방해하지 않는다.
    QNetworkReply *reply = nam.get(sunapi_request(url, 15000));

    QString captured;
    QObject::connect(&nam, &QNetworkAccessManager::sslErrors, &nam,
                     [&captured](QNetworkReply *r, const QList<QSslError> &errs) {
        const QSslCertificate peer = r->sslConfiguration().peerCertificate();
        if (!peer.isNull())
            captured = QString::fromLatin1(
                peer.digest(QCryptographicHash::Sha256).toHex()).toLower();
        // TOFU: 사용자가 명시적으로 요청한 1회 신뢰 개시이므로 진행시킨다
        r->ignoreSslErrors(errs);
    });

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();

    if (captured.isEmpty()) {
        const QSslCertificate peer = reply->sslConfiguration().peerCertificate();
        if (!peer.isNull())
            captured = QString::fromLatin1(
                peer.digest(QCryptographicHash::Sha256).toHex()).toLower();
    }
    reply->deleteLater();

    if (captured.isEmpty()) {
        if (out_error)
            *out_error = "카메라에서 인증서를 받지 못했습니다 "
                         "(HTTPS가 꺼져 있거나 접속 불가).";
        return false;
    }

    QSettings ini(config_path(), QSettings::IniFormat);
    ini.beginGroup("camera");
    ini.setValue("cert_sha256", captured);
    ini.setValue("https", true);
    ini.endGroup();
    ini.sync();

    if (out_fingerprint)
        *out_fingerprint = captured;
    return true;
}

bool Credentials::encrypt_config_file(QString *out_error)
{
    const QString path = config_path();
    if (!QFile::exists(path)) {
        if (out_error)
            *out_error = QString("자격 파일이 없습니다: %1").arg(path);
        return false;
    }

    QSettings ini(path, QSettings::IniFormat);
    int changed = 0;
    for (const char *group : { "camera", "mqtt" }) {
        ini.beginGroup(QString::fromLatin1(group));
        for (const char *key : { "user", "password" }) {
            const QString k = QString::fromLatin1(key);
            const QString v = ini.value(k).toString();
            if (v.isEmpty() || v.startsWith(DPAPI_PREFIX))
                continue;
            ini.setValue(k, protect(v));
            ++changed;
        }
        ini.endGroup();
    }
    ini.sync();

    if (ini.status() != QSettings::NoError) {
        if (out_error)
            *out_error = "자격 파일 쓰기 실패";
        return false;
    }
    qInfo() << "[Credentials]" << changed << "개 값을 DPAPI로 암호화했습니다:" << path;
    return true;
}
