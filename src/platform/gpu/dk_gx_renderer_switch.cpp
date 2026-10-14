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

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dol::dk {
// (the probe, further down: texture lookups and copies report to it)
bool probing();
void probe_note(const char* what, uint32_t address, uint32_t w, uint32_t h, uint32_t a, uint32_t b);

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
    return DkScissor{0, 0, width, height * 528 / 480};
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
  // (down to the EFB's 528th line: the strip below the screen is drawn into too)
  const uint32_t rows = height * 528 / 480;
  const uint32_t x0 = clampTo(left * sx, width), y0 = clampTo(top * sy, rows);
  const uint32_t x1 = clampTo(right * sx, width), y1 = clampTo(bottom * sy, rows);
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
  // Screen positions come out less the scissor box offset (BP 0x59, in pairs
  // of pixels, the hardware's 342 included): GXSetScissorBoxOffset moves what
  // is drawn, not only the scissor - a menu draws its 3D models into the
  // region it then copies to a texture that way. Taken as the default 342, the
  // models landed elsewhere and the copies came out empty (a shadow, no model).
  const uint32_t off = state.Bp(0x59);
  const float offX = off ? static_cast<float>((off & 0x3FF) << 1) : 342.0f;
  const float offY = off ? static_cast<float>(((off >> 10) & 0x3FF) << 1) : 342.0f;
  DkViewport out;
  out.x = (vp[3] - offX - w / 2.0f) * scaleX;
  out.y = (vp[4] - offY - h / 2.0f) * scaleY;
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
  // the frame its RAM was last hashed in: once a frame is enough, and hashing
  // on every bind (thousands a frame in a menu) was most of a draw's cost
  uint32_t checkedFrame = ~0u;
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
  uint32_t format = 0;  // the copy's (GXSetTexCopyDst)
  uint64_t ram = 0;
  uint32_t lastUsed = 0;
  uint32_t checkedFrame = ~0u;
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
        (c.checkedFrame == g_frame || c.ram == ram_hash(memory, map.address, c.width, c.height))) {
      if (probing()) {
        probe_note("copy used", map.address, map.width, map.height, static_cast<uint32_t>(map.format), c.format);
      }
      c.lastUsed = c.checkedFrame = g_frame;
      return &c.view;
    }
    if (probing()) {
      probe_note("copy MISSED", map.address, map.width, map.height, static_cast<uint32_t>(map.format),
                 c.width << 16 | c.height);
    }
    // (diagnosis, cheap: only a copy turned down) why a copy was not used - a
    // model drawn into a texture that blinks is its copy turned down for a frame
    static uint32_t missed = 0, lastAddress = 0, lastFrame = ~0u;
    if (missed < 40 && (map.address != lastAddress || g_frame != lastFrame)) {
      ++missed;
      lastAddress = map.address;
      lastFrame = g_frame;
      log("gx-copymiss %u: frame %u: 0x%08x sampled %ux%u f%u, the copy is %ux%u f%u%s, last used frame %u", missed,
          g_frame, map.address, map.width, map.height, static_cast<uint32_t>(map.format), c.width, c.height, c.format,
          c.width == map.width && c.height == map.height ? " (its RAM changed)" : " (size differs)", c.lastUsed);
    }
  }
  if (!IsKnown(map.format)) {
    if (probing()) {
      probe_note("unknown format", map.address, map.width, map.height, static_cast<uint32_t>(map.format), 0);
    }
    return nullptr;
  }
  const size_t size = TextureDataSize(map.format, map.width, map.height, map.mips);
  const uint8_t* data = size ? memory(map.address, static_cast<uint32_t>(size)) : nullptr;
  if (!data) {
    return nullptr;
  }
  uint64_t key = mix(mix(mix(0xcbf29ce484222325ull, map.address), map.width | map.height << 16),
                     static_cast<uint32_t>(map.format) | map.mips << 8);
  const uint8_t* tlut = nullptr;
  TlutLoad load;
  if (IsPaletted(map.format)) {
    load = state.Tlut(map.tlutTmem);
    tlut = load.address && load.entries ? memory(load.address, load.entries * 2) : nullptr;
    if (!tlut) {
      return nullptr;
    }
    key = mix(mix(key, load.address), static_cast<uint32_t>(map.tlutFormat) | load.entries << 8);
  }

  auto& slot = g_textures[key];
  if (slot && slot->checkedFrame == g_frame) {
    slot->lastUsed = g_frame;
    return &slot->view;
  }
  uint64_t contents = sample_hash(data, size);
  if (tlut) {
    contents = mix(contents, sample_hash(tlut, load.entries * 2));
  }
  if (slot && slot->contents == contents) {
    slot->lastUsed = slot->checkedFrame = g_frame;
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
  slot->checkedFrame = g_frame;
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

// what bind_textures last bound at each map (the dirty audit compares)
TexMap g_boundMaps[8];

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
    g_boundMaps[texMap] = map;
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

void probe_finish();  // (the probe, below)

void gx_frame_done() {
  probe_finish();
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

// ---------- EFB copies, as the hardware makes them
//
// A copy is not a blit. The hardware box-filters every EFB pixel under each
// texel it writes (halving with the mipmap flag), runs the copy filter's
// vertical taps over the lines, and stores the format the game asked for:
// intensity as luma for the I formats - what a bloom's threshold copy reads -
// single channels for R8, A8 and the rest, the depth buffer for the Z formats
// (a lens flare's visibility probe). Each copy is that, drawn by a small pass
// into the copy's texture, whose texels are then what sampling that format
// returns. The EFB here is scaled up, so the box covers the scaled pixels and
// the taps step one Wii line at a time.
namespace {
constexpr char kCopyVertex[] = R"(#version 460
void main() {
    // one triangle over the target
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr char kCopyFragment[] = R"(#version 460
layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2D efb;
layout (binding = 1) uniform sampler2D depthBuffer;
layout (std140, binding = 0) uniform Copy {
    ivec4 rect;   // the source, in EFB pixels: x, y, width, height
    ivec4 info;   // destination width, height; format; opaque (the EFB keeps no alpha)
    vec4 taps;    // the vertical filter's weights above, at and below the line; the line step in pixels
};

vec4 fetch(ivec2 at) {
    at = clamp(at, rect.xy, rect.xy + rect.zw - 1);
    vec4 c = texelFetch(efb, at, 0);
    if (info.w != 0) { c.a = 1.0; }
    return c;
}

float fetchDepth(ivec2 at) {
    return texelFetch(depthBuffer, clamp(at, rect.xy, rect.xy + rect.zw - 1), 0).r;
}

float q(float v, float levels) { return floor(clamp(v, 0.0, 1.0) * levels + 0.5) / levels; }
float luma(vec3 c) { return clamp(dot(c, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0, 0.0, 1.0); }

void main() {
    ivec2 o = ivec2(gl_FragCoord.xy);
    vec2 scale = vec2(rect.zw) / vec2(info.xy);
    ivec2 s0 = rect.xy + ivec2(floor(vec2(o) * scale));
    ivec2 s1 = max(rect.xy + ivec2(floor(vec2(o + 1) * scale)), s0 + 1);
    int step = int(taps.w);
    bool depth = info.z >= 16 && info.z < 32 || info.z >= 48;
    vec4 sum = vec4(0.0);
    float z = 0.0;
    int n = 0;
    for (int y = s0.y; y < s1.y && y < s0.y + 16; ++y) {
        for (int x = s0.x; x < s1.x && x < s0.x + 16; ++x) {
            if (depth) {
                z += fetchDepth(ivec2(x, y));
            } else {
                sum += taps.x * fetch(ivec2(x, y - step)) + taps.y * fetch(ivec2(x, y)) + taps.z * fetch(ivec2(x, y + step));
            }
            ++n;
        }
    }
    vec4 c = clamp(sum / float(n), 0.0, 1.0);
    uint z24 = uint(clamp(z / float(n), 0.0, 1.0) * 16777215.0);
    float zh = float((z24 >> 16) & 255u) / 255.0, zm = float((z24 >> 8) & 255u) / 255.0, zl = float(z24 & 255u) / 255.0;
    float y8 = luma(c.rgb);
    switch (info.z) {
    case 0: { float i = q(y8, 15.0); outColor = vec4(i); break; }                         // I4
    case 1: outColor = vec4(y8); break;                                                    // I8
    case 2: { float i = q(y8, 15.0); outColor = vec4(i, i, i, q(c.a, 15.0)); break; }     // IA4
    case 3: outColor = vec4(y8, y8, y8, c.a); break;                                       // IA8
    case 4: outColor = vec4(q(c.r, 31.0), q(c.g, 63.0), q(c.b, 31.0), 1.0); break;         // RGB565
    case 5:                                                                                // RGB5A3
        outColor = c.a >= 224.0 / 255.0 ? vec4(q(c.r, 31.0), q(c.g, 31.0), q(c.b, 31.0), 1.0)
                                         : vec4(q(c.r, 15.0), q(c.g, 15.0), q(c.b, 15.0), q(c.a, 7.0));
        break;
    case 0x20: { float r = q(c.r, 15.0); outColor = vec4(r); break; }                     // R4
    case 0x22: outColor = vec4(q(c.r, 15.0), q(c.r, 15.0), q(c.r, 15.0), q(c.a, 15.0)); break;  // RA4
    case 0x23: outColor = vec4(c.r, c.r, c.r, c.a); break;                                 // RA8
    case 0x27: outColor = vec4(c.a); break;                                                // A8
    case 0x28: outColor = vec4(c.r); break;                                                // R8
    case 0x29: outColor = vec4(c.g); break;                                                // G8
    case 0x2A: outColor = vec4(c.b); break;                                                // B8
    case 0x2B: outColor = vec4(c.g, c.g, c.g, c.r); break;                                 // RG8 (as IA8: G, then R)
    case 0x2C: outColor = vec4(c.b, c.b, c.b, c.g); break;                                 // GB8
    case 0x11: case 0x39: outColor = vec4(info.z == 0x11 ? zh : zm); break;                // Z8, Z8M
    case 0x3A: outColor = vec4(zl); break;                                                 // Z8L
    case 0x13: outColor = vec4(zm, zm, zm, zh); break;                                     // Z16
    case 0x3C: outColor = vec4(zl, zl, zl, zm); break;                                     // Z16L
    case 0x16: outColor = vec4(zh, zm, zl, 1.0); break;                                    // Z24X8
    default: outColor = c; break;                                                          // RGBA8
    }
}
)";

struct CopyUniforms {
    int32_t rect[4];
    int32_t info[4];
    float taps[4];
};
}  // namespace

void efb_copy(uint32_t address, uint32_t width, uint32_t height, const Texture& efb, TextureView& efbView,
              TextureView& depthView, const DkImageRect& source, const CommandProcessor::Memory& memory,
              uint32_t format, bool opaque, const float taps[3], uint32_t lineStep) {
  if (probing()) {
    probe_note("copy", address, width, height, format, source.y << 16 | source.height);
  }
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
  slot->format = format;

  static const DkShader* const vertex = shader(ShaderStage::Vertex, kCopyVertex);
  static const DkShader* const fragment = shader(ShaderStage::Fragment, kCopyFragment);
  static Sampler nearest = [] {
    Sampler s;
    dkSamplerDefaults(&s.sampler);
    s.sampler.minFilter = s.sampler.magFilter = DkFilter_Nearest;
    s.sampler.wrapMode[0] = s.sampler.wrapMode[1] = DkWrapMode_ClampToEdge;
    return s;
  }();
  const int32_t samplerSlot = sampler_slot(nearest);
  const int32_t efbSlot = image_slot(efbView);
  const int32_t depthSlot = image_slot(depthView);
  CopyUniforms u{};
  u.rect[0] = static_cast<int32_t>(source.x);
  u.rect[1] = static_cast<int32_t>(source.y);
  u.rect[2] = static_cast<int32_t>(source.width);
  u.rect[3] = static_cast<int32_t>(source.height);
  u.info[0] = static_cast<int32_t>(width);
  u.info[1] = static_cast<int32_t>(height);
  u.info[2] = static_cast<int32_t>(format);
  u.info[3] = opaque ? 1 : 0;
  u.taps[0] = taps[0];
  u.taps[1] = taps[1];
  u.taps[2] = taps[2];
  u.taps[3] = static_cast<float>(lineStep);
  const DkGpuAddr uniforms = stage_bytes(&u, sizeof(u), DK_UNIFORM_BUF_ALIGNMENT);
  if (!vertex || !fragment || samplerSlot < 0 || efbSlot < 0 || depthSlot < 0 || !uniforms) {
    // (no shader compiler, or out of descriptors: the plain scaled blit)
    copy_texture(efb, 0, source, slot->texture, 0, DkImageRect{0, 0, 0, width, height, 1}, /*filter=*/true);
  } else {
    DkCmdBuf cmd = commands();
    bind_descriptor_sets();
    const DkImageView* target = &slot->view.view;
    dkCmdBufBindRenderTargets(cmd, &target, 1, nullptr);
    const DkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    dkCmdBufSetViewports(cmd, 0, &viewport, 1);
    const DkScissor all{0, 0, width, height};
    dkCmdBufSetScissors(cmd, 0, &all, 1);
    PixelState plain;  // no blending, no depth, every channel written, nothing culled
    plain.depthTest = false;
    plain.depthWrite = false;
    plain.cull = CullMode::None;
    plain.alphaWrite = true;
    plain.efbAlpha = true;
    bind(pipeline_state(plain));
    const DkShader* shaders[] = {vertex, fragment};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, uniforms, align_up(sizeof(u), DK_UNIFORM_BUF_ALIGNMENT));
    dkCmdBufBindTexture(cmd, DkStage_Fragment, 0,
                        dkMakeTextureHandle(static_cast<uint32_t>(efbSlot), static_cast<uint32_t>(samplerSlot)));
    dkCmdBufBindTexture(cmd, DkStage_Fragment, 1,
                        dkMakeTextureHandle(static_cast<uint32_t>(depthSlot), static_cast<uint32_t>(samplerSlot)));
    dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    gx_rebind_all();  // (the GX draws after it bind everything again)
  }
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

// What the last draw bound, kept while the state behind it does not change.
struct DrawCache {
  const ShaderPair* pair = nullptr;
  TevConfig tev;
  bool rebindAll = true;
} g_drawCache;

void gx_rebind_all() { g_drawCache.rebindAll = true; }

// A uniform block staged once a frame, then changed in place by pushing only
// the bytes that differ (dkCmdBufPushConstants: written from the command
// stream, in order with the draws). Staging the whole block for every draw
// that loaded a matrix - 3 KB, ten thousand times a frame in a race intro -
// ran the frame's staging out, and the draws after it were refused: missing
// models, a black flyover.
struct PushedBlock {
  std::vector<uint8_t> bytes;
  DkGpuAddr gpu = 0;
  uint32_t frame = ~0u;

  // The buffer's address, staged anew when `fresh`; `bind` says whether the
  // caller must bind it (it is a new buffer).
  DkGpuAddr update(DkCmdBuf cmd, const std::vector<uint8_t>& block, bool& bind) {
    const uint32_t size = align_up(static_cast<uint32_t>(block.size()), DK_UNIFORM_BUF_ALIGNMENT);
    if (!gpu || frame != g_frame || bytes.size() != block.size()) {
      Allocation a = stage(size, DK_UNIFORM_BUF_ALIGNMENT);
      if (!a) {
        return 0;
      }
      std::memcpy(a.cpu(), block.data(), block.size());
      gpu = a.gpu();
      bytes = block;
      frame = g_frame;
      bind = true;
      return gpu;
    }
    bind = false;
    size_t first = 0;
    while (first < block.size() && block[first] == bytes[first]) {
      ++first;
    }
    if (first == block.size()) {
      return gpu;  // unchanged
    }
    size_t last = block.size();
    while (last > first && block[last - 1] == bytes[last - 1]) {
      --last;
    }
    first &= ~size_t(3);
    last = (last + 3) & ~size_t(3);
    if (last > block.size()) {
      last = block.size();
    }
    dkCmdBufPushConstants(cmd, gpu, size, static_cast<uint32_t>(first), static_cast<uint32_t>(last - first),
                          block.data() + first);
    std::memcpy(bytes.data() + first, block.data() + first, last - first);
    return gpu;
  }
};
PushedBlock g_tevUniforms, g_xfUniforms;

// Wait for the GPU after every draw and stop at the first that faults it, with
// what it was. For finding a fault; far too slow to leave on.
constexpr bool kCheckEveryDraw = false;
// Draws a command list holds before it is submitted (see draw_staged): one per
// draw stopped a GPU fault that whole frames in one list met; this tries 32.
constexpr uint32_t kDrawsPerList = 32;

// ---------- diagnosis, one frame in 120
// Models went missing with the build that brought dirty tracking and lists of
// 32 draws. In a probe frame every draw is (1) audited: what a full rebind
// would bind against what the dirty flags left bound, counted by kind - and
// (2) measured: the GPU's counters after it, so the frame's draws say where
// their primitives go (clipped away, culled, failing the depth test, or
// reaching the target), grouped by z test, cull mode and strips.
constexpr uint32_t kProbeEvery = 120;
constexpr uint32_t kProbeMax = 12000;
constexpr DkCounter kProbeCounters[4] = {DkCounter_ClipperInputPrimitives, DkCounter_ClipperOutputPrimitives,
                                         DkCounter_FragmentShaderInvocations, DkCounter_SamplesPassed};
Allocation g_probeMem;
uint32_t g_probeFrame = ~0u;
std::vector<uint8_t> g_probeGroups;
// a probe frame's depth-test-off draws, described as drawn: the big ones (by
// the GPU's sample count) are logged at the frame's end - a fade or bloom
// drawn wrongly over the whole scene leaves a black screen with the HUD on top
std::vector<std::pair<uint32_t, std::string>> g_probeOverlays;
// and its skinned 3D draws (the character models), described the same way:
// the biggest are logged - a model drawn black, its lighting or material seen
std::vector<std::pair<uint32_t, std::string>> g_probeModels;
uint32_t g_stale[5];  // shaders, TEV uniforms, XF uniforms, pixel state, textures
PixelState g_boundPixel;
bool g_pixelBound = false;

// (diagnosis) frames that draw into an EFB keeping alpha - a Mii's face and
// icon are made there - probed too, at most one in 60 frames, 30 in a run, and
// each such draw logged with its counters (gx-mii, at the frame's end)
// (off: each probed frame waits for the GPU, and in a menu that draws models for
// textures it probed one frame a second - a hiccup a second)
constexpr bool kProbeFocus = false;
uint32_t g_focusFrame = ~0u, g_focusCount = 0, g_focusLast = 0;
std::vector<std::pair<uint32_t, std::string>> g_probeMii;

// (the regular probe frames are off unless hunting: each waits for the GPU and
// reads its counters after every draw)
constexpr bool kProbePeriodic = false;
bool probing() { return (kProbePeriodic && g_frame % kProbeEvery == kProbeEvery / 2) || g_frame == g_focusFrame; }

// the rest of this frame probed: a render to a texture starting (gx_native's
// copies), 40 times a run
void probe_focus() {
  static uint32_t count = 0;
  if (kProbeFocus && g_focusFrame != g_frame && count < 40) {
    ++count;
    g_focusFrame = g_frame;
  }
}

// a probe frame's copies and the copies textures used or missed: each kind
// once per address, the first 24 a frame
void probe_note(const char* what, uint32_t address, uint32_t w, uint32_t h, uint32_t a, uint32_t b) {
  static uint32_t frame = ~0u;
  static std::vector<uint64_t> seen;
  if (frame != g_frame) {
    frame = g_frame;
    seen.clear();
  }
  const uint64_t key = static_cast<uint64_t>(address) << 8 | static_cast<uint8_t>(what[0] + std::strlen(what));
  if (seen.size() >= 24 || std::find(seen.begin(), seen.end(), key) != seen.end()) {
    return;
  }
  seen.push_back(key);
  log("gx-probe: frame %u: %s 0x%08x %ux%u (%u, 0x%x)", g_frame, what, address, w, h, a, b);
}

void probe_report(DkCmdBuf cmd, uint32_t slot) {
  for (uint32_t k = 0; k < 4; ++k) {
    dkCmdBufReportCounter(cmd, kProbeCounters[k], g_probeMem.gpu() + (slot * 4 + k) * 16);
  }
}

// A draw's first vertices taken through the matrices on the CPU, as the vertex
// shader would: where its positions land, and why (a probe frame's first few
// strips and lists)
void probe_vertices(const State& state, const char* what, uint8_t gxPrimitive, const float* vertices, uint32_t count) {
  static uint32_t logged = 0, frame = ~0u;
  if (frame != g_frame) {
    frame = g_frame;
    logged = 0;
  }
  if (++logged > 6) {
    return;
  }
  const std::vector<uint8_t> xf = state.XfUniforms();
  const auto row4 = [&](size_t offset) {
    const float* f = reinterpret_cast<const float*>(xf.data() + offset);
    return f;
  };
  int32_t current = 0;
  std::memcpy(&current, xf.data() + 3168, 4);
  const bool indexed = state.Vertex().indexedPosMtx;
  if (logged == 1) {
    const auto& vp = state.Viewport();
    log("gx-probe: viewport regs %.1f %.1f %.1f / %.1f %.1f %.1f", vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
  }
  for (uint32_t i = 0; i < count && i < 2; ++i) {
    const float* v = vertices + i * kVertexFloats;
    const int row = indexed ? static_cast<int>(v[30]) : current;
    float mv[3] = {};
    if (row >= 0 && row + 2 < 30) {
      for (int r = 0; r < 3; ++r) {
        const float* m = row4(static_cast<size_t>(row + r) * 16);
        mv[r] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
      }
    }
    float clip[4];
    for (int r = 0; r < 4; ++r) {
      const float* p = row4(2400 + static_cast<size_t>(r) * 16);
      clip[r] = p[0] * mv[0] + p[1] * mv[1] + p[2] * mv[2] + p[3];
    }
    log("gx-probe: %s 0x%02x n=%u v%u: pos %.3f %.3f %.3f, row %d (%s), view %.3f %.3f %.3f, clip %.3f %.3f %.3f "
        "w %.3f",
        what, gxPrimitive, count, i, v[0], v[1], v[2], row, indexed ? "per vertex" : "current", mv[0], mv[1], mv[2],
        clip[0], clip[1], clip[2], clip[3]);
  }
}

bool same_pixel(const PixelState& a, const PixelState& b) {
  return a.blendMode == b.blendMode && a.srcFactor == b.srcFactor && a.dstFactor == b.dstFactor &&
         a.logicOp == b.logicOp && a.depthTest == b.depthTest && a.depthCompare == b.depthCompare &&
         a.depthWrite == b.depthWrite && a.cull == b.cull && a.colorWrite == b.colorWrite &&
         a.alphaWrite == b.alphaWrite && a.efbAlpha == b.efbAlpha;
}

bool same_map(const TexMap& a, const TexMap& b) {
  return a.address == b.address && a.width == b.width && a.height == b.height && a.format == b.format &&
         a.mips == b.mips && a.wrapS == b.wrapS && a.wrapT == b.wrapT && a.magLinear == b.magLinear &&
         a.minFilter == b.minFilter && a.lodBias == b.lodBias && a.minLod == b.minLod && a.maxLod == b.maxLod &&
         a.tlutFormat == b.tlutFormat && a.tlutTmem == b.tlutTmem;
}

// at the probe frame's end: the GPU is waited for, and its counts read
void probe_finish() {
  if (g_probeFrame != g_frame || g_probeGroups.empty()) {
    return;
  }
  submit();
  wait_idle();
  struct Sum {
    uint32_t draws = 0;
    uint64_t v[4] = {};
  } sums[32];
  const auto* r = static_cast<const uint64_t*>(g_probeMem.cpu());
  for (size_t i = 0; i < g_probeGroups.size(); ++i) {
    Sum& s = sums[g_probeGroups[i] & 31];
    ++s.draws;
    for (uint32_t k = 0; k < 4; ++k) {
      s.v[k] += r[((i + 1) * 4 + k) * 2] - r[(i * 4 + k) * 2];
    }
  }
  static const char* const kCull[] = {"none", "front", "back", "all"};
  for (uint32_t g = 0; g < 32; ++g) {
    if (sums[g].draws != 0) {
      log("gx-probe: frame %u: z %s, cull %s, %s%s: %u draws, primitives %llu in %llu out, %llu fragments, %llu "
          "samples passed",
          g_frame, (g & 1) ? "on" : "off", kCull[(g >> 1) & 3], (g & 8) ? "strips/fans" : "lists",
          (g & 16) ? ", matrix per vertex" : "", sums[g].draws,
          static_cast<unsigned long long>(sums[g].v[0]), static_cast<unsigned long long>(sums[g].v[1]),
          static_cast<unsigned long long>(sums[g].v[2]), static_cast<unsigned long long>(sums[g].v[3]));
    }
  }
  int shown = 0;
  for (const auto& [index, what] : g_probeOverlays) {
    if (index + 1 > g_probeGroups.size() || shown >= 32) {
      continue;
    }
    const uint64_t samples = r[((index + 1) * 4 + 3) * 2] - r[(index * 4 + 3) * 2];
    if (samples >= 1000) {
      log("gx-overlay: frame %u: draw %u, %llu samples: %s", g_frame, index, static_cast<unsigned long long>(samples),
          what.c_str());
      ++shown;
    }
  }
  g_probeOverlays.clear();
  {
    std::vector<std::pair<uint64_t, const std::string*>> biggest;
    for (const auto& [index, what] : g_probeModels) {
      if (index + 1 <= g_probeGroups.size()) {
        biggest.emplace_back(r[((index + 1) * 4 + 3) * 2] - r[(index * 4 + 3) * 2], &what);
      }
    }
    std::sort(biggest.begin(), biggest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < biggest.size() && i < 6; ++i) {
      log("gx-model: frame %u: %llu samples: %s", g_frame, static_cast<unsigned long long>(biggest[i].first),
          biggest[i].second->c_str());
    }
    g_probeModels.clear();
  }
  for (const auto& [index, what] : g_probeMii) {
    if (index + 1 <= g_probeGroups.size()) {
      uint64_t d[4];
      for (uint32_t k = 0; k < 4; ++k) {
        d[k] = r[((index + 1) * 4 + k) * 2] - r[(index * 4 + k) * 2];
      }
      log("gx-mii: frame %u draw %u: prims %llu in %llu out, %llu frags, %llu samples | %s", g_frame, index,
          static_cast<unsigned long long>(d[0]), static_cast<unsigned long long>(d[1]),
          static_cast<unsigned long long>(d[2]), static_cast<unsigned long long>(d[3]), what.c_str());
    }
  }
  g_probeMii.clear();
  log("gx-probe: frame %u: stale after dirty tracking: shaders %u, TEV uniforms %u, XF uniforms %u, pixel %u, "
      "textures %u (of %zu draws)",
      g_frame, g_stale[0], g_stale[1], g_stale[2], g_stale[3], g_stale[4], g_probeGroups.size());
  std::memset(g_stale, 0, sizeof(g_stale));
  g_probeGroups.clear();
}

namespace {
// The shaders, uniforms, state and bindings the state asks for, and the GPU
// draw of `count` vertices already converted into `vertices`.
bool draw_staged(const State& state, DkPrimitive primitive, Allocation vertices, uint32_t count, uint32_t width,
                 uint32_t height, const CommandProcessor::Memory& memory, const Draw& draw, Allocation indices = {},
                 uint32_t indexCount = 0) {
  const uint32_t vertexBytes = count * static_cast<uint32_t>(kVertexFloats * sizeof(float));
  // Only what changed since the last draw is redone (State::TakeDirty): a menu
  // makes thousands of draws a frame, most of them a new matrix and nothing
  // else, and rebuilding every draw's shaders' keys, uniforms, pixel state and
  // texture bindings was most of the frame. A frame's start and a copy redo all
  // of it (gx_rebind_all).
  DrawCache& c = g_drawCache;
  uint32_t dirty = state.TakeDirty();
  if (c.rebindAll) {
    dirty = State::kDirtyAll;
  }
  DkCmdBuf cmd = commands();
  if (kProbeFocus && state.Pixel().efbAlpha && g_focusFrame != g_frame && g_focusCount < 30 &&
      (g_focusCount == 0 || g_frame - g_focusLast >= 60) && !probing()) {
    g_focusFrame = g_focusLast = g_frame;
    ++g_focusCount;
  }
  const bool probe = probing();
  if (probe && g_probeFrame != g_frame) {
    g_probeMem = stage((kProbeMax + 1) * 4 * 16, 16);
    g_probeFrame = g_probeMem ? g_frame : ~0u;
    g_probeGroups.clear();
    if (g_probeMem) {
      probe_report(cmd, 0);
    }
  }
  if (probe && c.pair) {
    // what a full rebind would bind, against what is bound
    if (!(dirty & State::kDirtyShader) && &shaders_for(state.Vertex(), state.Tev()) != c.pair) {
      ++g_stale[0];
    }
    if (!(dirty & State::kDirtyTevUniforms) && state.TevUniforms() != g_tevUniforms.bytes) {
      ++g_stale[1];
    }
    if (!(dirty & State::kDirtyXfUniforms) && state.XfUniforms() != g_xfUniforms.bytes) {
      ++g_stale[2];
    }
    if (!(dirty & State::kDirtyPixel) && g_pixelBound && !same_pixel(state.Pixel(), g_boundPixel)) {
      ++g_stale[3];
    }
    if (!(dirty & (State::kDirtyShader | State::kDirtyTextures))) {
      const TevConfig tev = state.Tev();
      for (unsigned i = 0; i < tev.stageCount; ++i) {
        const uint8_t m = tev.stages[i].texMap;
        if (m < 8 && tev.stages[i].texCoord < tev.texCoordCount && !same_map(state.Texture(m), g_boundMaps[m])) {
          ++g_stale[4];
          break;
        }
      }
    }
  }
  if (dirty & State::kDirtyShader) {
    c.tev = state.Tev();
    const ShaderPair& pair = shaders_for(state.Vertex(), c.tev);
    if (!pair.vertex || !pair.fragment) {
      c.pair = nullptr;
      return false;
    }
    if (c.rebindAll || &pair != c.pair) {
      const DkShader* shaders[] = {pair.vertex, pair.fragment};
      dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
    }
    c.pair = &pair;
  } else if (!c.pair) {
    return false;  // (the shaders the state asks for did not compile)
  }
  if (dirty & State::kDirtyTevUniforms) {
    static std::vector<uint8_t> tevBlock;  // (reused: one allocation for the run)
    state.TevUniforms(tevBlock);
    bool bindIt = false;
    const DkGpuAddr tevAddr = g_tevUniforms.update(cmd, tevBlock, bindIt);
    if (!tevAddr) {
      return false;
    }
    if (bindIt || c.rebindAll) {
      dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, tevAddr,
                                align_up(static_cast<uint32_t>(tevBlock.size()), DK_UNIFORM_BUF_ALIGNMENT));
    }
  }
  if (dirty & State::kDirtyXfUniforms) {
    static std::vector<uint8_t> xfBlock;
    state.XfUniforms(xfBlock);
    bool bindIt = false;
    const DkGpuAddr xfAddr = g_xfUniforms.update(cmd, xfBlock, bindIt);
    if (!xfAddr) {
      return false;
    }
    if (bindIt || c.rebindAll) {
      dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 1, xfAddr,
                                align_up(static_cast<uint32_t>(xfBlock.size()), DK_UNIFORM_BUF_ALIGNMENT));
    }
  }
  if (dirty & State::kDirtyPixel) {
    g_boundPixel = state.Pixel();
    g_pixelBound = true;
    bind(pipeline_state(g_boundPixel));
  }
  if (dirty & (State::kDirtyShader | State::kDirtyTextures)) {
    bind_textures(state, c.tev, memory);
  }
  if (c.rebindAll) {
    bind_vertex_layout(cmd);
    c.rebindAll = false;
  }
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
  if (primitive == DkPrimitive_Lines || primitive == DkPrimitive_LineStrip || primitive == DkPrimitive_Points) {
    // GXSetLineWidth/GXSetPointSize (SU_LPSIZE, BP 0x22), in sixths of an EFB
    // pixel, scaled as the rest of the frame is. Left at the GPU's one pixel,
    // a picture built from lines came out striped: Mega Man 9 draws its
    // 256x240 frame and sprites from them, and at 720p a one-pixel line every
    // one and a half rows left a dark scanline between them.
    const uint32_t lpsize = state.Bp(0x22);
    const float scale = std::max(static_cast<float>(width) / 640.0f, static_cast<float>(height) / 480.0f);
    if (primitive == DkPrimitive_Points) {
      dkCmdBufSetPointSize(cmd, std::max(1.0f, static_cast<float>((lpsize >> 8) & 0xFF) / 6.0f * scale));
    } else {
      dkCmdBufSetLineWidth(cmd, std::max(1.0f, static_cast<float>(lpsize & 0xFF) / 6.0f * scale));
    }
  }
  if (indexCount != 0) {
    // (captured and replayed like the vertex buffer's bind: our own words)
    uint32_t idxWords[16];
    dkCmdBufBeginCaptureCmds(cap, idxWords, 16);
    dkCmdBufBindIdxBuffer(cap, DkIdxFormat_Uint32, indices.gpu());
    const uint32_t idxCount = dkCmdBufEndCaptureCmds(cap);
    dkCmdBufReplayCmds(cmd, idxWords, idxCount);
    dkCmdBufDrawIndexed(cmd, primitive, indexCount, 1, 0, 0, 0);
  } else {
    dkCmdBufDraw(cmd, primitive, count, 1, 0, 0);
  }
  if (probe && state.Pixel().efbAlpha) {
    // Draws into an EFB that keeps alpha (a menu's models, drawn to be copied
    // into a texture): what decides the alpha they leave, the first few a frame
    static uint32_t frame = ~0u, logged = 0;
    if (frame != g_frame) {
      frame = g_frame;
      logged = 0;
    }
    if (logged++ < 6) {
      const PixelState px = state.Pixel();
      const TevConfig tev = state.Tev();
      log("gx-alpha: frame %u: prim 0x%02x n=%u alphaWrite %d dstAlpha %d (%.2f) blend %d src %d dst %d, "
          "alpha test %d/%u %d %d/%u, %u stages",
          g_frame, draw.primitive, count, px.alphaWrite, tev.dstAlpha, state.TevUniforms().size() >= 144
              ? reinterpret_cast<const float*>(state.TevUniforms().data() + 128)[3] : -1.0f,
          static_cast<int>(px.blendMode), static_cast<int>(px.srcFactor), static_cast<int>(px.dstFactor),
          static_cast<int>(tev.alphaComp0), tev.alphaRef0, static_cast<int>(tev.alphaOp),
          static_cast<int>(tev.alphaComp1), tev.alphaRef1, tev.stageCount);
      for (unsigned i = 0; i < tev.stageCount && i < 4; ++i) {
        const TevStage& st = tev.stages[i];
        const TevCombiner& a = st.alpha;
        log("gx-alpha:   stage %u: alpha a%u b%u c%u d%u op %d bias %d scale %d out %d, ksel %u, map %u coord %u "
            "chan %d, swap ras %u tex %u",
            i, a.a, a.b, a.c, a.d, static_cast<int>(a.op), static_cast<int>(a.bias), static_cast<int>(a.scale),
            static_cast<int>(a.out), st.kalphaSel, st.texMap, st.texCoord, static_cast<int>(st.channel), st.rasSwap,
            st.texSwap);
      }
    }
  }
  if (probe && g_probeFrame == g_frame && g_probeGroups.size() < kProbeMax) {
    const PixelState px = state.Pixel();
    const uint8_t prim = draw.primitive & 0xF8;
    const bool strip = prim == 0x98 || prim == 0xA0 || prim == 0xB0;
    if (px.depthTest && state.Vertex().indexedPosMtx && g_probeModels.size() < 600) {
      const VertexConfig vc = state.Vertex();
      const TevConfig tev = state.Tev();
      const std::vector<uint8_t> xu = state.XfUniforms();
      const float* amb = reinterpret_cast<const float*>(xu.data() + 2464);
      const float* mat = reinterpret_cast<const float*>(xu.data() + 2496);
      const float* l0 = reinterpret_cast<const float*>(xu.data() + 2528 + 32);  // light 0's colour
      const float* l1 = reinterpret_cast<const float*>(xu.data() + 2528 + 80 + 32);
      const ColorChannel& c0 = vc.channels[0];
      const ColorChannel& a0 = vc.channels[2];
      const float* v = static_cast<const float*>(vertices.cpu());
      char what[720];
      int at = std::snprintf(
          what, sizeof(what),
          "n=%u chans %u: c0 mat %d amb %d light %d mask 0x%02x diff %d attn %d, a0 mat %d light %d | amb0 %.2f %.2f "
          "%.2f %.2f mat0 %.2f %.2f %.2f %.2f light0 %.2f %.2f %.2f light1 %.2f %.2f %.2f | v0 col %.2f %.2f %.2f %.2f "
          "nrm %.2f %.2f %.2f | blend %d %d/%d cw %d | %u stages",
          count, vc.channelCount, static_cast<int>(c0.material), static_cast<int>(c0.ambient), c0.lighting,
          c0.lightMask, static_cast<int>(c0.diffuse), static_cast<int>(c0.attenuation), static_cast<int>(a0.material),
          a0.lighting, amb[0], amb[1], amb[2], amb[3], mat[0], mat[1], mat[2], mat[3], l0[0], l0[1], l0[2], l1[0],
          l1[1], l1[2], v[6], v[7], v[8], v[9], v[3], v[4], v[5], static_cast<int>(px.blendMode),
          static_cast<int>(px.srcFactor), static_cast<int>(px.dstFactor), px.colorWrite, tev.stageCount);
      for (unsigned i = 0; i < tev.stageCount && i < 3 && at < static_cast<int>(sizeof(what)) - 120; ++i) {
        const TevStage& st = tev.stages[i];
        const TexMap m = st.texMap < 8 ? state.Texture(st.texMap) : TexMap{};
        at += std::snprintf(what + at, sizeof(what) - at,
                            "; s%u c %u %u %u %u out %d k %u map %u (0x%08x %ux%u f%u) chan %d", i, st.color.a,
                            st.color.b, st.color.c, st.color.d, static_cast<int>(st.color.out), st.kcolorSel, st.texMap,
                            m.address, m.width, m.height, static_cast<unsigned>(m.format), static_cast<int>(st.channel));
      }
      g_probeModels.emplace_back(static_cast<uint32_t>(g_probeGroups.size()), what);
    }
    if (px.efbAlpha && g_probeMii.size() < 100) {
      const TevConfig tev = state.Tev();
      const VertexConfig vc = state.Vertex();
      const auto& vp = state.Viewport();
      const DkScissor sc = scissor_of(state, width, height);
      const float* v = static_cast<const float*>(vertices.cpu());
      char what[600];
      int at = std::snprintf(
          what, sizeof(what),
          "prim 0x%02x n=%u%s | z %d cmp %d w %d cull %d | blend %d %d/%d cw %d aw %d | alpha test %d/%u %d %d/%u | "
          "vp %.0f %.0f %.0f %.0f %.0f %.0f sc %u %u %u %u | chans %u light %d | v0 %.2f %.2f %.2f | %u stages",
          draw.primitive, count, vc.indexedPosMtx ? " mtx/vtx" : "", px.depthTest, static_cast<int>(px.depthCompare),
          px.depthWrite, static_cast<int>(px.cull), static_cast<int>(px.blendMode), static_cast<int>(px.srcFactor),
          static_cast<int>(px.dstFactor), px.colorWrite, px.alphaWrite, static_cast<int>(tev.alphaComp0),
          tev.alphaRef0, static_cast<int>(tev.alphaOp), static_cast<int>(tev.alphaComp1), tev.alphaRef1, vp[0], vp[1],
          vp[2], vp[3], vp[4], vp[5], sc.x, sc.y, sc.width, sc.height, vc.channelCount, vc.channels[0].lighting, v[0],
          v[1], v[2], tev.stageCount);
      for (unsigned i = 0; i < tev.stageCount && i < 4 && at < static_cast<int>(sizeof(what)) - 90; ++i) {
        const TevStage& st = tev.stages[i];
        const TexMap m = st.texMap < 8 ? state.Texture(st.texMap) : TexMap{};
        at += std::snprintf(what + at, sizeof(what) - at, "; s%u c%u%u%u%u a%u%u%u%u map %u 0x%08x %ux%u f%u", i,
                            st.color.a, st.color.b, st.color.c, st.color.d, st.alpha.a, st.alpha.b, st.alpha.c,
                            st.alpha.d, st.texMap, m.address, m.width, m.height, static_cast<unsigned>(m.format));
      }
      g_probeMii.emplace_back(static_cast<uint32_t>(g_probeGroups.size()), what);
    }
    if (!px.depthTest) {
      if (g_probeOverlays.size() >= 32) {
        g_probeOverlays.erase(g_probeOverlays.begin());  // (the frame's last ones are kept: the composite comes late)
      }
      const TevConfig tev = state.Tev();
      const TevStage& s0 = tev.stages[0];
      const std::vector<uint8_t> tu = state.TevUniforms();
      const std::vector<uint8_t> xu = state.XfUniforms();
      const float* reg = reinterpret_cast<const float*>(tu.data());       // TEV registers 0..3, RGBA
      const float* kc = reinterpret_cast<const float*>(tu.data() + 64);   // konst colours 0..3
      const float* mat = reinterpret_cast<const float*>(xu.data() + 2496);  // material colours 0..1
      const float* v = static_cast<const float*>(vertices.cpu());
      char what[640];
      std::snprintf(what, sizeof(what),
                    "pe 0x%06x efbAlpha %d | c0 mat %d amb %d light %d | "
                    "prim 0x%02x n=%u blend %d src %d dst %d logic %d colorWrite %d alphaWrite %d dstAlpha %d | "
                    "%u stages; s0 color %u %u %u %u op %d out %d alpha %u %u %u %u op %d out %d ksel %u/%u map %u "
                    "chan %d | prev %.2f %.2f %.2f %.2f reg0 %.2f %.2f %.2f %.2f k0 %.2f %.2f %.2f %.2f mat0 %.2f "
                    "%.2f %.2f %.2f | chans %u | v0 pos %.1f %.1f %.1f col %.2f %.2f %.2f %.2f",
                    state.Bp(0x43), px.efbAlpha, static_cast<int>(state.Vertex().channels[0].material),
                    static_cast<int>(state.Vertex().channels[0].ambient), state.Vertex().channels[0].lighting,
                    draw.primitive, count, static_cast<int>(px.blendMode), static_cast<int>(px.srcFactor),
                    static_cast<int>(px.dstFactor), static_cast<int>(px.logicOp), px.colorWrite, px.alphaWrite,
                    tev.dstAlpha, tev.stageCount, s0.color.a, s0.color.b, s0.color.c, s0.color.d,
                    static_cast<int>(s0.color.op), static_cast<int>(s0.color.out), s0.alpha.a, s0.alpha.b, s0.alpha.c,
                    s0.alpha.d, static_cast<int>(s0.alpha.op), static_cast<int>(s0.alpha.out), s0.kcolorSel,
                    s0.kalphaSel, s0.texMap, static_cast<int>(s0.channel), reg[0], reg[1], reg[2], reg[3], reg[4],
                    reg[5], reg[6], reg[7], kc[0], kc[1], kc[2], kc[3], mat[0], mat[1], mat[2], mat[3],
                    state.Vertex().channelCount, v[0], v[1], v[2], v[6], v[7], v[8], v[9]);
      g_probeOverlays.emplace_back(static_cast<uint32_t>(g_probeGroups.size()), what);
    }
    g_probeGroups.push_back(static_cast<uint8_t>((px.depthTest ? 1 : 0) | static_cast<uint8_t>(px.cull) << 1 |
                                                 (strip ? 8 : 0) | (state.Vertex().indexedPosMtx ? 16 : 0)));
    probe_report(cmd, static_cast<uint32_t>(g_probeGroups.size()));
  }
  // A command list every kDrawsPerList draws. A whole frame in one list went
  // wrong on the GPU (method errors on data-like words), a list per draw did
  // not; a list per draw also cost a queue flush - a kernel call - every few.
  static uint32_t drawsInList = 0;
  if (++drawsInList >= kDrawsPerList) {
    drawsInList = 0;
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
          drawIndex, draw.primitive, count, static_cast<unsigned long long>(vertices.gpu()), vertexBytes,
          viewport.x, viewport.y, viewport.width, viewport.height, viewport.near, viewport.far, scissor.x, scissor.y,
          scissor.width, scissor.height, width, height, c.tev.stageCount, c.tev.indStageCount, c.tev.texCoordCount);
      for (unsigned i = 0; i < c.tev.stageCount; ++i) {
        const TevStage& st = c.tev.stages[i];
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
}  // namespace

bool draw_gx(const State& state, const Draw& draw, uint32_t width, uint32_t height,
             const CommandProcessor::Memory& memory) {
  DkPrimitive primitive;
  if (draw.count == 0 || !primitive_of(draw.primitive, &primitive)) {
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
  if (probing()) {
    probe_vertices(state, "strip", draw.primitive, static_cast<const float*>(vertices.cpu()), draw.count);
  }
  return draw_staged(state, primitive, vertices, draw.count, width, height, memory, draw);
}

bool draw_converted(const State& state, uint8_t gxPrimitive, const float* vertices, uint32_t count, uint32_t width,
                    uint32_t height, const CommandProcessor::Memory& memory, const uint32_t* indices,
                    uint32_t indexCount) {
  DkPrimitive primitive;
  if (count == 0 || !primitive_of(gxPrimitive, &primitive)) {
    return false;
  }
  const uint32_t vertexBytes = count * static_cast<uint32_t>(kVertexFloats * sizeof(float));
  Allocation staged = stage(vertexBytes, 16);
  if (!staged) {
    return false;
  }
  std::memcpy(staged.cpu(), vertices, vertexBytes);
  Allocation stagedIndices;
  if (indexCount != 0) {
    stagedIndices = stage(indexCount * 4u, 16);
    if (!stagedIndices) {
      return false;
    }
    std::memcpy(stagedIndices.cpu(), indices, indexCount * 4u);
  }
  if (probing() && state.Pixel().depthTest) {
    probe_vertices(state, "list", gxPrimitive, vertices, count);
  }
  Draw draw;
  draw.primitive = gxPrimitive;
  draw.count = static_cast<uint16_t>(count > 0xFFFF ? 0xFFFF : count);
  return draw_staged(state, primitive, staged, count, width, height, memory, draw, stagedIndices, indexCount);
}

} // namespace dol::dk
