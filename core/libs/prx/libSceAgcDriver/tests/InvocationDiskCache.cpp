#include "prx/libSceAgcDriver/Execution/include/InvocationDiskCache.hpp"
#include <stdexcept>
#include <chrono>

namespace Cache = AgcDriver::InvocationDiskCache;
using namespace ShaderRecompiler;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / ("aps5-invocation-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        _putenv_s("ANYPS5_SHADER_CACHE_DIR", root.string().c_str());
        _putenv_s("APS5_DISK_INVOCATION_MIN_CODE_WORDS", "1");
        RecompileRequest request{};
        std::array<std::uint32_t, 2> code{1, 2}, data{3, 4};
        std::array<std::byte, 2> memory{std::byte{5}, std::byte{6}};
        const std::array<MemoryRegion, 1> regions{{{0x10000, memory}}};
        request.shader.stage = ShaderStage::Compute;
        request.shader.code = code;
        request.context.userData = data;
        request.context.memory = regions;
        require(Cache::Eligible(request), "eligible request rejected");
        const auto key = Cache::Key(request);
        RecompileResult result{};
        result.spirv = std::vector<std::uint32_t>{0x07230203, 1, 2, 3};
        result.variantId = 123;
        result.specializationId = 456;
        result.pushConstants = {std::byte{8}};
        require(Cache::Store(key, result), "store failed");
        const auto loaded = Cache::Load(key);
        require(bool(loaded), "load failed");
        require(loaded->variantId == 0 && loaded->specializationId == 0, "process-local IDs reused");
        std::vector<std::byte> before, after;
        ShaderDiskCache::EncodeResult(result, before);
        ShaderDiskCache::EncodeResult(*loaded, after);
        require(before == after, "payload changed");
        data[0]++; require(!Cache::Load(Cache::Key(request)), "userdata mismatch accepted"); data[0]--;
        memory[0] = std::byte{9}; require(!Cache::Load(Cache::Key(request)), "memory mismatch accepted"); memory[0] = std::byte{5};
        code[0]++; require(!Cache::Load(Cache::Key(request)), "code mismatch accepted"); code[0]--;
        request.target.fixedPushSlots = !request.target.fixedPushSlots;
        require(!Cache::Load(Cache::Key(request)), "extra target mismatch accepted");
        request.target.fixedPushSlots = !request.target.fixedPushSlots;
        result.poisonedSrtReads = 1;
        require(!Cache::Store(key, result), "poisoned result accepted");
        result.poisonedSrtReads = 0;
        result.bindings.emplace_back(); result.bindings.back().role = DescriptorRole::GuestImages;
        require(!Cache::IndependentOfImageFeedback(result), "image feedback result accepted");
        const auto path = Cache::Directory() / ShaderDiskCache::EntryName(key);
        std::vector<std::byte> bytes;
        require(Cache::ReadBounded(path, bytes), "read failed");
        auto corrupt = bytes; corrupt.back() ^= std::byte{1};
        require(WriteFileAtomically(path, corrupt) && !Cache::Load(key), "corrupt payload accepted");
        corrupt = bytes; corrupt[8] ^= std::byte{1};
        require(WriteFileAtomically(path, corrupt) && !Cache::Load(key), "wrong source version accepted");
        corrupt.resize(8);
        require(WriteFileAtomically(path, corrupt) && !Cache::Load(key), "truncation accepted");
        // This unique temporary directory contains only this test's generated fixture.
        std::filesystem::remove_all(root);
        std::puts("exact cache: round trip, complete input identity, IDs, feedback, corruption and truncation passed");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
