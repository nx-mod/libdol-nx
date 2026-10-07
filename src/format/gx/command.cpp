// GX's command stream (wiinx/format/gx/command.hpp).
//
// The CP register layouts are Dolphin's (CPMemory), as Aurora's command
// processor (lib/gx/command_processor.cpp, MIT) reads them.
#include "wiinx/format/gx/command.hpp"

#include <cstring>

namespace wiinx::gx {
namespace command_detail {

std::uint32_t Be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

std::uint16_t Be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }

std::uint32_t Bits(std::uint32_t v, unsigned size, unsigned shift) { return (v >> shift) & ((1u << size) - 1u); }

constexpr std::uint8_t kNone = 0, kDirect = 1, kIndex8 = 2, kIndex16 = 3;
constexpr int kMaxDepth = 4;  // (a list that calls a list that calls ...)

}  // namespace command_detail

using namespace command_detail;

CommandProcessor::CommandProcessor(State& state, Memory memory, DrawFn draw)
    : mState(state), mMemory(std::move(memory)), mDraw(std::move(draw)) {}

std::uint8_t CommandProcessor::AttrType(std::uint8_t attr) const {
    if (attr == VaPnMtxIdx) {
        return Bits(mVcdLo, 1, 0) ? kDirect : kNone;
    }
    if (attr <= 8) {
        return Bits(mVcdLo, 1, attr) ? kDirect : kNone;
    }
    if (attr <= VaClr1) {
        return static_cast<std::uint8_t>(Bits(mVcdLo, 2, 9 + (attr - VaPos) * 2));
    }
    return static_cast<std::uint8_t>(Bits(mVcdHi, 2, (attr - VaTex0) * 2));
}

AttrFormat CommandProcessor::Format(std::uint8_t table, std::uint8_t attr) const {
    const std::uint32_t a = mVatA[table & 7], b = mVatB[table & 7], c = mVatC[table & 7];
    const auto f = [](std::uint32_t count, std::uint32_t type, std::uint32_t frac) {
        return AttrFormat{static_cast<std::uint8_t>(count), static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(frac)};
    };
    switch (attr) {
    case VaPos: return f(Bits(a, 1, 0), Bits(a, 3, 1), Bits(a, 5, 4));
    case VaNrm: return f(Bits(a, 1, 9), Bits(a, 3, 10), 0);
    case VaClr0: return f(Bits(a, 1, 13), Bits(a, 3, 14), 0);
    case VaClr1: return f(Bits(a, 1, 17), Bits(a, 3, 18), 0);
    case VaTex0: return f(Bits(a, 1, 21), Bits(a, 3, 22), Bits(a, 5, 25));
    case VaTex0 + 1: return f(Bits(b, 1, 0), Bits(b, 3, 1), Bits(b, 5, 4));
    case VaTex0 + 2: return f(Bits(b, 1, 9), Bits(b, 3, 10), Bits(b, 5, 13));
    case VaTex0 + 3: return f(Bits(b, 1, 18), Bits(b, 3, 19), Bits(b, 5, 22));
    case VaTex0 + 4: return f(Bits(b, 1, 27), Bits(b, 3, 28), Bits(c, 5, 0));
    case VaTex0 + 5: return f(Bits(c, 1, 5), Bits(c, 3, 6), Bits(c, 5, 9));
    case VaTex0 + 6: return f(Bits(c, 1, 14), Bits(c, 3, 15), Bits(c, 5, 18));
    case VaTex0 + 7: return f(Bits(c, 1, 23), Bits(c, 3, 24), Bits(c, 5, 27));
    default: return {};
    }
}

VertexFormat CommandProcessor::DirectFormat(std::uint8_t table) const {
    VertexFormat format;
    for (std::uint8_t attr = 0; attr < VaCount; ++attr) {
        format.present[attr] = AttrType(attr) != kNone;
        format.format[attr] = Format(table, attr);
    }
    return format;
}

void CommandProcessor::LoadCP(std::uint8_t reg, std::uint32_t value) {
    if (reg == 0x50) {
        mVcdLo = value;
    } else if (reg == 0x60) {
        mVcdHi = value;
    } else if (reg >= 0x70 && reg < 0x78) {
        mVatA[reg - 0x70] = value;
    } else if (reg >= 0x80 && reg < 0x88) {
        mVatB[reg - 0x80] = value;
    } else if (reg >= 0x90 && reg < 0x98) {
        mVatC[reg - 0x90] = value;
    } else if (reg >= 0xA0 && reg < 0xB0) {
        mArrayBase[reg - 0xA0] = value & 0x3FFFFFFF;  // (a physical address)
    } else if (reg >= 0xB0 && reg < 0xC0) {
        mArrayStride[reg - 0xB0] = value & 0xFF;
    } else {
        return;
    }
    if (reg == 0x50 || reg == 0x60) {
        // the shaders follow the descriptor
        mState.ClearVtxDesc();
        for (std::uint8_t attr = 0; attr < VaCount; ++attr) {
            mState.SetVtxDesc(attr, AttrType(attr));
        }
    }
}

