// libdol's GX renderer on the console, alone (docs/deko3d.md): display lists
// built here as a game's FIFO would carry them - CP vertex tables, XF matrices,
// projection, viewport and channels, BP TEV and pixel state, draws - run by
// wiinx::gx::CommandProcessor and drawn by dk::draw_gx. No game, no Aurora.
//
// What it should show: on dark blue, a triangle shading red, green and blue,
// below it on the left a quad from orange to purple, and on the right a red
// and white checkerboard (an 8x8 RGB565 texture repeated four times each way)
// tinted by its vertex colours. The log says how many of the three draws were
// made and how many refused. + leaves.
#include "../../src/platform/gpu/dk.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct List {
    std::vector<uint8_t> bytes;
    void U8(uint32_t v) { bytes.push_back(static_cast<uint8_t>(v)); }
    void U16(uint32_t v) { U8(v >> 8), U8(v); }
    void U32(uint32_t v) { U16(v >> 16), U16(v); }
    void F32(float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        U32(bits);
    }
    void CP(uint8_t reg, uint32_t value) { U8(0x08), U8(reg), U32(value); }
    void BP(uint32_t reg, uint32_t value) { U8(0x61), U32(reg << 24 | value); }
    void XF(uint16_t address, std::initializer_list<uint32_t> words) {
        U8(0x10), U32(static_cast<uint32_t>(words.size() - 1) << 16 | address);
        for (uint32_t w : words) {
            U32(w);
        }
    }
    static uint32_t F(float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    }
    void Vertex(float x, float y, uint8_t r, uint8_t g, uint8_t b) {
        F32(x), F32(y), F32(0.0f);
        U8(r), U8(g), U8(b), U8(255);
    }
};

// GX set up as GXInit and a 2D overlay would leave it, in the hardware's terms
List Setup() {
    List l;
    l.CP(0x50, 1u << 9 | 1u << 13);                   // position and colour 0, direct
    l.CP(0x60, 0);
    l.CP(0x70, 1u | 4u << 1 | 1u << 13 | 5u << 14);   // table 0: XYZ f32, RGBA8
    // XF: one channel, unlit, from the vertex colour; position matrix 0 the identity
    l.XF(0x1009, {1});
    l.XF(0x100E, {1u});  // colour 0: material from the vertex
    l.XF(0x1010, {1u});  // alpha 0
    l.XF(0x0000, {List::F(1), 0, 0, 0, 0, List::F(1), 0, 0, 0, 0, List::F(1), 0});
    l.XF(0x1018, {0});   // matrix index A: position matrix 0
    // an orthographic projection over 640x480, y down, z 0..1
    l.XF(0x1020, {List::F(2.0f / 640), List::F(-1.0f), List::F(-2.0f / 480), List::F(1.0f), List::F(-1.0f),
                  List::F(-1.0f), 1});
    // the viewport: the whole 640x480 EFB
    l.XF(0x101A, {List::F(320), List::F(-240), List::F(16777215), List::F(320 + 342), List::F(240 + 342),
                  List::F(16777215)});
    l.XF(0x103F, {0});   // no texgens
    // BP: no texgens, one channel, one stage, no culling
    l.BP(0x00, 0u | 1u << 4 | 0u << 10 | 0u << 14);
    // stage 0 passes the rasterised colour: colour d RASC, alpha d RASA, clamped, into PREV
    l.BP(0xC0, 15u << 12 | 15u << 8 | 15u << 4 | 10u | 1u << 19);
    l.BP(0xC1, 7u << 13 | 7u << 10 | 7u << 7 | 5u << 4 | 1u << 19);
    l.BP(0x28, 7u << 3 | 0u << 6 | 0u << 7);  // no texture, channel COLOR0A0
    l.BP(0x40, 0);                            // no depth test
    l.BP(0x41, 1u << 3 | 1u << 4);            // no blending; colour and alpha written
    l.BP(0xF3, 7u << 16 | 7u << 19);          // alpha test always passes
    return l;
}

}  // namespace

