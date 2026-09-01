#!/usr/bin/env python3
"""Phase C — 2-zone pilot analyses over rpi_b's Postgres (zone_occupancy, line_flow).

Subcommands (each prints a self-contained text report):
  conservation   Δocc vs net metered inflow, per zone  -> instrument validation
  dup            z1z2boundarydup vs z2z1boundary agreement, per direction
  leadlag        boundary inflow -> Z1 occupancy change, r by lag (coupling gate)
  little         arrivals x dwell (Little's law) by hour of day
  all            run everything

Zero pip dependencies: talks to Postgres by shelling out to `psql` (CSV mode).
Run on RPi B (local socket) or anywhere psql reaches the DB:
  python3 phase_c.py all
  python3 phase_c.py conservation --since 2026-07-29T09:03:00Z
  PGDATABASE=guardx python3 phase_c.py dup --psql "psql -h rpib.local -U pi"

Line-name -> direction mapping (walk test 2026-07-29, AFTER the CH2 name swap):
  z1entrance       Right = out->Z1     Left = Z1->out
  z2entrance       Right = Z2->out     Left = out->Z2
  z2z1boundary     Right = Z1->Z2      Left = Z2->Z1
  z1z2boundarydup  Right = Z1->Z2      Left = Z2->Z1   (NEVER summed; dup check only)

Data epoch: flow models reset + names fixed at 2026-07-29T09:02:55Z. Rows for the
two renamed CH2 rules from before that moment carry the OPPOSITE physical label —
the default --since excludes them. Do not analyze across that boundary.
"""
import argparse
import csv
import io
import math
import shlex
import statistics
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timedelta, timezone

EPOCH_DEFAULT = "2026-07-29T09:03:00Z"
KST = timezone(timedelta(hours=9))

# rule -> (meaning of Right, meaning of Left) as signed contribution per zone.
# +1 = into the zone, -1 = out of the zone, 0 = not a door of this zone.
#            rule              zone   Right  Left
DIRECTIONS = {
    ("z1entrance",      "Z1"): (+1, -1),
    ("z2z1boundary",    "Z1"): (-1, +1),
    ("z2entrance",      "Z2"): (-1, +1),
    ("z2z1boundary",    "Z2"): (+1, -1),
    # dup intentionally absent: excluded from all conservation sums.
}
BOUNDARY, DUP = "z2z1boundary", "z1z2boundarydup"
ENTRANCES_IN = [("z1entrance", "Right"), ("z2entrance", "Left")]  # into building


def run_psql(psql_cmd, sql):
    cmd = shlex.split(psql_cmd) + ["-X", "-q", "-A", "--csv", "-v", "ON_ERROR_STOP=1",
                                   "-c", sql]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"psql failed:\n{p.stderr.strip()}\n--- SQL ---\n{sql}")
    return list(csv.DictReader(io.StringIO(p.stdout)))


def parse_ts(s):
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def minute_of(ts):
    return int(ts.timestamp()) // 60


def fetch(args):
    """occ[zone][minute] = person_count ; flow[(rule,action)][minute] = count"""
    zrows = run_psql(args.psql, "SELECT zone_id, channel, zone_name FROM zones ORDER BY channel")
    if not zrows:
        sys.exit("zones table is empty")
    by_ch = {int(r["channel"]): int(r["zone_id"]) for r in zrows}
    if args.z1_channel not in by_ch or args.z2_channel not in by_ch:
        sys.exit(f"channels {args.z1_channel}/{args.z2_channel} not in zones "
                 f"(have: {sorted(by_ch)}) — pass --z1-channel/--z2-channel")
    zid = {"Z1": by_ch[args.z1_channel], "Z2": by_ch[args.z2_channel]}
    print(f"# zones: Z1=zone_id {zid['Z1']} (ch{args.z1_channel}), "
          f"Z2=zone_id {zid['Z2']} (ch{args.z2_channel}); since {args.since}\n")

    occ = {"Z1": {}, "Z2": {}}
    rows = run_psql(args.psql,
        f"SELECT zone_id, bucket_ts, person_count FROM zone_occupancy "
        f"WHERE bucket_ts >= '{args.since}' ORDER BY bucket_ts")
    inv = {v: k for k, v in zid.items()}
    for r in rows:
        z = inv.get(int(r["zone_id"]))
        if z:
            occ[z][minute_of(parse_ts(r["bucket_ts"]))] = int(r["person_count"])

    flow = defaultdict(dict)   # (rule, action) -> minute -> count
    rows = run_psql(args.psql,
        f"SELECT rule, action, bucket_ts, flow_count FROM line_flow "
        f"WHERE bucket_ts >= '{args.since}' ORDER BY bucket_ts")
    for r in rows:
        flow[(r["rule"], r["action"])][minute_of(parse_ts(r["bucket_ts"]))] = int(r["flow_count"])
    return occ, flow


