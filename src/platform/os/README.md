# platform/os

The operating system a game links: threads, alarms, interrupts, time and caches.

The SDK's OS is compiled into every game, so the game brings its own copy and
this answers at the same boundaries. Threads are real host threads with the
guest's scheduling decisions preserved - a game that relies on its own priority
order gets it, because the run queue it reads is still its own.

| File | Is |
|---|---|
| `os_thread.cpp`, `os_scheduler.cpp` | creating, switching and choosing threads |
| `os_context.cpp` | saving and restoring a thread's registers |
| `os_alarm.cpp`, `os_time.cpp` | alarms, and the timebase everything is measured against |
| `os_interrupt.cpp` | the interrupt table and the handlers a game installs |
| `os_message.cpp` | message queues |
| `os_cache.cpp` | cache maintenance, which is where DMA-visible writes are declared |
| `os_init.cpp`, `os_reset.cpp`, `os_sleep.cpp` | startup, shutdown and idling |
| `os_report.cpp`, `trk.cpp`, `c_stdio.cpp` | a game's own logging and debug stubs |

Where the game's OS keeps its globals is the game's business: it installs their
addresses at startup through `wiinx/cpu/guest_os.hpp`.