int main() {
    PadState pad;

    // (the layer logs to stderr: into a file on the card, to be read over FTP)
    std::freopen("sdmc:/switch/libdol-tests/gx_test.log", "w", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    dol::dk::log("gx_test: start");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    if (!dol::dk::initialize() || !dol::dk::initialize_screen(DkImageFormat_RGBA8_Unorm)) {
        return 1;
    }

    List frame = Setup();
    frame.U8(0x90);  // GX_TRIANGLES, table 0
    frame.U16(3);
    frame.Vertex(120, 360, 255, 32, 32);
    frame.Vertex(320, 80, 32, 255, 32);
    frame.Vertex(520, 360, 32, 32, 255);
    frame.U8(0x80);  // GX_QUADS
    frame.U16(4);
    frame.Vertex(40, 400, 255, 140, 0);
    frame.Vertex(40, 460, 255, 140, 0);
    frame.Vertex(200, 460, 140, 0, 255);
    frame.Vertex(200, 400, 140, 0, 255);

    // a textured quad: an 8x8 RGB565 checkerboard at (fake) 0x00100000, repeated
    // four times each way, modulated by the vertex colour
    static std::vector<uint8_t> memory(0x200);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            // RGB565 tiles are 4x4: tile (x/4, y/4), texel (x%4, y%4)
            const int tile = (y / 4) * 2 + (x / 4);
            const int at = tile * 32 + ((y % 4) * 4 + (x % 4)) * 2;
            const uint16_t texel = ((x + y) & 1) ? 0xF800 : 0xFFFF;  // red or white
            memory[at] = static_cast<uint8_t>(texel >> 8);
            memory[at + 1] = static_cast<uint8_t>(texel);
        }
    }
    const wiinx::gx::CommandProcessor::Memory guest = [](uint32_t address, uint32_t size) -> const uint8_t* {
        address &= 0x3FFFFFFF;
        return address >= 0x00100000 && address + size <= 0x00100000 + memory.size() ? memory.data() + (address - 0x00100000)
                                                                                    : nullptr;
    };
    frame.CP(0x60, 1u);                                          // TEX0 direct
    frame.CP(0x70, 1u | 4u << 1 | 1u << 13 | 5u << 14 | 1u << 21 | 4u << 22);  // + TEX0 ST f32
    frame.XF(0x103F, {1});                                       // one texgen
    frame.XF(0x1040, {0u | 5u << 7});                            // 2x4 from TEX0 (row 5)
    frame.XF(0x1050, {61u});                                     // no post matrix
    frame.XF(0x1018, {0u | 60u << 6});                           // texgen 0: the identity
    frame.BP(0x00, 1u | 1u << 4 | 0u << 10);                     // one texgen, one channel, one stage
    frame.BP(0xC0, 15u << 12 | 8u << 8 | 10u << 4 | 15u | 1u << 19);  // TEXC * RASC
    frame.BP(0xC1, 7u << 13 | 4u << 10 | 5u << 7 | 7u << 4 | 1u << 19);  // TEXA * RASA
    frame.BP(0x28, 0u | 0u << 3 | 1u << 6 | 0u << 7);            // map 0, coordinate 0, COLOR0A0
    frame.BP(0x80, 1u | 1u << 2);                                // repeat both ways, nearest
    frame.BP(0x84, 0);
    frame.BP(0x88, 7u | 7u << 10 | 4u << 20);                    // 8x8 RGB565
    frame.BP(0x94, 0x00100000u >> 5);
    frame.U8(0x80);
    frame.U16(4);
    const float quad[4][4] = {{400, 200, 0, 0}, {400, 440, 0, 4}, {600, 440, 4, 4}, {600, 200, 4, 0}};
    const uint8_t tint[4][3] = {{255, 255, 255}, {255, 255, 128}, {128, 255, 255}, {255, 255, 255}};
    for (int v = 0; v < 4; ++v) {
        frame.Vertex(quad[v][0], quad[v][1], tint[v][0], tint[v][1], tint[v][2]);
        frame.F32(quad[v][2]), frame.F32(quad[v][3]);
    }

    unsigned logged = 0;
    unsigned frameCount = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        // (+, - or B leaves)
        if (padGetButtonsDown(&pad) & (HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_B)) {
            dol::dk::log("gx_test: leaving at frame %u", frameCount);
            break;
        }
        if (frameCount % 120 == 0) {
            dol::dk::log("gx_test: frame %u", frameCount);
        }
        ++frameCount;
        dol::dk::Texture& screen = dol::dk::acquire_screen();
        dol::dk::TextureView view = dol::dk::make_view(screen);
        dol::dk::ColorTarget target;
        target.view = &view;
        target.clear = true;
        target.clear_color[0] = 0.05f, target.clear_color[1] = 0.1f, target.clear_color[2] = 0.35f;
        target.clear_color[3] = 1.0f;
        uint32_t width = 0, height = 0;
        dol::dk::begin_pass(&target, 1, nullptr, &width, &height);

        wiinx::gx::State state;
        unsigned drawn = 0, refused = 0;
        wiinx::gx::CommandProcessor cp(state, guest, [&](const wiinx::gx::Draw& d) {
            (dol::dk::draw_gx(state, d, width, height, guest) ? drawn : refused)++;
        });
        const size_t ran = cp.Run(frame.bytes.data(), frame.bytes.size());
        if (logged++ == 0) {
            dol::dk::log("gx test: ran %zu of %zu bytes; %u draws made, %u refused", ran, frame.bytes.size(), drawn,
                         refused);
        }
        dol::dk::end_pass();
        dol::dk::present();
    }
    dol::dk::shutdown();
    return 0;
}
