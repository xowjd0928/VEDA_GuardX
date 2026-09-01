#include "zone_settings_page.h"
#include "account_settings_card.h"
#include "auth.h"
#include "credentials.h"
#include "fire_charts.h"
#include "mqtt_link.h"
#include "panel_chrome.h"
#include "reauth_dialog.h"
#include "site_settings_card.h"
#include "storage_settings_card.h"
#include "theme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDebug>
#include <QDesktopServices>
#include <QFrame>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <cmath>

namespace {

const QString ZONES_TOPIC   = "guardx/db/rpib/zones";
const QString SETZONE_TOPIC = "guardx/db/rpib/cmd/set_zone";
const QString FIRE_TOPIC    = "guardx/db/rpib/fire_threshold";
const QString SETFIRE_TOPIC = "guardx/db/rpib/cmd/set_fire_threshold";
const int QOS = 1;

/// 헤더 부제 — 탭마다 다르다(무엇을 보고 있는지가 제목만으로는 안 읽힌다)
const char *TAB_SUB_GENERAL =
    "site · zone capacity · congestion thresholds · fire thresholds";

/**
 * @brief fire_threshold 편집 필드 정의
 *
 * 범위와 단위를 여기 한 곳에 모은다. DB CHECK(fire_schema.sql)와 어긋나면
 * 사용자는 입력할 수 있는데 저장만 실패하는 화면이 되므로, 값을 고칠 때는
 * 양쪽을 함께 봐야 한다.
 *
 * step 을 명시하는 이유: 가중치는 0.05, 퍼지 구간은 정수 단위로 움직이는 게
 * 자연스러운데 Qt 기본값(1.0)을 쓰면 가중치가 0→1 로 튀어 합계가 매번 깨진다.
 */
struct FireField {
    const char *key;        ///< DB 컬럼명 = MQTT 페이로드 키
    const char *label;
    double min;
    double max;
    int decimals;
    double step;
    const char *suffix;
};

const FireField FIRE_FIELDS[] = {
    // ── 퍼지 구간: 이 사이에서 위험도가 0→100 으로 선형 증가(또는 감소) ──
    { "gas_raw_min",          "Gas safe",       0,   1023, 0, 10,   " adc" },
    { "gas_raw_max",          "Gas danger",     0,   1023, 0, 10,   " adc" },
    // 스파크·습도는 내림차순 — safe 가 danger 보다 크다 (fire_schema.sql)
    { "spark_raw_safe",       "Flame safe",     0,   1023, 0, 10,   " adc" },
    { "spark_raw_danger",     "Flame danger",   0,   1023, 0, 10,   " adc" },
    { "temp_min_c",           "Temp safe",      -40, 150,  1, 1,    " °C"  },
    { "temp_max_c",           "Temp danger",    -40, 150,  1, 1,    " °C"  },
    { "humi_safe_percent",    "Humidity safe",  0,   100,  1, 1,    " %"   },
    { "humi_danger_percent",  "Humidity danger",0,   100,  1, 1,    " %"   },
    { "irtemp_min_c",         "Surface safe",   -40, 300,  1, 1,    " °C"  },
    { "irtemp_max_c",         "Surface danger", -40, 300,  1, 1,    " °C"  },

    // ── 가중치: 합 1.0 (DB가 CHECK 로 강제) ──
    { "weight_gas",           "Gas",            0,   1,    2, 0.05, ""     },
    { "weight_spark",         "Flame",          0,   1,    2, 0.05, ""     },
    { "weight_temp",          "Temperature",    0,   1,    2, 0.05, ""     },
    { "weight_humi",          "Humidity",       0,   1,    2, 0.05, ""     },
    { "weight_irtemp",        "Surface temp",   0,   1,    2, 0.05, ""     },

    // ── 판정 ──
    { "fire_score_threshold", "Fire threshold", 0,   100,  1, 1,    " pts" },
    { "n_confirm",            "Confirm cycles", 1,   999,  0, 1,    "×"    },
    { "n_recover",            "Recover cycles", 1,   999,  0, 1,    "×"    },
    { "freeze_relax_cycles",  "Freeze relax",   1,   9999, 0, 10,   "×"    },
    { "min_valid_weight",     "Min valid weight", 0.01, 1, 2, 0.05, ""     },
    { "override_spark_score", "Flame override", 0,   100,  1, 1,    " pts" },
    { "override_irtemp_score","Surface override",0,  100,  1, 1,    " pts" },
};

const QStringList FUZZY_KEYS = {
    "gas_raw_min", "gas_raw_max", "spark_raw_safe", "spark_raw_danger",
    "temp_min_c", "temp_max_c", "humi_safe_percent", "humi_danger_percent",
    "irtemp_min_c", "irtemp_max_c" };
const QStringList WEIGHT_KEYS = {
    "weight_gas", "weight_spark", "weight_temp", "weight_humi",
    "weight_irtemp" };
const QStringList DECIDE_KEYS = {
    "fire_score_threshold", "n_confirm", "n_recover", "freeze_relax_cycles",
    "min_valid_weight", "override_spark_score", "override_irtemp_score" };

/// 정수로 보내야 하는 컬럼 (DB가 INT) — 소수점이 붙으면 캐스트에서 죽는다
bool is_int_field(const QString &key)
{
    return key == "n_confirm" || key == "n_recover"
        || key == "freeze_relax_cycles";
}

const FireField *find_field(const QString &key)
{
    for (const FireField &f : FIRE_FIELDS)
        if (key == QLatin1String(f.key))
            return &f;
    return nullptr;
}

// 폴러의 검증 범위와 맞춰야 한다 (task_vms.cpp CAP_MIN/CAP_MAX).
// 여기서 먼저 막으면 왕복 없이 즉시 알려줄 수 있다.
const int CAP_MIN = 1;
const int CAP_MAX = 10000;

// 이름 길이 — 폴러는 120바이트로 막는다(UTF-8). 한글 3바이트 기준 40자면
// 그 안에 들어오고, LIVE 타일 좌상단이 감당하는 폭이기도 하다.
const int NAME_MAX_CHARS = 40;

/**
 * @brief [적용] 버튼 스타일 — 저장 안 된 변경이 있으면 브랜드 오렌지로 채운다
 *
 * 색을 위젯 코드에 흩뿌리지 않도록 Theme 상수에서 만든다(theme.h 규칙).
 * 평상시는 테두리만 있는 #OutlineBtn 모양이라 눈에 띄지 않고, 고칠 것이
 * 생겼을 때만 오렌지로 차오른다 — 어느 줄을 아직 안 보냈는지가 한눈에
 * 보인다. (08-19 보드: 활성/변경 강조는 파랑이 아니라 오렌지다.)
 */
QString apply_qss(bool dirty)
{
    // :disabled 는 zone_id 를 아직 못 받은 줄 — 색과 무관하게 눌리지 않는다
    const QString common =
        QString("QPushButton { border-radius:2px; padding:4px 14px; }"
                "QPushButton:disabled { background:transparent; color:%1;"
                "                       border:1px solid %2; font-weight:400; }")
            .arg(Theme::textFaint.name(), Theme::border.name());

    if (dirty)
        return common
             + QString("QPushButton { background:%1; border:1px solid %1;"
                       "              color:%2; font-weight:700; }"
                       "QPushButton:hover { background:%3; border-color:%3; }")
                   .arg(Theme::chromeSelText.name(), Theme::bg0.name(),
                        Theme::chromeSelText.lighter(115).name());

    return common
         + QString("QPushButton { background:transparent; border:1px solid %1;"
                   "              color:%2; font-weight:400; }"
                   "QPushButton:hover { border-color:%3; color:%4; }")
               .arg(Theme::border2.name(), Theme::textMuted.name(),
                    Theme::textFaint.name(), Theme::textHi.name());
}

/**
 * @brief 두 색을 k 비율로 섞는다 (k=0 이면 base, 1 이면 over)
 *
 * 새 상수를 theme.h 에 만들지 않으려는 것이다. "바뀐 칸" 배경은 accent 를
 * 아주 옅게 깐 색인데, 그건 독립된 디자인 토큰이 아니라 elevated 와 accent
 * 사이의 파생값이라 여기서 만드는 편이 맞다 (theme.h 의 색 규칙 참조).
 */
QColor mix(const QColor &base, const QColor &over, double k)
{
    return QColor(int(base.red()   + (over.red()   - base.red())   * k),
                  int(base.green() + (over.green() - base.green()) * k),
                  int(base.blue()  + (over.blue()  - base.blue())  * k));
}

/**
 * @brief 계절 프리셋 버튼 — 마지막으로 불러온 것만 테두리·글자에 색
 *
 * [적용]의 "가득 찬 오렌지"와 구분한다. 그쪽은 *해야 할 일*(아직 안 보냄)이고
 * 이쪽은 *한 일*(이 프리셋을 불러왔음)이라, 같은 강도로 칠하면 무엇을 눌러야
 * 하는지 헷갈린다. 그래서 여기는 윤곽선만 물들인다.
 */
QString season_qss(bool active)
{
    const QString common =
        QString("QPushButton { border-radius:2px; padding:4px 14px; }");

    if (active)
        return common
             + QString("QPushButton { background:%1; border:1px solid %2;"
                       "              color:%3; font-weight:700; }")
                   .arg(mix(Theme::panel, Theme::chromeSelText, 0.16).name(),
                        Theme::chromeSelText.name(),
                        Theme::chromeSelText.lighter(115).name());

    return common
         + QString("QPushButton { background:transparent; border:1px solid %1;"
                   "              color:%2; font-weight:400; }"
                   "QPushButton:hover { border-color:%3; color:%4; }")
               .arg(Theme::border2.name(), Theme::textMuted.name(),
                    Theme::textFaint.name(), Theme::textHi.name());
}

} // namespace

