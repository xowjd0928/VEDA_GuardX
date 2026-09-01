#pragma once
// credentials — 카메라 자격증명 로드
//
// ★★★ [방준한 구현 예정 — 이 모듈만 남겨둠] ★★★
// 계획: 카메라 접속 계정(user/pass)을 DB에 저장하고 여기서 조회.
// (DB 변경 노트: cameras 테이블 확장 또는 camera_credentials 테이블 —
//  저장처 스키마 확정은 사용자 몫)
// 구현 전까지는 환경변수(CAM_USER/CAM_PASS) 값을 그대로 사용한다.
#include <pqxx/pqxx>
#include "Config/config.hpp"

void loadCameraCredentials(Config& cfg, pqxx::connection& db);

