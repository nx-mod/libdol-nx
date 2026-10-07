// GX vertices as the vertex shader reads them (wiinx/format/gx/vertex_stream.hpp).
#include "wiinx/format/gx/vertex_stream.hpp"

#include <cstring>

namespace wiinx::gx {
namespace stream_detail {

std::size_t CompSize(CompType type) {
    switch (type) {
    case CompType::U8:
    case CompType::S8: return 1;
    case CompType::U16:
    case CompType::S16: return 2;
    case CompType::F32: return 4;
    }
    return 0;
}

std::size_t ColorSize(ColorType type) {
    switch (type) {
    case ColorType::RGB565:
    case ColorType::RGBA4: return 2;
    case ColorType::RGB8:
    case ColorType::RGBA6: return 3;
    case ColorType::RGBX8:
    case ColorType::RGBA8: return 4;
    }
    return 0;
}

// how many components an attribute's count means
unsigned Components(std::uint8_t attr, std::uint8_t count) {
    if (attr == VaPos) {
        return count ? 3 : 2;  // GX_POS_XYZ : GX_POS_XY
    }
    if (attr == VaNrm) {
        return count ? 9 : 3;  // GX_NRM_NBT / NBT3 : GX_NRM_XYZ
    }
    return count ? 2 : 1;  // GX_TEX_ST : GX_TEX_S
}

float ReadComp(const std::uint8_t* p, CompType type, float scale) {
    switch (type) {
    case CompType::U8: return p[0] * scale;
    case CompType::S8: return static_cast<std::int8_t>(p[0]) * scale;
    case CompType::U16: return static_cast<std::uint16_t>(p[0] << 8 | p[1]) * scale;
    case CompType::S16: return static_cast<std::int16_t>(p[0] << 8 | p[1]) * scale;
    case CompType::F32: {
        const std::uint32_t bits = static_cast<std::uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    }
    return 0.0f;
}

void ReadColor(const std::uint8_t* p, ColorType type, float* rgba) {
    unsigned r = 0, g = 0, b = 0, a = 255;
    switch (type) {
    case ColorType::RGB565: {
        const unsigned v = p[0] << 8 | p[1];
        r = (v >> 11 & 0x1f) * 255 / 31;
        g = (v >> 5 & 0x3f) * 255 / 63;
        b = (v & 0x1f) * 255 / 31;
        break;
    }
    case ColorType::RGB8:
    case ColorType::RGBX8: r = p[0], g = p[1], b = p[2]; break;
    case ColorType::RGBA4: {
        const unsigned v = p[0] << 8 | p[1];
        r = (v >> 12 & 0xf) * 17, g = (v >> 8 & 0xf) * 17, b = (v >> 4 & 0xf) * 17, a = (v & 0xf) * 17;
        break;
    }
    case ColorType::RGBA6: {
        const unsigned v = p[0] << 16 | p[1] << 8 | p[2];
        r = (v >> 18 & 0x3f) * 255 / 63, g = (v >> 12 & 0x3f) * 255 / 63, b = (v >> 6 & 0x3f) * 255 / 63,
        a = (v & 0x3f) * 255 / 63;
        break;
    }
    case ColorType::RGBA8: r = p[0], g = p[1], b = p[2], a = p[3]; break;
    }
    rgba[0] = r / 255.0f;
    rgba[1] = g / 255.0f;
    rgba[2] = b / 255.0f;
    rgba[3] = a / 255.0f;
}

// the bytes an attribute takes in a record, 0 when its format is invalid
std::size_t AttrSize(std::uint8_t attr, const AttrFormat& f) {
    if (attr <= 8) {
        return 1;  // matrix indices
    }
    if (attr == VaClr0 || attr == VaClr1) {
        return f.type <= static_cast<std::uint8_t>(ColorType::RGBA8) ? ColorSize(static_cast<ColorType>(f.type)) : 0;
    }
    if (f.type > static_cast<std::uint8_t>(CompType::F32)) {
        return 0;
    }
    return Components(attr, f.count) * CompSize(static_cast<CompType>(f.type));
}

}  // namespace stream_detail

using namespace stream_detail;

std::size_t PackedVertexSize(const VertexFormat& format) {
    std::size_t total = 0;
    for (std::uint8_t attr = 0; attr < VaCount; ++attr) {
        if (!format.present[attr]) {
            continue;
        }
        const std::size_t size = AttrSize(attr, format.format[attr]);
        if (size == 0) {
            return 0;
        }
        total += size;
    }
    return total;
}

std::size_t ConvertVertices(const VertexFormat& format, const std::uint8_t* src, std::size_t size, std::size_t count,
                            float* out) {
    const std::size_t stride = PackedVertexSize(format);
    if (stride == 0 || src == nullptr || out == nullptr) {
        return 0;
    }
    std::size_t done = 0;
    for (; done < count && (done + 1) * stride <= size; ++done) {
        const std::uint8_t* p = src + done * stride;
        float* v = out + done * kVertexFloats;
        std::memset(v, 0, kVertexFloats * sizeof(float));
        // (vertices without colours read opaque white: the shader decides which
        // it uses, from the vertex configuration)
        for (std::uint8_t attr = 0; attr < VaCount; ++attr) {
            if (!format.present[attr]) {
                continue;
            }
            const AttrFormat& f = format.format[attr];
            if (attr == VaPnMtxIdx) {
                v[30] = p[0];
            } else if (attr <= 8) {
                // per-vertex texture matrices: not used by the shader yet
            } else if (attr == VaClr0 || attr == VaClr1) {
                ReadColor(p, static_cast<ColorType>(f.type), v + (attr == VaClr0 ? 6 : 10));
            } else {
                const CompType type = static_cast<CompType>(f.type);
                const std::size_t comp = CompSize(type);
                // normals: a fixed scale whatever the table says (GX ignores it
                // for them): u8 7 fractional bits, s8 6, u16 15, s16 14
                float scale = 1.0f / static_cast<float>(1u << (f.frac & 31));
                if (attr == VaNrm) {
                    switch (type) {
                    case CompType::U8: scale = 1.0f / 128.0f; break;
                    case CompType::S8: scale = 1.0f / 64.0f; break;
                    case CompType::U16: scale = 1.0f / 32768.0f; break;
                    case CompType::S16: scale = 1.0f / 16384.0f; break;
                    case CompType::F32: scale = 1.0f; break;
                    }
                }
                const unsigned n = Components(attr, f.count);
                float* target = attr == VaPos ? v + 0 : attr == VaNrm ? v + 3 : v + 14 + (attr - VaTex0) * 2;
                // (an NBT normal's binormal and tangent are read past, not kept)
                const unsigned keep = attr == VaNrm ? 3 : n;
                for (unsigned c = 0; c < n; ++c) {
                    const float value = ReadComp(p + c * comp, type, scale);
                    if (c < keep) {
                        target[c] = value;
                    }
                }
            }
            p += AttrSize(attr, f);
        }
        if (!format.present[VaClr0]) {
            v[6] = v[7] = v[8] = v[9] = 1.0f;
        }
        if (!format.present[VaClr1]) {
            v[10] = v[11] = v[12] = v[13] = 1.0f;
        }
    }
    return done;
}

}  // namespace wiinx::gx
