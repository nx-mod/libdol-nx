#pragma once

// MKW_THREAD_LOCAL: use in place of the `thread_local` keyword for state that
// is only ever touched from a single host thread (which is true for nearly
// everything in this runtime - guest CPU/GX/OS emulation all runs on one
// host thread via cooperative fiber scheduling; see host_context.cpp and
// docs/switch-port-notes.md's "Round 8" for the full story).
//
// On every other platform this is exactly `thread_local`. On Switch it's
// nothing (ordinary storage duration): libnx never initializes TPIDR_EL0 for
// compiler-emitted `thread_local` storage - confirmed on-device (any access
// faults reading `tpidr_el0 + <offset>` with tpidr_el0 == 0) and by reading
// libnx's own source (nothing in nx/source ever writes that register; the
// `.main.tls` region switch.ld reserves is never wired up). `thread_local`
// simply does not work here, for anyone, ever - so for state genuinely
// confined to one thread, ordinary storage is both the only option and the
// correct one.
//
// Do NOT use this for state that's actually read/written from more than one
// real OS thread (e.g. the audio mix worker thread in hle/audio/ax_mix.cpp) -
// that needs real per-thread storage on Switch too. Use a
// pthread_key_t-backed wrapper for those instead (see ax_internal.h).
#if defined(__SWITCH__)
#define MKW_THREAD_LOCAL
#else
#define MKW_THREAD_LOCAL thread_local
#endif
