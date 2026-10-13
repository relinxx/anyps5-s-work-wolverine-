#include "prx/libSceAgcDriver/Graphics/include/ZeroMirrorPolicy.hpp"
#include <cstdio>
using namespace AgcDriver::Graphics;

int main() {
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    check(ParseZeroMirrorMode("") == ZeroMirrorMode::Disabled, "unset disables experiment");
    check(ParseZeroMirrorMode("invalid") == ZeroMirrorMode::Disabled, "unknown mode disables experiment");
    check(ParseZeroMirrorMode("10d") == ZeroMirrorMode::Only10d, "10d mode parsed");
    check(ParseZeroMirrorMode("1") == ZeroMirrorMode::Both, "original pilot remains reproducible");
    for (const auto mode : {ZeroMirrorMode::Disabled, ZeroMirrorMode::Both, ZeroMirrorMode::Only10d}) {
        check(!IsZeroMirrorOwner(mode, 0x1120000000ull, 0xac600000ull), "protected GPU-written owner excluded");
        check(!IsZeroMirrorOwner(mode, 0x1210000000ull, 0xab000000ull), "other large owner excluded");
        check(!IsZeroMirrorOwner(mode, 0x10d0000000ull, 0x33010000ull), "changed owner geometry excluded");
    }
    check(IsZeroMirrorOwner(ZeroMirrorMode::Only10d, 0x10d0000000ull, 0x33000000ull), "10d selected");
    check(!IsZeroMirrorOwner(ZeroMirrorMode::Only10d, 0x11e0000000ull, 0x19e00000ull), "11e import-first in 10d mode");
    check(IsZeroMirrorOwner(ZeroMirrorMode::Both, 0x11e0000000ull, 0x19e00000ull), "11e selected in original pilot");
    check(!IsZeroMirrorOwner(ZeroMirrorMode::Disabled, 0x10d0000000ull, 0x33000000ull), "control selects no zero mirrors");
    check(!IsZeroMirrorOwner(ZeroMirrorMode::Only10d, 0x11e128c000ull, 0x18b74000ull), "fragment address never mistaken for owner");
    check(PreserveZeroPilotHostImport(true, ZeroMirrorMode::Only10d, 0x11e0000000ull, 0x19e00000ull), "comparison owner protected against budget eviction");
    check(!PreserveZeroPilotHostImport(false, ZeroMirrorMode::Only10d, 0x11e0000000ull, 0x19e00000ull), "old experiment remains reproducible");
    for (const auto mode : {ZeroMirrorMode::Disabled, ZeroMirrorMode::Both})
        check(!PreserveZeroPilotHostImport(true, mode, 0x11e0000000ull, 0x19e00000ull), "other modes preserve original import behavior");
    check(!PreserveZeroPilotHostImport(true, ZeroMirrorMode::Only10d, 0x1120000000ull, 0xac600000ull), "critical owner policy unchanged");
    check(!PreserveZeroPilotHostImport(true, ZeroMirrorMode::Only10d, 0x11e0000000ull, 0x19e10000ull), "different allocation not protected");
    return failures != 0;
}
