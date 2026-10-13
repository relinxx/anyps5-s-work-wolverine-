#pragma once

#include "ControlFlow/RequestSerializer.hpp"
#include "ShaderDiskCache.hpp"
#include "ShaderCacheDirectory.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>

// Driver-side, opt-in cache of a COMPLETE invocation, not just its shader code.
// Guest addresses/data, compiler switches, target features and source version
// must all match. In particular a result's bindings cannot be reused by code hash.
namespace AgcDriver::InvocationDiskCache {
using ShaderRecompiler::RecompileResult;
// These are serialization/identity APIs only; this header never changes compiler
// switches or compiler output. It has a separate driver-format namespace and
// compares the full input bytes, including the existing compiler switch key.
namespace ShaderDiskCache = ShaderRecompiler::ShaderDiskCache;
inline constexpr std::size_t MaxEntry = 64ull << 20;
inline constexpr std::size_t MaxDirectory = 256ull << 20;

inline bool Eligible(const ShaderRecompiler::RecompileRequest& request) {
    const auto* value = std::getenv("APS5_DISK_INVOCATION_MIN_CODE_WORDS");
    if (!value || std::strtoull(value, nullptr, 10) == 0 || request.graphics) return false;
    if (request.shader.code.size() < std::strtoull(value, nullptr, 10)) return false;
    std::size_t bytes = request.shader.code.size_bytes() + request.shader.header.size();
    for (const auto& region : request.context.memory) {
        if (region.bytes.size() > MaxEntry / 8 || bytes > MaxEntry / 8 - region.bytes.size()) return false;
        bytes += region.bytes.size();
    }
    return bytes <= MaxEntry / 8;
}

inline std::array<std::uint32_t, 4> ExtraTarget(const ShaderRecompiler::RecompileRequest& request) {
    // Include the full target extension set explicitly; fixedPushSlots is not in the serializer.
    return {request.target.nonConstantImageOffsets, request.target.srgbDecodeFormats,
            request.target.narrowSubgroupClock, request.target.fixedPushSlots};
}

inline std::vector<std::byte> Key(const ShaderRecompiler::RecompileRequest& request) {
    std::vector<std::byte> base;
    ShaderDiskCache::BuildKey(request, request.target.subgroupSize, base);
    const auto text = ShaderRecompiler::RequestSerializer{}.Serialize(request);
    const auto extra = ExtraTarget(request);
    const std::uint64_t baseSize = base.size();
    std::vector<std::byte> key;
    const auto append = [&](const void* data, std::size_t size) {
        const auto* begin = static_cast<const std::byte*>(data);
        key.insert(key.end(), begin, begin + size);
    };
    append(&baseSize, sizeof(baseSize));
    append(base.data(), base.size());
    append(extra.data(), sizeof(extra));
    append(text.data(), text.size());
    return key;
}

inline bool ReadBounded(const std::filesystem::path& path, std::vector<std::byte>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto size = file.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > MaxEntry) return false;
    bytes.resize(static_cast<std::size_t>(size));
    file.seekg(0);
    return bool(file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
}

inline std::filesystem::path Directory() { return ShaderRecompiler::ShaderCacheDirectory() / "driver-invocations-v1"; }

inline bool IndependentOfImageFeedback(const RecompileResult& result);

inline std::shared_ptr<const RecompileResult> Load(std::span<const std::byte> key) noexcept {
    try {
        std::vector<std::byte> bytes;
        if (!ReadBounded(Directory() / ShaderDiskCache::EntryName(key), bytes)) return {};
        std::array<std::uint64_t, 7> header{};
        if (bytes.size() < sizeof(header)) return {};
        std::memcpy(header.data(), bytes.data(), sizeof(header));
        if (header[0] != 0x31564e49535041ull || header[1] != ShaderDiskCache::SourceVersion() ||
            header[2] != key.size() || header[2] > bytes.size() - sizeof(header) ||
            header[3] != bytes.size() - sizeof(header) - header[2] || header[6] != 1) return {};
        const auto storedKey = std::span(bytes).subspan(sizeof(header), key.size());
        const auto payload = std::span(bytes).subspan(sizeof(header) + key.size());
        if (!std::equal(key.begin(), key.end(), storedKey.begin()) ||
            header[4] != ShaderRecompiler::HashBytes(storedKey) || header[5] != ShaderRecompiler::HashBytes(payload)) return {};
        auto result = std::make_shared<RecompileResult>();
        if (!ShaderDiskCache::DecodeResult(payload, *result) || result->spirv.empty() || !IndependentOfImageFeedback(*result)) return {};
        // Runtime IDs are process-local. Zero deliberately requests a fresh
        // Vulkan pipeline (which can hit its device cache), never a colliding ID.
        result->variantId = 0;
        result->specializationId = 0;
        result->cacheHit = true;
        return result;
    } catch (...) { return {}; }
}

inline bool IndependentOfImageFeedback(const RecompileResult& result) {
    if (result.poisonedSrtReads != 0) return false;
    return std::none_of(result.bindings.begin(), result.bindings.end(), [](const auto& binding) {
        return binding.role == ShaderRecompiler::DescriptorRole::GuestImages ||
               binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers ||
               binding.role == ShaderRecompiler::DescriptorRole::WideFeedback;
    });
}

inline bool Store(std::span<const std::byte> key, const RecompileResult& result) noexcept {
    if (!IndependentOfImageFeedback(result)) return false;
    try {
        static std::mutex mutex;
        const std::lock_guard lock(mutex);
        std::vector<std::byte> payload;
        ShaderDiskCache::EncodeResult(result, payload);
        const std::array<std::uint64_t, 7> header{0x31564e49535041ull, ShaderDiskCache::SourceVersion(),
            key.size(), payload.size(), ShaderRecompiler::HashBytes(key), ShaderRecompiler::HashBytes(payload), 1};
        const auto size = sizeof(header) + key.size() + payload.size();
        if (size > MaxEntry) return false;
        const auto directory = Directory();
        std::filesystem::create_directories(directory);
        const auto path = directory / ShaderDiskCache::EntryName(key);
        if (std::filesystem::exists(path)) return false;
        std::uint64_t total = 0, entries = 0;
        for (const auto& file : std::filesystem::directory_iterator(directory)) {
            if (file.is_regular_file()) { total += file.file_size(); ++entries; }
        }
        if (entries >= 32 || total + size > MaxDirectory) return false;
        std::vector<std::byte> bytes(sizeof(header));
        std::memcpy(bytes.data(), header.data(), sizeof(header));
        bytes.insert(bytes.end(), key.begin(), key.end());
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return ShaderRecompiler::WriteFileAtomically(path, bytes);
    } catch (...) { return false; }
}

}
