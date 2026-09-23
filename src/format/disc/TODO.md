# TODO - format/disc

- [x] Raw images, both consoles, and the file table they share
- [x] Wii partitions, their clusters, and decryption through the caller's cipher
- [x] A cluster cache, since a game reading a file reads one many times over
- [x] Synthetic images under test
- [ ] WBFS and CISO: the same image with the empty parts left out
- [ ] RVZ, which needs zstd or LZMA and the regeneration of the padding Dolphin
      throws away
- [ ] The apploader, for a GameCube disc's boot path
- [ ] Verify a partition against its hashes, for a dump that may be damaged
- [ ] A check against a real dump, on a machine that has one

Then `platform/fs` reads through this instead of an extracted folder, and a dump
on the card is all a player needs.
