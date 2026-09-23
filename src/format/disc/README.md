# format/disc

Disc images, GameCube and Wii, and the containers dumps come in.

Three layers, each usable on its own:

| Layer | Is |
|---|---|
| container | the file as it sits on a card: raw (`.iso`, `.gcm`), WBFS, CISO |
| partition | the Wii's partition table, and the encryption over its clusters. A GameCube disc has neither |
| filesystem | the FST both consoles share, and the files in it |

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
