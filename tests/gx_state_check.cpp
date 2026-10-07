// GX state: SDK calls as a game makes them, then what a draw takes from them -
// the shader configurations (both compiled by UAM where it is installed), the
// pixel state, and the uniform bytes at the offsets the shaders read.
#include "wiinx/format/gx/state.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace wiinx::gx;

namespace {
int gFailures = 0;
std::string gUam;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += ok ? 0 : 1;
}

bool Compiles(const std::string& glsl, const char* stage, const char* name) {
    if (glsl.empty()) {
        return false;
    }
    if (gUam.empty()) {
        return true;
    }
    const std::string path = std::string("gx_state_") + name + "." + stage;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    std::fwrite(glsl.data(), 1, glsl.size(), f);
    std::fclose(f);
    const int status = std::system((gUam + " -s " + stage + " " + path + " -o " + path + ".dksh 2>&1").c_str());
    if (status != 0) {
        std::printf("%s\n", glsl.c_str());
    }
    std::remove((path + ".dksh").c_str());
    if (status == 0) {
        std::remove(path.c_str());
    }
    return status == 0;
}

float FloatAt(const std::vector<std::uint8_t>& block, std::size_t offset) {
    float v;
    std::memcpy(&v, block.data() + offset, sizeof(v));
    return v;
}

bool Near(float a, float b) { return a > b - 1e-5f && a < b + 1e-5f; }
}  // namespace

