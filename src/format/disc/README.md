# format/disc

Disc images, GameCube and Wii, and the containers dumps come in.

Three layers, each usable on its own:

| Layer | Is |
|---|---|
| container | the shape the dump was written in |
| partition | the Wii's partition table, and the encryption over its clusters. A GameCube disc has neither |
| filesystem | the FST both consoles share, and the files in it |

## Containers

Almost nobody keeps a raw image - a GameCube disc is 1.35 GB and a Wii disc is
4.4 GB - so a dump arrives in whatever the tool of the day wrote. Each is
recognised by its first bytes, and the ones that need no decompressor are read:

| | Is | State |
|---|---|---|
| `.iso`, `.gcm` | the image itself | **read** |
| `.ciso`, `.cso` | the image with its empty blocks left out | **read** |
| `.wbfs` | the USB loaders' layout, one game to a file | **read** |
| `.rvz`, `.wia` | Dolphin's own: the disc in compressed chunks, with its padding thrown away | **read**, with a decompressor the caller supplies |
| `.gcz` | Dolphin's older format: zlib blocks, nothing scrubbed | **read**, with a decompressor the caller supplies |
| `.nkit.iso`, `.nkit.gcz` | a preservation format that rebuilds the original exactly | recognised; convert it first |

A file that is recognised and not readable yet is reported by name, which is
more use to whoever is holding it than a failure to open.

### RVZ and WIA

These are what a dump made in the last few years almost always is, so they are
read here - with two things left to the caller, as encryption is:

- **the decompressor.** A `Decompressor` is handed in, so this library depends
  on no compressor and a program links only the methods it wants. Zstandard is
  what RVZ uses in practice.
- **the padding.** A dump throws away the filler between a disc's files and
  keeps only the seed it can be regrown from. That filler lies where no file
  does, so it is written as zeros and every real byte still reads correctly.
  Regenerating it matters only for verifying a dump against its hashes.

A Wii disc in these formats needs no cipher at all: the partitions are stored
decrypted, and `Image` is told so.

Three things a reader has to get right, and each cost a bug before it worked
against a real file:

- a raw-data region's chunks sit on a grid of whole disc sectors, so a region
  that begins inside one starts at the sector below it;
- a partition's chunks carry the hashes they replaced as exception lists in
  front of their data, inside the compressed stream when there is one;
- `rvz_packed_size` is the length of the *packed* representation, not of what it
  expands to.

A Wii image is a GameCube image plus the middle layer, so one reader serves
both.

This is what lets a game be played from the dump itself rather than from an
extracted copy: the runtime reads files through the same interface whether they
come from a folder or from an image.

## Reading one

A caller gives a `Source` - anything that answers "read these bytes at this
offset" - and, for a Wii disc, a `Cipher`. Nothing here opens a file or holds a
disc in memory, which is what lets the same code read a dump on a PC, a dump on
an SD card, and a buffer in a test.

```cpp
auto image = wiinx::disc::Image::Open(source, cipher);
image->ReadFileTable();
auto course = image->ReadFile("course.szs");
```

No key is in this library. A Wii partition's title key is wrapped with a common
key the caller supplies, and the cipher itself is the caller's too.

Reads are cut at cluster boundaries, because each cluster carries its own hashes
and its own initialisation vector, and the last cluster decrypted is kept: a
game reading one file reads the same cluster many times over.

## What is done

- [x] Raw images, GameCube and Wii
- [x] The Wii's partition table, and the game partition's clusters
- [x] The file table both consoles share, and reading a file out of it
- [x] The executable, sized from its own section table
- [ ] WBFS and CISO ([TODO](TODO.md))
- [ ] RVZ, which needs a decompressor and Dolphin's junk-data generator
