#include "prx/libSceAgcDriver/Graphics/include/MemoryAccounting.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MemoryLedger.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <type_traits>

// Every bookkeeping step that can allocate (maps, strings) runs inside try/catch: a diagnostic
// failure is counted in `droppedAccounting` (lock-free) and never changes or hides the result of
// the real Vulkan call. A resolver that cannot record its device returns the real function.
namespace AgcDriver::Graphics::MemoryAccounting {
namespace {

struct Record {
    std::uint64_t bytes = 0;
    MemoryClass memoryClass = MemoryClass::OtherOrUnknownType;
    std::uint32_t heap = VK_MAX_MEMORY_HEAPS; // VK_MAX_MEMORY_HEAPS: unknown
    bool primaryDevice = false;               // counted in the first registered device's heap totals
};

struct DeviceInfo {
    VkPhysicalDeviceMemoryProperties properties{};
    bool registered = false;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    PFN_vkGetPhysicalDeviceMemoryProperties2 properties2 = nullptr;
    bool memoryBudget = false;
    PFN_vkAllocateMemory allocate = nullptr;
    PFN_vkFreeMemory free = nullptr;
    PFN_vkCreateComputePipelines compute = nullptr;
    PFN_vkCreateGraphicsPipelines graphics = nullptr;
};

struct State {
    std::mutex mutex;
    std::map<std::pair<VkDevice, VkDeviceMemory>, Record> live;
    std::map<VkDevice, DeviceInfo> devices;
    VkDevice firstRegistered = VK_NULL_HANDLE;
    Totals totals;
};

State& Shared() {
    static State* state = new State(); // never destroyed: trampolines may run during static teardown
    return *state;
}

std::atomic<PFN_vkGetDeviceProcAddr> realResolver{nullptr};
std::atomic<int> forced{-1};
std::atomic<std::uint64_t> droppedAccounting{0};
std::atomic<std::uint64_t> pipelineCallId{0};

constexpr std::size_t index(MemoryClass memoryClass) {
    return static_cast<std::size_t>(memoryClass);
}

bool importsHostPointer(const VkMemoryAllocateInfo* info) {
    for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next != nullptr; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT) return true;
    }
    return false;
}

