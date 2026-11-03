// GXBegin, natively: the header of every primitive a game draws itself.
//
// Each text glyph and layout quad begins with it, and under deko3d it ran
// translated, a few thousand times a frame (3% of Bomberman Blast's Characters
// menu). The dirty-state flush it starts with stays the SDK's own
// (__GXSetDirtyState, called through the game's binding where the original
// calls it); the rest is the header and, the first time after a flush, the
// SDK's dummy primitive.
//
// Read off rvl.gx@2007-12-11 as Bomberman Blast ships it: gxData's first word
// says whether anything has been drawn since the last flush; +4 and +6 are the
// dummy primitive's vertex count and per-vertex size.
#include "gx_internal.hpp"
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/guest.hpp"
#include "wiinx/core/host.hpp"

namespace wiinx::sdk::gx {
namespace {

constexpr u32 kDrewSinceFlush = 0x000;  // u32: 0 until a primitive follows a flush
constexpr u32 kFlushVertices = 0x004;   // u16
constexpr u32 kFlushVertexSize = 0x006; // u16

void GXBegin(Cpu* cpu) {
    const Host& h = host();
    const u32 type = h.gpr(cpu, 3), format = h.gpr(cpu, 4), count = h.gpr(cpu, 5);
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    if (load<u32>(gd + detail::GxData2007::kDirtyState) != 0) {
        static const Bound* const self = bound("GXBegin");
        static const Bound* const dirty = bound("__GXSetDirtyState");
        call_bound(cpu, dirty, self);
    }
    detail::Fifo fifo;
    if (load<u32>(gd + kDrewSinceFlush) == 0) {
        // __GXSendFlushPrim: a strip of zeros the size the SDK keeps
        const u32 vertices = load<u16>(gd + kFlushVertices);
        const u32 bytes = vertices * load<u16>(gd + kFlushVertexSize);
        fifo.u8_(0x98);
        fifo.u8_(static_cast<u8>(vertices >> 8));
        fifo.u8_(static_cast<u8>(vertices));
        for (u32 i = 0; i < (bytes + 3) / 4; ++i) {
            fifo.u32_(0);
        }
        store<u16>(gd + detail::GxData2007::kBpSentNot, 1);
    }
    fifo.u8_(static_cast<u8>(type | format));
    fifo.u8_(static_cast<u8>(count >> 8));
    fifo.u8_(static_cast<u8>(count));
}

}  // namespace

extern const Native kGxGeometryNatives[] = {
    WIINX_NATIVE("GXBegin", kGx_2007_12, GXBegin),
};
extern const std::size_t kGxGeometryNativeCount = std::size(kGxGeometryNatives);

}  // namespace wiinx::sdk::gx
