// The GPU on deko3d: GX draws (dk.h).
//
// A draw from wiinx::gx::CommandProcessor, with the State it was made in:
// the shaders the state describes (tev.hpp, vertex.hpp), its two uniform
// blocks, its vertices in the shaders' layout (vertex_stream.hpp), its pixel
// state and viewport - all through the frame's staging memory. Textures are
// not bound yet.
//
// The shaders are generated for each draw and found again by their source
// (dk_shader_switch.cpp compiles each once); keeping them by configuration
// instead is for when a measurement asks.
#include "dk.h"

#include "wiinx/format/gx/state.hpp"
#include "wiinx/format/gx/tev.hpp"
#include "wiinx/format/gx/vertex.hpp"
#include "wiinx/format/gx/vertex_stream.hpp"

#include <cstring>
#include <vector>

namespace dol::dk {
namespace {

using namespace wiinx::gx;

bool primitive_of(uint8_t gx, DkPrimitive* out) {
  switch (gx & 0xF8) {
  case 0x80: // GX_QUADS
  case 0x88: *out = DkPrimitive_Quads; return true; // GX_QUADS2
  case 0x90: *out = DkPrimitive_Triangles; return true;
  case 0x98: *out = DkPrimitive_TriangleStrip; return true;
  case 0xA0: *out = DkPrimitive_TriangleFan; return true;
  case 0xA8: *out = DkPrimitive_Lines; return true;
  case 0xB0: *out = DkPrimitive_LineStrip; return true;
  case 0xB8: *out = DkPrimitive_Points; return true;
  }
  return false;
}

// the vertex shader's thirteen inputs, as vertex_stream.hpp lays them out
void bind_vertex_layout(DkCmdBuf cmd) {
  static const DkVtxAttribState attribs[13] = {
      {0, 0, kOffsetPosition, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetNormal, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetColor0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetColor1, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetTex0 + 0 * 8, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetTex0 + 1 * 8, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetTex0 + 2 * 8, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetTex0 + 3 * 8, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetTex0 + 4 * 8, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetTex0 + 5 * 8, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetTex0 + 6 * 8, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetTex0 + 7 * 8, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      {0, 0, kOffsetPosMtxIndex, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
  };
  static const DkVtxBufferState buffer = {kVertexFloats * sizeof(float), 0};
  dkCmdBufBindVtxAttribState(cmd, attribs, 13);
  dkCmdBufBindVtxBufferState(cmd, &buffer, 1);
}

// `bytes` into the frame's staging memory; its GPU address, 0 when full
DkGpuAddr stage_bytes(const void* data, uint32_t size, uint32_t alignment) {
  Allocation a = stage(size, alignment);
  if (!a) {
    return 0;
  }
  std::memcpy(a.cpu(), data, size);
  return a.gpu();
}

// XF's viewport registers, in GX's 640-wide EFB space, onto the target
DkViewport viewport_of(const State& state, uint32_t width, uint32_t height) {
  const auto& vp = state.Viewport();
  const float scale = static_cast<float>(width) / 640.0f;
  const float w = vp[0] * 2.0f, h = -vp[1] * 2.0f;
  constexpr float z24 = 16777216.0f;
  DkViewport out;
  out.x = (vp[3] - 342.0f - w / 2.0f) * scale;
  out.y = (vp[4] - 342.0f - h / 2.0f) * scale;
  out.width = w * scale;
  out.height = h * scale;
  out.near = (vp[5] - vp[2]) / z24;
  out.far = vp[5] / z24;
  // (a viewport never set: the whole target)
  if (out.width <= 0.0f || out.height <= 0.0f) {
    out = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
  }
  return out;
}

} // namespace

bool draw_gx(const State& state, const Draw& draw, uint32_t width, uint32_t height) {
  DkPrimitive primitive;
  if (draw.count == 0 || !primitive_of(draw.primitive, &primitive)) {
    return false;
  }
  const std::string vertexGlsl = XfVertexGlsl(state.Vertex());
  const std::string fragmentGlsl = TevFragmentGlsl(state.Tev());
  if (vertexGlsl.empty() || fragmentGlsl.empty()) {
    return false;
  }
  const DkShader* vertex = shader(ShaderStage::Vertex, vertexGlsl.c_str());
  const DkShader* fragment = shader(ShaderStage::Fragment, fragmentGlsl.c_str());
  if (!vertex || !fragment) {
    return false;
  }

  // the vertices, converted where the GPU reads them
  const uint32_t vertexBytes = static_cast<uint32_t>(draw.count * kVertexFloats * sizeof(float));
  Allocation vertices = stage(vertexBytes, 16);
  if (!vertices ||
      ConvertVertices(draw.format, draw.records, draw.bytes, draw.count, static_cast<float*>(vertices.cpu())) !=
          draw.count) {
    return false;
  }
  const std::vector<uint8_t> tevBlock = state.TevUniforms();
  const std::vector<uint8_t> xfBlock = state.XfUniforms();
  const DkGpuAddr tevAddr = stage_bytes(tevBlock.data(), static_cast<uint32_t>(tevBlock.size()), DK_UNIFORM_BUF_ALIGNMENT);
  const DkGpuAddr xfAddr = stage_bytes(xfBlock.data(), static_cast<uint32_t>(xfBlock.size()), DK_UNIFORM_BUF_ALIGNMENT);
  if (!tevAddr || !xfAddr) {
    return false;
  }

  DkCmdBuf cmd = commands();
  const DkShader* shaders[] = {vertex, fragment};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, tevAddr, static_cast<uint32_t>(tevBlock.size()));
  dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 1, xfAddr, static_cast<uint32_t>(xfBlock.size()));
  bind(pipeline_state(state.Pixel()));
  bind_vertex_layout(cmd);
  dkCmdBufBindVtxBuffer(cmd, 0, vertices.gpu(), vertexBytes);
  const DkViewport viewport = viewport_of(state, width, height);
  dkCmdBufSetViewports(cmd, 0, &viewport, 1);
  dkCmdBufDraw(cmd, primitive, draw.count, 1, 0, 0);
  return true;
}

} // namespace dol::dk
