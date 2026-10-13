#pragma once
#include <cstdint>

namespace AgcDriver::Graphics {
// Use allocation ownership rather than an imported window's geometry. A heap can
// contain dynamic BDA stores that the current descriptor list cannot enumerate.
inline bool PreserveImportedHeap(std::uint64_t owner, std::uint64_t ownerBytes,
        bool gpu, bool writable, int protection, std::uint64_t gpuWriteBindings) {
    if (owner == 0x1120000000ull && ownerBytes == 0xac600000ull) return true;
    if (ownerBytes <= (1ull << 20u)) return false;
    if (gpuWriteBindings != 0) return true; // conservative for this import's lifetime
    const bool knownGpuReadOnly = protection >= 0 && (protection & 0x30) == 0x10;
    return gpu && writable && !knownGpuReadOnly;
}
}
