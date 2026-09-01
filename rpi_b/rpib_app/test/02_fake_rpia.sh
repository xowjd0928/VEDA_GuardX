#!/bin/bash
# 02_fake_rpia.sh - RPi A 역할을 흉내내 센서 데이터 발행 헬퍼
# (프로토콜 규약 4-1/4-2 스키마, RPi A json_builder.c 출력과 동일 형태)
#
# 신 스키마: values={gas_raw,spark_raw,temperature,humidity,
#           irtemp_ambient,irtemp_object}, valid={gas,temphum,spark,irtemp}
#
# PHASE 2: 판단이 퍼지 가중치 융합(decision.c)으로 바뀌어, "센서 1개만
# 극단값"으로는 더 이상 화재가 확정되지 않는다 - 채널 단독 최대 기여도가
# 전부 FIRE_SCORE_THRESHOLD(65)보다 낮기 때문 (아래 표). 실제 FIRE를
# 보려면 combo/crossfire/fire_demo를 쓸 것.
#
#   채널      가중치  단독 최대 기여도(가중치*100)
#   spark     0.35    35.0  (그래도 65 미만)
#   irtemp    0.25    25.0
#   gas       0.20    20.0
#   temp      0.15    15.0
#   humidity  0.05     5.0
#
# PHASE 2 보강: 위 표 때문에 "불꽃 100 + 표면온도 100"도 60점이라 화재가
# 안 되는 구멍이 있었다. 그래서 두 경로가 추가됐고 각각 전용 케이스가 있다.
#   crossfire -> 교차 확증 오버라이드 (가중합산 42점인데도 FIRE)
#   degraded  -> 무효 채널 재정규화   (보정 전 61.5점 -> 보정 후 76.9점, FIRE)
#
# 사용법:
#   ./02_fake_rpia.sh normal [N]     # 전 채널 안전구간 N사이클 (기본 1, 1Hz) - 종합 0점
#   ./02_fake_rpia.sh gas [N]        # 가스만 최대치 N사이클 (기여 20) - FIRE 안 남, 오탐방지 데모
#   ./02_fake_rpia.sh temp [N]       # 온도만 최대치 N사이클 (기여 15) - FIRE 안 남
#   ./02_fake_rpia.sh spark [N]      # 불꽃만 최대치 N사이클 (기여 35) - FIRE 안 남 (표면온도가 낮아 오버라이드도 불성립)
#   ./02_fake_rpia.sh combo [N]      # 5채널 동시 상승 N사이클 (종합 약 70점) - FIRE 확정
#   ./02_fake_rpia.sh mid [N]        # 종합 55.4점 - 임계 65면 정상, 임계 50이면 FIRE (PHASE 4 리로드 검증용)
#   ./02_fake_rpia.sh crossfire [N]  # 불꽃+표면온도만 70점, 나머지 안전 - 가중합산 42점이나 오버라이드로 FIRE
#   ./02_fake_rpia.sh degraded [N]   # 가스 valid=false + 나머지 상승 - 재정규화로 FIRE (cause=irtemp 기대)
#   ./02_fake_rpia.sh invalid [N]    # 가스 valid=false + 나머지 정상 N사이클 (동결 검증용, 생존 가중치 0.80)
#   ./02_fake_rpia.sh crippled [N]   # 불꽃+표면온도 valid=false (생존 0.40) - 동결 완화가 "거부"돼야 정상
#   ./02_fake_rpia.sh button         # 비상 버튼 로그 1건 (QoS 2)
#   ./02_fake_rpia.sh fire_demo      # combo로 화재 확정 -> 해제까지 풀 시나리오
set -e

HOST="${MQTT_HOST:-localhost}"
TOPIC_SENSOR="guardx/sensor/rpia"
TOPIC_BUTTON="guardx/sensor/rpia/button"
SEQ=$RANDOM

now_ms() { date +%s%3N; }

# sensor 1건 발행 인자:
#   $1 gas_raw  $2 spark_raw  $3 temp  $4 hum  $5 irtemp_object
#   $6 gas_valid  [$7 spark_valid]  [$8 irtemp_valid]  [$9 temphum_valid]
# 7~9는 생략 시 true - 기존 케이스들은 gas만 무효화하면 됐기 때문이다.
# 동결 완화 검증(생존 가중치 게이트)에는 불꽃·표면온도를 따로 죽여야 해서
# 나중에 확장했다.
# (irtemp_ambient는 판단 입력이 아니라 고정값으로 채움)
pub_sensor() {
    local sv="${7:-true}" iv="${8:-true}" tv="${9:-true}"
    local payload="{\"node_id\":\"rpia\",\"timestamp\":$(now_ms),\"seq\":$SEQ,\"values\":{\"gas_raw\":$1,\"spark_raw\":$2,\"temperature\":$3,\"humidity\":$4,\"irtemp_ambient\":23.0,\"irtemp_object\":$5},\"valid\":{\"gas\":$6,\"temphum\":$tv,\"spark\":$sv,\"irtemp\":$iv}}"
    SEQ=$((SEQ + 1))
    echo "PUB $payload"
    mosquitto_pub -h "$HOST" -q 0 -t "$TOPIC_SENSOR" -m "$payload"
}

