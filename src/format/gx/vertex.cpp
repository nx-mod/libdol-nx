// GX's transform unit as GLSL (wiinx/format/gx/vertex.hpp).
//
// The lighting and texgen arithmetic are Aurora's (lib/gx/shader.cpp, MIT,
// Copyright the Aurora contributors), after Dolphin's.
#include "wiinx/format/gx/vertex.hpp"

#include <string_view>

namespace wiinx::gx {
namespace xf_detail {

constexpr std::string_view kPrelude = R"(#version 460

struct XfLight {
    vec4 pos;
    vec4 dir;
    vec4 color;
    vec4 cosAtt;
    vec4 distAtt;
};

layout (std140, binding = 1) uniform XfBlock {
    vec4 posMtx[30];
    vec4 nrmMtx[30];
    vec4 texMtx[30];
    vec4 postMtx[60];
    vec4 proj[4];
    vec4 chanAmb[2];
    vec4 chanMat[2];
    XfLight lights[8];
    ivec4 posMtxIndex;
} xf;

layout (location = 0) in vec3 aPosition;

vec3 xf_rows(int row, vec4 v) {
    return vec3(dot(xf.posMtx[row], v), dot(xf.posMtx[row + 1], v), dot(xf.posMtx[row + 2], v));
}

// XF's own rounding: a colour leaves the lighting block in 8 bits
vec3 xf_byte(vec3 v) { return round(clamp(v, 0.0, 1.0) * 255.0) / 255.0; }
float xf_byte(float v) { return round(clamp(v, 0.0, 1.0) * 255.0) / 255.0; }

layout (location = 0) out vec4 vColor0;
layout (location = 1) out vec4 vColor1;
)";

std::string Num(unsigned v) { return std::to_string(v); }

struct Generator {
    const VertexConfig& config;
    bool ok = true;

    // the vertex colour a channel takes, or white when the vertices carry none
    // (GX supplies opaque white; with only colour 1 present, channel 0 reads it)
    std::string VertexColor(unsigned channel) const {
        if (channel == 0) {
            return config.hasColor0 ? "aColor0" : config.hasColor1 ? "aColor1" : "vec4(1.0)";
        }
        return config.hasColor0 && config.hasColor1 ? "aColor1" : "vec4(1.0)";
    }

    // one channel's colour (alpha = false) or alpha, as XF lights it
    std::string Channel(unsigned index, bool alpha) {
        const ColorChannel& cc = config.channels[index + (alpha ? 2 : 0)];
        const std::string n = Num(index);
        const std::string comp = alpha ? ".a" : ".rgb";
        const std::string material =
            (cc.material == ColorSrc::Vertex ? VertexColor(index) : "xf.chanMat[" + n + "]") + comp;
        const std::string target = alpha ? "vColor" + n + ".a" : "vColor" + n + ".rgb";
        if (!cc.lighting) {
            return "    " + target + " = xf_byte(" + material + ");\n";
        }
        if (!config.hasNormal && cc.diffuse != DiffuseFn::None) {
            ok = false;  // diffuse lighting with no normals: nothing to light by
        }
        const std::string ambient =
            (cc.ambient == ColorSrc::Vertex ? VertexColor(index) : "xf.chanAmb[" + n + "]") + comp;
        const std::string type = alpha ? "int" : "ivec3";
        const std::string ftype = alpha ? "float" : "vec3";

        std::string attn;
        switch (cc.attenuation) {
        case AttnFn::None: attn = "            attn = 1.0;\n"; break;
        case AttnFn::Spot:
            attn = "            float cosine = max(0.0, dot(ldir, light.dir.xyz));\n"
                   "            float cosAttn = dot(light.cosAtt.xyz, vec3(1.0, cosine, cosine * cosine));\n"
                   "            float distAttn = dot(light.distAtt.xyz, vec3(1.0, dist, dist2));\n"
                   "            attn = max(0.0, cosAttn / distAttn);\n";
            break;
        case AttnFn::Spec: {
            const std::string distAtt = cc.diffuse != DiffuseFn::None ? "normalize(light.distAtt.xyz)" : "light.distAtt.xyz";
            attn = "            attn = dot(mvNormal, ldir) >= 0.0 ? max(0.0, dot(mvNormal, light.dir.xyz)) : 0.0;\n"
                   "            float cosAttn = dot(light.cosAtt.xyz, vec3(1.0, attn, attn * attn));\n"
                   "            float distAttn = dot(" + distAtt + ", vec3(1.0, attn, attn * attn));\n"
                   "            attn = distAttn != 0.0 ? max(0.0, cosAttn / distAttn) : (cosAttn > 0.0 ? 1.0 : 0.0);\n";
            break;
        }
        default: ok = false; break;
        }
        const std::string dirSetup =
            cc.attenuation == AttnFn::None
                ? "            vec3 ldir = light.pos.xyz - mvPosition;\n"
                  "            float dist2 = dot(ldir, ldir);\n"
                  "            float dist = sqrt(dist2);\n"
                  "            ldir = dist > 0.0 ? ldir / max(dist, 1e-20) : mvNormal;\n"
                : "            vec3 ldir = light.pos.xyz - mvPosition;\n"
                  "            float dist2 = dot(ldir, ldir);\n"
                  "            float dist = sqrt(dist2);\n"
                  "            ldir = ldir / dist;\n";
        std::string diffuse;
        switch (cc.diffuse) {
        case DiffuseFn::None: diffuse = "1.0"; break;
        case DiffuseFn::Sign: diffuse = "dot(ldir, mvNormal)"; break;
        case DiffuseFn::Clamp: diffuse = "max(0.0, dot(ldir, mvNormal))"; break;
        }
        const std::string lightColor = "light.color" + comp;
        const std::string lo = alpha ? "0" : "ivec3(0)";
        const std::string hi = alpha ? "255" : "ivec3(255)";
        return "    {\n"
               "        " + type + " lighting = " + type + "(round(" + ambient + " * 255.0));\n"
               "        for (int i = 0; i < 8; i++) {\n"
               "            if ((" + Num(cc.lightMask) + " & (1 << i)) == 0) { continue; }\n"
               "            XfLight light = xf.lights[i];\n" +
               dirSetup +
               "            float attn;\n" + attn +
               "            float diff = " + diffuse + ";\n"
               "            lighting += " + type + "(round(attn * diff * " + lightColor + " * 255.0));\n"
               "        }\n"
               "        " + type + " lacc = clamp(lighting, " + lo + ", " + hi + ");\n"
               "        " + type + " material = " + type + "(round(" + material + " * 255.0));\n"
               "        " + target + " = " + ftype + "((material * (lacc + (lacc >> 7))) >> 8) / 255.0;\n"
               "    }\n";
    }

