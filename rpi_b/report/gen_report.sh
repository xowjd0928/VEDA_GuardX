#!/bin/bash
# gen_report.sh — GuardX 예측 리포트 데이터 생성 (root cron에서 10분마다)
# 산출: /srv/guardx-report/data/*.csv|json — index.html이 fetch로 소비.
# root로 도는 이유: sudo -u postgres(peer 인증)가 비밀번호 없이 되는 유일한
# 계정. guardx_admin SELECT 권한(GRANT) 정리되면 일반 계정+PGCONN으로 전환 가능.
set -u
OUT=/srv/guardx-report/data
mkdir -p "$OUT"

CAM_BASE="https://192.168.0.3/opensdk/juan_application"
CAM_AUTH="admin:qkdwnsgks123!"     # chmod 700 유지 (자격증명 포함)

# ── ① 예측 vs 실측 (최근 7일, 분 단위) ───────────────────────────────
# actual = 모델 관측과 동일 정의: 300프레임 0패딩 분 중앙값
# 주의: psql -o는 postgres 계정으로 파일을 열어 root 소유 디렉터리에 못 쓴다 —
# 리다이렉션은 이 스크립트(root) 셸이 한다.
sudo -u postgres psql -d guardx -At -F, <<'SQL' > "$OUT/pred_vs_actual.csv.tmp"
WITH fc AS (
  SELECT date_trunc('minute', ts) AS m, ts, count(*)::int AS c
  FROM detections
  WHERE ts > now() - interval '7 days' AND ts < now()
  GROUP BY 1, 2
),
mm AS (
  SELECT m, CASE WHEN count(*) <= 150 THEN 0
                 ELSE (array_agg(c ORDER BY c))[count(*) - 150] END AS actual
  FROM fc GROUP BY m
),
p AS (
  SELECT round(extract(epoch FROM (target_ts - predicted_at))/60.0)::int AS h,
         date_trunc('minute', target_ts) AS m, predicted_count AS pc
  FROM congestion_prediction
  WHERE target_ts > now() - interval '7 days'
    AND target_ts < now() - interval '1 minute'
)
SELECT extract(epoch FROM p.m)::bigint AS em,
       coalesce(mm.actual, 0) AS actual,
       max(pc) FILTER (WHERE h = 5)   AS p5,
       max(pc) FILTER (WHERE h = 30)  AS p30,
       max(pc) FILTER (WHERE h = 60)  AS p60,
       max(pc) FILTER (WHERE h = 180) AS p180
FROM p LEFT JOIN mm USING (m)
GROUP BY p.m, mm.actual ORDER BY p.m;
SQL

# ── ② 오늘 실측 (KST 15분 빈, 사람-프레임 수) ────────────────────────
sudo -u postgres psql -d guardx -At -F, <<'SQL' > "$OUT/today_bins.csv.tmp"
SELECT (floor((extract(epoch FROM ts)+32400)/900)::bigint % 96) AS bin,
       count(*) AS person_frames
FROM detections
WHERE floor((extract(epoch FROM ts)+32400)/86400)
    = floor((extract(epoch FROM now())+32400)/86400)
GROUP BY 1 ORDER BY 1;
SQL

# ── ③ 카메라: 학습된 프로파일 + 현재 모델 상태 ──────────────────────
curl -sS -k -m 10 --digest -u "$CAM_AUTH" \
  "$CAM_BASE/forecast_day?date=tomorrow" -o "$OUT/profile.json.tmp"
curl -sS -k -m 10 --digest -u "$CAM_AUTH" \
  "$CAM_BASE/prediction" -o "$OUT/prediction_now.json.tmp"

# ── ④ 원자적 교체 + 생성 시각 (부분 파일이 서빙되지 않게) ────────────
for f in pred_vs_actual.csv today_bins.csv profile.json prediction_now.json; do
  [ -s "$OUT/$f.tmp" ] && mv "$OUT/$f.tmp" "$OUT/$f" || rm -f "$OUT/$f.tmp"
done
date -u +%FT%TZ > "$OUT/generated_at.txt"
chmod -R a+r "$OUT"
