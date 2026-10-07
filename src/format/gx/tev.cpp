// The TEV as GLSL (wiinx/format/gx/tev.hpp).
//
// The arithmetic is Aurora's (lib/gx/shader.cpp, MIT, Copyright the Aurora
// contributors), after Dolphin's: inputs wrapped to 8 bits, the lerp in
// integers with the hardware's rounding, the result clamped to 0..255 or kept
// in the registers' 11-bit signed range.
#include "wiinx/format/gx/tev.hpp"

#include <string_view>

namespace wiinx::gx {
namespace tev_detail {

constexpr std::string_view kPrelude = R"(#version 460

layout (std140, binding = 0) uniform TevBlock {
    vec4 tevreg[4];
    vec4 kcolor[4];
    vec4 fogColor;   // (w: the destination alpha)
    vec4 fogParams;  // A, C, B's magnitude, B's shift
} tev;

float tev_wrap(float v) { float s = v * 255.0; return (s - floor(s / 256.0) * 256.0) / 255.0; }
vec3 tev_wrap(vec3 v) { vec3 s = v * 255.0; return (s - floor(s / 256.0) * 256.0) / 255.0; }

int tev_regular(int a, int b, int c, int d, int bias, int scale, bool sub) {
    int d_part = d + bias;
    int lerp_part = (a << 8) + (b - a) * (c + (c >> 7));
    if (scale == 1) { d_part <<= 1; lerp_part <<= 1; }
    if (scale == 2) { d_part <<= 2; lerp_part <<= 2; }
    if (scale != 3) { lerp_part += sub ? 127 : 128; }
    int lerp = lerp_part >> 8;
    int result = sub ? d_part - lerp : d_part + lerp;
    if (scale == 3) { result >>= 1; }
    return result;
}

int tev_byte(float v) { return int(round(v * 255.0)); }

float tev_regular(float a, float b, float c, float d, int bias, int scale, bool sub) {
    return float(tev_regular(tev_byte(a), tev_byte(b), tev_byte(c), tev_byte(d), bias, scale, sub)) / 255.0;
}

vec3 tev_regular(vec3 a, vec3 b, vec3 c, vec3 d, int bias, int scale, bool sub) {
    return vec3(tev_regular(a.r, b.r, c.r, d.r, bias, scale, sub),
                tev_regular(a.g, b.g, c.g, d.g, bias, scale, sub),
                tev_regular(a.b, b.b, c.b, d.b, bias, scale, sub));
}

float tev_r8(vec3 v) { return round(v.r * 255.0); }
float tev_gr16(vec3 v) { return round(dot(v.rg * 255.0, vec2(1.0, 256.0))); }
float tev_bgr24(vec3 v) { return round(dot(v.rgb * 255.0, vec3(1.0, 256.0, 65536.0))); }

layout (location = 0) in vec4 vColor0;
layout (location = 1) in vec4 vColor1;
layout (location = 0) out vec4 outColor;
)";

std::string_view SwapLetter(SwapChannel c) {
    switch (c) {
    case SwapChannel::Red: return "r";
    case SwapChannel::Green: return "g";
    case SwapChannel::Blue: return "b";
    default: return "a";
    }
}

std::string Swizzle3(const TevSwap& s) {
    return std::string(SwapLetter(s.r)) + std::string(SwapLetter(s.g)) + std::string(SwapLetter(s.b));
}

std::string Num(unsigned v) { return std::to_string(v); }

struct Generator {
    const TevConfig& config;
    bool ok = true;

    bool HasTexture(const TevStage& stage) const {
        return stage.texMap < 8 && stage.texCoord < 8 && stage.texCoord < config.texCoordCount;
    }

    // what a stage reads as its texture when it has none: GX gives 0 with no
    // texture coordinates at all, 1 otherwise
    std::string NoTexture() const { return config.texCoordCount == 0 ? "0.0" : "1.0"; }

    std::string Ras(const TevStage& stage) const {
        return stage.channel == TevChannel::Color1 ? "vColor1" : "vColor0";
    }

    bool RasZero(const TevStage& stage) const {
        return stage.channel != TevChannel::Color0 && stage.channel != TevChannel::Color1;
    }

    std::string KColor(std::uint8_t sel) {
        if (sel <= 7) {
            return "vec3(" + Num(8 - sel) + ".0 / 8.0)";
        }
        if (sel >= 0x0C && sel <= 0x0F) {
            return "tev.kcolor[" + Num(sel - 0x0C) + "].rgb";
        }
        if (sel >= 0x10 && sel <= 0x1F) {
            static constexpr const char* kComp = "rgba";
            return "vec3(tev.kcolor[" + Num(sel & 3) + "]." + kComp[(sel - 0x10) / 4] + ")";
        }
        ok = false;
        return "vec3(0.0)";
    }

