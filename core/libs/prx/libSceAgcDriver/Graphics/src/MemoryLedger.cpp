#include "prx/libSceAgcDriver/Graphics/include/MemoryLedger.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MemoryAccounting.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include <algorithm>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace AgcDriver::Graphics::MemoryLedger {
namespace {

std::atomic<std::int64_t> textureSnapshotBytes{0};

constexpr std::uintptr_t GuestArenaStart = 0x0000000200000000ull;
constexpr std::uintptr_t GuestArenaEnd = 0x000000FC00000000ull;

#ifdef _WIN32
struct Region {
    std::uintptr_t begin;
    std::uintptr_t end;
    std::uintptr_t allocation;
    DWORD type;
    bool guestMappedPathKnown;
    bool guestFileBackedDirect;
};

// HeapSummary is Windows 10+; looked up so older headers need not declare it.
struct HeapSummaryInfo {
    DWORD cb;
    SIZE_T cbAllocated;
    SIZE_T cbCommitted;
    SIZE_T cbReserved;
    SIZE_T cbMaxReserve;
};
using HeapSummaryFunction = BOOL(WINAPI*)(HANDLE, DWORD, HeapSummaryInfo*);

std::atomic<std::uint64_t> lastVolumeFree{0};

std::int64_t intervalMs() {
    static const auto interval = [] {
        const char* text = std::getenv("APS5_MEMORY_LEDGER_INTERVAL_MS");
        if (text == nullptr) return std::int64_t{10000};
        char* end = nullptr;
        const auto value = std::strtoll(text, &end, 10);
        return end != text && *end == '\0' && value >= 1000 && value <= 60000 ? value : std::int64_t{10000};
    }();
    return interval;
}

// Free space of the volume holding the temp directory, where libkernel creates the sparse
// direct-memory backing files (DirectMemory.cpp SparseDirectBacking uses GetTempPathW).
bool backingVolumeFree(char* volumeName, std::size_t volumeBytes, std::uint64_t* freeBytes) {
    wchar_t temp[MAX_PATH + 1];
    const auto length = GetTempPathW(MAX_PATH + 1, temp);
    if (length == 0 || length > MAX_PATH) return false;
    wchar_t root[MAX_PATH + 1];
    if (!GetVolumePathNameW(temp, root, MAX_PATH + 1)) return false;
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(root, &available, nullptr, nullptr)) return false;
    *freeBytes = available.QuadPart;
    if (WideCharToMultiByte(CP_UTF8, 0, root, -1, volumeName, static_cast<int>(volumeBytes), nullptr, nullptr) == 0) std::snprintf(volumeName, volumeBytes, "unknown");
    return true;
}

// Low physical memory, low commit, or a drop of the backing volume's free space by more than
// 512 MiB since the last ledger line: the periodic 10 s cadence would miss these fast phases.
const char* pressure() {
    constexpr std::uint64_t Low = 2ull << 30u;
    MEMORYSTATUSEX status{sizeof(MEMORYSTATUSEX)};
    if (GlobalMemoryStatusEx(&status)) {
        if (status.ullAvailPhys < Low) return "physical_pressure";
        if (status.ullAvailPageFile < Low) return "commit_pressure";
    }
    char volume[MAX_PATH * 2];
    std::uint64_t free = 0;
    const auto last = lastVolumeFree.load(std::memory_order_relaxed);
    if (last != 0 && backingVolumeFree(volume, sizeof(volume), &free) && free + (512ull << 20u) < last) return "disk_drop";
    return nullptr;
}

