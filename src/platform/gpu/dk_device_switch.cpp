// The GPU on deko3d: the device, its queue and the frame's memory (dk.h).
#include "dk.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

void SwitchBootLogExternal(const char* text) noexcept;

namespace dol::dk {
namespace {

// command memory a frame records into, and staging its writes copy from
constexpr uint32_t kCommandSlice = 4u * 1024 * 1024;
constexpr uint32_t kStagingSlice = 16u * 1024 * 1024;


// memory a frame writes as it goes: kFrames slices, the next taken at each
// frame's end once its fence has passed
struct FrameRing {
  DkMemBlock block = nullptr;
  uint32_t slice = 0;
  uint32_t used = 0;

  // `base` is the slice this frame writes (frame_end sets it): the end is that
  // slice's, never worked out from `used`, which at exactly a slice's end named
  // the next slice and let an allocation run into it - in the last slice, past
  // the block altogether.
  uint32_t base = 0;

  Allocation take(uint32_t size, uint32_t alignment) {
    uint32_t end = base + slice;
    uint32_t offset = align_up(used, alignment);

    if (offset + size > end)
      return {};
    used = offset + size;
    return {block, offset, size};
  }
};

struct State {
  DkDevice device = nullptr;
  DkQueue queue = nullptr;
  // the frame's commands, and the writes' own (recorded and submitted at once,
  // so they land before what is submitted after them and never split the
  // frame's recording)
  DkCmdBuf commands = nullptr;
  DkCmdBuf transfer = nullptr;
  // only ever captures (dkCmdBufBeginCaptureCmds clears the buffer it is
  // given, so capturing on `commands` threw away the frame recorded so far)
  DkCmdBuf capture = nullptr;
  FrameRing command_memory;
  FrameRing staging;
  uint32_t frame = 0;
  DkFence fences[kFrames] = {};
  bool fence_pending[kFrames] = {};
  DkMemBlock descriptors = nullptr;
  bool descriptors_bound = false;
  // What was destroyed while a frame was being recorded, freed once that
  // frame's fence has signalled: draws already recorded may still use it (a
  // texture replaced mid-frame was freed under them, the GPU faulted, and the
  // next acquire aborted), and a descriptor slot reused at once rewrote what
  // those draws read.
  std::vector<DkMemBlock> retired_blocks[kFrames];
  std::vector<uint32_t> retired_images[kFrames], retired_samplers[kFrames];
} g;

bool ring_make(FrameRing& ring, uint32_t size) {
  DkMemBlockMaker maker;

  ring.slice = align_up(size, DK_MEMBLOCK_ALIGNMENT);
  dkMemBlockMakerDefaults(&maker, g.device, ring.slice * kFrames);
  maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  ring.block = dkMemBlockCreate(&maker);
  ring.used = 0;
  return ring.block != nullptr;
}

// the command buffers are given memory a piece at a time from the frame's
// slice; a slice that fills is a frame too big for it

void command_memory_needed(void*, DkCmdBuf commands, size_t minimum) {
  uint32_t size = align_up(minimum < 64 * 1024 ? 64 * 1024 : static_cast<uint32_t>(minimum), DK_CMDMEM_ALIGNMENT);
  Allocation piece = g.command_memory.take(size, DK_CMDMEM_ALIGNMENT);

  if (!piece)
    fatal("a frame's commands are more than %u KB", kCommandSlice / 1024);
  dkCmdBufAddMemory(commands, piece.block, piece.offset, piece.size);
}

void debug_callback(void*, const char* context, DkResult result, const char* message) {
  log("deko3d %s: %s (%d)", context, message, static_cast<int>(result));
}

} // namespace

void log(const char* format, ...) {
  va_list arguments;
  char line[512];

  va_start(arguments, format);
  std::vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  std::fprintf(stderr, "[deko3d] %s\n", line);
  // and in boot.log, which a crash folder keeps when console.log is not
  // written: the renderer's per-second counts are what a failed run is read by
  char tagged[528];
  std::snprintf(tagged, sizeof(tagged), "[deko3d] %s", line);
  SwitchBootLogExternal(tagged);
}

void fatal(const char* format, ...) {
  va_list arguments;

  va_start(arguments, format);
  std::fputs("[deko3d] fatal: ", stderr);
  std::vfprintf(stderr, format, arguments);
  std::fputc('\n', stderr);
  va_end(arguments);
  std::abort();
}

bool initialize() {
  DkDeviceMaker device_maker;
  DkQueueMaker queue_maker;
  DkCmdBufMaker command_maker;
  DkMemBlockMaker descriptor_maker;

  dkDeviceMakerDefaults(&device_maker);
  device_maker.cbDebug = debug_callback;
  // depth from 0 to 1, the origin top left
  device_maker.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft;
  g.device = dkDeviceCreate(&device_maker);
  if (!g.device)
    return false;
  dkQueueMakerDefaults(&queue_maker, g.device);
  queue_maker.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute;
  // room for a whole frame's work items before the queue has to flush
  queue_maker.commandMemorySize = 1024 * 1024;
  queue_maker.flushThreshold = 128 * 1024;
  g.queue = dkQueueCreate(&queue_maker);
  if (!g.queue || !ring_make(g.command_memory, kCommandSlice) || !ring_make(g.staging, kStagingSlice))
    return false;
  dkCmdBufMakerDefaults(&command_maker, g.device);
  command_maker.cbAddMem = command_memory_needed;
  g.commands = dkCmdBufCreate(&command_maker);
  g.transfer = dkCmdBufCreate(&command_maker);
  g.capture = dkCmdBufCreate(&command_maker);
  dkMemBlockMakerDefaults(&descriptor_maker, g.device,
                          align_up(kImageSlots * sizeof(DkImageDescriptor) + kSamplerSlots * sizeof(DkSamplerDescriptor),
                                   DK_MEMBLOCK_ALIGNMENT));
  descriptor_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  g.descriptors = dkMemBlockCreate(&descriptor_maker);
  if (!g.commands || !g.transfer || !g.descriptors)
    return false;
  log("device ready");
  return true;
}

void shutdown() {
  if (g.queue)
    dkQueueWaitIdle(g.queue);
  // (every memory block before the device: a program the homebrew loader
  // runs shares its process, and a block left mapped - the shaders' code
  // above all - crashes the loader when it takes over again)
  shutdown_screen();
  shutdown_gx();
  shutdown_shaders();
  for (uint32_t f = 0; f < kFrames; ++f)
    release_retired(f);  // the queue is idle: everything retired can go
  if (g.commands)
    dkCmdBufDestroy(g.commands);
  if (g.transfer)
    dkCmdBufDestroy(g.transfer);
  if (g.capture)
    dkCmdBufDestroy(g.capture);
  if (g.descriptors)
    dkMemBlockDestroy(g.descriptors);
  if (g.staging.block)
    dkMemBlockDestroy(g.staging.block);
  if (g.command_memory.block)
    dkMemBlockDestroy(g.command_memory.block);
  if (g.queue)
    dkQueueDestroy(g.queue);
  if (g.device)
    dkDeviceDestroy(g.device);
  g = State{};
}

DkDevice device() { return g.device; }
DkQueue queue() { return g.queue; }
DkCmdBuf commands() { return g.commands; }

void submit() {
  dkQueueSubmitCommands(g.queue, dkCmdBufFinishList(g.commands));
  dkQueueFlush(g.queue);
}

void submit_list() { dkQueueSubmitCommands(g.queue, dkCmdBufFinishList(g.commands)); }

Allocation stage(uint32_t size, uint32_t alignment) { return g.staging.take(size, alignment); }

DkCmdBuf transfer() { return g.transfer; }

DkCmdBuf capture() { return g.capture; }

void submit_transfer() { dkQueueSubmitCommands(g.queue, dkCmdBufFinishList(g.transfer)); }

void frame_end() {
  submit();
  dkQueueSignalFence(g.queue, &g.fences[g.frame], true);
  dkQueueFlush(g.queue);
  // (the command buffers move to the next frame's slice below)
  g.fence_pending[g.frame] = true;
  g.frame = (g.frame + 1) % kFrames;
  if (g.fence_pending[g.frame]) {
    dkFenceWait(&g.fences[g.frame], -1);
    g.fence_pending[g.frame] = false;
  }
  release_retired(g.frame);  // that frame's work is done: what it used can go
  g.command_memory.used = g.command_memory.base = g.frame * g.command_memory.slice;
  // The next frame records into its own slice. Left alone, a command buffer
  // goes on writing into the piece it was last given - this frame's, which
  // the GPU has not run yet - and dkCmdBufClear does not change that: it only
  // rewinds to that piece's start. So each is cleared and given a fresh piece
  // of the new slice at once.
  for (DkCmdBuf commands : {g.commands, g.transfer}) {
    dkCmdBufClear(commands);
    command_memory_needed(nullptr, commands, 0);
  }
  g.staging.used = g.staging.base = g.frame * g.staging.slice;
  // (bound again at the frame's first draw: the queue's state is not kept)
  g.descriptors_bound = false;
}

void wait_idle() { dkQueueWaitIdle(g.queue); }

// ---------- the descriptor sets

namespace {
std::vector<uint32_t> free_images, free_samplers;
uint32_t images_used = 0, samplers_used = 0;

int32_t slot_take(std::vector<uint32_t>& free_slots, uint32_t& used, uint32_t limit) {
  if (!free_slots.empty()) {
    uint32_t slot = free_slots.back();
    free_slots.pop_back();
    return static_cast<int32_t>(slot);
  }
  if (used == limit) {
    log("more than %u descriptors of a kind", limit);
    return -1;
  }
  return static_cast<int32_t>(used++);
}

DkImageDescriptor* image_descriptors() { return static_cast<DkImageDescriptor*>(dkMemBlockGetCpuAddr(g.descriptors)); }
DkSamplerDescriptor* sampler_descriptors() {
  return reinterpret_cast<DkSamplerDescriptor*>(image_descriptors() + kImageSlots);
}
} // namespace

int32_t image_slot(TextureView& view) {
  if (view.slot < 0) {
    view.slot = slot_take(free_images, images_used, kImageSlots);
    if (view.slot >= 0) {
      dkImageDescriptorInitialize(&image_descriptors()[view.slot], &view.view, false, false);
      dkCmdBufBarrier(g.commands, DkBarrier_None, DkInvalidateFlags_Descriptors);
    }
  }
  return view.slot;
}

int32_t sampler_slot(Sampler& sampler) {
  if (sampler.slot < 0) {
    sampler.slot = slot_take(free_samplers, samplers_used, kSamplerSlots);
    if (sampler.slot >= 0) {
      dkSamplerDescriptorInitialize(&sampler_descriptors()[sampler.slot], &sampler.sampler);
      dkCmdBufBarrier(g.commands, DkBarrier_None, DkInvalidateFlags_Descriptors);
    }
  }
  return sampler.slot;
}

void destroy(TextureView& view) {
  if (view.slot >= 0)
    g.retired_images[g.frame].push_back(static_cast<uint32_t>(view.slot));
  view = TextureView{};
}

void destroy(Sampler& sampler) {
  if (sampler.slot >= 0)
    g.retired_samplers[g.frame].push_back(static_cast<uint32_t>(sampler.slot));
  sampler = Sampler{};
}

void retire(DkMemBlock block) {
  if (block)
    g.retired_blocks[g.frame].push_back(block);
}

void release_retired(uint32_t frame) {
  for (DkMemBlock block : g.retired_blocks[frame])
    dkMemBlockDestroy(block);
  g.retired_blocks[frame].clear();
  free_images.insert(free_images.end(), g.retired_images[frame].begin(), g.retired_images[frame].end());
  g.retired_images[frame].clear();
  free_samplers.insert(free_samplers.end(), g.retired_samplers[frame].begin(), g.retired_samplers[frame].end());
  g.retired_samplers[frame].clear();
}

void bind_descriptor_sets() {
  if (g.descriptors_bound)
    return;
  DkGpuAddr base = dkMemBlockGetGpuAddr(g.descriptors);
  dkCmdBufBindImageDescriptorSet(g.commands, base, kImageSlots);
  dkCmdBufBindSamplerDescriptorSet(g.commands, base + kImageSlots * sizeof(DkImageDescriptor), kSamplerSlots);
  g.descriptors_bound = true;
}

} // namespace dol::dk
