// EGG::AsyncDisplay::endRender, for every game built on EGG.
//
// On hardware the GPU's finish interrupt runs EGG's DrawDoneCallback, which
// hands the copied XFB to VISetNextFrameBuffer and VIFlush. The runtime's GX
// finishes synchronously and dispatches no such callback, so without this the
// XFB ring never advances and VISetBlack(FALSE) never commits.
//
// The three functions it calls are the game's own, by name (guest_globals.h);
// tools/wiinx-find-globals reads them out of the game's endRender itself.
#include "hle_stubs.h"
#include "abi_bridge.h"
#include "guest_globals.h"
#include "native_bindings.h"
#include "runtime_log.h"

extern "C" void EGG__AsyncDisplay__endRender_HLE_8020ff9c(CpuContext* ctx) {
    static const uint32_t self = ::NativeBindings::Resolve(0x8020FF9Cu);
    static const uint32_t copyEfbToXfb = RuntimeGuestGlobals::find("egg.Display_copyEFBtoXFB");
    static const uint32_t setDrawDoneCallback = RuntimeGuestGlobals::find("gx.GXSetDrawDoneCallback");
    static const uint32_t drawDoneCallback = RuntimeGuestGlobals::find("egg.DrawDoneCallback");
    if (!copyEfbToXfb || !setDrawDoneCallback || !drawDoneCallback) {
        RT_LOG(RT_TAG_HLE) << "EGG::AsyncDisplay::endRender: egg.Display_copyEFBtoXFB, "
                              "gx.GXSetDrawDoneCallback or egg.DrawDoneCallback not named; "
                              "frame not handed to VI" << std::endl;
        return;
    }
    ctx->lr = self;
    InvokeIndirectCpu(copyEfbToXfb, ctx);
    // r3 is copyEFBtoXFB's leftover here, so pass the callback explicitly.
    ctx->gpr[3] = drawDoneCallback;
    InvokeIndirectCpu(setDrawDoneCallback, ctx);
    ctx->lr = self;
    InvokeIndirectCpu(drawDoneCallback, ctx);
}
PPC_NATIVE_OVERRIDE_VOID(8020FF9C, EGG__AsyncDisplay__endRender_HLE_8020ff9c, (CpuContext* ctx), (ctx));
