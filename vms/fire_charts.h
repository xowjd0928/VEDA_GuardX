#pragma once

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

/**
 * @brief 화재 판단을 눈으로 보여주는 원형 위젯 2종
 *
 * 둘 다 QPainter의 호(arc)만 쓴다 — 삼각함수를 안 쓰는 게 의도적이다.
 * MinGW 빌드에서 libm(sin)이 링크에 안 잡혀 깨진 적이 있어(zone_sensor_store
 * 더미 파형), 각도 계산이 필요한 눈금도 "짧은 호 한 조각"으로 그린다.
 *
 * 색은 Theme:: 전역 팔레트를 쓴다. REPORT 화면처럼 별도 토큰을 두지 않는
 * 이유는 이 둘이 REPORT 전용이 아니라 SETTINGS·DEVICE CONTROL 양쪽에
 * 놓이기 때문이다 — 화면마다 다른 색이면 같은 의미가 달라 보인다.
 */

/**
 * @brief 실시간 화재 위험도 게이지 (도넛형 0~100 + 임계 눈금)
 *
 * decision.c가 낸 종합 위험도(composite_score)를 그린다. 이 값이 임계
 * (fire_score_threshold, 기본 65)를 n_confirm 사이클 연속 넘으면 화재로
 * 확정되므로, "지금 임계에서 얼마나 떨어져 있나"가 한눈에 보여야 한다.
 *
 * ⚠ 음수는 "0점"이 아니라 **"계산 안 함"**이다. FIRE 상태에서 센서가 무효라
 * 판정이 동결된 사이클이 그렇다(decision.h 규약). 0으로 그리면 "위험도 0 =
 * 안전"으로 정반대로 읽히므로 회색 + "판정 동결"로 따로 표시한다.
 */
class RiskGauge : public QWidget
{
    Q_OBJECT

public:
    explicit RiskGauge(int diameter, QWidget *parent = nullptr);

    /**
     * @param score     종합 위험도 0~100. 음수 = 판정 동결(계산 안 함)
     * @param threshold 화재 확정 임계 (fire_score_threshold). <=0 이면 눈금 생략
     */
    void set_score(double score, double threshold);

    /** @brief 도넛 아래 작은 설명 (예: "Z1"). 비우면 안 그린다 */
    void set_caption(const QString &caption);

protected:
    void paintEvent(QPaintEvent *ev) override;

private:
    double m_score = -1;
    double m_threshold = 0;
    QString m_caption;
    int m_diameter;
};

/**
 * @brief 퍼지 가중치 파이 (합 1.00) + 범례
 *
 * fire_threshold의 weight_* 5개를 그린다. 숫자 5개를 나열하는 것보다
 * "불꽃이 3분의 1을 차지한다"가 한눈에 들어오는 쪽이 판단 근거를 이해하기 쉽다.
 *
 * ⚠ 이 파이는 **설정값**이지 "이번 사이클의 실제 기여도"가 아니다.
 * decision.c는 무효(is_valid=false) 채널을 분모에서 빼고 재정규화하므로,
 * 불꽃 센서가 죽으면 실제 분포는 이 그림과 전혀 달라진다. 실시간 기여도를
 * 그리려면 채널별 점수를 RPi B가 내보내야 하는데(지금은 계산 후 버림)
 * 그건 별건이다.
 */
class WeightPie : public QWidget
{
    Q_OBJECT

public:
    explicit WeightPie(QWidget *parent = nullptr);

    struct Slice {
        QString label;
        double weight = 0;   ///< 0~1
        QColor color;
    };

    void set_slices(const QVector<Slice> &slices);

    /** @brief 채널 순서(가스/불꽃/온도/습도/표면온도)에 대응하는 고정 색.
     *  위험도 색(파랑/노랑/빨강)과 안 겹치게 골랐다 — 여기서 색은 "얼마나
     *  위험한가"가 아니라 "어느 센서인가"를 뜻하기 때문이다. */
    static QColor slice_color(int index);

protected:
    void paintEvent(QPaintEvent *ev) override;

private:
    QVector<Slice> m_slices;
};
