#!/usr/bin/env python3
"""
GuardX camera-side contract test — juan_application MetadataProbe v12
Phases 0-2 (automatic). Phases 3-5 are manual; see CHECKLIST.md.

Usage:
    python3 test_camera_v12.py
    CAM_IP=172.20.33.201 CAM_PW='...' python3 test_camera_v12.py

Exit 0 = all pass. Exit 1 = at least one FAIL.
"""
import json
import os
import subprocess
import sys
import urllib.parse
from datetime import datetime, timezone

CAM = os.environ.get("CAM_IP", "172.20.33.201")
USER = os.environ.get("CAM_USER", "admin")
PW = os.environ.get("CAM_PW", "qkdwnsgks123!")
INSECURE = os.environ.get("CAM_INSECURE", "1") == "1"
BASE = f"https://{CAM}/opensdk/juan_application"

FRAME_W, FRAME_H = 2592, 1520
HORIZONS = {1, 5, 10}

fails = []
inconclusive = []


def check(name, cond, detail=""):
    mark = "PASS" if cond else "FAIL"
    print(f"  [{mark}] {name}" + (f"  -- {detail}" if detail else ""))
    if not cond:
        fails.append(name)
    return cond


def skip(name, why):
    """A check whose inputs cannot make it fail. Never report this as PASS."""
    print(f"  [SKIP] {name}  -- {why}")
    inconclusive.append(name)


def get(path):
    """Returns (http_code, body). Raises on curl failure."""
    cmd = ["curl", "-sS", "--digest", "-u", f"{USER}:{PW}",
           "-m", "20", "-w", "\n%{http_code}", f"{BASE}{path}"]
    if INSECURE:
        cmd.insert(1, "-k")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"curl failed for {path}: {r.stderr.strip()}")
    body, _, code = r.stdout.rpartition("\n")
    return int(code), body


def get_json(path):
    code, body = get(path)
    if code != 200:
        raise RuntimeError(f"{path} -> HTTP {code}")
    return json.loads(body)


def parse_ts(s):
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def now_utc():
    return datetime.now(timezone.utc)


# --------------------------------------------------------------------------
def phase0():
    print("\n=== Phase 0: preflight ===")
    json_eps = ["/metadata", "/events", "/detections", "/prediction",
                "/bestshots", "/stats"]
    raw_eps = ["/capture", "/samples"]  # JSONL / XML-bearing, code check only

    for ep in json_eps:
        try:
            code, body = get(ep)
            ok = check(f"{ep} HTTP 200", code == 200, f"got {code}")
            if ok:
                json.loads(body)
                check(f"{ep} parses as JSON", True)
        except json.JSONDecodeError as e:
            check(f"{ep} parses as JSON", False, str(e))
        except Exception as e:
            check(f"{ep} reachable", False, str(e))

    for ep in raw_eps:
        try:
            code, _ = get(ep)
            check(f"{ep} HTTP 200", code == 200, f"got {code}")
        except Exception as e:
            check(f"{ep} reachable", False, str(e))


