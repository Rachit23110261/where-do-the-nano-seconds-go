// tsc.hpp - cycle-accurate timestamps for the flight recorder.
//
// tsc()           : rdtscp (waits for earlier instructions to finish -> the
//                   timestamp is not taken "too early" by out-of-order execution)
// calibrate_tsc() : TSC cycles per nanosecond, measured against CLOCK_MONOTONIC
// tsc_overhead()  : cost of back-to-back tsc() calls (report it with results)
// cpu_has_flag()  : check /proc/cpuinfo for constant_tsc / nonstop_tsc
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <chrono>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace tscx {

inline uint64_t mono_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

inline uint64_t tsc() {
#if defined(__x86_64__)
    unsigned aux;
    return __rdtscp(&aux);
#else
    return mono_ns();            // fallback: 1 "cycle" == 1 ns
#endif
}

// Cycles per ns. Measures TSC ticks across a sleep of `ms` milliseconds.
inline double calibrate_tsc(int ms = 200) {
#if defined(__x86_64__)
    const uint64_t n0 = mono_ns(), c0 = tsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    const uint64_t n1 = mono_ns(), c1 = tsc();
    return double(c1 - c0) / double(n1 - n0);
#else
    (void)ms; return 1.0;
#endif
}

struct Overhead { uint64_t min_cycles; double mean_cycles; };

// Back-to-back cost of tsc(): the smallest interval you can meaningfully measure.
inline Overhead tsc_overhead(int iters = 10'000'000) {
    uint64_t mn = UINT64_MAX, sum = 0;
    for (int i = 0; i < iters; ++i) {
        const uint64_t a = tsc(), b = tsc();
        const uint64_t d = b - a;
        if (d < mn) mn = d;
        sum += d;
    }
    return {mn, double(sum) / iters};
}

inline bool cpu_has_flag(const char* flag) {
    FILE* f = std::fopen("/proc/cpuinfo", "r");
    if (!f) return false;
    char line[8192];
    bool found = false;
    const size_t fl = std::strlen(flag);
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, "flags", 5) != 0) continue;
        for (char* p = std::strstr(line, flag); p; p = std::strstr(p + 1, flag)) {
            const bool start_ok = (p == line) || p[-1] == ' ' || p[-1] == '\t';
            const char e = p[fl];
            if (start_ok && (e == ' ' || e == '\n' || e == '\0')) { found = true; break; }
        }
        break;                                  // first CPU's flags are enough
    }
    std::fclose(f);
    return found;
}

}  // namespace tscx