    std::string KAlpha(std::uint8_t sel) {
        if (sel <= 7) {
            return "(" + Num(8 - sel) + ".0 / 8.0)";
        }
        if (sel >= 0x10 && sel <= 0x1F) {
            static constexpr const char* kComp = "rgba";
            return std::string("tev.kcolor[") + Num(sel & 3) + "]." + kComp[(sel - 0x10) / 4];
        }
        ok = false;
        return "0.0";
    }

    std::string ColorArg(std::uint8_t arg, unsigned i, const TevStage& stage) {
        const TevSwap& tex = config.swapTable[stage.texSwap & 3];
        const TevSwap& ras = config.swapTable[stage.rasSwap & 3];
        switch (static_cast<TevColorArg>(arg)) {
        case TevColorArg::CPrev: return "prev.rgb";
        case TevColorArg::APrev: return "vec3(prev.a)";
        case TevColorArg::C0: return "reg0.rgb";
        case TevColorArg::A0: return "vec3(reg0.a)";
        case TevColorArg::C1: return "reg1.rgb";
        case TevColorArg::A1: return "vec3(reg1.a)";
        case TevColorArg::C2: return "reg2.rgb";
        case TevColorArg::A2: return "vec3(reg2.a)";
        case TevColorArg::TexC:
            return HasTexture(stage) ? "sampled" + Num(i) + "." + Swizzle3(tex) : "vec3(" + NoTexture() + ")";
        case TevColorArg::TexA:
            return HasTexture(stage) ? "vec3(sampled" + Num(i) + "." + std::string(SwapLetter(tex.a)) + ")"
                                     : "vec3(" + NoTexture() + ")";
        case TevColorArg::RasC: return RasZero(stage) ? "vec3(0.0)" : Ras(stage) + "." + Swizzle3(ras);
        case TevColorArg::RasA:
            return RasZero(stage) ? "vec3(0.0)" : "vec3(" + Ras(stage) + "." + std::string(SwapLetter(ras.a)) + ")";
        case TevColorArg::One: return "vec3(1.0)";
        case TevColorArg::Half: return "vec3(0.5)";
        case TevColorArg::Konst: return KColor(stage.kcolorSel);
        case TevColorArg::Zero: return "vec3(0.0)";
        }
        ok = false;
        return "vec3(0.0)";
    }

    std::string AlphaArg(std::uint8_t arg, unsigned i, const TevStage& stage) {
        const TevSwap& tex = config.swapTable[stage.texSwap & 3];
        const TevSwap& ras = config.swapTable[stage.rasSwap & 3];
        switch (static_cast<TevAlphaArg>(arg)) {
        case TevAlphaArg::APrev: return "prev.a";
        case TevAlphaArg::A0: return "reg0.a";
        case TevAlphaArg::A1: return "reg1.a";
        case TevAlphaArg::A2: return "reg2.a";
        case TevAlphaArg::TexA:
            return HasTexture(stage) ? "sampled" + Num(i) + "." + std::string(SwapLetter(tex.a)) : NoTexture();
        case TevAlphaArg::RasA:
            return RasZero(stage) ? "0.0" : Ras(stage) + "." + std::string(SwapLetter(ras.a));
        case TevAlphaArg::Konst: return KAlpha(stage.kalphaSel);
        case TevAlphaArg::Zero: return "0.0";
        }
        ok = false;
        return "0.0";
    }

    static std::string Bias(TevBias bias) {
        return bias == TevBias::AddHalf ? "128" : bias == TevBias::SubHalf ? "-128" : "0";
    }

