#pragma once

#include "abi_bridge.h"

// hle/gx/gx_fatal_stubs.cpp includes nothing but this header and reaches
// std::fprintf / std::snprintf / std::abort through it.
#include <cstdio>
#include <cstdlib>

// VI Utils
void VI_HLE_ForceRetrace(CpuContext* ctx);
void VI_HLE_PollRetrace(CpuContext* ctx);
void VI_HLE_ProcessRetracesDeferred(int maxToProcess);
void VI_HLE_WaitForNextRetracePoll();
// Single owner of the Aurora frame presentation sequence (seal, optional pace
// to the VI retrace boundary, pre-warm the next frame). paceToRetrace is true
// for the GXCopyDisp producer path and false for retrace-context presents.
void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace);
bool VI_HLE_IsAdvancingRetrace();
void VI_HLE_SetXfbReady(uint32_t xfbAddr); // Called by GXCopyDisp to signal EFB→XFB copy
void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros);
void Audio_HLE_Poll(CpuContext* ctx);
// Deferred twin of Audio_HLE_Poll for the long host waits that already service
// retraces and alarms (the VI retrace pacing loop, the Aurora frame-worker wait
// callback). Runs the AI DMA tick on an isolated register file the way
// OS_HLE_ProcessAlarmsDeferred does, so it is safe to call from the middle of an
// arbitrary translated function.
void Audio_HLE_PollDeferred();
bool OS_HLE_InterruptsEnabled() noexcept;
extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess);
extern "C" void OS_HLE_BeginDeferredGuestCallbacks();
extern "C" void OS_HLE_EndDeferredGuestCallbacks();


// Defines and registers a faithful native reimplementation that REPLACES the translated function
// at a PPC address (not a stub; genuine not-yet-implemented entries live in hle/gx/gx_fatal_stubs.cpp
// and abort). The translator regex-parses these macro names to skip that address at build time, so
// renaming requires updating Translator.Cli/Program.cs, RuntimeNativeGuestEffectAnalyzer.cs,
// TranslatedBuildShardEmitter.cs and RuntimeNativeFunctionAbiProvider.cs together.

//
// Each one does two things, and only the second is true of every game:
//
//   1. It defines `func_<address>`, so translated code that calls that address
//      directly lands on the native. The address is the one the native was
//      written from, so this is only meaningful in that game - in another, the
//      same address is a different function, which the translator has trans-
//      lated under the same name. Hence WIINX_NATIVE_ADDRESS_ALIASES, which a
//      build turns off for every game but the one the addresses belong to.
//   2. It registers the native, through NativeBindings::Resolve, which looks
//      the address up in the game's own table and falls back to the one written
//      here. That part is right for every game and is always compiled.
#ifndef WIINX_NATIVE_ADDRESS_ALIASES
#define WIINX_NATIVE_ADDRESS_ALIASES 1
#endif

#if WIINX_NATIVE_ADDRESS_ALIASES
#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list) \
    extern "C" ret_type func_##addr_hex arg_list { return name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)

#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list) \
    extern "C" void func_##addr_hex arg_list { name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)
#else
#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list) \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)

#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list) \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)
#endif
