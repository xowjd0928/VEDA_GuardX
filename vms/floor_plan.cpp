#include "floor_plan.h"
#include "calibration_store.h"
#include "credentials.h"

#include <QDateTime>
#include <QPainter>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QSettings>

namespace FloorPlan {

QString path()
{
    const QString ini = QSettings("GuardX", "VMS")
                            .value("floorplan_path").toString();
    if (!ini.isEmpty())
        return ini;
    // 자격·인증서와 같은 폴더에 둔다 — 사용자가 이미 아는 자리이고,
    // 설치 경로·작업 디렉터리에 의존하지 않는다 (Credentials::certs_dir 규약).
    return QFileInfo(Credentials::config_path()).absolutePath()
           + "/floorplan.png";
}

QPixmap image()
{
    static QPixmap cached;
    static QString cached_path;
    static qint64 cached_size = -1;
    static QDateTime cached_time;

    const QString p = path();
    const QFileInfo fi(p);
    if (!fi.exists() || !fi.isFile()) {
        cached = QPixmap();
        cached_path.clear();
        cached_size = -1;
        return cached;
    }

    // 같은 파일이 그대로면 다시 읽지 않는다 — 지도는 250ms 마다 다시 그린다.
    if (p == cached_path && fi.size() == cached_size
        && fi.lastModified() == cached_time)
        return cached;

    cached = QPixmap(p);
    cached_path = p;
    cached_size = fi.size();
    cached_time = fi.lastModified();
    return cached;
}

namespace {

/** @brief 사각형 안에 세로선 n개로 칸을 나눈다 (벤치 좌석·패널 분할) */
void split_v(QPainter &p, const QRectF &r, int parts)
{
    for (int i = 1; i < parts; ++i) {
        const double x = r.left() + r.width() * i / parts;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
}

/** @brief 사각형 안에 가로선 n개로 칸을 나눈다 (진열장 선반) */
void split_h(QPainter &p, const QRectF &r, int parts)
{
    for (int i = 1; i < parts; ++i) {
        const double y = r.top() + r.height() * i / parts;
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
}

}  // namespace

void draw_fixture(QPainter &p, const QRectF &box, const QString &name,
                  const QColor &ink, bool dashed)
{
    if (box.width() < 2.0 || box.height() < 2.0)
        return;

    p.save();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ink, 1.1, dashed ? Qt::DashLine : Qt::SolidLine));
    p.drawRect(box);

    // 안쪽 기호는 실선으로 (점선은 바깥 테두리만 — 그게 "못 믿는 좌표"의 표시다)
    p.setPen(QPen(ink, 0.9));
    const bool tall = box.height() >= box.width();
    // 작은 칸에 기호를 넣으면 선만 뭉친다 — 바깥 네모로 끝낸다.
    const double small = 9.0;

