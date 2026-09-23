// THPVideoDecode, natively.
//
// The decoder itself is in format/media, working on bytes. This is what the
// game calls: it resolves the guest's buffers, runs the decode into them, and
// tells the graphics layer the textures changed.
//
// Run as translated PowerPC, the SDK's decoder spends every frame emulating
// paired singles through a Huffman decoder and an IDCT - which is why a menu
// page of animated buttons, each one a THP movie, is the slowest kind of screen
// a game has.
#include "wiinx/accel/sdk.hpp"

#include "wiinx/core/guest.hpp"
#include "wiinx/core/host.hpp"
#include "wiinx/format/media/thp.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>

namespace wiinx::sdk::thp {
namespace {

using media::kThpBadSyntax;
using media::kThpNoInput;
using media::kThpNoOutput;
using media::kThpNoWork;
using media::kThpOk;

// The frame's length is not passed in, so find how much guest memory is
// readable from its start, up to a bound no THP frame approaches.
size_t ReadableFrom(uint32_t address) {
    size_t good = 0;
    size_t probe = 64u * 1024u;
    constexpr size_t kLimit = 4u * 1024u * 1024u;
    while (probe <= kLimit && host().valid(address, static_cast<u32>(probe))) {
        good = probe;
        probe *= 2;
    }
    return good;
}

std::atomic<uint32_t> g_frames{0};
std::atomic<uint64_t> g_micros{0};

int32_t Decode(uint32_t file, uint32_t tileY, uint32_t tileU, uint32_t tileV, uint32_t work) {
    if (file == 0) return kThpNoInput;
    if (tileY == 0 || tileU == 0 || tileV == 0) return kThpNoOutput;
    if (work == 0) return kThpNoWork;

    const Host& h = host();
    const auto start = std::chrono::steady_clock::now();

    const size_t readable = ReadableFrom(file);
    if (readable == 0) return kThpBadSyntax;

    u8* const fileBytes = at(file, static_cast<u32>(readable));
    if (fileBytes == nullptr) {
        return kThpNoOutput;
    }
    media::ThpDecoder decoder{fileBytes, readable};
    const int32_t header = decoder.ParseHeaders();
    if (header != kThpOk) return header;

    const u32 lumaBytes = static_cast<u32>(decoder.Width()) * decoder.Height();
    if (lumaBytes == 0 || !h.valid(tileY, lumaBytes) || !h.valid(tileU, lumaBytes / 4) ||
        !h.valid(tileV, lumaBytes / 4)) {
        return kThpNoOutput;
    }

    u8* const luma = at(tileY, lumaBytes);
    u8* const chromaU = at(tileU, lumaBytes / 4);
    u8* const chromaV = at(tileV, lumaBytes / 4);
    if (luma == nullptr || chromaU == nullptr || chromaV == nullptr) {
        return kThpNoOutput;
    }
    const int32_t result = decoder.Decode(luma, chromaU, chromaV);

    // The planes are GX textures, and the renderer only re-reads a texture when
    // told its memory changed. The SDK decoder writes each strip with
    // LCStoreData, which the platform reports as a DMA; writing the planes
    // directly skips it, and the renderer goes on drawing a cached frame for
    // one plane and a fresh one for another - two videos flickering over each
    // other.
    if (result == kThpOk && h.notify_write != nullptr) {
        h.notify_write(tileY, lumaBytes);
        h.notify_write(tileU, lumaBytes / 4);
        h.notify_write(tileV, lumaBytes / 4);
    }

    g_micros.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - start).count()),
                       std::memory_order_relaxed);
    const uint32_t frames = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (h.log != nullptr && (frames == 1 || (frames % 300) == 0)) {
        char line[128];
        std::snprintf(line, sizeof(line), "[thp] native decode #%u %ux%u avg=%lluus", frames,
                      decoder.Width(), decoder.Height(),
                      static_cast<unsigned long long>(g_micros.load(std::memory_order_relaxed) / frames));
        h.log(line);
    }
    return result;
}

// THPVideoDecode(file, tileY, tileU, tileV, work) -> error code.
void video_decode(Cpu* cpu) {
    const Host& h = host();
    const int32_t result = Decode(h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5), h.gpr(cpu, 6), h.gpr(cpu, 7));
    h.set_gpr(cpu, 3, static_cast<u32>(result));
}

}  // namespace

int32_t decode_frame(uint32_t file, uint32_t tileY, uint32_t tileU, uint32_t tileV, uint32_t work) {
    return Decode(file, tileY, tileU, tileV, work);
}

extern const Native kThpNatives[] = {
    WIINX_NATIVE("THPVideoDecode", kThp_2007_08, video_decode),
};
extern const std::size_t kThpNativeCount = std::size(kThpNatives);

}  // namespace wiinx::sdk::thp
