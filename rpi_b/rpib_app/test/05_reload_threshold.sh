#!/bin/bash
# 05_reload_threshold.sh - PHASE 4 핫리로드 검증 헬퍼
#
# fire_threshold에 새 활성 행을 넣고 guardx/config/rpib 신호를 보낸다.
# 엔진을 재시작하지 않고 stdout에 "threshold: applied (...)" 로그가
# 새 값으로 찍히면 핫리로드 성공.
#
# 사전조건: PG* 환경변수가 이미 export 되어 있어야 함 (엔진과 동일한
# 접속 정보 - 비밀번호를 이 스크립트에 넣지 않기 위함). 예:
#   export PGHOST=localhost PGUSER=guardx_writer PGDATABASE=guardx
#   export PGPASSWORD=...   # 직접 입력 (echo로 남기지 말 것)
#
# 사용법:
#   ./05_reload_threshold.sh demo     # fire_score_threshold 65->50 낮춘 새 행 활성화 + 신호
#   ./05_reload_threshold.sh restore  # decision.h 원래 잠정치(65/relax 60)로 복귀 + 신호
#   ./05_reload_threshold.sh fastfreeze # freeze_relax_cycles 60->5 (동결 완화 검증용)
set -e

HOST="${MQTT_HOST:-localhost}"
TOPIC_CONFIG="guardx/config/rpib"

# $1 = fire_score_threshold, $2 = freeze_relax_cycles, $3 = updated_by 메모
# is_active 부분 유니크 인덱스 때문에 "기존 행 비활성화 + 신규 행
# 활성화"가 한 트랜잭션 안에서 일어나야 한다.
activate_row() {
    psql -v ON_ERROR_STOP=1 <<SQL
BEGIN;
UPDATE fire_threshold SET is_active = FALSE WHERE is_active;
INSERT INTO fire_threshold (
    gas_raw_min, gas_raw_max, spark_raw_safe, spark_raw_danger,
    temp_min_c, temp_max_c, humi_safe_percent, humi_danger_percent,
    irtemp_min_c, irtemp_max_c,
    weight_gas, weight_spark, weight_temp, weight_humi, weight_irtemp,
    fire_score_threshold, n_confirm, n_recover, freeze_relax_cycles,
    min_valid_weight, override_spark_score, override_irtemp_score,
    is_active, updated_by
) VALUES (
    650.0, 800.0, 850.0, 30.0,
    35.0, 60.0, 50.0, 15.0,
    40.0, 80.0,
    0.20, 0.35, 0.15, 0.05, 0.25,
    $1, 3, 10, $2,
    0.50, 70.0, 70.0,
    TRUE, '$3'
);
COMMIT;
SQL
}

signal_reload() {
    echo "PUB (트리거 전용, 내용 없음) -> $TOPIC_CONFIG"
    mosquitto_pub -h "$HOST" -q 1 -t "$TOPIC_CONFIG" -m "reload"
}

case "$1" in
demo)
    echo "== fire_score_threshold 65 -> 50 낮춘 새 행 활성화 =="
    activate_row 50.0 60 "05_reload_threshold.sh demo"
    signal_reload
    echo "엔진 stdout에서 'threshold: applied (fire_score>=50.0 ...)' 확인"
    ;;
restore)
    echo "== decision.h 원래 잠정치(65 / relax 60)로 복귀 =="
    activate_row 65.0 60 "05_reload_threshold.sh restore"
    signal_reload
    echo "엔진 stdout에서 'threshold: applied (fire_score>=65.0 ... relax=60 ...)' 확인"
    ;;
fastfreeze)
    # 기본 60사이클을 그대로 쓰면 동결 완화 검증에 1분씩 걸린다. 임계값을
    # 낮춰서 5초 만에 완화가 발동하게 한다 - 재컴파일 없이 이런 실험을
    # 하려고 PHASE 4 핫리로드를 만든 것이다.
    echo "== freeze_relax_cycles 60 -> 5 (동결 완화 검증용) =="
    activate_row 65.0 5 "05_reload_threshold.sh fastfreeze"
    signal_reload
    echo "엔진 stdout에서 'threshold: applied (... relax=5 cycles)' 확인"
    ;;
*)
    grep '^#   ' "$0" | sed 's/^#   //'
    exit 1
    ;;
esac
