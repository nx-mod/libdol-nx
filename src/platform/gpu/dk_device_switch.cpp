// The GPU on deko3d: the device, its queue and the frame's memory (dk.h).
#include "dk.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace dol::dk {
namespace {

// command memory a frame records into, and staging its writes copy from
constexpr uint32_t kCommandSlice = 4u * 1024 * 1024;
constexpr uint32_t kStagingSlice = 16u * 1024 * 1024;

uint32_t align_up(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

// memory a frame writes as it goes: kFrames slices, the next taken at each
// frame's end once its fence has passed
struct FrameRing {
  DkMemBlock block = nullptr;
  uint32_t slice = 0;
  uint32_t used = 0;

  Allocation take(uint32_t size, uint32_t alignment) {
    uint32_t end = (used / slice + 1) * slice;
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
  FrameRing command_memory;
  FrameRing staging;
  uint32_t frame = 0;
  DkFence fences[kFrames] = {};
  bool fence_pending[kFrames] = {};
  DkMemBlock descriptors = nullptr;
  bool descriptors_bound = false;
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

  va_start(arguments, format);
  std::fputs("[deko3d] ", stderr);
  std::vfprintf(stderr, format, arguments);
  std::fputc('\n', stderr);
  va_end(arguments);
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
  g.queue = dkQueueCreate(&queue_maker);
  if (!g.queue || !ring_make(g.command_memory, kCommandSlice) || !ring_make(g.staging, kStagingSlice))
    return false;
  dkCmdBufMakerDefaults(&command_maker, g.device);
  command_maker.cbAddMem = command_memory_needed;
  g.commands = dkCmdBufCreate(&command_maker);
  g.transfer = dkCmdBufCreate(&command_maker);
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
  if (g.commands)
    dkCmdBufDestroy(g.commands);
  if (g.transfer)
    dkCmdBufDestroy(g.transfer);
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

Allocation stage(uint32_t size, uint32_t alignment) { return g.staging.take(size, alignment); }

DkCmdBuf transfer() { return g.transfer; }

void submit_transfer() { dkQueueSubmitCommands(g.queue, dkCmdBufFinishList(g.transfer)); }

void frame_end() {
  submit();
  dkQueueSignalFence(g.queue, &g.fences[g.frame], true);
  dkQueueFlush(g.queue);
  g.fence_pending[g.frame] = true;
  g.frame = (g.frame + 1) % kFrames;
  if (g.fence_pending[g.frame]) {
    dkFenceWait(&g.fences[g.frame], -1);
    g.fence_pending[g.frame] = false;
  }
  g.command_memory.used = g.frame * g.command_memory.slice;
  g.staging.used = g.frame * g.staging.slice;
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
    free_images.push_back(static_cast<uint32_t>(view.slot));
  view = TextureView{};
}

void destroy(Sampler& sampler) {
  if (sampler.slot >= 0)
    free_samplers.push_back(static_cast<uint32_t>(sampler.slot));
  sampler = Sampler{};
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
