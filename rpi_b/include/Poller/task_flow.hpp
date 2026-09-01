#pragma once
// task_flow — GET /events → line_flow INSERT (v15)
//   /events의 룰별 누적 count에서 60초 델타를 계산해 분당 통과량으로 적재.
//   카메라 수정 불필요. 카운터는 앱 재시작 시 리셋되므로(음수 델타) 그때는
//   적재 없이 기준선만 재설정 — DB가 유일한 영속 flow 기록이 된다.
//   LineCrossing 룰만 대상 (rule 컬럼이 라인 이름 — 채널 매핑은 소비자 몫).
#include <pqxx/pqxx>
#include "Config/config.hpp"

void pollFlow(const Config& cfg, pqxx::connection& db);