ZoneSettingsPage::ZoneSettingsPage(QWidget *parent) : QWidget(parent)
{
    build_ui();

    MqttLink::instance()->subscribe(
        ZONES_TOPIC,
        [this](const QByteArray &p) { on_zones(p); },
        QOS);

    MqttLink::instance()->subscribe(
        FIRE_TOPIC,
        [this](const QByteArray &p) { on_fire_threshold(p); },
        QOS);

    // 권한이 바뀌면 쓰기 버튼을 다시 계산한다 (판정은 refresh_write_enable 한 곳)
    connect(Auth::instance(), &Auth::state_changed,
            this, [this] { refresh_write_enable(); });
    connect(Auth::instance(), &Auth::verification_changed,
            this, [this] { refresh_write_enable(); });
    refresh_write_enable();

    // set_zone·set_fire_threshold 의 응답 짝맞춤·타임아웃은
    // MqttLink::request()가 맡는다

    // 상태에 따라 색이 바뀌는 라벨들(합계·상태 문구·[적용] 버튼·칸 표시)은
    // 반복 호출로 칠해지므로 restyle을 걸 수 없다. 대신 테마가 바뀌었을 때
    // **다시 칠하기만** 하도록 한 번 등록한다.
    Theme::on_theme_changed(this, [this] {
        update_weight_sum();
        set_status(m_status->text(), m_status_err);
        set_fire_status(m_fire_status->text(), m_fire_status_err);
        // [적용]·[기본값 불러오기]는 dirty 여부로 색이 갈린다 — 지금 상태
        // 그대로 다시 칠한다. 구역 표의 [적용]들은 restyle로 묶여 있어(§build_ui)
        // 자동으로 따라온다.
        m_fire_apply->setStyleSheet(apply_qss(m_fire_editing));
        m_fire_reset->setStyleSheet(apply_qss(false));
        refresh_season_marks();   // 비어 있으면(버튼 미생성) no-op
    });

    set_status("waiting for zone settings...");
    // 캘리브레이션 상태 표시는 SiteSettingsCard 로 옮겼다 (08-12) — 구독도
    // 그쪽에 있다. 여기 남아 있던 connect/호출은 함께 지웠다.
}

