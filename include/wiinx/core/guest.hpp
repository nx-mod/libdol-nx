// Typed access to game structures.
//
// A game structure is a base address plus a layout: a struct of Field
// constants giving each member's type and offset for one library version.
//
//     struct PaneLayout2008 {
//         static constexpr Field<u8, 0xB8> alpha{};
//     };
//     Guest<PaneLayout2008> pane{address};
//     u8 a = pane[PaneLayout2008::alpha];
//     pane.set(PaneLayout2008::alpha, 255);
//
// Everything is read and written big-endian through the host's memory
// mapping, so natives never see raw guest pointers.
#pragma once

#include "wiinx/core/host.hpp"
#include "wiinx/core/types.hpp"

namespace wiinx {

template <class T, u32 Offset>
struct Field {
    using type = T;
    static constexpr u32 offset = Offset;
};

// A 3x4 affine matrix as games store it: row-major f32.
struct Mtx34 {
    f32 m[3][4];
};

// How much guest memory a value occupies: a matrix is twelve floats, and
// anything else is its own size.
template <class T>
constexpr u32 sizeof_guest() noexcept {
    if constexpr (std::is_same_v<T, Mtx34>) {
        return 12 * static_cast<u32>(sizeof(f32));
    } else {
        return static_cast<u32>(sizeof(T));
    }
}

// Loads and stores of guest memory. Mtx34 is read element-wise.
// Where `size` bytes of guest memory are, through whichever way the host
// offers. A native that reads without checking gets what it deserves, so the
// callers below go through Guest<>'s checked helpers or check first.
inline u8* at(GuestAddr addr, u32 size) noexcept {
    const Host& h = host();
    if (h.pointer != nullptr) {
        return h.pointer(addr, size);
    }
    return h.memory != nullptr ? h.memory + addr : nullptr;
}

template <class T>
T load(GuestAddr addr) noexcept {
    const u8* p = at(addr, sizeof_guest<T>());
    if (p == nullptr) {
        // Not readable: give a zero rather than fault. A native that cares
        // checks first; this keeps a bad guest pointer from taking the process
        // down, which is what the runtime's own accessors did.
        return T{};
    }
    if constexpr (std::is_same_v<T, Mtx34>) {
        Mtx34 out;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                out.m[r][c] = from_be<f32>(p + (r * 4 + c) * 4);
        return out;
    } else {
        return from_be<T>(p);
    }
}

template <class T>
void store(GuestAddr addr, const T& value) noexcept {
    u8* p = at(addr, sizeof_guest<T>());
    if (p == nullptr) {
        return;
    }
    if constexpr (std::is_same_v<T, Mtx34>) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                to_be<f32>(p + (r * 4 + c) * 4, value.m[r][c]);
    } else {
        to_be<T>(p, value);
    }
}

template <class Layout>
struct Guest {
    GuestAddr addr = 0;

    explicit operator bool() const noexcept { return addr != 0; }

    template <class T, u32 Off>
    T operator[](Field<T, Off>) const noexcept {
        return load<T>(addr + Off);
    }

    template <class T, u32 Off>
    void set(Field<T, Off>, const T& value) const noexcept {
        store<T>(addr + Off, value);
    }

    // Element `index` of an array member of type T starting at the field.
    template <class T, u32 Off>
    T at(Field<T, Off>, u32 index) const noexcept {
        return load<T>(addr + Off + index * static_cast<u32>(sizeof(T)));
    }
};

}  // namespace wiinx
