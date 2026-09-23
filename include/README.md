# include

The public surface: everything a caller sees, and nothing else.

```
wiinx/wiinx.hpp        the sections that build anywhere, in one include
wiinx/version.hpp      which version of this surface you are compiling against

wiinx/core/            types, typed guest memory, the host interface, natives, builds
wiinx/format/          the consoles' own files: disc/ nand/ archive/ media/
wiinx/accel/           the native tables a program registers
wiinx/cpu/             the running CPU: hooks, the guest OS layout, bindings
wiinx/platform/        the running console: product, identity, paths, input
wiinx/app/             the program around the game
```

Each folder has an umbrella header naming what is in it, and every module header
stands on its own.

`core`, `format` and `accel` need only the standard library. `cpu`, `platform`
and `app` are the running console: their headers say so, and say what to build
with, rather than failing in a hundred lines of missing includes.

Nothing here includes a runtime's own headers, and nothing here names a game.
