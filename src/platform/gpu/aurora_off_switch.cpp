// What libdol still asked of Aurora, once it draws GX itself (WIINX_NATIVE_GX).
//
// With libdol's own renderer the program links no Aurora and no Dawn. A few
// calls into them remain in code shared with the Aurora build: its application
// layer (start, events, shut down), GX functions that only kept Aurora's copy
// of the state, its VI helpers, and the counters its renderer kept. Here they
// are, each doing what it means natively - mostly nothing, since the guest's
// own SDK code now sets the hardware's state and libdol's renderer reads it.
#if defined(WIINX_NATIVE_GX)

#include "dk.h"
#include "gx_native.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>

#include <switch.h>

#include <atomic>
#include <cstdint>

// ---------- Aurora's application layer

extern "C" {

AuroraInfo aurora_initialize(int, char*[], const AuroraConfig* config) {
  AuroraInfo info{};
  info.backend = BACKEND_NULL;  // (not Aurora's: deko3d, from GXInit's first frame)
  info.userPath = config ? config->userPath : nullptr;
  info.cachePath = config ? config->cachePath : nullptr;
  uint32_t width = 1280, height = 720;
  dol::dk::screen_size(&width, &height);
  info.windowSize.width = info.windowSize.fb_width = info.windowSize.native_fb_width = width;
  info.windowSize.height = info.windowSize.fb_height = info.windowSize.native_fb_height = height;
  info.windowSize.scale = 1.0f;
  return info;
}

// The console's own events: HOME and focus are the applet's (appletMainLoop),
// and leaving is AURORA_EXIT. The controllers are libdol's native input.
const AuroraEvent* aurora_update() {
  static AuroraEvent events[2];
  events[0] = AuroraEvent{};
  events[1] = AuroraEvent{};
  events[0].type = appletMainLoop() ? AURORA_NONE : AURORA_EXIT;
  return events;
}

void aurora_shutdown() { dol::dk::shutdown(); }

AuroraDisplayMode aurora_get_display_mode() { return AURORA_DISPLAY_MODE_EXCLUSIVE; }
void aurora_set_display_mode(AuroraDisplayMode) {}
void aurora_set_threaded_gx(bool) {}
void aurora_set_skip_unready_pipelines(bool) {}
void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback, AuroraGuestWriteNotifyCallback) {}
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback) {}
void aurora_set_frame_interpolation_fps(uint32_t) {}
void aurora_set_disable_copy_filter(bool) {}
void aurora_gx_sync_sites(char* out, size_t size) {
  if (out && size) {
    out[0] = '\0';
  }
}

// ---------- the counters Aurora's renderer kept (read by libdol's profiling)

uint64_t g_auroraRegWrites[3] = {};
uint64_t g_auroraProcessCalls = 0;
uint64_t g_auroraProcessBytes = 0;
uint64_t g_auroraDrawTicks[6] = {};
uint32_t g_auroraDrawCount = 0;
uint64_t g_auroraStorageBytes = 0;
uint64_t g_gxSyncWaitNs = 0;
uint32_t g_gxSyncCount = 0;
std::atomic<const char*> g_gxDecodePhase{nullptr};
std::atomic<uint32_t> g_gxDecodeQueued{0};
std::atomic<uint32_t> g_gxDecodeThreadHandle{0};

// ---------- GX: Aurora's copy of the state, which nothing reads natively

void GXSetCPUFifo(GXFifoObj*) {}
void GXSetGPFifo(GXFifoObj*) {}
void GXSaveCPUFifo(GXFifoObj*) {}
GXFifoObj* GXGetCPUFifo(void) { return nullptr; }
void GXFlush(void) { dol::gx_native::flush(); }
void GXPixModeSync(void) {}
void GXSetTexCopySrc(u16, u16, u16, u16) {}
void GXSetTexCopyDst(u16, u16, GXTexFmt, GXBool) {}
void GXSetDispCopySrc(u16, u16, u16, u16) {}
void GXSetDispCopyDst(u16, u16) {}
void GXSetDispCopyGamma(GXGamma) {}
void GXSetCopyFilter(GXBool, u8[12][2], GXBool, u8[7]) {}
void GXDestroyTexObj(GXTexObj*) {}
void GXDestroyTlutObj(GXTlutObj*) {}
void GXDestroyCopyTex(void*) {}

// The lines a display copy of this vertical scale makes, as the SDK works it
// out: the EFB's height over the scale, in its 8.8 fixed point.
u32 GXSetDispCopyYScale(f32 vscale) {
  if (vscale <= 0.0f) {
    return 0;
  }
  const u32 scale = static_cast<u32>(256.0f / vscale) & 0x1FF;
  return scale ? (528u * 256u + scale - 1) / scale : 0;
}

// The bytes a texture of this format takes in memory, with its mip levels: the
// SDK's tile sizes (8x8 for 4-bit formats, 8x4 for 8-bit, 4x4 for the rest,
// 64 bytes a 4x4 tile for RGBA8 and for 32 bytes otherwise).
u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, GXBool mipmap, u8 maxLod) {
  u32 tileW = 4, tileH = 4, tileBytes = 32;
  switch (format) {
  case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: case GX_CTF_R4:
    tileW = 8, tileH = 8;
    break;
  case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: case GX_CTF_RA4: case GX_CTF_A8: case GX_CTF_R8:
  case GX_CTF_G8: case GX_CTF_B8: case GX_TF_Z8: case GX_CTF_Z8M: case GX_CTF_Z8L:
    tileW = 8, tileH = 4;
    break;
  case GX_TF_RGBA8: case GX_TF_Z24X8:
    tileBytes = 64;
    break;
  default:
    break;
  }
  const u32 levels = mipmap ? (maxLod ? maxLod : 1) : 1;
  u32 total = 0, w = width, h = height;
  for (u32 level = 0; level < levels; ++level) {
    total += ((w + tileW - 1) / tileW) * ((h + tileH - 1) / tileH) * tileBytes;
    if (w == 1 && h == 1) {
      break;
    }
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }
  return total;
}

}  // extern "C"

// ---------- VI and the viewport: how Aurora scaled its own output

void VISetFrameBufferScale(float) {}
void VILockAspectRatio(int, int) {}
void VIUnlockAspectRatio() {}
void VIConfigure(const GXRenderModeObj*) {}
void AuroraSetViewportPolicy(AuroraViewportPolicy) {}

#endif  // WIINX_NATIVE_GX
