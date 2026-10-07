// The GPU on deko3d: passes, copies and the screen (dk.h).
#include "dk.h"

namespace dol::dk {
namespace {

// the screen's images are the docked size; each frame shows as much of them
// as the console's screen is (dkSwapchainSetCrop), as Halo's port does
constexpr uint32_t kScreenWidth = 1920;
constexpr uint32_t kScreenHeight = 1080;
constexpr int kScreenImages = 3;

struct Screen {
  DkSwapchain swapchain = nullptr;
  DkMemBlock memory = nullptr;
  Texture images[kScreenImages];
  uint32_t width = 0, height = 0;
  int acquired = -1;
} s;

DkImageView whole(const Texture& texture, uint32_t mip) {
  DkImageView view;

  dkImageViewDefaults(&view, &texture.image);
  view.mipLevelOffset = static_cast<uint8_t>(mip);
  view.mipLevelCount = 1;
  return view;
}

} // namespace

// ---------- passes

void begin_pass(ColorTarget* colors, uint32_t color_count, DepthTarget* depth, uint32_t* width, uint32_t* height) {
  DkImageView const* views[8];
  DkCmdBuf cmd = commands();
  uint32_t w = 0, h = 0;

  for (uint32_t index = 0; index < color_count && index < 8; index++) {
    TextureView* view = colors[index].view;

    views[index] = &view->view;
    w = view->texture->width >> view->view.mipLevelOffset;
    h = view->texture->height >> view->view.mipLevelOffset;
  }
  if (depth && depth->view && !color_count) {
    w = depth->view->texture->width >> depth->view->view.mipLevelOffset;
    h = depth->view->texture->height >> depth->view->view.mipLevelOffset;
  }
  dkCmdBufBindRenderTargets(cmd, views, color_count, depth && depth->view ? &depth->view->view : nullptr);
  DkViewport viewport = {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
  DkScissor scissor = {0, 0, w, h};
  dkCmdBufSetViewports(cmd, 0, &viewport, 1);
  dkCmdBufSetScissors(cmd, 0, &scissor, 1);
  for (uint32_t index = 0; index < color_count && index < 8; index++) {
    if (colors[index].clear)
      dkCmdBufClearColorFloat(cmd, index, DkColorMask_RGBA, colors[index].clear_color[0], colors[index].clear_color[1],
                              colors[index].clear_color[2], colors[index].clear_color[3]);
  }
  if (depth && depth->view && (depth->clear_depth || depth->clear_stencil))
    dkCmdBufClearDepthStencil(cmd, depth->clear_depth, depth->depth, depth->clear_stencil ? 0xff : 0, depth->stencil);
  if (width)
    *width = w;
  if (height)
    *height = h;
}

void end_pass() { dkCmdBufBarrier(commands(), DkBarrier_Fragments, DkInvalidateFlags_Image); }

// ---------- copies

void copy_buffer(const Buffer& from, uint32_t from_offset, const Buffer& to, uint32_t to_offset, uint32_t size) {
  dkCmdBufCopyBuffer(commands(), from.gpu() + from_offset, to.gpu() + to_offset, size);
}

void copy_texture(const Texture& from, uint32_t from_mip, const DkImageRect& from_rect, const Texture& to,
                  uint32_t to_mip, const DkImageRect& to_rect, bool filter) {
  DkImageView source = whole(from, from_mip), destination = whole(to, to_mip);

  if (from_rect.width == to_rect.width && from_rect.height == to_rect.height && from.format == to.format)
    dkCmdBufCopyImage(commands(), &source, &from_rect, &destination, &to_rect, 0);
  else
    dkCmdBufBlitImage(commands(), &source, &from_rect, &destination, &to_rect,
                      filter ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest, 0);
}

void copy_texture_to_buffer(const Texture& from, uint32_t mip, const DkImageRect& rect, const Buffer& to,
                            uint32_t offset, uint32_t bytes_per_row) {
  DkImageView view = whole(from, mip);
  DkCopyBuf copy{};

  copy.addr = to.gpu() + offset;
  copy.rowLength = bytes_per_row;
  copy.imageHeight = rect.height;
  dkCmdBufCopyImageToBuffer(commands(), &view, &rect, &copy, 0);
}

// ---------- the screen

void screen_size(uint32_t* width, uint32_t* height) {
  bool docked = appletGetOperationMode() == AppletOperationMode_Console;

  *width = docked ? 1920 : 1280;
  *height = docked ? 1080 : 720;
}

bool initialize_screen(DkImageFormat format) {
  DkImageLayoutMaker maker;
  DkImageLayout layout;
  DkMemBlockMaker block_maker;
  DkSwapchainMaker swapchain_maker;
  DkImage const* images[kScreenImages];
  uint32_t size, alignment;

  dkImageLayoutMakerDefaults(&maker, device());
  maker.flags =
      DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression;
  maker.format = format;
  maker.dimensions[0] = kScreenWidth;
  maker.dimensions[1] = kScreenHeight;
  dkImageLayoutInitialize(&layout, &maker);
  alignment = dkImageLayoutGetAlignment(&layout);
  size = static_cast<uint32_t>((dkImageLayoutGetSize(&layout) + alignment - 1) & ~uint64_t(alignment - 1));
  dkMemBlockMakerDefaults(&block_maker, device(),
                          (kScreenImages * size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1));
  block_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  s.memory = dkMemBlockCreate(&block_maker);
  if (!s.memory)
    return false;
  for (int index = 0; index < kScreenImages; index++) {
    Texture& texture = s.images[index];

    texture.format = format;
    texture.width = kScreenWidth;
    texture.height = kScreenHeight;
    dkImageInitialize(&texture.image, &layout, s.memory, static_cast<uint32_t>(index) * size);
    images[index] = &texture.image;
  }
  dkSwapchainMakerDefaults(&swapchain_maker, device(), nwindowGetDefault(), images, kScreenImages);
  s.swapchain = dkSwapchainCreate(&swapchain_maker);
  log("presenting from %ux%u images", kScreenWidth, kScreenHeight);
  return s.swapchain != nullptr;
}

Texture& acquire_screen() {
  uint32_t width, height;

  screen_size(&width, &height);
  if (width != s.width || height != s.height) {
    s.width = width;
    s.height = height;
    dkSwapchainSetCrop(s.swapchain, 0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height));
  }
  s.acquired = dkQueueAcquireImage(queue(), s.swapchain);
  Texture& image = s.images[s.acquired];
  // (as big as the screen, as far as anything drawing into it knows)
  image.width = width;
  image.height = height;
  return image;
}

void shutdown_screen() {
  if (!s.swapchain) {
    return;
  }
  // (the swapchain before the device and its memory: destroyed the other way
  // round, the program does not close)
  dkQueueWaitIdle(queue());
  dkSwapchainDestroy(s.swapchain);
  if (s.memory)
    dkMemBlockDestroy(s.memory);
  s = Screen{};
}

void present() {
  submit();
  if (s.acquired >= 0)
    dkQueuePresentImage(queue(), s.swapchain, s.acquired);
  s.acquired = -1;
  frame_end();
}

} // namespace dol::dk
