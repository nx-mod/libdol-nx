// The TEV's GLSL: configurations a game uses, generated, then compiled by UAM -
// deko3d's own compiler, the one the console runs - when the build machine has
// it (devkitPro's `uam`, or $WIINX_UAM). Without it the shaders are only
// generated and checked for what they must contain.
#include "wiinx/format/gx/tev.hpp"

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
    const char* candidates[] = {"/opt/devkitpro/tools/bin/uam"};
    for (const char* path : candidates) {
        if (std::FILE* f = std::fopen(path, "rb")) {
            std::fclose(f);
            return path;
        }
    }
    return {};
}

// UAM accepts it, as the console's compiler will
bool Compiles(const std::string& glsl, const char* name) {
    if (gUam.empty()) {
        return true;
    }
    const std::string path = std::string("gx_tev_") + name + ".frag";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    std::fwrite(glsl.data(), 1, glsl.size(), f);
    std::fclose(f);
    const std::string command = gUam + " -s frag " + path + " -o " + path + ".dksh 2>&1";
    const int status = std::system(command.c_str());
    if (status != 0) {
        std::printf("      %s:\n%s\n", path.c_str(), glsl.c_str());
    }
    std::remove((path + ".dksh").c_str());
    if (status == 0) {
        std::remove(path.c_str());
    }
    return status == 0;
}

TevCombiner Color(TevColorArg a, TevColorArg b, TevColorArg c, TevColorArg d) {
    return {static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(c),
            static_cast<std::uint8_t>(d)};
}

TevCombiner Alpha(TevAlphaArg a, TevAlphaArg b, TevAlphaArg c, TevAlphaArg d) {
    return {static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(c),
            static_cast<std::uint8_t>(d)};
}

bool Contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
}  // namespace

