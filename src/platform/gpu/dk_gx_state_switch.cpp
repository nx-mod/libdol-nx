// The GPU on deko3d: GX's pixel state as deko3d's state objects (dk.h).
//
// deko3d's logic ops are GX's, in GX's order, and its compares are GX's plus
// one. Blending is where GX differs: its factor 2 is the destination's colour
// on the source side and the source's on the destination side, the same
// factors apply to alpha, and its subtract mode is destination minus source
// with the factors ignored.
#include "dk.h"

namespace dol::dk {
namespace {

using wiinx::gx::BlendFactor;
using wiinx::gx::BlendMode;
using wiinx::gx::CullMode;

DkBlendFactor source_factor(BlendFactor factor) {
  switch (factor) {
  case BlendFactor::Zero: return DkBlendFactor_Zero;
  case BlendFactor::One: return DkBlendFactor_One;
  case BlendFactor::SrcClr: return DkBlendFactor_DstColor;
  case BlendFactor::InvSrcClr: return DkBlendFactor_InvDstColor;
  case BlendFactor::SrcAlpha: return DkBlendFactor_SrcAlpha;
  case BlendFactor::InvSrcAlpha: return DkBlendFactor_InvSrcAlpha;
  case BlendFactor::DstAlpha: return DkBlendFactor_DstAlpha;
  case BlendFactor::InvDstAlpha: return DkBlendFactor_InvDstAlpha;
  }
  return DkBlendFactor_One;
}

DkBlendFactor destination_factor(BlendFactor factor) {
  switch (factor) {
  case BlendFactor::SrcClr: return DkBlendFactor_SrcColor;
  case BlendFactor::InvSrcClr: return DkBlendFactor_InvSrcColor;
  default: return source_factor(factor);
  }
}

} // namespace

PipelineState pipeline_state(const wiinx::gx::PixelState& state) {
  PipelineState out;

  dkRasterizerStateDefaults(&out.rasterizer);
  out.rasterizer.frontFace = DkFrontFace_CW;
  switch (state.cull) {
  case CullMode::None: out.rasterizer.cullMode = DkFace_None; break;
  case CullMode::Front: out.rasterizer.cullMode = DkFace_Front; break;
  case CullMode::Back: out.rasterizer.cullMode = DkFace_Back; break;
  case CullMode::All: out.rasterizer.cullMode = DkFace_FrontAndBack; break;
  }

  dkColorStateDefaults(&out.color);
  dkBlendStateDefaults(&out.blend);
  switch (state.blendMode) {
  case BlendMode::None: break;
  case BlendMode::Blend:
    dkColorStateSetBlendEnable(&out.color, 0, true);
    out.blend.colorBlendOp = out.blend.alphaBlendOp = DkBlendOp_Add;
    out.blend.srcColorBlendFactor = out.blend.srcAlphaBlendFactor = source_factor(state.srcFactor);
    out.blend.dstColorBlendFactor = out.blend.dstAlphaBlendFactor = destination_factor(state.dstFactor);
    break;
  case BlendMode::Subtract:
    dkColorStateSetBlendEnable(&out.color, 0, true);
    out.blend.colorBlendOp = out.blend.alphaBlendOp = DkBlendOp_RevSub;
    out.blend.srcColorBlendFactor = out.blend.srcAlphaBlendFactor = DkBlendFactor_One;
    out.blend.dstColorBlendFactor = out.blend.dstAlphaBlendFactor = DkBlendFactor_One;
    break;
  case BlendMode::Logic:
    out.color.logicOp = static_cast<DkLogicOp>(state.logicOp);
    break;
  }

  dkColorWriteStateDefaults(&out.color_write);
  dkColorWriteStateSetMask(&out.color_write, 0,
                           (state.colorWrite ? DkColorMask_RGB : 0u) | (state.alphaWrite ? DkColorMask_A : 0u));

  dkDepthStencilStateDefaults(&out.depth_stencil);
  out.depth_stencil.depthTestEnable = state.depthTest;
  // (with the test off GX writes no depth either)
  out.depth_stencil.depthWriteEnable = state.depthTest && state.depthWrite;
  out.depth_stencil.depthCompareOp = static_cast<DkCompareOp>((state.depthCompare & 7) + 1);
  out.depth_stencil.stencilTestEnable = false;
  return out;
}

void bind(const PipelineState& state) {
  DkCmdBuf cmd = commands();

  dkCmdBufBindRasterizerState(cmd, &state.rasterizer);
  dkCmdBufBindColorState(cmd, &state.color);
  dkCmdBufBindColorWriteState(cmd, &state.color_write);
  dkCmdBufBindBlendStates(cmd, 0, &state.blend, 1);
  dkCmdBufBindDepthStencilState(cmd, &state.depth_stencil);
}

} // namespace dol::dk
