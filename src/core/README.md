# core

What every other part builds on. Headers in `include/wiinx/core/`, sources
here. Library: `wiinx::core`.

| Header       | Gives                                                                  |
|--------------|------------------------------------------------------------------------|
| `types.hpp`  | `u8`…`f64`, `GuestAddr`, big-endian `from_be` / `to_be`                |
| `host.hpp`   | `Host`: guest memory, bounds, registers, guest calls, write notify, log |
| `guest.hpp`  | `Field<T, offset>`, `Guest<Layout>`, `load` / `store`, `Mtx34`         |
| `native.hpp` | `LibVersion`, `Native`, `WIINX_NATIVE`, `add_natives`, `find_native`, `detected` |

## Host

libdol-nx never owns the CPU or memory. The runtime running the game fills a
`Host` once at startup and passes it to `set_host()`: a flat mapping of guest
memory and its bounds, register access, a way to call guest functions (the host
chooses the link register), a notification for memory native code rewrote, and
an optional log. Nothing in libdol-nx includes a runtime's headers.

## Typed guest access

A layout is a struct of `Field` constants for one build:

```cpp
struct PaneLayout2008 {
    static constexpr Field<u8, 0xB8> alpha{};
};
Guest<PaneLayout2008> pane{address};
u8 a = pane[PaneLayout2008::alpha];
pane.set(PaneLayout2008::alpha, u8{255});
```

Reads and writes are big-endian through the host's mapping.

## Builds and natives

`LibVersion` names one build of anything versioned - a library, the SDK or IOS
a game targets, a game revision - by a stable id (`nw4r.lyt@2008-03-08`), what
it versions (`nw4r.lyt`) and how the game names it.

- Natives: each module keeps one table and returns it from `natives()`; the
  program calls `add_natives()` for each module it links, then binds with
  `find_native(name, build_id)`. No self-registering statics, so static linking
  never drops a module's natives, and core never depends on a module.
- Behavior: the host passes the game's detected builds to `set_detected()`;
  any module asks `detected("rvl.os")` when its behavior depends on a build.

## Check

```sh
g++ -std=c++20 -Iinclude tests/core_check.cpp src/core/*.cpp -o check && ./check
```

## Notes

- 2026-09-22: staged types, host, typed access, registry. Builds with the
  Switch toolchain; `tests/core_check.cpp` passes on the host.
- Registry made explicit (`add_natives`) so core stays below the modules.
- `Host::call` has no return address: the runtime's lyt native hard-coded Mario
  Kart Wii's, which a library cannot know.
- `LibVersion` generalized beyond libraries; `set_detected` / `detected` added
  so platform behavior can depend on builds too.
