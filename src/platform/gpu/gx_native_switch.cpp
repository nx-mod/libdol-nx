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
  dk::TextureView screenView;
  dk::Texture depth;
  dk::TextureView depthView;

  void draw(const wiinx::gx::Draw& d) {
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

void write(const uint8_t* data, size_t size) {
  Renderer& r = renderer();
  r.pending.insert(r.pending.end(), data, data + size);
  const size_t ran = r.processor.Run(r.pending.data(), r.pending.size());
  // (whatever is left is a packet still arriving - or bytes that are not a
  // command at all, which would hold everything after them; those are let go
  // once they pass a size no packet reaches)
  r.pending.erase(r.pending.begin(), r.pending.begin() + static_cast<std::ptrdiff_t>(ran));
  if (r.pending.size() > 0x10000) {
    r.counts.stalled += static_cast<uint32_t>(r.pending.size());
    r.pending.clear();
  }
}

void call_list(uint32_t address, uint32_t size) {
  Renderer& r = renderer();
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
  if (r.passOpen) {
    return;
  }
  dk::Texture& screen = dk::acquire_screen();
  r.screenView = dk::make_view(screen);
  if (r.depth.width != screen.width || r.depth.height != screen.height) {
    if (r.depth.block) {
      dk::destroy(r.depthView);
      dk::destroy(r.depth);
    }
    r.depth = dk::create_texture(DkImageFormat_Z24S8, screen.width, screen.height, 1, 1, 1, dk::TextureRender);
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
  if (!r.passOpen || !r.framePass) {
    return;
  }
  dk::end_pass();
  dk::present();
  r.passOpen = false;
  r.framePass = false;
}

wiinx::gx::State& state() { return renderer().state; }

Counts take_counts() {
  Renderer& r = renderer();
  const Counts c = r.counts;
  r.counts = {};
  return c;
}

} // namespace dol::gx_native
