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
   permission to launch one. That is libwii-nx's `es`.

## What installing one takes

A WAD carries everything: certificates, ticket, TMD and contents. Installing is
writing them where the NAND keeps them - `format/nand`'s paths - and nothing
more, because the console verifies at launch rather than at install.

Both kinds of WAD are read today, and `tools/wiinx-wad` is how you look at one:

```sh
tools/wiinx-wad Channel.wad                 # which title it is, and what it carries
tools/wiinx-wad Channel.wad unpacked/       # decrypted, with the executable as main.dol
```

What is not written yet is the writing - installing a title into a NAND: see
[src/format/nand/TODO.md](../src/format/nand/TODO.md).

## Where the titles come from

Nintendo's servers still serve system titles, and each user downloads their own
copy; `tools/wiinx-fetch-title` does that. WiiWare and Virtual Console titles
are not served any more, so those come from a user's own console.

Nothing Nintendo owns is in this repository, and none of it is distributed.