def net_inflow(flow, zone, minute):
    """Metered net inflow for `zone` at `minute` (missing row = 0 crossings)."""
    s = 0
    for (rule, z), (w_right, w_left) in DIRECTIONS.items():
        if z != zone:
            continue
        s += w_right * flow.get((rule, "Right"), {}).get(minute, 0)
        s += w_left * flow.get((rule, "Left"), {}).get(minute, 0)
    return s


def corr(xs, ys):
    if len(xs) < 3:
        return float("nan")
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    if sx == 0 or sy == 0:
        return float("nan")
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / (sx * sy)


# --------------------------------------------------------------- conservation
def cmd_conservation(occ, flow):
    print("=" * 66)
    print("C1. CONSERVATION — Δocc(t) vs metered net inflow(t)  (dup excluded)")
    print("=" * 66)
    for zone in ("Z1", "Z2"):
        mins = sorted(occ[zone])
        pairs = [(m, occ[zone][m] - occ[zone][m - 1]) for m in mins if m - 1 in occ[zone]]
        if len(pairs) < 30:
            print(f"\n[{zone}] only {len(pairs)} consecutive minutes — need more data")
            continue
        # bucket alignment between occupancy and flow is not guaranteed to be
        # exact; try small shifts and report the best, so a fixed off-by-one
        # doesn't masquerade as instrument error.
        best = None
        for shift in (-2, -1, 0, 1, 2):
            d = [dy for _, dy in pairs]
            f = [net_inflow(flow, zone, m + shift) for m, _ in pairs]
            r = corr(f, d)
            if best is None or (not math.isnan(r) and r > best[1]):
                best = (shift, r, d, f)
        shift, r, d, f = best
        resid = [dy - fl for dy, fl in zip(d, f)]
        n = len(resid)
        drift_h = 60.0 * statistics.fmean(resid)
        mae = statistics.fmean(abs(x) for x in resid)
        exact = sum(1 for x in resid if x == 0) / n
        print(f"\n[{zone}] n={n} min | best shift {shift:+d} min | corr(Δocc, inflow) = {r:.3f}")
        print(f"  residual: mean {statistics.fmean(resid):+.3f}/min "
              f"(drift {drift_h:+.1f} 명/h), MAE {mae:.3f}, exact-zero {100*exact:.0f}%")
        print(f"  cumulative drift over span: {sum(resid):+d} 명")
        worst = sorted(((abs(dy - fl), m, dy, fl) for (m, dy), fl in zip(pairs, f)),
                       reverse=True)[:5]
        print("  worst minutes (|Δocc − inflow|):")
        for a, m, dy, fl in worst:
            t = datetime.fromtimestamp(m * 60, KST).strftime("%m-%d %H:%M")
            print(f"    {t} KST  Δocc={dy:+d}  inflow={fl:+d}  resid={dy-fl:+d}")
        # sign convention verified on synthetic data with a planted 10% inbound
        # miss -> drift came out POSITIVE (occupancy rises without metered inflow)
        print("  READ: corr>0.6 & drift ~0 → both instruments healthy. POSITIVE "
              "drift → inbound undercount (진입 미검지: 유입이 장부에 안 잡힘). "
              "NEGATIVE drift → outbound undercount. Seam double-vision shows as "
              "high MAE with ~0 drift, not as drift.")


# ------------------------------------------------------------------ dup check
def cmd_dup(occ, flow):
    print("=" * 66)
    print(f"C2. REDUNDANCY — {DUP} (CH1) vs {BOUNDARY} (CH2), same passage")
    print("=" * 66)
    for action, label in (("Right", "Z1→Z2"), ("Left", "Z2→Z1")):
        a = flow.get((DUP, action), {})
        b = flow.get((BOUNDARY, action), {})
        ta, tb = sum(a.values()), sum(b.values())
        mins = set(a) | set(b)
        agree = sum(min(a.get(m, 0), b.get(m, 0)) for m in mins)
        union = sum(max(a.get(m, 0), b.get(m, 0)) for m in mins)
        j = agree / union if union else float("nan")
        print(f"\n[{label}] dup={ta}  boundary={tb}  "
              f"ratio={ta/tb if tb else float('nan'):.2f}  minute-Jaccard={j:.2f}")
    print("\n  READ: ratio≈1 & Jaccard≥0.7 → pair healthy; use disagreement rate "
          "as the line-crossing error floor for C1 residual interpretation. "
          "ratio≪1 in one direction → that camera still misses that direction "
          "(라인 위치 재조정). dup stays OUT of all sums regardless.")


