#pragma once

// Calling a helper inside the game's own SDK.
//
// A few places in the platform have to hand work back to code the game itself
// carries - the SDK routine that re-arms a periodic alarm, the one that returns
// the HTTP library's system information - because reproducing it here would
// mean reproducing the state it keeps.
//
// The translated function for an address exists only in the game that has that
// address, so these are declared weak: the game they were written from links
// them, and any other game links a null pointer. Nothing calls through a null
// pointer, and a game that does not have the helper does without it, which is
// the same rule the natives follow - unknown means unbound, slower or thinner,
// never wrong.
//
// Each one is a Mario Kart Wii address, and the reason they are still addresses
// rather than names is that they are helpers the SDK never exported: there is
// no symbol to look them up by. Naming them for other games means finding them
// by signature, the way natives are found (docs/signatures.md).

struct CpuContext;

// True when this game carries the helper at all.
#define WIINX_HAS_GUEST_HELPER(name) (&(name) != nullptr)

// Calls it when this game has it, and says so once when it does not.
#define WIINX_GUEST_HELPER(name, cpu)                                              \
    do {                                                                           \
        if (&(name) != nullptr) {                                                  \
            (name)(cpu);                                                           \
        } else {                                                                   \
            static bool reported = false;                                          \
            if (!reported) {                                                       \
                reported = true;                                                   \
                RT_LOGF(RT_TAG_HLE, "%s: this game has no %s; skipped\n", __func__, \
                        #name);                                                    \
            }                                                                      \
        }                                                                          \
    } while (0)
