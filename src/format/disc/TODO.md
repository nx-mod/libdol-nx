# TODO - format/disc

- [x] Raw images, both consoles, and the file table they share
- [x] Wii partitions, their clusters, and decryption through the caller's cipher
- [x] A cluster cache, since a game reading a file reads one many times over
- [x] Synthetic images under test
- [x] CISO and WBFS: the same image with the empty parts left out
- [x] Recognise every common container by name, so an unreadable one says which
      it is
- [x] RVZ and WIA, through a decompressor the caller supplies
- [ ] A zstd decompressor in the app and the tools, which is what makes RVZ work
      in practice rather than in principle
- [ ] LZMA and bzip2, for older WIA files
- [ ] Regenerate a disc's padding from its seed, for verifying a dump against
      its hashes. Dolphin is GPL-2-or-later, so its generator can be followed
      and credited
- [x] Checked against real dumps made by Dolphin: a GameCube disc and a Wii one,
      both read through to their files and their executables
- [x] GCZ, common in older collections: zlib blocks, no scrubbing
- [ ] Split files - `.wbf1`, `.part1.iso` - joined before the container sees
      them, since FAT32 stops at 4 GB
- [ ] NKit: recognised today. Reading it means rebuilding what it removed
- [ ] The apploader, for a GameCube disc's boot path
- [ ] Extract straight into a game project, so `wiinx-build` needs nothing
      installed at all
- [ ] Verify a partition against its hashes, for a dump that may be damaged
- [ ] A check against a real dump, on a machine that has one

Then `platform/fs` reads through this instead of an extracted folder, and a dump
on the card is all a player needs.
