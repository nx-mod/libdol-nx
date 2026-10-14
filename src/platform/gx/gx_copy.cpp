#if defined(WIINX_NATIVE_GX)
#include "../gpu/gx_native.h"
#endif
#include <cstdio>
#include <cstring>
// gx_copy.cpp - Framebuffer Copy Operations
#include <aurora/gfx.h>
#include "gx_internal.h"

#if defined(__SWITCH__)
#include <atomic>
// Last host call the main thread entered that can block on the GPU/Aurora;
// read by the Switch freeze watchdog in main.cpp.
extern std::atomic<const char*> g_switchHostPhase;
#define SWITCH_PHASE(name) g_switchHostPhase.store(name, std::memory_order_relaxed)
#else
#define SWITCH_PHASE(name) ((void)0)
#endif

#include "settings_overlay.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace {
// Copy destinations stay GPU-only until an explicit/lazy readback. Track the
// guest ranges that own those results so a later data-cache flush over a reused
// allocation can retire the stale GPU texture before it is considered by a
// subsequent GXLoadTexObj. There is at most one live range per destination;
// rewriting the destination replaces its previous extent.
std::mutex g_efbCopyDestinationsMutex;
std::map<uint32_t, uint32_t> g_efbCopyDestinations;
uint32_t g_largestEfbCopyDestination = 0;

void RememberEfbCopyDestination(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
    g_efbCopyDestinations[CanonicalizeGxMainRamAddress(addr)] = size;
    // A conservative monotonic maximum lets invalidation jump directly to
    // the only map interval that could overlap instead of walking every copy
    // destination on every DCStoreRange.
    g_largestEfbCopyDestination = std::max(g_largestEfbCopyDestination, size);
}

} // namespace

void InvalidateEfbCopyDestinationsForRange(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }

    const uint64_t dirtyStart = CanonicalizeGxMainRamAddress(addr);
    const uint64_t dirtyEnd = dirtyStart + size;
    std::vector<uint32_t> retired;
    {
        std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
        const uint64_t earliestCandidate =
            dirtyStart > g_largestEfbCopyDestination ? dirtyStart - g_largestEfbCopyDestination : 0;
        for (auto it = g_efbCopyDestinations.lower_bound(static_cast<uint32_t>(earliestCandidate));
             it != g_efbCopyDestinations.end() && static_cast<uint64_t>(it->first) < dirtyEnd;) {
            const uint64_t copyStart = it->first;
            const uint64_t copyEnd = copyStart + it->second;
            if (dirtyEnd <= copyStart || dirtyStart >= copyEnd) {
                ++it;
                continue;
            }
            retired.push_back(it->first);
            it = g_efbCopyDestinations.erase(it);
        }
    }

    // Preserve FIFO ordering: the destroy command is emitted before any later
    // texture load that can consume the freshly flushed RAM bytes.
    for (const uint32_t copyAddr : retired) {
        GXDestroyCopyTex(GuestToHostPtr(copyAddr));
    }
}

// ============================================================================
// Display Copy Source/Destination
// ============================================================================


#if defined(__SWITCH__)
void SwitchBootLogExternal(const char* text) noexcept;
#endif
// The display copy's source and destination, the first few times and on change.
static void LogCopySetting(const char* what, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
#if defined(__SWITCH__)
    static int n = 0;
    static char last[3][96] = {};
    char line[96];
    std::snprintf(line, sizeof(line), "[gx] %s %u %u %u %u", what, a, b, c, d);
    const int slot = what[9] == 's' ? 0 : what[9] == 'd' ? 1 : 2;
    if (n < 6 || std::strcmp(line, last[slot]) != 0) {
        ++n;
        std::snprintf(last[slot], sizeof(last[slot]), "%s", line);
        if (n < 60) SwitchBootLogExternal(line);
    }
#else
    (void)what; (void)a; (void)b; (void)c; (void)d;
#endif
}

extern "C" void GX__SetDispCopySrc_8016f438(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    LogCopySetting("dispcopy src", l, t, w, h);
#if defined(WIINX_NATIVE_GX)
    dol::gx_native::set_display_source((u16)l, (u16)t, (u16)w, (u16)h);
    // And the two words the SDK's own keeps in gxData (rvl.gx@2007-12-11,
    // read from Mario Kart Wii's GXSetDispCopySrc): BP 0x49 and 0x4A as they
    // will be sent. Its GXSetDispCopyYScale counts the XFB lines from the
    // second; left at 0 it read a height of 1 and answered one line.
    if (const uint32_t gx = GX_HLE_GuestData()) {
        Memory::Write32(gx + 0x230, 0x49u << 24 | (t & 0x3FF) << 10 | (l & 0x3FF));
        Memory::Write32(gx + 0x234, 0x4Au << 24 | ((h - 1) & 0x3FF) << 10 | ((w - 1) & 0x3FF));
    }
#else
    GXSetDispCopySrc((u16)l, (u16)t, (u16)w, (u16)h);
#endif
}
PPC_NATIVE_OVERRIDE_VOID(8016f438, GX__SetDispCopySrc_8016f438, (uint32_t l, uint32_t t, uint32_t w, uint32_t h), (l, t, w, h));

