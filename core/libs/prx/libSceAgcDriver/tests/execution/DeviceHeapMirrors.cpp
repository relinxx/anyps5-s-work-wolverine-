#include "prx/libSceAgcDriver/Execution/include/BdaFeatures.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/HeapWritebackSnapshot.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <SDL_loadso.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
namespace {
using namespace AgcDriver::Graphics;
#ifdef _WIN32
void FragmentedSnapshot() {
    if (std::getenv("APS5_STREAM_HEAP_WRITEBACK_MIB") == nullptr) return;
    std::vector<std::byte> input(HeapWritebackSnapshot::MaxChunkBytes);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = (i / HeapWritebackSnapshot::BlockBytes) % 2 == 0 ? std::byte{0} : std::byte{static_cast<unsigned char>(i % 251)};
    HeapWritebackSnapshot snapshot;
    const auto start = std::chrono::steady_clock::now();
    snapshot.Append(input);
    const auto captured = std::chrono::steady_clock::now();
    std::vector<std::byte> output(input.size());
    snapshot.Read(0, output);
    const auto finished = std::chrono::steady_clock::now();
    Require(output == input, "fragmented snapshot did not preserve exact mixed and uniform blocks");
    Require(snapshot.storedBytes == input.size() / 2, "fragmented snapshot stored uniform blocks on disk");
    Require(snapshot.writeCalls == 1 && snapshot.readCalls == 1, "uniform gaps fragmented bounded snapshot I/O");
    std::printf("fragmented snapshot: 16 MiB alternating blocks, capture %.3f ms, read %.3f ms\n",
        std::chrono::duration<double, std::milli>(captured - start).count(),
        std::chrono::duration<double, std::milli>(finished - captured).count());
}

// A retained snapshot recaptured over changed bytes must read back exactly the new bytes,
// rewriting only the changed mixed blocks; a uniform block turning mixed must be refused.
void RecapturedSnapshot() {
    if (std::getenv("APS5_STREAM_HEAP_WRITEBACK_MIB") == nullptr) return;
    constexpr auto Block = HeapWritebackSnapshot::BlockBytes;
    // Two chunks, the second ending in a short block, as a heap range not a multiple of 64 KiB.
    // Block 260 (the short one) is mixed; blocks 3 and 6 are uniform.
    const std::size_t size = HeapWritebackSnapshot::MaxChunkBytes + 4 * Block + 4096;
    std::vector<std::byte> input(size);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = (i / Block) % 3 == 0 ? std::byte{0x5a} : std::byte{static_cast<unsigned char>(i % 253)};
    const auto chunks = [&](const std::vector<std::byte>& bytes, auto&& take) {
        for (std::size_t at = 0; at < bytes.size(); at += HeapWritebackSnapshot::MaxChunkBytes)
            if (!take(at, std::span<const std::byte>(bytes).subspan(at, std::min(HeapWritebackSnapshot::MaxChunkBytes, bytes.size() - at)))) return false;
        return true;
    };
    const auto readAll = [&](HeapWritebackSnapshot& snapshot) {
        std::vector<std::byte> output(size);
        for (std::size_t at = 0; at < size; at += HeapWritebackSnapshot::MaxChunkBytes)
            snapshot.Read(at, std::span(output).subspan(at, std::min(HeapWritebackSnapshot::MaxChunkBytes, size - at)));
        return output;
    };
    HeapWritebackSnapshot snapshot;
    chunks(input, [&](std::size_t, std::span<const std::byte> part) { snapshot.Append(part); return true; });
    Require(readAll(snapshot) == input, "fresh snapshot differs from its input");
    std::vector<std::byte> previous;
    // Unchanged: nothing rewritten.
    snapshot.rewrittenBytes = 0;
    Require(chunks(input, [&](std::size_t at, std::span<const std::byte> part) { return snapshot.Recapture(at, part, previous); }), "unchanged recapture refused");
    Require(snapshot.rewrittenBytes == 0, "unchanged recapture rewrote spooled bytes");
    // One byte in a mixed block of each chunk (the second in the short final block), and a
    // uniform block taking another uniform value.
    auto changed = input;
    changed[Block + 7] = std::byte{0xee};
    changed[size - 1] = std::byte{0x11};
    std::fill_n(changed.begin() + 3 * Block, Block, std::byte{0x00});
    snapshot.rewrittenBytes = 0;
    Require(chunks(changed, [&](std::size_t at, std::span<const std::byte> part) { return snapshot.Recapture(at, part, previous); }), "recapture of mixed and uniform changes refused");
    Require(readAll(snapshot) == changed, "recaptured snapshot does not hold the new bytes exactly");
    Require(snapshot.rewrittenBytes == Block + 4096, "recapture rewrote more than the changed mixed blocks");
    // A uniform block turning mixed cannot be held in place.
    auto mixed = changed;
    mixed[6 * Block + 3] = std::byte{0x01};
    Require(!chunks(mixed, [&](std::size_t at, std::span<const std::byte> part) { return snapshot.Recapture(at, part, previous); }), "recapture accepted a uniform block turning mixed");
    std::printf("recaptured snapshot: exact after mixed and uniform changes, rewrote %llu bytes\n", static_cast<unsigned long long>(Block + 4096));
}
#endif
class Device {
public:
    Device() {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        Require(library != nullptr, "cannot load Vulkan");
        try {
            instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
            Require(instanceProc != nullptr, "missing Vulkan instance resolver");
            VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &application;
            Check(function<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance), "vkCreateInstance");
            std::uint32_t count = 0;
            const auto enumerate = function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
            Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
            Require(count != 0, "no Vulkan device");
            std::vector<VkPhysicalDevice> devices(count);
            Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
            const auto rankDeviceType = [](VkPhysicalDeviceType type) {
                switch (type) {
                    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
                    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
                    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 1;
                    default: return 0;
                }
            };
            const auto physicalProperties = function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
            int selectedRank = -1;
            for (const auto physical : devices) {
                VkPhysicalDeviceProperties candidate{};
                physicalProperties(physical, &candidate);
                if (candidate.apiVersion < VK_API_VERSION_1_1) continue;
                const int rank = rankDeviceType(candidate.deviceType);
                if (rank <= selectedRank) continue;
                context.physical = physical;
                selectedRank = rank;
                cpu = candidate.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            }
            Require(context.physical != VK_NULL_HANDLE, "no Vulkan 1.1 device");
            const auto extensions = function<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
            Check(extensions(context.physical, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties");
            std::vector<VkExtensionProperties> available(count);
            Check(extensions(context.physical, nullptr, &count, available.data()), "vkEnumerateDeviceExtensionProperties");
            auto bytes = AgcDriver::QueryBdaByteFeatures(context.physical, function<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), available);
            auto address = AgcDriver::QueryBdaFeatures(context.physical, function<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), available);
            const auto queues = function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
            queues(context.physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            queues(context.physical, &count, families.data());
            std::uint32_t family = 0;
            while (family < count && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) ++family;
            Require(family < count, "no Vulkan compute queue");
            const float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkPhysicalDeviceFeatures enabled{};
            enabled.shaderInt64 = VK_TRUE;
            address.pNext = &bytes;
            const std::array<const char*, 2> extensionsEnabled{VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_8BIT_STORAGE_EXTENSION_NAME};
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &address};
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos = &queue;
            device.enabledExtensionCount = 2;
            device.ppEnabledExtensionNames = extensionsEnabled.data();
            device.pEnabledFeatures = &enabled;
            Check(function<PFN_vkCreateDevice>("vkCreateDevice")(context.physical, &device, nullptr, &context.device), "vkCreateDevice");
            context.deviceProc = function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
            function<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(context.physical, &context.memory);
            VkPhysicalDeviceProperties properties{};
            function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &properties);
            context.limits = properties.limits;
            context.bufferDeviceAddress = true;
            context.Function<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(context.device, family, 0, &context.queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = family;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &pool, nullptr, &context.pool), "vkCreateCommandPool");
        } catch (...) {
            release();
            throw;
        }
    }

    ~Device() { release(); }
    const Context& GetContext() const { return context; }
    bool RunsOnCpu() const { return cpu; }