    if (name.contains(QStringLiteral("LED"), Qt::CaseInsensitive)) {
        // 디스플레이 월 — 상판 위 패널 셋 (작은 것 · 큰 것 · 세운 것)
        if (box.width() < small * 3 || box.height() < small) { p.restore(); return; }
        const QRectF in = box.adjusted(box.width() * 0.06, box.height() * 0.18,
                                       -box.width() * 0.06, -box.height() * 0.18);
        const double u = in.width();
        p.drawRect(QRectF(in.left(), in.top(), u * 0.14, in.height()));
        p.drawRect(QRectF(in.left() + u * 0.22, in.top(), u * 0.44, in.height()));
        p.drawRect(QRectF(in.left() + u * 0.74, in.top(), u * 0.10, in.height()));
    } else if (name.contains(QStringLiteral("벤치"))) {
        // 벤치 — 좌석 분할 (긴 쪽으로 3칸)
        if (qMax(box.width(), box.height()) < small * 2) { p.restore(); return; }
        const QRectF in = box.adjusted(2, 2, -2, -2);
        if (in.width() > 4 && in.height() > 4) {
            p.drawRect(in);
            tall ? split_h(p, in, 3) : split_v(p, in, 3);
        }
    } else if (name.contains(QStringLiteral("의자"))) {
        // 의자 — 등받이 한 줄
        if (qMin(box.width(), box.height()) < 7.0) { p.restore(); return; }
        const QRectF in = box.adjusted(box.width() * 0.22, box.height() * 0.22,
                                       -box.width() * 0.22, -box.height() * 0.22);
        p.drawRect(in);
    } else if (tall) {
        // 세로 진열장 — 선반 칸. 높이에 따라 칸 수를 정한다(선이 뭉치지 않게)
        if (box.height() < small * 2) { p.restore(); return; }
        const QRectF in = box.adjusted(2, 2, -2, -2);
        const int shelves = qBound(3, int(in.height() / 9.0), 8);
        p.drawRect(in);
        split_h(p, in, shelves);
    } else {
        // 가로 카운터 — 상판 위 물건 둘
        if (box.width() < small * 2 || box.height() < small) { p.restore(); return; }
        const QRectF in = box.adjusted(box.width() * 0.08, box.height() * 0.22,
                                       -box.width() * 0.08, -box.height() * 0.22);
        const double u = in.width();
        p.drawRect(QRectF(in.left(), in.top(), u * 0.22, in.height()));
        p.drawRect(QRectF(in.left() + u * 0.58, in.top(), u * 0.18, in.height()));
        p.drawRect(QRectF(in.left() + u * 0.82, in.top(), u * 0.18, in.height()));
    }
    p.restore();
}

QString props_dir()
{
    return QFileInfo(Credentials::config_path()).absolutePath() + "/props";
}

QPixmap prop_image(int index, int channel, const QString &name)
{
    // 후보 이름 — 구체적인 것부터. 확장자는 흔한 네 가지.
    QStringList stems;
    stems << QString::number(index)
          << QString("CH%1-%2").arg(channel + 1).arg(name)
          << name;
    QString found;
    for (const QString &stem : stems) {
        for (const char *ext : { ".png", ".jpg", ".jpeg", ".webp" }) {
            const QString cand = props_dir() + "/" + stem + QString::fromLatin1(ext);
            if (QFileInfo::exists(cand)) { found = cand; break; }
        }
        if (!found.isEmpty()) break;
    }
    if (found.isEmpty())
        return QPixmap();

    // 파일이 그대로면 다시 읽지 않는다 — 지도는 250ms 마다 다시 그린다.
    struct Entry { QPixmap pix; qint64 size; QDateTime time; };
    static QHash<QString, Entry> cache;
    const QFileInfo fi(found);
    auto it = cache.find(found);
    if (it != cache.end() && it->size == fi.size() && it->time == fi.lastModified())
        return it->pix;
    const Entry e { QPixmap(found), fi.size(), fi.lastModified() };
    cache.insert(found, e);
    return e.pix;
}

bool show_prop_keys()
{
    return QSettings("GuardX", "VMS")
        .value("floorplan_prop_keys", false).toBool();
}

void refresh_fixture_list()
{
    const QVector<CalibrationStore::Obstacle> obs =
        CalibrationStore::instance()->obstacles();

    // 목록이 그대로면 파일을 다시 쓰지 않는다 (지도는 250ms 마다 그린다)
    QString sig;
    for (const CalibrationStore::Obstacle &o : obs)
        sig += QString("%1/%2;").arg(o.channel).arg(o.name);
    static QString last_sig;
    if (sig == last_sig || obs.isEmpty())
        return;
    last_sig = sig;

    QDir().mkpath(props_dir());
    QFile f(props_dir() + "/_fixtures.txt");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << "# GuardX 평면도 가구 목록 — 앱이 자동으로 씁니다 (고쳐도 덮어써집니다)\n"
        << "# 사진을 붙이려면 이 폴더에 <번호>.png 로 저장하세요 (예: 3.png).\n"
        << "#   같은 이름 전부에 붙이려면 <이름>.png · 채널까지 나누려면 CH<n>-<이름>.png\n"
        << "#   확장자 png·jpg·jpeg·webp. 덮어쓰면 앱을 끄지 않아도 바로 바뀝니다.\n\n";
    for (int i = 0; i < obs.size(); ++i) {
        const CalibrationStore::Obstacle &o = obs[i];
        double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (const QPointF &p : o.footprint_cm) {
            x0 = qMin(x0, p.x()); x1 = qMax(x1, p.x());
            y0 = qMin(y0, p.y()); y1 = qMax(y1, p.y());
        }
        const double w = x1 - x0, h = y1 - y0;
        out << QString("%1.png  =  CH%2 · %3  (%4x%5 cm, %6)  %7\n")
                   .arg(i + 1, 2)
                   .arg(o.channel + 1)
                   .arg(o.name, -12)
                   .arg(int(w)).arg(int(h))
                   .arg(h >= w ? QStringLiteral("세로") : QStringLiteral("가로"))
                   .arg(prop_image(i + 1, o.channel, o.name).isNull()
                            ? QStringLiteral("[사진 없음]")
                            : QStringLiteral("[사진 있음]"));
    }
}

bool show_obstacles()
{
    return QSettings("GuardX", "VMS")
        .value("floorplan_show_obstacles", false).toBool();
}

}  // namespace FloorPlan
