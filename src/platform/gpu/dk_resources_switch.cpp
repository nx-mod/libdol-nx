// The GPU on deko3d: buffers, textures and views, and writes into them
// (dk.h).
//
// Each buffer and texture has a memory block of its own: a renderer makes its big
// buffers once and its textures as a game loads them. A pool can come if a
// measurement says so.
#include "dk.h"

#include <cstring>

namespace dol::dk {
namespace {


DkMemBlock memory_block(uint32_t size, uint32_t flags) {
  DkMemBlockMaker maker;

  dkMemBlockMakerDefaults(&maker, device(), align_up(size ? size : 1, DK_MEMBLOCK_ALIGNMENT));
  maker.flags = flags;
  return dkMemBlockCreate(&maker);
}

// the bytes one row of `width` texels takes, and the rows a `height` makes,
// for a format's blocks (compressed formats are 4x4 blocks)
void row_layout(DkImageFormat format, uint32_t width, uint32_t height, uint32_t* row_bytes, uint32_t* rows) {
  uint32_t block = 1, bytes = 4;

  switch (format) {
  case DkImageFormat_R8_Unorm: case DkImageFormat_R8_Snorm: case DkImageFormat_R8_Uint: case DkImageFormat_R8_Sint:
  case DkImageFormat_S8:
    bytes = 1; break;
  case DkImageFormat_RG8_Unorm: case DkImageFormat_RG8_Snorm: case DkImageFormat_RG8_Uint: case DkImageFormat_RG8_Sint:
  case DkImageFormat_R16_Float: case DkImageFormat_R16_Unorm: case DkImageFormat_R16_Uint: case DkImageFormat_R16_Sint:
  case DkImageFormat_Z16: case DkImageFormat_RGB565_Unorm: case DkImageFormat_RGB5A1_Unorm:
  case DkImageFormat_RGBA4_Unorm:
    bytes = 2; break;
  case DkImageFormat_RGBA16_Float: case DkImageFormat_RGBA16_Unorm: case DkImageFormat_RGBA16_Uint:
  case DkImageFormat_RGBA16_Sint: case DkImageFormat_RG32_Float: case DkImageFormat_RG32_Uint:
  case DkImageFormat_RG32_Sint:
    bytes = 8; break;
  case DkImageFormat_RGBA32_Float: case DkImageFormat_RGBA32_Uint: case DkImageFormat_RGBA32_Sint:
    bytes = 16; break;
  case DkImageFormat_RGBA_BC1: case DkImageFormat_RGBA_BC1_sRGB: case DkImageFormat_RGB_BC1:
  case DkImageFormat_R_BC4_Unorm: case DkImageFormat_R_BC4_Snorm:
    block = 4; bytes = 8; break;
  case DkImageFormat_RGBA_BC2: case DkImageFormat_RGBA_BC2_sRGB: case DkImageFormat_RGBA_BC3:
  case DkImageFormat_RGBA_BC3_sRGB: case DkImageFormat_RG_BC5_Unorm: case DkImageFormat_RG_BC5_Snorm:
  case DkImageFormat_RGBA_BC7_Unorm: case DkImageFormat_RGBA_BC7_Unorm_sRGB:
    block = 4; bytes = 16; break;
  default:
    break;
  }
  *row_bytes = (width + block - 1) / block * bytes;
  *rows = (height + block - 1) / block;
}

} // namespace

// ---------- buffers

Buffer create_buffer(uint32_t size, bool readback) {
  Buffer buffer;

  buffer.size = size;
  buffer.block = memory_block(size, readback ? DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached
                                             : DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  if (!buffer.block)
    log("no memory for a %u byte buffer", size);
  return buffer;
}

void destroy(Buffer& buffer) {
  if (buffer.block) {
    // (the GPU may still read it)
    wait_idle();
    dkMemBlockDestroy(buffer.block);
  }
  buffer = Buffer{};
}

void write_buffer(const Buffer& buffer, uint32_t offset, const void* data, uint32_t size) {
  Allocation staging = stage(size, 16);

  if (!staging) {
    // the frame's staging is full: the GPU is let finish and the bytes go in
    wait_idle();
    std::memcpy(static_cast<uint8_t*>(buffer.cpu()) + offset, data, size);
    return;
  }
  std::memcpy(staging.cpu(), data, size);
  dkCmdBufCopyBuffer(transfer(), staging.gpu(), buffer.gpu() + offset, size);
  submit_transfer();
}

// ---------- textures

Texture create_texture(DkImageFormat format, uint32_t width, uint32_t height, uint32_t layers, uint32_t mip_levels,
                       uint32_t samples, uint32_t flags, DkImageType type) {
  Texture texture;
  DkImageLayoutMaker maker;
  DkImageLayout layout;

  texture.format = format;
  texture.width = width;
  texture.height = height;
  texture.layers = layers ? layers : 1;
  texture.mip_levels = mip_levels ? mip_levels : 1;
  texture.samples = samples ? samples : 1;
  dkImageLayoutMakerDefaults(&maker, device());
  // (copies and blits are the 2D engine's)
  maker.flags = DkImageFlags_Usage2DEngine;
  if (flags & TextureRender)
    maker.flags |= DkImageFlags_UsageRender | DkImageFlags_HwCompression;
  if (flags & TextureStorage)
    maker.flags |= DkImageFlags_UsageLoadStore;
  maker.format = format;
  maker.dimensions[0] = width;
  maker.dimensions[1] = height;
  maker.mipLevels = texture.mip_levels;
  if (type == DkImageType_3D || type == DkImageType_2DArray || type == DkImageType_Cubemap) {
    maker.dimensions[2] = texture.layers;
  } else if (texture.layers > 1) {
    type = DkImageType_2DArray;
    maker.dimensions[2] = texture.layers;
  } else if (texture.samples > 1) {
    type = DkImageType_2DMS;
    maker.msMode = texture.samples >= 8 ? DkMsMode_8x : texture.samples >= 4 ? DkMsMode_4x : DkMsMode_2x;
  }
  maker.type = type;
  texture.type = type;
  dkImageLayoutInitialize(&layout, &maker);
  texture.block = memory_block(static_cast<uint32_t>(dkImageLayoutGetSize(&layout)),
                               DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
  if (!texture.block) {
    log("no memory for a %ux%u texture", width, height);
    return Texture{};
  }
  dkImageInitialize(&texture.image, &layout, texture.block, 0);
  return texture;
}

void destroy(Texture& texture) {
  if (texture.block) {
    wait_idle();
    dkMemBlockDestroy(texture.block);
  }
  texture = Texture{};
}

void write_texture(const Texture& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t layer, uint32_t width,
                   uint32_t height, const void* data, uint32_t size, uint32_t bytes_per_row) {
  Allocation staging = stage(size, 256);
  DkImageView view;
  DkImageRect rect = {x, y, layer, width, height, 1};
  DkCopyBuf copy{};
  uint32_t row_bytes, rows;

  if (!staging) {
    log("a frame wrote more than its staging memory holds; a texture write is dropped");
    return;
  }
  std::memcpy(staging.cpu(), data, size);
  row_layout(texture.format, width, height, &row_bytes, &rows);
  dkImageViewDefaults(&view, &texture.image);
  view.mipLevelOffset = static_cast<uint8_t>(mip);
  view.mipLevelCount = 1;
  copy.addr = staging.gpu();
  copy.rowLength = bytes_per_row ? bytes_per_row : row_bytes;
  copy.imageHeight = rows;
  dkCmdBufCopyBufferToImage(transfer(), &copy, &view, &rect, 0);
  submit_transfer();
}

TextureView make_view(const Texture& texture, uint32_t base_mip, uint32_t mip_count, uint32_t base_layer,
                      uint32_t layer_count, DkImageFormat format) {
  TextureView view;

  view.texture = &texture;
  dkImageViewDefaults(&view.view, &texture.image);
  if (format != DkImageFormat_None)
    view.view.format = format;
  view.view.mipLevelOffset = static_cast<uint8_t>(base_mip);
  view.view.mipLevelCount = static_cast<uint8_t>(mip_count ? mip_count : texture.mip_levels - base_mip);
  view.view.layerOffset = static_cast<uint16_t>(base_layer);
  view.view.layerCount = static_cast<uint16_t>(layer_count ? layer_count : texture.layers - base_layer);
  if (layer_count == 1 && texture.layers > 1)
    view.view.type = DkImageType_2D;
  return view;
}

} // namespace dol::dk