# --------------------------------------------------------------------------
def phase1():
    print("\n=== Phase 1: static contract ===")

    # ---- /metadata
    m = get_json("/metadata")
    check("/metadata has people_count", "people_count" in m)
    check("/metadata has objects[]", isinstance(m.get("objects"), list))
    check("/metadata has frame_utc", "frame_utc" in m)
    if "frame_utc" in m:
        skew = abs((now_utc() - parse_ts(m["frame_utc"])).total_seconds())
        check("/metadata frame_utc is fresh & UTC (<5s skew)", skew < 5,
              f"skew {skew:.1f}s")
    if isinstance(m.get("objects"), list):
        humans = [o for o in m["objects"] if o.get("type") == "Human"]
        check("/metadata people_count == Human object count",
              m.get("people_count") == len(humans),
              f"{m.get('people_count')} vs {len(humans)}")

    # ---- /events
    e = get_json("/events")
    check("/events has rules[]", isinstance(e.get("rules"), list))
    check("/events has debounced_pairs", isinstance(e.get("debounced_pairs"), int))
    check("/events has config_version", isinstance(e.get("config_version"), int))
    for r in e.get("rules", []):
        need = {"topic", "rule", "action", "count", "rate_per_min", "ewma_per_min"}
        missing = need - set(r)
        if not check(f"/events rule {r.get('rule')}/{r.get('action')} full schema",
                     not missing, f"missing {missing}"):
            break

    # ---- /detections (full ring)
    d = get_json("/detections")
    dets = d.get("detections", [])
    check("/detections count == len(detections)", d.get("count") == len(dets),
          f"{d.get('count')} vs {len(dets)}")
    check("/detections has ring_size diag", "ring_size" in d)
    check("/detections has config_version", isinstance(d.get("config_version"), int))

    if not dets:
        print("  NOTE: ring empty — walk in front of the camera, then re-run.")
    else:
        need = {"channel", "object_id", "category", "likelihood",
                "rect_sx", "rect_sy", "rect_ex", "rect_ey", "x", "y", "ts"}
        missing = set()
        for det in dets:
            missing |= (need - set(det))
        check("/detections every row has full schema", not missing,
              f"missing {missing}")

        bad_box = [d_ for d_ in dets
                   if not (d_["rect_sx"] <= d_["rect_ex"] and d_["rect_sy"] <= d_["rect_ey"])]
        check("/detections rect_s* <= rect_e*", not bad_box, f"{len(bad_box)} bad")

        oob = [d_ for d_ in dets
               if not (0 <= d_["x"] <= FRAME_W and 0 <= d_["y"] <= FRAME_H)]
        check(f"/detections CoG within {FRAME_W}x{FRAME_H}", not oob,
              f"{len(oob)} out of bounds")

        # KST contamination defence (v12): no ts may be in the future
        n = now_utc()
        future = [d_ for d_ in dets if (parse_ts(d_["ts"]) - n).total_seconds() > 60]
        check("/detections no future ts (KST normalization holding)", not future,
              f"{len(future)} future rows")

        ordered = all(parse_ts(dets[i]["ts"]) <= parse_ts(dets[i + 1]["ts"])
                      for i in range(len(dets) - 1))
        check("/detections ts monotonically non-decreasing", ordered)

    # ---- /prediction
    p = get_json("/prediction")
    for k in ("occupancy_direct", "occupancy_from_flow", "flow",
              "net_rate_per_min", "predictions", "config_version",
              "direction_mapping"):
        check(f"/prediction has {k}", k in p)

    preds = p.get("predictions", [])
    check("/prediction has exactly 3 horizons {1,5,10}",
          {x.get("horizon_min") for x in preds} == HORIZONS,
          str([x.get("horizon_min") for x in preds]))
    check("/prediction all occupancies >= 0 (clamp holds)",
          all(x.get("occupancy", -1) >= 0 for x in preds))

    o = p.get("occupancy_direct")
    net = p.get("net_rate_per_min")
    ff = p.get("occupancy_from_flow")

    # The clamp makes several assertions unfalsifiable when net is strongly
    # negative: max(0, x) == 0 holds no matter what the extrapolation computes.
    # Report those as SKIP, never PASS.
    clamped = (isinstance(o, (int, float)) and isinstance(net, (int, float))
               and all(o + net * x["horizon_min"] <= 0 for x in preds))

    if ff == 0 and isinstance(net, (int, float)) and net < 0:
        skip("/prediction occupancy_from_flow >= 0",
             "pinned at clamp (net<0) -- cannot fail, verifies nothing")
    else:
        check("/prediction occupancy_from_flow >= 0 (clamp holds)", ff >= 0)

    # formula: o_hat(t+h) = max(0, o(t) + net_rate * h)
    if not isinstance(o, (int, float)) or not isinstance(net, (int, float)):
        skip("/prediction extrapolation", "occupancy_direct/net_rate not numeric")
    elif clamped:
        skip("/prediction extrapolation formula",
             f"all horizons clamp to 0 (o={o}, net={net}/min) -- inputs cannot "
             f"exercise o+net*h")
        print(f"         To make this testable you need net*10 > -{o}, i.e. roughly "
              f"balanced or inbound flow.")
        print(f"         A looping video pins net negative forever. Use live "
              f"subjects (Phase 3).")
    else:
        for x in preds:
            h = x["horizon_min"]
            expect = max(0.0, o + net * h)
            got = x["occupancy"]
            check(f"/prediction h={h} matches o+net*h", abs(got - expect) <= 0.51,
                  f"got {got}, expect {expect:.2f}")

    # dual-occupancy self-diagnostic (informational, not a pass/fail)
    if isinstance(p.get("occupancy_direct"), (int, float)):
        print(f"  INFO: occupancy direct={p['occupancy_direct']} "
              f"from_flow={p.get('occupancy_from_flow')} "
              f"delta={p['occupancy_direct'] - p.get('occupancy_from_flow', 0)} "
              f"(delta = live line accuracy indicator)")

    check("/prediction direction_mapping present",
          p.get("direction_mapping") in ("unset", "in_right", "in_left"),
          str(p.get("direction_mapping")))

    # ?line= filter must not 500 and must still carry the schema
    rules = [r["rule"] for r in e.get("rules", []) if r.get("topic", "").find("Line") >= 0]
    if rules:
        pl = get_json(f"/prediction?line={urllib.parse.quote(rules[0])}")
        check(f"/prediction?line={rules[0]} responds with schema",
              "predictions" in pl and "flow" in pl)
    else:
        print("  NOTE: no LineCrossing rule in /events — skipping ?line= test.")


