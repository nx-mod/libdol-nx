# core

What every other part builds on. Headers in `include/wiinx/core/`, sources here.

| Header        | Gives                                                         |
|---------------|---------------------------------------------------------------|
| `types.hpp`   | `u8`…`f64`, `GuestAddr`, big-endian `from_be` / `to_be`       |
| `host.hpp`    | `Host`: guest memory, CPU registers, guest calls, write notify |
| `guest.hpp`   | `Field<T, offset>`, `Guest<Layout>`, `load` / `store`, `Mtx34` |
| `native.hpp`  | `LibVersion`, `Native`, `WIINX_NATIVE`, `natives()`, `find_native()` |

## Host

libwii-nx never owns the CPU or memory. The runtime running the game fills a
`Host` once at startup and passes it to `set_host()`: a flat mapping of guest
memory, register access, a way to call guest functions, and a notification for
memory native code rewrote. Nothing in libwii-nx includes a runtime's headers.

## Typed guest access

A layout is a struct of `Field` constants for one library build:

```cpp
struct PaneLayout2008 {
    static constexpr Field<u8, 0xB8> alpha{};
};
Guest<PaneLayout2008> pane{address};
u8 a = pane[PaneLayout2008::alpha];
pane.set(PaneLayout2008::alpha, u8{255});
```

Reads and writes are big-endian through the host's mapping.

## Registry

Each module keeps its natives in one array; `natives()` joins them. There are
no self-registering static objects, so linking a module as a static library
never silently drops its natives (the trap Source's `interface.h` works around).

## Check

```sh
g++ -std=c++20 -Iinclude tests/core_check.cpp src/core/*.cpp -o core_check && ./core_check
```

## Notes

- Staged 2026-09-22: types, host, typed access, registry. Builds with the
  Switch toolchain; `tests/core_check.cpp` passes on the host.
- The registry is empty until the first module lands.
