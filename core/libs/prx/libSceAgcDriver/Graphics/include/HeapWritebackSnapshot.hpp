#pragma once

#include "Resources.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver::Graphics {

// An exact pre-work GPU snapshot with bounded RAM: uniform 64 KiB blocks
// retain one byte; other blocks are spooled to a delete-on-close temporary file.
// It never reads guest memory, whose CPU writes may race the GPU's work.
struct HeapWritebackSnapshot {
    static constexpr std::size_t BlockBytes = 65536;
    static constexpr std::size_t MaxChunkBytes = 16u << 20u;
    struct Block {
        std::uint64_t offset = 0;
        std::uint32_t bytes = 0;
        std::byte value{};
        bool stored = false;
    };
    std::vector<Block> blocks;
    std::uint64_t logicalBytes = 0;
    std::uint64_t storedBytes = 0;
    std::uint64_t writeCalls = 0;
    std::uint64_t readCalls = 0;
    // Mixed bytes written to the spool by the latest capture (all of them for a fresh one).
    std::uint64_t rewrittenBytes = 0;
#ifdef _WIN32
    struct AlignedBytes {
        std::byte* data = nullptr;
        std::size_t size = 0;
        explicit AlignedBytes(std::size_t bytes) : size((bytes + BlockBytes - 1) / BlockBytes * BlockBytes) {
            data = static_cast<std::byte*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            Require(data != nullptr, "cannot allocate aligned snapshot transfer scratch");
        }
        ~AlignedBytes() { if (data) VirtualFree(data, 0, MEM_RELEASE); }
        AlignedBytes(const AlignedBytes&) = delete;
        AlignedBytes& operator=(const AlignedBytes&) = delete;
    };
    HANDLE file = INVALID_HANDLE_VALUE;
    ~HeapWritebackSnapshot() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    HeapWritebackSnapshot() = default;
    HeapWritebackSnapshot(const HeapWritebackSnapshot&) = delete;
    HeapWritebackSnapshot& operator=(const HeapWritebackSnapshot&) = delete;

