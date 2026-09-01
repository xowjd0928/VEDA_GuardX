#!/bin/bash
# backfill_jsonl.sh - rpib_events.jsonl -> PostgreSQL 백필 래퍼 (PHASE 5)
#
# 사용법:
#   ./backfill_jsonl.sh [jsonl경로]
#   (경로 생략 시 ../rpib_app/app/rpib_events.jsonl)
#
# 사전조건: PG* 환경변수가 export 되어 있어야 함 (엔진과 동일).
#
# 왜 래퍼가 필요한가:
#   파일을 읽는 \copy는 psql 메타명령이라 :'변수' 치환이 되지 않는다.
#   psql -v path=... 로 넘겨도 무시되고 "No such file or directory"가 난다.
#   그래서 경로를 셸에서 확정해 넣는다. 적재와 변환이 같은 psql 세션에
#   있어야 임시 테이블이 살아 있으므로, 한 번의 psql 호출 안에서
#   "적재 -> \i 본체" 순으로 실행한다.
set -e

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
JSONL="${1:-$SELF_DIR/../rpib_app/app/rpib_events.jsonl}"
SQL="$SELF_DIR/backfill_jsonl.sql"

if [ ! -f "$JSONL" ]; then
    echo "백필할 파일이 없다: $JSONL" >&2
    exit 1
fi
if [ ! -s "$JSONL" ]; then
    echo "파일이 비어 있다(폴백 기록이 없었다는 뜻): $JSONL"
    exit 0
fi

echo "== 백필 대상: $JSONL ($(wc -l < "$JSONL") 줄) =="

# JSON 한 줄을 통째로 한 필드로 읽어야 하는데, COPY의 기본 text 포맷은
# 백슬래시를 이스케이프로 해석하고 CSV 기본 인용부호(")는 JSON에 잔뜩
# 들어 있다. 그래서 JSON에 절대 나올 수 없는 제어문자를 인용/구분자로
# 지정한다 - 라인 원문 그대로 읽기 위한 관용구.
psql -v ON_ERROR_STOP=1 <<EOF
CREATE TEMP TABLE backfill_raw (line text);
\copy backfill_raw FROM '$JSONL' WITH (FORMAT csv, QUOTE E'\x01', DELIMITER E'\x02')
\i $SQL
EOF

echo
echo "완료. 다시 돌리면 중복 방지가 완벽하지 않으므로(A 재시작 시 seq 리셋)"
echo "이 파일은 옮기거나 비울 것:"
echo "  mv '$JSONL' '$JSONL.backfilled'"