int main() {
    if (const char* env = std::getenv("WIINX_UAM")) {
        gUam = env;
    } else if (std::FILE* f = std::fopen("/opt/devkitpro/tools/bin/uam", "rb")) {
        std::fclose(f);
        gUam = "/opt/devkitpro/tools/bin/uam";
    }

    std::printf("GXInit's defaults\n");
    {
        State state;
        const TevConfig tev = state.Tev();
        Check("one stage, one texgen", tev.stageCount == 1 && tev.texCoordCount == 1);
        Check("stage 0 replaces with texture 0 at coordinate 0",
              tev.stages[0].texMap == 0 && tev.stages[0].texCoord == 0 &&
                  tev.stages[0].color.d == static_cast<std::uint8_t>(TevColorArg::TexC));
        Check("swap table 1 is RRRA", tev.swapTable[1].g == SwapChannel::Red && tev.swapTable[1].a == SwapChannel::Alpha);
        Check("depth tested and written, back faces culled",
              state.Pixel().depthTest && state.Pixel().depthWrite && state.Pixel().cull == CullMode::Back);
        Check("position matrix 0 is the identity", Near(FloatAt(state.XfUniforms(), 0), 1.0f) &&
                                                     Near(FloatAt(state.XfUniforms(), 20), 1.0f));
    }

    std::printf("A lit, textured, blended draw, set up as a game sets it up\n");
    {
        State state;
        state.ClearVtxDesc();
        state.SetVtxDesc(9, 1);   // GX_VA_POS, direct
        state.SetVtxDesc(25, 1);  // GX_VA_NBT: lit by its normal
        state.SetVtxDesc(11, 1);  // GX_VA_CLR0
        state.SetVtxDesc(13, 1);  // GX_VA_TEX0
        state.SetNumChans(1);
        state.SetChanCtrl(4, true, 0, 1, 0x01 | 0x04, 2, 1);  // GX_COLOR0A0, lit, amb reg, mat vtx, lights 0 and 2
        state.SetChanAmbColor(4, {32, 64, 96, 255});
        Light light;
        light.pos[0] = 10.0f;
        light.color = {255, 128, 0, 255};
        light.distAtt[1] = 0.5f;
        state.LoadLight(0x04, light);  // GX_LIGHT2
        state.SetNumTexGens(2);
        state.SetTexCoordGen2(0, 1, 4, 36, false, 125);  // 2x4 from TEX0 by GX_TEXMTX2
        state.SetTexCoordGen2(1, 0, 1, 60, false, 125);  // 3x4 from the normal, identity
        const float texMtx[3][4] = {{2, 0, 0, 0.25f}, {0, 2, 0, 0.5f}, {9, 9, 9, 9}};
        state.LoadTexMtx(texMtx, 36, true);
        state.SetNumTevStages(2);
        state.SetTevOrder(0, 0, 0, 4);
        state.SetTevOp(0, TevMode::Modulate);
        state.SetTevOrder(1, 1, 3 | 0x100, 0xFF);  // GX_TEX_DISABLE: no texture
        state.SetTevOp(1, TevMode::Decal);
        state.SetTevKColor(2, {255, 0, 0, 128});
        state.SetAlphaCompare(4, 0, 0, 7, 0);  // GREATER 0 AND ALWAYS
        state.SetBlendMode(1, 4, 5, 3);         // blend, src alpha, inv src alpha
        state.SetZMode(true, 3, false);
        state.SetCullMode(0);
        const float pos[3][4] = {{1, 0, 0, 5}, {0, 1, 0, 6}, {0, 0, 1, 7}};
        state.LoadPosMtx(pos, 3);
        state.SetCurrentMtx(3);

        const TevConfig tev = state.Tev();
        const VertexConfig vtx = state.Vertex();
        Check("stage 1's preset reads the previous stage", tev.stages[1].color.a == static_cast<std::uint8_t>(TevColorArg::CPrev));
        Check("GX_TEX_DISABLE leaves the stage untextured", tev.stages[1].texMap == kTexNull);
        Check("COLOR0A0 lit the alpha channel too", vtx.channels[2].lighting && vtx.channels[2].lightMask == 0x05);
        Check("NBT counts as a normal", vtx.hasNormal);
        Check("GX_TEXMTX2 is texture matrix slot 2", vtx.texGens[0].matrix == 2);
        Check("GX_IDENTITY is no matrix", vtx.texGens[1].matrix == kIdentity);
        Check("the vertex shader compiles", Compiles(XfVertexGlsl(vtx), "vert", "lit"));
        Check("the fragment shader compiles", Compiles(TevFragmentGlsl(tev), "frag", "lit"));

        const PixelState pixel = state.Pixel();
        Check("blend with source alpha", pixel.blendMode == BlendMode::Blend && pixel.srcFactor == BlendFactor::SrcAlpha &&
                                             pixel.dstFactor == BlendFactor::InvSrcAlpha);
        Check("depth tested, not written, nothing culled", pixel.depthTest && !pixel.depthWrite && pixel.cull == CullMode::None);

        const std::vector<std::uint8_t> xf = state.XfUniforms();
        Check("the XF block is its declared size", xf.size() == State::kXfBlockSize);
        Check("position matrix rows 3..5 hold the loaded matrix", Near(FloatAt(xf, 3 * 16 + 12), 5.0f) &&
                                                                    Near(FloatAt(xf, 5 * 16 + 12), 7.0f));
        Check("texture matrix 2's rows start at 960 + 6 * 16", Near(FloatAt(xf, 960 + 6 * 16), 2.0f) &&
                                                                 Near(FloatAt(xf, 960 + 7 * 16 + 12), 0.5f));
        Check("a 2x4 matrix leaves its third row alone", Near(FloatAt(xf, 960 + 8 * 16), 0.0f));
        Check("channel 0's ambient colour", Near(FloatAt(xf, 2464 + 4), 64 / 255.0f));
        Check("light 2's position and colour", Near(FloatAt(xf, 2528 + 2 * 80), 10.0f) &&
                                                Near(FloatAt(xf, 2528 + 2 * 80 + 32 + 4), 128 / 255.0f));
        Check("light 2's distance attenuation", Near(FloatAt(xf, 2528 + 2 * 80 + 64 + 4), 0.5f));
        std::int32_t row;
        std::memcpy(&row, xf.data() + 3168, sizeof(row));
        Check("the current matrix is row 3", row == 3);

        const std::vector<std::uint8_t> tevBlock = state.TevUniforms();
        Check("konst colour 2 in the TEV block", tevBlock.size() == State::kTevBlockSize &&
                                                    Near(FloatAt(tevBlock, 64 + 2 * 16), 1.0f) &&
                                                    Near(FloatAt(tevBlock, 64 + 2 * 16 + 12), 128 / 255.0f));
    }

    std::printf(gFailures ? "%d FAILED\n" : "all passed\n", gFailures);
    return gFailures ? 1 : 0;
}
