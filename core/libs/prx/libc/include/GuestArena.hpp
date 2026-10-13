#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTARENA_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTARENA_HPP

#include <cstddef>
#include <cstdint>

// Guest virtual memory is placed inside one reserved arena below the PS5 application map limit
// absolute address and breaks on host addresses outside that range.
namespace GuestArena {

#ifndef _WIN32
using SharedBackingResolver = bool (*)(std::uintptr_t address, std::size_t bytes, int* file, std::uint64_t* offset);
using SharedBackingWriter = bool (*)(std::uintptr_t address, const void* source, std::size_t bytes);
#endif

extern "C" {

bool GuestArenaAvailable_nid_postfix();
bool GuestArenaContains_nid_postfix(const void* pointer, std::size_t bytes);
void* GuestArenaAllocate_nid_postfix(std::size_t bytes, std::size_t alignment);
void* GuestArenaAllocateAtOrAbove_nid_postfix(std::uintptr_t hint, std::size_t bytes, std::size_t alignment);
void GuestArenaMarkUsed_nid_postfix(const void* pointer, std::size_t bytes);
void GuestArenaRelease_nid_postfix(const void* pointer, std::size_t bytes);
// The reserved range, and whether it was reserved with page write watching (Windows MEM_WRITE_WATCH).
void GuestArenaRange_nid_postfix(std::uintptr_t* base, std::size_t* bytes);
bool GuestArenaWriteWatched_nid_postfix();
void GuestArenaSetProtection_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uint32_t protection);
void GuestArenaCommit_nid_postfix(void* pointer, std::size_t bytes, std::uint32_t protection, std::size_t granule);
void GuestArenaReset_nid_postfix(void* pointer, std::size_t bytes);
#ifdef _WIN32
bool GuestArenaHandleWrite_nid_postfix(std::uintptr_t address);
void GuestArenaPinWritable_nid_postfix(const void* pointer, std::size_t bytes);
void GuestArenaUnpinWritable_nid_postfix(const void* pointer, std::size_t bytes);
bool GuestArenaProtection_nid_postfix(std::uintptr_t address, std::uint32_t* protection);
bool GuestArenaCollectWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear);
bool GuestArenaHostRegionOverlaps_nid_postfix(std::uintptr_t address, std::size_t bytes);
std::uint64_t GuestArenaCommitGeneration_nid_postfix();
void GuestArenaSetPrivateMappingObserver_nid_postfix(void (*callback)(std::uintptr_t address, std::size_t bytes, std::uint64_t generation));
void GuestArenaMap_nid_postfix(void* pointer, std::size_t bytes, void* section, std::uint64_t offset, std::uint32_t protection);
void* GuestArenaMapAlias_nid_postfix(std::uintptr_t address, std::size_t bytes);
void GuestArenaUnmapAlias_nid_postfix(void* alias);
// One process-wide lock for operations that change which pages are locked in the working set:
// guest mlock (libkernel VirtualLock) and the driver's query-then-VirtualUnlock source-page trim.
// Holding it across the trim's query and unlock means a page cannot be locked in between.
void GuestArenaWorkingSetLock_nid_postfix();
void GuestArenaWorkingSetUnlock_nid_postfix();
// Bounded accounting of file-backed direct memory: libkernel registers each sparse backing file
// handle it retains; the sum of their allocated (on-disk) sizes is readable from any library.
void GuestArenaRegisterBackingFile_nid_postfix(void* file);
void GuestArenaUnregisterBackingFile_nid_postfix(void* file);
void GuestArenaBackingFileBytes_nid_postfix(std::uint64_t* logical, std::uint64_t* allocated, std::uint32_t* files);
#else
void GuestArenaSetSharedBacking_nid_postfix(SharedBackingResolver resolver);
void GuestArenaSetSharedBackingWriter_nid_no_patch(SharedBackingWriter writer);
bool GuestArenaWriteSharedBacking_nid_no_patch(std::uintptr_t address, const void* source, std::size_t bytes);
bool GuestArenaSharedBacking_nid_postfix(std::uintptr_t address, std::size_t bytes, int* file, std::uint64_t* offset);
#endif
bool GuestArenaBeginHostWrite_nid_postfix(void* pointer, std::size_t bytes);
void GuestArenaEndHostWrite_nid_postfix(void* pointer, std::size_t bytes);

}

#ifdef _WIN32
class WorkingSetOwnership {
public:
    WorkingSetOwnership() { GuestArenaWorkingSetLock_nid_postfix(); }
    ~WorkingSetOwnership() { GuestArenaWorkingSetUnlock_nid_postfix(); }
    WorkingSetOwnership(const WorkingSetOwnership&) = delete;
    WorkingSetOwnership& operator=(const WorkingSetOwnership&) = delete;
};
#endif

class HostWrite {
public:
    HostWrite(void* pointer, std::size_t bytes) : pointer(pointer), bytes(bytes), open(GuestArenaBeginHostWrite_nid_postfix(pointer, bytes)) {}
    ~HostWrite() {
        if (open) GuestArenaEndHostWrite_nid_postfix(pointer, bytes);
    }
    HostWrite(const HostWrite&) = delete;
    HostWrite& operator=(const HostWrite&) = delete;
    bool Open() const { return open; }

private:
    void* pointer;
    std::size_t bytes;
    bool open;
};

}

#endif
