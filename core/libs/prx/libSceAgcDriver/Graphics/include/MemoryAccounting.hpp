#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <string>

// Diagnostic accounting of VkDeviceMemory allocations (APS5_MEMORY_LEDGER=1). WrapDeviceProc puts
// counting trampolines in front of vkAllocateMemory and vkFreeMemory for every caller that resolves
// device functions through the wrapped vkGetDeviceProcAddr (Graphics::Context::Function, VulkanDevice
// and every stored function pointer obtained from them). Only successful allocations are counted;
// identity is (device, memory). It never changes an allocation's arguments or result.
namespace AgcDriver::Graphics::MemoryAccounting {

// Mutually exclusive classes. Imported host memory (VkImportMemoryHostPointerInfoEXT in pNext) is
// its own class whatever its memory type; the others follow the type's property flags.
enum class MemoryClass : std::uint32_t {
    ImportedHost,
    DeviceLocalOnly,      // DEVICE_LOCAL without HOST_VISIBLE
    DeviceLocalMappable,  // DEVICE_LOCAL and HOST_VISIBLE (resizable BAR / UMA)
    HostVisibleOnly,      // HOST_VISIBLE without DEVICE_LOCAL
    OtherOrUnknownType,   // neither flag, or the device's memory properties were not registered
    Count,
};

const char* ClassName(MemoryClass memoryClass);

struct Totals {
    std::array<std::uint64_t, static_cast<std::size_t>(MemoryClass::Count)> liveBytes{};
    std::array<std::uint64_t, static_cast<std::size_t>(MemoryClass::Count)> liveCount{};
    // Heap bytes of the FIRST registered device only (heap indices are per device); allocations of
    // any other device are in otherDeviceBytes. Class totals above cover all devices.
    std::array<std::uint64_t, VK_MAX_MEMORY_HEAPS> heapLiveBytes{};
    std::uint32_t heapCount = 0; // heaps of the first registered device (0: none registered)
    std::uint64_t otherDeviceBytes = 0;
    std::uint64_t droppedAccounting = 0; // bookkeeping failures; affected allocations are uncounted
    std::uint64_t allocations = 0;     // successful vkAllocateMemory calls seen
    std::uint64_t failedAllocations = 0;
    std::uint64_t frees = 0;           // vkFreeMemory calls on tracked memory
    std::uint64_t untrackedFrees = 0;  // vkFreeMemory of memory this accounting never saw allocated
    std::uint64_t nullFrees = 0;
    std::uint64_t duplicateHandles = 0; // a successful allocation returned a (device, memory) still live
};

bool Enabled();
// Tests only: forces accounting on or off regardless of the environment.
void SetEnabledForTesting(bool enabled);
// Returns a resolver that forwards every name to `real`, except vkAllocateMemory and vkFreeMemory,
// which it returns as counting trampolines. Returns `real` unchanged when accounting is disabled.
PFN_vkGetDeviceProcAddr WrapDeviceProc(PFN_vkGetDeviceProcAddr real);
// Memory type and heap layout for classifying `device`'s allocations; optional budget query
// (VK_EXT_memory_budget through vkGetPhysicalDeviceMemoryProperties2) for reports.
void RegisterDevice(VkDevice device, const VkPhysicalDeviceMemoryProperties& properties, VkPhysicalDevice physicalDevice = VK_NULL_HANDLE, PFN_vkGetPhysicalDeviceMemoryProperties2 properties2 = nullptr, bool memoryBudget = false);
void UnregisterDevice(VkDevice device);
Totals Snapshot();
// One-line summary: per-class live MiB/count, per-heap live MiB and, when available, the driver's
// VK_EXT_memory_budget heap usage/budget (called without holding the accounting lock).
std::string Describe();
// Tests only: drops all live records and counters.
void ResetForTesting();

}
