#pragma once

#include <cstddef>

/**
 * @brief 화재 zone의 VMS 표시 정보 — 단일 진실원천
 *
 * rpib_engine은 zone을 fire_zone(DB)에서 읽지만, 그 표에는 **VMS에만 필요한
 * 것**이 없다: 어느 CCTV 채널이 그 구역을 비추는가, 그리고 실제 하드웨어가
 * 있는가. 그래서 화면 쪽 메타데이터는 여기 한 곳에 모은다.
 *
 * 여기 있는 것을 fire_zone 테이블에 넣지 않은 이유:
 *   - cctv_channel은 카메라 배치(VMS 관심사)라 판단 엔진이 알 필요가 없다.
 *     넣으면 rpib_engine이 안 쓰는 컬럼을 읽고 나르게 된다.
 *   - dummy는 "아직 하드웨어가 없다"는 VMS 시연용 표시다. DB에 넣으면
 *     rpib_engine이 그 zone을 진짜로 취급해 영원히 오지 않는 rpia-N을
 *     기다리는 좀비 zone이 된다 — 값은 영영 안 오는데 판정 상태만 3개 는다.
 *
 * ── 확장 방법 ──
 * zone 2에 실제 RPi A/C와 카메라가 붙으면:
 *   ① DB: fire_zone에 (2, '2구역', 'rpia-2', 'rpic-2') INSERT
 *      → rpib_engine은 재시작만으로 자동 인식(zone_loader.c)
 *   ② 여기: 아래 표의 zone 2 행에서 dummy를 false로, cctv_channel을 실제
 *      채널로 수정
 * 화면 코드는 한 줄도 안 바뀐다 — DeviceControlPage(구역 셀렉터·비교 칩),
 * ZoneSensorStore(더미 생성), MainWindow(경보 라우팅) 모두 이 표만 읽는다.
 */
struct FireZoneInfo {
    int  zone_id;        ///< fire_zone.zone_id — RPi B와 주고받는 식별자
    int  cctv_channel;   ///< LiveViewer 화면 채널(0..3). -1 = 배정된 카메라 없음
    bool dummy;          ///< true면 실 하드웨어 없음 (MQTT 미구독, 가짜 값 표시)
};

/*
 * !!! cctv_channel은 실측 확인 필요 !!!
 * zone 1을 실제로 비추는 카메라가 화면 몇 번 채널인지는 현장 배치에
 * 달렸다. 지금은 0(첫 채널)로 둔다 - 틀렸다면 이 숫자만 고치면 되고,
 * 다른 코드는 영향받지 않는다.
 *
 * fire_zone.zone_id와 congestion의 zones.zone_id는 서로 다른 체계라
 * (fire_schema.sql 주석) 그쪽 매핑을 그대로 가져다 쓰면 안 된다.
 */
inline const FireZoneInfo *fire_zone_table(int *count)
{
    static const FireZoneInfo kZones[] = {
        { 1, 0,  false },   // 실 하드웨어 (rpia/rpic)
        { 2, -1, true  },   // 시연용 더미 — 카메라 미배정
        { 3, -1, true  },
        { 4, -1, true  },
    };
    if (count)
        *count = int(sizeof(kZones) / sizeof(kZones[0]));
    return kZones;
}

/** @brief zone_id로 조회. 없으면 nullptr — 호출측이 "모르는 zone"으로 처리할 것 */
inline const FireZoneInfo *find_fire_zone(int zone_id)
{
    int n = 0;
    const FireZoneInfo *t = fire_zone_table(&n);
    for (int i = 0; i < n; ++i)
        if (t[i].zone_id == zone_id)
            return &t[i];
    return nullptr;
}
