# TODO - include

- [ ] Move the runtime's own headers under `wiinx/cpu/` and `wiinx/platform/`,
      so the public surface is the only surface. Needs one retranslate, since
      generated code includes them by name
- [ ] `dolnx::` for what both consoles share, once that move happens
- [ ] An installed package: `find_package(wiinx)` for a project that is not
      building this tree itself