// an XF load from an array: A matrices, B normal matrices, C texture
// matrices, D lights (arrays 12..15)
void CommandProcessor::LoadIndexed(std::uint8_t cmd, std::uint32_t value) {
    const std::uint32_t array = 12 + (cmd - 0x20) / 8;
    const std::uint32_t index = value >> 16;
    const std::uint32_t count = Bits(value, 4, 12) + 1;
    const std::uint32_t address = Bits(value, 12, 0);
    const std::uint8_t* src = mMemory ? mMemory(mArrayBase[array] + index * mArrayStride[array], count * 4) : nullptr;
    if (!src) {
        return;
    }
    std::uint32_t words[16];
    for (std::uint32_t i = 0; i < count; ++i) {
        words[i] = Be32(src + i * 4);
    }
    mState.ApplyXF(address, words, count);
}

bool CommandProcessor::RunDraw(std::uint8_t cmd, const std::uint8_t* data, std::size_t size, std::size_t& pos) {
    if (size - pos < 2) {
        return false;
    }
    Draw draw;
    draw.primitive = cmd & 0xF8;
    draw.table = cmd & 7;
    draw.count = Be16(data + pos);
    draw.format = DirectFormat(draw.table);
    pos += 2;

    // the stream's record size, indices as they are, and the direct one
    std::size_t streamSize = 0;
    bool indexed = false;
    for (std::uint8_t attr = 0; attr < VaCount; ++attr) {
        const std::uint8_t type = AttrType(attr);
        if (type == kIndex8 || type == kIndex16) {
            indexed = true;
            streamSize += type == kIndex8 ? 1 : 2;
        } else if (type == kDirect) {
            VertexFormat one;
            one.present[attr] = true;
            one.format[attr] = draw.format.format[attr];
            const std::size_t attrSize = PackedVertexSize(one);
            if (attrSize == 0) {
                return false;
            }
            streamSize += attrSize;
        }
    }
    const std::size_t directSize = PackedVertexSize(draw.format);
    if (directSize == 0 && draw.count != 0) {
        return false;
    }
    const std::size_t streamBytes = streamSize * draw.count;
    if (size - pos < streamBytes) {
        return false;
    }
    const std::uint8_t* in = data + pos;
    pos += streamBytes;

    if (!indexed) {
        draw.records = in;
        draw.bytes = streamBytes;
    } else {
        mExpanded.resize(directSize * draw.count);
        std::uint8_t* out = mExpanded.data();
        for (std::uint16_t v = 0; v < draw.count; ++v) {
            for (std::uint8_t attr = 0; attr < VaCount; ++attr) {
                const std::uint8_t type = AttrType(attr);
                if (type == kNone) {
                    continue;
                }
                VertexFormat one;
                one.present[attr] = true;
                one.format[attr] = draw.format.format[attr];
                const std::size_t attrSize = PackedVertexSize(one);
                if (type == kDirect) {
                    std::memcpy(out, in, attrSize);
                    in += attrSize;
                } else {
                    const std::uint32_t index = type == kIndex8 ? in[0] : Be16(in);
                    in += type == kIndex8 ? 1 : 2;
                    const std::uint32_t array = attr - VaPos;
                    const std::uint8_t* src =
                        mMemory ? mMemory(mArrayBase[array] + index * mArrayStride[array], static_cast<std::uint32_t>(attrSize))
                                : nullptr;
                    if (src) {
                        std::memcpy(out, src, attrSize);
                    } else {
                        std::memset(out, 0, attrSize);
                    }
                }
                out += attrSize;
            }
        }
        draw.records = mExpanded.data();
        draw.bytes = mExpanded.size();
    }
    if (mDraw) {
        mDraw(draw);
    }
    return true;
}

std::size_t CommandProcessor::Run(const std::uint8_t* data, std::size_t size) {
    std::size_t pos = 0;
    while (pos < size) {
        const std::size_t start = pos;
        const std::uint8_t cmd = data[pos++];
        if (cmd >= 0x80 && cmd <= 0xBF) {
            if (!RunDraw(cmd, data, size, pos)) {
                return start;
            }
            continue;
        }
        switch (cmd) {
        case 0x00:
        case 0x48: break;
        case 0x08:
            if (size - pos < 5) {
                return start;
            }
            LoadCP(data[pos], Be32(data + pos + 1));
            pos += 5;
            break;
        case 0x10: {
            if (size - pos < 4) {
                return start;
            }
            const std::uint32_t header = Be32(data + pos);
            const std::uint32_t count = (header >> 16) + 1;
            if (size - pos - 4 < static_cast<std::size_t>(count) * 4) {
                return start;
            }
            std::vector<std::uint32_t> words(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                words[i] = Be32(data + pos + 4 + i * 4);
            }
            mState.ApplyXF(header & 0xFFFF, words.data(), count);
            pos += 4 + static_cast<std::size_t>(count) * 4;
            break;
        }
        case 0x20:
        case 0x28:
        case 0x30:
        case 0x38:
            if (size - pos < 4) {
                return start;
            }
            LoadIndexed(cmd, Be32(data + pos));
            pos += 4;
            break;
        case 0x40: {
            if (size - pos < 8) {
                return start;
            }
            const std::uint32_t address = Be32(data + pos) & 0x3FFFFFFF;
            const std::uint32_t bytes = Be32(data + pos + 4);
            pos += 8;
            const std::uint8_t* list = mMemory ? mMemory(address, bytes) : nullptr;
            if (list && mDepth < kMaxDepth) {
                ++mDepth;
                Run(list, bytes);
                --mDepth;
            }
            break;
        }
        case 0x61:
            if (size - pos < 4) {
                return start;
            }
            mState.ApplyBP(Be32(data + pos));
            pos += 4;
            break;
        default:
            return start;  // not a command: stop rather than read garbage as one
        }
    }
    return pos;
}

}  // namespace wiinx::gx
