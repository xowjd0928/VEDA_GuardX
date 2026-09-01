#!/usr/bin/env bash
# Export accumulated pilot data into backtest2zone.cpp's CSV format, so the
# network-model experiment re-runs on REAL data with zero glue code:
#   ./export_twozone.sh > twozone_real.csv
#   ./backtest2zone twozone_real.csv
#
# Real data has no ground truth, so truth columns = observed counts (same
# scoring basis the incumbent backtest has always used). Direction mapping is
# the walk-test result of 2026-07-29 (post name-swap):
#   z1entrance  R=in   L=out  |  z2entrance  L=in   R=out
#   z2z1boundary R=Z1→Z2  L=Z2→Z1     (dup excluded on purpose)
# Zone->channel comes from zones (Z1=ch0, Z2=ch1) — adjust EPOCH/psql as needed.
set -euo pipefail
EPOCH="${EPOCH:-2026-07-29T09:17:00Z}"
PSQL="${PSQL:-psql}"

# header first — backtest2zone consumes line 1 as a header
echo "min,occ1_obs,occ2_obs,occ1_true,occ2_true,z1in,z1out,b12,b21,z2in,z2out"
$PSQL -X -q -A -t -F, -v ON_ERROR_STOP=1 <<SQL
WITH z AS (SELECT zone_id, channel FROM zones WHERE channel IN (0,1)),
mins AS (
  SELECT generate_series(
    date_trunc('minute', (SELECT min(bucket_ts) FROM zone_occupancy
                          WHERE bucket_ts >= '$EPOCH')),
    date_trunc('minute', (SELECT max(bucket_ts) FROM zone_occupancy)),
    interval '1 minute') AS ts),
f AS (SELECT bucket_ts, rule, action, flow_count FROM line_flow
      WHERE bucket_ts >= '$EPOCH')
SELECT
  extract(epoch FROM m.ts)::bigint / 60,
  COALESCE(o1.person_count, 0), COALESCE(o2.person_count, 0),
  COALESCE(o1.person_count, 0), COALESCE(o2.person_count, 0),
  COALESCE((SELECT flow_count FROM f WHERE bucket_ts=m.ts AND rule='z1entrance'   AND action='Right'), 0),
  COALESCE((SELECT flow_count FROM f WHERE bucket_ts=m.ts AND rule='z1entrance'   AND action='Left'),  0),
  COALESCE((SELECT flow_count FROM f WHERE bucket_ts=m.ts AND rule='z2z1boundary' AND action='Right'), 0),
  COALESCE((SELECT flow_count FROM f WHERE bucket_ts=m.ts AND rule='z2z1boundary' AND action='Left'),  0),
  COALESCE((SELECT flow_count FROM f WHERE bucket_ts=m.ts AND rule='z2entrance'   AND action='Left'),  0),
  COALESCE((SELECT flow_count FROM f WHERE bucket_ts=m.ts AND rule='z2entrance'   AND action='Right'), 0)
FROM mins m
LEFT JOIN zone_occupancy o1 ON o1.bucket_ts=m.ts
  AND o1.zone_id=(SELECT zone_id FROM z WHERE channel=0)
LEFT JOIN zone_occupancy o2 ON o2.bucket_ts=m.ts
  AND o2.zone_id=(SELECT zone_id FROM z WHERE channel=1)
ORDER BY m.ts
SQL
