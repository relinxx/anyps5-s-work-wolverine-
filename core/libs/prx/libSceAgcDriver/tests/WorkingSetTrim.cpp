// WorkingSetTrim::TrimRange must never release pages the process locked (guest mlock is
// VirtualLock), whether they sit inside a trim chunk or straddle its edges, and must report
// VirtualUnlock's documented ERROR_NOT_LOCKED as the expected outcome rather than an error.
#include "prx/libSceAgcDriver/Graphics/include/WorkingSetTrim.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::uint64_t attributes(const void* address) {
    PSAPI_WORKING_SET_EX_INFORMATION info{};
    info.VirtualAddress = const_cast<void*>(address);
    if (!K32QueryWorkingSetEx(GetCurrentProcess(), &info, sizeof(info))) return ~0ull;
    return static_cast<std::uint64_t>(info.VirtualAttributes.Flags);
}

bool locked(const void* address) {
    const auto flags = attributes(address);
    return flags != ~0ull && (flags & 1ull) != 0 && (flags & (1ull << 22u)) != 0;
}

}

int main() {
    constexpr std::size_t page = 4096;
    constexpr std::size_t chunk = 16u << 20u;
    // Room for locks to straddle both edges of the trimmed chunk.
    const std::size_t total = chunk + 2 * 65536;
    SIZE_T minimum = 0, maximum = 0;
    GetProcessWorkingSetSize(GetCurrentProcess(), &minimum, &maximum);
    SetProcessWorkingSetSize(GetCurrentProcess(), minimum + (8u << 20u), maximum + (8u << 20u));
    auto* base = static_cast<std::byte*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (base == nullptr) {
        std::fprintf(stderr, "VirtualAlloc failed %lu\n", GetLastError());
        return 1;
    }
    for (std::size_t i = 0; i < total; i += page) base[i] = static_cast<std::byte>(i / page);
    auto* chunkBegin = base + 65536;
    auto* chunkEnd = chunkBegin + chunk;
    struct Lock { std::byte* address; std::size_t bytes; const char* name; };
    const Lock locks[] = {
        {chunkBegin - 2 * page, 4 * page, "lock straddling the chunk start"},
        {chunkBegin + chunk / 2, 16 * page, "lock inside the chunk"},
        {chunkEnd - 2 * page, 4 * page, "lock straddling the chunk end"},
    };
    for (const auto& lock : locks) check(VirtualLock(lock.address, lock.bytes) != 0, lock.name);

    const auto result = AgcDriver::Graphics::WorkingSetTrim::TrimRange(reinterpret_cast<std::uintptr_t>(chunkBegin), reinterpret_cast<std::uintptr_t>(chunkEnd));
    check(result.unexpectedErrors == 0, "no unexpected VirtualUnlock or query errors");
    check(result.queriedBytes == chunk, "every committed byte of the chunk was examined");
    // Locked pages inside the chunk: 2 at the start edge, 16 in the middle, 2 at the end edge.
    check(result.lockedBytes == 20 * page, "locked pages inside the chunk were counted and skipped");
    check(result.expectedNotLocked >= 1, "releasing unlocked pages reports ERROR_NOT_LOCKED");
    check(result.requestedBytes + result.lockedBytes <= chunk, "no locked page was requested for release");

    // Every locked page, inside and outside the chunk, must still be resident and locked.
    for (const auto& lock : locks) {
        for (std::size_t offset = 0; offset < lock.bytes; offset += page) check(locked(lock.address + offset), lock.name);
        // VirtualUnlock succeeds only on a range that is still locked.
        check(VirtualUnlock(lock.address, lock.bytes) != 0, "lock survived the trim");
    }
    // Contents are untouched by the trim.
    bool same = true;
    for (std::size_t i = 0; i < total; i += page) same = same && base[i] == static_cast<std::byte>(i / page);
    check(same, "page contents unchanged");
    VirtualFree(base, 0, MEM_RELEASE);

    // Concurrent: a locker thread follows guest mlock's protocol (lock under the shared ownership,
    // as libkernel LockHostPages does) on rotating pages while trims run over the same range under
    // the same ownership. A lock made at any point must still hold when the locker re-checks it.
    {
        constexpr std::size_t span = 4u << 20u;
        auto* region = static_cast<std::byte*>(VirtualAlloc(nullptr, span, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        check(region != nullptr, "concurrent region allocated");
        for (std::size_t i = 0; i < span; i += page) region[i] = std::byte{1};
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> trims{0}, unexpected{0}, lockedSeen{0};
        std::thread trimmer([&] {
            while (!stop.load()) {
                const GuestArena::WorkingSetOwnership ownership;
                const auto r = AgcDriver::Graphics::WorkingSetTrim::TrimRange(reinterpret_cast<std::uintptr_t>(region), reinterpret_cast<std::uintptr_t>(region + span));
                unexpected += r.unexpectedErrors;
                lockedSeen += r.lockedBytes;
                ++trims;
            }
        });
        std::uint64_t lost = 0, made = 0;
        const auto deadline = GetTickCount64() + 1500;
        for (std::size_t round = 0; GetTickCount64() < deadline; ++round) {
            auto* target = region + ((round * 37) % (span / page)) * page;
            {
                const GuestArena::WorkingSetOwnership ownership;
                if (!VirtualLock(target, page)) continue;
            }
            ++made;
            Sleep(0); // let trims interleave with a held lock
            {
                const GuestArena::WorkingSetOwnership ownership;
                if (!locked(target)) ++lost;
                if (!VirtualUnlock(target, page)) ++lost; // fails only if the lock was released by a trim
            }
        }
        stop = true;
        trimmer.join();
        check(made > 50, "concurrent locks were made");
        check(trims.load() > 5, "trims ran concurrently");
        check(unexpected.load() == 0, "no unexpected errors during concurrent trims");
        check(lost == 0, "no concurrently made lock was released by a trim");
        std::printf("concurrent: %llu locks, %llu trims, %llu locked bytes seen by trims, %llu lost\n", static_cast<unsigned long long>(made), static_cast<unsigned long long>(trims.load()), static_cast<unsigned long long>(lockedSeen.load()), static_cast<unsigned long long>(lost));
        VirtualFree(region, 0, MEM_RELEASE);
    }
    if (failures != 0) return 1;
    std::printf("working set trim tests passed (locked skipped %llu bytes, release requested %llu bytes, %llu not-locked returns)\n",
        static_cast<unsigned long long>(result.lockedBytes), static_cast<unsigned long long>(result.requestedBytes), static_cast<unsigned long long>(result.expectedNotLocked));
    return 0;
}
#else
int main() { return 77; }
#endif
