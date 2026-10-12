// libdol's own GX renderer, fed what the game feeds the hardware (gx_native.h).
#include "gx_native.h"

#include "dk.h"
#include "memory.h"

#include "wiinx/format/gx/command.hpp"

#include <vector>

namespace dol::gx_native {
namespace {

// a hardware (physical) address as a pointer into guest memory, as libdol's
// GX code reads CP array bases: cached MEM1/MEM2 first
const uint8_t* guest_bytes(uint32_t address, uint32_t size) {
  const uint32_t physical = address & 0x1FFFFFFF;
  for (const uint32_t base : {0x80000000u, 0xC0000000u, 0u}) {
    const uint32_t at = physical | base;
    if (physical != 0 && Memory::Contains(at, size)) {
      return static_cast<const uint8_t*>(Memory::GetPointer(at));
    }
  }
  return nullptr;
}

struct Renderer {
  wiinx::gx::State state;
  wiinx::gx::CommandProcessor::Memory memory = guest_bytes;
  wiinx::gx::CommandProcessor processor{state, memory, [this](const wiinx::gx::Draw& d) { draw(d); }};
  std::vector<uint8_t> pending;  // FIFO bytes not yet a whole packet
  bool passOpen = false;
  bool framePass = false;  // the open pass is begin_frame's, on the screen
  uint32_t width = 0, height = 0;
  Counts counts;
  dk::Texture* screen = nullptr;  // the frame's image: the EFB, drawn at the screen's size
  dk::TextureView screenView;
  dk::Texture depth;
  dk::TextureView depthView;

