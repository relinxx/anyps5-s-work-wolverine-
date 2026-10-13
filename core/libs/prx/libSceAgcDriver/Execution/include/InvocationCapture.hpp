#pragma once

#include "ControlFlow/RequestSerializer.hpp"
#include "ShaderCacheDirectory.hpp"
#include <algorithm>
#include <array>
#include <cstdio>

// Diagnostic-only capture of a live invocation, before specialization starts.
// The auxiliary target data restores fields missing from the request wire format.
namespace AgcDriver::InvocationCapture {
inline constexpr std::size_t MaxEntry = 64ull << 20;

inline std::array<std::uint32_t, 4> ExtraTarget(const ShaderRecompiler::RecompileRequest& request) {
    // Include the full target extension set explicitly; fixedPushSlots is not in the serializer.
    return {request.target.nonConstantImageOffsets, request.target.srgbDecodeFormats,
            request.target.narrowSubgroupClock, request.target.fixedPushSlots};
}

inline std::vector<std::byte> Key(const ShaderRecompiler::RecompileRequest& request) {
    // A diagnostic round-trip identity, no production cache entries are loaded here.
    // Serialize plus the omitted target fields carries all live invocation inputs.
    const auto text = ShaderRecompiler::RequestSerializer{}.Serialize(request);
    const auto extra = ExtraTarget(request);
    std::vector<std::byte> key;
    const auto append = [&](const void* data, std::size_t size) {
        const auto* begin = static_cast<const std::byte*>(data);
        key.insert(key.end(), begin, begin + size);
    };
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

inline void Capture(const ShaderRecompiler::RecompileRequest& request) {
    const auto* directory = std::getenv("APS5_CAPTURE_INVOCATION_DIR");
    const auto* address = std::getenv("APS5_CAPTURE_INVOCATION");
    if (!directory || !address || request.shader.codeAddress != std::strtoull(address, nullptr, 16)) return;
    const auto path = std::filesystem::path(directory) / "invocation.req";
    const auto text = ShaderRecompiler::RequestSerializer{}.Serialize(request);
    const auto extra = ExtraTarget(request);
    const auto key = Key(request);
    if (!ShaderRecompiler::WriteFileAtomically(path, std::as_bytes(std::span(text))) ||
        !ShaderRecompiler::WriteFileAtomically(path.string() + ".target", std::as_bytes(std::span(extra))) ||
        !ShaderRecompiler::WriteFileAtomically(path.string() + ".key", key))
        throw std::runtime_error("could not save complete shader invocation");
    std::fprintf(stderr, "[gpu] complete shader invocation saved: %s (%zu regions)\n", path.string().c_str(), request.context.memory.size());
    std::fflush(stderr);
}

inline void RestoreAndVerify(const char* path, ShaderRecompiler::RecompileRequest& request) {
    std::vector<std::byte> bytes, savedKey;
    if (!ReadBounded(std::string(path) + ".target", bytes) || bytes.size() != 16 ||
        !ReadBounded(std::string(path) + ".key", savedKey)) throw std::runtime_error("missing invocation identity");
    std::array<std::uint32_t, 4> extra;
    std::memcpy(extra.data(), bytes.data(), sizeof(extra));
    request.target.nonConstantImageOffsets = extra[0] != 0;
    request.target.srgbDecodeFormats = extra[1];
    request.target.narrowSubgroupClock = extra[2] != 0;
    request.target.fixedPushSlots = extra[3] != 0;
    if (Key(request) != savedKey) throw std::runtime_error("invocation identity differs after serialization round trip");
}
}
