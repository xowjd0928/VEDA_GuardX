#pragma once

#include <QLatin1String>
#include <QString>

/**
 * @brief RPi A 센서 채널 정의 — 표시 메타데이터의 단일 진실원천
 *
 * 원래 device_control_page.cpp 안에 있었는데, ZoneSensorStore(더미 값 생성이
 * 채널별 범위를 알아야 함)와 화면 양쪽이 쓰게 되면서 헤더로 뺐다. 표를 두
 * 벌 두면 한쪽만 고쳐져 라벨·범위가 어긋난다.
 *
 * key는 DB sensor_channel.channel_key / RPi A json_builder.c의 values 키와
 * 1:1이다 — 이름으로 접근하므로 채널 순서가 바뀌거나 하나 빠져도 안 깨진다.
 *
 * thr_safe_key/thr_danger_key는 fire_threshold의 컬럼명. 그 값을 못 받았을
 * 때만 fallback_min/max로 예전 방식(파랑/회색) 그대로 표시한다.
 *
 * fallback_min/max는 threshold를 아직 못 받았을 때(또는 irtemp_ambient
 * 처럼 threshold가 원래 없을 때)만 쓰는 표시 범위다 — fire_threshold와는
 * 별개이므로 절대 판정값으로 취급하면 안 된다.
 */
struct SensorField {
    const char *key;
    const char *label;
    const char *device;
    double fallback_min;
    double fallback_max;
    int decimals;
    const char *suffix;
    const char *thr_safe_key;
    const char *thr_danger_key;
    /// 그래프를 위아래 뒤집을지 — 이 채널이 "낮을수록 위험"(내림차순)이면
    /// 값이 그대로면 위험해질수록 선이 아래로 내려가 다른 채널(오름차순,
    /// 위험해질수록 위로)과 시각적으로 반대로 보인다. true면 축을 뒤집어서
    /// "위로 갈수록 위험"을 모든 채널에서 통일한다. 게이지·색상(risk_raw)은
    /// 이미 방향 무관하게 계산되므로 영향 없음 — 여기 그래프 렌더링만의 문제.
    bool graph_reversed = false;

    /// 더미 구역이 흉내낼 평상시 값과 흔들림 폭 (ZoneSensorStore 전용).
    /// fallback_min/max의 중간을 쓰면 실제와 전혀 다른 값이 나온다 —
    /// 예를 들어 spark_raw는 범위가 0~1023이지만 평시 실측은 900~1000대다
    /// (불꽃이 없을 때 오히려 높은 내림차순 채널). 그래서 판정 범위가 아니라
    /// **실측 평시값**을 따로 적는다. 출처는 decision.h의 실측 주석
    /// (2026-07-31 벤치마킹) + zone 1 라이브 관측값.
    double dummy_base;
    double dummy_jitter;
};

inline const SensorField SENSOR_FIELDS[] = {
    //                                                                          그래프  더미   흔들림
    //  key             label            device       min  max  dec  suffix     반전    기준   폭
    { "gas_raw",        "Gas (raw)",      "MQ-2",     0,   1023, 0, " adc",
      "gas_raw_min",        "gas_raw_max",         false,  565.0, 22.0 },
    { "spark_raw",      "Flame (raw)",    "TS0226",   0,   1023, 0, " adc",
      "spark_raw_safe",     "spark_raw_danger",    true,   960.0, 45.0 },
    { "temperature",    "Temperature",    "SHT30",    0,   60,   1, " °C",
      "temp_min_c",         "temp_max_c",          false,   24.2,  0.9 },
    { "humidity",       "Humidity",       "SHT30",    0,   100,  1, " %",
      "humi_safe_percent",  "humi_danger_percent", false,   51.5,  2.2 },
    { "irtemp_ambient", "Ambient temp",   "MLX90614", 0,   60,   1, " °C",
      nullptr,              nullptr,               false,   24.5,  0.9 },
    { "irtemp_object",  "Surface temp",   "MLX90614", 0,   100,  1, " °C",
      "irtemp_min_c",       "irtemp_max_c",        false,   24.5,  1.3 },
};

inline constexpr int SENSOR_FIELD_COUNT =
    int(sizeof(SENSOR_FIELDS) / sizeof(SENSOR_FIELDS[0]));

inline const SensorField *find_sensor_field(const QString &key)
{
    for (const SensorField &f : SENSOR_FIELDS)
        if (key == QLatin1String(f.key))
            return &f;
    return nullptr;
}

/** @brief 화재 원인 channel_key -> 사람이 읽는 라벨. 위 표를 그대로 쓴다 —
 *  같은 채널을 가리키는데 화면마다 다른 이름이 뜨면 안 되기 때문 */
inline QString cause_text(const QString &channel_key)
{
    if (const SensorField *f = find_sensor_field(channel_key))
        return QString::fromUtf8(f->label);
    return channel_key.isEmpty() ? QString("unknown") : channel_key;
}
