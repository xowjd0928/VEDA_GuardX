#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QGridLayout;
class QHBoxLayout;
class QStackedWidget;
class QJsonArray;
class WeightPie;

/**
 * @brief SETTINGS 화면 — 구역 정원/임계 + 화재 판단 임계 편집
 *
 * 두 표를 한 화면에 둔다. 성격이 같기 때문이다 — 둘 다 "현장에서 값을
 * 조정하며 맞춰가는 판정 기준"이고, 둘 다 VMS가 DB에 직접 쓰지 못해
 * MQTT 명령으로 우회한다. 화면을 나누면 운영자가 임계를 맞출 때 두 곳을
 * 오가야 한다.
 *
 * (클래스·파일명은 ZoneSettings 그대로 둔다 — 이름만 바꾸면 diff가 커져
 *  실제 변경이 리뷰에서 묻힌다. 정리는 별도 커밋에서.)
 *
 * @section zones 구역 정원/임계
 *
 * VMS는 DB에 직접 붙지 않으므로(v6) 값을 고칠 때도 MQTT 명령을 보낸다.
 * RPi B 폴러가 그 명령을 받아 UPDATE 하고 곧바로 zones 를 재발행하므로,
 * 다음 폴링 틱(기본 30초)을 기다리지 않고 1초 안에 화면이 갱신된다 —
 * 그 갱신 자체가 성공 확인이 된다(별도 ack UI가 필요 없다).
 *
 *   [VMS] --cmd/set_zone--> [폴러] --UPDATE--> [DB]
 *                              └---zones 재발행---> [VMS] 화면 갱신
 *
 * 현재 값은 guardx/db/rpib/zones 를 직접 구독해 채운다. ZoneConfig 도 같은
 * 토픽을 쓰지만 그쪽은 channel 기준 캐시라 zone_id 를 갖고 있지 않고,
 * 명령에는 zone_id 가 필요하다.
 */
class ZoneSettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit ZoneSettingsPage(QWidget *parent = nullptr);

