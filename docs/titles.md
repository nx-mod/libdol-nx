# Titles: the console's own software, and the games sold for it

A disc game is one kind of title. The NAND holds the rest, and they are all the
same shape: a ticket, a TMD, and a set of contents. What differs is the high
word of the title id, which says what a title is and where the console will look
for it.

| High word | Kind | Examples |
|---|---|---|
| `00000001` | system | the System Menu, every IOS, BC and MIOS |
| `00010000` | disc save | a disc game's save folder, made on first play |
| `00010001` | channel | WiiWare, and anything installed from a WAD |
| `00010002` | system channel | Mii Channel, Photo Channel, News, Weather, the Shop |
| `00010004` | game channel | a disc game's own channel tile |
| `00010005` | downloadable content | extra levels and songs a game loads |
| `00010008` | hidden channel | region select, EULA |

Virtual Console titles are channels: the emulator and the ROM travel together as
one title's contents, so from the outside a Virtual Console game and a WiiWare
game are indistinguishable.

## What a WAD actually contains

Reading twelve of them - ten WiiWare, two Virtual Console - says more than the
format documentation does:

- **The content a title boots is usually not the game.** The TMD names a boot
  index, and for WiiWare and Virtual Console that content is a small loader,
  about 300 KB, which reads the real code out of another content and jumps into
  it. Several titles boot the *same* loader, because it is a shared content.
- **Shared contents are common.** Both Virtual Console titles carried the same
  three: the emulator they run on. They live in `/shared1` on a console, listed
  in `content.map`, and a title that installs without them will not start.
- **The key index byte is often rubbish.** Tickets in circulation have been
  through repacking tools that scribbled in the reserved bytes, so a value
  greater than 2 means the ordinary common key rather than a key that does not
  exist.

For static recompilation that boot content is only the first step: the loader is
what gets translated, and whatever it loads has to be translated as its own
module, the way a game's RELs are.

## What running one takes

Nothing in a title is special to the console's hardware - it is PowerPC code in
a content file, and the translator treats it as it treats a disc's executable.
What it needs that a disc game does not:

1. **Contents rather than files.** A title's executable is content `0` (or
   whichever the TMD's boot index names), and its data is the other contents.
   `format/nand` reads all of this already.
2. **Decryption.** Contents are AES-CBC under the title key, which is itself
   wrapped with a common key. The formats and both IVs are in
   `wiinx/format/nand/title.hpp`; the keys and the cipher are the caller's.
3. **A NAND to run against.** A channel expects `/title/<id>/data` to exist and
   to be writable, and expects the settings and Mii database to be there.
4. **ES.** Titles ask IOS which titles exist, what their versions are, and for
   permission to launch one. That is libdol-nx's `es`.

## What installing one takes

A WAD carries everything: certificates, ticket, TMD and contents. Installing is
writing them where the NAND keeps them - `format/nand`'s paths - and nothing
more, because the console verifies at launch rather than at install.

Both kinds of WAD are read today, and `tools/wiinx-wad` is how you look at one:

```sh
tools/wiinx-wad Channel.wad                 # which title it is, and what it carries
tools/wiinx-wad Channel.wad unpacked/       # decrypted, as main.dol and payload.dol
```

`main.dol` is the content the title boots; `payload.dol` is the largest
executable among the rest, which for WiiWare and Virtual Console is the game.

Installing one is `tools/wiinx-install-title`, which takes either a WAD or a
title downloaded from Nintendo and writes what a console keeps:

```
/ticket/<high>/<low>.tik
/title/<high>/<low>/content/title.tmd and <id>.app
/title/<high>/<low>/data/                          the title's own saves
/shared1/<n>.app and /shared1/content.map          what it shares with others
```

Contents are written decrypted, which is how a console keeps them: what
encrypts a real NAND is the filesystem underneath, not the title. A content two
titles share is written once - Mega Man 9 and the Mii Channel share the Home
Button menu's resources, and the second install writes none of them again.

## Where the titles come from

Nintendo's servers still serve system titles, and each user downloads their own
copy; `tools/wiinx-fetch-title` does that. WiiWare and Virtual Console titles
are not served any more, so those come from a user's own console.

Nothing Nintendo owns is in this repository, and none of it is distributed.
