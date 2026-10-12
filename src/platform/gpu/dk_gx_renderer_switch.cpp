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

// GX's scissor (BP 0x20 top-left, 0x21 bottom-right, 0x59 the offset) onto the
// target, as Dolphin's ComputeScissorRects reads it: the corners and the offset
// both carry GX's +342, which cancels in the subtraction; the offset's halves
// are in units of two pixels; the range is closed. (The wrap-around cases a
// large offset makes are not drawn twice here.)
DkScissor scissor_of(const State& state, uint32_t width, uint32_t height) {
  const uint32_t tl = state.Bp(0x20), br = state.Bp(0x21), off = state.Bp(0x59);
  if (tl == 0 && br == 0) {
    // never loaded (libdol's GXInit writes no registers): GXInit's own
    // scissor is the whole EFB
    return DkScissor{0, 0, width, height};
  }
  const int left = static_cast<int>((tl >> 12) & 0x7FF) - static_cast<int>((off & 0x3FF) << 1);
  const int top = static_cast<int>(tl & 0x7FF) - static_cast<int>(((off >> 10) & 0x3FF) << 1);
  const int right = static_cast<int>((br >> 12) & 0x7FF) - static_cast<int>((off & 0x3FF) << 1) + 1;
  const int bottom = static_cast<int>(br & 0x7FF) - static_cast<int>(((off >> 10) & 0x3FF) << 1) + 1;
  const float sx = static_cast<float>(width) / 640.0f;
  const float sy = static_cast<float>(height) / 480.0f;
  const auto clampTo = [](float v, uint32_t limit) {
    return static_cast<uint32_t>(v < 0.0f ? 0.0f : v > static_cast<float>(limit) ? static_cast<float>(limit) : v);
  };
  const uint32_t x0 = clampTo(left * sx, width), y0 = clampTo(top * sy, height);
  const uint32_t x1 = clampTo(right * sx, width), y1 = clampTo(bottom * sy, height);
  return DkScissor{x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
}

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

// XF's viewport registers, in GX's 640x480 EFB space, onto the target
DkViewport viewport_of(const State& state, uint32_t width, uint32_t height) {
  const auto& vp = state.Viewport();
  // (each axis its own scale: the EFB's 640x480 stretched to the target, as
  // the Wii's video output stretches it - 16:9 games draw anamorphic)
  const float scaleX = static_cast<float>(width) / 640.0f;
  const float scaleY = static_cast<float>(height) / 480.0f;
  const float w = vp[0] * 2.0f, h = -vp[1] * 2.0f;
  constexpr float z24 = 16777216.0f;
  DkViewport out;
  out.x = (vp[3] - 342.0f - w / 2.0f) * scaleX;
  out.y = (vp[4] - 342.0f - h / 2.0f) * scaleY;
  out.width = w * scaleX;
  out.height = h * scaleY;
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
  uint32_t lastUsed = 0;  // the frame it was last bound in (evict_unused)
};

// Frames presented, for the caches' ages (gx_frame_done).
uint32_t g_frame = 0;

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

// EFB copies by the guest address they were copied to (efb_copy). What the
// guest's RAM held there at the copy is hashed: the copy stands for that
// address until the RAM changes, which means the game put a texture of its own
// there instead.
struct EfbCopy {
  Texture texture;
  TextureView view;
  uint32_t width = 0, height = 0;
  uint64_t ram = 0;
  uint32_t lastUsed = 0;
};
std::unordered_map<uint32_t, std::unique_ptr<EfbCopy>> g_efbCopies;

uint64_t ram_hash(const CommandProcessor::Memory& memory, uint32_t address, uint32_t width, uint32_t height) {
  const uint32_t bytes = width * height * 2;
  const uint8_t* data = memory ? memory(address, bytes) : nullptr;
  return data ? sample_hash(data, bytes) : 0;
}

// a map's image, decoded and on the GPU; null when it cannot be read
TextureView* texture_for(const State& state, const TexMap& map, const CommandProcessor::Memory& memory) {
  if (!memory || map.address == 0) {
    return nullptr;
  }
  if (const auto copy = g_efbCopies.find(map.address); copy != g_efbCopies.end()) {
    EfbCopy& c = *copy->second;
    if (c.width == map.width && c.height == map.height &&
        c.ram == ram_hash(memory, map.address, c.width, c.height)) {
      c.lastUsed = g_frame;
      return &c.view;
    }
  }
  if (!IsKnown(map.format)) {
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
    slot->lastUsed = g_frame;
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
    if (!slot->texture.block) {
      // out of GPU memory: nothing to write into (writing anyway faulted the
      // GPU); the draw samples the placeholder
      g_textures.erase(key);
      return nullptr;
    }
    slot->view = make_view(slot->texture);
  }
  slot->lastUsed = g_frame;
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

// one white texel, made the first time a map cannot be read
TextureView* placeholder() {
  static Texture texture;
  static TextureView view;
  static bool made = false;
  if (!made) {
    made = true;
    texture = create_texture(DkImageFormat_RGBA8_Unorm, 1, 1);
    if (!texture.block) {
      return nullptr;
    }
    const uint8_t white[4] = {255, 255, 255, 255};
    write_texture(texture, 0, 0, 0, 0, 1, 1, white, sizeof(white), 0);
    view = make_view(texture);
  }
  return texture.block ? &view : nullptr;
}

void bind_textures(const State& state, const TevConfig& tev, const CommandProcessor::Memory& memory) {
  bool bound[8] = {};
  DkCmdBuf cmd = commands();
  bind_descriptor_sets();
  // the TEV stages' maps, then the indirect stages' (tev.hpp binds map m at m)
  uint8_t maps[16 + 4];  // a map per TEV stage at most, and the four indirect stages'
  unsigned mapCount = 0;
  for (unsigned i = 0; i < tev.stageCount; ++i) {
    const TevStage& stage = tev.stages[i];
    if (stage.texMap < 8 && stage.texCoord < tev.texCoordCount) {
      maps[mapCount++] = stage.texMap;
    }
  }
  for (unsigned i = 0; i < tev.indStageCount && i < 4; ++i) {
    if (tev.indStages[i].texCoord < tev.texCoordCount) {
      maps[mapCount++] = tev.indStages[i].texMap & 7;
    }
  }
  for (unsigned k = 0; k < mapCount; ++k) {
    const uint8_t texMap = maps[k];
    if (bound[texMap]) {
      continue;
    }
    bound[texMap] = true;
    const TexMap map = state.Texture(texMap);
    TextureView* view = texture_for(state, map, memory);
    if (!view) {
      // Not readable (an unknown format, an address outside memory, a palette
      // not loaded): a white texel rather than whatever the binding last held
      // - before anything was bound there, an uninitialised descriptor, which
      // the GPU faults on.
      view = placeholder();
      if (!view) {
        continue;
      }
    }
    const int32_t image = image_slot(*view);
    const int32_t sampler = sampler_slot(*sampler_for(map));
    if (image >= 0 && sampler >= 0) {
      dkCmdBufBindTexture(cmd, DkStage_Fragment, texMap,
                          dkMakeTextureHandle(static_cast<uint32_t>(image), static_cast<uint32_t>(sampler)));
    }
  }
}

} // namespace

// Textures and copies not used for kEvictAge frames are freed, a second's
// worth at a time. Kept forever, every texture a game ever bound stayed on the
// GPU; the memory ran out, an allocation failed, and the upload into it
// faulted the GPU minutes into a run.
constexpr uint32_t kEvictAge = 300;

void gx_frame_done() {
  ++g_frame;
  if (g_frame % 60 != 0) {
    return;
  }
  for (auto it = g_textures.begin(); it != g_textures.end();) {
    if (g_frame - it->second->lastUsed > kEvictAge) {
      destroy(it->second->view);
      destroy(it->second->texture);  // freed once the frames that used it are done
      it = g_textures.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = g_efbCopies.begin(); it != g_efbCopies.end();) {
    if (g_frame - it->second->lastUsed > kEvictAge) {
      destroy(it->second->view);
      destroy(it->second->texture);
      it = g_efbCopies.erase(it);
    } else {
      ++it;
    }
  }
}

void efb_copy(uint32_t address, uint32_t width, uint32_t height, const Texture& efb, const DkImageRect& source,
              const CommandProcessor::Memory& memory) {
  if (address == 0 || width == 0 || height == 0 || source.width == 0 || source.height == 0) {
    return;
  }
  auto& slot = g_efbCopies[address];
  if (!slot || slot->width != width || slot->height != height) {
    if (slot) {
      destroy(slot->view);
      destroy(slot->texture);
    } else {
      slot = std::make_unique<EfbCopy>();
    }
    slot->texture = create_texture(DkImageFormat_RGBA8_Unorm, width, height, 1, 1, 1, TextureRender);
    if (!slot->texture.block) {
      g_efbCopies.erase(address);
      return;
    }
    slot->view = make_view(slot->texture);
    slot->width = width;
    slot->height = height;
  }
  slot->lastUsed = g_frame;
  // Scaled with filtering: the EFB is drawn at the screen's size, the copy is
  // the size GX asked for (half of it when the copy box-filters to a mip).
  copy_texture(efb, 0, source, slot->texture, 0, DkImageRect{0, 0, 0, width, height, 1}, /*filter=*/true);
  slot->ram = ram_hash(memory, address, width, height);
}

void shutdown_gx() {
  for (auto& [key, cached] : g_textures) {
    destroy(cached->view);
    destroy(cached->texture);
  }
  g_textures.clear();
  for (auto& [key, copy] : g_efbCopies) {
    destroy(copy->view);
    destroy(copy->texture);
  }
  g_efbCopies.clear();
  for (auto& [key, sampler] : g_samplers) {
    destroy(*sampler);
  }
  g_samplers.clear();
  g_shaderPairs.clear();
}

// A uniform block staged once and used again while its bytes stay the same.
// Every draw uploaded both - 3.4 KB, three thousand times a frame in a 3D menu,
// most of it the same matrices - until the frame's staging ran out and draws
// were refused (a black screen); now only a change is uploaded. The address is
// in this frame's staging, so it is forgotten when the frame ends.
struct StagedBlock {
  std::vector<uint8_t> bytes;
  DkGpuAddr gpu = 0;
  uint32_t frame = ~0u;

  DkGpuAddr address(const std::vector<uint8_t>& block) {
    if (gpu && frame == g_frame && bytes == block) {
      return gpu;
    }
    gpu = stage_bytes(block.data(), static_cast<uint32_t>(block.size()), DK_UNIFORM_BUF_ALIGNMENT);
    bytes = block;
    frame = g_frame;
    return gpu;
  }
};
StagedBlock g_tevUniforms, g_xfUniforms;

// Wait for the GPU after every draw and stop at the first that faults it, with
// what it was. For finding a fault; far too slow to leave on.
constexpr bool kCheckEveryDraw = false;
// Submit after every draw (no wait): see draw_gx.
constexpr bool kSubmitEveryDraw = true;

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
  const DkGpuAddr tevAddr = g_tevUniforms.address(tevBlock);
  const DkGpuAddr xfAddr = g_xfUniforms.address(xfBlock);
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
  // Captured, then replayed: dkCmdBufBindVtxBuffers reserves five words and
  // writes six (two commands of a header and a 40-bit address; deko3d's
  // source as of 2026). At the very end of a piece of command memory the sixth
  // went past it, deko3d's write pointer with it, and what followed overwrote
  // the next piece - "GPU method error [b197:7c0] = 0x20200458", our vertex
  // layout's header read as the buffer's limit, at the same draw every run.
  // Captured into our own words, the call can write what it likes; the replay
  // reserves exactly the words captured.
  // (on capture(): beginning a capture clears the buffer it is given)
  uint32_t vtxWords[16];
  DkCmdBuf cap = capture();
  dkCmdBufBeginCaptureCmds(cap, vtxWords, 16);
  dkCmdBufBindVtxBuffer(cap, 0, vertices.gpu(), vertexBytes);
  const uint32_t vtxCount = dkCmdBufEndCaptureCmds(cap);
  dkCmdBufReplayCmds(cmd, vtxWords, vtxCount);
  const DkViewport viewport = viewport_of(state, width, height);
  dkCmdBufSetViewports(cmd, 0, &viewport, 1);
  const DkScissor scissor = scissor_of(state, width, height);
  dkCmdBufSetScissors(cmd, 0, &scissor, 1);
  dkCmdBufDraw(cmd, primitive, draw.count, 1, 0, 0);
  if (kSubmitEveryDraw) {
    // Each draw its own command list. Many draws in one list went wrong on the
    // GPU (method errors on data-like words), each draw on its own did not;
    // this says whether the list's size is what matters, without the waits.
    submit_list();
  }
  if (kCheckEveryDraw) {
    // Diagnosis: the GPU runs each draw before the next is recorded, and the
    // first one that puts the queue in its error state is described and the
    // run stops - a fault otherwise shows only at the frame's present.
    static uint32_t drawIndex = 0;
    ++drawIndex;
    submit();
    wait_idle();
    if (dkQueueIsInErrorState(queue())) {
      log("gx-native: draw %u faulted the GPU: primitive 0x%02x, %u vertices, vertex buffer 0x%llx + %u, "
          "viewport %.1f,%.1f %.1fx%.1f depth %.3f..%.3f, scissor %u,%u %ux%u, target %ux%u, %u TEV stages, "
          "%u indirect, %u tex coords",
          drawIndex, draw.primitive, draw.count, static_cast<unsigned long long>(vertices.gpu()), vertexBytes,
          viewport.x, viewport.y, viewport.width, viewport.height, viewport.near, viewport.far, scissor.x, scissor.y,
          scissor.width, scissor.height, width, height, tev.stageCount, tev.indStageCount, tev.texCoordCount);
      for (unsigned i = 0; i < tev.stageCount; ++i) {
        const TevStage& st = tev.stages[i];
        if (st.texMap < 8) {
          const TexMap m = state.Texture(st.texMap);
          log("gx-native:   stage %u: map %u coord %u: 0x%08x %ux%u format %u mips %u ind 0x%06x", i, st.texMap,
              st.texCoord, m.address, m.width, m.height, static_cast<unsigned>(m.format), m.mips, st.ind);
        }
      }
      fatal("gx-native: a draw faulted the GPU (see the lines above)");
    }
  }
  return true;
}

} // namespace dol::dk