void ZoneSettingsPage::build_ui()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(26, 24, 26, 24);   // 4a §Geometry
    outer->setSpacing(20);

    // ---- 헤더 ----
    auto *head = new QHBoxLayout();
    head->setSpacing(12);

    auto *title = new QLabel("Settings", this);
    // 08-19 보드: 화면 제목 줄은 한 줄 30px, 13/700(자간 1px)
    title->setFont(Theme::ui_font(13, 700, 0.08));
    Theme::restyle(title, [] {
        return QString("color:%1;").arg(Theme::textHi.name());
    });
    head->addWidget(title);

    m_sub = new QLabel(this);
    m_sub->setFont(Theme::mono_font(10));
    Theme::restyle(m_sub, [] {
        return QString("color:%1;").arg(Theme::textFaint.name());
    });
    m_sub->setText(TAB_SUB_GENERAL);
    head->addWidget(m_sub);
    head->addStretch(1);
    outer->addLayout(head);
    // m_status 는 구역 카드 머리글로 옮겼다 (08-19 보드 — 상태는 해당
    // 카드 안에서 말한다)


    // 이 아래는 General 탭의 내용이다. 변수 이름을 root 그대로 두는 이유는
    // diff 다 — 카드들을 다시 들여쓰면 실제 변경(탭 분리)이 200줄짜리 이동에
    // 묻힌다. 부모는 addWidget 이 다시 잡아준다.
    auto *general = new QWidget(this);
    auto *root = new QVBoxLayout(general);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(20);

    // 08-19 저녁: 보드 "Settings" 배치 확정 — [좌: 구역 표 + 화재 임계 |
    // 우 560px: 현장 설정 + 계정]. 화재 패널도 좌측 열이다(보드와 동일,
    // 계정 카드의 왼쪽) — 1920 기준 좌측 열 ~1300px 에 세 묶음+파이+계절이
    // 들어간다. 우측 열이 다시 잘리지 않도록 SiteSettingsCard·계정 카드
    // 내부를 좁은 열용 줄바꿈으로 고쳤다.
    // 화면 테마 카드는 삭제(다크 고정 — theme.cpp), General/Accounts
    // 서브탭도 해체 — 보드처럼 한 화면이다.
    auto *top_cols = new QWidget(general);
    auto *top_lay = new QHBoxLayout(top_cols);
    top_lay->setContentsMargins(0, 0, 0, 0);
    top_lay->setSpacing(20);

    auto *left_host = new QWidget(top_cols);
    auto *left_col = new QVBoxLayout(left_host);
    left_col->setContentsMargins(0, 0, 0, 0);
    left_col->setSpacing(20);
    top_lay->addWidget(left_host, 1);

    auto *side_host = new QWidget(top_cols);
    side_host->setFixedWidth(560);
    auto *side_col = new QVBoxLayout(side_host);
    side_col->setContentsMargins(0, 0, 0, 0);
    side_col->setSpacing(20);
    side_col->addWidget(new SiteSettingsCard(this));
    side_col->addWidget(new AccountSettingsCard(this));
    side_col->addStretch(1);
    top_lay->addWidget(side_host);

    root->addWidget(top_cols);

    // ---- 구역 표 (좌측 열) ----
    // 08-19 보드 형식: 제목은 패널 바깥 라벨이 아니라 카드 머리글(.chead)
    // 이다 — 12/600 textHi 제목 + 우측 mono 힌트, 아래 구분선, 그 밑에 표.
    m_table = new QWidget(this);
    m_table->setObjectName("Panel");
    auto *zcol = new QVBoxLayout(m_table);
    zcol->setContentsMargins(0, 0, 0, 0);
    zcol->setSpacing(0);

    // 머리글은 PanelChrome::header — 카드 머리글 폰트·높이가 앱 전체와
    // 한 벌이 되게 한다(08-19 "폰트 통일" 피드백). 우측 힌트·상태는
    // trailing 위젯으로 넣는다.
    auto *ztrail = new QWidget(m_table);
    auto *ztl = new QHBoxLayout(ztrail);
    ztl->setContentsMargins(0, 0, 0, 0);
    ztl->setSpacing(10);
    auto *zone_hint = new QLabel(
        QString::fromUtf8("writes go via RPi B · confirmed by re-publish ≤1 s"),
        ztrail);
    zone_hint->setFont(Theme::mono_font(10));
    Theme::restyle(zone_hint, [] {
        return QString("color:%1;").arg(Theme::textMuted.name());
    });
    ztl->addWidget(zone_hint);

    m_status = new QLabel(ztrail);
    m_status->setFont(Theme::mono_font(10));
    ztl->addWidget(m_status);

    zcol->addWidget(PanelChrome::header(
        QString::fromUtf8("Zone Capacity & Congestion Thresholds"),
        QString(), m_table, ztrail));

    auto *zbody = new QWidget(m_table);
    auto *grid = new QGridLayout(zbody);
    grid->setContentsMargins(16, 12, 16, 14);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(10);

    const char *headers[] = { "Channel", "zone_id", "Zone name",
                              "Capacity", "Warn (%)", "Critical (%)", "" };
    for (int c = 0; c < 7; ++c) {
        auto *h = new QLabel(QString::fromUtf8(headers[c]), m_table);
        h->setFont(Theme::ui_font(10.5, 700, 0.12));
        Theme::restyle(h, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        grid->addWidget(h, 0, c);
    }

    // 채널 수는 zones 수신 전까지 모르므로 4줄을 미리 만들어 두고
    // 수신 시 채운다. 없는 채널은 숨긴다.
    for (int i = 0; i < 4; ++i) {
        Row r;
        const int row = i + 1;

        r.ch_label = new QLabel(QString("CH%1").arg(i + 1), m_table);
        r.ch_label->setFont(Theme::mono_font(11));
        grid->addWidget(r.ch_label, row, 0);

        r.zone_label = new QLabel("-", m_table);
        r.zone_label->setFont(Theme::mono_font(10));
        Theme::restyle(r.zone_label, [] {
            return QString("color:%1;").arg(Theme::textMuted.name());
        });
        grid->addWidget(r.zone_label, row, 1);

        r.name = new QLineEdit(m_table);
        r.name->setFont(Theme::mono_font(11));
        r.name->setMinimumWidth(170);
        r.name->setMaxLength(NAME_MAX_CHARS);
        r.name->setPlaceholderText("Zone name");
        grid->addWidget(r.name, row, 2);

        r.capacity = new QSpinBox(m_table);
        r.capacity->setRange(CAP_MIN, CAP_MAX);
        r.capacity->setFont(Theme::mono_font(11));
        r.capacity->setMinimumWidth(90);
        grid->addWidget(r.capacity, row, 3);

        r.warn = new QDoubleSpinBox(m_table);
        r.warn->setRange(1.0, 99.0);
        r.warn->setDecimals(0);
        r.warn->setSuffix(" %");
        r.warn->setFont(Theme::mono_font(11));
        r.warn->setMinimumWidth(90);
        grid->addWidget(r.warn, row, 4);

        r.critical = new QDoubleSpinBox(m_table);
        r.critical->setRange(1.0, 100.0);
        r.critical->setDecimals(0);
        r.critical->setSuffix(" %");
        r.critical->setFont(Theme::mono_font(11));
        r.critical->setMinimumWidth(90);
        grid->addWidget(r.critical, row, 5);

        r.apply = new QPushButton("Apply", m_table);
        r.apply->setCursor(Qt::PointingHandCursor);
        r.apply->setEnabled(false);
        QPushButton *apply_btn = r.apply;
        Theme::restyle(apply_btn, [apply_btn] {
            return apply_qss(apply_btn->property("dirty").toBool());
        });
        grid->addWidget(r.apply, row, 6);

        m_rows.append(r);
    }
    // 구역 이름 열이 남는 폭을 다 먹는다 — 보드처럼 표가 카드 폭을 채운다
    grid->setColumnStretch(2, 1);
    zcol->addWidget(zbody);

    // 위젯을 만든 뒤에 연결한다 — 람다가 인덱스를 캡처해야 하므로
    for (int i = 0; i < m_rows.size(); ++i) {
        connect(m_rows[i].apply, &QPushButton::clicked, this,
                [this, i] { apply_row(i); });

        // 사용자가 편집 중이면 수신값으로 덮어쓰지 않는다 — 입력하는 도중에
        // 30초 틱이 와서 값이 되돌아가면 쓸 수 없는 화면이 된다.
        // 동시에 [적용]이 파랗게 차올라 "아직 안 보냈다"를 알린다.
        auto mark = [this, i] { set_dirty(i, true); refresh_row_marks(i); };
        // textEdited는 사용자 입력에만 발생한다 — setText로 채울 때는 안 온다.
        // textChanged를 쓰면 수신 갱신이 스스로를 "편집 중"으로 표시해버린다.
        connect(m_rows[i].name, &QLineEdit::textEdited, this, mark);
        connect(m_rows[i].capacity, &QSpinBox::valueChanged, this, mark);
        connect(m_rows[i].warn, &QDoubleSpinBox::valueChanged, this, mark);
        connect(m_rows[i].critical, &QDoubleSpinBox::valueChanged, this, mark);
    }

    left_col->addWidget(m_table);
    // 사용법 설명 문단은 삭제(08-19 보드 — 카드 밖 잔글씨 없음). 오렌지
    // [Apply] 가 "아직 안 보냄"을 말하는 것으로 충분하다.

    // ---- 화재 판단 임계 (구역 표 아래 · 계정 카드 왼쪽 — 보드 카드) ----
    // 구역 카드와 같은 형식: 제목은 카드 머리글(.chead)에, 12/600 textHi.
    auto *fire_panel = new QWidget(this);
    fire_panel->setObjectName("Panel");
    auto *fire_card = new QVBoxLayout(fire_panel);
    fire_card->setContentsMargins(0, 0, 0, 0);
    fire_card->setSpacing(0);

    // threshold_id·수정 시각·작성자는 화면에서 뺐다(08-19 — 보드에 없는
    // 잔글씨). 갱신 코드(on_fire_threshold)가 계속 참조하므로 위젯은
    // 만들어만 두고 숨긴다.
    m_fire_meta = new QLabel("-", fire_panel);
    m_fire_meta->hide();

    // 머리글 우측(trailing): season preset 캡션 + 버튼 줄 + 상태.
    // 계절 프리셋 — 온습도·표면온도만 계절 차가 있고 나머지(가중치·사이클·
    // 가스/불꽃 ADC)는 계절이 아니라 센서 신뢰도·대응 정책의 문제라 버튼
    // 하나로 22칸 전부를 갈아끼운다(load_fire_season). 실측 전엔 5행 다
    // fire_threshold 시드와 같은 값이다(migration_season_threshold.sql 주석).
    auto *ftrail = new QWidget(fire_panel);
    auto *ftl = new QHBoxLayout(ftrail);
    ftl->setContentsMargins(0, 0, 0, 0);
    ftl->setSpacing(10);

    auto *season_cap = new QLabel("season preset", ftrail);
    season_cap->setFont(Theme::mono_font(10));
    Theme::restyle(season_cap, [] {
        return QString("color:%1;").arg(Theme::textMuted.name());
    });
    ftl->addWidget(season_cap);

    auto *season_host = new QWidget(ftrail);
    m_season_col = new QHBoxLayout(season_host);
    m_season_col->setContentsMargins(0, 0, 0, 0);
    m_season_col->setSpacing(6);

    // season_threshold 테이블이 없는 DB 도 있을 수 있어(마이그레이션 전) —
    // seasons 가 안 오는 동안은 이 안내만 보인다. rebuild_season_buttons가
    // 첫 수신 때 지운다.
    m_season_wait = new QLabel("waiting...", season_host);
    m_season_wait->setFont(Theme::mono_font(10));
    Theme::restyle(m_season_wait, [] {
        return QString("color:%1;").arg(Theme::textFaint.name());
    });
    m_season_col->addWidget(m_season_wait);
    m_season_col->addStretch(1);
    ftl->addWidget(season_host);

    m_fire_status = new QLabel(ftrail);
    m_fire_status->setFont(Theme::mono_font(10));
    ftl->addWidget(m_fire_status);

    fire_card->addWidget(PanelChrome::header(
        QStringLiteral("Fire Decision Thresholds"), QString(),
        fire_panel, ftrail));

    // 08-19 단순화(사용자 요청): 보이는 것은 파이 + 가중치 5칸 + Fire
    // threshold 하나 — 보드의 Fire Decision Thresholds 구성 그대로.
    // 퍼지 구간 10칸과 판정 세부 6칸은 화면에서 뺐다(아래 숨은 편집기 참조).
    // 세 덩이(파이·가중치·판정)는 늘어나는 빈 열(1·4·7)로 카드 폭에 고르게
    // 편다 — 왼쪽에 몰아 두면 우측 절반이 통째로 빈다.
    auto *fbody = new QWidget(fire_panel);
    auto *fgrid = new QGridLayout(fbody);
    fgrid->setContentsMargins(20, 14, 20, 8);
    fgrid->setHorizontalSpacing(18);
    fgrid->setVerticalSpacing(8);

    // 가중치 파이 — 맨 왼쪽(보드와 동일). 숫자 5개를 읽고 머릿속에서
    // 비율을 그리는 대신 "불꽃이 3분의 1"이 바로 보이게 한다. 스핀박스를
    // 돌리면 즉시 따라 그려진다(update_weight_sum).
    m_fire_pie = new WeightPie(fbody);
    fgrid->addWidget(m_fire_pie, 0, 0, int(WEIGHT_KEYS.size()) + 2, 1);

    build_fire_ui(fgrid, 2, "Weights (sum 1.00)", WEIGHT_KEYS);
    build_fire_ui(fgrid, 5, "Decision",
                  QStringList{ QStringLiteral("fire_score_threshold") });

    // 가중치 합계 — 가중치 묶음 바로 아래
    m_fire_sum = new QLabel("sum 1.00", fbody);
    m_fire_sum->setFont(Theme::mono_font(10.5, 700));
    fgrid->addWidget(m_fire_sum, int(WEIGHT_KEYS.size()) + 1, 3);

    // 화면에서 뺀 칸(퍼지 10 + 판정 세부 6)도 편집기는 만들어 둔다 —
    // 수신값이 그대로 담겨 [적용]이 22개 전부를 되보내고, 계절 프리셋·
    // 기본값 채우기(fill_fire_form)도 예전과 동일하게 동작한다. 화면에만
    // 없다. ⚠ 폼에서 지우면 안 된다: apply_fire 가 m_fire 를 통째로 JSON
    // 으로 옮기므로 빠진 키는 DB CHECK 위반이 된다.
    for (const FireField &f : FIRE_FIELDS) {
        const QString key = QString::fromLatin1(f.key);
        if (m_fire.contains(key))
            continue;
        auto *box = new QDoubleSpinBox(fbody);
        box->setRange(f.min, f.max);
        box->setDecimals(f.decimals);
        box->setSingleStep(f.step);
        if (*f.suffix)
            box->setSuffix(QString::fromUtf8(f.suffix));
        box->setKeyboardTracking(false);
        box->hide();
        m_fire[key] = box;
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, key] {
            set_fire_dirty(true);
            refresh_fire_marks();
            if (WEIGHT_KEYS.contains(key))
                update_weight_sum();
        });
    }

    // 빈 열 셋에 같은 stretch — 세 덩이가 카드 폭에 고르게 퍼진다
    fgrid->setColumnStretch(1, 1);
    fgrid->setColumnStretch(4, 1);
    fgrid->setColumnStretch(7, 1);
    fire_card->addWidget(fbody);

    // 카드 바닥 줄 — 버튼만 우하단에 (설명 문단은 삭제, 구역 카드와 동일).
    // "합이 1.00이어야 한다"는 sum 라벨이 빨개지는 것으로 이미 말한다.
    auto *fire_foot = new QHBoxLayout();
    fire_foot->setContentsMargins(16, 0, 16, 12);
    fire_foot->setSpacing(10);
    fire_foot->addStretch(1);

    m_fire_reset = new QPushButton("Load defaults", fire_panel);
    m_fire_reset->setCursor(Qt::PointingHandCursor);
    m_fire_reset->setEnabled(false);   // 시드 행을 받기 전에는 채울 값이 없다
    m_fire_reset->setStyleSheet(apply_qss(false));   // 항상 테두리만 — 주 동작이 아니다
    m_fire_reset->setToolTip(QString::fromUtf8(
        "Fills the form with the first fire_threshold row (factory defaults). "
        "You still have to press [Apply fire thresholds] to save."));
    connect(m_fire_reset, &QPushButton::clicked, this,
            [this] { load_fire_default(); });
    fire_foot->addWidget(m_fire_reset);

    m_fire_apply = new QPushButton("Apply fire thresholds", fire_panel);
    m_fire_apply->setCursor(Qt::PointingHandCursor);
    m_fire_apply->setEnabled(false);   // 첫 수신 전에는 보낼 값이 없다
    m_fire_apply->setStyleSheet(apply_qss(false));
    connect(m_fire_apply, &QPushButton::clicked, this, [this] { apply_fire(); });
    fire_foot->addWidget(m_fire_apply);
    fire_card->addLayout(fire_foot);

    left_col->addWidget(fire_panel);
    // 이벤트 녹화는 좌측 열, 화재 임계 아래 (08-20 사용자 배치 지시 —
    // 우측 열은 계정 목록이 길어 카드가 스크롤 밖으로 밀렸다)
    left_col->addWidget(new StorageSettingsCard(this));
    left_col->addStretch(1);

    root->addStretch(1);

    // ---- 탭에 꽂는다 ----
    outer->addWidget(general, 1);
}

