// libdol's own GX renderer, fed what the game feeds the hardware (gx_native.h).
#include "gx_native.h"

#include "dk.h"
#include "memory.h"
#include "switch_layout.h"

#include "wiinx/format/gx/command.hpp"
#include "wiinx/format/gx/vertex_stream.hpp"

#include <algorithm>
#include <switch.h>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <array>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

void SwitchPinCurrentThread(int core) noexcept;  // (app/switch/switch_thread_stack.cpp)

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

struct Renderer;
void snapshot(Renderer& r, uint32_t draw);  // (below)

// Diagnosis: in a few probe frames late in a run, the EFB saved as small
// images at points through the frame (logs/snap_<frame>_<draw>.ppm) - where a
// scene appears, and what turns it black, seen rather than guessed.
constexpr uint32_t kSnapWidth = 320, kSnapHeight = 198;
// (diagnosis) off: each one stalls the GPU, and a run wrote 80 MB of them
constexpr bool kSnapshots = false;
constexpr uint32_t kSnapAt[] = {100, 400, 1500, 4000, 8000};
// (frames of a race: the one before drew more than this - a menu draws ~4000)
constexpr uint32_t kSnapDraws = 6000, kSnapFrames = 4;

struct Renderer {
  wiinx::gx::State state;
  wiinx::gx::CommandProcessor::Memory memory = guest_bytes;
  wiinx::gx::CommandProcessor processor{state, memory, [this](const wiinx::gx::Draw& d) { draw(d); }};
  std::vector<uint8_t> pending;  // FIFO bytes not yet a whole packet
  bool passOpen = false;
  bool framePass = false;  // the open pass is begin_frame's, on the screen
  // Whether the display copy that ended the last frame cleared the EFB. One
  // that did not leaves the image there: a game drawing at 30 fps copies the
  // same EFB on two retraces, and clearing it anyway showed the clear colour
  // every other frame (Mega Man 9).
  bool clearNext = true;
  uint32_t width = 0, height = 0;
  Counts counts;
  dk::Texture* screen = nullptr;  // the swapchain image being presented (present_now)
  // The XFBs display copies went to, as images: VI shows one of them from the
  // flip on, so a copy is only drawn here and presented at VIFlush. A game may
  // copy more than once into one XFB before it flips (Mega Man 9 copies a
  // cleared EFB, then the scaled-up frame): only the last copy is ever seen.
  struct Xfb {
    uint32_t address = 0;
    dk::Texture image;
    uint32_t width = 0, height = 0;
    uint32_t used = 0;  // when it was last copied into (for reuse)
  };
  std::array<Xfb, 4> xfbs;
  uint32_t copies = 0;
  dk::TextureView screenView;
  // The EFB, all 528 of its lines, at the screen's scale: games draw into the
  // strip below line 480 - a menu's 3D models, rendered there and copied into
  // a texture - which drawn straight onto the 480-line screen fell off it, and
  // came back from the copy empty (the models missing, their shadows left).
  // The top 480 lines go to the screen at the frame's end.
  dk::Texture efb;
  dk::TextureView efbView;
  uint32_t efbRows = 0;  // the frame's 528 lines, in pixels
  // the snapshots (diagnosis): this frame's, waiting to be written at its end
  struct Snap {
    dk::Allocation pixels;
    uint32_t draw;
  };
  std::vector<Snap> snaps;
  dk::Texture snapTextures[28];
  uint32_t frameCopies = 0;
  uint32_t frameNumber = 0, frameDraws = 0, lastFrameDraws = 0, snapFramesDone = 0;
  bool snapFrame = false;
  // the display copy's source (GXSetDispCopySrc), in GX's EFB space
  uint16_t displayLeft = 0, displayTop = 0, displayWidth = 640, displayHeight = 480;
  dk::Texture depth;
  dk::TextureView depthView;

  // Draws that share a state, merged into one: a menu makes thousands of tiny
  // draws a frame (a glyph, a button's corner) that differ only in their
  // vertices, and drawn one by one each rebuilt the shaders' keys, the
  // uniforms and the bindings - most of the frame. Aurora merged them the same
  // way. A batch is sent the moment the state is about to change (the command
  // processor's hook), before a copy, and at the frame's end.
  //
  // Every kind of triangle goes into one indexed triangle list - quads as two
  // triangles, strips with their alternating winding kept (culling reads it),
  // fans around their first vertex - and lines and line strips into one line
  // list: a menu's and a race's 3D models are nine draws in ten strips, and
  // drawn one by one they were most of a frame's GPU draws. Each vertex is
  // converted and staged once; the indices do the expanding.
  struct Batch {
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    uint32_t count = 0;      // vertices
    uint8_t primitive = 0;   // the list it draws as: triangles, lines or points
    uint64_t generation = 0;
  } batch;

  // the list a GX primitive merges into
  static uint8_t list_of(uint8_t primitive) {
    switch (primitive & 0xF8) {
    case 0x80: case 0x88: case 0x90: case 0x98: case 0xA0: return 0x90;  // quads, triangles, strips, fans
    case 0xA8: case 0xB0: return 0xA8;                                   // lines, line strips
    default: return 0xB8;                                                // points
    }
  }

