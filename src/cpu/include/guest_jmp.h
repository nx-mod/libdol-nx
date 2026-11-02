#pragma once

#include <csetjmp>
#include <cstdint>

struct CpuContext;

// Guest setjmp/longjmp (the Metrowerks pair the translator recognises by their
// code). A guest longjmp can land in a frame that only exists on the host stack
// that ran its setjmp, or on a stack the guest has never run on (a task system
// that sets a buffer's LR and SP by hand). Each guest stack gets a host
// coroutine; a jump inside one is a host longjmp, a jump to another switches
// to it. All coroutines of a guest thread share its CpuContext.
namespace GuestJmp {

// At a setjmp call site, before the guest setjmp runs: the host buffer the
// site's host setjmp fills, registered for the guest buffer in r3 with the LR
// and SP the guest setjmp is about to store there.
std::jmp_buf* Arm(CpuContext* ctx);

// At a longjmp call site, after the guest longjmp loaded the buffer into ctx.
[[noreturn]] void Jump(CpuContext* ctx, uint32_t guestBuffer);

}  // namespace GuestJmp
