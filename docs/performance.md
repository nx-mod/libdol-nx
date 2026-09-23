# Performance, and what has been measured

Numbers here come from a Switch running Mario Kart Wii built from libwii-nx,
with the runtime's own per-frame counters sent over the network log
(`wii-nx/config/loghost.txt` names a host; the runtime then reports every
second and arms its sampling profiler after 30 seconds).

## Watch out for

- **`[video] threaded_gx` costs more than it saves.** Decoding graphics
  commands on a worker was built to take work off the game thread. Measured on
  hardware it does the opposite:

  | per frame | threaded on | threaded off |
  |---|---|---|
  | GX `apply` | 1743 us | ~130 us |
  | GX `sync` | 195 us | ~18 us |
  | game thread waiting on the worker | 7353 us | 0 |
  | audio inside the idle loop | 7323 us | ~1300 us |
  | game-thread slack | 0 | 4000-5500 us |

  The worker reported `idle, queued 0` while the game thread waited seven
  milliseconds for it. The default is off, and a config left over from the
  experiments is worth checking before measuring anything else: a game folder's
  `config.toml` keeps whatever was last written there.

- **Free memory sits at 3 MB of 3189 MB.** Every report above ends with
  `[mem used=3185MB/3189MB free=3MB]`. Nothing has been traced to it yet, but
  there is no headroom for anything new.

- **Retraces get dropped**: `[retrace] SKIPPED, guard held` counted 16,400 in
  one session.

## What the game spends a race in

The sampling profiler splits the main thread between translated game code and
the native runtime, and ranks the guest functions it lands in. In a race:

**80% translated game code, 20% the whole native runtime.**

| share | function (Mario Kart Wii) |
|---|---|
| 14.9% | `0x80064FD0` - unnamed, immediately after `nw4r::g3d::detail::LoadMaterial` |
| 12.6% | `0x80063870` - unnamed, among the `nw4r::g3d::G3DState::LoadRes*` loaders |
| 3.2% | `nw4r::g3d::ScnMdl::G3dProc` |
| 2.5% | `nw4r::g3d::CalcWorld` |
| 1.9% | `0x80066AA0` - unnamed, same g3d block |
| 1.4% + 0.9% | `nw4r::snd::detail::SoundThread::AxCallbackFunc`, `SoundThreadFunc` |
| 1.3% | `nw4r::ef::DrawBillboardStrategy::DrawNormalBillboard` |
| 1.1% | `__AXOutAiCallback` |
| 1.0% | `RaceScene::OnCalc`, `WheelPhysics::UpdateCollision` |

Over a quarter of the frame is nw4r g3d loading material and scene state into
the GX FIFO, and almost none of the top of the list is the game's own logic.
That is what the `accel/nw4r` g3d natives are for, and it is shared by every
nw4r game rather than being Mario Kart Wii's problem.

The two largest are unnamed in every map we have: Wii Sports' g3d is 2007-06,
Brawl's is 2007-12, Mario Kart Wii's is 2008-03, and these particular functions
differ enough between those builds that signatures do not carry the names
across. Their neighbours place them.

## The FIFO counters, which say more than the sampler

`[gxfifo]` reports what decoding the guest's graphics commands costs. In a
race:

```
per frame: writes=10255 decode=15231us
split: bp=1379x 93us  cp=96x 3us  xf=1791x 608us
       calldl=1492x 12649us  draw=0x 0us  vertex=1491x 1086us
```

**15.2 ms of a ~66 ms frame is FIFO decode, and 12.6 ms of it is display-list
calls** - 1,492 a frame, about 8.5 us each. Two things follow:

- `draw=0x`. Nothing is drawn immediate-mode in a race; every draw is inside a
  display list. The raw direct-draw fast path in `gx_fifo.cpp`
  (`TryGetRawDirectFifoVertexSize`, which gives up when any attribute is
  indexed) therefore cannot help here, whatever it is made to accept.
- The sampling profiler blames this on the game. It charges FIFO decode to
  whichever guest function wrote the words, which is why `nw4r::g3d`'s material
  and state loaders looked like the hot spots - they are the code emitting and
  calling these lists.

Where those 12.6 ms go, with the scan counters and section timers in place:

| | per frame |
|---|---|
| inside Aurora's decoder (`process`: parse + submit draws) | 9.2 ms |
| replaying the list's CP register writes | 2.1 ms |
| the scan on a cache miss, flattening, index bounds | ~1 ms |
| publishing vertex state afterwards | 0.2 ms |

**Caching a display list does not work, and the cache cannot be fixed.**
Measured in a race: 723 hits against 864 misses a frame. Keying the cache on
the list's content rather than its address - so a list rebuilt into a different
scratch buffer each frame would still be recognised - changed nothing: 952
misses, and no digest collisions. The lists a game builds for its animated
models differ in their own bytes every frame, matrix indices among them, so
there is nothing for a cache to recognise. That attempt was reverted; only its
counters were kept.

What is left is Aurora's decoder itself, and the draws it submits: of the
9.2 ms, about 3 ms was measured as draw submission (pipeline 1.0 ms, textures
0.7 ms, vertex arrays 0.5 ms, bind 0.3 ms, uniforms 0.4 ms) when those timers
were on, leaving roughly 6 ms parsing bytes that have to be parsed.

## Where a frame goes (threaded off, menus)

| | |
|---|---|
| graphics commands into Aurora | 1.8 - 2.5 ms (`aurora` 1.2-1.9, pipeline 0.6, textures 0.5-1.0) |
| audio | ~5 ms (`guestcb` 2.7-3.3, `push` 1.9-2.6) |
| idle / waiting | 4 - 5.5 ms |

Audio is now the largest single cost, and most of it is the guest's own
callback - translated game code, not the runtime's mixer.

## Notes

- 2026-09-23: the graphics-command worker is not the thing to fix. It lives in
  the 20% the native runtime costs, and the whole graphics path is under 2.5 ms
  of a ~66 ms frame; the 53 ms is translated code.
- 2026-09-23: first measurements of a libwii-nx build on hardware. The game
  reaches a race; it runs about four times slower than the console, which is
  where it was before the port, so the move cost nothing measurable.
