#include "storage_settings_card.h"
#include "clip_recorder.h"
#include "panel_chrome.h"
#include "theme.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

// 새 UI 문구는 영어 (12번 전면 영문화 합의 — site_settings_card 와 동일 규칙)

StorageSettingsCard::StorageSettingsCard(QWidget *parent)
    : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ⚠ WA_StyledBackground 없이는 QSS #Panel 배경이 안 칠해진다 (08-19)
    auto *card = new QWidget(this);
    card->setObjectName("Panel");
    card->setAttribute(Qt::WA_StyledBackground);
    outer->addWidget(card);
    auto *shell = new QVBoxLayout(card);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    shell->addWidget(PanelChrome::header(
        QStringLiteral("Event recording"), QString(), card));

    auto *body = new QWidget(card);
    shell->addWidget(body);
    auto *col = new QVBoxLayout(body);
    col->setContentsMargins(20, 10, 20, 14);
    col->setSpacing(8);

    auto *hint = new QLabel(
        "on fire, emergency-button or critical crowd alerts the vms saves "
        "a clip covering 15 s before and 15 s after the event "
        "(fire and button = all channels)", this);
    hint->setFont(Theme::mono_font(10));
    hint->setWordWrap(true);
    Theme::restyle(hint, [] {
        return QString("color:%1;").arg(Theme::textFaint.name());
    });
    col->addWidget(hint);

    // ---- 저장 폴더 행: 경로 + [Change...] + [Open] ----
    auto *dir_row = new QHBoxLayout();
    dir_row->setSpacing(10);

    m_dir_lbl = new QLabel(this);
    m_dir_lbl->setFont(Theme::mono_font(10.5));
    // 좁은 우측 열(560px) — 긴 경로가 카드 폭을 밀지 않게 접는다
    // ("crunched" 사고의 재발 방지, site_settings_card 와 같은 이유)
    m_dir_lbl->setWordWrap(true);
    Theme::restyle(m_dir_lbl, [] {
        return QString("color:%1;").arg(Theme::textMid.name());
    });
    dir_row->addWidget(m_dir_lbl, 1);

    auto *change_btn = new QPushButton("Change...", this);
    change_btn->setFont(Theme::ui_font(11, 600));
    change_btn->setCursor(Qt::PointingHandCursor);
    connect(change_btn, &QPushButton::clicked, this, [this] {
        const QString picked = QFileDialog::getExistingDirectory(
            this, "Choose the recording folder", ClipRecorder::storage_dir());
        if (picked.isEmpty())
            return;   // 취소 — 기존 값 유지
        ClipRecorder::set_storage_dir(picked);
        refresh_dir_label();
        set_status("recording folder updated", false);
    });
    dir_row->addWidget(change_btn);

    auto *open_btn = new QPushButton("Open", this);
    open_btn->setFont(Theme::ui_font(11, 600));
    open_btn->setCursor(Qt::PointingHandCursor);
    connect(open_btn, &QPushButton::clicked, this, [this] {
        // 아직 클립이 없어도 열리게 폴더를 먼저 만든다
        const QString dir = ClipRecorder::storage_dir();
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    dir_row->addWidget(open_btn);
    col->addLayout(dir_row);

    // ---- 테스트 녹화 — 실제 이벤트 경로를 그대로 태운다 ----
    auto *test_row = new QHBoxLayout();
    test_row->setSpacing(10);

    auto *test_btn = new QPushButton("Record test clip", this);
    test_btn->setFont(Theme::ui_font(11, 600));
    test_btn->setCursor(Qt::PointingHandCursor);
    test_btn->setToolTip(
        "Save a clip from every channel right now, exactly like a real "
        "event would - buffered video plus the next 15 s.");
    connect(test_btn, &QPushButton::clicked, this, [this] {
        ClipRecorder::instance()->trigger_all(QStringLiteral("test"));
        set_status("recording... finishes 15 s from now", false);
    });
    test_row->addWidget(test_btn);
    test_row->addStretch(1);
    col->addLayout(test_row);

    m_status = new QLabel(this);
    m_status->setFont(Theme::mono_font(10));
    m_status->setWordWrap(true);
    // ⚠ restyle 은 연결을 누적한다 — 등록은 여기 1회뿐이어야 한다 (08-06
    //   실사고). 상태가 바뀔 때는 set_status 가 플래그만 바꾸고 같은 규칙을
    //   직접 재적용한다.
    Theme::restyle(m_status, [this] {
        return QString("color:%1;")
            .arg((m_status_error ? Theme::alarm : Theme::green).name());
    });
    col->addWidget(m_status);

    // 결과는 이벤트 녹화든 테스트든 같은 신호로 온다 — 마지막 것을 보여준다
    connect(ClipRecorder::instance(), &ClipRecorder::clip_saved, this,
            [this](int ch, const QString &path) {
        set_status(QString("CH%1 clip saved: %2").arg(ch + 1).arg(path), false);
    });
    connect(ClipRecorder::instance(), &ClipRecorder::clip_failed, this,
            [this](int ch, const QString &reason) {
        set_status(QString("CH%1 clip failed: %2").arg(ch + 1).arg(reason), true);
    });

    refresh_dir_label();
}

void StorageSettingsCard::refresh_dir_label()
{
    m_dir_lbl->setText(QDir::toNativeSeparators(ClipRecorder::storage_dir()));
}

void StorageSettingsCard::set_status(const QString &text, bool error)
{
    m_status->setText(text);
    m_status_error = error;
    // 생성자에서 등록한 restyle 람다와 같은 규칙 — 여기서 restyle 을 다시
    // 부르면 테마 전환 연결이 호출마다 쌓인다.
    m_status->setStyleSheet(QString("color:%1;")
        .arg((error ? Theme::alarm : Theme::green).name()));
}