  // `n` vertices from `base` as `primitive`, into the batch's indices
  void add_indices(uint8_t primitive, uint32_t base, uint32_t n) {
    std::vector<uint32_t>& out = batch.indices;
    switch (primitive & 0xF8) {
    case 0x80: case 0x88:  // quads: (0 1 2) (0 2 3)
      for (uint32_t q = 0; q + 3 < n; q += 4) {
        out.insert(out.end(), {base + q, base + q + 1, base + q + 2, base + q, base + q + 2, base + q + 3});
      }
      break;
    case 0x90:
      for (uint32_t i = 0; i + 2 < n; i += 3) {
        out.insert(out.end(), {base + i, base + i + 1, base + i + 2});
      }
      break;
    case 0x98:  // strip: every other triangle turned back, as the hardware winds it
      for (uint32_t i = 0; i + 2 < n; ++i) {
        if (i & 1) {
          out.insert(out.end(), {base + i + 1, base + i, base + i + 2});
        } else {
          out.insert(out.end(), {base + i, base + i + 1, base + i + 2});
        }
      }
      break;
    case 0xA0:  // fan
      for (uint32_t i = 1; i + 1 < n; ++i) {
        out.insert(out.end(), {base, base + i, base + i + 1});
      }
      break;
    case 0xA8:
      for (uint32_t i = 0; i + 1 < n; i += 2) {
        out.insert(out.end(), {base + i, base + i + 1});
      }
      break;
    case 0xB0:  // line strip
      for (uint32_t i = 0; i + 1 < n; ++i) {
        out.insert(out.end(), {base + i, base + i + 1});
      }
      break;
    default:  // points
      for (uint32_t i = 0; i < n; ++i) {
        out.push_back(base + i);
      }
      break;
    }
  }

  void send_batch() {
    if (batch.count == 0) {
      return;
    }
    if (!batch.indices.empty()) {
      (dk::draw_converted(state, batch.primitive, batch.vertices.data(), batch.count, width, height, memory,
                          batch.indices.data(), static_cast<uint32_t>(batch.indices.size()))
           ? counts.batches : counts.refused)++;
    }
    batch.count = 0;
    batch.indices.clear();
  }

  // What split batches, by change (ChangeCause), counted flat until the
  // once-a-second line takes the commonest: BP registers, CP registers, XF
  // arrays, then XF addresses
  static constexpr uint32_t kSplitXf = 256 + 256 + 16;
  std::vector<uint32_t> splits = std::vector<uint32_t>(kSplitXf + 0x1100);
  static uint32_t split_slot(uint32_t cause) {
    const uint32_t kind = cause >> 16, id = cause & 0xFFFF;
    switch (kind) {
    case 1: return id & 0xFF;
    case 3: return 256 + (id & 0xFF);
    case 4: return 512 + (id & 0xF);
    default: return kSplitXf + (id < 0x1100 ? id : 0x10FF);
    }
  }
  static uint32_t split_cause(uint32_t slot) {
    return slot < 256 ? 1u << 16 | slot : slot < 512 ? 3u << 16 | (slot - 256)
         : slot < kSplitXf ? 4u << 16 | (slot - 512) : 2u << 16 | (slot - kSplitXf);
  }
  void count_split(uint32_t cause) {
    ++counts.split;
    ++splits[split_slot(cause)];
  }

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
    ++counts.draws;
    ++frameDraws;
    {
      // (diagnosis) the EFB's pixel format as it changes: which draws a frame
      // makes in each, the first changes of a probe frame
      static uint32_t lastFormat = ~0u, logged = 0, frameLogged = ~0u;
      const uint32_t format = state.Bp(0x43) & 7;
      if (format != lastFormat) {
        if (dk::probing()) {
          if (frameLogged != frameNumber) {
            frameLogged = frameNumber;
            logged = 0;
          }
          if (logged++ < 12) {
            dk::log("gx-pe: frame %u draw %u: EFB format %u -> %u (PE_CONTROL 0x%06x, %u loads)", frameNumber,
                    frameDraws, lastFormat, format, state.Bp(0x43), state.PeControlWrites());
          }
        }
        lastFormat = format;
      }
    }
    if (snapFrame) {
      for (const uint32_t at : kSnapAt) {
        if (frameDraws == at) {
          snapshot(*this, at);
        }
      }
    }
    const uint8_t primitive = list_of(d.primitive);
    if (batch.count != 0 && (batch.primitive != primitive || batch.generation != state.Generation())) {
      send_batch();
    }
    // (the buffer only grows: resizing it per draw zeroed what the
    // conversion then wrote over)
    const size_t at = static_cast<size_t>(batch.count) * wiinx::gx::kVertexFloats;
    const size_t need = at + static_cast<size_t>(d.count) * wiinx::gx::kVertexFloats;
    if (batch.vertices.size() < need) {
      batch.vertices.resize(need > batch.vertices.size() * 2 ? need : batch.vertices.size() * 2);
    }
    if (wiinx::gx::ConvertVertices(d.format, d.records, d.bytes, d.count, batch.vertices.data() + at) != d.count) {
      ++counts.refused;
      return;
    }
    if ((d.primitive & 0xF8) == 0x98 || (d.primitive & 0xF8) == 0xA0) {
      ++counts.alone;  // (strips and fans: merged now, still counted)
    }
    add_indices(d.primitive, batch.count, d.count);
    batch.count += d.count;
    batch.primitive = primitive;
    batch.generation = state.Generation();
  }
};

