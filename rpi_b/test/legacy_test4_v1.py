#!/usr/bin/env python3
"""
Phase 4: config epoch mechanism (spec SS4).

    python3 phase4_epoch.py

PRECONDITION: video loop RUNNING for >=2 min, so flow rates are non-zero.
A rate that is already 0 cannot demonstrate "reset to 0".

Mutates camera state (bumps config_version). Run Phase 3 first.
"""
import json
import os
import subprocess
import sys
import time

CAM = os.environ.get("CAM_IP", "172.20.33.201")
USER = os.environ.get("CAM_USER", "admin")
PW = os.environ.get("CAM_PW", "qkdwnsgks123!")
BASE = f"https://{CAM}/opensdk/juan_application"
RULE = os.environ.get("LINE_RULE", "name1")

fails, inconclusive = [], []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f"  -- {detail}" if detail else ""))
    if not cond:
        fails.append(name)
    return cond


def skip(name, why):
    print(f"  [SKIP] {name}  -- {why}")
    inconclusive.append(name)


def get(path):
    r = subprocess.run(["curl", "-sSk", "--digest", "-u", f"{USER}:{PW}", "-m", "10",
                        f"{BASE}{path}"], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"curl {path}: {r.stderr.strip()}")
    return json.loads(r.stdout)


def snap():
    p, e = get("/prediction"), get("/events")
    fl = p.get("flow", {})
    return {
        "ver": p.get("config_version"),
        "ev_ver": e.get("config_version"),
        "det_ver": get("/detections?since=2099-01-01T00:00:00Z").get("config_version"),
        "occ": p.get("occupancy_direct"),
        "from_flow": p.get("occupancy_from_flow"),
        "r_rate": fl.get("right", {}).get("rate_per_min", 0.0),
        "l_rate": fl.get("left", {}).get("rate_per_min", 0.0),
        "r_ewma": fl.get("right", {}).get("ewma", 0.0),
        "l_ewma": fl.get("left", {}).get("ewma", 0.0),
        "r_count": fl.get("right", {}).get("count", 0),
        "l_count": fl.get("left", {}).get("count", 0),
        "ev_counts": {f"{r['rule']}/{r['action']}": r["count"] for r in e.get("rules", [])},
    }


def show(tag, s):
    print(f"  {tag:6} ver={s['ver']}  occ={s['occ']}  from_flow={s['from_flow']}  "
          f"rate R={s['r_rate']:.2f} L={s['l_rate']:.2f}  "
          f"ewma R={s['r_ewma']:.2f} L={s['l_ewma']:.2f}  "
          f"count R={s['r_count']} L={s['l_count']}")


# ---------------------------------------------------------------- preconditions
print("=== Phase 4: config epoch ===\n--- preconditions ---")
before = snap()
show("BEFORE", before)

rates_live = (before["r_rate"] > 0.1 or before["l_rate"] > 0.1)
if not check("flow rates non-zero before bump (else reset test is vacuous)",
             rates_live, f"R={before['r_rate']} L={before['l_rate']}"):
    print("\n  -> start the video loop and let it run ~2 min, then re-run.")
    sys.exit(1)

occ_live = (before["occ"] or 0) > 0
ff_live = (before["from_flow"] or 0) > 0

V = before["ver"]
print(f"\n--- bumping {V} -> {V + 1} ---")
r = get(f"/config?version={V + 1}")
check("/config?version=N+1 returns config_version=N+1", r.get("config_version") == V + 1,
      f"got {r.get('config_version')}")
check("/config response carries served_utc", "served_utc" in r)

time.sleep(2)
after = snap()
show("AFTER", after)

print("\n--- epoch propagation (SS4.3) ---")
check("/prediction tags new version", after["ver"] == V + 1, f"got {after['ver']}")
check("/events tags new version", after["ev_ver"] == V + 1, f"got {after['ev_ver']}")
check("/detections tags new version", after["det_ver"] == V + 1, f"got {after['det_ver']}")

print("\n--- epoch isolation ---")
check("flow rates reset to ~0 (current-version buckets only)",
      after["r_rate"] < 0.5 and after["l_rate"] < 0.5,
      f"R={after['r_rate']} L={after['l_rate']}")

if ff_live:
    check("occupancy_from_flow resets to 0", after["from_flow"] == 0,
          f"got {after['from_flow']}")
else:
    skip("occupancy_from_flow resets to 0",
         f"was already {before['from_flow']} before bump (clamp) -- cannot fail")

if occ_live:
    check("occupancy_direct UNaffected by epoch (instantaneous obs)",
          after["occ"] == before["occ"], f"{before['occ']} -> {after['occ']}")
else:
    skip("occupancy_direct unaffected by epoch",
         "frame empty (occ=0 both sides) -- cannot fail")

print("\n--- backcompat: cumulative counters must NOT reset (SS4.3, intentional) ---")
check("/events cumulative counts survive epoch",
      after["ev_counts"].get(f"{RULE}/Right", -1) >= before["ev_counts"].get(f"{RULE}/Right", 0)
      and after["ev_counts"].get(f"{RULE}/Left", -1) >= before["ev_counts"].get(f"{RULE}/Left", 0),
      f"R {before['ev_counts'].get(f'{RULE}/Right')} -> {after['ev_counts'].get(f'{RULE}/Right')}, "
      f"L {before['ev_counts'].get(f'{RULE}/Left')} -> {after['ev_counts'].get(f'{RULE}/Left')}")

# ------------------------------------------------------- rebuild in the new epoch
print("\n--- 4.8: rebuild in new epoch (waiting 75s, loop must be running) ---")
time.sleep(75)
reb = snap()
show("REBUILD", reb)
check("flow rate rebuilds in new epoch", reb["r_rate"] > 0.1 or reb["l_rate"] > 0.1,
      f"R={reb['r_rate']} L={reb['l_rate']}")
check("rebuild stays tagged N+1", reb["ver"] == V + 1)

# --------------------------------------------- TASK-15 precondition: version jump
# The poller's planned reinject (camera restart -> version drops to 1 -> B pushes
# its stored value) requires the camera to accept a NON-SEQUENTIAL jump. If it
# silently ignores one, that ~5-line fix will not work. Cheap to learn now.
print("\n--- TASK-15 precondition: does the camera accept a version JUMP? ---")
target = V + 10
j = get(f"/config?version={target}")
check(f"/config accepts non-sequential jump ({V + 1} -> {target})",
      j.get("config_version") == target, f"got {j.get('config_version')}")
time.sleep(1)
check("jump propagates to /prediction", snap()["ver"] == target)

# ---------------------------------------------------------------------- summary
print("\n" + "=" * 62)
if inconclusive:
    print(f"{len(inconclusive)} INCONCLUSIVE:")
    for i in inconclusive:
        print(f"  - {i}")
if fails:
    print(f"{len(fails)} FAILED:")
    for f in fails:
        print(f"  - {f}")
    sys.exit(1)
print(f"Phase 4 epoch mechanism verified. config_version now {target}.")
print("NOTE: Phase 5 (restart) will reset this to 1 -- that is TASK-15, expected.")
sys.exit(2 if inconclusive else 0)