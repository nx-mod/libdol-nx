# Performance, and what has been measured

Mario Kart Wii on a Switch, native GX (deko3d). The runtime logs per-frame
counters to `logs/boot.log` every second; `[sched]`, `[idle]` and `[audio]` say
where the game thread's time goes, `gx-native` what the renderer did.

## A race frame, 2026-10-13 (build 51, ~12 fps)

About 80 ms a frame:

| | per frame | where |
|---|---|---|
| translated game code | ~45% | g3d 7.5%, EGG 3%, sound 2.8%, kart logic 2.8%, collision 1.7%, effects 1.4% |
| guest memory through the checked path | ~15% | `Memory::Write32`/`WriteFloat32`/`Read32` and fallbacks, out of line |
| the game's audio callback, in the idle loop | ~13 ms | 24 AX frames back to back: `guestcb` ~10.6 ms (nw4r snd, AXFX reverb), `push` ~2.2 ms |
| `GXDrawDone` waiting for the render thread | 2-8 ms | |

The render thread runs on core 2 at ~84%; the GPU often idles, waiting on it.

## Fixed in build 52

- **Guest memory.** On a Switch the flat guest view cannot be backed
  (`guest_flat_memory.cpp`), so every access takes the checked path. That path
  was a runtime test in every access plus an out-of-line call: clang outlined
  the `Memory::` accessors in the big shards. `RequiresCheckedAccess()` is now
  a constant on Switch and the accessors are forced inline - each access is a
  table load and a byteswap. Expected: most of the ~15%.
- **Audio output.** Handing each 3 ms block to audren (`audrvUpdate`, an IPC
  call) moved to a thread on core 1; the game thread only converts and queues.
- **Threads placed.** The render thread has core 2 to itself; the AX mixer,
  the audio output and disc reads share core 1. Horizon runs one thread of a
  priority on a core until it blocks, so two busy workers on one core took
  turns.
- **Diagnostics off.** Snapshots (80 MB of PPMs a run, a GPU stall each),
  probe frames every two seconds (a full GPU wait) and the PC sampler (the
  game thread paused a thousand times a second) all skewed the numbers. Each
  is a constant now ([deko3d](deko3d.md#diagnostics-off-by-default)).

## What would move it next, in order

1. **The audio callback off the game thread.** On a Wii it runs from the DSP
   interrupt, beside the game. Here it runs on the game thread in the time the
   game waits for the retrace. Moving it to core 1 needs `OSDisableInterrupts`
   to be a real lock (the game guards its sound data with it) and wakeups from
   the callback handed to the game thread's scheduler. ~13 ms a race frame.
2. **Vertex decode on the GPU.** The render thread converts every vertex to a
   128-byte record; decoding the game's own vertex bytes in the shader cuts its
   work and the upload. This is what lets `GXDrawDone` stop waiting.
3. **Natives for the hot library code** (`accel/nw4r`, `accel/egg`): g3d's
   material and shape loaders, `CalcWorld`, the AXFX reverb.
4. **Async disc reads** (`kDvdAsync`): written, off since a hang between
   menus that is not found yet.

## Watch out for

- Profile and log numbers from a build with diagnostics on are not speed
  numbers.
- `[retrace] SKIPPED, guard held` is re-entry, not lost retraces: the retrace
  count still advances ~59.5 a second.
- `free=3MB` on every log line is the allocator holding the whole app heap,
  not a shortage.
- The game thread being idle is not always idle: `[idle] ... audio=` is work
  done there, and `wait=` alone is time with nothing to run.
