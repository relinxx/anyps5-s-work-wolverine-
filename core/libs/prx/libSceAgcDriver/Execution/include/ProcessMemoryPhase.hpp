#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace AgcDriver {

// Process totals bracket an operation; concurrent threads may contribute to the
// delta. They are measurements, not per-object allocation ownership claims.
class ProcessMemoryPhase {
public:
    ProcessMemoryPhase(bool enabled, const char* operation, std::uint64_t bytes,
                       std::uint64_t detail) : enabled(enabled), operation(operation),
        bytes(bytes), detail(detail) {
        if (!enabled) return;
        static std::atomic<std::uint64_t> sequence{0};
        id = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        start = std::chrono::steady_clock::now();
        before = sample();
        print("begin", before, 0);
    }
    ~ProcessMemoryPhase() {
        if (!enabled) return;
        const auto after = sample();
        print("end", after, std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now()-start).count());
    }
    ProcessMemoryPhase(const ProcessMemoryPhase&) = delete;
    ProcessMemoryPhase& operator=(const ProcessMemoryPhase&) = delete;
private:
    struct Sample { bool valid = false; std::uint64_t privateBytes = 0, workingSet = 0; };
    static Sample sample() noexcept {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
            return {true, counters.PrivateUsage, counters.WorkingSetSize};
#endif
        return {};
    }
    void print(const char* phase, Sample value, double elapsed) const noexcept {
        std::fprintf(stderr, "[process-memory-phase] id=%llu phase=%s operation=%s bytes=%llu detail=0x%llx valid=%d private=%llu working_set=%llu before_valid=%d before_private=%llu elapsed_ms=%.3f concurrent_process_total=1\n",
            static_cast<unsigned long long>(id), phase, operation,
            static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(detail),
            value.valid, static_cast<unsigned long long>(value.privateBytes),
            static_cast<unsigned long long>(value.workingSet), before.valid,
            static_cast<unsigned long long>(before.privateBytes), elapsed);
    }
    bool enabled;
    const char* operation;
    std::uint64_t bytes, detail, id = 0;
    Sample before;
    std::chrono::steady_clock::time_point start;
};

}
