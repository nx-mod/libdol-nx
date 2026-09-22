#pragma once

#include <atomic>
#include <cstdint>

#define MKW_RESTRICT __restrict
#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#else
#error "ppc_isa_config.h has no SIMD intrinsics header for this architecture"
#endif

inline constexpr bool MkwStateFreeAbiEnabled(uint32_t) noexcept
{
    return true;
}

#if defined(_WIN32)
#define MKW_PPC_FORCE_INLINE __forceinline
#define MKW_PPC_NO_INLINE __declspec(noinline)
#define MKW_PPC_INTERNAL_CALL __regcall
#else
// __forceinline/__declspec are MS-extension keywords Clang only recognizes when targeting
// Windows (MSVC or mingw); native Linux Clang needs the GNU-attribute spellings instead.
// __regcall has no portable non-Windows equivalent worth chasing here - the extra register
// args it saves matter for the hot PPC interpreter loop on Windows, but plain calls are fine
// elsewhere.
#define MKW_PPC_FORCE_INLINE __attribute__((always_inline)) inline
#define MKW_PPC_NO_INLINE __attribute__((noinline))
#define MKW_PPC_INTERNAL_CALL
#endif
// Needs `inline` for the same reason MKW_PPC_FORCE_INLINE above has it: GCC
// refuses to always-inline an externally-linked function ("function body can
// be overwritten at link time") unless it also has inline/vague linkage.
// Clang doesn't enforce this as an error, which is why it went unnoticed
// until the first GCC (devkitA64/Switch) build of this code path.
#define MKW_PPC_ALWAYS_INLINE_BODY __attribute__((always_inline)) inline
#define MKW_PPC_COLD __attribute__((cold))

#if defined(__SWITCH__)
// devkitA64's GCC drops GNU ext_vector_type on this target (the audit sweep shows the
// attribute silently ignored, leaving the plain 64-bit type behind), so the 2x64-bit
// 128-bit "state-free" return has to be spelled as a struct instead. The translated
// code only ever builds these with a {lo, hi} aggregate and reads them back as v[0]/v[1],
// and under AAPCS64 a two-uint64_t return travels in x0/x1 exactly like Clang's vector
// type does, so this is ABI-identical to every other supported host.
struct MkwStateFreeResult2 {
    uint64_t lo;
    uint64_t hi;

    uint64_t& operator[](size_t index) { return index == 0 ? lo : hi; }
    const uint64_t& operator[](size_t index) const { return index == 0 ? lo : hi; }
};
#else
using MkwStateFreeResult2 = uint64_t __attribute__((ext_vector_type(2)));
#endif