extern "C" void GX__SetDispCopyDst_8016f4b8(uint32_t w, uint32_t h) {
    LogCopySetting("dispcopy dst", w, h, 0, 0);
    GXSetDispCopyDst((u16)w, (u16)h);
}
PPC_NATIVE_OVERRIDE_VOID(8016f4b8, GX__SetDispCopyDst_8016f4b8, (uint32_t w, uint32_t h), (w, h));

// ============================================================================
// Texture Copy Source/Destination
// ============================================================================

extern "C" void GX__SetTexCopySrc_8016f478(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    GXSetTexCopySrc((u16)l, (u16)t, (u16)w, (u16)h);
    g_texCopyState.srcLeft=(u16)l; g_texCopyState.srcTop=(u16)t;
    g_texCopyState.srcWidth=(u16)w; g_texCopyState.srcHeight=(u16)h;
}
PPC_NATIVE_OVERRIDE_VOID(8016f478, GX__SetTexCopySrc_8016f478, (uint32_t l, uint32_t t, uint32_t w, uint32_t h), (l, t, w, h));

extern "C" void GX__SetTexCopyDst_8016f4dc(uint32_t w, uint32_t h, uint32_t f, uint32_t m) {
    GXSetTexCopyDst((u16)w, (u16)h, (GXTexFmt)f, (GXBool)m);
    g_texCopyState.dstWidth=(u16)w; g_texCopyState.dstHeight=(u16)h;
    g_texCopyState.dstFormat=f; g_texCopyState.dstMipmap=m & 0xFF;  // (GXBool: the low byte)
}
PPC_NATIVE_OVERRIDE_VOID(8016f4dc, GX__SetTexCopyDst_8016f4dc, (uint32_t w, uint32_t h, uint32_t f, uint32_t m), (w, h, f, m));

extern "C" void GX__SetCopyFilter_8016fa40(uint32_t aa, uint32_t spa, uint32_t vf, uint32_t vfa) {
    uint8_t sp[12][2]={}, vfb[7]={};
    if(spa) std::memcpy(sp, GuestToHostPtr(spa, 24), 24);
    if(vfa) std::memcpy(vfb, GuestToHostPtr(vfa, 7), 7);
    // the vertical filter, for the native copies (GXBool: the low byte)
    g_texCopyState.vfilterOn = (vf & 0xFF) != 0 && vfa != 0;
    std::memcpy(g_texCopyState.vfilter, vfb, sizeof(vfb));
    GXSetCopyFilter((GXBool)aa, sp, (GXBool)vf, vfb);
}
PPC_NATIVE_OVERRIDE_VOID(8016fa40, GX__SetCopyFilter_8016fa40, (uint32_t aa, uint32_t spa, uint32_t vf, uint32_t vfa), (aa, spa, vf, vfa));

// GXSetDispCopyYScale(vscale) -> the XFB lines the display copy makes. The SDK's
// own counts them from the copy source in its GX state, which the native
// GXSetDispCopySrc never writes: it read a height of 0 and answered 1 line, the
// Wii Menu passed that to GXSetDispCopyDst, and every frame was copied into a
// single line across the middle of the screen. Aurora keeps the real source.
extern "C" uint32_t GX__SetDispCopyYScale_8016f8fc(float vscale) {
    const uint32_t lines = GXSetDispCopyYScale(vscale);
    LogCopySetting("dispcopy yscale lines", lines, static_cast<uint32_t>(vscale * 1000.0f), 0, 0);
    return lines;
}
#if !defined(WIINX_NATIVE_GX)  // the game's own SDK code counts them, from the source above
PPC_NATIVE_OVERRIDE(8016f8fc, GX__SetDispCopyYScale_8016f8fc, uint32_t, (float vscale), (vscale));
#endif

extern "C" void GX__SetDispCopyGamma_8016fc24(uint32_t g) { GXSetDispCopyGamma((GXGamma)g); }
PPC_NATIVE_OVERRIDE_VOID(8016fc24, GX__SetDispCopyGamma_8016fc24, (uint32_t g), (g));

// ============================================================================
// Copy Execution
// ============================================================================