  void draw(const wiinx::gx::Draw& d) {
    if (!passOpen) {
      // A frame's first draw opens it: the screen as the EFB, cleared as the
      // copy clear registers say (begin_frame). VI presents it at the copy.
      begin_frame();
    }
    if (!passOpen) {
      ++counts.dropped;
      return;
    }
    (dk::draw_gx(state, d, width, height, memory) ? counts.draws : counts.refused)++;
  }
};

Renderer& renderer() {
  static Renderer r;
  return r;
}

} // namespace

bool initialize() {
  static const bool up = dk::initialize() && dk::initialize_screen(DkImageFormat_RGBA8_Unorm);
  return up;
}

// The gather pipe arrives a word at a time. Running the command processor on
// each one re-read an unfinished packet from its start every time - a GXBegin
// of a thousand vertices parsed its stream a thousand times - so the bytes are
// buffered and run in batches, and wherever the result is needed (flush).
constexpr size_t kRunBatch = 16 * 1024;
constexpr size_t kStallLimit = 256 * 1024;

void flush() {
  Renderer& r = renderer();
  if (r.pending.empty()) {
    return;
  }
  const size_t ran = r.processor.Run(r.pending.data(), r.pending.size());
  // (whatever is left is a packet still arriving - or bytes that are not a
  // command at all, which would hold everything after them; those are let go
  // once they pass a size no packet reaches)
  r.pending.erase(r.pending.begin(), r.pending.begin() + static_cast<std::ptrdiff_t>(ran));
  if (r.pending.size() > kStallLimit) {
    r.counts.stalled += static_cast<uint32_t>(r.pending.size());
    r.pending.clear();
  }
}

void write(const uint8_t* data, size_t size) {
  Renderer& r = renderer();
  r.pending.insert(r.pending.end(), data, data + size);
  if (r.pending.size() >= kRunBatch) {
    flush();
  }
}

void call_list(uint32_t address, uint32_t size) {
  Renderer& r = renderer();
  flush();  // what the FIFO set before the call is the list's state
  if (const uint8_t* list = guest_bytes(address, size)) {
    r.processor.Run(list, size);
  }
}

void begin_efb(uint32_t width, uint32_t height) {
  Renderer& r = renderer();
  r.passOpen = true;
  r.width = width;
  r.height = height;
}

void end_efb() { renderer().passOpen = false; }

void begin_frame() {
  Renderer& r = renderer();
  if (!initialize()) {
    return;  // no GPU: draws are counted as dropped
  }
  if (r.passOpen) {
    return;
  }
  dk::Texture& screen = dk::acquire_screen();
  r.screen = &screen;
  r.screenView = dk::make_view(screen);
  // The depth buffer at the size the screen's images are allocated at, not the
  // size handheld crops them to: bound beside a 1920x1080 image, a 1280x720
  // depth buffer was cleared and drawn past its end, over whatever memory
  // followed it - textures (the menus' glitches) and command memory (GPU
  // method errors on data-like words, a different method each run).
  uint32_t imageWidth = 0, imageHeight = 0;
  dk::screen_image_size(&imageWidth, &imageHeight);
  if (!r.depth.block) {
    r.depth = dk::create_texture(DkImageFormat_Z24S8, imageWidth, imageHeight, 1, 1, 1, dk::TextureRender);
    r.depthView = dk::make_view(r.depth);
  }
  const auto clear = r.state.CopyClearColor();
  dk::ColorTarget color;
  color.view = &r.screenView;
  color.clear = true;
  for (int i = 0; i < 4; ++i) {
    color.clear_color[i] = clear[i];
  }
  dk::DepthTarget depth;
  depth.view = &r.depthView;
  depth.clear_depth = true;
  depth.depth = r.state.CopyClearDepth();
  uint32_t width = 0, height = 0;
  dk::begin_pass(&color, 1, &depth, &width, &height);
  r.passOpen = true;
  r.framePass = true;
  r.width = width;
  r.height = height;
}

void end_frame() {
  Renderer& r = renderer();
  flush();
  if (!r.passOpen) {
    // A frame with no draws (a cleared loading screen) is still shown.
    begin_frame();
  }
  if (!r.passOpen || !r.framePass) {
    return;
  }
  dk::end_pass();
  dk::present();
  dk::gx_frame_done();
  // Once a second, what the frames drew and what they could not: the first
  // thing to read when a title draws nothing, or not all of it.
  static uint32_t frames = 0;
  static Counts total;
  const Counts c = r.counts;
  r.counts = {};
  total.draws += c.draws;
  total.refused += c.refused;
  total.dropped += c.dropped;
  total.stalled += c.stalled;
  if (++frames % 60 == 0) {
    dk::log("gx-native: frame %u: last 60 frames %u draws, %u refused (shaders), %u dropped, %u bytes stalled",
            frames, total.draws, total.refused, total.dropped, total.stalled);
    total = {};
  }
  r.passOpen = false;
  r.framePass = false;
}

namespace {
// The frame's pass again on the same image after a copy: kept, or cleared as
// the copy asks.
void reopen(Renderer& r, bool clear) {
  const auto clearColor = r.state.CopyClearColor();
  dk::ColorTarget color;
  color.view = &r.screenView;
  color.clear = clear;
  for (int i = 0; i < 4; ++i) {
    color.clear_color[i] = clearColor[i];
  }
  dk::DepthTarget depth;
  depth.view = &r.depthView;
  depth.clear_depth = clear;
  depth.depth = r.state.CopyClearDepth();
  uint32_t w = 0, h = 0;
  dk::begin_pass(&color, 1, &depth, &w, &h);
}
}  // namespace

void copy_tex(uint32_t address, uint16_t left, uint16_t top, uint16_t srcWidth, uint16_t srcHeight,
              uint16_t width, uint16_t height, bool clear) {
  Renderer& r = renderer();
  flush();
  if (!r.passOpen) {
    begin_frame();  // a copy of an EFB nothing was drawn into yet: its clear
  }
  if (!r.passOpen || !r.framePass || !r.screen) {
    return;
  }
  // The copy reads what the pass drew, so the pass ends first, and reopens on
  // the same image afterwards - kept, or cleared as the copy asks.
  dk::end_pass();
  const float sx = static_cast<float>(r.width) / 640.0f;
  const float sy = static_cast<float>(r.height) / 480.0f;
  // Within the image: the EFB is up to 528 lines tall, but drawn as 480 onto
  // the screen (viewport_of), so a copy reaching past line 480 - a full-EFB
  // copy - asked for rows below the image, and a blit out of bounds faults
  // the GPU (the queue then aborts at the next present).
  const auto clampTo = [](float v, uint32_t limit) {
    return static_cast<uint32_t>(v < 0.0f ? 0.0f : v > static_cast<float>(limit) ? static_cast<float>(limit) : v);
  };
  const uint32_t x0 = clampTo(left * sx, r.width), y0 = clampTo(top * sy, r.height);
  const uint32_t x1 = clampTo((left + srcWidth) * sx, r.width), y1 = clampTo((top + srcHeight) * sy, r.height);
  if (x1 <= x0 || y1 <= y0) {
    reopen(r, clear);
    return;
  }
  DkImageRect source{x0, y0, 0, x1 - x0, y1 - y0, 1};
  dk::efb_copy(address & 0x1FFFFFFF, width, height, *r.screen, source, r.memory);
  reopen(r, clear);
}

wiinx::gx::State& state() { return renderer().state; }

Counts take_counts() {
  Renderer& r = renderer();
  const Counts c = r.counts;
  r.counts = {};
  return c;
}

} // namespace dol::gx_native
