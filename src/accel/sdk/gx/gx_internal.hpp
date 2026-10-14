// What the GX natives share: the SDK's own state (gxData) as this build lays it
// out, and the command stream they write into.
//
// A native here does exactly what the SDK function does - it updates the
// game's gxData and writes the same register words - so it can be bound alone,
// next to SDK functions still running as the game's code, and display lists,
// GXBeginDisplayList's save of gxData and the GXGet* functions all keep
// working. The offsets are Mario Kart Wii's (doldecomp/mkw, CC0), named as
// doldecomp/ogws and projectPiki/pikmin2 name them (CC0).
#pragma once

#include "wiinx/core/guest.hpp"
#include "wiinx/core/host.hpp"

namespace wiinx::sdk::gx::detail {

// gxData, rvl.gx@2007-12-11 (the fields the natives so far touch)
struct GxData2007 {
    static constexpr u32 kBpSentNot = 0x002;          // u16: 0 once a BP register has gone out
    static constexpr u32 kTexRegionCallback = 0x518;  // GXTexRegion* (*)(GXTexObj*, GXTexMapID)
    static constexpr u32 kTlutRegionCallback = 0x51C; // GXTlutRegion* (*)(u32 tlut)
    static constexpr u32 kTImage0 = 0x564;            // u32[8]: each map's image0, as last loaded
    static constexpr u32 kTMode0 = 0x584;             // u32[8]: each map's mode0, as last loaded
    static constexpr u32 kDirtyState = 0x5FC;         // u32: what __GXSetDirtyState still has to send
};

// The game's gxData: where __GXData points. 0 when the game has none bound.
inline GuestAddr data() noexcept {
    const Host& h = host();
    return h.gx_data ? h.gx_data() : 0;
}

// One native call's commands, sent as one write when it goes out of scope -
// in the order the original writes them to the gather pipe.
class Fifo {
  public:
    Fifo() = default;
    Fifo(const Fifo&) = delete;
    Fifo& operator=(const Fifo&) = delete;
    ~Fifo() { flush(); }

    void u8_(u8 value) noexcept {
        if (mSize == sizeof(mBytes)) {
            flush();
        }
        mBytes[mSize++] = value;
    }
    void u32_(u32 value) noexcept {
        u8_(static_cast<u8>(value >> 24));
        u8_(static_cast<u8>(value >> 16));
        u8_(static_cast<u8>(value >> 8));
        u8_(static_cast<u8>(value));
    }
    // a BP register: its number in the top byte of `value`
    void bp(u32 value) noexcept {
        u8_(0x61);
        u32_(value);
    }

    void flush() noexcept {
        const Host& h = host();
        if (mSize != 0 && h.gx_write) {
            h.gx_write(mBytes, mSize);
        }
        mSize = 0;
    }

  private:
    u8 mBytes[256];
    u32 mSize = 0;
};

// rlwimi as the SDK's code uses it: `value` rotated left by `shift`, inserted
// under `mask`
constexpr u32 insert(u32 into, u32 value, u32 shift, u32 mask) noexcept {
    const u32 rotated = shift ? (value << shift) | (value >> (32 - shift)) : value;
    return (into & ~mask) | (rotated & mask);
}

}  // namespace wiinx::sdk::gx::detail
