"""Shared measurement helpers for the GuardX audio benchmarks.

Deliberately stdlib-only. The RPi C venv already carries numpy, scipy and
the TFLite runtime for the detector itself; adding psutil or a stats package
just to time things would mean the benchmark cannot be run on a machine
where the detector can.
"""

import json
import os
import platform
import sys
import time


# --------------------------------------------------------------- statistics

def summarise(samples):
    """min/p50/p95/p99/max/mean over a list of floats. Empty -> zeros."""
    xs = sorted(float(x) for x in samples)
    if not xs:
        return dict(n=0, min=0.0, p50=0.0, p95=0.0, p99=0.0, max=0.0, mean=0.0)

    def at(q):
        i = int(q * (len(xs) - 1) + 0.5)
        return xs[min(i, len(xs) - 1)]

    return dict(n=len(xs), min=xs[0], max=xs[-1], mean=sum(xs) / len(xs),
                p50=at(0.50), p95=at(0.95), p99=at(0.99))


def print_stats(label, unit, s):
    if s["n"] == 0:
        print("  %-22s (측정값 없음)" % label)
        return
    print("  %-22s n=%5d  min %8.2f  p50 %8.2f  p95 %8.2f  p99 %8.2f  "
          "max %8.2f  %s"
          % (label, s["n"], s["min"], s["p50"], s["p95"], s["p99"], s["max"],
             unit))


# ------------------------------------------------------------- process cost

def sysstat():
    """CPU seconds and RSS of this process. Linux reads /proc; elsewhere it
    falls back to what the stdlib can offer, which is CPU only."""
    out = dict(cpu_user_s=0.0, cpu_sys_s=0.0, rss_now_mb=0.0, rss_peak_mb=0.0)
    try:
        import resource   # POSIX only
        ru = resource.getrusage(resource.RUSAGE_SELF)
        out["cpu_user_s"] = float(ru.ru_utime)
        out["cpu_sys_s"] = float(ru.ru_stime)
        # ru_maxrss is kB on Linux, bytes on macOS.
        scale = 1024.0 if platform.system() == "Linux" else 1024.0 * 1024.0
        out["rss_peak_mb"] = float(ru.ru_maxrss) / scale
    except ImportError:
        t = time.process_time()
        out["cpu_user_s"] = float(t)

    try:
        with open("/proc/self/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    out["rss_now_mb"] = float(line.split()[1]) / 1024.0
                elif line.startswith("VmHWM:"):
                    out["rss_peak_mb"] = float(line.split()[1]) / 1024.0
    except OSError:
        pass
    return out


def print_sysstat(s, wall_s):
    share = ((s["cpu_user_s"] + s["cpu_sys_s"]) / wall_s * 100.0) if wall_s else 0.0
    print("  %-22s %.2f s user + %.2f s sys  (%.1f %% of one core)"
          % ("cpu", s["cpu_user_s"], s["cpu_sys_s"], share))
    print("  %-22s %.1f MB now, %.1f MB peak"
          % ("rss", s["rss_now_mb"], s["rss_peak_mb"]))


# ----------------------------------------------------------------- reporting

def host_info():
    return dict(python=sys.version.split()[0], platform=platform.platform(),
                machine=platform.machine(), cpu_count=os.cpu_count())


def write_json(path, payload):
    payload = dict(payload)
    payload.setdefault("taken_at", time.strftime("%Y-%m-%dT%H:%M:%S"))
    payload.setdefault("host", host_info())
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
    print("  wrote %s" % path)
