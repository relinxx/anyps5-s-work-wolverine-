#pragma once
#include <cstdint>
#include <string_view>

namespace AgcDriver::Graphics {
enum class ZeroMirrorMode { Disabled, Both, Only10d };

inline ZeroMirrorMode ParseZeroMirrorMode(std::string_view value) {
    if (value == "1") return ZeroMirrorMode::Both;
    if (value == "10d") return ZeroMirrorMode::Only10d;
    return ZeroMirrorMode::Disabled;
}

// Match allocation ownership, not fragment geometry: protection changes split 11e into
// several registrations. No mode in this experiment selects the protected 112 owner.
inline bool IsZeroMirrorOwner(ZeroMirrorMode mode, std::uint64_t address, std::uint64_t bytes) {
    if (mode == ZeroMirrorMode::Disabled) return false;
    if (address == 0x10d0000000ull && bytes == 0x33000000ull) return true;
    return mode == ZeroMirrorMode::Both && address == 0x11e0000000ull && bytes == 0x19e00000ull;
}

// Keep the comparison owner out of budget/LRU retirement in the 10d-only experiment.
// Registration changes must still retire it through the ordinary fence/coherence path.
inline bool PreserveZeroPilotHostImport(bool enabled, ZeroMirrorMode mode, std::uint64_t address, std::uint64_t bytes) {
    return enabled && mode == ZeroMirrorMode::Only10d && address == 0x11e0000000ull && bytes == 0x19e00000ull;
}
}