private:
    /** @brief 화면에 표시되는 한 줄 = 구역 하나 */
    struct Row {
        int zone_id = -1;
        int channel = -1;
        QLabel *ch_label = nullptr;
        QLabel *zone_label = nullptr;
        /// zones.zone_name — 구역명만 담는다 ("LOBBY EAST"). "CH1 · " 접두는
        /// 표시하는 쪽이 붙인다.
        QLineEdit *name = nullptr;
        QSpinBox *capacity = nullptr;
        QDoubleSpinBox *warn = nullptr;
        QDoubleSpinBox *critical = nullptr;
        QPushButton *apply = nullptr;
        bool editing = false;   ///< 사용자가 만지는 중이면 수신값으로 덮지 않는다

        /// 마지막으로 수신한 DB 값. "어느 칸을 내가 고쳤나"의 기준이다 —
        /// editing 플래그는 줄 단위라 네 칸 중 무엇이 바뀌었는지 모른다.
        /// 편집 중에는 갱신하지 않는다 (그러면 비교 대상이 사라진다).
        QString base_name;
        int     base_capacity = -1;
        double  base_warn = -1.0;
        double  base_critical = -1.0;
        bool    base_valid = false;   ///< 첫 수신 전에는 비교하지 않는다
    };

    void build_ui();

    /**
     * @brief 서브탭 바 (General | Accounts) — 08-12
     *
     * `QTabWidget` 을 쓰지 않는다. 이 앱의 탭 언어는 `#Segmented`/`#SegBtn` +
     * `QStackedWidget` 이고(`CameraPage::build_subtabs`), 여기만 OS 크롬으로
     * 보이면 그 자체가 버그로 신고된다. 전역 QSS 도 그 이름들만 칠한다.
     *
     * ⚠ Accounts 탭은 **관리자 전용**이다. 탭 버튼은 자체 활성 조건이 없어
     * `Auth::bind()` 가 맞는 자리지만, 보고 있던 중에 권한이 사라지는 경우
     * (로그아웃·유예 진입)까지 `bind` 가 처리해 주지는 않는다 —
     * `refresh_write_enable()` 이 General 로 되돌린다.
     */

    /**
     * @brief 화재 임계 폼 구성 — fire_threshold 22개 컬럼
     *
     * 컬럼이 많아 위젯을 하나씩 손으로 만들면 22번 반복된다. 필드 정의를
     * 표(FIRE_FIELDS)로 두고 여기서 훑어 만든다 — 범위·단위·라벨이 한 곳에
     * 모여 있어야 DB CHECK 와 어긋났을 때 찾기 쉽다.
     */
    void build_fire_ui(QGridLayout *grid, int col, const char *group,
                       const QStringList &keys);

    /** @brief fire_threshold 수신 → 폼 갱신 */
    void on_fire_threshold(const QByteArray &payload);

    /** @brief 화재 임계 적용 — cmd/set_fire_threshold 발행 (스냅샷 통째로) */
    void apply_fire();

    /**
     * @brief 쓰기 버튼(구역 [적용]·화재 [적용]/[기본값])의 활성 조건을
     *        **한 곳에서** 계산한다 = (보낼 값이 있는가) AND (권한이 있는가)
     */
    void refresh_write_enable();

    /**
     * @brief 가중치 합계 표시 갱신
     *
     * DB가 합계 1.0(±0.001)을 CHECK 로 강제한다. 다섯 칸을 각각 고치다 보면
     * 합이 틀어지는데, 보내고 나서 에러를 보는 것보다 입력하는 동안 보이는
     * 편이 낫다 — 틀리면 빨갛게, 맞으면 평소 색.
     */
    void update_weight_sum();

    /** @brief 화재 폼에 저장 안 된 변경 표시 */
    void set_fire_dirty(bool dirty);

    /**
     * @brief 폼을 공장 기본값(fire_threshold 시드 행)으로 채운다 — 저장은 안 함
     *
     * 채우기만 하고 [적용]을 눌러야 DB에 들어간다. 버튼 하나로 바로 저장되면
     * 튜닝하던 값을 실수로 날리고, 그게 화재 판정 기준이라 되돌릴 시간에
     * 경보가 안 울릴 수 있다.
     */
    void load_fire_default();

    /**
     * @brief 계절 프리셋 버튼 재구성 (seasons 수신 시)
     *
     * 버튼을 소스에 박지 않고 payload 로 만든다 — 계절을 늘리거나 이름을 바꾸는
     * 것이 DB만의 일이 되도록. ZoneConfig 가 정원을 상수로 두지 않은 것과 같은
     * 이유다: 소스에 박으면 DB를 고쳐도 VMS만 옛 목록을 보여준다.
     *
     * 순서는 서버가 sort_order 로 정렬해 보내므로 받은 순서 그대로 그린다.
     */
    void rebuild_season_buttons(const QJsonArray &seasons);

    /**
     * @brief 계절 프리셋을 폼에 채운다 — 저장은 안 함 (load_fire_default 와 동일)
     *
     * 프리셋은 읽기 전용 카탈로그다(season_threshold). 여기서 폼에 옮긴 뒤
     * 운영자가 더 손볼 수 있고, 실제 저장은 [화재 임계 적용]이 한다.
     */
    void load_fire_season(const QJsonObject &season, const QString &name,
                          const QString &key);

    /**
     * @brief 주어진 행으로 화재 폼을 채운다 (dirty·상태문구는 호출자가)
     *
     * 기본값·계절 프리셋·수신 갱신이 전부 "22개 스핀박스에 JSON 값을 옮긴다"라
     * 세 곳에 같은 루프를 두면 한 곳만 고쳐지는 일이 생긴다.
     */
    void fill_fire_form(const QJsonObject &src);

    /**
     * @brief 저장 안 한 변경이 있으면 덮어쓸지 물어본다 (없으면 그냥 true)
     *
     * 프리셋 버튼은 한 번 누르면 22개 값이 통째로 바뀐다. 튜닝하던 중이었다면
     * 되돌릴 방법이 없어서, 그때만 확인을 받는다.
     */
    bool confirm_overwrite(const QString &what);

    /**
     * @brief 입력칸 하나에 "DB 값과 다름" 표시를 켜고 끈다
     *
     * [적용] 버튼이 파래지는 것과 같은 뜻을 칸 단위로 좁힌 것이다. 버튼만으로는
     * "이 줄에 뭔가 바뀌었다"까지만 알 수 있어서, 22칸짜리 화재 폼에서는
     * 무엇을 건드렸는지 되짚을 방법이 없었다.
     *
     * 색은 [적용]과 같은 계열(브랜드 오렌지)을 쓴다 — 화면 전체에서
     * "오렌지 = 아직 DB에 안 보낸 것"이 한 가지 뜻으로 읽혀야 한다
     * (08-19 보드: 활성/변경 강조는 전부 오렌지).
     */
    void mark_changed(QWidget *w, bool changed);

    /** @brief 구역 한 줄의 네 칸을 기준값과 비교해 표시 갱신 */
    void refresh_row_marks(int index);

    /** @brief 화재 폼 22칸을 기준값과 비교해 표시 갱신 */
    void refresh_fire_marks();

    /** @brief 계절 버튼 강조 갱신 (m_season_active 만 색이 들어간다) */
    void refresh_season_marks();

    void set_fire_status(const QString &text, bool error = false);

    /**
     * @brief 화면 테마 카드 (다크 / 라이트) — 앱 전역 설정
     *
     * 구역 설정과 성격이 다르지만, 이 화면이 앱의 "설정" 자리라 여기 둔다.
     * 선택은 즉시 반영되고 레지스트리(`theme_mode`)에 저장된다.
     */

    // 계정 카드는 **AccountSettingsCard** (account_settings_card.h) 로 옮겼다
    // (08-12). SETTINGS 하위 Accounts 탭이 되면서 카드가 하나가 아니게 됐고,
    // 이 페이지가 계정 상태를 들고 있을 이유도 없어졌다. 여기는 이제
    // 탭에 넣는 한 줄뿐이다.

    // 바닥 캘리브레이션 카드는 **SiteSettingsCard** (site_settings_card.h) 로
    // 옮겼다 (08-12). 전역 저장(site_config)과 [Apply] 버튼이 붙으면서
    // 카드가 커졌고, "①어디서 가져오나"가 파일 → MQTT 로 확장됐다.
    // 이 페이지는 이제 생성 한 줄만 들고 있다.

    /**
     * @brief 저장 안 된 변경 여부 지정 — editing 플래그와 [적용] 버튼 색을 함께
     *
     * 둘은 같은 사실("이 줄에 아직 DB에 안 넘긴 값이 있다")의 두 표현이라
     * 따로 두면 어긋난다. 색이 없던 동안은 무엇을 고쳤는지 화면만 봐서는
     * 알 수 없었다.
     */
    void set_dirty(int index, bool dirty);

    /** @brief zones 수신 → 표 갱신 */
    void on_zones(const QByteArray &payload);

    /** @brief 한 줄 적용 — cmd/set_zone 발행 */
    void apply_row(int index);

    void set_status(const QString &text, bool error = false);

    QVector<Row> m_rows;

    // ── 서브탭 ──
    QLabel *m_sub = nullptr;                ///< 헤더의 부제 — 탭마다 다르다

    QLabel *m_status = nullptr;
    bool m_status_err = false;   ///< m_fire_status_err 와 같은 이유 (테마 재도색용)
    QWidget *m_table = nullptr;

    // ── 화재 임계 (fire_threshold) ──
    /// 컬럼명 -> 입력 위젯. 전송할 때 이 해시를 그대로 JSON 으로 옮긴다.
    QHash<QString, QDoubleSpinBox *> m_fire;
    QLabel *m_fire_sum = nullptr;      ///< 가중치 합계 (1.0 이어야 함)
    WeightPie *m_fire_pie = nullptr;   ///< 가중치 배분 파이 — 합계 라벨과 같은 값으로 갱신
    QLabel *m_fire_meta = nullptr;     ///< threshold_id · 마지막 수정 시각
    QLabel *m_fire_status = nullptr;
    /// 마지막 상태가 오류였나 — 테마가 바뀌면 **같은 의미로** 다시 칠해야 한다
    /// (색을 값으로 들고 있으면 옛 팔레트의 색이 그대로 굳는다)
    bool m_fire_status_err = false;
    QPushButton *m_fire_apply = nullptr;
    QPushButton *m_fire_reset = nullptr;   ///< [기본값 불러오기]
    /// fire_threshold 시드 행(threshold_id 최소) — 발행 payload 의 "default"
    QJsonObject m_fire_default;
    bool m_fire_editing = false;       ///< 편집 중이면 수신값으로 덮지 않는다
    bool m_fire_seeded = false;        ///< 첫 수신 전에는 [적용]을 막는다

    /// 마지막으로 수신한 DB 값 (컬럼명 -> 값). Row::base_* 와 같은 역할이다.
    QHash<QString, double> m_fire_base;

    // ── 계절 프리셋 (season_threshold) ──
    /// season_key -> 버튼. 강조를 갈아끼우려면 키로 찾아야 한다.
    QHash<QString, QPushButton *> m_season_btns;
    /// 마지막으로 불러온 프리셋의 season_key (빈 문자열 = 없음).
    /// DB 값이 폼을 새로 채우면 지운다 — 그 순간 폼은 프리셋이 아니라 DB다.
    QString m_season_active;
    /// 버튼이 들어가는 가로 줄(08-19 — 보드처럼 패널 머리글 우측).
    /// 값이 오기 전에는 안내 라벨만 들어 있다.
    QHBoxLayout *m_season_col = nullptr;
    QLabel *m_season_wait = nullptr;   ///< "수신 대기" 자리표시 — 버튼이 생기면 사라진다
    /// 한 번 만든 뒤에는 다시 만들지 않는다. 30초마다 오는 발행에 버튼을
    /// 매번 지웠다 만들면 누르는 순간 사라지는 일이 생긴다.
    bool m_seasons_built = false;
};
