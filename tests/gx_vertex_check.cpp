// XF's GLSL: vertex configurations a game uses, generated, then compiled by UAM
// - deko3d's own compiler - when the build machine has it (devkitPro's `uam`,
// or $WIINX_UAM).
#include "wiinx/format/gx/vertex.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace wiinx::gx;

namespace {
int gFailures = 0;
std::string gUam;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += ok ? 0 : 1;
}

std::string FindUam() {
    if (const char* env = std::getenv("WIINX_UAM")) {
        return env;
    }
    if (std::FILE* f = std::fopen("/opt/devkitpro/tools/bin/uam", "rb")) {
        std::fclose(f);
        return "/opt/devkitpro/tools/bin/uam";
    }
    return {};
}

bool Compiles(const std::string& glsl, const char* name) {
    if (glsl.empty()) {
        return false;
    }
    if (gUam.empty()) {
        return true;
    }
    const std::string path = std::string("gx_vertex_") + name + ".vert";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    std::fwrite(glsl.data(), 1, glsl.size(), f);
    std::fclose(f);
    const int status = std::system((gUam + " -s vert " + path + " -o " + path + ".dksh 2>&1").c_str());
    if (status != 0) {
        std::printf("      %s:\n%s\n", path.c_str(), glsl.c_str());
    }
    std::remove((path + ".dksh").c_str());
    if (status == 0) {
        std::remove(path.c_str());
    }
    return status == 0;
}

bool Contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
}  // namespace

int main() {
    gUam = FindUam();
    std::printf(gUam.empty() ? "no UAM here: generating only\n" : "UAM: %s\n", gUam.c_str());

    std::printf("Unlit: a coloured, textured quad\n");
    {
        VertexConfig config;
        config.hasColor0 = true;
        config.texCoords = 1;
        config.channelCount = 1;
        config.channels[0].material = ColorSrc::Vertex;
        config.channels[2].material = ColorSrc::Vertex;
        config.texGenCount = 1;
        const std::string glsl = XfVertexGlsl(config);
        Check("the vertex colour passes through, rounded to 8 bits", Contains(glsl, "xf_byte(aColor0.rgb)"));
        Check("texture coordinate 0 from the vertices, q of 1", Contains(glsl, "vec4(aTex0, 1.0, 1.0)") && Contains(glsl, "tc0t.z = 1.0"));
        Check("UAM compiles it", Compiles(glsl, "unlit"));
    }

    std::printf("Lit: two channels, diffuse with spot attenuation, specular, alpha lit\n");
    {
        VertexConfig config;
        config.hasNormal = true;
        config.hasColor0 = true;
        config.channelCount = 2;
        ColorChannel& c0 = config.channels[0];
        c0.lighting = true;
        c0.material = ColorSrc::Vertex;
        c0.ambient = ColorSrc::Reg;
        c0.lightMask = 0x03;
        c0.diffuse = DiffuseFn::Clamp;
        c0.attenuation = AttnFn::Spot;
        ColorChannel& c1 = config.channels[1];
        c1.lighting = true;
        c1.lightMask = 0x80;
        c1.diffuse = DiffuseFn::None;
        c1.attenuation = AttnFn::Spec;
        ColorChannel& a0 = config.channels[2];
        a0.lighting = true;
        a0.lightMask = 0x04;
        a0.diffuse = DiffuseFn::Sign;
        a0.attenuation = AttnFn::None;
        const std::string glsl = XfVertexGlsl(config);
        Check("lights accumulate in 8-bit integers", Contains(glsl, "ivec3 lighting"));
        Check("the material modulates as XF does", Contains(glsl, "(material * (lacc + (lacc >> 7))) >> 8"));
        Check("spot attenuation", Contains(glsl, "cosAttn / distAttn"));
        Check("UAM compiles it", Compiles(glsl, "lit"));
    }

    std::printf("Texgens: matrices, normals, colours, dual texturing\n");
    {
        VertexConfig config;
        config.hasNormal = true;
        config.hasColor0 = true;
        config.texCoords = 0x03;
        config.indexedPosMtx = true;
        config.texGenCount = 4;
        config.dualTexture = true;
        config.texGens[0] = {TexGenType::Mtx2x4, 4, 3, false, kIdentity};
        config.texGens[1] = {TexGenType::Mtx3x4, static_cast<std::uint8_t>(TexGenSrc::Normal), 9, true, 7};
        config.texGens[2] = {TexGenType::SRTG, static_cast<std::uint8_t>(TexGenSrc::Color0), kIdentity, false, kIdentity};
        config.texGens[3] = {TexGenType::Mtx3x4, static_cast<std::uint8_t>(TexGenSrc::Position), kIdentity, false, 19};
        const std::string glsl = XfVertexGlsl(config);
        Check("the position matrix comes with each vertex", Contains(glsl, "int(aPosMtxIndex)"));
        Check("texture matrix 3 is rows 9..11", Contains(glsl, "xf.texMtx[9]"));
        Check("post-transform matrix 7 is rows 21..23", Contains(glsl, "xf.postMtx[21]"));
        Check("a normalised coordinate", Contains(glsl, "tc1t = normalize(tc1t)"));
        Check("UAM compiles it", Compiles(glsl, "texgens"));
    }

    std::printf("Refusals\n");
    {
        VertexConfig config;
        config.texGenCount = 1;
        config.texGens[0].source = 5;  // texture coordinate 1, which the vertices do not carry
        Check("a texgen from a coordinate the vertices lack", XfVertexGlsl(config).empty());
        config.texCoords = 0x02;
        config.texGens[0].matrix = 10;
        Check("a texture matrix past the tenth", XfVertexGlsl(config).empty());
        VertexConfig lit;
        lit.channelCount = 1;
        lit.channels[0].lighting = true;
        lit.channels[0].diffuse = DiffuseFn::Clamp;
        Check("diffuse lighting without normals", XfVertexGlsl(lit).empty());
    }

    std::printf(gFailures ? "%d FAILED\n" : "all passed\n", gFailures);
    return gFailures ? 1 : 0;
}
