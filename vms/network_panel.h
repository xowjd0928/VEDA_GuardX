#pragma once

#include <QWidget>

class QLabel;
class QNetworkAccessManager;

/**
 * @brief CAMERA 탭 [네트워크] 하위 탭 — **조회 전용** (기획서 §3-F, 8단계)
 *
 * 인터페이스(IP/DNS/MAC)·RTSP·QoS를 view로 읽어 원문 표시만 한다.
 * 설정 변경 버튼은 의도적으로 없다 — 원격 네트워크 변경은 자기 목 자르기.
 * 미지원 서브메뉴(NVR 전용 등)는 카드에 미지원으로 표시.
 */
class NetworkPanel : public QWidget
{
    Q_OBJECT

public:
    explicit NetworkPanel(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *ev) override;

private:
    void fetch(const QString &submenu, QLabel *into);

    QNetworkAccessManager *m_net = nullptr;
    QList<QPair<QString, QLabel *>> m_cards;  ///< submenu -> 본문 라벨
    bool m_loaded = false;
};
