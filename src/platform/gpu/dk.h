// The GPU on deko3d: libdol's native graphics layer (docs/deko3d.md; section 6
// of docs/native.md).
//
// The console's GPU through deko3d, nothing in between: one device and queue,
// buffers, textures, views and samplers, passes, copies and the screen. GX
// draws onto these once the renderer moves off Aurora.
#pragma once

#include <deko3d.h>
#include <switch.h>

#include "wiinx/format/gx/command.hpp"
#include "wiinx/format/gx/pixel.hpp"

#include <cstdint>

namespace dol::dk {

void log(const char* format, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void fatal(const char* format, ...) __attribute__((format(printf, 1, 2)));

// the frames in flight: command and staging memory are rings of this many
// slices, each reused once its frame's fence has passed
constexpr uint32_t kFrames = 3;
// the descriptor sets: every view and sampler bound for sampling has a slot
constexpr uint32_t kImageSlots = 4096;
constexpr uint32_t kSamplerSlots = 1024;

// a run of GPU memory in a bigger block
struct Allocation {
  DkMemBlock block = nullptr;
  uint32_t offset = 0;
  uint32_t size = 0;
  explicit operator bool() const { return block != nullptr; }
  void* cpu() const { return static_cast<uint8_t*>(dkMemBlockGetCpuAddr(block)) + offset; }
  DkGpuAddr gpu() const { return dkMemBlockGetGpuAddr(block) + offset; }
};

// ---------- the device

bool initialize();
void shutdown();
DkDevice device();
DkQueue queue();
// the commands the frame records, closed into a list and submitted by submit()
DkCmdBuf commands();
void submit();
// `size` bytes of the frame's staging memory: what a write to a buffer or
// texture copies from; empty when the frame's slice is full
Allocation stage(uint32_t size, uint32_t alignment);
// the frame recorded so far submitted, its fence signalled, and the next
// frame's slices waited for and begun
void frame_end();
// everything submitted done
void wait_idle();
// (the layer's own: the writes' command buffer, and its submission)
DkCmdBuf transfer();
void submit_transfer();

// ---------- buffers

struct Buffer {
  DkMemBlock block = nullptr;
  uint32_t size = 0;
  void* cpu() const { return dkMemBlockGetCpuAddr(block); }
  DkGpuAddr gpu() const { return dkMemBlockGetGpuAddr(block); }
};

// `readback`: read by the CPU (cached for it); otherwise written by the CPU
// and read by the GPU
Buffer create_buffer(uint32_t size, bool readback = false);
void destroy(Buffer& buffer);
// bytes into a buffer in submission order, through staging
void write_buffer(const Buffer& buffer, uint32_t offset, const void* data, uint32_t size);

// ---------- textures, views and samplers

enum TextureFlags : uint32_t {
  TextureRender = 1,   // drawn into
  TextureStorage = 2,  // loaded and stored by shaders
};

struct Texture {
  DkImage image{};
  DkMemBlock block = nullptr; // null for the screen's images
  DkImageFormat format = DkImageFormat_None;
  DkImageType type = DkImageType_2D;
  uint32_t width = 0, height = 0, layers = 1, mip_levels = 1, samples = 1;
};

Texture create_texture(DkImageFormat format, uint32_t width, uint32_t height, uint32_t layers = 1,
                       uint32_t mip_levels = 1, uint32_t samples = 1, uint32_t flags = 0,
                       DkImageType type = DkImageType_2D);
void destroy(Texture& texture);
// texels into one mip of a texture, through staging; bytes_per_row 0 is tight
void write_texture(const Texture& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t layer, uint32_t width,
                   uint32_t height, const void* data, uint32_t size, uint32_t bytes_per_row = 0);

struct TextureView {
  const Texture* texture = nullptr;
  DkImageView view{};
  int32_t slot = -1; // its image descriptor, made when first sampled
};

TextureView make_view(const Texture& texture, uint32_t base_mip = 0, uint32_t mip_count = 0, uint32_t base_layer = 0,
                      uint32_t layer_count = 0, DkImageFormat format = DkImageFormat_None);
void destroy(TextureView& view);
// the view's descriptor slot, made the first time
int32_t image_slot(TextureView& view);

struct Sampler {
  DkSampler sampler{};
  int32_t slot = -1;
};

int32_t sampler_slot(Sampler& sampler);
void destroy(Sampler& sampler);
// the descriptor sets bound for the frame's draws
void bind_descriptor_sets();

// ---------- passes and copies

struct ColorTarget {
  TextureView* view = nullptr;
  bool clear = false;
  float clear_color[4] = {};
};

struct DepthTarget {
  TextureView* view = nullptr;
  bool clear_depth = false, clear_stencil = false;
  float depth = 1.0f;
  uint8_t stencil = 0;
};

// the targets bound, the viewport and scissor the whole of them, and the
// clears asked for; the pass's size is returned
void begin_pass(ColorTarget* colors, uint32_t color_count, DepthTarget* depth, uint32_t* width, uint32_t* height);
// its drawing finished before anything after it reads what it drew
void end_pass();

void copy_buffer(const Buffer& from, uint32_t from_offset, const Buffer& to, uint32_t to_offset, uint32_t size);
void copy_texture(const Texture& from, uint32_t from_mip, const DkImageRect& from_rect, const Texture& to,
                  uint32_t to_mip, const DkImageRect& to_rect, bool filter = false);
void copy_texture_to_buffer(const Texture& from, uint32_t mip, const DkImageRect& rect, const Buffer& to,
                            uint32_t offset, uint32_t bytes_per_row);

// ---------- shaders

enum class ShaderStage : uint8_t { Vertex, Fragment };

// GLSL compiled by UAM and loaded, once a run per source; null when it does
// not compile (or the build has no UAM). Kept for the program's life.
const DkShader* shader(ShaderStage stage, const char* glsl);

inline uint32_t align_up(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

// ---------- GX's pixel state

// GX's blending, logic op, depth test, culling and write masks as deko3d's
// state objects (dk_gx_state_switch.cpp). Culling takes GX's front faces as
// the clockwise ones, drawn without a flip (GX and the device both put the
// origin at the top left).
struct PipelineState {
  DkRasterizerState rasterizer;
  DkColorState color;
  DkColorWriteState color_write;
  DkBlendState blend;
  DkDepthStencilState depth_stencil;
};

PipelineState pipeline_state(const wiinx::gx::PixelState& state);
// on the frame's commands, for the draws after
void bind(const PipelineState& state);

// ---------- GX draws (dk_gx_renderer_switch.cpp)

// What a GX command stream draws, on the pass that is open: the shaders for
// the state, its uniforms, the draw's vertices converted, its pixel state, the
// viewport. `width` and `height` are the target's: GX's 640-wide EFB space is
// scaled onto it. False when the state asks for something the shaders do not
// do yet (the draw is skipped).
bool draw_gx(const wiinx::gx::State& state, const wiinx::gx::Draw& draw, uint32_t width, uint32_t height);

// ---------- the screen

// the console's screen: 1280x720 in handheld, 1920x1080 docked
void screen_size(uint32_t* width, uint32_t* height);
bool initialize_screen(DkImageFormat format);
// the next image to draw the frame into, its size the screen's now
Texture& acquire_screen();
// that image shown, and the frame ended (frame_end)
void present();

} // namespace dol::dk