# -------------------------------------------------------------------- leadlag
def cmd_leadlag(occ, flow):
    print("=" * 66)
    print("C3. LEAD-LAG — boundary flow into Z1 (t) vs Z1 occupancy change (t..t+k)")
    print("=" * 66)
    z1 = occ["Z1"]
    mins = sorted(z1)
    # inflow to Z1 through the boundary only (coupling signal, entrances excluded)
    binf = {m: flow.get((BOUNDARY, "Left"), {}).get(m, 0)
               - flow.get((BOUNDARY, "Right"), {}).get(m, 0) for m in mins}
    nz = [m for m in mins if binf[m] != 0]
    print(f"minutes={len(mins)}, boundary-active minutes={len(nz)}")
    if len(nz) < 20:
        print("too few boundary crossings so far — rerun after more traffic")
        return
    print(f"\n{'lag k':>6} {'corr(binf(t), occ(t+k)-occ(t))':>32} {'n':>7}")
    peak = (0, 0.0)
    for k in (1, 2, 3, 5, 8, 10, 15, 20, 30):
        xs, ys = [], []
        for m in mins:
            if m + k in z1:
                xs.append(binf[m])
                ys.append(z1[m + k] - z1[m])
        r = corr(xs, ys)
        if not math.isnan(r) and abs(r) > abs(peak[1]):
            peak = (k, r)
        print(f"{k:>6} {r:>32.3f} {len(xs):>7}")
    print(f"\n  peak |r| at k={peak[0]} (r={peak[1]:.3f})")
    print("  READ: this is the GO/NO-GO for the coupled (vector) deviation model. "
          "r≳0.3 sustained over k=1..10 → coupling worth modeling. r≈0 → zones "
          "weakly coupled; keep independent forecasters, skip Layer 2.")


# --------------------------------------------------------------------- little
def cmd_little(occ, flow):
    print("=" * 66)
    print("C4. LITTLE'S LAW — arrivals λ and implied dwell W = L/λ, by KST hour")
    print("=" * 66)
    both = sorted(set(occ["Z1"]) & set(occ["Z2"]))
    if len(both) < 60:
        print("not enough overlapping occupancy minutes yet")
        return
    by_hour = defaultdict(lambda: [0, 0.0, 0])          # hour -> [arrivals, occ_sum, n_min]
    for m in both:
        h = datetime.fromtimestamp(m * 60, KST).hour
        by_hour[h][0] += sum(flow.get(ra, {}).get(m, 0) for ra in ENTRANCES_IN)
        by_hour[h][1] += occ["Z1"][m] + occ["Z2"][m]
        by_hour[h][2] += 1
    print(f"\n{'KST hour':>8} {'λ(명/h)':>9} {'mean L':>8} {'W=L/λ (min)':>12} {'min':>6}")
    ws = []
    for h in sorted(by_hour):
        arr, osum, n = by_hour[h]
        lam_min = arr / n
        L = osum / n
        w = L / lam_min if lam_min > 0.05 else float("nan")   # skip dead hours
        if not math.isnan(w):
            ws.append(w)
        print(f"{h:>8} {60*lam_min:>9.1f} {L:>8.2f} {w:>12.1f} {n:>6}")
    if ws:
        print(f"\n  dwell W: median {statistics.median(ws):.1f} min "
              f"(spread {min(ws):.1f}–{max(ws):.1f})")
    print("  READ: stable, plausible W across busy hours → arrivals×dwell "
          "long-horizon model (Layer 3) is viable. Wildly drifting W → it isn't. "
          "NB: entrance-direction undercount inflates W — check C1 drift first.")


def main():
    if hasattr(sys.stdout, "reconfigure"):          # win cp949 consoles
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["conservation", "dup", "leadlag", "little", "all"])
    ap.add_argument("--since", default=EPOCH_DEFAULT,
                    help=f"data epoch, ISO8601 (default {EPOCH_DEFAULT} = reset moment)")
    ap.add_argument("--psql", default="psql", help="psql command incl. conn opts")
    ap.add_argument("--z1-channel", type=int, default=0, help="camera channel of Z1")
    ap.add_argument("--z2-channel", type=int, default=1, help="camera channel of Z2")
    args = ap.parse_args()

    occ, flow = fetch(args)
    steps = {"conservation": [cmd_conservation], "dup": [cmd_dup],
             "leadlag": [cmd_leadlag], "little": [cmd_little],
             "all": [cmd_conservation, cmd_dup, cmd_leadlag, cmd_little]}[args.cmd]
    for i, fn in enumerate(steps):
        if i:
            print()
        fn(occ, flow)


if __name__ == "__main__":
    main()