private:
    template<typename TFunction>
    TFunction function(const char* name) const {
        const auto result = reinterpret_cast<TFunction>(instanceProc(instance, name));
        Require(result != nullptr, name);
        return result;
    }

    void release() noexcept {
        if (context.pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, context.pool, nullptr);
        context.bufferPool.reset();
        if (context.device != VK_NULL_HANDLE) function<PFN_vkDestroyDevice>("vkDestroyDevice")(context.device, nullptr);
        if (instance != VK_NULL_HANDLE) function<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (library != nullptr) SDL_UnloadObject(library);
    }

    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    Context context{};
    bool cpu = false;
};

void Run(const Context& context) {
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    constexpr std::size_t bytes = (64u << 20u) + 65536;
    void* raw = GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536);
#ifdef _WIN32
    GuestArena::GuestArenaCommit_nid_postfix(raw, bytes, PAGE_READWRITE, bytes);
#endif
    auto* guest = static_cast<std::byte*>(raw);
    const auto address = reinterpret_cast<std::uintptr_t>(raw);
    std::memset(guest, 0x31, bytes);
#ifdef _WIN32
    const bool pressureSourceTest = std::getenv("APS5_TRIM_PRESSURE_SOURCE_FREE_MIB") != nullptr;
    const bool trimTest = pressureSourceTest || std::getenv("APS5_TRIM_MIRROR_WORKING_SET_MIB") != nullptr || std::getenv("APS5_TRIM_REFRESH_WORKING_SET_MIB") != nullptr;
    if (trimTest) Require(VirtualLock(guest, 4096) != 0, "real trim test cannot lock its control page");
    const auto freeControl = [](void* page) { if (page != nullptr) VirtualFree(page, 0, MEM_RELEASE); };
    std::unique_ptr<void, decltype(freeControl)> unrelated(
        pressureSourceTest ? VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : nullptr, freeControl);
    if (pressureSourceTest) Require(unrelated != nullptr, "pressure trim test cannot allocate unrelated control page");