void ZoneSettingsPage::build_fire_ui(QGridLayout *grid, int col,
                                     const char *group, const QStringList &keys)
{
    auto *head = new QLabel(QString::fromUtf8(group), grid->parentWidget());
    head->setFont(Theme::ui_font(10.5, 700, 0.12));
    // 이 함수는 그룹마다 **새 라벨을 만들며** 여러 번 불린다 — 그런 자리는
    // restyle이 안전하다(연결이 위젯과 함께 죽는다).
    Theme::restyle(head, [] {
        return QString("color:%1;").arg(Theme::textMuted.name());
    });
    grid->addWidget(head, 0, col, 1, 2);

    int row = 1;
    for (const QString &key : keys) {
        const FireField *f = find_field(key);
        if (!f)
            continue;   // 표에 없는 키 — 정의 표를 고치다 빠뜨린 경우

        auto *label = new QLabel(QString::fromUtf8(f->label),
                                 grid->parentWidget());
        label->setFont(Theme::mono_font(10.5));
        Theme::restyle(label, [] {
            return QString("color:%1;").arg(Theme::textDim.name());
        });
        grid->addWidget(label, row, col);

        auto *box = new QDoubleSpinBox(grid->parentWidget());
        box->setRange(f->min, f->max);
        box->setDecimals(f->decimals);
        box->setSingleStep(f->step);
        if (*f->suffix)
            box->setSuffix(QString::fromUtf8(f->suffix));
        box->setFont(Theme::mono_font(10.5));
        box->setMinimumWidth(104);
        box->setKeyboardTracking(false);   // 타이핑 도중 값이 튀지 않게
        grid->addWidget(box, row, col + 1);

        m_fire[key] = box;

        // 사용자가 만지는 동안에는 30초 틱 수신값으로 덮지 않는다 (구역 표와 동일)
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, key] {
            set_fire_dirty(true);
            refresh_fire_marks();   // 어느 칸이 DB와 달라졌는지 즉시 반영
            if (WEIGHT_KEYS.contains(key))
                update_weight_sum();
        });

        ++row;
    }
}