int main() {
    gUam = FindUam();
    std::printf(gUam.empty() ? "no UAM here: generating only\n" : "UAM: %s\n", gUam.c_str());

    std::printf("GX_PASSCLR: the rasterised colour\n");
    {
        TevConfig config;
        config.stages[0].channel = TevChannel::Color0;
        config.stages[0].color = Color(TevColorArg::Zero, TevColorArg::Zero, TevColorArg::Zero, TevColorArg::RasC);
        config.stages[0].alpha = Alpha(TevAlphaArg::Zero, TevAlphaArg::Zero, TevAlphaArg::Zero, TevAlphaArg::RasA);
        const std::string glsl = TevFragmentGlsl(config);
        Check("generated", !glsl.empty());
        Check("reads colour channel 0", Contains(glsl, "vColor0.rgb"));
        Check("no alpha test when it always passes", !Contains(glsl, "discard"));
        Check("UAM compiles it", Compiles(glsl, "passclr"));
    }

    std::printf("GX_MODULATE: a texture times the colour, alpha tested\n");
    {
        TevConfig config;
        config.texCoordCount = 1;
        TevStage& s = config.stages[0];
        s.texCoord = 0;
        s.texMap = 0;
        s.channel = TevChannel::Color0;
        s.color = Color(TevColorArg::Zero, TevColorArg::TexC, TevColorArg::RasC, TevColorArg::Zero);
        s.alpha = Alpha(TevAlphaArg::Zero, TevAlphaArg::TexA, TevAlphaArg::RasA, TevAlphaArg::Zero);
        config.alphaComp0 = Compare::Greater;
        config.alphaRef0 = 0;
        config.alphaComp1 = Compare::Greater;
        config.alphaRef1 = 0;
        const std::string glsl = TevFragmentGlsl(config);
        Check("samples texture map 0 at coordinate 0", Contains(glsl, "texture(texMap0, vTexCoord0.xy / vTexCoord0.z)"));
        Check("discards what fails the alpha test", Contains(glsl, "discard"));
        Check("UAM compiles it", Compiles(glsl, "modulate"));
    }

    std::printf("Four stages: konst colours, registers, swaps, scale and bias, unclamped\n");
    {
        TevConfig config;
        config.stageCount = 4;
        config.texCoordCount = 2;
        config.swapTable[1] = {SwapChannel::Alpha, SwapChannel::Alpha, SwapChannel::Alpha, SwapChannel::Red};
        TevStage& s0 = config.stages[0];
        s0.texCoord = 0;
        s0.texMap = 2;
        s0.texSwap = 1;
        s0.kcolorSel = 0x0D;  // K1
        s0.color = Color(TevColorArg::Konst, TevColorArg::TexC, TevColorArg::Half, TevColorArg::Zero);
        s0.color.out = TevReg::Reg0;
        s0.color.scale = TevScale::Two;
        s0.alpha = Alpha(TevAlphaArg::Zero, TevAlphaArg::TexA, TevAlphaArg::Konst, TevAlphaArg::Zero);
        s0.kalphaSel = 0x1E;  // K2 alpha
        TevStage& s1 = config.stages[1];
        s1.channel = TevChannel::Color1;
        s1.rasSwap = 1;
        s1.color = Color(TevColorArg::C0, TevColorArg::RasC, TevColorArg::APrev, TevColorArg::CPrev);
        s1.color.op = TevOp::Sub;
        s1.color.bias = TevBias::AddHalf;
        s1.color.clamp = false;
        s1.alpha = Alpha(TevAlphaArg::A0, TevAlphaArg::RasA, TevAlphaArg::APrev, TevAlphaArg::Zero);
        s1.alpha.scale = TevScale::Half;
        TevStage& s2 = config.stages[2];
        s2.kcolorSel = 3;  // 5/8
        s2.color = Color(TevColorArg::CPrev, TevColorArg::Konst, TevColorArg::C1, TevColorArg::Zero);
        s2.alpha = Alpha(TevAlphaArg::APrev, TevAlphaArg::A1, TevAlphaArg::A2, TevAlphaArg::Zero);
        TevStage& s3 = config.stages[3];
        s3.texCoord = 1;
        s3.texMap = 5;
        s3.color = Color(TevColorArg::Zero, TevColorArg::Zero, TevColorArg::Zero, TevColorArg::TexC);
        s3.alpha = Alpha(TevAlphaArg::Zero, TevAlphaArg::Zero, TevAlphaArg::Zero, TevAlphaArg::APrev);
        const std::string glsl = TevFragmentGlsl(config);
        Check("K1 is read whole", Contains(glsl, "tev.kcolor[1].rgb"));
        Check("K2's alpha is read", Contains(glsl, "tev.kcolor[2].a"));
        Check("5/8 is a constant", Contains(glsl, "vec3(5.0 / 8.0)"));
        Check("the texture swap is applied", Contains(glsl, "sampled0.aaa"));
        Check("the unclamped stage keeps the registers' range", Contains(glsl, "(-1024.0 / 255.0)"));
        Check("UAM compiles it", Compiles(glsl, "four_stages"));
    }

    std::printf("Compares\n");
    {
        TevConfig config;
        config.stageCount = 3;
        config.stages[0].color = Color(TevColorArg::C0, TevColorArg::C1, TevColorArg::One, TevColorArg::Zero);
        config.stages[0].color.op = TevOp::CompGR16Gt;
        config.stages[0].alpha = Alpha(TevAlphaArg::A0, TevAlphaArg::A1, TevAlphaArg::A2, TevAlphaArg::Zero);
        config.stages[0].alpha.op = TevOp::CompBGR24Eq;
        config.stages[1].color = Color(TevColorArg::CPrev, TevColorArg::C2, TevColorArg::One, TevColorArg::Zero);
        config.stages[1].color.op = TevOp::CompRGB8Eq;
        config.stages[1].alpha = Alpha(TevAlphaArg::APrev, TevAlphaArg::A2, TevAlphaArg::A1, TevAlphaArg::Zero);
        config.stages[1].alpha.op = TevOp::CompRGB8Gt;
        config.stages[2].color = Color(TevColorArg::CPrev, TevColorArg::C2, TevColorArg::One, TevColorArg::CPrev);
        config.stages[2].color.op = TevOp::CompR8Gt;
        config.stages[2].alpha = Alpha(TevAlphaArg::Zero, TevAlphaArg::Zero, TevAlphaArg::Zero, TevAlphaArg::APrev);
        config.alphaComp0 = Compare::GEqual;
        config.alphaRef0 = 128;
        config.alphaOp = AlphaOp::Xnor;
        config.alphaComp1 = Compare::LEqual;
        config.alphaRef1 = 200;
        const std::string glsl = TevFragmentGlsl(config);
        Check("GR16 compares two channels as one number", Contains(glsl, "tev_gr16("));
        Check("the alpha combiner's BGR24 compares the colour inputs", Contains(glsl, "tev_bgr24(tev_wrap(reg0.rgb))"));
        Check("RGB8 compares per channel", Contains(glsl, "equal(round("));
        Check("UAM compiles it", Compiles(glsl, "compares"));
    }

    std::printf("Sixteen stages, every one textured\n");
    {
        TevConfig config;
        config.stageCount = 16;
        config.texCoordCount = 8;
        for (unsigned i = 0; i < 16; ++i) {
            TevStage& s = config.stages[i];
            s.texCoord = static_cast<std::uint8_t>(i % 8);
            s.texMap = static_cast<std::uint8_t>((i * 3) % 8);
            s.channel = (i & 1) ? TevChannel::Color1 : TevChannel::Color0;
            s.color = Color(TevColorArg::CPrev, TevColorArg::TexC, TevColorArg::RasC, TevColorArg::Zero);
            s.alpha = Alpha(TevAlphaArg::APrev, TevAlphaArg::TexA, TevAlphaArg::RasA, TevAlphaArg::Zero);
        }
        const std::string glsl = TevFragmentGlsl(config);
        Check("UAM compiles it", Compiles(glsl, "sixteen"));
    }

    std::printf("Refusals\n");
    {
        TevConfig config;
        config.stageCount = 17;
        Check("seventeen stages", TevFragmentGlsl(config).empty());
        config.stageCount = 1;
        config.stages[0].kcolorSel = 0x09;
        config.stages[0].color = Color(TevColorArg::Konst, TevColorArg::Zero, TevColorArg::Zero, TevColorArg::Zero);
        Check("a konst selection the hardware does not have", TevFragmentGlsl(config).empty());
    }

    std::printf(gFailures ? "%d FAILED\n" : "all passed\n", gFailures);
    return gFailures ? 1 : 0;
}
