#pragma once

#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver::Graphics::HeapReclaim {
struct Result {
    bool supported = false;
    bool succeeded = false;
    std::uint32_t error = 0;
};

// Windows decommits unused LFH caches; live allocations stay valid. A null handle
// requests all process heaps. This releases commit, unlike a working-set trim.
inline Result Optimize(
#ifdef _WIN32
        HANDLE heap = nullptr
#endif
        ) noexcept {
#ifdef _WIN32
    // The project targets older Windows headers; class 3 and the version-1 ABI
    // are documented for Windows 8.1+. Older systems reject the request safely.
    struct Information { DWORD version; DWORD flags; } information{1, 0};
    static_assert(sizeof(Information) == 8);
    const bool succeeded = HeapSetInformation(heap, static_cast<HEAP_INFORMATION_CLASS>(3), &information, sizeof(information)) != 0;
    return {true, succeeded, succeeded ? 0u : GetLastError()};
#else
    return {};
#endif
}
}
