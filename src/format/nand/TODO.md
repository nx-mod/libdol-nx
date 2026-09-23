# TODO - format/nand

## Titles

- [ ] Install a title: write a WAD's ticket, TMD and contents where the NAND
      keeps them, including the shared-content map `/shared1/content.map`
- [ ] Remove one, and say what it was using
- [ ] List what is installed, with each title's version and kind, for a launcher
      to show ([docs/titles.md](../../../docs/titles.md))
- [ ] Decrypt a content, given a key the caller holds: the format and both IVs
      are here, the cipher is not
- [ ] Verify a signature. Today the type and issuer are reported and trust is
      the caller's; a launcher will want the real answer

## Saves

- [ ] `data.bin`, the format a save is exported and imported in
- [ ] The per-title save banner, which is what a save looks like on screen

## Console files

- [ ] Write `setting.txt`, not only read it
- [ ] ASH decompression, which the System Menu's own files need

## Checks

- [ ] A WAD round-trip against a real one, on a machine that has it
- [ ] SYSCONF written here, read back by the runtime
