#pragma once
// bench_sysstat.h - process CPU time and peak memory, without a dependency.
//
// A benchmark that reports latency but not cost is only half a benchmark:
// on the RPi C the interesting question is usually "can this run beside
// everything else", which is CPU and RSS, not milliseconds. Kept header-only
// and platform-split so the harness builds on the VMS PC (Windows) and on
// the Pi (Linux) with no extra packages.

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#endif

namespace bench {

struct SysStat {
    double cpu_user_s = 0;    ///< user CPU seconds consumed by this process
    double cpu_sys_s = 0;     ///< kernel CPU seconds
    double rss_peak_mb = 0;   ///< peak resident set size
    double rss_now_mb = 0;
};

inline SysStat sample()
{
    SysStat s;
#if defined(_WIN32)
    FILETIME create, exit, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) {
        auto to_s = [](const FILETIME &ft) {
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            return double(u.QuadPart) / 1e7;   // 100ns ticks
        };
        s.cpu_user_s = to_s(user);
        s.cpu_sys_s = to_s(kernel);
    }
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        s.rss_now_mb = double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        s.rss_peak_mb = double(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
#else
    // utime/stime are fields 14 and 15 of /proc/self/stat, but the process
    // name in field 2 can contain spaces and parentheses - so start
    // counting after the last ')'.
    if (FILE *f = std::fopen("/proc/self/stat", "r")) {
        char buf[4096];
        const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[n] = '\0';
        if (char *close = std::strrchr(buf, ')')) {
            long utime = 0, stime = 0;
            // fields after ")" start at 3; utime is 14, stime is 15
            int field = 2;
            for (char *tok = std::strtok(close + 1, " "); tok;
                 tok = std::strtok(nullptr, " ")) {
                ++field;
                if (field == 14) utime = std::atol(tok);
                else if (field == 15) { stime = std::atol(tok); break; }
            }
            const double hz = double(sysconf(_SC_CLK_TCK));
            if (hz > 0) {
                s.cpu_user_s = double(utime) / hz;
                s.cpu_sys_s = double(stime) / hz;
            }
        }
    }
    if (FILE *f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            long kb = 0;
            if (std::sscanf(line, "VmHWM: %ld kB", &kb) == 1)
                s.rss_peak_mb = double(kb) / 1024.0;
            else if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1)
                s.rss_now_mb = double(kb) / 1024.0;
        }
        std::fclose(f);
    }
#endif
    return s;
}

}  // namespace bench
