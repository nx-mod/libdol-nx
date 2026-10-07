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

    std::printf("Registers, as a display list loads them\n");
    {
        State state;
        const auto bp = [&](std::uint32_t reg, std::uint32_t value) { state.ApplyBP(reg << 24 | value); };
        // colour stage 0: a CPREV, b TEXC, c RASC, d ZERO, clamped, into REG1
        bp(0xC0, 0u << 12 | 8u << 8 | 10u << 4 | 15u | 1u << 19 | 2u << 22);
        // colour stage 1: a compare, GR16 equal (bias 3, op bit 1, scale bits 1)
        bp(0xC2, 2u << 12 | 4u << 8 | 12u << 4 | 15u | 3u << 16 | 1u << 18 | 1u << 20);
        // alpha stage 0: swaps ras 1 tex 2, a APREV b TEXA c RASA d ZERO, subtract, scale 2, bias +half
        bp(0xC1, 1u | 2u << 2 | 7u << 4 | 5u << 7 | 4u << 10 | 0u << 13 | 1u << 18 | 1u << 16 | 1u << 20);
        // orders: stage 0 map 2 coord 1 enabled channel COLOR0A0; stage 1 disabled, channel zero
        bp(0x28, 2u | 1u << 3 | 1u << 6 | 0u << 7 | 5u << 12 | 3u << 15 | 0u << 18 | 7u << 19);
        // TEV register 1 (REG0) red -16, alpha 300 (11-bit signed)
        bp(0xE2, 0x7F0u | 300u << 12);
        // konst 3 blue 0x40 green 0x80
        bp(0xE7, 0x40u | 0x80u << 12 | 1u << 23);
        // konst selections: stage 0 K3, stage 1 K1 alpha; swap table 0's red and green
        bp(0xF6, 1u | 2u << 2 | 0x0Fu << 4 | 0x1Du << 19);
        // gen mode: two texgens, one channel, three stages, hardware cull 1 (the SDK's back)
        bp(0x00, 2u | 1u << 4 | 2u << 10 | 1u << 14);
        // blend: subtract wins over blend; colour on, alpha off
        bp(0x41, 1u | 1u << 3 | 1u << 11 | 4u << 8 | 5u << 5);
        // alpha compare: GEQUAL 10 OR LESS 200
        bp(0xF3, 10u | 200u << 8 | 6u << 16 | 1u << 19 | 1u << 22);
        // a masked write: only the z mode's update bit
        bp(0x40, 1u | 3u << 1 | 1u << 4);
        bp(0xFE, 1u << 4);
        bp(0x40, 0u);

        const TevConfig tev = state.Tev();
        const TevStage& s0 = tev.stages[0];
        Check("colour stage 0's inputs", s0.color.a == 0 && s0.color.b == 8 && s0.color.c == 10 && s0.color.d == 15);
        Check("colour stage 0 writes REG1", s0.color.out == TevReg::Reg1 && s0.color.clamp);
        Check("bias 3 decodes as a compare", tev.stages[1].color.op == TevOp::CompGR16Eq);
        Check("alpha stage 0: subtract, +half, scale 2", s0.alpha.op == TevOp::Sub && s0.alpha.bias == TevBias::AddHalf &&
                                                             s0.alpha.scale == TevScale::Two && s0.alpha.d == 7);
        Check("alpha stage 0's swap selections", s0.rasSwap == 1 && s0.texSwap == 2);
        Check("stage 0's order", s0.texMap == 2 && s0.texCoord == 1 && s0.channel == TevChannel::Color0);
        Check("stage 1's texture disabled, channel zero", tev.stages[1].texMap == kTexNull &&
                                                            tev.stages[1].channel == TevChannel::Zero);
        Check("konst selections", s0.kcolorSel == 0x0F && tev.stages[1].kalphaSel == 0x1D);
        Check("swap table 0 rewritten", tev.swapTable[0].r == SwapChannel::Green && tev.swapTable[0].g == SwapChannel::Blue);
        Check("gen mode's counts", tev.stageCount == 3 && tev.texCoordCount == 2 && state.Vertex().channelCount == 1);
        Check("hardware cull 1 is the SDK's back", state.Pixel().cull == CullMode::Back);
        Check("subtract mode, alpha not written", state.Pixel().blendMode == BlendMode::Subtract && !state.Pixel().alphaWrite);
        Check("the alpha compare", tev.alphaComp0 == Compare::GEqual && tev.alphaRef0 == 10 && tev.alphaOp == AlphaOp::Or &&
                                       tev.alphaComp1 == Compare::Less && tev.alphaRef1 == 200);
        Check("a masked write changes only its bits", state.Pixel().depthTest && !state.Pixel().depthWrite &&
                                                          state.Pixel().depthCompare == 3);
        const std::vector<std::uint8_t> tevBlock = state.TevUniforms();
        Check("REG0's red is signed", Near(FloatAt(tevBlock, 16), -16 / 255.0f) && Near(FloatAt(tevBlock, 16 + 12), 300 / 255.0f));
        Check("konst 3's green and blue", Near(FloatAt(tevBlock, 64 + 48 + 4), 0x80 / 255.0f) &&
                                             Near(FloatAt(tevBlock, 64 + 48 + 8), 0x40 / 255.0f));

        const auto f = [](float v) {
            std::uint32_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            return bits;
        };
        // position matrix 1 (rows 3..5), as GXLoadPosMtxImm(m, GX_PNMTX1) loads it
        std::uint32_t pos[12];
        for (int i = 0; i < 12; ++i) {
            pos[i] = f(static_cast<float>(i));
        }
        state.ApplyXF(12, pos, 12);
        // light 4: colour, then position
        const std::uint32_t color = 0x11223344u;
        state.ApplyXF(0x600 + 4 * 16 + 3, &color, 1);
        const std::uint32_t lpos[3] = {f(1.0f), f(2.0f), f(3.0f)};
        state.ApplyXF(0x600 + 4 * 16 + 10, lpos, 3);
        // channel 0: lit, material from the vertex, lights 0 and 5, clamp, spot
        const std::uint32_t chan = 1u | 1u << 1 | 1u << 2 | 2u << 7 | 3u << 9 | 2u << 11;
        state.ApplyXF(0x100E, &chan, 1);
        // an orthographic projection
        const std::uint32_t proj[7] = {f(2.0f), f(-1.0f), f(3.0f), f(1.0f), f(-0.5f), f(-0.25f), 1};
        state.ApplyXF(0x1020, proj, 7);
        // texgen 1: 3x4 from the normal (row 1); post matrix 2, normalised
        const std::uint32_t tg = 1u << 1 | 1u << 2 | 0u << 4 | 1u << 7;
        state.ApplyXF(0x1041, &tg, 1);
        const std::uint32_t post = 6u | 1u << 8;
        state.ApplyXF(0x1051, &post, 1);
        const std::uint32_t dual = 1;
        state.ApplyXF(0x1012, &dual, 1);

        const std::vector<std::uint8_t> xf = state.XfUniforms();
        Check("XF matrix memory: row 4, column 1", Near(FloatAt(xf, 4 * 16 + 4), 5.0f));
        Check("light 4's colour, unpacked", Near(FloatAt(xf, 2528 + 4 * 80 + 32), 0x11 / 255.0f) &&
                                               Near(FloatAt(xf, 2528 + 4 * 80 + 44), 0x44 / 255.0f));
        Check("light 4's position", Near(FloatAt(xf, 2528 + 4 * 80 + 8), 3.0f));
        const VertexConfig vtx = state.Vertex();
        Check("channel control: lit, lights 0 and 5, clamp, spot", vtx.channels[0].lighting &&
                                                                    vtx.channels[0].lightMask == 0x21 &&
                                                                    vtx.channels[0].diffuse == DiffuseFn::Clamp &&
                                                                    vtx.channels[0].attenuation == AttnFn::Spot &&
                                                                    vtx.channels[0].material == ColorSrc::Vertex);
        Check("an orthographic projection's translation column", Near(FloatAt(xf, 2400 + 12), -1.0f) &&
                                                                    Near(FloatAt(xf, 2400 + 48 + 12), 1.0f));
        Check("texgen 1: 3x4 from the normal", vtx.texGens[1].type == TexGenType::Mtx3x4 && vtx.texGens[1].source == 1);
        Check("post matrix 2, normalised", vtx.texGens[1].postMatrix == 2 && vtx.texGens[1].normalize && vtx.dualTexture);

        state.ClearVtxDesc();
        state.SetVtxDesc(9, 1);
        state.SetVtxDesc(10, 1);
        state.SetVtxDesc(11, 1);
        state.SetVtxDesc(13, 1);
        state.SetVtxDesc(14, 1);
        Check("the vertex shader compiles from registers", Compiles(XfVertexGlsl(state.Vertex()), "vert", "registers"));
        Check("the fragment shader compiles from registers", Compiles(TevFragmentGlsl(state.Tev()), "frag", "registers"));
    }

    std::printf("Fog registers\n");
    {
        State state;
        const auto bp = [&](std::uint32_t reg, std::uint32_t value) { state.ApplyBP(reg << 24 | value); };
        const auto partial = [](float f) {
            std::uint32_t bits;
            std::memcpy(&bits, &f, sizeof(bits));
            return (bits >> 31) << 19 | ((bits >> 23) & 0xFF) << 11 | ((bits >> 12) & 0x7FF);
        };
        bp(0xEE, partial(0.75f));
        bp(0xEF, 0x123456u);
        bp(0xF0, 9u);
        bp(0xF1, partial(-0.5f) | 5u << 21 | 0u << 20);  // exp2, perspective
        bp(0xF2, 0x20u << 16 | 0x40u << 8 | 0x80u);
        const std::vector<std::uint8_t> block = state.TevUniforms();
        Check("the TEV block is its declared size", block.size() == State::kTevBlockSize);
        Check("A and C from GX's partial floats", Near(FloatAt(block, 144), 0.75f) && Near(FloatAt(block, 148), -0.5f));
        Check("B's magnitude and shift", FloatAt(block, 152) == static_cast<float>(0x123456) && FloatAt(block, 156) == 9.0f);
        Check("the fog colour", Near(FloatAt(block, 128), 0x20 / 255.0f) && Near(FloatAt(block, 136), 0x80 / 255.0f));
        Check("exp2, perspective", state.Tev().fogType == 5);
        Check("the fragment shader with fog compiles", Compiles(TevFragmentGlsl(state.Tev()), "frag", "fog_persp"));
        bp(0xF1, partial(0.0f) | 7u << 21 | 1u << 20);  // reverse exp2, orthographic
        Check("reverse exp2, orthographic", state.Tev().fogType == 15);
        Check("the orthographic fog shader compiles", Compiles(TevFragmentGlsl(state.Tev()), "frag", "fog_ortho"));
    }

    std::printf("Texture registers\n");
    {
        State state;
        const auto bp = [&](std::uint32_t reg, std::uint32_t value) { state.ApplyBP(reg << 24 | value); };
        // map 1: 256x128 RGB5A3 at 0x00340000, repeat/mirror, linear with mips, max LOD 3, bias -0.5
        bp(0x81, 1u | 2u << 2 | 1u << 4 | 6u << 5 | (static_cast<std::uint32_t>(-16) & 0xFF) << 9);
        bp(0x85, 0u | (3u * 16) << 8);
        bp(0x89, 255u | 127u << 10 | 5u << 20);
        bp(0x95, 0x00340000u >> 5);
        // map 5: CI8 with an RGB565 palette at line 2 of TMEM's palette half
        bp(0xA9, 63u | 63u << 10 | 9u << 20);
        bp(0xB5, 0x00500000u >> 5);
        bp(0xB9, 2u | 1u << 10);
        // the palette loaded: 256 entries from 0x00600000 into that TMEM line
        bp(0x64, 0x00600000u >> 5);
        bp(0x65, 2u | (256u / 16) << 10);

        const TexMap t1 = state.Texture(1);
        Check("map 1's image", t1.address == 0x00340000u && t1.width == 256 && t1.height == 128 &&
                                   t1.format == TexFormat::RGB5A3);
        Check("map 1 repeats and mirrors", t1.wrapS == 1 && t1.wrapT == 2);
        Check("map 1 filters linearly with four levels", t1.magLinear && t1.minFilter == 6 && t1.mips == 4);
        Check("map 1's LOD bias", Near(t1.lodBias, -0.5f) && Near(t1.maxLod, 3.0f));
        const TexMap t5 = state.Texture(5);
        Check("map 5 sits in the second register bank", t5.address == 0x00500000u && t5.width == 64 &&
                                                            t5.format == TexFormat::C8 && t5.mips == 1);
        Check("map 5's palette is RGB565 at its TMEM address", t5.tlutFormat == TlutFormat::RGB565 &&
                                                                   t5.tlutTmem == 1024);
        const TlutLoad tl = state.Tlut(t5.tlutTmem);
        Check("the palette load says where it came from", tl.address == 0x00600000u && tl.entries == 256);
        Check("a map never bound reads as nothing", state.Texture(7).address == 0);
    }

    std::printf(gFailures ? "%d FAILED\n" : "all passed\n", gFailures);
    return gFailures ? 1 : 0;
}
