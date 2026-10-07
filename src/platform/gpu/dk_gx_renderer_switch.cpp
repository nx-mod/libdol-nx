// The GPU on deko3d: GX draws (dk.h).
//
// A draw from wiinx::gx::CommandProcessor, with the State it was made in:
// the shaders the state describes (tev.hpp, vertex.hpp), its two uniform
// blocks, its vertices in the shaders' layout (vertex_stream.hpp), its pixel
// state and viewport - all through the frame's staging memory - and the
// textures it samples, decoded from guest memory and kept while their contents
// (a sampled hash) stay the same.
//
// The shaders are kept by configuration: a draw whose state matches one seen
// before builds no GLSL at all.
#include "dk.h"

#include "wiinx/format/gx/state.hpp"
#include "wiinx/format/gx/tev.hpp"
#include "wiinx/format/gx/vertex.hpp"
#include "wiinx/format/gx/vertex_stream.hpp"

#include <cstring>
#include <memory>
#include <unordered_map>
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

// ---------- shaders, kept by configuration (generated and compiled once each)

struct ShaderPair {
  const DkShader* vertex = nullptr;
  const DkShader* fragment = nullptr;
};

std::unordered_map<std::string, ShaderPair> g_shaderPairs;

const ShaderPair& shaders_for(const VertexConfig& vertexConfig, const TevConfig& tev) {
  std::string key = VertexConfigKey(vertexConfig);
  key.push_back('|');
  key += TevConfigKey(tev);
  auto [it, inserted] = g_shaderPairs.try_emplace(std::move(key));
  if (inserted) {
    const std::string vertexGlsl = XfVertexGlsl(vertexConfig);
    const std::string fragmentGlsl = TevFragmentGlsl(tev);
    if (!vertexGlsl.empty() && !fragmentGlsl.empty()) {
      it->second.vertex = shader(ShaderStage::Vertex, vertexGlsl.c_str());
      it->second.fragment = shader(ShaderStage::Fragment, fragmentGlsl.c_str());
    }
  }
  return it->second;
}

// ---------- textures

struct CachedTexture {
  Texture texture;
  TextureView view;
  uint64_t contents = 0;
};

// keyed by what names the image (address, size, format, levels, palette)
std::unordered_map<uint64_t, std::unique_ptr<CachedTexture>> g_textures;
std::unordered_map<uint32_t, std::unique_ptr<Sampler>> g_samplers;

uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

// a sampled hash of `size` bytes: at most 4096 points, so a big texture is not
// read whole every draw (a rewrite that touches none of them goes unseen)
uint64_t sample_hash(const uint8_t* data, size_t size) {
  uint64_t h = 0xcbf29ce484222325ull ^ size;
  const size_t step = size / 4096 + 1;
  for (size_t i = 0; i < size; i += step) {
    h = mix(h, data[i]);
  }
  return h;
}

// GX's hardware min filter: bit 2 linear, low bits the mip mode (0 none, 1 nearest, 2 linear)
Sampler* sampler_for(const TexMap& map) {
  const uint32_t key = map.wrapS | map.wrapT << 2 | map.magLinear << 4 | map.minFilter << 5 |
                       static_cast<uint32_t>(static_cast<int>(map.lodBias * 32) & 0xFF) << 8 |
                       static_cast<uint32_t>(map.minLod * 16) << 16 | static_cast<uint32_t>(map.maxLod * 16) << 24;
  auto& slot = g_samplers[key];
  if (!slot) {
    slot = std::make_unique<Sampler>();
    DkSampler& s = slot->sampler;
    dkSamplerDefaults(&s);
    const auto wrap = [](uint8_t gx) {
      return gx == 1 ? DkWrapMode_Repeat : gx == 2 ? DkWrapMode_MirroredRepeat : DkWrapMode_ClampToEdge;
    };
    s.wrapMode[0] = wrap(map.wrapS);
    s.wrapMode[1] = wrap(map.wrapT);
    s.magFilter = map.magLinear ? DkFilter_Linear : DkFilter_Nearest;
    s.minFilter = (map.minFilter & 4) ? DkFilter_Linear : DkFilter_Nearest;
    const uint8_t mip = map.minFilter & 3;
    s.mipFilter = mip == 1 ? DkMipFilter_Nearest : mip == 2 ? DkMipFilter_Linear : DkMipFilter_None;
    s.lodBias = map.lodBias;
    s.lodClampMin = map.minLod;
    s.lodClampMax = map.maxLod;
  }
  return slot.get();
}

