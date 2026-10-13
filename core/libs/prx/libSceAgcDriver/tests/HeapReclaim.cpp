#include "prx/libSceAgcDriver/Graphics/include/HeapReclaim.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
#ifdef _WIN32
    HANDLE heap = HeapCreate(0, 0, 0);
    if (heap == nullptr) return 1;
    ULONG compatibility = 2;
    if (!HeapSetInformation(heap, HeapCompatibilityInformation, &compatibility, sizeof(compatibility))) {
        HeapDestroy(heap);
        return 1;
    }
    struct Allocation { void* pointer; std::size_t bytes; unsigned char value; };
    std::vector<Allocation> live;
    for (std::size_t i = 0; i < 32768; ++i) {
        const auto bytes = 32 + (i % 32) * 32;
        auto* pointer = HeapAlloc(heap, 0, bytes);
        if (pointer == nullptr) { HeapDestroy(heap); return 1; }
        const auto value = static_cast<unsigned char>(i);
        std::memset(pointer, value, bytes);
        live.push_back({pointer, bytes, value});
    }
    // Leave sparse live allocations across the LFH size classes.
    for (std::size_t i = 0; i < live.size(); ++i) {
        if (i % 37 != 0) {
            if (!HeapFree(heap, 0, live[i].pointer)) { HeapDestroy(heap); return 1; }
            live[i].pointer = nullptr;
        }
    }
    const auto one = AgcDriver::Graphics::HeapReclaim::Optimize(heap);
    const auto all = AgcDriver::Graphics::HeapReclaim::Optimize();
    if (!one.supported || !one.succeeded || !all.succeeded) {
        std::fprintf(stderr, "heap optimization failed: one=%u all=%u\n", one.error, all.error);
        HeapDestroy(heap);
        return 1;
    }
    for (auto& allocation : live) {
        if (allocation.pointer == nullptr) continue;
        const auto* bytes = static_cast<unsigned char*>(allocation.pointer);
        for (std::size_t i = 0; i < allocation.bytes; ++i) {
            if (bytes[i] != allocation.value) { HeapDestroy(heap); return 1; }
        }
        if (!HeapFree(heap, 0, allocation.pointer)) { HeapDestroy(heap); return 1; }
    }
    if (!HeapValidate(heap, 0, nullptr)) { HeapDestroy(heap); return 1; }
    if (!HeapDestroy(heap)) return 1;
    std::puts("real Windows LFH optimization preserved live allocations; release size is allocator-dependent");
    return 0;
#else
    const auto result = AgcDriver::Graphics::HeapReclaim::Optimize();
    return result.supported ? 1 : 77;
#endif
}