    // a combiner's value before its clamp; `vec` for the colour combiner
    std::string Op(const TevCombiner& k, const std::string& a, const std::string& b, const std::string& c,
                   const std::string& d, bool vec, const std::string& colorA, const std::string& colorB) {
        const std::string zero = vec ? "vec3(0.0)" : "0.0";
        const std::string wrap = "tev_wrap(";
        switch (k.op) {
        case TevOp::Add:
        case TevOp::Sub:
            if (k.bias != TevBias::Zero && k.bias != TevBias::AddHalf && k.bias != TevBias::SubHalf) {
                ok = false;
            }
            return "tev_regular(" + wrap + a + "), " + wrap + b + "), " + wrap + c + "), " + d + ", " + Bias(k.bias) +
                   ", " + Num(static_cast<unsigned>(k.scale)) + ", " + (k.op == TevOp::Sub ? "true" : "false") + ")";
        case TevOp::CompR8Gt:
        case TevOp::CompR8Eq:
        case TevOp::CompGR16Gt:
        case TevOp::CompGR16Eq:
        case TevOp::CompBGR24Gt:
        case TevOp::CompBGR24Eq: {
            // (the alpha combiner compares the colour combiner's A and B here)
            const std::string& ca = vec ? a : colorA;
            const std::string& cb = vec ? b : colorB;
            const char* fn = k.op <= TevOp::CompR8Eq ? "tev_r8" : k.op <= TevOp::CompGR16Eq ? "tev_gr16" : "tev_bgr24";
            const char* rel = (static_cast<unsigned>(k.op) & 1) ? " == " : " > ";
            return "((" + std::string(fn) + "(" + wrap + ca + ")) " + rel + std::string(fn) + "(" + wrap + cb +
                   "))) ? " + wrap + c + ") : " + zero + ") + " + d;
        }
        case TevOp::CompRGB8Gt:
        case TevOp::CompRGB8Eq: {
            const bool eq = k.op == TevOp::CompRGB8Eq;
            if (vec) {
                return "(vec3(" + std::string(eq ? "equal" : "greaterThan") + "(round(" + wrap + a + ") * 255.0), round(" +
                       wrap + b + ") * 255.0))) * " + wrap + c + ")) + " + d;
            }
            return "((round(" + wrap + a + ") * 255.0)" + (eq ? " == " : " > ") + "round(" + wrap + b +
                   ") * 255.0)) ? " + wrap + c + ") : 0.0) + " + d;
        }
        }
        ok = false;
        return zero;
    }

    static std::string Reg(TevReg r) {
        switch (r) {
        case TevReg::Reg0: return "reg0";
        case TevReg::Reg1: return "reg1";
        case TevReg::Reg2: return "reg2";
        default: return "prev";
        }
    }

    static std::string Clamp(const TevCombiner& k, const std::string& expr, bool vec) {
        const std::string lo = k.clamp ? "0.0" : "(-1024.0 / 255.0)";
        const std::string hi = k.clamp ? "1.0" : "(1023.0 / 255.0)";
        return vec ? "clamp(" + expr + ", vec3(" + lo + "), vec3(" + hi + "))" : "clamp(" + expr + ", " + lo + ", " + hi + ")";
    }

    // the alpha test: each half against the final alpha in 0..255
    static std::string AlphaHalf(Compare comp, std::uint8_t ref) {
        const std::string r = Num(ref);
        switch (comp) {
        case Compare::Never: return "false";
        case Compare::Less: return "(alphaTest < " + r + ")";
        case Compare::Equal: return "(alphaTest == " + r + ")";
        case Compare::LEqual: return "(alphaTest <= " + r + ")";
        case Compare::Greater: return "(alphaTest > " + r + ")";
        case Compare::NEqual: return "(alphaTest != " + r + ")";
        case Compare::GEqual: return "(alphaTest >= " + r + ")";
        default: return "true";
        }
    }