// a map's image, decoded and on the GPU; null when it cannot be read
TextureView* texture_for(const State& state, const TexMap& map, const CommandProcessor::Memory& memory) {
  if (!memory || map.address == 0 || !IsKnown(map.format)) {
    return nullptr;
  }
  const size_t size = TextureDataSize(map.format, map.width, map.height, map.mips);
  const uint8_t* data = size ? memory(map.address, static_cast<uint32_t>(size)) : nullptr;
  if (!data) {
    return nullptr;
  }
  uint64_t key = mix(mix(mix(0xcbf29ce484222325ull, map.address), map.width | map.height << 16),
                     static_cast<uint32_t>(map.format) | map.mips << 8);
  uint64_t contents = sample_hash(data, size);
  const uint8_t* tlut = nullptr;
  TlutLoad load;
  if (IsPaletted(map.format)) {
    load = state.Tlut(map.tlutTmem);
    tlut = load.address && load.entries ? memory(load.address, load.entries * 2) : nullptr;
    if (!tlut) {
      return nullptr;
    }
    key = mix(mix(key, load.address), static_cast<uint32_t>(map.tlutFormat) | load.entries << 8);
    contents = mix(contents, sample_hash(tlut, load.entries * 2));
  }

  auto& slot = g_textures[key];
  if (slot && slot->contents == contents) {
    return &slot->view;
  }
  const std::vector<uint8_t> rgba =
      IsPaletted(map.format)
          ? DecodeTexturePalette(map.format, map.width, map.height, map.mips, data, size, map.tlutFormat, load.entries,
                                 tlut, load.entries * 2)
          : DecodeTexture(map.format, map.width, map.height, map.mips, data, size);
  if (rgba.empty()) {
    return nullptr;
  }
  if (!slot || slot->texture.width != map.width || slot->texture.height != map.height ||
      slot->texture.mip_levels != map.mips) {
    if (slot) {
      destroy(slot->view);
      destroy(slot->texture);
    } else {
      slot = std::make_unique<CachedTexture>();
    }
    slot->texture = create_texture(DkImageFormat_RGBA8_Unorm, map.width, map.height, 1, map.mips);
    slot->view = make_view(slot->texture);
  }
  // each level after the last, each half the one before
  size_t offset = 0;
  uint32_t w = map.width, h = map.height;
  for (uint32_t mip = 0; mip < map.mips; ++mip) {
    const uint32_t bytes = w * h * 4;
    write_texture(slot->texture, mip, 0, 0, 0, w, h, rgba.data() + offset, bytes, 0);
    offset += bytes;
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }
  slot->contents = contents;
  return &slot->view;
}

void bind_textures(const State& state, const TevConfig& tev, const CommandProcessor::Memory& memory) {
  bool bound[8] = {};
  DkCmdBuf cmd = commands();
  bind_descriptor_sets();
  for (unsigned i = 0; i < tev.stageCount; ++i) {
    const TevStage& stage = tev.stages[i];
    if (stage.texMap >= 8 || stage.texCoord >= tev.texCoordCount || bound[stage.texMap]) {
      continue;
    }
    bound[stage.texMap] = true;
    const TexMap map = state.Texture(stage.texMap);
    TextureView* view = texture_for(state, map, memory);
    if (!view) {
      continue;  // (the shader samples whatever was bound there before)
    }
    const int32_t image = image_slot(*view);
    const int32_t sampler = sampler_slot(*sampler_for(map));
    if (image >= 0 && sampler >= 0) {
      dkCmdBufBindTexture(cmd, DkStage_Fragment, stage.texMap,
                          dkMakeTextureHandle(static_cast<uint32_t>(image), static_cast<uint32_t>(sampler)));
    }
  }
}

} // namespace

bool draw_gx(const State& state, const Draw& draw, uint32_t width, uint32_t height,
             const CommandProcessor::Memory& memory) {
  DkPrimitive primitive;
  if (draw.count == 0 || !primitive_of(draw.primitive, &primitive)) {
    return false;
  }
  const TevConfig tev = state.Tev();
  const VertexConfig vertexConfig = state.Vertex();
  const ShaderPair& pair = shaders_for(vertexConfig, tev);
  if (!pair.vertex || !pair.fragment) {
    return false;
  }
  const DkShader* vertex = pair.vertex;
  const DkShader* fragment = pair.fragment;

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
  bind_textures(state, tev, memory);
  bind_vertex_layout(cmd);
  dkCmdBufBindVtxBuffer(cmd, 0, vertices.gpu(), vertexBytes);
  const DkViewport viewport = viewport_of(state, width, height);
  dkCmdBufSetViewports(cmd, 0, &viewport, 1);
  dkCmdBufDraw(cmd, primitive, draw.count, 1, 0, 0);
  return true;
}

} // namespace dol::dk
