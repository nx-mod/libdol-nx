// The GPU on deko3d: shaders - GLSL compiled on the console by UAM, loaded
// into code memory, kept for the run by the source's hash (dk.h).
//
// Compiled shaders are not kept on the card yet: that comes once a title draws
// on deko3d (docs/deko3d.md).
#include "dk.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

// UAM, from src/uam/uam_compile.cpp when the build has it; this stand-in when it
// does not (no UAM build found: cmake/game/CMakeLists.txt says so)
extern "C" __attribute__((weak)) int dol_dk_compile_glsl(int, const char*, char** dksh, size_t* size) {
    *dksh = nullptr;
    *size = 0;
    return 0;
}

namespace dol::dk {
namespace {

constexpr uint32_t kShaderCodeSize = 16u * 1024 * 1024;
constexpr uint32_t kDkshMagic = 0x48534b44;  // "DKSH"

// a DKSH file's header
struct DkshHeader {
    uint32_t magic, header_size, control_size, code_size, programs_offset, program_count;
};

struct Shaders {
    std::mutex lock;
    DkMemBlock code = nullptr;
    uint32_t code_used = 0;
    // by (stage, source hash); a failed compile is kept as null, not retried
    std::unordered_map<uint64_t, DkShader*> loaded;
    bool said_no_compiler = false;
} g_shaders;

// FNV-1a over the source, with the stage folded in
uint64_t source_key(ShaderStage stage, const char* glsl) {
    uint64_t hash = 0xcbf29ce484222325ull ^ static_cast<uint64_t>(stage);
    for (const char* c = glsl; *c; ++c) {
        hash = (hash ^ static_cast<uint8_t>(*c)) * 0x100000001b3ull;
    }
    return hash;
}

// a DKSH into code memory; null if it is not one or there is no room
DkShader* load(const char* dksh, size_t size) {
    DkshHeader header;

    if (size < sizeof(header)) {
        return nullptr;
    }
    std::memcpy(&header, dksh, sizeof(header));
    if (header.magic != kDkshMagic || header.control_size + header.code_size > size) {
        log("not a DKSH (%zu bytes)", size);
        return nullptr;
    }
    if (!g_shaders.code) {
        DkMemBlockMaker maker;
        dkMemBlockMakerDefaults(&maker, device(), kShaderCodeSize);
        maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
        g_shaders.code = dkMemBlockCreate(&maker);
    }
    const uint32_t offset = align_up(g_shaders.code_used, DK_SHADER_CODE_ALIGNMENT);
    if (offset + header.code_size > kShaderCodeSize - DK_SHADER_CODE_UNUSABLE_SIZE) {
        log("shader code memory full (%u bytes)", kShaderCodeSize);
        return nullptr;
    }
    std::memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(g_shaders.code)) + offset,
                dksh + header.control_size, header.code_size);
    auto* shader = new DkShader{};
    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, g_shaders.code, offset);
    maker.control = dksh;
    dkShaderInitialize(shader, &maker);
    if (!dkShaderIsValid(shader)) {
        delete shader;
        log("a compiled shader would not load");
        return nullptr;
    }
    g_shaders.code_used = offset + header.code_size;
    return shader;
}

}  // namespace

const DkShader* shader(ShaderStage stage, const char* glsl) {
    const uint64_t key = source_key(stage, glsl);
    std::lock_guard<std::mutex> guard(g_shaders.lock);

    if (const auto it = g_shaders.loaded.find(key); it != g_shaders.loaded.end()) {
        return it->second;
    }
    char* dksh = nullptr;
    size_t size = 0;
    DkShader* loaded = nullptr;
    if (dol_dk_compile_glsl(stage == ShaderStage::Fragment, glsl, &dksh, &size)) {
        loaded = load(dksh, size);
    } else if (dksh == nullptr && size == 0 && !g_shaders.said_no_compiler) {
        log("a shader did not compile (or this build has no UAM)");
        g_shaders.said_no_compiler = true;
    }
    std::free(dksh);
    g_shaders.loaded.emplace(key, loaded);
    return loaded;
}

}  // namespace dol::dk
