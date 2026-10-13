// MemoryAccounting with a FAKE vkGetDeviceProcAddr/vkAllocateMemory/vkFreeMemory: these checks
// exercise the counting logic only and make no claim about real Vulkan or VRAM usage.
#include "prx/libSceAgcDriver/Graphics/include/MemoryAccounting.hpp"
#include <cstdio>
#include <cstring>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

VkResult nextResult = VK_SUCCESS;
std::uint64_t nextHandle = 0x1000;
bool reuseHandle = false;
int realFrees = 0;
VkDevice pipelineDevice = VK_NULL_HANDLE;
VkPipelineCache pipelineCache = VK_NULL_HANDLE;
std::uint32_t pipelineCount = 0;
const void* pipelineInfos = nullptr;
const VkAllocationCallbacks* pipelineCallbacks = nullptr;
int realPipelineCalls = 0;

template<typename Info>
VkResult FakePipelines(VkDevice device, VkPipelineCache cache, std::uint32_t count, const Info* infos, const VkAllocationCallbacks* callbacks, VkPipeline* pipelines) {
    pipelineDevice=device; pipelineCache=cache; pipelineCount=count;
    pipelineInfos=infos; pipelineCallbacks=callbacks; ++realPipelineCalls;
    if (nextResult == VK_SUCCESS) for (std::uint32_t i=0;i<count;++i) pipelines[i]=reinterpret_cast<VkPipeline>(nextHandle++);
    return nextResult;
}
VKAPI_ATTR VkResult VKAPI_CALL FakeCompute(VkDevice device, VkPipelineCache cache, std::uint32_t count, const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* callbacks, VkPipeline* pipelines) { return FakePipelines(device,cache,count,infos,callbacks,pipelines); }
VKAPI_ATTR VkResult VKAPI_CALL FakeGraphics(VkDevice device, VkPipelineCache cache, std::uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* callbacks, VkPipeline* pipelines) { return FakePipelines(device,cache,count,infos,callbacks,pipelines); }

