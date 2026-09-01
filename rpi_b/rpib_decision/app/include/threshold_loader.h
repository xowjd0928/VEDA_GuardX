#ifndef THRESHOLD_LOADER_H
#define THRESHOLD_LOADER_H

#include "guardx_err.h"

/*
 * threshold_loader - fire_threshold 테이블의 is_active=true 행을 읽어
 * decision.c의 런타임 설정을 갱신한다 (PHASE 4 핫리로드).
 *
 * db_writer.c(JSONL 스텁, 센서/이벤트 기록 경로)와는 별개의 관심사라
 * 분리했다 - 이쪽은 libpq로 "읽기 전용" 짧은 연결을 매 호출마다 새로
 * 맺고 끊는다(리로드가 초당 1회씩 도는 게 아니라 설정이 바뀔 때만
 * 드물게 호출되므로 연결 유지 비용을 들일 이유가 없다).
 *
 * 접속 정보는 libpq 표준 환경변수(PGHOST/PGPORT/PGUSER/PGPASSWORD/
 * PGDATABASE)로 받는다 - 코드에 계정 정보를 넣지 않기 위함.
 */

/* 실패(연결 불가/활성 행 없음/검증 실패) 시 기존 설정을 그대로 두고
 * 에러만 반환한다 - 호출측은 "폴백 유지"를 따로 처리할 필요가 없다. */
guardx_err_t threshold_load_and_apply(void);

#endif /* THRESHOLD_LOADER_H */
