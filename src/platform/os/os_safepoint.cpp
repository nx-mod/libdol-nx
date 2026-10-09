// A safe point: what an interrupt does on the Wii, at a loop's backward edge.
//
// A Wii thread spinning in a loop is preempted by the next VI, decrementer or
// device interrupt: the handler runs, may wake a thread, and if that thread
// has the higher priority it runs first. Our guest threads only switch when
// they call into the OS, so a thread waiting in a loop for another one froze
// both - the Wii Menu's setup page stopped after its first button, the menu
// thread that should have answered never ran again and neither did retrace.
//
// Translated loops check g_guestSafePointRequest on every backward edge; the
// watchdog thread raises it every couple of milliseconds. Here, with the
// loop's registers published to ctx, the due retraces and alarms are
// delivered (each on its own interrupt context, as before), and if one woke a
// thread the scheduler picks again - the interrupt's return. Nothing happens
// while the guest has interrupts disabled, during a retrace already being
// delivered, or on a scratch context (an interrupt callback's own loop).
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "os_internal.h"

extern "C" void SelectThread_801a9c08(CpuContext* ctx);
extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess);

extern "C" void GuestSafePoint(CpuContext* ctx) {
    g_guestSafePointRequest.store(0, std::memory_order_relaxed);
    static bool active = false;
    if (active || ctx == nullptr || !OS_HLE_InterruptsEnabled() || VI_HLE_IsAdvancingRetrace()) {
        return;
    }
    active = true;
    VI_HLE_ProcessRetracesDeferred(1);
    OS_HLE_ProcessAlarmsDeferred(8);
    active = false;

    if (ctx != &GetPersistentCpuContext() || VI_HLE_IsAdvancingRetrace()) {
        return;
    }
    if (::Memory::Read32(kSchedulerReschedCounterAddr) == 0) {
        return;
    }
    const uint32_t r3 = ctx->gpr[3];
    ctx->gpr[3] = 0;  // SelectThread(yield = 0): only a higher priority preempts
    SelectThread_801a9c08(ctx);
    ctx->gpr[3] = r3;
}