Renderer& renderer() {
  static Renderer r;  // (never copied: its command processor calls back into it)
  static const bool hooked = [] {
    r.processor.SetBeforeChange([] {
      if (r.batch.count != 0) {
        r.count_split(r.processor.ChangeCause());
      }
      r.send_batch();
    });
    return true;
  }();
  (void)hooked;
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

void flush_now() {
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

void write_now(const uint8_t* data, size_t size) {
  Renderer& r = renderer();
  r.pending.insert(r.pending.end(), data, data + size);
  if (r.pending.size() >= kRunBatch) {
    flush_now();
  }
}

void call_list_now(uint32_t address, uint32_t size) {
  Renderer& r = renderer();
  flush_now();  // what the FIFO set before the call is the list's state
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
  uint32_t screenWidth = 0, screenHeight = 0;
  dk::screen_size(&screenWidth, &screenHeight);  // (the screen is presented later, at the flip)
  // The EFB and its depth buffer allocated once, at the docked size; each
  // frame uses as much as the screen's size asks (as the screen images do).
  // (Depth matches the EFB: bound beside a larger colour target, a smaller
  // depth buffer was cleared and drawn past its end - glitched textures and
  // GPU method errors.)
  uint32_t imageWidth = 0, imageHeight = 0;
  dk::screen_image_size(&imageWidth, &imageHeight);
  const uint32_t allocatedRows = imageHeight * 528 / 480;
  if (!r.efb.block) {
    r.efb = dk::create_texture(DkImageFormat_RGBA8_Unorm, imageWidth, allocatedRows, 1, 1, 1, dk::TextureRender);
    r.efbView = dk::make_view(r.efb);
    r.depth = dk::create_texture(DkImageFormat_Z24S8, imageWidth, allocatedRows, 1, 1, 1, dk::TextureRender);
    r.depthView = dk::make_view(r.depth);
  }
  if (!r.efb.block || !r.depth.block) {
    return;
  }
  r.efbRows = screenHeight * 528 / 480;
  r.efb.width = r.depth.width = screenWidth;  // (the pass's size: begin_pass reads them)
  r.efb.height = r.depth.height = r.efbRows;
  const auto clear = r.state.CopyClearColor();
  dk::ColorTarget color;
  color.view = &r.efbView;
  color.clear = r.clearNext;
  for (int i = 0; i < 4; ++i) {
    color.clear_color[i] = clear[i];
  }
  dk::DepthTarget depth;
  depth.view = &r.depthView;
  depth.clear_depth = r.clearNext;
  depth.depth = r.state.CopyClearDepth();
  uint32_t width = 0, height = 0;
  dk::begin_pass(&color, 1, &depth, &width, &height);
  height = screenHeight;  // (draws map GX's 480 lines onto this; the rest is the strip below)
  dk::gx_rebind_all();
  r.passOpen = true;
  r.framePass = true;
  r.width = width;
  r.height = height;
  r.lastFrameDraws = r.frameDraws;
  r.frameDraws = 0;
  r.frameCopies = 0;
  r.snapFrame = kSnapshots && dk::probing() && r.lastFrameDraws > kSnapDraws && r.snapFramesDone < kSnapFrames;
}


// ---------- the frame rate overlay (from nx-mod/haloce-nx's host_dk.c)
//
// "60 FPS  16 MS": frames shown over the last second and the slowest of them,
// drawn over the finished frame with clears alone - a 3x5 bitmap font, each
// row's run of lit cells one scissored clear - so it needs no shader of its own.
constexpr bool kOverlay = true;

const unsigned char* overlay_glyph(char c) {
  static const struct {
    char c;
    unsigned char rows[5];
  } font[] = {
      {'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}}, {'3', {7, 1, 7, 1, 7}},
      {'4', {5, 5, 7, 1, 1}}, {'5', {7, 4, 7, 1, 7}}, {'6', {7, 4, 7, 5, 7}}, {'7', {7, 1, 1, 1, 1}},
      {'8', {7, 5, 7, 5, 7}}, {'9', {7, 5, 7, 1, 7}}, {'F', {7, 4, 6, 4, 4}}, {'M', {5, 7, 7, 5, 5}},
      {'P', {6, 5, 6, 4, 4}}, {'S', {7, 4, 7, 1, 7}},
  };
  for (const auto& g : font) {
    if (g.c == c) {
      return g.rows;
    }
  }
  return nullptr;
}

void overlay_rectangle(DkCmdBuf cmd, int32_t x, int32_t y, int32_t width, int32_t height, float shade) {
  const DkScissor scissor{static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(width),
                          static_cast<uint32_t>(height)};
  dkCmdBufSetScissors(cmd, 0, &scissor, 1);
  dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, shade, shade, shade, 1.0f);
}

void draw_overlay(Renderer& r) {
  static uint64_t secondStart = 0, previous = 0, slowest = 0, slowestShown = 0;
  static unsigned frames = 0, fps = 0;
  const uint64_t now = armGetSystemTick();
  if (previous && now - previous > slowest) {
    slowest = now - previous;
  }
  previous = now;
  ++frames;
  if (!secondStart) {
    secondStart = now;
  } else if (armTicksToNs(now - secondStart) >= 1000000000ull) {
    fps = static_cast<unsigned>(static_cast<uint64_t>(frames) * 1000000000ull / armTicksToNs(now - secondStart));
    slowestShown = slowest;
    frames = 0;
    slowest = 0;
    secondStart = now;
  }
  if (!kOverlay || !r.screen) {
    return;
  }
  char text[48];
  const int length = std::snprintf(text, sizeof(text), "%u FPS  %u MS", fps,
                                   static_cast<unsigned>((armTicksToNs(slowestShown) + 500000ull) / 1000000ull));
  if (length <= 0) {
    return;
  }
  // 4 pixels a cell at 720 lines, 6 at 1080; a character 3 cells and a space;
  // a dark box a cell wider all round, top left
  const int32_t cell = std::max<int32_t>(2, static_cast<int32_t>(r.height) / 180);
  const int32_t x = cell * 3, y = cell * 3;
  dk::ColorTarget color;
  color.view = &r.screenView;
  uint32_t w = 0, h = 0;
  dk::begin_pass(&color, 1, nullptr, &w, &h);
  DkCmdBuf cmd = dk::commands();
  dkCmdBufBarrier(cmd, DkBarrier_Full, 0);  // (after the 2D engine's blit to the screen)
  overlay_rectangle(cmd, x - cell, y - cell, (length * 4 + 1) * cell, cell * 7, 0.0f);
  for (int index = 0; index < length; ++index) {
    const unsigned char* rows = overlay_glyph(text[index]);
    if (!rows) {
      continue;
    }
    for (int row = 0; row < 5; ++row) {
      int column = 0;
      while (column < 3) {
        if (!(rows[row] & (4 >> column))) {
          ++column;
          continue;
        }
        const int start = column;
        while (column < 3 && (rows[row] & (4 >> column))) {
          ++column;
        }
        overlay_rectangle(cmd, x + (index * 4 + start) * cell, y + row * cell, (column - start) * cell, cell, 0.9f);
      }
    }
  }
  dk::end_pass();
}

void copy_display_now(uint32_t address, bool clear) {
  Renderer& r = renderer();
  flush_now();
  if (!r.passOpen) {
    // A frame with no draws (a cleared loading screen) is still shown.
    begin_frame();
  }
  if (!r.passOpen || !r.framePass) {
    return;
  }
  r.send_batch();
  if (r.snapFrame) {
    snapshot(r, 99999);  // (the frame as finished)
  }
  dk::end_pass();
  // What the display copy takes (GXSetDispCopySrc) stretched over the whole
  // screen, as VI stretches the XFB: Mario Kart Wii draws 608x456 of the EFB,
  // and showing all 640x480 of it left black bars on the right and bottom.
  {
    const float sx = static_cast<float>(r.width) / 640.0f, sy = static_cast<float>(r.height) / 480.0f;
    const uint32_t x0 = std::min<uint32_t>(static_cast<uint32_t>(r.displayLeft * sx), r.width);
    const uint32_t y0 = std::min<uint32_t>(static_cast<uint32_t>(r.displayTop * sy), r.efbRows);
    const uint32_t x1 = std::min<uint32_t>(static_cast<uint32_t>((r.displayLeft + r.displayWidth) * sx), r.width);
    const uint32_t y1 =
        std::min<uint32_t>(static_cast<uint32_t>((r.displayTop + r.displayHeight) * sy), r.efbRows);
    // the XFB this copy goes to: the one already kept for that address, or
    // the slot copied into longest ago
    Renderer::Xfb* xfb = nullptr;
    for (auto& slot : r.xfbs) {
      if (slot.address == address && slot.image.block) {
        xfb = &slot;
      }
    }
    if (!xfb) {
      xfb = &*std::min_element(r.xfbs.begin(), r.xfbs.end(),
                               [](const Renderer::Xfb& a, const Renderer::Xfb& b) { return a.used < b.used; });
      if (!xfb->image.block) {
        uint32_t imageWidth = 0, imageHeight = 0;
        dk::screen_image_size(&imageWidth, &imageHeight);
        xfb->image = dk::create_texture(DkImageFormat_RGBA8_Unorm, imageWidth, imageHeight, 1, 1, 1, dk::TextureRender);
      }
      xfb->address = address;
    }
    xfb->used = ++r.copies;
    xfb->width = r.width;
    xfb->height = r.height;
    if (x1 > x0 && y1 > y0 && xfb->image.block) {
      dk::copy_texture(r.efb, 0, DkImageRect{x0, y0, 0, x1 - x0, y1 - y0, 1}, xfb->image, 0,
                       DkImageRect{0, 0, 0, r.width, r.height, 1}, /*filter=*/true);
    }
  }
  r.clearNext = clear;
  ++r.frameNumber;
  if (!r.snaps.empty()) {
    dk::wait_idle();
    for (const auto& snap : r.snaps) {
      char path[128];
      std::snprintf(path, sizeof(path), WIINX_GAME_PATH("logs/snap_%u_%u.ppm"), r.frameNumber, snap.draw);
      if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", kSnapWidth, kSnapHeight);
        const auto* rgba = static_cast<const uint8_t*>(snap.pixels.cpu());
        std::vector<uint8_t> rgb(kSnapWidth * kSnapHeight * 3);
        for (uint32_t i = 0; i < kSnapWidth * kSnapHeight; ++i) {
          rgb[i * 3] = rgba[i * 4];
          rgb[i * 3 + 1] = rgba[i * 4 + 1];
          rgb[i * 3 + 2] = rgba[i * 4 + 2];
        }
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
      }
    }
    dk::log("gx-native: frame %u: %zu snapshots written", r.frameNumber, r.snaps.size());
    r.snaps.clear();
    ++r.snapFramesDone;
  }
  // Once a second, what the frames drew and what they could not: the first
  // thing to read when a title draws nothing, or not all of it.
  static uint32_t frames = 0;
  static Counts total;
  const Counts c = r.counts;
  r.counts = {};
  total.draws += c.draws;
  total.batches += c.batches;
  total.refused += c.refused;
  total.dropped += c.dropped;
  total.stalled += c.stalled;
  total.alone += c.alone;
  total.split += c.split;
  if (++frames % 60 == 0) {
    dk::log("gx-native: frame %u: last 60 frames %u draws in %u GPU draws, %u refused, %u dropped, %u bytes stalled",
            frames, total.draws, total.batches, total.refused, total.dropped, total.stalled);
    // what kept draws apart: strips and fans, and the changes that split
    // batches (BP register, XF address, CP register, XF array), commonest first
    char top[256];
    int at = 0;
    static const char* const kKinds[] = {"?", "bp", "xf", "cp", "xfa"};
    for (int n = 0; n < 8 && at < static_cast<int>(sizeof(top)) - 32; ++n) {
      const auto most = std::max_element(r.splits.begin(), r.splits.end());
      if (*most == 0) {
        break;
      }
      const uint32_t cause = Renderer::split_cause(static_cast<uint32_t>(most - r.splits.begin()));
      at += std::snprintf(top + at, sizeof(top) - at, " %s:%X=%u", kKinds[cause >> 16], cause & 0xFFFF, *most);
      *most = 0;
    }
    top[at] = '\0';
    dk::log("gx-native: apart: %u strips/fans (merged), %u splits:%s", total.alone, total.split, top);
    std::fill(r.splits.begin(), r.splits.end(), 0u);
    total = {};
  }
  r.passOpen = false;
  r.framePass = false;
}

namespace {
void reopen(Renderer& r, bool clear);
}  // namespace

// What VI shows from the flip on: the image of the XFB at `address`, the last
// display copy into it (0, or one never copied into: black).
void present_now(uint32_t address) {
  Renderer& r = renderer();
  if (!initialize()) {
    return;
  }
  const bool reopenEfb = r.passOpen;  // (the next frame's draws already begun)
  if (reopenEfb) {
    flush_now();
    r.send_batch();
    dk::end_pass();
  }
  dk::Texture& screen = dk::acquire_screen();
  r.screen = &screen;
  r.screenView = dk::make_view(screen);
  const Renderer::Xfb* shown = nullptr;
  for (const auto& slot : r.xfbs) {
    if (address != 0 && slot.address == address && slot.image.block && slot.width != 0) {
      shown = &slot;
    }
  }
  if (shown) {
    dk::copy_texture(shown->image, 0, DkImageRect{0, 0, 0, shown->width, shown->height, 1}, screen, 0,
                     DkImageRect{0, 0, 0, screen.width, screen.height, 1}, /*filter=*/true);
  } else {
    dk::ColorTarget color;
    color.view = &r.screenView;
    color.clear = true;
    color.clear_color[3] = 1.0f;  // black
    uint32_t w = 0, h = 0;
    dk::begin_pass(&color, 1, nullptr, &w, &h);
    dk::end_pass();
  }
  draw_overlay(r);
  dk::present();
  dk::gx_frame_done();
  r.screen = nullptr;
  if (reopenEfb) {
    reopen(r, false);
    dk::gx_rebind_all();
  }
}

namespace {
// The frame's pass again on the same image after a copy: kept, or cleared as
// the copy asks.
void reopen(Renderer& r, bool clear) {
  const auto clearColor = r.state.CopyClearColor();
  dk::ColorTarget color;
  color.view = &r.efbView;
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

void snapshot(Renderer& r, uint32_t draw) {
  if (r.snaps.size() >= 28 || !r.passOpen) {
    return;
  }
  r.send_batch();
  dk::end_pass();
  dk::Texture& image = r.snapTextures[r.snaps.size()];
  if (!image.block) {
    image = dk::create_texture(DkImageFormat_RGBA8_Unorm, kSnapWidth, kSnapHeight, 1, 1, 1, 0);
  }
  dk::Allocation pixels = dk::stage(kSnapWidth * kSnapHeight * 4, 256);
  if (image.block && pixels) {
    // the whole EFB, the strip below the screen too
    dk::copy_texture(r.efb, 0, DkImageRect{0, 0, 0, r.width, r.efbRows, 1}, image, 0,
                     DkImageRect{0, 0, 0, kSnapWidth, kSnapHeight, 1}, /*filter=*/true);
    DkCmdBuf cmd = dk::commands();
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    DkImageView view;
    dkImageViewDefaults(&view, &image.image);
    const DkImageRect rect{0, 0, 0, kSnapWidth, kSnapHeight, 1};
    const DkCopyBuf to{pixels.gpu(), 0, 0};
    dkCmdBufCopyImageToBuffer(cmd, &view, &rect, &to, 0);
    r.snaps.push_back({pixels, draw});
  }
  reopen(r, false);
  dk::gx_rebind_all();
}
}  // namespace

void set_display_source_now(uint16_t left, uint16_t top, uint16_t width, uint16_t height) {
  Renderer& r = renderer();
  if (width != 0 && height != 0) {
    r.displayLeft = left;
    r.displayTop = top;
    r.displayWidth = width;
    r.displayHeight = height;
  }
}

void copy_tex_now(uint32_t address, uint16_t left, uint16_t top, uint16_t srcWidth, uint16_t srcHeight,
                  uint16_t width, uint16_t height, bool clear, uint32_t format, const uint8_t* vfilter) {
  Renderer& r = renderer();
  flush_now();
  if (!r.passOpen) {
    begin_frame();  // a copy of an EFB nothing was drawn into yet: its clear
  }
  if (!r.passOpen || !r.framePass) {
    return;
  }
  r.send_batch();  // what it copies includes every draw before it
  // The copy reads what the pass drew, so the pass ends first, and reopens on
  // the same image afterwards - kept, or cleared as the copy asks.
  dk::end_pass();
  const float sx = static_cast<float>(r.width) / 640.0f;
  const float sy = static_cast<float>(r.height) / 480.0f;
  // Within the EFB's 528 lines (a blit out of bounds faults the GPU, and the
  // queue then aborts at the next present).
  const auto clampTo = [](float v, uint32_t limit) {
    return static_cast<uint32_t>(v < 0.0f ? 0.0f : v > static_cast<float>(limit) ? static_cast<float>(limit) : v);
  };
  const uint32_t x0 = clampTo(left * sx, r.width), y0 = clampTo(top * sy, r.efbRows);
  const uint32_t x1 = clampTo((left + srcWidth) * sx, r.width), y1 = clampTo((top + srcHeight) * sy, r.efbRows);
  if (x1 <= x0 || y1 <= y0) {
    reopen(r, false);
    return;
  }
  DkImageRect source{x0, y0, 0, x1 - x0, y1 - y0, 1};
  // (diagnosis) the first time each copy address is seen, in any frame: the
  // EFB just before it and what it is - a Mii's face or icon, a map, made at a
  // load and never again (snap_<frame>_6<nnn>, nnn the copy's number in the run)
  static std::vector<uint32_t> seenCopies;
  static uint32_t newCopies = 0;
  if (newCopies < 40 && std::find(seenCopies.begin(), seenCopies.end(), address) == seenCopies.end()) {
    seenCopies.push_back(address);
    dk::log("gx-newcopy %u: frame %u after draw %u: 0x%08x %ux%u format %u from %u,%u %ux%u%s efbAlpha %d", newCopies,
            r.frameNumber, r.frameDraws, address, width, height, format, left, top, srcWidth, srcHeight,
            clear ? " CLEAR" : "", r.state.Pixel().efbAlpha);
    if (kSnapshots) {
      reopen(r, false);  // (the snapshot ends a pass; the copy below wants it ended)
      snapshot(r, 60000000u + newCopies);
      dk::end_pass();
    }
    ++newCopies;
  }
  // (from an EFB without alpha the copy is opaque, as GX makes it; an RGBA6
  // pass - a menu's model drawn for a texture - keeps its transparency)
  // the copy filter's taps: the line above (coefficients 0-1), the line
  // itself (2-4) and the line below (5-6), in 64ths; none is the line alone
  float taps[3] = {0.0f, 1.0f, 0.0f};
  if (vfilter) {
    taps[0] = (vfilter[0] + vfilter[1]) / 64.0f;
    taps[1] = (vfilter[2] + vfilter[3] + vfilter[4]) / 64.0f;
    taps[2] = (vfilter[5] + vfilter[6]) / 64.0f;
  }
  const uint32_t lineStep = sy > 1.0f ? static_cast<uint32_t>(sy + 0.5f) : 1u;
  if (clear && format == 5 && left == 0 && top == 0) {
    dk::probe_focus();  // (diagnosis) a Mii's face or icon about to be drawn
  }
  dk::efb_copy(address & 0x1FFFFFFF, width, height, r.efb, r.efbView, r.depthView, source, r.memory, format,
               /*opaque=*/!r.state.Pixel().efbAlpha, taps, lineStep);
  reopen(r, false);
  if (clear) {
    // A copy clears what it copied - its source rectangle - not the whole EFB:
    // a race's bloom copies a corner eight times a frame, and clearing all of it
    // each time wiped the scene the bloom was for (a black flyover).
    // (deko3d's clears keep to the scissor)
    //
    // And only what the update masks let through, as the hardware clears (as
    // Dolphin's ClearScreen has it): colour if colour updates are on, alpha if
    // alpha updates are and the EFB keeps alpha, depth if depth updates are. A
    // race copies its scene for bloom with colour updates off - the copy's
    // clear resets depth and leaves the picture - and clearing the colour too
    // wiped the scene it never redraws (the black flyover).
    DkCmdBuf cmd = dk::commands();
    const DkScissor area{x0, y0, x1 - x0, y1 - y0};
    dkCmdBufSetScissors(cmd, 0, &area, 1);
    const wiinx::gx::PixelState px = r.state.Pixel();
    const uint32_t mask = (px.colorWrite ? DkColorMask_RGB : 0u) | (px.alphaWrite && px.efbAlpha ? DkColorMask_A : 0u);
    if (mask != 0) {
      const auto color = r.state.CopyClearColor();
      dkCmdBufClearColorFloat(cmd, 0, mask, color[0], color[1], color[2], color[3]);
    }
    if (px.depthWrite) {
      dkCmdBufClearDepthStencil(cmd, true, r.state.CopyClearDepth(), 0, 0);
    }
  }
  dk::gx_rebind_all();

  ++r.frameCopies;
}

// ---------- the render thread
//
// The renderer runs on a thread of its own, as Aurora's decoder did. The game
// thread only queues what it writes - FIFO bytes, display-list calls, copies,
// the frame's end - in order, and the render thread does everything else: the
// command processor, GX state, vertex conversion, deko3d. On the game thread
// that work was a third of a race frame. Nothing of the renderer is shared, so
// only the queue is locked.
//
// GXDrawDone waits for the render thread to finish what it was given, as the
// hardware's draw done means the GPU has finished: the game may then rewrite
// what was being read. The frame's end lets one frame be in flight at most.
constexpr bool kThreaded = true;
constexpr bool kDrawDoneWaits = true;

namespace {
struct Packet {
  enum class Kind : uint8_t { Bytes, CallList, CopyTex, DisplaySource, CopyDisplay, EndFrame };
  Kind kind = Kind::Bytes;
  std::vector<uint8_t> bytes;
  uint32_t a = 0, b = 0;  // a call: address, size; a copy: address, format
  uint16_t left = 0, top = 0, srcWidth = 0, srcHeight = 0, width = 0, height = 0;
  bool clear = false, filtered = false;
  uint8_t vfilter[7] = {};
};

struct RenderThread {
  std::mutex lock;
  std::condition_variable wake, idle;
  std::deque<Packet> queue;
  std::vector<std::vector<uint8_t>> spare;  // byte buffers handed back, for reuse
  bool busy = false;
  uint64_t framesQueued = 0, framesDone = 0;
  std::vector<uint8_t> pending;  // (game thread only) bytes not queued yet
  std::once_flag started;
} g_thread;

void run(Packet& p) {
  switch (p.kind) {
  case Packet::Kind::Bytes: write_now(p.bytes.data(), p.bytes.size()); break;
  case Packet::Kind::CallList: call_list_now(p.a, p.b); break;
  case Packet::Kind::CopyTex:
    copy_tex_now(p.a, p.left, p.top, p.srcWidth, p.srcHeight, p.width, p.height, p.clear, p.b,
                 p.filtered ? p.vfilter : nullptr);
    break;
  case Packet::Kind::DisplaySource: set_display_source_now(p.left, p.top, p.srcWidth, p.srcHeight); break;
  case Packet::Kind::CopyDisplay: copy_display_now(p.a, p.clear); break;
  case Packet::Kind::EndFrame: present_now(p.a); break;
  }
}

void loop() {
  SwitchPinCurrentThread(2);  // a core of its own: it is a core's worth of work
  std::unique_lock<std::mutex> held(g_thread.lock);
  for (;;) {
    g_thread.wake.wait(held, [] { return !g_thread.queue.empty(); });
    Packet p = std::move(g_thread.queue.front());
    g_thread.queue.pop_front();
    g_thread.busy = true;
    held.unlock();
    run(p);
    held.lock();
    g_thread.busy = false;
    if (p.kind == Packet::Kind::EndFrame) {
      ++g_thread.framesDone;
    }
    if (p.bytes.capacity() != 0 && g_thread.spare.size() < 16) {
      p.bytes.clear();
      g_thread.spare.push_back(std::move(p.bytes));
    }
    if (g_thread.queue.empty() || p.kind == Packet::Kind::EndFrame) {
      g_thread.idle.notify_all();
    }
  }
}

void push(Packet&& p) {
  // (kept for the run rather than detached: libnx has no pthread_detach)
  std::call_once(g_thread.started, [] { new std::thread(loop); });
  {
    std::lock_guard<std::mutex> held(g_thread.lock);
    g_thread.queue.push_back(std::move(p));
  }
  g_thread.wake.notify_one();
}

// what the game thread wrote since the last packet, as a packet
void push_bytes() {
  if (g_thread.pending.empty()) {
    return;
  }
  Packet p;
  p.kind = Packet::Kind::Bytes;
  p.bytes.swap(g_thread.pending);
  {
    std::lock_guard<std::mutex> held(g_thread.lock);
    if (!g_thread.spare.empty()) {
      g_thread.pending.swap(g_thread.spare.back());
      g_thread.spare.pop_back();
    }
  }
  push(std::move(p));
}

void wait_idle() {
  std::unique_lock<std::mutex> held(g_thread.lock);
  g_thread.idle.wait(held, [] { return g_thread.queue.empty() && !g_thread.busy; });
}
}  // namespace

void write(const uint8_t* data, size_t size) {
  if (!kThreaded) {
    write_now(data, size);
    return;
  }
  g_thread.pending.insert(g_thread.pending.end(), data, data + size);
  if (g_thread.pending.size() >= kRunBatch) {
    push_bytes();
  }
}

void flush() {
  if (!kThreaded) {
    flush_now();
    return;
  }
  push_bytes();
  if (kDrawDoneWaits) {
    wait_idle();
  }
}

void submit() {
  if (!kThreaded) {
    flush_now();
    return;
  }
  push_bytes();
}

void call_list(uint32_t address, uint32_t size) {
  if (!kThreaded) {
    call_list_now(address, size);
    return;
  }
  push_bytes();
  Packet p;
  p.kind = Packet::Kind::CallList;
  p.a = address;
  p.b = size;
  push(std::move(p));
}

void set_display_source(uint16_t left, uint16_t top, uint16_t width, uint16_t height) {
  if (!kThreaded) {
    set_display_source_now(left, top, width, height);
    return;
  }
  push_bytes();
  Packet p;
  p.kind = Packet::Kind::DisplaySource;
  p.left = left;
  p.top = top;
  p.srcWidth = width;
  p.srcHeight = height;
  push(std::move(p));
}

void copy_tex(uint32_t address, uint16_t left, uint16_t top, uint16_t srcWidth, uint16_t srcHeight,
              uint16_t width, uint16_t height, bool clear, uint32_t format, const uint8_t* vfilter) {
  if (!kThreaded) {
    copy_tex_now(address, left, top, srcWidth, srcHeight, width, height, clear, format, vfilter);
    return;
  }
  push_bytes();
  Packet p;
  p.kind = Packet::Kind::CopyTex;
  p.a = address;
  p.b = format;
  p.left = left;
  p.top = top;
  p.srcWidth = srcWidth;
  p.srcHeight = srcHeight;
  p.width = width;
  p.height = height;
  p.clear = clear;
  if (vfilter) {
    p.filtered = true;
    std::memcpy(p.vfilter, vfilter, sizeof(p.vfilter));
  }
  push(std::move(p));
}

void copy_display(uint32_t xfb, bool clear) {
  if (!kThreaded) {
    copy_display_now(xfb, clear);
    return;
  }
  push_bytes();
  Packet p;
  p.kind = Packet::Kind::CopyDisplay;
  p.a = xfb;
  p.clear = clear;
  push(std::move(p));
}

void end_frame(uint32_t xfb) {
  if (!kThreaded) {
    present_now(xfb);
    return;
  }
  push_bytes();
  Packet p;
  p.kind = Packet::Kind::EndFrame;
  p.a = xfb;
  push(std::move(p));
  // one frame in flight at most: the game draws the next while this one renders
  std::unique_lock<std::mutex> held(g_thread.lock);
  ++g_thread.framesQueued;
  g_thread.idle.wait(held, [] { return g_thread.framesDone + 1 >= g_thread.framesQueued; });
}

wiinx::gx::State& state() { return renderer().state; }

Counts take_counts() {
  Renderer& r = renderer();
  const Counts c = r.counts;
  r.counts = {};
  return c;
}

} // namespace dol::gx_native
