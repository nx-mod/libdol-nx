#pragma once

// GX's command stream: what the FIFO and display lists carry, run against a
// State (state.hpp).
//
// Commands (big-endian):
//   0x00         NOP
//   0x08 r v     a CP register: the vertex descriptor, attribute tables,
//                array bases and strides
//   0x10 h d..   XF: (count - 1) << 16 | address, then count words
//   0x20..0x38   XF loaded from an array (matrices, lights): index << 16 |
//                (count - 1) << 12 | address
//   0x40 a s     a display list called: address, size
//   0x48         vertex cache invalidated
//   0x61 v       a BP register
//   0x80..0xBF   a draw: the primitive in the top five bits, the attribute
//                table in the bottom three, a 16-bit vertex count, the records
//
// A draw reaches the callback with every indexed attribute already fetched
// from its array, so its records are direct - what vertex_stream.hpp
// converts. Guest memory (arrays, called lists) is read through the accessor.

#include "wiinx/format/gx/state.hpp"
#include "wiinx/format/gx/vertex_stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace wiinx::gx {

struct Draw {
    std::uint8_t primitive = 0;  // GX_QUADS 0x80, GX_TRIANGLES 0x90, ... GX_POINTS 0xB8
    std::uint8_t table = 0;      // the vertex attribute table (GX_VTXFMT0..7)
    std::uint16_t count = 0;
    VertexFormat format;         // every present attribute direct
    const std::uint8_t* records = nullptr;
    std::size_t bytes = 0;
};

class CommandProcessor {
  public:
    // `size` bytes of guest memory at `address`, or null when it is not there
    using Memory = std::function<const std::uint8_t*(std::uint32_t address, std::uint32_t size)>;
    using DrawFn = std::function<void(const Draw&)>;

    CommandProcessor(State& state, Memory memory, DrawFn draw);

    // Runs `size` bytes of commands. Returns the bytes it ran: all of them, or
    // up to the first command it does not know or that runs past the end.
    std::size_t Run(const std::uint8_t* data, std::size_t size);

    // The format a table describes for the current descriptor, as the records
    // carry it (indexed attributes as 1 or 2 bytes are not a VertexFormat:
    // this is the direct format the draw callback receives).
    VertexFormat DirectFormat(std::uint8_t table) const;

  private:
    State& mState;
    Memory mMemory;
    DrawFn mDraw;
    std::uint32_t mVcdLo = 0, mVcdHi = 0;
    std::array<std::uint32_t, 8> mVatA{}, mVatB{}, mVatC{};
    std::array<std::uint32_t, 16> mArrayBase{}, mArrayStride{};
    std::vector<std::uint8_t> mExpanded;
    int mDepth = 0;

    void LoadCP(std::uint8_t reg, std::uint32_t value);
    std::uint8_t AttrType(std::uint8_t attr) const;  // GX_NONE / DIRECT / INDEX8 / INDEX16
    AttrFormat Format(std::uint8_t table, std::uint8_t attr) const;
    bool RunDraw(std::uint8_t cmd, const std::uint8_t* data, std::size_t size, std::size_t& pos);
    void LoadIndexed(std::uint8_t cmd, std::uint32_t value);
};

}  // namespace wiinx::gx
