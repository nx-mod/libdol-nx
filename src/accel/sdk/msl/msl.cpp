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
#include <optional>

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

// A guest string, as far as it is readable. Returns the length, or nothing when
// the string runs off the end of what the guest can address - which is what a
// game with a runaway pointer looks like, and is better answered than faulted
// on.
std::optional<u32> guest_strlen(GuestAddr addr) noexcept {
    constexpr u32 kSane = 1u << 20;  // no string a game passes is a megabyte
    for (u32 length = 0; length < kSane; length++) {
        const u8* byte = at(addr + length, 1);
        if (byte == nullptr) {
            return std::nullopt;
        }
        if (*byte == 0) {
            return length;
        }
    }
    return std::nullopt;
}

void strcpy_native(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr dst = h.gpr(cpu, 3);
    const GuestAddr src = h.gpr(cpu, 4);
    if (const auto length = guest_strlen(src)) {
        const u8* from = range(src, *length + 1);
        u8* to = range(dst, *length + 1);
        if (from != nullptr && to != nullptr) {
            std::memmove(to, from, *length + 1);
        }
    }
    h.set_gpr(cpu, 3, dst);
}

void strncpy_native(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr dst = h.gpr(cpu, 3);
    const GuestAddr src = h.gpr(cpu, 4);
    const u32 size = h.gpr(cpu, 5);
    if (size != 0) {
        u8* to = range(dst, size);
        if (to != nullptr) {
            // The standard's own oddity: the destination is filled to `size`
            // with zeros, and is left unterminated when the source is longer.
            u32 copied = 0;
            for (; copied < size; copied++) {
                const u8* byte = at(src + copied, 1);
                if (byte == nullptr) {
                    return;
                }
                to[copied] = *byte;
                if (*byte == 0) {
                    break;
                }
            }
            for (u32 index = copied; index < size; index++) {
                to[index] = 0;
            }
        }
    }
    h.set_gpr(cpu, 3, dst);
}

void strcat_native(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr dst = h.gpr(cpu, 3);
    const GuestAddr src = h.gpr(cpu, 4);
    const auto already = guest_strlen(dst);
    const auto adding = guest_strlen(src);
    if (already && adding) {
        const u8* from = range(src, *adding + 1);
        u8* to = range(dst + *already, *adding + 1);
        if (from != nullptr && to != nullptr) {
            std::memmove(to, from, *adding + 1);
        }
    }
    h.set_gpr(cpu, 3, dst);
}

void strncmp_native(Cpu* cpu) {
    const Host& h = host();
    GuestAddr a = h.gpr(cpu, 3);
    GuestAddr b = h.gpr(cpu, 4);
    u32 size = h.gpr(cpu, 5);

    while (size-- != 0) {
        const u8* left = at(a++, 1);
        const u8* right = at(b++, 1);
        if (left == nullptr || right == nullptr) {
            h.set_gpr(cpu, 3, 0);
            return;
        }
        if (*left != *right) {
            h.set_gpr(cpu, 3, static_cast<u32>(*left < *right ? -1 : 1));
            return;
        }
        if (*left == 0) {
            break;
        }
    }
    h.set_gpr(cpu, 3, 0);
}

void memcmp_native(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr a = h.gpr(cpu, 3);
    const GuestAddr b = h.gpr(cpu, 4);
    const u32 size = h.gpr(cpu, 5);

    int result = 0;
    if (size != 0) {
        const u8* left = range(a, size);
        const u8* right = range(b, size);
        if (left == nullptr || right == nullptr) {
            h.set_gpr(cpu, 3, 0);
            return;
        }
        result = std::memcmp(left, right, size);
    }
    // The standard says only the sign matters, and the SDK's own returns the
    // difference of the bytes, so a game comparing against -1 or 1 is a game
    // that was already relying on more than it should have.
    h.set_gpr(cpu, 3, static_cast<u32>(result < 0 ? -1 : (result > 0 ? 1 : 0)));
}

void strchr_native(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr text = h.gpr(cpu, 3);
    const auto wanted = static_cast<u8>(h.gpr(cpu, 4));
    const auto length = guest_strlen(text);
    if (!length) {
        h.set_gpr(cpu, 3, 0);
        return;
    }
    const u8* bytes = range(text, *length + 1);
    if (bytes == nullptr) {
        h.set_gpr(cpu, 3, 0);
        return;
    }
    // The terminator counts, which is how a caller finds the end of a string.
    for (u32 index = 0; index <= *length; index++) {
        if (bytes[index] == wanted) {
            h.set_gpr(cpu, 3, text + index);
            return;
        }
    }
    h.set_gpr(cpu, 3, 0);
}

void strrchr_native(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr text = h.gpr(cpu, 3);
    const auto wanted = static_cast<u8>(h.gpr(cpu, 4));
    const auto length = guest_strlen(text);
    if (!length) {
        h.set_gpr(cpu, 3, 0);
        return;
    }
    const u8* bytes = range(text, *length + 1);
    if (bytes == nullptr) {
        h.set_gpr(cpu, 3, 0);
        return;
    }
    for (u32 index = *length + 1; index-- != 0;) {
        if (bytes[index] == wanted) {
            h.set_gpr(cpu, 3, text + index);
            return;
        }
    }
    h.set_gpr(cpu, 3, 0);
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
    WIINX_NATIVE("strncmp", kMsl_Any, strncmp_native),
    WIINX_NATIVE("strcpy", kMsl_Any, strcpy_native),
    WIINX_NATIVE("strncpy", kMsl_Any, strncpy_native),
    WIINX_NATIVE("strcat", kMsl_Any, strcat_native),
    WIINX_NATIVE("strchr", kMsl_Any, strchr_native),
    WIINX_NATIVE("strrchr", kMsl_Any, strrchr_native),
    WIINX_NATIVE("memcmp", kMsl_Any, memcmp_native),
};
extern const std::size_t kMslNativeCount = std::size(kMslNatives);

}  // namespace wiinx::sdk::msl