# N사이클 반복 (1Hz)
cycles() {  # $1=횟수, 나머지=pub_sensor 인자
    local n=$1; shift
    for _ in $(seq "$n"); do
        pub_sensor "$@"
        sleep 1
    done
}

case "$1" in
# 2026-07-31: gas_raw/spark_raw 둘 다 실측 반영.
#   gas   평시 500~600(700 안 넘음), 장시간 노출 780~790 포화(1회 800)
#         -> MIN 300->650 (MAX 800은 실측 포화점과 맞아 유지)
#   spark 불꽃 근접 시 raw가 낮아지는 방향으로 확인돼(평시 900~1000,
#         라이터 근접 2~3) 내림차순 퍼지화(safe=850/danger=30)로 변경
#         - "spark 낮을수록 위험"이다.
#                        gas spark temp  hum  irtemp_obj gas_valid
normal)  cycles "${2:-1}" 500  950  23.5  60.2   25.0     true  ;;  # 전 채널 안전구간, 종합 0점
gas)     cycles "${2:-3}" 800  950  23.5  60.2   25.0     true  ;;  # gas만 최대(기여20) - FIRE 안 남
temp)    cycles "${2:-3}" 500  950  60.0  60.2   25.0     true  ;;  # temp만 최대(기여15) - FIRE 안 남
spark)   cycles "${2:-3}" 500   20  23.5  60.2   25.0     true  ;;  # spark만 최대(기여35) - FIRE 안 남 (표면온도가 낮아 오버라이드도 불성립)
combo)   cycles "${2:-3}" 755  276  52.5  25.5   68.0     true  ;;  # 5채널 동시 상승, 종합 정확히 70.0점 - FIRE 확정
crossfire) cycles "${2:-3}" 500 276  23.5  60.2   68.0     true ;;  # 불꽃70/표면온도70, 나머지 0 -> 가중합산 42점(미달)이나 오버라이드로 FIRE
# PHASE 4 핫리로드 검증 전용. 임계값 두 개(65/50) 사이에 정확히 끼도록 고른 값이다.
#   gas 70*0.20=14.0 + spark 50*0.35=17.5 + temp 40*0.15=6.0
#   + humi 57.1*0.05=2.86 + irtemp 60*0.25=15.0  =>  종합 55.36점
# 불꽃 50점이라 교차 확증(70)은 불성립 - 순수하게 가중합산 임계값만 가른다.
# 기대: 임계 65 -> 아무 일 없음 / 임계 50 -> FIRE 확정(cause=spark, 기여 17.5 최대)
mid)     cycles "${2:-3}" 755  440  45.0  30.0   64.0     true ;;
degraded) cycles "${2:-3}"  0  358  57.5  18.5   76.0     false ;;  # gas 무효 + spark60/temp90/humi90/irtemp90 -> 재정규화 76.9점 FIRE (보정 전 61.5로 미달이던 값)
invalid) cycles "${2:-1}"   0  950  23.5  60.2   25.0     false ;;  # gas valid=false 동결
# 동결 완화 거부 검증. 불꽃(0.35)+표면온도(0.25)를 죽여 생존 가중치를
# 0.40(gas .20 + temp .15 + humi .05)으로 만든다. min_valid_weight 0.50
# 미만이라 아무리 오래 동결돼도 완화가 발동하면 안 된다 - "화재가 직접
# 증거 센서를 태워먹은" 패턴이라 사람이 개입해야 하는 경우다.
# 값 자체는 전부 안전구간이라, 만약 완화가 잘못 발동하면 곧바로 해제가
# 성립해버려서 실패가 눈에 띈다.
crippled) cycles "${2:-8}" 500 950  23.5  60.2   25.0     true false false ;;
button)
    payload="{\"node_id\":\"rpia\",\"timestamp\":$(now_ms),\"seq\":$SEQ,\"event\":\"emergency_button\",\"press_count\":1}"
    echo "PUB $payload"
    mosquitto_pub -h "$HOST" -q 2 -t "$TOPIC_BUTTON" -m "$payload"
    ;;
fire_demo)
    echo "== 1) 정상 2사이클 (아무 일 없어야 함) =="
    cycles 2 500 950 23.5 60.2 25.0 true
    echo "== 2) 5채널 동시 상승 3사이클 (3번째에 FIRE 확정 + 명령 5건, 종합 약 70점) =="
    cycles 3 755 276 52.5 25.5 68.0 true
    echo "== 3) 화재 중 정상값 10사이클 (10번째에 RECOVER + 정지 3건) =="
    cycles 10 500 950 23.5 60.2 25.0 true
    echo "fire_demo 완료 - 엔진 stdout과 03_watch_rpic.sh 출력 확인"
    ;;
*)
    grep '^#   ' "$0" | sed 's/^#   //'
    exit 1
    ;;
esac