VKAPI_ATTR VkResult VKAPI_CALL FakeAllocate(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    if (nextResult != VK_SUCCESS) return nextResult;
    *memory = reinterpret_cast<VkDeviceMemory>(reuseHandle ? 0x1000 : nextHandle++);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL FakeFree(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {
    ++realFrees;
}

VKAPI_ATTR void VKAPI_CALL FakeOther() {}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeResolver(VkDevice, const char* name) {
    if (std::strcmp(name, "vkAllocateMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&FakeAllocate);
    if (std::strcmp(name, "vkFreeMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&FakeFree);
    if (std::strcmp(name, "vkCreateComputePipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&FakeCompute);
    if (std::strcmp(name, "vkCreateGraphicsPipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&FakeGraphics);
    if (std::strcmp(name, "vkDestroyBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&FakeOther);
    return nullptr;
}

VkPhysicalDeviceMemoryProperties Properties() {
    VkPhysicalDeviceMemoryProperties properties{};
    properties.memoryHeapCount = 2;
    properties.memoryHeaps[0] = {12ull << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    properties.memoryHeaps[1] = {16ull << 30, 0};
    properties.memoryTypeCount = 4;
    properties.memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    properties.memoryTypes[1] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1};
    properties.memoryTypes[2] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
    properties.memoryTypes[3] = {0, 1};
    return properties;
}

std::uint64_t live(MemoryAccounting::MemoryClass memoryClass) {
    return MemoryAccounting::Snapshot().liveBytes[static_cast<std::size_t>(memoryClass)];
}

}

int main() {
    using MemoryAccounting::MemoryClass;
    MemoryAccounting::SetEnabledForTesting(false);
    check(MemoryAccounting::WrapDeviceProc(&FakeResolver) == &FakeResolver, "disabled accounting returns the real resolver unchanged");
    MemoryAccounting::SetEnabledForTesting(true);
    MemoryAccounting::ResetForTesting();
    const auto resolver = MemoryAccounting::WrapDeviceProc(&FakeResolver);
    check(resolver != &FakeResolver, "enabled accounting wraps the resolver");
    const auto deviceA = reinterpret_cast<VkDevice>(0xA0);
    const auto deviceB = reinterpret_cast<VkDevice>(0xB0);
    const auto properties = Properties();
    MemoryAccounting::RegisterDevice(deviceA, properties);
    MemoryAccounting::RegisterDevice(deviceB, properties);
    check(resolver(deviceA, "vkDestroyBuffer") == reinterpret_cast<PFN_vkVoidFunction>(&FakeOther), "other functions are forwarded unchanged");
    const auto allocateA = reinterpret_cast<PFN_vkAllocateMemory>(resolver(deviceA, "vkAllocateMemory"));
    const auto freeA = reinterpret_cast<PFN_vkFreeMemory>(resolver(deviceA, "vkFreeMemory"));
    const auto allocateB = reinterpret_cast<PFN_vkAllocateMemory>(resolver(deviceB, "vkAllocateMemory"));
    const auto freeB = reinterpret_cast<PFN_vkFreeMemory>(resolver(deviceB, "vkFreeMemory"));
    check(allocateA != &FakeAllocate && freeA != &FakeFree, "allocate and free are trampolines");

    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = 64ull << 20;
    info.memoryTypeIndex = 0;

    // 1. Successful allocate/free round trip (device-local only, heap 0).
    VkDeviceMemory memory = VK_NULL_HANDLE;
    check(allocateA(deviceA, &info, nullptr, &memory) == VK_SUCCESS, "allocation succeeds");
    check(live(MemoryClass::DeviceLocalOnly) == (64ull << 20), "device-local bytes counted");
    check(MemoryAccounting::Snapshot().heapLiveBytes[0] == (64ull << 20), "heap 0 bytes counted");
    freeA(deviceA, memory, nullptr);
    check(live(MemoryClass::DeviceLocalOnly) == 0 && MemoryAccounting::Snapshot().heapLiveBytes[0] == 0, "free returns to baseline");
    check(realFrees == 1, "real free called once");
    const auto createCompute = reinterpret_cast<PFN_vkCreateComputePipelines>(resolver(deviceA,"vkCreateComputePipelines"));
    const auto createGraphics = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(resolver(deviceB,"vkCreateGraphicsPipelines"));
    VkComputePipelineCreateInfo computeInfos[2]{};
    VkGraphicsPipelineCreateInfo graphicsInfo{};
    VkAllocationCallbacks callbacks{};
    VkPipeline pipelines[2]{};
    const auto cache = reinterpret_cast<VkPipelineCache>(0x1234);
    check(createCompute(deviceA,cache,2,computeInfos,&callbacks,pipelines)==VK_SUCCESS, "timed compute call succeeds");
    check(pipelineDevice==deviceA && pipelineCache==cache && pipelineCount==2 && pipelineInfos==computeInfos && pipelineCallbacks==&callbacks && pipelines[0]!=VK_NULL_HANDLE && pipelines[1]!=VK_NULL_HANDLE, "pipeline timing changed arguments or results");
    nextResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;
    check(createGraphics(deviceB,VK_NULL_HANDLE,1,&graphicsInfo,nullptr,pipelines)==nextResult, "timed graphics failure propagated");
    check(pipelineDevice==deviceB && pipelineCache==VK_NULL_HANDLE && pipelineCount==1 && pipelineInfos==&graphicsInfo && realPipelineCalls==2, "pipeline calls must forward once per call, not once per pipeline");
    nextResult=VK_SUCCESS;

    // 2. Failed allocation: not counted live, failure recorded, result unchanged.
    nextResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    memory = VK_NULL_HANDLE;
    check(allocateA(deviceA, &info, nullptr, &memory) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "failure result passed through");
    nextResult = VK_SUCCESS;
    check(MemoryAccounting::Snapshot().failedAllocations == 1 && live(MemoryClass::DeviceLocalOnly) == 0, "failed allocation not live");

    // 3. Bind/map failure rollback: the caller frees right after allocating.
    check(allocateA(deviceA, &info, nullptr, &memory) == VK_SUCCESS, "rollback allocation");
    freeA(deviceA, memory, nullptr);
    check(live(MemoryClass::DeviceLocalOnly) == 0 && MemoryAccounting::Snapshot().liveCount[static_cast<std::size_t>(MemoryClass::DeviceLocalOnly)] == 0, "rollback leaves nothing live");

    // 4. Imported host pointer: its own class whatever the memory type.
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    VkMemoryAllocateInfo imported = info;
    imported.pNext = &import;
    imported.memoryTypeIndex = 1;
    VkDeviceMemory importedMemory = VK_NULL_HANDLE;
    check(allocateA(deviceA, &imported, nullptr, &importedMemory) == VK_SUCCESS, "import allocation");
    check(live(MemoryClass::ImportedHost) == (64ull << 20) && live(MemoryClass::HostVisibleOnly) == 0, "import counted as imported only");

    // 5. DEVICE_LOCAL | HOST_VISIBLE type: counted once, in the overlap class.
    VkMemoryAllocateInfo mappable = info;
    mappable.memoryTypeIndex = 2;
    VkDeviceMemory mappableMemory = VK_NULL_HANDLE;
    check(allocateA(deviceA, &mappable, nullptr, &mappableMemory) == VK_SUCCESS, "mappable allocation");
    check(live(MemoryClass::DeviceLocalMappable) == (64ull << 20) && live(MemoryClass::DeviceLocalOnly) == 0 && live(MemoryClass::HostVisibleOnly) == 0, "overlapping flags not double counted");

    // 6. Two devices returning identical handle values: separate identities.
    reuseHandle = true;
    VkDeviceMemory sameA = VK_NULL_HANDLE, sameB = VK_NULL_HANDLE;
    check(allocateA(deviceA, &info, nullptr, &sameA) == VK_SUCCESS && allocateB(deviceB, &info, nullptr, &sameB) == VK_SUCCESS, "identical handles allocated");
    reuseHandle = false;
    check(sameA == sameB, "fake returned identical handles");
    check(live(MemoryClass::DeviceLocalOnly) == (128ull << 20), "both devices counted");
    check(MemoryAccounting::Snapshot().otherDeviceBytes == (64ull << 20), "second device's bytes kept out of first-device heap totals");
    check(MemoryAccounting::Snapshot().heapLiveBytes[0] == (64ull << 20) + (64ull << 20), "first-device heap 0 holds only device A (mappable + this allocation)");
    freeB(deviceB, sameB, nullptr);
    check(live(MemoryClass::DeviceLocalOnly) == (64ull << 20), "freeing device B leaves device A's allocation");
    freeA(deviceA, sameA, nullptr);
    check(live(MemoryClass::DeviceLocalOnly) == 0, "freeing device A clears it");

    // 7. Deferred free through a stored function pointer after other allocations.
    PFN_vkFreeMemory stored = freeA;
    VkDeviceMemory deferred = VK_NULL_HANDLE;
    check(allocateA(deviceA, &info, nullptr, &deferred) == VK_SUCCESS, "deferred allocation");
    VkDeviceMemory other = VK_NULL_HANDLE;
    check(allocateA(deviceA, &info, nullptr, &other) == VK_SUCCESS, "interleaved allocation");
    freeA(deviceA, other, nullptr);
    check(live(MemoryClass::DeviceLocalOnly) == (64ull << 20), "deferred allocation still live");
    stored(deviceA, deferred, nullptr);
    check(live(MemoryClass::DeviceLocalOnly) == 0, "deferred free through stored pointer counted");

    // 8. Untracked and null frees are counted, never subtracted.
    const auto before = MemoryAccounting::Snapshot();
    freeA(deviceA, reinterpret_cast<VkDeviceMemory>(0xDEAD), nullptr);
    freeA(deviceA, VK_NULL_HANDLE, nullptr);
    const auto after = MemoryAccounting::Snapshot();
    check(after.untrackedFrees == before.untrackedFrees + 1 && after.nullFrees == before.nullFrees + 1, "untracked and null frees reported");
    check(after.liveBytes == before.liveBytes, "untracked frees change no live bytes");

    // 9. Unregistered device: reported as unknown type, not silently classified.
    const auto deviceC = reinterpret_cast<VkDevice>(0xC0);
    const auto allocateC = reinterpret_cast<PFN_vkAllocateMemory>(resolver(deviceC, "vkAllocateMemory"));
    VkDeviceMemory unknownMemory = VK_NULL_HANDLE;
    check(allocateC(deviceC, &info, nullptr, &unknownMemory) == VK_SUCCESS && live(MemoryClass::OtherOrUnknownType) == (64ull << 20), "unregistered device counted as unknown type");

    // 10. Unregistering a device drops its live records; the description stays well formed.
    MemoryAccounting::UnregisterDevice(deviceC);
    check(live(MemoryClass::OtherOrUnknownType) == 0, "unregister drops the device's records");
    const auto text = MemoryAccounting::Describe();
    check(text.find("imported_host=64/1") != std::string::npos && text.find("device_local_host_visible=64/1") != std::string::npos, "description reports per-class MiB/count");
    check(text.find("driver_usage=unknown") != std::string::npos, "missing budget reported as unknown, not zero");
    freeA(deviceA, importedMemory, nullptr);
    freeA(deviceA, mappableMemory, nullptr);
    check(MemoryAccounting::Snapshot().liveBytes == decltype(MemoryAccounting::Snapshot().liveBytes){}, "everything freed returns all classes to zero");

    if (failures != 0) return 1;
    std::puts("memory accounting tests passed (fake Vulkan dispatch; no real VRAM measured)");
    return 0;
}