void ZoneSettingsPage::update_weight_sum()
{
    double sum = 0.0;
    for (const QString &key : WEIGHT_KEYS)
        if (auto *box = m_fire.value(key))
            sum += box->value();

    // DB CHECK 와 같은 허용 오차(0.001)를 쓴다 — 화면은 통과인데 저장은
    // 실패하는 상태가 생기면 안 된다.
    const bool ok = std::abs(sum - 1.0) < 0.001;
    m_fire_sum->setText(QString("sum %1").arg(sum, 0, 'f', 2));
    // ⚠ 이 함수는 스핀박스 valueChanged마다 불린다 — restyle을 쓰면 연결이
    //   무한히 쌓인다. 색은 평범하게 칠하고, 테마 전환 때 다시 부르는 것은
    //   생성자의 on_theme_changed가 맡는다.
    m_fire_sum->setStyleSheet(
        QString("color:%1;").arg(ok ? Theme::textMuted.name()
                                    : Theme::alarm.name()));

    // 파이도 같은 값으로 다시 그린다 — 합이 1.00이 아니어도 그대로 비율로
    // 보여준다(정규화하지 않는다). 합이 틀어진 걸 숨기면 안 되고, 그건
    // 위의 합계 라벨이 빨갛게 경고하는 몫이다.
    //
    // null 검사: 이 함수는 스핀박스 valueChanged로도 불리는데, 그 connect가
    // 파이 생성보다 앞에 있다(build_fire_ui가 먼저 돈다). 지금은 connect가
    // setRange 뒤라 구축 중에 신호가 안 나가지만, 그 순서에 기대지 않는다.
    if (!m_fire_pie)
        return;

    QVector<WeightPie::Slice> slices;
    for (int i = 0; i < WEIGHT_KEYS.size(); ++i) {
        auto *box = m_fire.value(WEIGHT_KEYS[i]);
        if (!box)
            continue;
        WeightPie::Slice s;
        if (const FireField *f = find_field(WEIGHT_KEYS[i]))
            s.label = QString::fromUtf8(f->label);
        s.weight = box->value();
        s.color = WeightPie::slice_color(i);
        slices.append(s);
    }
    m_fire_pie->set_slices(slices);
}

void ZoneSettingsPage::set_fire_dirty(bool dirty)
{
    if (m_fire_editing == dirty)
        return;
    m_fire_editing = dirty;
    m_fire_apply->setStyleSheet(apply_qss(dirty));
}

void ZoneSettingsPage::mark_changed(QWidget *w, bool changed)
{
    if (!w)
        return;

    const QColor fg = changed ? Theme::chromeSelText.lighter(115) : Theme::textHi;
    const QColor bg = changed ? mix(Theme::elevated, Theme::chromeSelText, 0.16)
                              : Theme::elevated;

    // ⚠ QLineEdit 은 **인라인 스타일시트**로 칠한다. 08-19 부터 전역 QSS 에
    //   QLineEdit 규칙이 생겼고(OS 팔레트가 흰 상자를 그리던 문제 — theme.cpp
    //   참조) QSS 는 QPalette 를 이긴다. 팔레트로 칠하면 이 표시가 조용히
    //   죽는다. 인라인은 전역보다 우선하므로 여기서만 덮어쓴다.
    if (auto *edit = qobject_cast<QLineEdit *>(w)) {
        edit->setStyleSheet(
            QString("QLineEdit { background:%1; color:%2; border:1px solid %3;"
                    "            border-radius:3px; padding:4px 8px; }")
                .arg(bg.name(), fg.name(),
                     (changed ? Theme::chromeSelText : Theme::border2).name()));
        return;
    }

    // 스핀박스는 팔레트로 칠한다 — 스타일시트를 걸면 위/아래 화살표까지
    // QSS 렌더링으로 넘어가 안 바뀐 칸과 모양이 달라진다. "색만 다르고 모양은
    // 같아야" 무엇이 바뀌었는지가 읽힌다.
    QPalette p = w->palette();
    p.setColor(QPalette::Text, fg);
    p.setColor(QPalette::Base, bg);
    w->setPalette(p);

    // 스핀박스는 이것만으로는 안 바뀐다. 이 앱은 스타일을 지정하지 않아
    // Windows 기본 스타일로 그려지는데, 그 스타일은 스핀박스 본체를 네이티브
    // 테마로 그려 팔레트를 무시한다 (구역 이름은 QLineEdit 이라 먹혔던 것).
    //
    // 다만 스핀박스 *안의* QLineEdit 은 글자를 palette().text() 로 직접
    // 칠하므로, 그 자식에 걸면 스타일과 무관하게 숫자에 색이 들어간다.
    // 배경(Base)은 자동으로 안 칠해져서 autoFillBackground 를 함께 켠다.
    // QLineEdit 자신에게는 자식이 없어 이 분기가 그냥 지나간다.
    if (auto *edit = w->findChild<QLineEdit *>()) {
        edit->setPalette(p);
        edit->setAutoFillBackground(changed);
    }
}

void ZoneSettingsPage::refresh_row_marks(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    Row &r = m_rows[index];

    // 첫 수신 전에는 비교 대상이 없다 — 전부 "안 바뀜"으로 둔다.
    // 여기서 비교를 하면 기본값 0 과 달라 온 칸이 파랗게 뜬다.
    const bool cmp = r.base_valid;

    mark_changed(r.name,     cmp && r.name->text() != r.base_name);
    mark_changed(r.capacity, cmp && r.capacity->value() != r.base_capacity);
    // 소수 비교 — 표시 자릿수가 0 이라 0.5 미만 차이는 화면에 안 보인다.
    // 눈에 안 보이는 차이로 색이 켜지면 "왜 파란지" 알 수 없다.
    mark_changed(r.warn,     cmp && std::abs(r.warn->value() - r.base_warn) >= 0.5);
    mark_changed(r.critical, cmp && std::abs(r.critical->value() - r.base_critical) >= 0.5);
}

