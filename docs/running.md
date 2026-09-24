# Getting something to run

Four kinds of thing come out of this library, and they differ only in how the
executable is reached. After that they are the same work.

| From | Getting the executable | Then |
|---|---|---|
| a disc | `wiinx-disc extract`, or read the image directly | translate, build |
| a WiiWare or Virtual Console title | `wiinx-wad` - decrypt, and expand what the loader would have | translate, build |
| a system title | `wiinx-fetch-title` - Nintendo still serves them | translate, build |
| homebrew | its source, rebuilt; or its DOL, translated | build |

## What each needs that the others do not

**A disc game** needs its disc served, which is `platform/fs`. Mario Kart Wii is
the one running.

**A title** needs ES to answer questions about itself - which title am I, where
is my data - and a NAND to keep that data in. That is libwii-nx's `ios` and
`nand`, and it is the difference between a channel that starts and one that
stops on its first call.

**A WiiWare title** is an ordinary Wii game once its content is expanded: the
same SDK, the same middleware. What is unusual is the shape of the WAD, not the
game inside it.

**Homebrew** needs neither: no disc, no NAND, no ES. Built from source against
libogc-nx it is a program like any other, which is why it is the only thing here
that can be handed out ready to run.

## The order that reaches something soonest

1. **A WiiWare title.** Smallest executable, no disc to serve, and the least of
   the console involved.
2. **A system title.** Same size, and it exercises ES and the NAND - which is
   what the console's own screens are made of.
3. **A second disc game.** Bigger, and the disc path is already proven by the
   first.
4. **A GameCube game.** Everything above plus libgc-nx's four modules.

## What "runs" means at each step

Worth being precise, because a game that draws nothing has still proved a great
deal:

| | Proves |
|---|---|
| the program starts | the translation is valid code and the runtime links |
| it reaches its own main | the startup path, the SDK's init, the heap |
| it asks for files | the disc or the NAND is being served |
| it draws | GX, and everything under it |
| it responds | input, and the game's own loop |