    std::string TexGenCode(unsigned i) {
        const TexGen& tg = config.texGens[i];
        const std::string n = Num(i);
        std::string src;
        const unsigned source = tg.source;
        if (source >= 4 && source <= 11) {
            const unsigned coord = source - 4;
            if (!(config.texCoords & (1u << coord))) {
                ok = false;
                return {};
            }
            src = "vec4(aTex" + Num(coord) + ", 1.0, 1.0)";
        } else if (source == static_cast<unsigned>(TexGenSrc::Position)) {
            src = "vec4(aPosition, 1.0)";
        } else if (source == static_cast<unsigned>(TexGenSrc::Normal)) {
            if (!config.hasNormal) {
                ok = false;
                return {};
            }
            src = "vec4(aNormal, 1.0)";
        } else if (source == static_cast<unsigned>(TexGenSrc::Color0) || source == static_cast<unsigned>(TexGenSrc::Color1)) {
            src = "vec4(" + VertexColor(source == static_cast<unsigned>(TexGenSrc::Color1) ? 1 : 0) + ".rgb, 1.0)";
        } else {
            ok = false;
            return {};
        }
        std::string s = "    vec4 tc" + n + " = " + src + ";\n";
        if (tg.type == TexGenType::Mtx2x4 || tg.type == TexGenType::Mtx3x4) {
            if (tg.matrix == kIdentity) {
                s += "    vec3 tc" + n + "t = tc" + n + ".xyz;\n";
            } else if (tg.matrix < 10) {
                const std::string r = Num(tg.matrix * 3u);
                s += "    vec3 tc" + n + "t = vec3(dot(xf.texMtx[" + r + "], tc" + n + "), dot(xf.texMtx[" + r +
                     " + 1], tc" + n + "), dot(xf.texMtx[" + r + " + 2], tc" + n + "));\n";
            } else {
                ok = false;
                return {};
            }
            if (tg.type == TexGenType::Mtx2x4) {
                s += "    tc" + n + "t.z = 1.0;\n";
            }
        } else if (tg.type == TexGenType::SRTG) {
            s += "    vec3 tc" + n + "t = vec3(tc" + n + ".xy, 1.0);\n";
        } else {
            ok = false;
            return {};
        }
        if (config.dualTexture && tg.normalize) {
            s += "    tc" + n + "t = normalize(tc" + n + "t);\n";
        }
        if (config.dualTexture && tg.postMatrix != kIdentity) {
            if (tg.postMatrix >= 20) {
                ok = false;
                return {};
            }
            const std::string r = Num(tg.postMatrix * 3u);
            s += "    vec4 tc" + n + "p4 = vec4(tc" + n + "t, 1.0);\n"
                 "    tc" + n + "t = vec3(dot(xf.postMtx[" + r + "], tc" + n + "p4), dot(xf.postMtx[" + r + " + 1], tc" +
                 n + "p4), dot(xf.postMtx[" + r + " + 2], tc" + n + "p4));\n";
        }
        if (tg.type != TexGenType::SRTG) {
            // a q of 0: GX keeps s and t, halved and clamped, rather than dividing by it
            s += "    if (tc" + n + "t.z == 0.0) { tc" + n + "t = vec3(clamp(tc" + n + "t.xy / 2.0, -1.0, 1.0), 1.0); }\n";
        }
        s += "    vTexCoord" + n + " = tc" + n + "t;\n";
        return s;
    }