    std::string Run() {
        if (config.stageCount < 1 || config.stageCount > 16 || config.texCoordCount > 8) {
            return {};
        }
        std::string s(kPrelude);
        for (unsigned n = 0; n < config.texCoordCount; ++n) {
            s += "layout (location = " + Num(2 + n) + ") in vec3 vTexCoord" + Num(n) + ";\n";
        }
        bool samplerUsed[8] = {};
        for (unsigned i = 0; i < config.stageCount; ++i) {
            if (HasTexture(config.stages[i])) {
                samplerUsed[config.stages[i].texMap] = true;
            }
        }
        for (unsigned m = 0; m < 8; ++m) {
            if (samplerUsed[m]) {
                s += "layout (binding = " + Num(m) + ") uniform sampler2D texMap" + Num(m) + ";\n";
            }
        }
        s += "\nvoid main() {\n"
             "    vec4 prev = tev.tevreg[0];\n"
             "    vec4 reg0 = tev.tevreg[1];\n"
             "    vec4 reg1 = tev.tevreg[2];\n"
             "    vec4 reg2 = tev.tevreg[3];\n";
        for (unsigned i = 0; i < config.stageCount; ++i) {
            const TevStage& stage = config.stages[i];
            const std::string n = Num(i);
            s += "\n    // stage " + n + "\n";
            if (HasTexture(stage)) {
                const std::string tc = "vTexCoord" + Num(stage.texCoord);
                s += "    vec4 sampled" + n + " = texture(texMap" + Num(stage.texMap) + ", " + tc + ".xy / " + tc +
                     ".z);\n";
            }
            const TevCombiner& c = stage.color;
            const TevCombiner& a = stage.alpha;
            const std::string ca = ColorArg(c.a, i, stage), cb = ColorArg(c.b, i, stage);
            const std::string colorExpr = Op(c, ca, cb, ColorArg(c.c, i, stage), ColorArg(c.d, i, stage), true, {}, {});
            const std::string alphaExpr = Op(a, AlphaArg(a.a, i, stage), AlphaArg(a.b, i, stage),
                                             AlphaArg(a.c, i, stage), AlphaArg(a.d, i, stage), false, ca, cb);
            // (both read the registers as the stage found them)
            s += "    vec3 color" + n + " = " + Clamp(c, colorExpr, true) + ";\n";
            s += "    float alpha" + n + " = " + Clamp(a, alphaExpr, false) + ";\n";
            s += "    " + Reg(c.out) + ".rgb = color" + n + ";\n";
            s += "    " + Reg(a.out) + ".a = alpha" + n + ";\n";
        }
        const bool test = !(config.alphaComp0 == Compare::Always && config.alphaComp1 == Compare::Always &&
                            config.alphaOp == AlphaOp::And);
        if (test) {
            const std::string h0 = AlphaHalf(config.alphaComp0, config.alphaRef0);
            const std::string h1 = AlphaHalf(config.alphaComp1, config.alphaRef1);
            std::string pass;
            switch (config.alphaOp) {
            case AlphaOp::And: pass = h0 + " && " + h1; break;
            case AlphaOp::Or: pass = h0 + " || " + h1; break;
            case AlphaOp::Xor: pass = h0 + " != " + h1; break;
            case AlphaOp::Xnor: pass = h0 + " == " + h1; break;
            }
            s += "\n    int alphaTest = int(round(clamp(prev.a, 0.0, 1.0) * 255.0));\n"
                 "    if (!(" + pass + ")) {\n        discard;\n    }\n";
        }
        const unsigned fogFn = config.fogType & 7;
        if (fogFn >= 2) {
            // the depth GX fogs by: the 24-bit z this fragment would write
            s += "\n    // fog, as Dolphin computes it\n"
                 "    uint z24 = uint(round(clamp(gl_FragCoord.z, 0.0, 1.0) * 16777215.0));\n";
            if (config.fogType & 8) {
                s += "    float ze = tev.fogParams.x * float(z24) / 16777216.0;\n";
            } else {
                s += "    float ze = (tev.fogParams.x * 16777216.0) / (tev.fogParams.z - float(z24 >> uint(tev.fogParams.w)));\n";
            }
            s += "    float fog = clamp(ze - tev.fogParams.y, 0.0, 1.0);\n";
            switch (fogFn) {
            case 4: s += "    fog = 1.0 - exp2(-8.0 * fog);\n"; break;
            case 5: s += "    fog = 1.0 - exp2(-8.0 * fog * fog);\n"; break;
            case 6: s += "    fog = exp2(-8.0 * (1.0 - fog));\n"; break;
            case 7: s += "    fog = 1.0 - fog;\n    fog = exp2(-8.0 * fog * fog);\n"; break;
            default: break;  // linear
            }
            s += "    prev.rgb = mix(clamp(prev.rgb, 0.0, 1.0), tev.fogColor.rgb, fog);\n";
        }
        s += "\n    outColor = clamp(prev, 0.0, 1.0);\n";
        if (config.dstAlpha) {
            // (after the alpha test, which tested the TEV's alpha)
            s += "    outColor.a = tev.fogColor.w;\n";
        }
        s += "}\n";
        return ok ? s : std::string();
    }
};

}  // namespace tev_detail

std::string TevConfigKey(const TevConfig& c) {
    std::string k;
    const auto put = [&k](unsigned v) { k.push_back(static_cast<char>(v)); };
    put(c.stageCount), put(c.texCoordCount), put(static_cast<unsigned>(c.alphaComp0)), put(c.alphaRef0);
    put(static_cast<unsigned>(c.alphaOp)), put(static_cast<unsigned>(c.alphaComp1)), put(c.alphaRef1);
    put(c.fogType), put(c.dstAlpha);
    for (const TevSwap& s : c.swapTable) {
        put(static_cast<unsigned>(s.r) | static_cast<unsigned>(s.g) << 2 | static_cast<unsigned>(s.b) << 4 |
            static_cast<unsigned>(s.a) << 6);
    }
    for (unsigned i = 0; i < c.stageCount && i < 16; ++i) {
        const TevStage& s = c.stages[i];
        for (const TevCombiner* k2 : {&s.color, &s.alpha}) {
            put(k2->a), put(k2->b), put(k2->c), put(k2->d), put(static_cast<unsigned>(k2->op));
            put(static_cast<unsigned>(k2->bias) | static_cast<unsigned>(k2->scale) << 2 | k2->clamp << 4 |
                static_cast<unsigned>(k2->out) << 5);
        }
        put(s.kcolorSel), put(s.kalphaSel), put(s.texCoord), put(s.texMap), put(static_cast<unsigned>(s.channel));
        put(s.rasSwap | s.texSwap << 2);
    }
    return k;
}

std::string TevFragmentGlsl(const TevConfig& config) {
    tev_detail::Generator generator{config};
    return generator.Run();
}

}  // namespace wiinx::gx
