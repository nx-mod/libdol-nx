# TODO - format/disc

Nothing is written yet. In the order it has to be built:

- [ ] Raw images: the GameCube and Wii headers, and what each says about the
      rest of the disc
- [ ] The FST both consoles share: files, folders, and reading one out
- [ ] Wii partitions: the table at `0x40000`, each partition's ticket, and the
      cluster layout - `0x400` of hashes and `0x7C00` of data
- [ ] Cluster decryption, with the key the caller supplies
- [ ] WBFS and CISO, which are ways of storing the same image with the empty
      parts left out
- [ ] A cache, so a game reading a file does not decrypt the same cluster twice
- [ ] The apploader, for a GameCube disc's boot path
- [ ] A check against a real dump on the machine that has one, and a synthetic
      image built here for the rest

Then `platform/fs` reads through this instead of an extracted folder, and a dump
on the card is all a player needs.