#endif
    GuestAllocations::Mutation().Add(raw, bytes, true, true, true);
    const auto releaseGuest = [&](void* pointer) {
        DropAddressSpaceCache();
        GuestAllocations::Mutation().Remove(pointer);
        ClearImageMirrors(context.device);
        ClearHostImports(context.device);
        GuestArena::GuestArenaReset_nid_postfix(pointer, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(pointer, bytes);
    };
    std::unique_ptr<void, decltype(releaseGuest)> guestCleanup(raw, releaseGuest);
    const auto descriptor = [&](GuestBufferMemory& memory) {
        std::uint32_t adjustment = 0;
        const auto view = memory.Descriptor(address, bytes, adjustment);
        Require(adjustment == 0, "real device heap descriptor base was adjusted");
        return view;
    };
    {
        GuestBufferMemory memory(context);
        memory.AcquireRegistered();
        constexpr std::size_t start = (16u << 20u) - 7;
        memory.AddWritable(address + start, 29);
#ifdef _WIN32
        if (pressureSourceTest) std::memset(unrelated.get(), 0x6a, 4096);
#endif
        memory.Upload(true);
#ifdef _WIN32
        if (trimTest) {
            std::vector<PSAPI_WORKING_SET_EX_INFORMATION> pages(bytes / 4096);
            for (std::size_t at = 0; at < pages.size(); ++at) pages[at].VirtualAddress = guest + at * 4096;
            Require(K32QueryWorkingSetEx(GetCurrentProcess(), pages.data(), static_cast<DWORD>(pages.size() * sizeof(pages.front()))) != 0, "real trim test cannot query residency");
            const auto resident = std::count_if(pages.begin(), pages.end(), [](const auto& page) { return page.VirtualAttributes.Valid; });
            Require(pages.front().VirtualAttributes.Valid && pages.front().VirtualAttributes.Locked, "working-set trim unlocked a locked guest page");
            Require(resident < static_cast<std::ptrdiff_t>(pages.size() / 8), "working-set trim did not release the unlocked source pages");
            if (pressureSourceTest) {
                PSAPI_WORKING_SET_EX_INFORMATION control{};
                control.VirtualAddress = unrelated.get();
                Require(K32QueryWorkingSetEx(GetCurrentProcess(), &control, sizeof(control)) != 0,
                    "pressure trim test cannot query unrelated residency");
                Require(control.VirtualAttributes.Valid, "targeted pressure trim evicted unrelated process memory");
                Require(!control.VirtualAttributes.Locked, "unrelated control page unexpectedly locked");
                const auto* data = static_cast<const std::byte*>(unrelated.get());
                Require(std::all_of(data, data + 4096, [](std::byte byte) { return byte == std::byte{0x6a}; }),
                    "pressure trim changed unrelated page contents");
                std::printf("targeted pressure trim preserved the unrelated unlocked resident control page\n");
            }
            const auto before = MirrorCounters().blocksCopied;
            {
                GuestBufferMemory unchanged(context);
                unchanged.AcquireRegistered();
                unchanged.Upload(true);
                unchanged.WriteBack();
            }
            Require(MirrorCounters().blocksCopied == before, "working-set trim made unchanged source pages appear dirty");
            Require(VirtualUnlock(guest, 4096) != 0, "real trim test cannot unlock its control page");
            std::printf("working-set trim retained the locked page; resident source pages=%llu of %llu, unchanged write-watch verified\n", static_cast<unsigned long long>(resident), static_cast<unsigned long long>(pages.size()));
        }
#endif
        Require(MirrorCounters().deviceHeapBytes >= bytes, "real transfer test did not use a device heap mirror");
        const auto view = descriptor(memory);
        Buffer downloaded(context, 16u << 20u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        for (std::size_t at = 0; at < bytes;) {
            const auto count = std::min<std::size_t>(bytes - at, 16u << 20u);
            CommandBatch batch(context);
            RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            CopyBuffer(context, batch.Handle(), view.buffer, view.offset + at, downloaded.Handle(), 0, count);
            RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            batch.SubmitAndWait();
            Require(std::memcmp(downloaded.Bytes().data(), guest + at, count) == 0, "real GPU device heap upload lost bytes across chunk boundaries");
            at += count;
        }
        guest[start + 1] = std::byte{0x51};
        const std::uint32_t first = 0x31316131;
        const std::uint32_t last = 0x31316231;
        {
            CommandBatch batch(context);
            context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(batch.Handle(), view.buffer, view.offset + (start & ~std::size_t{3}), 4, &first);
            context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(batch.Handle(), view.buffer, view.offset + ((start + 28) & ~std::size_t{3}), 4, &last);
            RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
            batch.SubmitAndWait();
        }
        memory.WriteBack();
        Require(guest[start] == std::byte{0x61} && guest[start + 28] == std::byte{0x62} && guest[start + 1] == std::byte{0x51}, "real GPU device heap write-back lost GPU writes or concurrent CPU bytes");
    }
    {
        auto memory = std::make_shared<GuestBufferMemory>(context);
        memory->AcquireRegistered();
        memory->AddWritable(address + 7, 9);
        memory->Upload(true);
        const auto view = descriptor(*memory);
        Recorder recorder(context);
        recorder.Activate();
        const std::uint32_t first = 0x71313131;
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(recorder.Commands(), view.buffer, view.offset + 4, 4, &first);
        memory->RecordCopyBacks(recorder);
        bool firstDone = false;
        bool laterSawFirstDone = false;
        Require(recorder.AfterRecordedWork([&] { memory->WriteBack(); firstDone = true; }), "real GPU result has no completion");
        recorder.Submit();
        guest[8] = std::byte{0x72};
        const std::uint32_t later = 0x73313131;
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(recorder.Commands(), view.buffer, view.offset + 4, 4, &later);
        Require(recorder.AfterRecordedWork([&] { laterSawFirstDone = firstDone; }), "real GPU later completion was not recorded");
        recorder.Submit();
        recorder.Sync();
        Require(firstDone && laterSawFirstDone && guest[7] == std::byte{0x71} && guest[8] == std::byte{0x72}, "real GPU immutable result or completion order failed");
    }
    // Descriptor-only builds share the existing heap buffer and pin its registration.
    // No address-space acquisition on the descriptor build itself.
    VkBuffer heapBuffer = VK_NULL_HANDLE;
    {
        GuestBufferMemory whole(context);
        whole.AcquireRegistered();
        whole.Upload(true);
        heapBuffer = descriptor(whole).buffer;
        whole.WriteBack();
    }
    constexpr std::size_t descriptorOffset = 65536;
    {
        // An unchanged GPU result over several transfer chunks must preserve a
        // CPU store made after the GPU reference snapshot was captured.
        GuestBufferMemory unchanged(context);
        unchanged.AddWritable(address + descriptorOffset, bytes - descriptorOffset);
        unchanged.Upload(false);
        const auto cpuOffset = descriptorOffset + (16u << 20u) + 512;
        guest[cpuOffset] = std::byte{0x73};
        unchanged.WriteBack();
        Require(guest[cpuOffset] == std::byte{0x73}, "unchanged GPU chunks overwrote a concurrent CPU store");
        Require(unchanged.OwnedBufferBytes() == 0 && !unchanged.HoldsLease(), "unchanged write-back retained its snapshots or registration");
    }
    {
        guest[descriptorOffset] = std::byte{0x41};
        GuestBufferMemory read(context);
        read.AddReadable(address + descriptorOffset, 32);
        read.Upload(false);
        std::uint32_t adjustment = 0;
        const auto view = read.Descriptor(address + descriptorOffset, 32, adjustment);
        Require(view.buffer == heapBuffer && adjustment == 0 && read.HoldsLease(), "descriptor read copied its heap or did not pin the registration");
        Require(read.OwnedBufferBytes() == 0, "read-only descriptor accounted the persistent shared heap as a transient copy");
        Buffer downloaded(context, 32, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        CommandBatch batch(context);
        CopyBuffer(context, batch.Handle(), view.buffer, view.offset, downloaded.Handle(), 0, 32);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        Require(std::memcmp(downloaded.Bytes().data(), guest + descriptorOffset, 32) == 0, "descriptor heap read missed a CPU update");
        read.WriteBack();
        Require(!read.HoldsLease(), "descriptor read retained the registration after completion");
    }
    {
        std::memset(guest + descriptorOffset, 0x31, 32);
        GuestBufferMemory write(context);
        write.AddWritable(address + descriptorOffset, 32);
        write.Upload(false);
        std::uint32_t adjustment = 0;
        const auto view = write.Descriptor(address + descriptorOffset, 32, adjustment);
        Require(view.buffer == heapBuffer && write.HoldsLease(), "writable descriptor did not share and pin the heap");
        Require(write.OwnedBufferBytes() >= 64, "writable descriptor omitted its GPU reference/result bytes");
        guest[descriptorOffset + 1] = std::byte{0x51};
        const std::uint32_t first = 0x31313161;
        const std::uint32_t last = 0x62313131;
        CommandBatch batch(context);
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(batch.Handle(), view.buffer, view.offset, 4, &first);
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(batch.Handle(), view.buffer, view.offset + 28, 4, &last);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
        batch.SubmitAndWait();
        write.WriteBack();
        Require(guest[descriptorOffset] == std::byte{0x61} && guest[descriptorOffset + 1] == std::byte{0x51} && guest[descriptorOffset + 31] == std::byte{0x62}, "descriptor write-back lost GPU changes or concurrent CPU bytes");
        Require(!write.HoldsLease(), "descriptor write retained the registration after completion");
    }
    {
        GuestBufferMemory stale(context);
        stale.AddReadable(address + descriptorOffset, 32);
        stale.UploadPrepare(false);
        DropAddressSpaceCache();
        GuestAllocations::Mutation().Remove(raw);
        GuestAllocations::Mutation().Add(raw, bytes, true, true, true);
        guest[descriptorOffset] = std::byte{0x55};
        stale.UploadFinish(false);
        std::uint32_t adjustment = 0;
        const auto view = stale.Descriptor(address + descriptorOffset, 32, adjustment);
        Require(view.buffer != heapBuffer, "descriptor used a heap mirror from a replaced registration");
        stale.WriteBack();
    }
    std::puts("descriptor heap sharing passed: CPU refresh, changed-byte write-back, lease release and stale-registration fallback");
    {
        // A single oversized resource must finish safely rather than accumulate beyond
        // the configured limit. Count actual result/reference allocations, not the heap.
        GuestBufferMemory whole(context);
        whole.AcquireRegistered();
        whole.Upload(true);
        whole.WriteBack();
        auto memory = std::make_shared<GuestBufferMemory>(context);
        constexpr std::size_t writtenBytes = (1u << 20u) + 4;
        memory->AddWritable(address + descriptorOffset, writtenBytes);
        memory->Upload(false);
        const auto owned = memory->OwnedBufferBytes();
        Require(owned >= 2 * writtenBytes && owned < bytes, "transient memory accounting omitted references or counted the persistent heap");
        std::uint32_t adjustment = 0;
        const auto view = memory->Descriptor(address + descriptorOffset, writtenBytes, adjustment);
        Recorder recorder(context);
        recorder.Activate();
        const std::uint32_t value = 0x66666666;
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(recorder.Commands(), view.buffer, view.offset, 4, &value);
        memory->RecordCopyBacks(recorder);
        bool completed = false;
        Require(recorder.AfterRecordedWork([&] { memory->WriteBack(); completed = true; }), "budget test has no completion");
        recorder.Keep(memory, owned);
        recorder.BoundKeptBytes();
        Require(completed && recorder.InFlightKeptBytes() == 0 && guest[descriptorOffset] == std::byte{0x66}, "retained-byte limit did not complete oversized GPU work and write-back safely");
        Require(memory->OwnedBufferBytes() == 0, "completed heap write retained its GPU snapshots");
    }
    std::puts("transient GPU reference accounting and oversized-work backpressure passed");
    // Re-registration must reuse a compatible allocation only after previous GPU
    // reads finish. Keep a queued read of the old bytes across replacement.
    VkBuffer previousBuffer = VK_NULL_HANDLE;
    {
        GuestBufferMemory before(context);
        before.AcquireRegistered();
        before.Upload(true);
        previousBuffer = descriptor(before).buffer;
    }
    DropAddressSpaceCache();
    Buffer previousRead(context, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    {
        const auto oldByte = guest[4096];
        Recorder recorder(context);
        recorder.Activate();
        RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        CopyBuffer(context, recorder.Commands(), previousBuffer, 4096, previousRead.Handle(), 0, 4);
        RecordMemoryBarrier(context, recorder.Commands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Submit();
        GuestAllocations::Mutation().Remove(raw);
        GuestAllocations::Mutation().Add(raw, bytes, true, true, true);
        guest[4096] = std::byte{0x45};
        GuestBufferMemory after(context);
        after.AcquireRegistered();
        after.Upload(true);
        const auto view = descriptor(after);
        Require(view.buffer == previousBuffer, "compatible re-registration allocated another device heap buffer");
        Require(previousRead.Bytes()[0] == oldByte, "device heap reuse overwrote data before an earlier GPU read completed");
        CommandBatch batch(context);
        CopyBuffer(context, batch.Handle(), view.buffer, 4096, previousRead.Handle(), 0, 4);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        Require(previousRead.Bytes()[0] == std::byte{0x45}, "reused device heap buffer did not refresh replacement bytes");
    }
    std::puts("compatible heap re-registration reused the allocation after earlier GPU reads and refreshed new data");
    {
#ifdef _WIN32
        const bool refreshTrim = trimTest && std::getenv("APS5_TRIM_REFRESH_SOURCE_RANGE") != nullptr;
        if (refreshTrim) Require(VirtualLock(guest, 4096) != 0, "refresh trim cannot lock its control page");
#endif
        std::memset(guest, 0x49, bytes);
        GuestBufferMemory refreshed(context);
        refreshed.AcquireRegistered();
        refreshed.Upload(true);
#ifdef _WIN32
        if (refreshTrim) {
            std::vector<PSAPI_WORKING_SET_EX_INFORMATION> pages(bytes / 4096);
            for (std::size_t at = 0; at < pages.size(); ++at) pages[at].VirtualAddress = guest + at * 4096;
            Require(K32QueryWorkingSetEx(GetCurrentProcess(), pages.data(), static_cast<DWORD>(pages.size() * sizeof(pages.front()))) != 0, "refresh trim cannot query residency");
            const auto resident = std::count_if(pages.begin(), pages.end(), [](const auto& page) { return page.VirtualAttributes.Valid; });
            Require(pages.front().VirtualAttributes.Valid && pages.front().VirtualAttributes.Locked, "refresh trim unlocked the control page");
            Require(resident < static_cast<std::ptrdiff_t>(pages.size() / 8), "large mirror refresh retained its unlocked source working set");
            const auto copied = MirrorCounters().blocksCopied;
            GuestBufferMemory unchanged(context);
            unchanged.AcquireRegistered();
            unchanged.Upload(true);
            unchanged.WriteBack();
            Require(MirrorCounters().blocksCopied == copied, "refresh trim changed write-watch state");
            Require(VirtualUnlock(guest, 4096) != 0, "refresh trim cannot unlock the control page");
            std::puts("large refresh trimmed source pages, kept the locked control page and preserved write-watch");
        }
#endif
        const auto view = descriptor(refreshed);
        Buffer downloaded(context, 16u << 20u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        for (std::size_t at = 0; at < bytes;) {
            const auto count = std::min<std::size_t>(bytes - at, 16u << 20u);
            CommandBatch batch(context);
            RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            CopyBuffer(context, batch.Handle(), view.buffer, view.offset + at, downloaded.Handle(), 0, count);
            RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            batch.SubmitAndWait();
            Require(std::all_of(downloaded.Bytes().begin(), downloaded.Bytes().begin() + count, [](std::byte value) { return value == std::byte{0x49}; }), "large refresh lost GPU bytes across transfer chunks");
            at += count;
        }
        refreshed.WriteBack();
    }
    {
        const auto before = MirrorCounters().blocksCopied;
        GuestBufferMemory descriptorOnly(context);
        descriptorOnly.AddReadable(address, bytes);
        descriptorOnly.Upload(false);
        const auto view = descriptor(descriptorOnly);
        Require(MirrorCounters().blocksCopied == before, "clean descriptor recopied the device heap");
        descriptorOnly.WriteBack();
        GuestBufferMemory addressOnly(context);
        addressOnly.AcquireRegistered();
        addressOnly.Upload(true);
        Require(MirrorCounters().blocksCopied == before, "descriptor refresh invalidated heap tracking generations");
        addressOnly.WriteBack();

        // A partial descriptor must update a complete dirty tracking block,
        // including modified bytes outside the descriptor in that same block.
        guest[65536 + 4] = std::byte{0x72};
        guest[65536 + 128] = std::byte{0x73};
        GuestBufferMemory partial(context);
        partial.AddReadable(address + 65536, 16);
        partial.Upload(false);
        Require(MirrorCounters().blocksCopied == before + 1, "partial descriptor did not copy exactly its dirty tracking block");
        Buffer checked(context, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        CommandBatch batch(context);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        CopyBuffer(context, batch.Handle(), view.buffer, view.offset + 65536, checked.Handle(), 0, 256);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        Require(checked.Bytes()[4] == std::byte{0x72} && checked.Bytes()[128] == std::byte{0x73}, "partial descriptor left uncopied bytes outside its boundary");
        partial.WriteBack();
        GuestBufferMemory repeat(context);
        repeat.AddReadable(address, bytes);
        repeat.Upload(false);
        repeat.WriteBack();
        Require(MirrorCounters().blocksCopied == before + 1, "repeated descriptor recopied unchanged heap bytes");
        std::puts("clean heap descriptors skip copying; dirty partial descriptors preserve whole tracking blocks and generations");
    }
    if (std::getenv("APS5_STREAM_HEAP_WRITEBACK_MIB") != nullptr && std::getenv("APS5_GPU_HEAP_COMPARE") != nullptr) {
        // The pre-work reference stays in device memory; the GPU compares it with the result per
        // 64 KiB block and only changed blocks are downloaded and stored byte by byte.
        const auto pattern = [](std::size_t at) { return std::byte{static_cast<unsigned char>((at * 29) % 253)}; };
        for (std::size_t i = 0; i < bytes; ++i) guest[i] = pattern(i);
        std::vector<std::byte> expected(guest, guest + bytes);
        struct GpuWrite { std::size_t at; std::uint32_t value; };
        struct CpuWrite { std::size_t at; std::byte value; };
        const auto round = [&](const char* name, const std::vector<GpuWrite>& gpuWrites, const std::vector<CpuWrite>& cpuWrites, std::uint64_t changedBlocks) {
            const auto before = GpuCompareCounters();
            auto memory = std::make_shared<GuestBufferMemory>(context);
            // Unaligned edges: byte 0 and byte bytes-1 are view padding, never stored.
            memory->AddWritable(address + 1, bytes - 2);
            memory->Upload(false);
            Require(memory->RequiresImmediateWriteBack(), std::string(name) + ": GPU-compared heap write did not take the immediate write-back");
            Require(memory->OwnedBufferBytes() >= bytes - 2, std::string(name) + ": GPU-compared heap write did not keep its device reference");
            std::uint32_t adjustment = 0;
            const auto view = memory->Descriptor(address + 1, bytes - 2, adjustment);
            Require(adjustment == 1, std::string(name) + ": GPU compare test lost the unaligned view adjustment");
            Recorder recorder(context);
            recorder.Activate();
            for (const auto& write : gpuWrites) {
                context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(recorder.Commands(), view.buffer, view.offset + write.at, 4, &write.value);
                for (std::size_t k = 0; k < 4; ++k) {
                    const auto at = write.at + k;
                    if (at >= 1 && at < bytes - 1) expected[at] = std::byte{static_cast<unsigned char>(write.value >> (8 * k))};
                }
            }
            // CPU stores while the GPU work is outstanding, to bytes the GPU leaves unchanged.
            for (const auto& write : cpuWrites) {
                guest[write.at] = write.value;
                expected[write.at] = write.value;
            }
            memory->RecordCopyBacks(recorder);
            bool completed = false;
            recorder.OnComplete([&] { memory->WriteBack(); completed = true; });
            recorder.Keep(memory, memory->OwnedBufferBytes());
            memory->FinishBoundedWriteBack(recorder);
            Require(completed && !memory->RequiresImmediateWriteBack() && !memory->HoldsLease(), std::string(name) + ": GPU-compared write-back did not seal before later work");
            for (std::size_t i = 0; i < bytes; ++i) {
                if (guest[i] != expected[i]) {
                    char message[160];
                    std::snprintf(message, sizeof(message), "%s: GPU-compared write-back differs at byte %zu (got 0x%02x, expected 0x%02x)", name, i, static_cast<unsigned>(guest[i]), static_cast<unsigned>(expected[i]));
                    throw std::runtime_error(message);
                }
            }
            const auto after = GpuCompareCounters();
            Require(after.compares == before.compares + 1, std::string(name) + ": write-back did not run exactly one GPU compare");
            Require(after.changedBlocks - before.changedBlocks == changedBlocks, std::string(name) + ": GPU compare flagged blocks other than the changed ones");
            Require(after.downloadedBytes - before.downloadedBytes == changedBlocks * 65536, std::string(name) + ": GPU compare downloaded more than the changed blocks");
        };
        const auto word = [&](std::size_t at) {
            std::uint32_t value = 0;
            std::memcpy(&value, guest + at, 4);
            return value;
        };
        constexpr std::size_t block = 65536;
        constexpr std::size_t lastBlock = bytes / block - 1;
        // Unchanged: no GPU stores, a GPU store of the bytes already there, and a concurrent CPU store.
        round("unchanged", {{10 * block + 40, word(10 * block + 40)}}, {{300, std::byte{0x44}}}, 0);
        // Sparse: three far-apart blocks; a CPU store inside one of them must survive.
        round("sparse", {{2 * block + 64, 0x11223344u}, {700 * block + 12, 0x55667788u}, {1000 * block, 0x99aabbccu}}, {{2 * block + 200, std::byte{0x5c}}}, 3);
        // Boundaries: view padding at both ends (bytes 0 and bytes-1 must not be stored), and the
        // last word of one block next to the first word of the following one (one coalesced run).
        round("boundary", {{0, 0xa1a2a3a4u}, {bytes - 4, 0xb1b2b3b4u}, {6 * block - 4, 0xc1c2c3c4u}, {6 * block, 0xd1d2d3d4u}}, {}, 4);
        Require(guest[0] == pattern(0) && guest[bytes - 1] == pattern(bytes - 1), "GPU compare stored view padding");
        // Repeated: the same blocks written again with new values, then once more unchanged.
        round("repeated", {{2 * block + 64, 0x0badf00du}, {700 * block + 12, 0x12345678u}, {1000 * block, 0x87654321u}}, {{lastBlock * block + 8, std::byte{0x6d}}}, 3);
        round("repeated-unchanged", {}, {}, 0);
        std::puts("GPU-compared heap write-back passed: unchanged, sparse, boundary, repeated and concurrent CPU stores exact; only changed blocks downloaded");
    }
#ifdef _WIN32
    if (std::getenv("APS5_STREAM_HEAP_WRITEBACK_MIB") != nullptr && std::getenv("APS5_GPU_HEAP_COMPARE") == nullptr) {
        const auto snapshotFiles = [] {
            wchar_t directory[MAX_PATH + 1]{};
            Require(GetTempPathW(MAX_PATH + 1, directory) != 0, "cannot query snapshot test temp path");
            const auto pattern = std::wstring(directory) + L"anyps5-writeback-" + std::to_wstring(GetCurrentProcessId()) + L"-*.tmp";
            WIN32_FIND_DATAW entry{};
            const auto handle = FindFirstFileW(pattern.c_str(), &entry);
            unsigned count = 0;
            if (handle != INVALID_HANDLE_VALUE) {
                do { ++count; } while (FindNextFileW(handle, &entry));
                FindClose(handle);
            }
            return count;
        };
        // Earlier fixtures' completed snapshots are kept for reuse; this one starts without them.
        GuestBufferMemory::ReleaseRetainedSnapshots();
        const auto filesBefore = snapshotFiles();
        std::memset(guest, 0x31, bytes);
        for (std::size_t i = 0; i < 3 * 65536; ++i) guest[65536 + i] = std::byte{static_cast<unsigned char>((i * 17) % 251)};
        constexpr std::size_t mixedLater = (16u << 20u) + 4 * 65536;
        for (std::size_t i = 0; i < 2 * 65536; ++i) guest[mixedLater + i] = std::byte{static_cast<unsigned char>((i * 13) % 239)};
        auto memory = std::make_shared<GuestBufferMemory>(context);
        // Unaligned writable edges exercise padding. The GPU may change those
        // padding bytes, but write-back must only store the declared guest range.
        memory->AddWritable(address + 1, bytes - 2);
        memory->Upload(false);
        Require(memory->RequiresImmediateWriteBack(), "large heap write did not use bounded snapshots");
        Require(memory->OwnedBufferBytes() < (1u << 20u), "bounded snapshot retained full-size GPU or CPU copies");
        Require(snapshotFiles() > filesBefore, "mixed-data original did not exercise the snapshot spool");
        guest[128] = std::byte{0x75};
        guest[65536 + 5] = std::byte{0x74};
        std::uint32_t adjustment = 0;
        const auto view = memory->Descriptor(address + 1, bytes - 2, adjustment);
        Require(adjustment == 1, "bounded snapshot test lost unaligned view adjustment");
        Recorder recorder(context);
        recorder.Activate();
        const std::uint32_t first = 0x61616191;
        const std::uint32_t last = 0x92626262;
        const std::uint32_t middle = 0x66666666;
        const auto commands = recorder.Commands();
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(commands, view.buffer, view.offset, 4, &first);
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(commands, view.buffer, view.offset + bytes - 4, 4, &last);
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(commands, view.buffer, view.offset + 65536 + 128, 4, &middle);
        memory->RecordCopyBacks(recorder);
        bool completed = false;
        recorder.OnComplete([&] { memory->WriteBack(); completed = true; });
        recorder.Keep(memory, memory->OwnedBufferBytes());
        memory->FinishBoundedWriteBack(recorder);
        Require(completed && !memory->RequiresImmediateWriteBack() && !memory->HoldsLease(), "bounded snapshot did not seal its result before later work");
        // The completed snapshot is kept (one spool) for the next capture of the range.
        Require(snapshotFiles() == filesBefore + 1, "completed bounded snapshot was not retained for reuse");
        for (std::size_t i = 0; i < bytes; ++i) {
            auto expected = std::byte{0x31};
            if (i >= 65536 && i < 4 * 65536) expected = std::byte{static_cast<unsigned char>(((i - 65536) * 17) % 251)};
            if (i >= mixedLater && i < mixedLater + 2 * 65536) expected = std::byte{static_cast<unsigned char>(((i - mixedLater) * 13) % 239)};
            if (i >= 1 && i < 4) expected = std::byte{0x61};
            if (i >= bytes - 4 && i < bytes - 1) expected = std::byte{0x62};
            if (i >= 65536 + 128 && i < 65536 + 132) expected = std::byte{0x66};
            if (i == 128) expected = std::byte{0x75};
            if (i == 65536 + 5) expected = std::byte{0x74};
            Require(guest[i] == expected, "bounded snapshot changed GPU results, concurrent CPU bytes or padding");
        }
        // Subsequent GPU work overwrites the mirror, after the first result was
        // committed; it cannot retroactively alter the first guest write-back.
        const std::uint32_t later = 0x77777777;
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(recorder.Commands(), view.buffer, view.offset, 4, &later);
        recorder.Sync();
        Require(guest[1] == std::byte{0x61}, "later GPU work altered an already committed bounded result");
        {
            // A second write of the same range starts from the retained snapshot: no new spool, and
            // only the bytes this work changed are stored (not the earlier unstored mirror change
            // at the view's start, nor the CPU's own bytes).
            guest[65536 + 9] = std::byte{0x7a};
            GuestBufferMemory again(context);
            again.AddWritable(address + 1, bytes - 2);
            again.Upload(false);
            Require(again.RequiresImmediateWriteBack(), "second large heap write did not use bounded snapshots");
            Require(snapshotFiles() == filesBefore + 1, "second capture of a retained range created another spool");
            const auto againView = again.Descriptor(address + 1, bytes - 2, adjustment);
            const std::uint32_t value = 0x55443322;
            CommandBatch batch(context);
            context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(batch.Handle(), againView.buffer, againView.offset + 2 * 65536 + 64, 4, &value);
            batch.SubmitAndWait();
            again.WriteBack();
            Require(guest[2 * 65536 + 64] == std::byte{0x22} && guest[2 * 65536 + 67] == std::byte{0x55}, "reused snapshot lost the GPU result");
            Require(guest[65536 + 9] == std::byte{0x7a} && guest[1] == std::byte{0x61}, "reused snapshot stored bytes the work did not change");
            Require(snapshotFiles() == filesBefore + 1 && !again.HoldsLease(), "reused snapshot leaked a spool or its lease");
        }
        Require(GuestBufferMemory::ReleaseRetainedSnapshots() == 1 && snapshotFiles() == filesBefore, "released snapshot kept its temporary spool");
        // Fully mixed data exercises the maximum spool size, coalesced I/O,
        // and a final partial 64 KiB block, including view padding.
        constexpr std::size_t tailLength = (17u << 20u) + 13;
        for (std::size_t i = 0; i < tailLength + 4; ++i) guest[i] = std::byte{static_cast<unsigned char>((i * 19) % 241)};
        GuestBufferMemory partialTail(context);
        partialTail.AddWritable(address + 3, tailLength);
        partialTail.Upload(false);
        Require(partialTail.RequiresImmediateWriteBack(), "partial-tail write did not use bounded snapshots");
        guest[8] = std::byte{0x75};
        const auto tailView = partialTail.Descriptor(address + 3, tailLength, adjustment);
        Require(adjustment == 3, "partial-tail test lost view padding");
        const std::uint32_t tailFirst = 0x68313131;
        CommandBatch tailBatch(context);
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(tailBatch.Handle(), tailView.buffer, tailView.offset, 4, &tailFirst);
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(tailBatch.Handle(), tailView.buffer, tailView.offset + tailLength - 1, 4, &last);
        tailBatch.SubmitAndWait();
        partialTail.WriteBack();
        for (std::size_t i = 0; i < tailLength + 4; ++i) {
            auto expected = std::byte{static_cast<unsigned char>((i * 19) % 241)};
            if (i == 3) expected = std::byte{0x68};
            if (i == 8) expected = std::byte{0x75};
            if (i >= tailLength - 1 && i < tailLength + 2) expected = std::byte{0x62};
            if (i == tailLength + 2) expected = std::byte{0x92};
            Require(guest[i] == expected, "bounded partial-tail write-back changed boundary or original bytes");
        }
        Require(GuestBufferMemory::ReleaseRetainedSnapshots() == 1 && snapshotFiles() == filesBefore && !partialTail.HoldsLease(), "partial-tail write leaked its spool or lease");
        std::puts("bounded large write-back passed: exact uniform/mixed originals, padding, concurrent CPU writes, batch seal and spool cleanup");
    }
#endif
    DropAddressSpaceCache();
    std::memset(guest, 0x31, bytes);
    {
        GuestBufferMemory warm(context);
        warm.AcquireRegistered();
        warm.Upload(false);
        Require(warm.CachedAddressTable().has_value(), "BDA mirror regression did not create a cached address space");
    }
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
        const auto hits = AddressSpaceCounters().hits;
        GuestBufferMemory bdaWrite(context);
        bdaWrite.AcquireRegistered();
        Require(AddressSpaceCounters().hits > hits, "BDA mirror regression missed the cached address-space path");
        bdaWrite.AddAddressWritableMirrors();
        bdaWrite.Upload(false);
        const auto ranges = bdaWrite.AddressRanges();
        const auto found = std::find_if(ranges.begin(), ranges.end(), [&](const auto& range) { return range.begin <= address && range.end >= address + bytes; });
        Require(found != ranges.end() && (found->permissions & ShaderRecompiler::BdaAbi::Write) != 0,
            "cached device heap mirror missing BDA write permission");
        std::uint32_t adjustment = 0;
        const auto view = bdaWrite.Descriptor(address, bytes, adjustment);
        const std::uint32_t value = 0x51515151u + attempt;
        CommandBatch batch(context);
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(batch.Handle(), view.buffer, view.offset + 128, 4, &value);
        batch.SubmitAndWait();
        guest[256] = std::byte{0x73};
        bdaWrite.WriteBack();
        std::uint32_t result = 0;
        std::memcpy(&result, guest + 128, 4);
        Require(result == value && guest[256] == std::byte{0x73}, "cached BDA mirror write-back lost GPU or concurrent CPU bytes");
    }
    std::puts("cached BDA heap mirror permissions and repeated exact GPU write-back passed");
    GuestBufferMemory::ReleaseRetainedSnapshots();
    DropAddressSpaceCache();
    std::memset(guest, 0x31, bytes);
    guestCleanup.reset();
}
}
int main() {
#ifdef _WIN32
    _putenv_s("APS5_DEVICE_HEAP_MIRRORS", "1");
    _putenv_s("APS5_DEVICE_HEAP_DESCRIPTOR_MIRRORS", "1");
    _putenv_s("APS5_REUSE_DEVICE_HEAP_MIRRORS", "1");
    _putenv_s("APS5_HOST_IMPORT_MIB", "0");
    _putenv_s("APS5_ASSERT_GPU_LOCK", "1");
    _putenv_s("APS5_RECORDER_KEPT_MIB", "1");
#else
    setenv("APS5_DEVICE_HEAP_MIRRORS", "1", 1);
    setenv("APS5_DEVICE_HEAP_DESCRIPTOR_MIRRORS", "1", 1);
    setenv("APS5_REUSE_DEVICE_HEAP_MIRRORS", "1", 1);
    setenv("APS5_HOST_IMPORT_MIB", "0", 1);
    setenv("APS5_ASSERT_GPU_LOCK", "1", 1);
    setenv("APS5_RECORDER_KEPT_MIB", "1", 1);
#endif
    try {
#ifdef _WIN32
        FragmentedSnapshot();
        RecapturedSnapshot();
#endif
        Device device;
        Run(device.GetContext());
        std::puts("real GPU device heap upload, refresh, write-back and deferred ordering passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