void ZoneSettingsPage::refresh_fire_marks()
{
    if (m_fire_base.isEmpty())
        return;   // 아직 DB 값을 못 받았다 — 비교 기준이 없다

    for (auto it = m_fire.cbegin(); it != m_fire.cend(); ++it) {
        const auto base = m_fire_base.constFind(it.key());
        if (base == m_fire_base.cend())
            continue;   // 옛 발행에 없던 컬럼 — 비교하지 않는다

        // 표시 자릿수의 절반을 문턱으로 쓴다 (같은 이유는 refresh_row_marks 참조)
        //
        // ⚠ std::pow 를 쓰지 않는다 (08-10): MinGW 링크에서 `undefined reference
        // to pow` 로 죽는다. pow 는 libmingwex 에만 있고 objects.a 뒤로 오지
        // 않아 해석되지 않는다 — m/mingwex 를 명시 링크해도 안 풀렸다.
        // decimals()는 작은 음이 아닌 정수라 정수 거듭제곱이 값도 같고 더 싸다.
        double scale = 1.0;
        for (int d = it.value()->decimals(); d > 0; --d)
            scale *= 10.0;
        const double eps = 0.5 / scale;
        mark_changed(it.value(), std::abs(it.value()->value() - *base) >= eps);
    }
}

void ZoneSettingsPage::fill_fire_form(const QJsonObject &src)
{
    for (auto it = m_fire.cbegin(); it != m_fire.cend(); ++it) {
        const QJsonValue v = src.value(it.key());
        if (v.isDouble()) {
            QSignalBlocker block(it.value());   // 채우는 동안 dirty 를 켜지 않게
            it.value()->setValue(v.toDouble());
        }
    }
    update_weight_sum();
}

void ZoneSettingsPage::on_fire_threshold(const QByteArray &payload)
{
    QJsonParseError err{};
    const QJsonObject o = QJsonDocument::fromJson(payload, &err).object();
    if (err.error != QJsonParseError::NoError) {
        qDebug() << "[Settings] fire_threshold 파싱 실패:" << err.errorString();
        return;
    }

    // 공장 기본값(시드 행)은 활성 행과 함께 온다 — [기본값 불러오기]가 쓴다.
    // 편집 중이어도 이건 받아둔다. 폼을 건드리지 않고 보관만 하기 때문이다.
    if (o.value("default").isObject()) {
        m_fire_default = o.value("default").toObject();
        refresh_write_enable();   // 기본값이 도착했다
    }

    // 계절 버튼은 편집 중이어도 만든다 — 버튼을 새로 세우는 것뿐이고 폼은
    // 안 건드린다. m_seasons_built 가 두 번째 발행부터는 막는다.
    if (o.value("seasons").isArray())
        rebuild_season_buttons(o.value("seasons").toArray());

    const QJsonValue tv = o.value("threshold");
    if (!tv.isObject()) {
        // 활성 행이 없는 상태. 폼을 채울 값이 없으니 적용도 막는다 —
        // 빈 폼을 그대로 보내면 0 투성이 설정이 활성화된다.
        m_fire_meta->setText("no active threshold row");
        m_fire_seeded = false;   // 보낼 값이 없다
        refresh_write_enable();
        return;
    }
    const QJsonObject t = tv.toObject();

    m_fire_meta->setText(
        QString::fromUtf8("id %1 · %2 · %3")
            .arg(t.value("threshold_id").toInt())
            .arg(t.value("updated_at").toString("-"))
            .arg(t.value("updated_by").toString("-")));

    m_fire_seeded = true;
    refresh_write_enable();

    // 편집 중이면 덮지 않는다 — 입력하는 도중 30초 틱이 와서 값이 되돌아가면
    // 쓸 수 없는 화면이 된다 (구역 표와 같은 규칙).
    if (m_fire_editing)
        return;

    fill_fire_form(t);   // 수신 갱신 — dirty 를 켜지 않는다

    // 이 값이 "안 바뀐 상태"의 기준이 된다. 폼을 덮은 직후에 잡아야
    // 방금 채운 값과 기준이 어긋나지 않는다.
    m_fire_base.clear();
    for (auto it = m_fire.cbegin(); it != m_fire.cend(); ++it)
        if (const QJsonValue v = t.value(it.key()); v.isDouble())
            m_fire_base[it.key()] = v.toDouble();

    // 폼이 DB 값으로 덮였으니 불러왔던 프리셋은 더 이상 화면에 없다.
    // 강조를 남기면 "지금 여름 기준" 이라는 거짓말이 된다.
    m_season_active.clear();
    refresh_season_marks();
    refresh_fire_marks();

    // "current settings received" 문구는 삭제(08-19 — 폼이 채워진 것 자체가
    // 말해준다). 지우지 않고 두면 "applied ..." 확인 문구를 1초 뒤 재발행이
    // 덮어버리는 부작용도 있었다.
}

void ZoneSettingsPage::load_fire_default()
{
    if (m_fire_default.isEmpty()) {
        set_fire_status("the defaults have not arrived yet", true);
        return;
    }

    fill_fire_form(m_fire_default);
    refresh_fire_marks();

    // 기본값은 계절이 아니다 — 계절 강조를 끈다
    m_season_active.clear();
    refresh_season_marks();

    // 저장은 안 했다 — [적용]을 파랗게 켜서 "아직 DB에 안 보냈다"를 남긴다.
    // 동시에 dirty 라 다음 수신 틱이 이 값을 덮지 않는다.
    set_fire_dirty(true);
    set_fire_status(QString::fromUtf8(
        "Defaults loaded (id %1) - press [Apply fire thresholds] to save")
            .arg(m_fire_default.value("threshold_id").toInt()));
}

