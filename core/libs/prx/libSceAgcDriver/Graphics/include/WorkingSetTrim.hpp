#pragma once

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <vector>
#endif

// Removes the resident, unlocked pages of [begin, end) from this process's working set. Pages a
// guest (or anything else) locked with VirtualLock are skipped and stay locked: VirtualUnlock is
// called only on runs of pages QueryWorkingSetEx reports resident and not locked. Contents,
// protections, mappings and write-watch state are untouched; a trimmed page faults back in on use.
namespace AgcDriver::Graphics::WorkingSetTrim {

struct Result {
    std::uint64_t queriedBytes = 0;   // committed bytes examined
    std::uint64_t lockedBytes = 0;    // resident locked pages skipped
    std::uint64_t requestedBytes = 0; // resident unlocked pages VirtualUnlock was asked to release
    std::uint64_t expectedNotLocked = 0; // VirtualUnlock calls ending in ERROR_NOT_LOCKED (the documented release case)
    std::uint64_t unexpectedErrors = 0;  // any other failure
    std::uint32_t lastError = 0;
};

#ifdef _WIN32
inline Result TrimRange(std::uintptr_t begin, std::uintptr_t end) {
    constexpr std::uintptr_t page = 4096;
    constexpr std::uint64_t ValidBit = 1ull;
    constexpr std::uint64_t LockedBit = 1ull << 22u;
    Result result;
    begin &= ~(page - 1);
    end = (end + page - 1) & ~(page - 1);
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> info;
    const auto release = [&](std::uintptr_t first, std::uintptr_t last) {
        if (first >= last) return;
        result.requestedBytes += last - first;
        if (VirtualUnlock(reinterpret_cast<void*>(first), static_cast<SIZE_T>(last - first))) return;
        const auto error = GetLastError();
        if (error == ERROR_NOT_LOCKED) ++result.expectedNotLocked;
        else {
            ++result.unexpectedErrors;
            result.lastError = error;
        }
    };
    for (auto cursor = begin; cursor < end;) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) != sizeof(region)) break;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (regionEnd <= cursor) break; // no progress: stop rather than loop
        const auto stop = std::min(end, regionEnd);
        if (region.State == MEM_COMMIT) {
            const auto count = static_cast<std::size_t>((stop - cursor) / page);
            info.resize(count);
            for (std::size_t i = 0; i < count; ++i) info[i].VirtualAddress = reinterpret_cast<void*>(cursor + i * page);
            result.queriedBytes += stop - cursor;
            if (count != 0 && K32QueryWorkingSetEx(GetCurrentProcess(), info.data(), static_cast<DWORD>(count * sizeof(info[0])))) {
                std::uintptr_t runStart = 0;
                bool inRun = false;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto attributes = static_cast<std::uint64_t>(info[i].VirtualAttributes.Flags);
                    const auto address = cursor + i * page;
                    const bool resident = (attributes & ValidBit) != 0;
                    const bool locked = (attributes & LockedBit) != 0;
                    if (resident && locked) result.lockedBytes += page;
                    const bool eligible = resident && !locked;
                    if (eligible && !inRun) {
                        runStart = address;
                        inRun = true;
                    } else if (!eligible && inRun) {
                        release(runStart, address);
                        inRun = false;
                    }
                }
                if (inRun) release(runStart, stop);
            } else if (count != 0) {
                ++result.unexpectedErrors;
                result.lastError = GetLastError();
            }
        }
        cursor = stop;
    }
    return result;
}
#endif

}
