# wiimmfi

`wiiwarepatcher.cpp` is KcrPL's WiiWare-Patcher, unchanged, from
https://github.com/KcrPL/WiiWare-Patcher at commit `989da59e0185` (`src/wiiwarepatcher.cpp`).
Its `do_new_wiimmfi`, the security patch, is by MrBean35000vr and Leseratte for the Wiimmfi project.

AGPL-3.0 (`LICENSE`). It is built as a program of its own, `wiinx-wiimmfi`, and run on a title's
executable before it is translated (`tools/wiinx-build`, when `recomp.yml` says `wiimmfi: true`);
nothing of it is linked into libdol-nx.

It points the game's `nintendowifi.net` servers at Wiimmfi's, marks its User-Agent, and patches the
GameSpy GT2 peer-to-peer and master-server code Wiimmfi requires before a game may connect.
