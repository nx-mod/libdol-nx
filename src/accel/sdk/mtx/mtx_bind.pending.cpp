// PENDING: not built yet. The runtime globs *_bind.cpp, and this file is not
// that until it is renamed.
//
// A native only takes effect if its registration existed when the game was
// translated - the translator skips emitting a function it knows is replaced,
// and a registration added afterwards collides with the code already emitted
// (multiple definition of func_80199D64). Switching one on therefore costs a
// re-translation and a full rebuild, so bindings are promoted in batches:
// write the native, test it here, leave the binding pending, and rename every
// pending file at once when the batch is worth the rebuild.
//
// Binds the SDK's matrix library for the CPU runtime (see thp_bind.cpp).
// The addresses are Mario Kart Wii's, until bindings carry them per game; the
// code behind them is the same assembly in every build of the SDK.
#include "hle_stubs.h"
#include "abi_bridge.h"
#include "wiinx/accel/sdk.hpp"

namespace {
const wiinx::Native* Find(const char* name) {
    return wiinx::find_native(name, wiinx::sdk::kMtx_Any.id);
}
}  // namespace

extern "C" void PSMTXIdentity_HLE(CpuContext* ctx) {
    static const wiinx::Native* const native = Find("PSMTXIdentity");
    native->entry(reinterpret_cast<wiinx::Cpu*>(ctx));
}

extern "C" void PSMTXConcat_HLE(CpuContext* ctx) {
    static const wiinx::Native* const native = Find("PSMTXConcat");
    native->entry(reinterpret_cast<wiinx::Cpu*>(ctx));
}

extern "C" void PSMTXConcatArray_HLE(CpuContext* ctx) {
    static const wiinx::Native* const native = Find("PSMTXConcatArray");
    native->entry(reinterpret_cast<wiinx::Cpu*>(ctx));
}

extern "C" void PSMTXCopy_HLE(CpuContext* ctx) {
    static const wiinx::Native* const native = Find("PSMTXCopy");
    native->entry(reinterpret_cast<wiinx::Cpu*>(ctx));
}

extern "C" void PSMTXTranspose_HLE(CpuContext* ctx) {
    static const wiinx::Native* const native = Find("PSMTXTranspose");
    native->entry(reinterpret_cast<wiinx::Cpu*>(ctx));
}

extern "C" void PSMTXTrans_HLE(CpuContext* ctx) {
    static const wiinx::Native* const native = Find("PSMTXTrans");
    native->entry(reinterpret_cast<wiinx::Cpu*>(ctx));
}

extern "C" void PSMTXTransApply_HLE(CpuContext* ctx) {
    static const wiinx::Native* const native = Find("PSMTXTransApply");
    native->entry(reinterpret_cast<wiinx::Cpu*>(ctx));
}

extern "C" void PSMTXScale_HLE(CpuContext* ctx) {
    static const wiinx::Native* const native = Find("PSMTXScale");
    native->entry(reinterpret_cast<wiinx::Cpu*>(ctx));
}

extern "C" void PSMTXScaleApply_HLE(CpuContext* ctx) {
    static const wiinx::Native* const native = Find("PSMTXScaleApply");
    native->entry(reinterpret_cast<wiinx::Cpu*>(ctx));
}

PPC_NATIVE_OVERRIDE_VOID(80199D04, PSMTXIdentity_HLE, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(80199D64, PSMTXConcat_HLE, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(80199E30, PSMTXConcatArray_HLE, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(8019A3E0, PSMTXTrans_HLE, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(8019A414, PSMTXTransApply_HLE, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(8019A460, PSMTXScale_HLE, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(8019A488, PSMTXScaleApply_HLE, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(80199D30, PSMTXCopy_HLE, (CpuContext* ctx), (ctx));
// PSMTXTranspose is not in this game's map, so it is not bound here; the
// native is written and tested for the game that does have it.
