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

*To write.*
