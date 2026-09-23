// THP: Nintendo's movie format, decoded.
//
// THP is the SDK's movie format: baseline JPEG frames, 4:2:0, decoded straight
// into GX I8 textures that the game combines with a YUV-to-RGB TEV pass. Run as
// translated PowerPC, the SDK's decoder spends every frame emulating paired
// singles through a Huffman decoder and an IDCT - which is why a menu page with
// animated buttons (each one a THP movie) was the slowest screen in the game
// while static pages were not.
//
// This follows the decompiled SDK decoder (THPDec.c, from the projectPiki/pikmin2
// decompilation, CC0) wherever the format departs
// from ordinary JPEG, because a stock JPEG decoder gets each of these wrong:
//
//  - The entropy-coded data has no byte stuffing. The SDK reads it as whole
//    big-endian words; 0xFF bytes are data, never escapes.
//  - A restart interval carries no RST markers in the stream: the decoder just
//    realigns to the next byte and resets the DC predictors.
//  - The output is not rows of pixels but GX I8 tiles (8x4 texels, 32 bytes):
//    pixel (x, y) of a plane W wide lives at
//        (y / 4) * (W * 4) + (x / 8) * 32 + (y % 4) * 8 + (x % 8)
//    which is what the SDK's `slwi xPos, 2` / `slwi wid, 2` store addressing and
//    its 0x2000-byte-per-strip copies (512 wide) amount to.
//
// The IDCT is derived from libjpeg's float AAN IDCT (jidctflt.c; based in part
// on the work of the Independent JPEG Group - see THIRD-PARTY-NOTICES.md): the SDK uses the same
// algorithm with the same constants, AAN-scaled quantisation tables and a
// 1024-bias / 8 output step, so the pixels agree with the original's.
#include "wiinx/format/media/thp.hpp"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace wiinx::media {
namespace {


// Zig-zag position -> natural (row-major) position.
constexpr uint8_t kNaturalOrder[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// cos(k*pi/16) * sqrt(2), with 1.0 for k = 0: the AAN scale factors.
constexpr double kAanScale[8] = {
    1.0, 1.387039845, 1.306562965, 1.175875602,
    1.0, 0.785694958, 0.541196100, 0.275899379,
};

// Codes up to this many bits resolve with one table lookup; longer ones (rare)
// fall back to walking the canonical code lengths. The SDK does the same with
// its 5-bit quick[] table; 9 bits catches almost every JPEG code in one step.
constexpr int kLookupBits = 9;

struct HuffmanTable {
    bool valid = false;
    uint16_t lookup[1 << kLookupBits] = {};   // (length << 8) | value, 0 = longer code
    int32_t maxCode[18] = {};   // largest code of each length, -1 if none
    int32_t valPtr[17] = {};    // index into vals of the first code of each length
    int32_t minCode[17] = {};
    uint8_t vals[256] = {};
};

struct Component {
    uint8_t quant = 0;
    uint8_t dcTable = 0;
    uint8_t acTable = 0;
    int32_t predDC = 0;
};

class Decoder {
public:
    Decoder(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}

    int32_t Decode(uint8_t* y, uint8_t* u, uint8_t* v);
    uint16_t Width() const { return width_; }
    uint16_t Height() const { return height_; }
    int32_t ParseHeaders();

private:
    uint8_t Byte() { return p_ < end_ ? *p_++ : 0; }
    uint16_t Word() { const uint16_t hi = Byte(); return static_cast<uint16_t>(hi << 8 | Byte()); }

    int32_t ReadFrameHeader();
    int32_t ReadScanHeader();
    void ReadQuantTables();
    void ReadHuffmanTables();

    // Bit reader: no byte stuffing (see the top of the file). Bytes go in whole,
    // so a restart's realignment just drops the bits left of the current byte.
    void Fill(int bits) {
        while (bitCount_ < bits) {
            bitBuffer_ = (bitBuffer_ << 8) | Byte();
            bitCount_ += 8;
        }
    }
    uint32_t Peek(int bits) const {
        return static_cast<uint32_t>(bitBuffer_ >> (bitCount_ - bits)) & ((1u << bits) - 1u);
    }
    uint32_t Bit() {
        Fill(1);
        --bitCount_;
        return static_cast<uint32_t>(bitBuffer_ >> bitCount_) & 1u;
    }
    int32_t Bits(int n) {
        if (n == 0) return 0;
        Fill(n);
        const uint32_t value = Peek(n);
        bitCount_ -= n;
        return static_cast<int32_t>(value);
    }
    void AlignToByte() { bitCount_ -= bitCount_ % 8; }

    int32_t DecodeHuffman(const HuffmanTable& table);
    static int32_t Extend(int32_t value, int bits) {
        return value < (1 << (bits - 1)) ? value - (1 << bits) + 1 : value;
    }
    bool DecodeBlock(Component& comp, float out[64]);  // true: some AC coefficient set
    void Idct(const float coef[64], const float quant[64], uint8_t* plane, uint32_t planeWidth,
              uint32_t x, uint32_t y) const;
    // A block with no AC terms. The float IDCT reduces exactly to dc * q[0] at
    // every pixel (the column pass fills one column, the row pass passes it
    // through unchanged), so this writes the same bytes without the arithmetic.
    // Flat blocks are common in video.
    static void FlatBlock(float dc, uint8_t* plane, uint32_t planeWidth, uint32_t x, uint32_t y);

    const uint8_t* p_;
    const uint8_t* end_;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    uint16_t restartInterval_ = 0;
    Component comp_[3];
    float quant_[4][64] = {};
    HuffmanTable huff_[8];      // index (id << 1) | class, as the SDK lays them out
    uint64_t bitBuffer_ = 0;
    int bitCount_ = 0;
};

int32_t Decoder::ReadFrameHeader() {
    Word();                                  // length
    if (Byte() != 8) return kThpBadPrecision;
    height_ = Word();
    width_ = Word();
    if (Byte() != 3) return kThpBadComponents;
    for (int i = 0; i < 3; ++i) {
        Byte();                              // component id
        const uint8_t sampling = Byte();
        // 4:2:0 only: Y 2x2, U and V 1x1 - the only layout the SDK accepts.
        if ((i == 0 && sampling != 0x22) || (i > 0 && sampling != 0x11)) return kThpBadSampling;
        comp_[i].quant = static_cast<uint8_t>(Byte() & 3);
    }
    return kThpOk;
}

int32_t Decoder::ReadScanHeader() {
    Word();                                  // length
    if (Byte() != 3) return kThpBadComponents;
    for (int i = 0; i < 3; ++i) {
        Byte();                              // component id
        const uint8_t tables = Byte();
        comp_[i].dcTable = static_cast<uint8_t>((tables >> 4) & 3);
        comp_[i].acTable = static_cast<uint8_t>(tables & 3);
        if (!huff_[comp_[i].dcTable << 1].valid || !huff_[(comp_[i].acTable << 1) | 1].valid) {
            return kThpMissingHuffman;
        }
        comp_[i].predDC = 0;
    }
    Byte(); Byte(); Byte();                  // spectral selection / approximation: baseline
    return kThpOk;
}

void Decoder::ReadQuantTables() {
    int32_t length = Word() - 2;
    while (length > 0 && p_ < end_) {
        const uint8_t id = static_cast<uint8_t>(Byte() & 3);
        uint8_t natural[64];
        for (int i = 0; i < 64; ++i) natural[kNaturalOrder[i]] = Byte();
        for (int row = 0; row < 8; ++row) {
            for (int col = 0; col < 8; ++col) {
                quant_[id][row * 8 + col] = static_cast<float>(
                    natural[row * 8 + col] * kAanScale[row] * kAanScale[col]);
            }
        }
        length -= 65;
    }
}

void Decoder::ReadHuffmanTables() {
    int32_t length = Word() - 2;
    while (length > 0 && p_ < end_) {
        const uint8_t spec = Byte();
        HuffmanTable& table = huff_[((spec & 3) << 1) | ((spec >> 4) & 1)];
        uint8_t counts[17] = {};
        int total = 0;
        for (int len = 1; len <= 16; ++len) {
            counts[len] = Byte();
            total += counts[len];
        }
        total = std::min(total, 256);
        for (int i = 0; i < total; ++i) table.vals[i] = Byte();

        // Canonical code assignment (JPEG Annex C / F.2.2.3).
        int32_t code = 0;
        int32_t index = 0;
        for (int len = 1; len <= 16; ++len) {
            table.valPtr[len] = index;
            table.minCode[len] = code;
            code += counts[len];
            index += counts[len];
            table.maxCode[len] = counts[len] ? code - 1 : -1;
            code <<= 1;
        }
        table.maxCode[17] = 0x7FFFFFFF;

        std::fill(std::begin(table.lookup), std::end(table.lookup), uint16_t{0});
        for (int len = 1; len <= kLookupBits; ++len) {
            for (int32_t c = table.minCode[len]; c <= table.maxCode[len]; ++c) {
                const int32_t index = table.valPtr[len] + c - table.minCode[len];
                if (index < 0 || index >= 256) continue;
                const uint16_t entry = static_cast<uint16_t>((len << 8) | table.vals[index]);
                const int shift = kLookupBits - len;
                for (int fill = 0; fill < (1 << shift); ++fill) {
                    table.lookup[(static_cast<uint32_t>(c) << shift) | static_cast<uint32_t>(fill)] = entry;
                }
            }
        }
        table.valid = true;
        length -= 17 + total;
    }
}

int32_t Decoder::ParseHeaders() {
    for (;;) {
        if (p_ >= end_) return kThpBadSyntax;
        if (Byte() != 0xFF) return kThpBadSyntax;
        while (p_ < end_ && *p_ == 0xFF) ++p_;
        const uint8_t marker = Byte();

        int32_t status = kThpOk;
        switch (marker) {
        case 0xC4: ReadHuffmanTables(); break;
        case 0xC0: status = ReadFrameHeader(); break;
        case 0xDB: ReadQuantTables(); break;
        case 0xDD: Word(); restartInterval_ = Word(); break;
        case 0xD8: break;                                   // SOI
        case 0xDA: return ReadScanHeader();                 // entropy data follows
        default:
            if ((marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE) {
                const uint16_t skip = static_cast<uint16_t>(p_[0] << 8 | p_[1]);
                p_ = std::min(p_ + skip, end_);
                break;
            }
            return kThpUnsupportedMarker;
        }
        if (status != kThpOk) return status;
    }
}

int32_t Decoder::DecodeHuffman(const HuffmanTable& table) {
    Fill(kLookupBits);
    const uint16_t entry = table.lookup[Peek(kLookupBits)];
    if (entry != 0) {
        bitCount_ -= entry >> 8;
        return entry & 0xFF;
    }

    // Longer than kLookupBits (rare): peek all 16 bits once and compare each
    // length's prefix against its largest code, as libjpeg does - no per-bit
    // reads.
    Fill(16);
    const uint32_t window = Peek(16);
    for (int len = kLookupBits + 1; len <= 16; ++len) {
        const int32_t code = static_cast<int32_t>(window >> (16 - len));
        if (code <= table.maxCode[len]) {
            bitCount_ -= len;
            const int32_t index = table.valPtr[len] + code - table.minCode[len];
            return (index >= 0 && index < 256) ? table.vals[index] : 0;
        }
    }
    bitCount_ -= 16;
    return 0;                                               // corrupt stream: treat as EOB
}

bool Decoder::DecodeBlock(Component& comp, float out[64]) {
    std::fill(out, out + 64, 0.0f);

    const int32_t dcBits = DecodeHuffman(huff_[comp.dcTable << 1]);
    const int32_t diff = dcBits ? Extend(Bits(dcBits), dcBits) : 0;
    comp.predDC += diff;
    out[0] = static_cast<float>(comp.predDC);

    const HuffmanTable& ac = huff_[(comp.acTable << 1) | 1];
    bool anyAc = false;
    for (int k = 1; k < 64;) {
        const int32_t rs = DecodeHuffman(ac);
        const int run = rs >> 4;
        const int size = rs & 15;
        if (size == 0) {
            if (run != 15) break;                           // EOB
            k += 16;                                        // ZRL
            continue;
        }
        k += run;
        if (k > 63) break;
        out[kNaturalOrder[k]] = static_cast<float>(Extend(Bits(size), size));
        anyAc = true;
        ++k;
    }
    return anyAc;
}

void Decoder::FlatBlock(float dc, uint8_t* plane, uint32_t planeWidth, uint32_t x0, uint32_t y0) {
    int32_t v = static_cast<int32_t>((dc + 1024.0f) * 0.125f);
    const uint8_t value = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    // An 8x8 block is two whole 8x4 tiles: 32 contiguous bytes each.
    uint8_t* top = plane + (y0 >> 2) * (planeWidth * 4) + (x0 >> 3) * 32;
    std::memset(top, value, 32);
    std::memset(top + planeWidth * 4, value, 32);
}

// libjpeg's jidctflt: float AAN, dequantising as it reads. Output is the SDK's
// (x + 1024) / 8, saturated to u8, stored as GX I8 tiles.
void Decoder::Idct(const float coef[64], const float quant[64], uint8_t* plane,
                   uint32_t planeWidth, uint32_t x0, uint32_t y0) const {
    float ws[64];

#if defined(__ARM_NEON)
    // The column pass does the same arithmetic to eight independent columns, so
    // four of them fit in one register and the whole pass is two iterations.
    // The DC-only shortcut survives: it is taken when all four columns have no
    // AC coefficients, which is the common case in video.
    for (int col = 0; col < 8; col += 4) {
        const float32x4_t q0 = vld1q_f32(quant + col);
        const float32x4_t c0 = vmulq_f32(vld1q_f32(coef + col), q0);

        float32x4_t ac = vabsq_f32(vld1q_f32(coef + col + 8));
        for (int k = 16; k <= 56; k += 8) {
            ac = vaddq_f32(ac, vabsq_f32(vld1q_f32(coef + col + k)));
        }
        if (vmaxvq_f32(ac) == 0.0f) {
            for (int row = 0; row < 8; ++row) {
                vst1q_f32(ws + row * 8 + col, c0);
            }
            continue;
        }

        const float32x4_t c2 = vmulq_f32(vld1q_f32(coef + col + 32), vld1q_f32(quant + col + 32));
        const float32x4_t c1 = vmulq_f32(vld1q_f32(coef + col + 16), vld1q_f32(quant + col + 16));
        const float32x4_t c3 = vmulq_f32(vld1q_f32(coef + col + 48), vld1q_f32(quant + col + 48));

        const float32x4_t t10 = vaddq_f32(c0, c2);
        const float32x4_t t11 = vsubq_f32(c0, c2);
        const float32x4_t t13 = vaddq_f32(c1, c3);
        const float32x4_t t12 = vsubq_f32(vmulq_n_f32(vsubq_f32(c1, c3), 1.414213562f), t13);

        const float32x4_t e0 = vaddq_f32(t10, t13);
        const float32x4_t e3 = vsubq_f32(t10, t13);
        const float32x4_t e1 = vaddq_f32(t11, t12);
        const float32x4_t e2 = vsubq_f32(t11, t12);

        const float32x4_t o4 = vmulq_f32(vld1q_f32(coef + col + 8), vld1q_f32(quant + col + 8));
        const float32x4_t o5 = vmulq_f32(vld1q_f32(coef + col + 24), vld1q_f32(quant + col + 24));
        const float32x4_t o6 = vmulq_f32(vld1q_f32(coef + col + 40), vld1q_f32(quant + col + 40));
        const float32x4_t o7 = vmulq_f32(vld1q_f32(coef + col + 56), vld1q_f32(quant + col + 56));

        const float32x4_t z13 = vaddq_f32(o6, o5);
        const float32x4_t z10 = vsubq_f32(o6, o5);
        const float32x4_t z11 = vaddq_f32(o4, o7);
        const float32x4_t z12 = vsubq_f32(o4, o7);

        const float32x4_t f7 = vaddq_f32(z11, z13);
        const float32x4_t f11 = vmulq_n_f32(vsubq_f32(z11, z13), 1.414213562f);
        const float32x4_t z5 = vmulq_n_f32(vaddq_f32(z10, z12), 1.847759065f);
        const float32x4_t f10 = vsubq_f32(vmulq_n_f32(z12, 1.082392200f), z5);
        const float32x4_t f12 = vaddq_f32(vmulq_n_f32(z10, -2.613125930f), z5);
        const float32x4_t f6 = vsubq_f32(f12, f7);
        const float32x4_t f5 = vsubq_f32(f11, f6);
        const float32x4_t f4 = vaddq_f32(f10, f5);

        vst1q_f32(ws + 0 * 8 + col, vaddq_f32(e0, f7));
        vst1q_f32(ws + 7 * 8 + col, vsubq_f32(e0, f7));
        vst1q_f32(ws + 1 * 8 + col, vaddq_f32(e1, f6));
        vst1q_f32(ws + 6 * 8 + col, vsubq_f32(e1, f6));
        vst1q_f32(ws + 2 * 8 + col, vaddq_f32(e2, f5));
        vst1q_f32(ws + 5 * 8 + col, vsubq_f32(e2, f5));
        vst1q_f32(ws + 4 * 8 + col, vaddq_f32(e3, f4));
        vst1q_f32(ws + 3 * 8 + col, vsubq_f32(e3, f4));
    }
#else
    for (int col = 0; col < 8; ++col) {
        const float* in = coef + col;
        const float* q = quant + col;
        if (in[8] == 0 && in[16] == 0 && in[24] == 0 && in[32] == 0 &&
            in[40] == 0 && in[48] == 0 && in[56] == 0) {
            const float dc = in[0] * q[0];
            for (int row = 0; row < 8; ++row) ws[row * 8 + col] = dc;
            continue;
        }

        float tmp0 = in[0] * q[0], tmp1 = in[16] * q[16];
        float tmp2 = in[32] * q[32], tmp3 = in[48] * q[48];
        float tmp10 = tmp0 + tmp2, tmp11 = tmp0 - tmp2;
        float tmp13 = tmp1 + tmp3;
        float tmp12 = (tmp1 - tmp3) * 1.414213562f - tmp13;
        tmp0 = tmp10 + tmp13; tmp3 = tmp10 - tmp13;
        tmp1 = tmp11 + tmp12; tmp2 = tmp11 - tmp12;

        float tmp4 = in[8] * q[8], tmp5 = in[24] * q[24];
        float tmp6 = in[40] * q[40], tmp7 = in[56] * q[56];
        const float z13 = tmp6 + tmp5, z10 = tmp6 - tmp5;
        const float z11 = tmp4 + tmp7, z12 = tmp4 - tmp7;
        tmp7 = z11 + z13;
        tmp11 = (z11 - z13) * 1.414213562f;
        const float z5 = (z10 + z12) * 1.847759065f;
        tmp10 = 1.082392200f * z12 - z5;
        tmp12 = -2.613125930f * z10 + z5;
        tmp6 = tmp12 - tmp7; tmp5 = tmp11 - tmp6; tmp4 = tmp10 + tmp5;

        ws[0 * 8 + col] = tmp0 + tmp7; ws[7 * 8 + col] = tmp0 - tmp7;
        ws[1 * 8 + col] = tmp1 + tmp6; ws[6 * 8 + col] = tmp1 - tmp6;
        ws[2 * 8 + col] = tmp2 + tmp5; ws[5 * 8 + col] = tmp2 - tmp5;
        ws[4 * 8 + col] = tmp3 + tmp4; ws[3 * 8 + col] = tmp3 - tmp4;
    }
#endif

    const auto store = [&](uint32_t x, uint32_t y, float value) {
        int32_t v = static_cast<int32_t>((value + 1024.0f) * 0.125f);
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        plane[(y >> 2) * (planeWidth * 4) + (x >> 3) * 32 + (y & 3) * 8 + (x & 7)] =
            static_cast<uint8_t>(v);
    };

    for (int row = 0; row < 8; ++row) {
        const float* w = ws + row * 8;
        float tmp10 = w[0] + w[4], tmp11 = w[0] - w[4];
        float tmp13 = w[2] + w[6];
        float tmp12 = (w[2] - w[6]) * 1.414213562f - tmp13;
        const float tmp0 = tmp10 + tmp13, tmp3 = tmp10 - tmp13;
        const float tmp1 = tmp11 + tmp12, tmp2 = tmp11 - tmp12;

        const float z13 = w[5] + w[3], z10 = w[5] - w[3];
        const float z11 = w[1] + w[7], z12 = w[1] - w[7];
        const float tmp7 = z11 + z13;
        tmp11 = (z11 - z13) * 1.414213562f;
        const float z5 = (z10 + z12) * 1.847759065f;
        tmp10 = 1.082392200f * z12 - z5;
        tmp12 = -2.613125930f * z10 + z5;
        const float tmp6 = tmp12 - tmp7, tmp5 = tmp11 - tmp6, tmp4 = tmp10 + tmp5;

        const uint32_t y = y0 + static_cast<uint32_t>(row);
#if defined(__ARM_NEON)
        // The eight pixels of a row land in eight consecutive bytes of the
        // tile, so the scale, the clamp and the store are one vector each
        // instead of eight scalar clamps and eight byte writes.
        if ((x0 & 7) == 0) {
            const float32x4_t lo{tmp0 + tmp7, tmp1 + tmp6, tmp2 + tmp5, tmp3 - tmp4};
            const float32x4_t hi{tmp3 + tmp4, tmp2 - tmp5, tmp1 - tmp6, tmp0 - tmp7};
            const float32x4_t bias = vdupq_n_f32(1024.0f);
            const int32x4_t iLo = vcvtq_s32_f32(vmulq_n_f32(vaddq_f32(lo, bias), 0.125f));
            const int32x4_t iHi = vcvtq_s32_f32(vmulq_n_f32(vaddq_f32(hi, bias), 0.125f));
            const uint8x8_t packed =
                vqmovn_u16(vcombine_u16(vqmovun_s32(iLo), vqmovun_s32(iHi)));
            vst1_u8(plane + (y >> 2) * (planeWidth * 4) + (x0 >> 3) * 32 + (y & 3) * 8, packed);
            continue;
        }
#endif
        store(x0 + 0, y, tmp0 + tmp7); store(x0 + 7, y, tmp0 - tmp7);
        store(x0 + 1, y, tmp1 + tmp6); store(x0 + 6, y, tmp1 - tmp6);
        store(x0 + 2, y, tmp2 + tmp5); store(x0 + 5, y, tmp2 - tmp5);
        store(x0 + 4, y, tmp3 + tmp4); store(x0 + 3, y, tmp3 - tmp4);
    }
}

int32_t Decoder::Decode(uint8_t* yPlane, uint8_t* uPlane, uint8_t* vPlane) {
    const uint32_t mcusPerRow = (width_ + 15u) / 16u;
    const uint32_t mcuRows = (height_ + 15u) / 16u;
    const uint32_t chromaWidth = width_ / 2u;
    uint32_t untilRestart = restartInterval_;
    float block[64];

    for (uint32_t mcuY = 0; mcuY < mcuRows; ++mcuY) {
        for (uint32_t mcuX = 0; mcuX < mcusPerRow; ++mcuX) {
            const uint32_t x = mcuX * 16u, y = mcuY * 16u;

            // Same order as the SDK: four Y blocks, then U, then V.
            const auto emit = [&](const Component& comp, bool anyAc, uint8_t* plane, uint32_t planeWidth,
                                  uint32_t bx, uint32_t by) {
                const float* q = quant_[comp.quant];
                if (anyAc) {
                    Idct(block, q, plane, planeWidth, bx, by);
                } else {
                    FlatBlock(block[0] * q[0], plane, planeWidth, bx, by);
                }
            };
            for (int b = 0; b < 4; ++b) {
                const bool anyAc = DecodeBlock(comp_[0], block);
                emit(comp_[0], anyAc, yPlane, width_,
                     x + static_cast<uint32_t>((b & 1) * 8), y + static_cast<uint32_t>((b >> 1) * 8));
            }
            emit(comp_[1], DecodeBlock(comp_[1], block), uPlane, chromaWidth, x / 2u, y / 2u);
            emit(comp_[2], DecodeBlock(comp_[2], block), vPlane, chromaWidth, x / 2u, y / 2u);

            if (restartInterval_ != 0 && --untilRestart == 0) {
                untilRestart = restartInterval_;
                AlignToByte();
                comp_[0].predDC = comp_[1].predDC = comp_[2].predDC = 0;
            }
        }
    }
    return kThpOk;
}


}  // namespace

// The public decoder is the class above, kept out of the header so a caller
// does not carry its Huffman tables about.
struct ThpDecoder::State {
    Decoder decoder;
    State(const std::uint8_t* data, std::size_t size) : decoder(data, size) {}
};

ThpDecoder::ThpDecoder(const std::uint8_t* data, std::size_t size)
    : mState(std::make_unique<State>(data, size)) {}
ThpDecoder::~ThpDecoder() = default;
ThpDecoder::ThpDecoder(ThpDecoder&&) noexcept = default;
ThpDecoder& ThpDecoder::operator=(ThpDecoder&&) noexcept = default;

std::int32_t ThpDecoder::ParseHeaders() { return mState->decoder.ParseHeaders(); }
std::uint16_t ThpDecoder::Width() const { return mState->decoder.Width(); }
std::uint16_t ThpDecoder::Height() const { return mState->decoder.Height(); }

std::int32_t ThpDecoder::Decode(std::uint8_t* luma, std::uint8_t* chromaU, std::uint8_t* chromaV) {
    return mState->decoder.Decode(luma, chromaU, chromaV);
}

}  // namespace wiinx::media