extern "C" void GX__CopyDisp_8016fc38(uint32_t da, uint32_t c) {
#if defined(WIINX_NATIVE_GX)
    // libdol's own renderer keeps the copy as that XFB's image; the screen
    // shows it from the flip on (VIFlush presents and paces to the retrace).
    // (The copy's clear is the next frame's: begin_frame clears with the copy
    // clear registers, if the copy asked for one.)
    dol::gx_native::copy_display(da, c != 0);
    ++g_gxFrameCount;
    VI_HLE_SetXfbReady(da);
    // A title that copies again and again without flipping still has its
    // frames shown, a few copies late at worst.
    if (VI_HLE_CopiesSincePresent() >= 3) {
        VI_HLE_PresentFrame(/*presentedXfb=*/true, /*paceToRetrace=*/true);
    }
    return;
#endif
    SWITCH_PHASE("CopyDisp EnsureAuroraFrameActive");
    EnsureAuroraFrameActive();
    // GX copies are FIFO-ordered on hardware. Drain submitted draws before
    // resolving the EFB so high-level copies see the same contents.
    SWITCH_PHASE("CopyDisp GXDrawDone");
    // Threaded decode queues the copy in stream order, which is this ordering
    // already; waiting for the decoder here would only stall the game thread.
    if (!aurora_gx_threaded()) {
        GXDrawDone();
    }
    SWITCH_PHASE("CopyDisp GXCopyDisp");
    GXCopyDisp(GuestToHostPtr(da), (GXBool)c);
    // No second GXDrawDone here: the frame-worker wait below is for the DONE
    // phase, which strictly subsumes the drain this call would perform.
    ++g_gxFrameCount;
    VI_HLE_SetXfbReady(da);
    // Present immediately so post-copy draws don't leak into this frame. Join at the DONE phase
    // (not the cheaper SEALED phase GXDrawDone waits for) because ImGui's draw lists, owned by
    // Aurora's render worker, replay during encode; aurora_end_frame would join here anyway.
    SWITCH_PHASE("CopyDisp aurora_wait_for_frame_worker");
    aurora_wait_for_frame_worker();
    SWITCH_PHASE("CopyDisp overlay Draw");
    settings_overlay::Draw();
    // Seal, pace to the VI retrace boundary (Aurora renders the sealed frame
    // during the wait), and pre-warm the next frame.
    VI_HLE_PresentFrame(/*presentedXfb=*/true, /*paceToRetrace=*/true);
}

PPC_NATIVE_OVERRIDE_VOID(8016fc38, GX__CopyDisp_8016fc38, (uint32_t da, uint32_t c), (da, c));


extern "C" void GX__CopyTex_8016fd74(uint32_t da, uint32_t c) {
#if defined(WIINX_NATIVE_GX)
    {
        // The destination is the size GXSetTexCopyDst gave. A copy that
        // box-filters to a mip level halves the source; a destination still the
        // source's size has not been halved yet, so it is halved here.
        const TexCopyState& copy = g_texCopyState;
        uint16_t width = copy.dstWidth, height = copy.dstHeight;
        if (copy.dstMipmap && width == copy.srcWidth && height == copy.srcHeight) {
            width = static_cast<uint16_t>((width + 1) / 2);
            height = static_cast<uint16_t>((height + 1) / 2);
        }
        // (GXBool: only the low byte is the argument - the caller leaves the
        // rest of the register as it was, and every copy read as a clear)
        dol::gx_native::copy_tex(da, copy.srcLeft, copy.srcTop, copy.srcWidth, copy.srcHeight,
                                 width, height, (c & 0xFF) != 0, copy.dstFormat,
                                 copy.vfilterOn ? copy.vfilter : nullptr);
        return;
    }
#endif
    EnsureAuroraFrameActive();
    // Match GX FIFO ordering: texture copies observe all prior draws. Threaded
    // decode queues the copy in stream order, which gives that already.
    if (!aurora_gx_threaded()) {
        GXDrawDone();
    }
    const uint16_t rawSrcLeft = g_texCopyState.srcLeft;
    const uint16_t rawSrcTop = g_texCopyState.srcTop;
    const uint16_t rawSrcWidth = g_texCopyState.srcWidth;
    const uint16_t rawSrcHeight = g_texCopyState.srcHeight;

    // Keep the source in guest EFB coordinates. Aurora maps it to the scaled
    // EFB exactly once, matching Dolphin's ConvertEFBRectangle path.
    GXSetTexCopySrc(rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight);
    // EFB copies stay GPU-only except probe-sized ones (e.g. the 4x4 lens-flare depth probe),
    // which Aurora reads back asynchronously via efb_ram::schedule and land in guest RAM a frame
    // later. RISK: copies above the probe threshold, or on the offscreen list, are not
    // auto-downloaded, so guest reads see stale RAM; call aurora_flush_efb_copies_to_ram if a
    // copy needs reading back.
    GXCopyTex(GuestToHostPtr(da), (GXBool)c);
    RememberEfbCopyDestination(
        da, GXGetTexBufferSize(g_texCopyState.dstWidth, g_texCopyState.dstHeight,
                               g_texCopyState.dstFormat, GX_FALSE, 0));
    GXSetTexCopySrc(rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight);
}
PPC_NATIVE_OVERRIDE_VOID(8016fd74, GX__CopyTex_8016fd74, (uint32_t da, uint32_t c), (da, c));
