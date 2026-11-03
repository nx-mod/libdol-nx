// GX's transform-stage setters, natively: GXSetScissor.
//
// A menu that draws its 3D models piece by piece sets the scissor before each
// piece: Bomberman Blast's Characters menu calls GXSetScissor thousands of
// times a frame, 5% of it translated, each call four gather-pipe stores through
// the checked path. Here it is the original's register words, worked out the
// same way, noted in gxData the same way, and sent as one write.
//
// Read off the code of rvl.gx@2007-12-11 as Bomberman Blast ships it (the
// build Mario Kart Wii links): the scissor's top-left and bottom-right corners
// in BP registers 0x20 and 0x21, each offset by 342, kept in gxData so a later
// GXGetScissor and GXSetScissorBoxOffset start from them.
#include "gx_internal.hpp"
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/guest.hpp"
#include "wiinx/core/host.hpp"

namespace wiinx::sdk::gx {
namespace {

using detail::insert;

// gxData's copies of the two scissor registers (suScis0, suScis1)
constexpr u32 kScissor0 = 0x148;
constexpr u32 kScissor1 = 0x14C;
constexpr u32 kOffset = 342;  // the scissor's origin, as the hardware counts it
constexpr u32 kLow11 = 0x000007FFu;   // rlwimi ...,0,21,31
constexpr u32 kHigh11 = 0x007FF000u;  // rlwimi ...,12,9,19

}  // namespace

void set_scissor(u32 left, u32 top, u32 width, u32 height) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    const u32 y0 = top + kOffset;
    const u32 x0 = left + kOffset;
    u32 scissor0 = load<u32>(gd + kScissor0);
    scissor0 = insert(scissor0, y0, 0, kLow11);
    scissor0 = insert(scissor0, x0, 12, kHigh11);
    store<u32>(gd + kScissor0, scissor0);
    u32 scissor1 = load<u32>(gd + kScissor1);
    scissor1 = insert(scissor1, y0 + height - 1, 0, kLow11);
    scissor1 = insert(scissor1, x0 + width - 1, 12, kHigh11);
    store<u32>(gd + kScissor1, scissor1);
    {
        detail::Fifo fifo;
        fifo.bp(load<u32>(gd + kScissor0));
        fifo.bp(load<u32>(gd + kScissor1));
    }
    store<u16>(gd + detail::GxData2007::kBpSentNot, 0);
}

namespace {
void GXSetScissor(Cpu* cpu) {
    const Host& h = host();
    set_scissor(h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5), h.gpr(cpu, 6));
}
}  // namespace

extern const Native kGxTransformNatives[] = {
    WIINX_NATIVE("GXSetScissor", kGx_2007_12, GXSetScissor),
};
extern const std::size_t kGxTransformNativeCount = std::size(kGxTransformNatives);

}  // namespace wiinx::sdk::gx