# --------------------------------------------------------------------------
def phase2():
    print("\n=== Phase 2: /detections incremental semantics ===")
    full = get_json("/detections")
    dets = full.get("detections", [])
    if not dets:
        check("ring has data for incremental test", False,
              "walk in front of camera and re-run")
        return

    last_ts = dets[-1]["ts"]

    # strict greater-than: the anchor ts itself must be excluded
    r = get_json(f"/detections?since={urllib.parse.quote(last_ts)}")
    same = [x for x in r.get("detections", []) if x["ts"] == last_ts]
    check("since=<ts> uses strict > (anchor ts excluded)", not same,
          f"{len(same)} rows echoed the anchor")
    check("since_echo diagnostic present", "since_echo" in r)
    check("since_ms diagnostic present", "since_ms" in r)

    # millisecond normalization: .7 must mean 700ms, not 7ms
    if "." in last_ts:
        head, _, tail = last_ts.partition(".")
        ms = tail.rstrip("Z")
        if len(ms) == 3 and ms.endswith("00"):
            short = f"{head}.{ms[0]}Z"   # e.g. .700Z -> .7Z
            a = get_json(f"/detections?since={urllib.parse.quote(last_ts)}")
            b = get_json(f"/detections?since={urllib.parse.quote(short)}")
            check("ms normalization: .N == .N00", a["since_ms"] == b["since_ms"],
                  f"{a['since_ms']} vs {b['since_ms']}")
        else:
            # synthesize an anchor we control
            anchor = f"{head}.700Z"
            a = get_json(f"/detections?since={urllib.parse.quote(anchor)}")
            b = get_json(f"/detections?since={urllib.parse.quote(head + '.7Z')}")
            check("ms normalization: .7 == .700", a["since_ms"] == b["since_ms"],
                  f"{a['since_ms']} vs {b['since_ms']}")

    # %3A colon encoding must decode identically
    enc = last_ts.replace(":", "%3A")
    c = get_json(f"/detections?since={enc}")
    check("%3A colon encoding accepted", c.get("since_ms") == r.get("since_ms"),
          f"{c.get('since_ms')} vs {r.get('since_ms')}")

    # far future -> empty
    fut = get_json("/detections?since=2099-01-01T00:00:00.000Z")
    check("since far-future returns 0 rows", fut.get("count") == 0,
          f"got {fut.get('count')}")

    # far past -> whole ring (ring may have advanced, so >= is the honest check)
    past = get_json("/detections?since=2000-01-01T00:00:00.000Z")
    check("since far-past returns whole ring", past.get("count") >= len(dets) - 5,
          f"{past.get('count')} vs full {len(dets)}")

    # no param == far past (ring drift tolerance)
    check("no param ~= full ring", abs(past.get("count", 0) - len(dets)) <= 10,
          f"{past.get('count')} vs {len(dets)}")


# --------------------------------------------------------------------------
if __name__ == "__main__":
    print(f"Target: {BASE}  (insecure={INSECURE})")
    try:
        phase0()
        if fails:
            print("\nPreflight failed — stopping before contract tests.")
            sys.exit(1)
        phase1()
        phase2()
    except Exception as ex:
        print(f"\nABORT: {ex}")
        sys.exit(1)

    print("\n" + "=" * 60)
    if inconclusive:
        print(f"{len(inconclusive)} INCONCLUSIVE (not verified, not failed):")
        for i in inconclusive:
            print(f"  - {i}")
    if fails:
        print(f"{len(fails)} FAILED:")
        for f in fails:
            print(f"  - {f}")
        sys.exit(1)
    if inconclusive:
        print("\nNo failures, but the above were not exercised. Do NOT read this "
              "run as full camera-side coverage.")
        sys.exit(2)
    print("Phases 0-2 all pass. Proceed to Phase 3 (live) in CHECKLIST.md.")
    sys.exit(0)