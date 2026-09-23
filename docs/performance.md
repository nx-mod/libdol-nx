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