    void open() {
        if (file != INVALID_HANDLE_VALUE) return;
        wchar_t directory[MAX_PATH + 1]{};
        const auto length = GetTempPathW(MAX_PATH + 1, directory);
        Require(length != 0 && length <= MAX_PATH, "cannot locate heap snapshot temporary directory");
        static std::atomic<std::uint64_t> serial{0};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            const auto path = std::wstring(directory) + L"anyps5-writeback-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(++serial) + L".tmp";
            file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_NO_BUFFERING, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                // 64 KiB-aligned VirtualAlloc buffers and transfers cover logical
                // and physical sectors. Reject unsupported geometry explicitly.
                // Same Win8+ ABI as FILE_STORAGE_INFO; the project's older
                // SDK target hides its declaration even on supported Windows.
                struct StorageSectorInfo {
                    DWORD LogicalBytesPerSector, PhysicalBytesPerSectorForAtomicity,
                        PhysicalBytesPerSectorForPerformance, FileSystemEffectivePhysicalBytesPerSectorForAtomicity,
                        Flags, ByteOffsetForSectorAlignment, ByteOffsetForPartitionAlignment;
                } storage{};
                Require(GetFileInformationByHandleEx(file, static_cast<FILE_INFO_BY_HANDLE_CLASS>(16), &storage, sizeof(storage)) != 0,
                    "cannot query snapshot spool sector geometry");
                const auto aligned = [](DWORD sector) { return sector != 0 && BlockBytes % sector == 0; };
                Require(aligned(storage.LogicalBytesPerSector) && aligned(storage.PhysicalBytesPerSectorForPerformance) &&
                    aligned(storage.PhysicalBytesPerSectorForAtomicity), "snapshot spool sector exceeds transfer alignment");
                return;
            }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
        }
        throw std::runtime_error("cannot create bounded heap snapshot spool: Windows error " + std::to_string(GetLastError()));
    }

    void seek(std::uint64_t offset) {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        Require(SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != 0, "heap snapshot spool seek failed");
    }

    void Append(std::span<const std::byte> data) {
        Require(!data.empty(), "heap snapshot cannot append an empty chunk");
        Require(data.size() <= MaxChunkBytes, "heap snapshot append exceeds its bounded transfer chunk");
        Require(logicalBytes % BlockBytes == 0, "heap snapshot append is not block aligned");
        // Mixed blocks may be separated by thousands of uniform blocks. Pack
        // all mixed bytes in this bounded chunk into ONE unbuffered call.
        // WRITE_THROUGH alone still populates the Windows file cache, defeating
        // the bounded-RAM goal. These disposable snapshots need no crash durability.
        const auto packedOffset = storedBytes;
        std::unique_ptr<AlignedBytes> packed;
        std::size_t packedBytes = 0;
        for (std::size_t at = 0; at < data.size(); at += BlockBytes) {
            const auto part = data.subspan(at, std::min(BlockBytes, data.size() - at));
            Block block{storedBytes, static_cast<std::uint32_t>(part.size()), part.front(), false};
            block.stored = !std::all_of(part.begin(), part.end(), [&](std::byte value) { return value == block.value; });
            if (block.stored) {
                if (!packed) packed = std::make_unique<AlignedBytes>(data.size());
                std::memcpy(packed->data + packedBytes, part.data(), part.size());
                packedBytes += part.size();
                storedBytes += block.bytes;
            }
            blocks.push_back(block);
            logicalBytes += block.bytes;
        }
        if (packed) {
            open();
            seek(packedOffset);
            DWORD written = 0;
            ++writeCalls;
            const auto transfer = (packedBytes + BlockBytes - 1) / BlockBytes * BlockBytes;
            // A final short block is zero padded on disk only. Its logical byte
            // count is unchanged; no following append may begin inside a block.
            Require(WriteFile(file, packed->data, static_cast<DWORD>(transfer), &written, nullptr) != 0 && written == transfer,
                "heap snapshot spool write failed");
        }
    }

    // Captures one chunk again over this complete snapshot of the same range, so that afterwards
    // it holds exactly `data`: every byte is compared with the retained copy (no hashing); a
    // uniform block that stays uniform takes its new value, a changed mixed block is rewritten in
    // place in the spool. Returns false, leaving the snapshot unusable, when a uniform block
    // turns mixed (the packed layout has no room for it); the caller then takes a fresh one.
    bool Recapture(std::size_t offset, std::span<const std::byte> data, std::vector<std::byte>& previous) {
        Require(!data.empty() && data.size() <= MaxChunkBytes, "heap snapshot recapture exceeds its bounded transfer chunk");
        Require(offset % BlockBytes == 0 && offset <= logicalBytes && data.size() <= logicalBytes - offset, "heap snapshot recapture exceeds its exact original bytes");
        previous.resize(data.size());
        Read(offset, previous);
        std::vector<std::size_t> changed;
        auto index = offset / BlockBytes;
        for (std::size_t at = 0; at < data.size(); at += BlockBytes, ++index) {
            auto& block = blocks.at(index);
            Require(block.bytes == std::min(BlockBytes, data.size() - at), "heap snapshot recapture is not block aligned");
            const auto part = data.subspan(at, block.bytes);
            if (std::memcmp(part.data(), previous.data() + at, part.size()) == 0) continue;
            if (block.stored) {
                changed.push_back(at);
                continue;
            }
            if (!std::all_of(part.begin(), part.end(), [&](std::byte value) { return value == part.front(); })) return false;
            block.value = part.front();
        }
        // Rewrite runs of changed mixed blocks that are adjacent in the spool with one call each.
        index = offset / BlockBytes;
        for (std::size_t first = 0; first < changed.size();) {
            auto last = first + 1;
            while (last < changed.size() && blocks[index + changed[last] / BlockBytes].offset ==
                blocks[index + changed[last - 1] / BlockBytes].offset + blocks[index + changed[last - 1] / BlockBytes].bytes) ++last;
            std::size_t runBytes = 0;
            for (auto i = first; i < last; ++i) runBytes += blocks[index + changed[i] / BlockBytes].bytes;
            AlignedBytes packed(runBytes);
            std::size_t packedBytes = 0;
            for (auto i = first; i < last; ++i) {
                const auto& block = blocks[index + changed[i] / BlockBytes];
                std::memcpy(packed.data + packedBytes, data.data() + changed[i], block.bytes);
                packedBytes += block.bytes;
            }
            seek(blocks[index + changed[first] / BlockBytes].offset);
            DWORD written = 0;
            ++writeCalls;
            // As in Append: only the snapshot's final block may be short, padded on disk only.
            Require(WriteFile(file, packed.data, static_cast<DWORD>(packed.size), &written, nullptr) != 0 && written == packed.size,
                "heap snapshot spool rewrite failed");
            rewrittenBytes += packedBytes;
            first = last;
        }
        return true;
    }

    void Read(std::size_t offset, std::span<std::byte> output) {
        Require(output.size() <= MaxChunkBytes, "heap snapshot read exceeds its bounded transfer chunk");
        Require(offset % BlockBytes == 0 && offset <= logicalBytes && output.size() <= logicalBytes - offset,
            "heap snapshot read exceeds its exact original bytes");
        const auto first = offset / BlockBytes;
        auto last = first;
        std::size_t logical = 0, packedBytes = 0;
        std::uint64_t packedOffset = 0;
        while (logical < output.size()) {
            const auto& block = blocks.at(last++);
            Require(block.bytes <= output.size() - logical, "heap snapshot read ends inside a block");
            logical += block.bytes;
            if (block.stored) {
                if (packedBytes == 0) packedOffset = block.offset;
                Require(block.offset == packedOffset + packedBytes, "heap snapshot mixed blocks are not packed");
                packedBytes += block.bytes;
            }
        }
        if (packedBytes != 0) {
            seek(packedOffset);
            AlignedBytes scratch(packedBytes);
            DWORD read = 0;
            ++readCalls;
            Require(ReadFile(file, scratch.data, static_cast<DWORD>(scratch.size), &read, nullptr) != 0 && read == scratch.size,
                "heap snapshot spool read failed");
            std::memcpy(output.data(), scratch.data, packedBytes);
        }
        // Expand backwards in-place. Each destination is at or above its
        // packed source, so uniform fills cannot overwrite unread mixed bytes.
        while (last > first) {
            const auto& block = blocks[--last];
            logical -= block.bytes;
            if (block.stored) {
                packedBytes -= block.bytes;
                std::memmove(output.data() + logical, output.data() + packedBytes, block.bytes);
            } else {
                std::fill_n(output.data() + logical, block.bytes, block.value);
            }
        }
    }
#endif
    std::size_t MemoryBytes() const { return sizeof(*this) + blocks.capacity() * sizeof(Block); }
};

}
