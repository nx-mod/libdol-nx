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
  uint32_t width = 0, height = 0;
  Counts counts;

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

wiinx::gx::State& state() { return renderer().state; }

Counts take_counts() {
  Renderer& r = renderer();
  const Counts c = r.counts;
  r.counts = {};
  return c;
}

} // namespace dol::gx_native
