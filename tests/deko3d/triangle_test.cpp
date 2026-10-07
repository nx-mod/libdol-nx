// libdol's GPU layer on the console, alone (docs/deko3d.md): a triangle,
// its shaders compiled by UAM on the console at start, drawn through dk.h onto
// the screen - no Aurora, no Dawn, no NVK. Blue behind the triangle when it
// worked; the whole screen red when a shader did not compile. + leaves.
#include "../../src/platform/gpu/dk.h"

#include <cstdio>

namespace {

const char* const kVertex = R"(#version 460
layout (location = 0) out vec3 outColor;
const vec2 kPositions[3] = vec2[](vec2(0.0, 0.6), vec2(-0.6, -0.5), vec2(0.6, -0.5));
const vec3 kColors[3] = vec3[](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.2, 0.4, 1.0));
void main() {
    gl_Position = vec4(kPositions[gl_VertexID], 0.0, 1.0);
    outColor = kColors[gl_VertexID];
}
)";

const char* const kFragment = R"(#version 460
layout (location = 0) in vec3 inColor;
layout (location = 0) out vec4 outColor;
void main() {
    outColor = vec4(inColor, 1.0);
}
)";

}  // namespace

int main() {
    PadState pad;

    // (the layer logs to stderr: into a file on the card, to be read over FTP)
    std::freopen("sdmc:/switch/libdol-tests/triangle_test.log", "w", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    dol::dk::log("triangle_test: start");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    if (!dol::dk::initialize() || !dol::dk::initialize_screen(DkImageFormat_RGBA8_Unorm)) {
        return 1;
    }
    const u64 start = armGetSystemTick();
    const DkShader* vertex = dol::dk::shader(dol::dk::ShaderStage::Vertex, kVertex);
    const DkShader* fragment = dol::dk::shader(dol::dk::ShaderStage::Fragment, kFragment);
    const bool compiled = vertex != nullptr && fragment != nullptr;
    dol::dk::log("shaders %s in %llu ms", compiled ? "compiled" : "did not compile",
                 static_cast<unsigned long long>(armTicksToNs(armGetSystemTick() - start) / 1000000));

    DkRasterizerState rasterizer;
    DkColorState color;
    DkColorWriteState colorWrite;
    dkRasterizerStateDefaults(&rasterizer);
    rasterizer.cullMode = DkFace_None;
    dkColorStateDefaults(&color);
    dkColorWriteStateDefaults(&colorWrite);

    unsigned frame = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        // (+, - or B leaves)
        if (padGetButtonsDown(&pad) & (HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_B)) {
            dol::dk::log("triangle_test: leaving at frame %u", frame);
            break;
        }
        if (frame % 120 == 0) {
            dol::dk::log("triangle_test: frame %u", frame);
        }
        ++frame;
        dol::dk::Texture& screen = dol::dk::acquire_screen();
        dol::dk::TextureView view = dol::dk::make_view(screen);
        dol::dk::ColorTarget target;
        target.view = &view;
        target.clear = true;
        target.clear_color[0] = compiled ? 0.05f : 0.8f;
        target.clear_color[1] = compiled ? 0.1f : 0.05f;
        target.clear_color[2] = compiled ? 0.35f : 0.05f;
        target.clear_color[3] = 1.0f;
        dol::dk::begin_pass(&target, 1, nullptr, nullptr, nullptr);
        if (compiled) {
            const DkShader* shaders[] = {vertex, fragment};
            DkCmdBuf commands = dol::dk::commands();
            dkCmdBufBindShaders(commands, DkStageFlag_GraphicsMask, shaders, 2);
            dkCmdBufBindRasterizerState(commands, &rasterizer);
            dkCmdBufBindColorState(commands, &color);
            dkCmdBufBindColorWriteState(commands, &colorWrite);
            dkCmdBufDraw(commands, DkPrimitive_Triangles, 3, 1, 0, 0);
        }
        dol::dk::end_pass();
        dol::dk::present();
    }
    dol::dk::shutdown();
    return 0;
}
