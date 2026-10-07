// libdol's GX renderer on the console, alone (docs/deko3d.md): display lists
// built here as a game's FIFO would carry them - CP vertex tables, XF matrices,
// projection, viewport and channels, BP TEV and pixel state, draws - run by
// wiinx::gx::CommandProcessor and drawn by dk::draw_gx. No game, no Aurora.
//
// What it should show: on dark blue, a triangle shading red, green and blue,
// and beside it a quad from orange to purple. The log says how many draws were
// made and how many were refused. + leaves.
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

    consoleDebugInit(debugDevice_SVC);
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

    unsigned logged = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }
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
        wiinx::gx::CommandProcessor cp(state, nullptr, [&](const wiinx::gx::Draw& d) {
            (dol::dk::draw_gx(state, d, width, height) ? drawn : refused)++;
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
