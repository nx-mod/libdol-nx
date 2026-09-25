# Building on more than one machine

A shard takes 25-30 seconds to compile here and there are 106 of them, and this
machine is pinned to one job at a time by memory rather than by cores - two
compilers on the generated code exhaust 3 GB of swap and take proot with them.
So the way to build faster is another machine, not more threads.

distcc suits that: the project, the generated code and the build directory all
stay here, and only preprocessed translation units go over the wire. No second
checkout, nothing to keep in step.

## What a helper needs

Only the compiler. distcc servers never link and never read headers - the client
sends fully preprocessed source - so no libnx, no portlibs, no project.

| | |
|---|---|
| devkitA64 | **the same version**, `aarch64-none-elf-g++ (devkitA64) 16.1.0` |
| distcc | `apt-get install distcc` |
| disk | 367 MB for the compiler, and transient job files |

The version match is the part that matters. A different compiler links cleanly
and miscompiles quietly, which is worse than a node that refuses to work.

## Setting up a helper

```sh
# inside whatever userland holds devkitA64
apt-get install -y distcc
mkdir -p /usr/lib/distcc
ln -sf /opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++ /usr/lib/distcc/
ln -sf /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc /usr/lib/distcc/
distccd --daemon --allow <your subnet> --port 3632 --jobs <cores>
```

`/usr/lib/distcc` is not optional: it is the whitelist distccd checks, and
without it every job comes back `CRITICAL! aarch64-none-elf-g++ not in
/usr/lib/distcc`, the client falls back to compiling locally, and the build
merely looks slow.

On a phone, the daemon has to outlive the ssh session that started it, and
`nohup` inside `proot-distro login` does not - proot kills its children on
exit. Start it under tmux instead:

```sh
tmux new-session -d -s distcc "proot-distro login debian -- bash -lc \
    'export PATH=/opt/devkitpro/devkitA64/bin:\$PATH; \
     distccd --no-detach --allow 10.0.0.0/8 --port 3632 --jobs 6 \
             --log-file /tmp/distccd.log --log-level notice'"
```

Termux also needs its wakelock held and battery optimisation turned off, or
Android stops the node mid-build.

## Building against them

```sh
export DISTCC_HOSTS="10.214.216.122:3632/6 10.214.216.58:3632/1 --localslots=1"
cmake -S libdol-nx/cmake/game -B <build> ... \
      -DCMAKE_CXX_COMPILER_LAUNCHER=libdol-nx/tools/wiinx-distcc-launch \
      -DCMAKE_C_COMPILER_LAUNCHER=libdol-nx/tools/wiinx-distcc-launch
cmake --build <build> --target <game>_nro -j 7
```

`-j` is the total across every machine, so it is the sum of the slots plus the
local one. `/N` after a host is how many jobs that host takes, and distcc fills
hosts left to right, so put the fastest first.

### Why the launcher exists

CMake records the compiler's absolute path, and distccd refuses one:

```
compiler name </opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++>
cannot be an absolute path
```

Only a bare name is resolved, through the whitelist above.
`tools/wiinx-distcc-launch` drops the path and keeps the name. Setting
`DISTCC_HOSTS` and putting a masquerade directory on `PATH` does **not** work by
itself: the build calls the compiler by its absolute path and never looks at
`PATH`, so every job runs locally and the only clue is that the remote's job
count never moves.

## Checking it is actually working

The build does not fail when distribution fails - it compiles locally instead.
So confirm, rather than assume:

```sh
# on the helper
grep -c COMPILE_OK /tmp/distccd.log      # must climb while building
# here
ps -eo comm | grep -c cc1plus            # should be about the local slots
```

## What it is worth

Measured on one phone, six cores, over a ZeroTier link:

| | |
|---|---|
| one shard, locally | 25-30 s |
| one shard, on the phone | 82 s |
| **four shards on the phone, at once** | **88 s** |

A single job is three times slower there, but four cost 7% more than one, so the
win is parallelism: a shard every 22 seconds instead of every 25-30, on top of
whatever this machine is doing. Per-job speed is not the number to optimise.
