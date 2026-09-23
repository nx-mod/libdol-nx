# accel/nw4r/lyt

Layouts: the screens a Wii game draws that are not the world - menus, HUDs,
buttons, and the animations they run.

`Pane::CalculateMtx` is here, for two builds. A pane's matrix is computed from
its parent's, so the walk down a menu's tree is the hot part: a page of animated
buttons recomputes every one of them each frame.

Two builds differ only in where the fields sit, so one implementation takes a
layout table and each build names its own offsets. A game whose build is not
known runs its own code instead.

The binding (`pane_bind.cpp`) tells the native where the game binds it, so a
child dispatched through its vtable back into the same function is recognised
and recursed into directly rather than leaving through the guest.