    std::string Run() {
        if (config.channelCount > 2 || config.texGenCount > 8) {
            return {};
        }
        std::string s(kPrelude);
        if (config.hasNormal) {
            s += "layout (location = 1) in vec3 aNormal;\n";
        }
        if (config.hasColor0) {
            s += "layout (location = 2) in vec4 aColor0;\n";
        }
        if (config.hasColor1) {
            s += "layout (location = 3) in vec4 aColor1;\n";
        }
        for (unsigned t = 0; t < 8; ++t) {
            if (config.texCoords & (1u << t)) {
                s += "layout (location = " + Num(4 + t) + ") in vec2 aTex" + Num(t) + ";\n";
            }
        }
        if (config.indexedPosMtx) {
            s += "layout (location = 12) in float aPosMtxIndex;\n";
        }
        for (unsigned t = 0; t < config.texGenCount; ++t) {
            s += "layout (location = " + Num(2 + t) + ") out vec3 vTexCoord" + Num(t) + ";\n";
        }
        s += "\nvoid main() {\n";
        s += config.indexedPosMtx ? "    int row = int(aPosMtxIndex);\n" : "    int row = xf.posMtxIndex.x;\n";
        s += "    vec3 mvPosition = xf_rows(row, vec4(aPosition, 1.0));\n";
        if (config.hasNormal) {
            s += "    vec3 mvNormal = normalize(vec3(dot(xf.nrmMtx[row].xyz, aNormal), dot(xf.nrmMtx[row + 1].xyz, aNormal),"
                 " dot(xf.nrmMtx[row + 2].xyz, aNormal)));\n";
        } else {
            s += "    vec3 mvNormal = vec3(0.0, 0.0, 1.0);\n";
        }
        s += "    vec4 view = vec4(mvPosition, 1.0);\n"
             "    vec4 clip = vec4(dot(xf.proj[0], view), dot(xf.proj[1], view), dot(xf.proj[2], view), dot(xf.proj[3], view));\n"
             "    // GX's clip depth runs -w..0; a 0..1 depth range wants 0..w\n"
             "    clip.z += clip.w;\n"
             "    gl_Position = clip;\n\n";
        s += "    vColor0 = vec4(0.0);\n    vColor1 = vec4(0.0);\n";
        for (unsigned c = 0; c < config.channelCount; ++c) {
            s += Channel(c, false);
            s += Channel(c, true);
        }
        for (unsigned t = 0; t < config.texGenCount; ++t) {
            s += TexGenCode(t);
        }
        s += "}\n";
        return ok ? s : std::string();
    }
};

}  // namespace xf_detail

std::string VertexConfigKey(const VertexConfig& c) {
    std::string k;
    const auto put = [&k](unsigned v) { k.push_back(static_cast<char>(v)); };
    put(c.hasNormal | c.hasColor0 << 1 | c.hasColor1 << 2 | c.indexedPosMtx << 3 | c.dualTexture << 4);
    put(c.texCoords), put(c.channelCount), put(c.texGenCount);
    for (unsigned i = 0; i < 4; ++i) {
        const ColorChannel& ch = c.channels[i];
        put(static_cast<unsigned>(ch.material) | static_cast<unsigned>(ch.ambient) << 1 | ch.lighting << 2 |
            static_cast<unsigned>(ch.diffuse) << 3 | static_cast<unsigned>(ch.attenuation) << 5);
        put(ch.lightMask);
    }
    for (unsigned i = 0; i < c.texGenCount && i < 8; ++i) {
        const TexGen& t = c.texGens[i];
        put(static_cast<unsigned>(t.type)), put(t.source), put(t.matrix), put(t.normalize), put(t.postMatrix);
    }
    return k;
}

std::string XfVertexGlsl(const VertexConfig& config) {
    xf_detail::Generator generator{config};
    return generator.Run();
}

}  // namespace wiinx::gx
