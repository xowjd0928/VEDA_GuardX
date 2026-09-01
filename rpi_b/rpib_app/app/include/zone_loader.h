#ifndef ZONE_LOADER_H
#define ZONE_LOADER_H

#include "guardx_err.h"

/*
 * zone_loader.h - fire_zone 테이블(DB) 읽기.
 *
 * "zone(RPi A/C 물리 쌍)이 몇 개고, 각각 어느 MQTT node_id를 쓰는가"의
 * 진실원천은 DB다(fire_schema.sql fire_zone). 코드에 하드코딩하지 않고
 * 매번 여기서 읽어온다 - threshold_loader.c와 완전히 같은 이유·같은 패턴
 * (부팅 시 1회 + guardx/config/rpib 신호로 핫리로드).
 *
 * MAX_FIRE_ZONES: 동적 할당(malloc) 대신 고정 상한 배열을 쓴다 - 이
 * 코드베이스가 임베디드 스타일로 동적 할당을 거의 안 쓰는 관례를 따른다
 * (예: sensor_msg_t/GUARDX_JSON_MAX 고정 버퍼). 실제 zone 수가 이보다
 * 많아지면 컴파일타임 상수 하나만 올리면 된다.
 */
#define MAX_FIRE_ZONES 8

typedef struct {
    int  zone_id;
    char zone_name[64];
    char rpia_node_id[32];   /* guardx/sensor/%s 의 %s */
    char rpic_node_id[32];   /* guardx/actuator/%s 의 %s */
} fire_zone_t;

/*
 * fire_zone 테이블 전체를 zone_id 순으로 읽어 out에 채운다.
 * 성공 시 *count에 실제 로드된 zone 수를 쓴다(1 이상 보장).
 * 실패(연결 실패, 빈 테이블, 상한 초과 등) 시 out과 *count를 건드리지
 * 않는다 - 호출측(main.c)이 기존 매핑을 그대로 유지할 수 있게.
 */
guardx_err_t zone_loader_load(fire_zone_t out[MAX_FIRE_ZONES], int *count);

#endif /* ZONE_LOADER_H */