void census(const char* reason) {
    std::vector<Region> regions;
    MEMORY_BASIC_INFORMATION info{};
    for (std::uintptr_t address = 0; VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == sizeof(info);) {
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.State == MEM_COMMIT) {
            bool guestMappedPathKnown = false;
            bool guestFileBackedDirect = false;
            if (base < GuestArenaEnd && base + info.RegionSize > GuestArenaStart && info.Type == MEM_MAPPED) {
                wchar_t mappedPath[1024]{};
                const auto chars = K32GetMappedFileNameW(GetCurrentProcess(), info.BaseAddress, mappedPath, static_cast<DWORD>(std::size(mappedPath)));
                if (chars != 0) {
                    guestMappedPathKnown = true;
                    guestFileBackedDirect = std::wcsstr(mappedPath, L"anyps5-dmem-") != nullptr;
                }
            }
            regions.push_back({base, base + info.RegionSize, reinterpret_cast<std::uintptr_t>(info.AllocationBase), info.Type, guestMappedPathKnown, guestFileBackedDirect});
        }
        const auto next = base + info.RegionSize;
        if (next <= address) break;
        address = next;
    }
    // The working-set list: one entry per resident page (bit 8 = shared, bits 12+ = page address).
    std::vector<ULONG_PTR> list(1u << 20u);
    while (!K32QueryWorkingSet(GetCurrentProcess(), list.data(), static_cast<DWORD>(list.size() * sizeof(ULONG_PTR)))) {
        if (GetLastError() != ERROR_BAD_LENGTH || list.size() >= (1u << 25u)) {
            std::fprintf(stderr, "[mem-ledger] QueryWorkingSet failed error=%lu\n", GetLastError());
            return;
        }
        list.resize(std::max<std::size_t>(list.size() * 2, static_cast<std::size_t>(list[0]) + 4096));
    }
    const auto count = static_cast<std::size_t>(list[0]);
    std::uint64_t guestPrivate = 0, guestShared = 0, guestMapped = 0, guestFileMapped = 0, guestMappedUnknown = 0, hostPrivate = 0, hostShared = 0, hostImage = 0, hostMapped = 0, unknown = 0;
    std::vector<std::pair<std::uintptr_t, std::uint64_t>> allocations;
    std::size_t cursor = 0;
    std::vector<std::uintptr_t> pages;
    pages.reserve(count);
    for (std::size_t i = 1; i <= count && i < list.size(); ++i) pages.push_back(list[i]);
    std::sort(pages.begin(), pages.end(), [](ULONG_PTR left, ULONG_PTR right) { return (left & ~0xfffull) < (right & ~0xfffull); });
    for (const auto entry : pages) {
        const auto page = static_cast<std::uintptr_t>(entry & ~0xfffull);
        const bool shared = (entry >> 8u) & 1u;
        while (cursor < regions.size() && regions[cursor].end <= page) ++cursor;
        if (cursor == regions.size() || page < regions[cursor].begin) {
            unknown += 4096;
            continue;
        }
        const auto& region = regions[cursor];
        if (page >= GuestArenaStart && page < GuestArenaEnd) {
            if (region.type == MEM_MAPPED) {
                if (!region.guestMappedPathKnown) guestMappedUnknown += 4096;
                else if (region.guestFileBackedDirect) guestFileMapped += 4096;
                else guestMapped += 4096;
            }
            else (shared ? guestShared : guestPrivate) += 4096;
            continue;
        }
        if (region.type == MEM_IMAGE) {
            hostImage += 4096;
        } else if (region.type == MEM_MAPPED) {
            hostMapped += 4096;
        } else if (shared) {
            hostShared += 4096;
        } else {
            hostPrivate += 4096;
            if (allocations.empty() || allocations.back().first != region.allocation) allocations.emplace_back(region.allocation, 0);
            allocations.back().second += 4096;
        }
    }
    std::sort(allocations.begin(), allocations.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    std::vector<std::pair<std::uintptr_t, std::uint64_t>> merged;
    for (const auto& item : allocations) {
        if (!merged.empty() && merged.back().first == item.first) merged.back().second += item.second;
        else merged.push_back(item);
    }
    std::sort(merged.begin(), merged.end(), [](const auto& left, const auto& right) { return left.second > right.second; });

    // Bytes handed out by every Windows heap of the process (the C runtime heap is the process
    // heap; other heaps belong to the loader, drivers and libraries). `heapsSummed` of `heapCount`
    // were summarized; a heap HeapSummary refused is reported, not counted as zero.
    static const auto summary = reinterpret_cast<HeapSummaryFunction>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary"));
    std::uint64_t heapAllocated = 0, heapCommitted = 0;
    DWORD heapCount = 0, heapsSummed = 0;
    std::vector<HANDLE> heaps(64);
    if (summary != nullptr) {
        for (;;) {
            heapCount = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
            if (heapCount <= heaps.size()) break;
            heaps.resize(heapCount + 16);
        }
        for (DWORD i = 0; i < heapCount; ++i) {
            HeapSummaryInfo heap{};
            heap.cb = sizeof(heap);
            if (!summary(heaps[i], 0, &heap)) continue;
            heapAllocated += heap.cbAllocated;
            heapCommitted += heap.cbCommitted;
            ++heapsSummed;
        }
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    const bool countersKnown = K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != 0;
    if (!countersKnown) std::fprintf(stderr, "[mem-ledger] process memory counters unknown (K32GetProcessMemoryInfo error=%lu); private commit and working set below are 0 placeholders\n", GetLastError());
    constexpr double MiB = 1024.0 * 1024.0;
    char top[512];
    int written = 0;
    for (std::size_t i = 0; i < merged.size() && i < 8 && written >= 0 && static_cast<std::size_t>(written) < sizeof(top); ++i) {
        written += std::snprintf(top + written, sizeof(top) - static_cast<std::size_t>(written), "%s0x%llx:%.0f", i == 0 ? "" : " ", static_cast<unsigned long long>(merged[i].first), merged[i].second / MiB);
    }
    if (written <= 0) top[0] = '\0';
    std::fprintf(stderr,
        "[mem-ledger] resident %.0f MiB: guest private %.0f shared %.0f mapped_dmem_file %.0f mapped_other %.0f mapped_path_unknown %.0f | host private %.0f (in %zu allocations) shared %.0f image %.0f mapped %.0f unknown %.0f | windows heaps allocated %.0f committed %.0f (HeapSummary over GetProcessHeaps: %lu of %lu heaps summarized; VirtualAlloc memory outside heaps is not included) | texture snapshots %.0f | private commit %.0f (heaps committed %.0f, remainder %.0f) | top host private MiB %s\n",
        count * 4096.0 / MiB, guestPrivate / MiB, guestShared / MiB, guestFileMapped / MiB, guestMapped / MiB, guestMappedUnknown / MiB, hostPrivate / MiB, merged.size(), hostShared / MiB, hostImage / MiB, hostMapped / MiB, unknown / MiB,
        heapAllocated / MiB, heapCommitted / MiB, static_cast<unsigned long>(heapsSummed), static_cast<unsigned long>(heapCount), static_cast<double>(textureSnapshotBytes.load(std::memory_order_relaxed)) / MiB,
        counters.PrivateUsage / MiB, heapCommitted / MiB, (static_cast<double>(counters.PrivateUsage) - static_cast<double>(heapCommitted)) / MiB, top);
}

// Frequent pressure samples must not enumerate or sort the process's resident pages, or walk
// Windows heaps: Report() can run on the GPU allocation thread.
void snapshot(const char* reason) {
    constexpr double MiB = 1024.0 * 1024.0;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    const bool countersKnown = K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != 0;
    char processText[160] = "process working_set unknown private_commit unknown";
    if (countersKnown) std::snprintf(processText, sizeof(processText), "process working_set %.0f MiB private_commit %.0f MiB", counters.WorkingSetSize / MiB, counters.PrivateUsage / MiB);
    // Disk held by file-backed direct memory (registered by libkernel, including delete-pending
    // files invisible to directory listings) and the system commit limit, timestamped so a commit
    // limit increase (pagefile extension) can be told apart from backing-file growth.
    std::uint64_t backingLogical = 0, backingAllocated = 0;
    std::uint32_t backingFiles = 0;
    GuestArena::GuestArenaBackingFileBytes_nid_postfix(&backingLogical, &backingAllocated, &backingFiles);
    MEMORYSTATUSEX status{sizeof(MEMORYSTATUSEX)};
    const bool statusKnown = GlobalMemoryStatusEx(&status) != 0;
    char volume[MAX_PATH * 2] = "unknown";
    std::uint64_t volumeFree = 0;
    const bool volumeKnown = backingVolumeFree(volume, sizeof(volume), &volumeFree);
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char statusText[192] = "system memory status unknown";
    if (statusKnown) std::snprintf(statusText, sizeof(statusText), "system commit_limit %.0f MiB commit_available %.0f MiB physical_available %.0f MiB", status.ullTotalPageFile / MiB, status.ullAvailPageFile / MiB, status.ullAvailPhys / MiB);
    char volumeText[64] = "unknown";
    if (volumeKnown) std::snprintf(volumeText, sizeof(volumeText), "%.0f MiB", volumeFree / MiB);
    const auto vulkan = MemoryAccounting::Enabled() ? MemoryAccounting::Describe() : std::string("vk_memory accounting off");
    std::fprintf(stderr, "[mem-ledger-backing] utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ uptime_ms=%llu reason=%s dmem_files=%u dmem_logical %.0f MiB dmem_allocated_on_disk %.0f MiB dmem_query_coverage=unknown | backing volume %s free %s | %s | %s | texture snapshots %.0f MiB | %s\n",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds, static_cast<unsigned long long>(GetTickCount64()), reason,
        backingFiles, backingLogical / MiB, backingAllocated / MiB, volume, volumeText, statusText,
        processText, static_cast<double>(textureSnapshotBytes.load(std::memory_order_relaxed)) / MiB, vulkan.c_str());
    if (volumeKnown) lastVolumeFree.store(volumeFree, std::memory_order_relaxed);
}
#endif

}

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_MEMORY_LEDGER");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void AddTextureSnapshotBytes(std::int64_t delta) {
    textureSnapshotBytes.fetch_add(delta, std::memory_order_relaxed);
}

