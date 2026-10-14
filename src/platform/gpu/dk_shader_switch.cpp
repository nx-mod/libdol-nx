// The GPU on deko3d: shaders - GLSL compiled on the console by UAM, loaded
// into code memory, kept for the run by the source's hash (dk.h).
//
// And kept between runs: every shader compiled is appended to one pack file in
// the game's cache folder, read whole the first time a shader is asked for, so
// a shader a game has drawn with before loads at once instead of being compiled
// again (UAM takes tens of milliseconds a shader - a stall the first time each
// effect appears, every run). Entries are keyed by the exact source's hash: a
// renderer that writes a shader differently makes a new entry, never a stale
// binary.
#include "dk.h"
#include "switch_layout.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

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
    // the pack's binaries not loaded yet, by the same key
    std::unordered_map<uint64_t, std::vector<char>> packed;
    bool pack_read = false;
} g_shaders;

// the pack: a tag, then (key, size, DKSH) entries one after another
constexpr char kPackTag[8] = {'W', 'X', 'S', 'H', 'P', 'K', '0', '1'};
constexpr const char* kPackPath = WIINX_GAME_PATH("cache/shaders.pack");

void read_pack() {
    g_shaders.pack_read = true;
    FILE* file = std::fopen(kPackPath, "rb");
    if (!file) {
        return;
    }
    std::vector<char> bytes;
    char chunk[64 * 1024];
    for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) {
        bytes.insert(bytes.end(), chunk, chunk + n);
    }
    std::fclose(file);
    if (bytes.size() < sizeof(kPackTag) || std::memcmp(bytes.data(), kPackTag, sizeof(kPackTag)) != 0) {
        log("shader pack: not ours, ignored");
        return;
    }
    // (a short or damaged entry ends the read: what came before it still counts)
    size_t at = sizeof(kPackTag);
    while (at + 12 <= bytes.size()) {
        uint64_t key;
        uint32_t size;
        std::memcpy(&key, bytes.data() + at, 8);
        std::memcpy(&size, bytes.data() + at + 8, 4);
        at += 12;
        if (size == 0 || at + size > bytes.size()) {
            break;
        }
        g_shaders.packed[key].assign(bytes.data() + at, bytes.data() + at + size);
        at += size;
    }
    log("shader pack: %zu shaders kept from earlier runs", g_shaders.packed.size());
}

void append_pack(uint64_t key, const char* dksh, size_t size) {
    struct stat info;
    const bool fresh = stat(kPackPath, &info) != 0 || info.st_size == 0;
    if (fresh) {
        mkdir(WIINX_GAME_PATH("cache"), 0777);  // (already there in a normal install)
    }
    FILE* file = std::fopen(kPackPath, "ab");
    if (!file) {
        return;
    }
    if (fresh) {
        std::fwrite(kPackTag, 1, sizeof(kPackTag), file);
    }
    const uint32_t length = static_cast<uint32_t>(size);
    std::fwrite(&key, 1, 8, file);
    std::fwrite(&length, 1, 4, file);
    std::fwrite(dksh, 1, size, file);
    std::fclose(file);
}

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

void shutdown_shaders() {
  std::lock_guard<std::mutex> guard(g_shaders.lock);
  for (auto& [key, loaded] : g_shaders.loaded) {
    delete loaded;
  }
  g_shaders.loaded.clear();
  if (g_shaders.code) {
    dkMemBlockDestroy(g_shaders.code);
    g_shaders.code = nullptr;
  }
  g_shaders.code_used = 0;
}

const DkShader* shader(ShaderStage stage, const char* glsl) {
    const uint64_t key = source_key(stage, glsl);
    std::lock_guard<std::mutex> guard(g_shaders.lock);

    if (const auto it = g_shaders.loaded.find(key); it != g_shaders.loaded.end()) {
        return it->second;
    }
    if (!g_shaders.pack_read) {
        read_pack();
    }
    if (const auto it = g_shaders.packed.find(key); it != g_shaders.packed.end()) {
        DkShader* kept = load(it->second.data(), it->second.size());
        g_shaders.packed.erase(it);
        if (kept) {
            g_shaders.loaded.emplace(key, kept);
            return kept;
        }
        // (a binary that would not load is compiled again below)
    }
    char* dksh = nullptr;
    size_t size = 0;
    DkShader* loaded = nullptr;
    if (dol_dk_compile_glsl(stage == ShaderStage::Fragment, glsl, &dksh, &size)) {
        loaded = load(dksh, size);
        if (loaded) {
            append_pack(key, dksh, size);
        }
    } else if (dksh == nullptr && size == 0 && !g_shaders.said_no_compiler) {
        log("a shader did not compile (or this build has no UAM)");
        g_shaders.said_no_compiler = true;
    }
    std::free(dksh);
    g_shaders.loaded.emplace(key, loaded);
    return loaded;
}

}  // namespace dol::dk
