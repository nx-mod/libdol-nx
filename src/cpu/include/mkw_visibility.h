#pragma once

// Everything here links into one executable, so a global defined in another
// translation unit can never be substituted at load time. Saying so lets the
// compiler address it directly (adrp/add) instead of loading its address from
// the GOT first, and lets it inline and allocate registers across calls.
//
// It matters for the globals the translated code touches on every guest memory
// access: without it one shard alone paid 8175 GOT loads.
#if defined(__ELF__) || defined(__SWITCH__)
#define MKW_HIDDEN __attribute__((visibility("hidden")))
#else
#define MKW_HIDDEN
#endif