Record classify(const DeviceInfo* device, const VkMemoryAllocateInfo* info, bool primary) {
    Record record;
    record.bytes = info->allocationSize;
    record.primaryDevice = primary;
    const bool imported = importsHostPointer(info);
    if (device == nullptr || !device->registered || info->memoryTypeIndex >= device->properties.memoryTypeCount) {
        record.memoryClass = imported ? MemoryClass::ImportedHost : MemoryClass::OtherOrUnknownType;
        return record;
    }
    const auto& type = device->properties.memoryTypes[info->memoryTypeIndex];
    record.heap = type.heapIndex;
    const bool local = (type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    const bool visible = (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    if (imported) record.memoryClass = MemoryClass::ImportedHost;
    else if (local && visible) record.memoryClass = MemoryClass::DeviceLocalMappable;
    else if (local) record.memoryClass = MemoryClass::DeviceLocalOnly;
    else if (visible) record.memoryClass = MemoryClass::HostVisibleOnly;
    else record.memoryClass = MemoryClass::OtherOrUnknownType;
    return record;
}

void add(Totals& totals, const Record& record, bool sign) {
    const auto apply = [sign](std::uint64_t& value, std::uint64_t amount) { value = sign ? value + amount : value - amount; };
    apply(totals.liveBytes[index(record.memoryClass)], record.bytes);
    apply(totals.liveCount[index(record.memoryClass)], 1);
    if (!record.primaryDevice) apply(totals.otherDeviceBytes, record.bytes);
    else if (record.heap < VK_MAX_MEMORY_HEAPS) apply(totals.heapLiveBytes[record.heap], record.bytes);
}

VKAPI_ATTR VkResult VKAPI_CALL CountedAllocate(VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    auto& state = Shared();
    PFN_vkAllocateMemory allocate = nullptr;
    {
        std::lock_guard lock(state.mutex);
        if (const auto found = state.devices.find(device); found != state.devices.end()) allocate = found->second.allocate;
    }
    if (allocate == nullptr) return VK_ERROR_INITIALIZATION_FAILED; // unreachable: handed out only after resolving the real entry for this device
    const auto result = allocate(device, info, callbacks, memory);
    try {
        std::lock_guard lock(state.mutex);
        if (result != VK_SUCCESS || memory == nullptr || *memory == VK_NULL_HANDLE) {
            ++state.totals.failedAllocations;
            return result;
        }
        const auto found = state.devices.find(device);
        const auto record = classify(found != state.devices.end() ? &found->second : nullptr, info, device == state.firstRegistered);
        const auto key = std::make_pair(device, *memory);
        if (const auto existing = state.live.find(key); existing != state.live.end()) {
            // The driver reused a handle we still consider live (a free we did not see): replace it.
            ++state.totals.duplicateHandles;
            add(state.totals, existing->second, false);
            state.live.erase(existing);
        }
        state.live.emplace(key, record);
        add(state.totals, record, true);
        ++state.totals.allocations;
    } catch (...) {
        droppedAccounting.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    // Allocation phases (imports, mirrors, pools) can run before the first dispatch reports; the
    // ledger rate-limits itself, catches its own errors, and no accounting lock is held here.
    MemoryLedger::Report();
    return result;
}

VKAPI_ATTR void VKAPI_CALL CountedFree(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* callbacks) {
    auto& state = Shared();
    PFN_vkFreeMemory release = nullptr;
    {
        std::lock_guard lock(state.mutex);
        if (const auto found = state.devices.find(device); found != state.devices.end()) release = found->second.free;
        if (memory == VK_NULL_HANDLE) {
            ++state.totals.nullFrees;
        } else if (const auto record = state.live.find(std::make_pair(device, memory)); record != state.live.end()) {
            // Removed before the real free, so a handle the driver reuses immediately afterwards is
            // never mistaken for this one. map::erase and arithmetic do not throw.
            add(state.totals, record->second, false);
            state.live.erase(record);
            ++state.totals.frees;
        } else {
            ++state.totals.untrackedFrees;
        }
    }
    if (release != nullptr) release(device, memory, callbacks);
}

template<bool Graphics>
VKAPI_ATTR VkResult VKAPI_CALL TimedCreatePipelines(VkDevice device, VkPipelineCache cache, std::uint32_t count,
        const std::conditional_t<Graphics, VkGraphicsPipelineCreateInfo, VkComputePipelineCreateInfo>* infos,
        const VkAllocationCallbacks* callbacks, VkPipeline* pipelines) {
    using Function = std::conditional_t<Graphics, PFN_vkCreateGraphicsPipelines, PFN_vkCreateComputePipelines>;
    Function function = nullptr;
    {
        auto& state = Shared();
        std::lock_guard lock(state.mutex);
        if (const auto found = state.devices.find(device); found != state.devices.end()) {
            if constexpr (Graphics) function = found->second.graphics;
            else function = found->second.compute;
        }
    }
    if (!function) return VK_ERROR_INITIALIZATION_FAILED;
    const auto id = pipelineCallId.fetch_add(1, std::memory_order_relaxed) + 1;
    std::fprintf(stderr, "[pipeline-create-begin] id=%llu kind=%s count=%u cache=%u\n", static_cast<unsigned long long>(id), Graphics ? "graphics" : "compute", count, cache != VK_NULL_HANDLE ? 1u : 0u);
    const auto start = std::chrono::steady_clock::now();
    const auto result = function(device, cache, count, infos, callbacks, pipelines); // no diagnostic lock held
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    std::fprintf(stderr, "[pipeline-create] id=%llu kind=%s count=%u cache=%u result=%d elapsed_ns=%llu\n", static_cast<unsigned long long>(id), Graphics ? "graphics" : "compute", count, cache != VK_NULL_HANDLE ? 1u : 0u, static_cast<int>(result), static_cast<unsigned long long>(ns));
    return result;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CountingResolver(VkDevice device, const char* name) {
    const auto real = realResolver.load(std::memory_order_acquire);
    if (real == nullptr) return nullptr;
    const auto function = real(device, name);
    if (function == nullptr || name == nullptr) return function;
    const bool allocate = std::strcmp(name, "vkAllocateMemory") == 0;
    const bool free = std::strcmp(name, "vkFreeMemory") == 0;
    const bool compute = std::strcmp(name, "vkCreateComputePipelines") == 0;
    const bool graphics = std::strcmp(name, "vkCreateGraphicsPipelines") == 0;
    if (!allocate && !free && !compute && !graphics) return function;
    try {
        auto& state = Shared();
        std::lock_guard lock(state.mutex);
        auto& entry = state.devices[device];
        if (allocate) entry.allocate = reinterpret_cast<PFN_vkAllocateMemory>(function);
        else if (free) entry.free = reinterpret_cast<PFN_vkFreeMemory>(function);
        else if (compute) entry.compute = reinterpret_cast<PFN_vkCreateComputePipelines>(function);
        else entry.graphics = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(function);
    } catch (...) {
        // Not recorded: hand out the real entry point; this caller's allocations go uncounted.
        droppedAccounting.fetch_add(1, std::memory_order_relaxed);
        return function;
    }
    if (allocate) return reinterpret_cast<PFN_vkVoidFunction>(&CountedAllocate);
    if (free) return reinterpret_cast<PFN_vkVoidFunction>(&CountedFree);
    return compute ? reinterpret_cast<PFN_vkVoidFunction>(&TimedCreatePipelines<false>) : reinterpret_cast<PFN_vkVoidFunction>(&TimedCreatePipelines<true>);
}

std::string mib(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

}

const char* ClassName(MemoryClass memoryClass) {
    switch (memoryClass) {
        case MemoryClass::ImportedHost: return "imported_host";
        case MemoryClass::DeviceLocalOnly: return "device_local_only";
        case MemoryClass::DeviceLocalMappable: return "device_local_host_visible";
        case MemoryClass::HostVisibleOnly: return "host_visible_only";
        case MemoryClass::OtherOrUnknownType: return "other_or_unknown_type";
        case MemoryClass::Count: break;
    }
    return "invalid";
}

bool Enabled() {
    const auto force = forced.load(std::memory_order_relaxed);
    if (force >= 0) return force != 0;
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_MEMORY_LEDGER");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void SetEnabledForTesting(bool enabled) {
    forced.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

PFN_vkGetDeviceProcAddr WrapDeviceProc(PFN_vkGetDeviceProcAddr real) {
    if (!Enabled() || real == nullptr || real == &CountingResolver) return real;
    realResolver.store(real, std::memory_order_release);
    // Start the diagnostic clock as soon as Vulkan device entry points are exposed, before
    // the first memory allocation or game frame. This captures load-time growth, not only draw-time.
    MemoryLedger::Report();
    return &CountingResolver;
}

void RegisterDevice(VkDevice device, const VkPhysicalDeviceMemoryProperties& properties, VkPhysicalDevice physicalDevice, PFN_vkGetPhysicalDeviceMemoryProperties2 properties2, bool memoryBudget) {
    try {
        auto& state = Shared();
        std::lock_guard lock(state.mutex);
        auto& entry = state.devices[device];
        entry.properties = properties;
        entry.registered = true;
        entry.physicalDevice = physicalDevice;
        entry.properties2 = properties2;
        entry.memoryBudget = memoryBudget;
        if (state.firstRegistered == VK_NULL_HANDLE) {
            state.firstRegistered = device;
            state.totals.heapCount = properties.memoryHeapCount;
        }
    } catch (...) {
        droppedAccounting.fetch_add(1, std::memory_order_relaxed);
    }
}

void UnregisterDevice(VkDevice device) {
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    for (auto it = state.live.begin(); it != state.live.end();) {
        if (it->first.first == device) {
            add(state.totals, it->second, false);
            it = state.live.erase(it);
        } else {
            ++it;
        }
    }
    state.devices.erase(device);
    if (state.firstRegistered == device) {
        state.firstRegistered = VK_NULL_HANDLE;
        state.totals.heapCount = 0;
    }
}

Totals Snapshot() {
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    auto totals = state.totals; // one consistent snapshot under the accounting lock
    totals.droppedAccounting = droppedAccounting.load(std::memory_order_relaxed);
    return totals;
}

std::string Describe() {
    try {
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        PFN_vkGetPhysicalDeviceMemoryProperties2 properties2 = nullptr;
        bool budget = false;
        std::size_t devices = 0;
        Totals totals;
        {
            auto& state = Shared();
            std::lock_guard lock(state.mutex);
            totals = state.totals;
            for (const auto& [handle, info] : state.devices) devices += info.registered ? 1u : 0u;
            if (const auto found = state.devices.find(state.firstRegistered); found != state.devices.end()) {
                physical = found->second.physicalDevice;
                properties2 = found->second.properties2;
                budget = found->second.memoryBudget;
            }
        }
        std::string text = "vk_memory (all devices) live MiB/count:";
        for (std::size_t i = 0; i < index(MemoryClass::Count); ++i) {
            text += " ";
            text += ClassName(static_cast<MemoryClass>(i));
            text += "=" + mib(totals.liveBytes[i]) + "/" + std::to_string(totals.liveCount[i]);
        }
        text += " | allocs=" + std::to_string(totals.allocations) + " failed=" + std::to_string(totals.failedAllocations) + " frees=" + std::to_string(totals.frees) + " untracked_frees=" + std::to_string(totals.untrackedFrees) + " duplicate_handles=" + std::to_string(totals.duplicateHandles) + " dropped_accounting=" + std::to_string(droppedAccounting.load(std::memory_order_relaxed)) + " registered_devices=" + std::to_string(devices);
        text += " | other_devices_live=" + mib(totals.otherDeviceBytes);
        if (totals.heapCount == 0) {
            text += " | first_device heaps: unknown (no device registered)";
            return text;
        }
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        query.pNext = &budgetProperties;
        const bool haveBudget = budget && properties2 != nullptr && physical != VK_NULL_HANDLE;
        if (haveBudget) properties2(physical, &query);
        // Heap bytes and the driver budget both describe ONLY the first registered device.
        for (std::uint32_t heap = 0; heap < totals.heapCount && heap < VK_MAX_MEMORY_HEAPS; ++heap) {
            text += " | first_device heap" + std::to_string(heap) + " tracked=" + mib(totals.heapLiveBytes[heap]);
            if (haveBudget) text += " driver_usage=" + mib(budgetProperties.heapUsage[heap]) + " budget=" + mib(budgetProperties.heapBudget[heap]);
            else text += " driver_usage=unknown budget=unknown";
        }
        return text;
    } catch (...) {
        droppedAccounting.fetch_add(1, std::memory_order_relaxed);
        return "vk_memory description unavailable (diagnostic error)";
    }
}

void ResetForTesting() {
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    state.live.clear();
    state.devices.clear();
    state.firstRegistered = VK_NULL_HANDLE;
    state.totals = Totals{};
    droppedAccounting.store(0, std::memory_order_relaxed);
    realResolver.store(nullptr, std::memory_order_release);
}

}