void ReportHostImports(const char* reason) {
    if (!Enabled()) return;
#ifdef _WIN32
    static std::atomic<std::int64_t> last{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto previous = last.load(std::memory_order_relaxed);
    const auto cadence = reason != nullptr && (std::strstr(reason, "pressure") != nullptr || std::strcmp(reason, "disk_drop") == 0) ? 1000 : intervalMs();
    if (previous != 0 && now - previous < cadence) return;
    if (!last.compare_exchange_strong(previous, now, std::memory_order_relaxed)) return;
    try {
        const auto report = DescribeHostImportCensus();
        if (!report.empty()) std::fprintf(stderr, "%s", report.c_str());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[host-import-census] diagnostic error ignored: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "[host-import-census] diagnostic error ignored\n");
    }
#endif
}

void Report() {
    if (!Enabled()) return;
#ifdef _WIN32
    InitializeHostImportZeroAudit();
    // Periodic line at the configured cadence; under pressure (see pressure()) at most one line per second.
    static std::atomic<std::int64_t> last{0};
    static std::atomic<std::int64_t> lastCheck{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto previous = last.load(std::memory_order_relaxed);
    const char* reason = nullptr;
    if (previous == 0 || now - previous >= intervalMs()) {
        reason = "periodic";
    } else if (now - previous >= 1000) {
        auto checked = lastCheck.load(std::memory_order_relaxed);
        if (now - checked < 1000 || !lastCheck.compare_exchange_strong(checked, now, std::memory_order_relaxed)) return;
        reason = pressure();
    }
    if (reason == nullptr) return;
    auto expected = previous;
    if (!last.compare_exchange_strong(expected, now, std::memory_order_relaxed)) return;
    // Diagnostics never propagate an error to the caller (a Vulkan allocation trampoline or a
    // dispatch): failures are reported and swallowed.
    try {
        static std::mutex running;
        std::unique_lock lock(running, std::try_to_lock);
        if (!lock.owns_lock()) return;
        const auto started = std::chrono::steady_clock::now();
        snapshot(reason);
        ReportHostImports(reason);
        std::fprintf(stderr, "[mem-ledger] sample_ms=%.3f reason=%s\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), reason);
        // Full attribution is explicitly opt-in and never accelerates under pressure. It is
        // too intrusive for a performance run (about one second with a large working set).
        static const bool fullCensus = [] {
            const char* value = std::getenv("APS5_MEMORY_LEDGER_FULL");
            return value != nullptr && value[0] == '1';
        }();
        static std::int64_t lastCensus = 0; // protected by running
        if (fullCensus && (lastCensus == 0 || now - lastCensus >= intervalMs())) {
            lastCensus = now;
            const auto censusStarted = std::chrono::steady_clock::now();
            census(reason);
            std::fprintf(stderr, "[mem-ledger] census_ms=%.1f reason=%s\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - censusStarted).count(), reason);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[mem-ledger] diagnostic error ignored: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "[mem-ledger] diagnostic error ignored\n");
    }
#endif
}

}
