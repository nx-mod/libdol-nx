# TODO - format/nand

## Titles

- [x] Install a title: a WAD's ticket, TMD and contents written where the NAND
      keeps them, shared contents included
- [x] Remove one, leaving what other titles share
- [x] List what is installed, with each title's version and what it carries
- [ ] Say what a removed title *was* using, so a launcher can offer to clean up
      shared contents nothing needs any more
- [ ] Decrypt a content in the library, given a key the caller holds: the format
      and both IVs are here, the cipher is not. `tools/wiinx-wad` does it today
      on a PC
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
