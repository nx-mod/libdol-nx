// MSL: the C library a game's compiler links, natively.
//
// memcpy and memset are called constantly by every game and translate to
// PowerPC loops that move four or eight bytes an iteration. The host's own are
// written for this machine and use its wide loads and stores, so the native is
// a resolution and a call.
//
// Bytes are bytes: none of this reinterprets guest memory, so the guest's
// big-endian layout is carried through untouched.
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/guest.hpp"
#include "wiinx/core/host.hpp"

#include <cstring>

namespace wiinx::sdk::msl {
namespace {

// A guest range as host bytes, or nullptr when it is not all there. Guest
// memory is one mapping for the ranges these functions are given, but a caller
// can hand over a length that runs off the end of a region, and the answer for
// that must be "no" rather than a fault.
u8* range(GuestAddr addr, u32 size) noexcept { return at(addr, size); }

void memcpy_native(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr dst = h.gpr(cpu, 3);
    const GuestAddr src = h.gpr(cpu, 4);
    const u32 size = h.gpr(cpu, 5);
    if (size != 0) {
        u8* to = range(dst, size);
        const u8* from = range(src, size);
        if (to != nullptr && from != nullptr) {
            // memmove, not memcpy: a game that overlaps its arguments gets what
            // the PowerPC loop gave it rather than whatever the host decides
            // undefined behaviour means today.
            std::memmove(to, from, size);
        }
    }
    h.set_gpr(cpu, 3, dst);  // memcpy returns its destination
}

void memmove_native(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr dst = h.gpr(cpu, 3);
    const GuestAddr src = h.gpr(cpu, 4);
    const u32 size = h.gpr(cpu, 5);
    if (size != 0) {
        u8* to = range(dst, size);
        const u8* from = range(src, size);
        if (to != nullptr && from != nullptr) {
            std::memmove(to, from, size);
        }
    }
    h.set_gpr(cpu, 3, dst);
}

void memset_native(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr dst = h.gpr(cpu, 3);
    const int value = static_cast<int>(h.gpr(cpu, 4) & 0xff);
    const u32 size = h.gpr(cpu, 5);
    if (size != 0) {
        if (u8* to = range(dst, size); to != nullptr) {
            std::memset(to, value, size);
        }
    }
    h.set_gpr(cpu, 3, dst);
}

// Walked a page at a time: a string's length is not known before reading it,
// so the range cannot be resolved in one go.
void strlen_native(Cpu* cpu) {
    const Host& h = host();
    GuestAddr p = h.gpr(cpu, 3);
    u32 length = 0;
    while (const u8* byte = range(p, 1)) {
        if (*byte == 0) {
            break;
        }
        ++length;
        ++p;
    }
    h.set_gpr(cpu, 3, length);
}

void strcmp_native(Cpu* cpu) {
    const Host& h = host();
    GuestAddr a = h.gpr(cpu, 3);
    GuestAddr b = h.gpr(cpu, 4);
    for (;;) {
        const u8* left = range(a, 1);
        const u8* right = range(b, 1);
        if (left == nullptr || right == nullptr) {
            h.set_gpr(cpu, 3, 0);
            return;
        }
        if (*left != *right) {
            h.set_gpr(cpu, 3, static_cast<u32>(*left < *right ? -1 : 1));
            return;
        }
        if (*left == 0) {
            h.set_gpr(cpu, 3, 0);
            return;
        }
        ++a;
        ++b;
    }
}

}  // namespace

// The C library is the compiler's, not a versioned Nintendo library, and the
// semantics are the standard's, so one native serves every game.
extern const Native kMslNatives[] = {
    WIINX_NATIVE("memcpy", kMsl_Any, memcpy_native),
    WIINX_NATIVE("memmove", kMsl_Any, memmove_native),
    WIINX_NATIVE("memset", kMsl_Any, memset_native),
    WIINX_NATIVE("strlen", kMsl_Any, strlen_native),
    WIINX_NATIVE("strcmp", kMsl_Any, strcmp_native),
};
extern const std::size_t kMslNativeCount = std::size(kMslNatives);

}  // namespace wiinx::sdk::msl