bool ZoneSettingsPage::confirm_overwrite(const QString &what)
{
    if (!m_fire_editing)
        return true;   // 저장 안 한 변경이 없으면 물어볼 게 없다

    return QMessageBox::question(
               this, QStringLiteral("Unsaved changes"),
               QString::fromUtf8(
                   "%1 — 지금 화면에 아직 [화재 임계 적용]을 안 누른 변경이 "
                   "있습니다. 덮어쓸까요?").arg(what),
               QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        == QMessageBox::Yes;
}

void ZoneSettingsPage::rebuild_season_buttons(const QJsonArray &seasons)
{
    // 30초마다 다시 오는 발행 payload 마다 버튼을 지웠다 만들면, 누르는
    // 순간(클릭 이벤트가 처리되기 전에 다음 틱이 와서) 버튼이 사라지는 일이
    // 생긴다 — 한 번 세운 뒤로는 다시 짓지 않는다.
    if (m_seasons_built || seasons.isEmpty())
        return;

    if (m_season_wait) {
        m_season_col->removeWidget(m_season_wait);
        m_season_wait->deleteLater();
        m_season_wait = nullptr;
    }

    for (const QJsonValue &v : seasons) {
        const QJsonObject s = v.toObject();
        const QString key = s.value("season_key").toString();
        const QString name = s.value("season_name").toString();
        if (key.isEmpty() || name.isEmpty())
            continue;   // 잘못된 행 — 버튼 없이 조용히 건너뛴다

        auto *btn = new QPushButton(name, m_season_col->parentWidget());
        btn->setCursor(Qt::PointingHandCursor);
        btn->setMinimumHeight(28);
        btn->setFont(Theme::mono_font(10.5));
        btn->setStyleSheet(season_qss(false));

        connect(btn, &QPushButton::clicked, this, [this, s, name, key] {
            if (!confirm_overwrite(
                    QString("Load the %1 preset").arg(name)))
                return;
            load_fire_season(s, name, key);
        });

        m_season_btns[key] = btn;
        m_season_col->insertWidget(m_season_col->count() - 1, btn);   // addStretch 앞에
    }
    m_seasons_built = true;
}

void ZoneSettingsPage::load_fire_season(const QJsonObject &season,
                                        const QString &name, const QString &key)
{
    fill_fire_form(season);

    // 기준값(m_fire_base)은 건드리지 않는다 — 그래야 프리셋과 DB 가 다른 칸이
    // 오렌지로 뜬다. 계절 프리셋의 요점이 "무엇이 달라지는가"라 이 표시가 곧
    // 기능 설명이 된다 (지금 보이는 칸에서는 주로 가중치·Fire threshold).
    refresh_fire_marks();

    m_season_active = key;
    refresh_season_marks();

    // 프리셋도 [기본값 불러오기]와 같은 규칙 — 채우기만 하고 저장은 안 한다.
    // 화재 판정 기준이라 실수로 덮으면 튜닝한 값이 되돌릴 새 없이 날아간다.
    set_fire_dirty(true);
    set_fire_status(
        QString("%1 preset loaded - press [Apply fire thresholds] to save")
            .arg(name));
}

void ZoneSettingsPage::refresh_season_marks()
{
    for (auto it = m_season_btns.cbegin(); it != m_season_btns.cend(); ++it)
        it.value()->setStyleSheet(season_qss(it.key() == m_season_active));
}

void ZoneSettingsPage::refresh_write_enable()
{
    // 쓰기 버튼의 활성 조건을 **한 곳**에서 계산한다. 예전엔 네 군데에서
    // 각자 setEnabled 를 불렀는데, 거기에 권한을 더하면 나중에 부른 쪽이
    // 이겨서 잠근 버튼이 도로 켜진다.
    //   조건 = (보낼 값이 있는가) AND (권한이 있는가)
    const bool may_zone = Auth::can(Auth::Action::ZoneSettings);
    const bool may_fire = Auth::can(Auth::Action::FireThreshold);
    const QString zone_why = Auth::deny_reason(Auth::Action::ZoneSettings);
    const QString fire_why = Auth::deny_reason(Auth::Action::FireThreshold);

    for (Row &r : m_rows) {
        if (!r.apply)
            continue;
        r.apply->setEnabled(r.zone_id > 0 && may_zone);
        r.apply->setToolTip(may_zone ? QString() : zone_why);
    }
    if (m_fire_apply) {
        m_fire_apply->setEnabled(m_fire_seeded && may_fire);
        m_fire_apply->setToolTip(may_fire ? QString() : fire_why);
    }
    if (m_fire_reset) {
        m_fire_reset->setEnabled(!m_fire_default.isEmpty() && may_fire);
        if (!may_fire)
            m_fire_reset->setToolTip(fire_why);
    }

    // Accounts 서브탭은 08-19 보드 전환으로 해체됐다 — 계정 카드는 항상
    // 보이고, 잠금·문구는 카드 내부의 Auth::bind()·서버 거절이 맡는다.
}

void ZoneSettingsPage::apply_fire()
{
    // 권한 백스톱 (§5) — 버튼 잠금은 표면이다.
    if (!Auth::can(Auth::Action::FireThreshold)) {
        set_fire_status(Auth::deny_reason(Auth::Action::FireThreshold), true);
        return;
    }
    if (!ReauthDialog::ensure_fresh(this))
        return;

    if (!m_fire_seeded) {
        set_fire_status("the current settings have not arrived yet", true);
        return;
    }

    // ── 폴러·DB와 같은 검증을 먼저 — 왕복 없이 즉시 알려준다 ──
    double sum = 0.0;
    for (const QString &key : WEIGHT_KEYS)
        sum += m_fire.value(key)->value();
    if (std::abs(sum - 1.0) >= 0.001) {
        set_fire_status(
            QString("The weights must sum to 1.00 (currently %1)")
                .arg(sum, 0, 'f', 2), true);
        return;
    }

    struct Pair { const char *lo; const char *hi; const char *msg; };
    // 앞이 뒤보다 작아야 하는 쌍. 스파크·습도는 방향이 반대라 뒤집어 넣는다
    // (내림차순 퍼지화 — fire_schema.sql 참조).
    static const Pair ORDERED[] = {
        { "gas_raw_min",     "gas_raw_max",     "Gas safe < Gas danger" },
        { "spark_raw_danger","spark_raw_safe",  "Flame danger < Flame safe" },
        { "temp_min_c",      "temp_max_c",      "Temp safe < Temp danger" },
        { "irtemp_min_c",    "irtemp_max_c",    "Surface safe < Surface danger" },
        { "humi_danger_percent", "humi_safe_percent",
          "Humidity danger < Humidity safe" },
    };
    for (const Pair &p : ORDERED) {
        if (m_fire.value(p.lo)->value() >= m_fire.value(p.hi)->value()) {
            set_fire_status(QString("%1 is required").arg(p.msg), true);
            return;
        }
    }

    QJsonObject cmd;
    cmd["cmd"] = "set_fire_threshold";
    for (auto it = m_fire.cbegin(); it != m_fire.cend(); ++it) {
        // INT 컬럼에 소수점이 붙으면 Postgres 캐스트에서 죽는다
        if (is_int_field(it.key()))
            cmd[it.key()] = (int)it.value()->value();
        else
            cmd[it.key()] = it.value()->value();
    }
    // 누가 바꿨는지 남긴다 — 이력 테이블의 존재 이유가 절반은 이것이다
    cmd["updated_by"] = QString("vms %1").arg(MqttLink::instance()->client_id());

    Auth::attach_token(cmd);   // §6 — 서버가 role='admin' 을 다시 본다

    const QString id = MqttLink::instance()->request(
        SETFIRE_TOPIC, cmd,
        [this](const QJsonObject &) {
            // 성공하면 RPi B가 fire_threshold 를 즉시 재발행하므로
            // on_fire_threshold 가 곧 폼을 갱신한다.
            set_fire_status("applied - engine reload signal published");
        },
        [this](const QString &reason) {
            set_fire_dirty(true);   // 아직 DB에 없다 — 다시 파랗게
            set_fire_status(QString("apply failed - %1").arg(reason), true);
        },
        MqttLink::DEFAULT_TIMEOUT_MS,
        // 인증 이유의 거절(`reason`)만 여기로 온다 — 값 오류(`error`)는 위
        // on_error 가 그대로 받는다. 서버가 층을 필드 이름으로 알려준다.
        [this](const QJsonObject &reply) {
            set_fire_dirty(true);
            set_fire_status(Auth::note_write_reject(reply), true);
        });

    if (id.isEmpty())
        return;   // 미연결 — on_error 가 이미 사유를 표시했다

    set_fire_dirty(false);
    set_fire_status("apply requested...");
}

void ZoneSettingsPage::set_fire_status(const QString &text, bool error)
{
    // 반복 호출되는 자리 — restyle 금지(§부록 D). 마지막 error 상태를 남겨
    // 두는 이유는 테마가 바뀌었을 때 **같은 의미로** 다시 칠하기 위해서다.
    // 색을 값으로 저장하면 옛 팔레트의 색이 굳는다.
    m_fire_status_err = error;
    m_fire_status->setStyleSheet(
        QString("color:%1;").arg(error ? Theme::alarm.name()
                                       : Theme::textDim.name()));
    m_fire_status->setText(text);
}

void ZoneSettingsPage::on_zones(const QByteArray &payload)
{
    QJsonParseError err{};
    const QJsonObject o = QJsonDocument::fromJson(payload, &err).object();
    if (err.error != QJsonParseError::NoError) {
        qDebug() << "[ZoneSettings] payload 파싱 실패:" << err.errorString();
        return;
    }

    const QJsonArray arr = o.value("zones").toArray();
    if (arr.isEmpty()) {
        set_status("no zones", true);
        return;
    }

    int filled = 0;
    for (const QJsonValue &v : arr) {
        const QJsonObject z = v.toObject();
        const int ch = z.value("channel").toInt(-1);
        if (ch < 0 || ch >= m_rows.size())
            continue;

        Row &r = m_rows[ch];
        r.zone_id = z.value("zone_id").toInt(-1);
        r.channel = ch;
        r.zone_label->setText(r.zone_id > 0 ? QString::number(r.zone_id) : "-");
        // 활성 조건은 refresh_write_enable() 한 곳에서만 계산한다

        // 편집 중이면 건드리지 않는다 (위 mark 람다 참조)
        if (!r.editing) {
            const QSignalBlocker b1(r.capacity), b2(r.warn), b3(r.critical);
            // zone_name이 없는 옛 폴러와도 동작해야 한다 — 필드가 없으면
            // 칸을 비워 "아직 모른다"로 두고, 빈 문자열로 덮어쓰지 않는다.
            if (const QJsonValue nv = z.value("zone_name"); nv.isString())
                r.name->setText(nv.toString());
            if (const QJsonValue cv = z.value("capacity_limit"); cv.isDouble())
                r.capacity->setValue(cv.toInt());
            if (const QJsonValue wv = z.value("warn_ratio"); wv.isDouble())
                r.warn->setValue(wv.toDouble() * 100.0);
            if (const QJsonValue rv = z.value("critical_ratio"); rv.isDouble())
                r.critical->setValue(rv.toDouble() * 100.0);

            // 덮은 직후의 값이 곧 "DB 값"이다. 편집 중(else)에는 갱신하지
            // 않는다 — 그러면 사용자가 고친 값이 기준이 되어 표시가 꺼진다.
            r.base_name     = r.name->text();
            r.base_capacity = r.capacity->value();
            r.base_warn     = r.warn->value();
            r.base_critical = r.critical->value();
            r.base_valid    = true;
        }
        refresh_row_marks(ch);
        ++filled;
    }

    // 발행에 없는 채널은 표에서 감춘다 — 존이 없는 채널을 편집하게 두면
    // "zone_id 없음" 에러만 보게 된다
    for (Row &r : m_rows) {
        const bool has = r.zone_id > 0;
        r.ch_label->setVisible(has);
        r.zone_label->setVisible(has);
        r.name->setVisible(has);
        r.capacity->setVisible(has);
        r.warn->setVisible(has);
        r.critical->setVisible(has);
        r.apply->setVisible(has);
    }

    // "N zones received"는 삭제(08-19 — 표가 채워진 것 자체가 말해준다).
    // 시작 시의 "waiting..."만 걷어낸다 — 다른 문구(적용 결과·오류)를
    // 1초 뒤 재발행이 지워버리면 안 된다.
    Q_UNUSED(filled);
    if (m_status && m_status->text().startsWith("waiting"))
        set_status(QString());
}

void ZoneSettingsPage::set_dirty(int index, bool dirty)
{
    if (index < 0 || index >= m_rows.size())
        return;
    Row &r = m_rows[index];
    if (r.editing == dirty)
        return;
    r.editing = dirty;
    // 테마가 바뀌어도 dirty 여부를 잃지 않도록 위젯에 남긴다 (build_ui의 restyle이 읽음)
    r.apply->setProperty("dirty", dirty);
    r.apply->setStyleSheet(apply_qss(dirty));
}

void ZoneSettingsPage::apply_row(int index)
{
    if (!Auth::can(Auth::Action::ZoneSettings)) {
        set_status(Auth::deny_reason(Auth::Action::ZoneSettings), true);
        return;
    }
    // 자동 로그인으로 들어왔거나 마지막 비밀번호 입력이 오래됐으면 한 번 묻는다.
    // 취소하면 아무것도 보내지 않는다 — 사유는 대화상자가 이미 말했다.
    if (!ReauthDialog::ensure_fresh(this))
        return;
    if (index < 0 || index >= m_rows.size())
        return;
    Row &r = m_rows[index];
    if (r.zone_id <= 0) {
        set_status("zone_id has not arrived yet", true);
        return;
    }

    const double warn = r.warn->value() / 100.0;
    const double crit = r.critical->value() / 100.0;
    const QString name = r.name->text().trimmed();

    // 폴러도 같은 검증을 하지만, 여기서 막으면 왕복 없이 즉시 알려줄 수 있다
    if (crit <= warn) {
        set_status("Critical must be higher than Warn", true);
        return;
    }
    if (name.isEmpty()) {
        set_status("Zone name cannot be empty", true);
        return;
    }
    // 폴러의 JSON 파서(jget)가 첫 따옴표에서 잘린다 — 여기서 막지 않으면
    // 이름이 조용히 잘린 채 DB에 들어간다.
    if (name.contains('"') || name.contains('\\')) {
        set_status("Zone names cannot contain \" or \\", true);
        return;
    }

    QJsonObject cmd;
    cmd["cmd"]            = "set_zone";
    cmd["zone_id"]        = r.zone_id;
    cmd["zone_name"]      = name;
    cmd["capacity_limit"] = r.capacity->value();
    cmd["warn_ratio"]     = warn;
    cmd["critical_ratio"] = crit;

    Auth::attach_token(cmd);   // §6

    const QString id = MqttLink::instance()->request(
        SETZONE_TOPIC, cmd,
        [this](const QJsonObject &) {
            // 성공하면 폴러가 zones 를 즉시 재발행하므로 on_zones 가 곧 화면을
            // 갱신한다. 여기서는 문구만 바꾼다.
            set_status("applied - DB updated");
        },
        [this, index](const QString &reason) {
            // 실패하면 사용자가 친 값이 아직 DB에 없다 — 다시 파랗게 돌려
            // "안 보내졌다"를 남기고, 수신값이 덮어쓰지 않게 막는다.
            set_dirty(index, true);
            set_status(QString("apply failed - %1").arg(reason), true);
        },
        MqttLink::DEFAULT_TIMEOUT_MS,
        [this, index](const QJsonObject &reply) {
            set_dirty(index, true);
            set_status(Auth::note_write_reject(reply), true);
        });

    if (id.isEmpty())
        return;   // 미연결 — on_error가 이미 사유를 표시했다

    set_dirty(index, false);   // 이제부터는 수신값으로 갱신되어도 된다
    set_status(QString("CH%1 apply requested...").arg(index + 1));
}

void ZoneSettingsPage::set_status(const QString &text, bool error)
{
    m_status_err = error;   // 테마 전환 때 같은 의미로 다시 칠하려고 남긴다
    m_status->setStyleSheet(QString("color:%1;").arg((error ? Theme::alarm : Theme::textDim).name()));
    m_status->setText(text);
}
