# PS5 test 20: independent review of the uncommitted diff

Read-only review. Nothing was built or run; every statement below comes from reading the source, the lifted
code under `src/recomp/gen` and the four audit documents (`ps5-audit-test19.md`, `ps5-audit-platform.md`,
`ps5-audit-pause.md`, `ps5-audit-input.md`).

Scope: the parent diff (`ps5/src/host_ps5.c`, `ps5_gamewatch.c`, `ps5_profile.c`, `ps5/cmake/ps5.cmake`,
`src/main.c`, `src/hooks/d3d11_translator.c`, new `ps5/include/ps5_quiet_fflush.h`) and the toolkit diff
(`win32_compat.c`, `kernel_bridge.c`, `kernel_hal.c`, `xbox_memory_layout.c`, `nv2a_pb_exec.c`, `apu_core.c`,
`apu_xaudio2.c`, `input_host.c`, `input_map.c`, `ohci.c`). Paths below are relative to the repo root; toolkit
paths are under `tools/xboxrecomp/src/`.

Verdict: no new freeze or crash found. The scheduling changes are bounded: every spin has a deadline or is
broken by a higher-priority thread. One change (668 for the audio writer) is likely to be partly cancelled by
an existing hold, and one (deadzone 0) will not take effect on a console that already has a settings.ini.
Items 3 and 4 are unverified risks; they are listed because a wrong guess makes the next run worse, not
because I could show them.

Severity words: freeze / crash / audible / wrong / minor.

---

## Findings, most severe first

### 1. audible: the 668 audio writer is still held by `kernel_gpu_preempt`, so the priority change cannot do what it was added for

- Where: `kernel/kernel_bridge.c:10560-10605` (the hold), called unconditionally from
  `kernel_thunk_dispatch_body` at `:10643`. The priority change it works against is
  `platform/win32_compat.c:1253-1266`.
- Scenario: `kernel_gpu_preempt()` returns early only when no GPU wait is pending, for an interrupt thread, or
  for a thread at DISPATCH_LEVEL or above (`xbox_IrqlPreemptible()`, `kernel_hal.c:318`). The audio writer (`sub_001242D0`) is an ordinary guest thread, so on every
  kernel call it makes while a GPU software method is waiting, or the executor is more than 0x40000 behind,
  it sits in a `SwitchToThread()` loop for up to 50 ms. The ring it refills is about 20 ms deep. The 668
  priority only wins the CPU; it does not stop the thread holding itself. During a heavy GPU frame the writer
  comes back late and the last lap replays, which is the crackle that 668 was meant to remove. A second
  effect: at 668, `sched_yield()` does not yield to the 700 threads on CPU 2 (the FreeBSD rule), so for the
  length of the hold the spin runs on CPU 2 and takes it from every 700 thread there. If the GPU executor
  or any other thread the wait depends on shares CPU 2 at 700, it is starved for the full 50 ms, after which
  the "executor not moving" check backs off for one second. That is bounded, not a freeze, but it can make
  the hold last its whole 50 ms every time instead of ending early.
- This was true before the diff too (the writer was held at 700), so the next run is not worse than the last
  one for this. It does mean test 20 may show the same crackle and it will look as if 668 did nothing.
- Smallest fix: skip the hold for a time-critical thread. In `kernel_gpu_preempt()` add
  a test of the calling thread's urgent flag next to the `on` test (`t_gc->urgent`, set per thread at
  `win32_compat.c:1244` and `:1270`). `t_gc` is not visible from `kernel_bridge.c` (no reference to it there),
  so export a one-line `int w32_thread_is_urgent(void)` from `win32_compat.c` and call it. A TC thread then never waits for the GPU, which is what a real
  time-critical thread on the console does.

### 2. wrong: the new deadzone default of 0 does not apply on a console that already has a settings.ini

- Where: default set at `input/input_map.c:295-304`; read back at `src/hooks/pc_input.c:104,106` (registered
  with `def.deadzone_left/right`) and `:169-170` (`geti("gamepad", "left_deadzone", 15)`); the file is written
  in full at `src/hooks/pc_settings.c:525-536` ("Write it out on first run, and after an upgrade adds a
  setting").
- Scenario: any console that ran test 18 or 19 already has `left_deadzone = 15` and `right_deadzone = 15` in
  its settings.ini. A stored value overrides the registered default, so the engine deadzone stays at 15 and
  the title adds its own 0.25 on top: the stick still needs about 62% of travel for a direction, which is
  what the audit wanted gone. The change is only seen on a fresh install. There is no environment name for
  these two keys (unlike the others, which `apply_env` overrides), so `RECOMP_*` cannot be used to force it.
  Secondary: the literal fallback `15` at `pc_input.c:169-170` differs from the PROSPERO default (0). It is
  only reached if the key is missing from the registered table, which does not happen today, but it is a
  trap for the next edit.
- Smallest fix, either: (a) tell the tester to delete `settings.ini` on the console before test 20 (cheapest;
  the file is regenerated); or (b) in `pc_input.c` under `__PROSPERO__`, ignore the stored values with
  `cfg->padmap.deadzone_left = cfg->padmap.deadzone_right = 0;` after the two `geti` calls. Also change the
  two fallbacks to `def.deadzone_left/right` so the three places agree.

### 3. wrong (unverified, medium-low): priority 668 is never taken back, and any title thread that asks for TIME_CRITICAL gets it

- Where: `platform/win32_compat.c:1253-1266`.
- Scenario: the block only ever raises. A thread that calls `SetThreadPriority(TIME_CRITICAL)` and later
  `SetThreadPriority(NORMAL)` (or any lower value) stays at RR 668 on the console, while the title believes
  it is normal. The title's thread class (`sub_001ED2D0`, `SetPriorityLevel`, levels 0..6, 6 gives TC)
  is reached from `sub_001ED420`, `sub_001ED4B0` and `sub_000D1280`. I read the callers: the constant levels
  at `recomp_0003.c:70208`, `recomp_0006.c:32440`, `recomp_0013.c:710` and `recomp_0013.c:27011` are 0, 1,
  2 and 2, so none asks for TC. One caller (`recomp_0006.c:16753`, via `sub_000D1280`) passes a value from
  `MEM32(esi + 0x20)` that I could not resolve. If that value is 6 for any object, the thread gets 668 on CPU
  2 and runs ahead of every 700 title thread; a thread of that kind that spins waiting on a 700 thread is a
  hang, because `sched_yield` will not yield down. The log line is capped at 4 (`told++ < 4`) so a fifth
  request would be invisible.
- Smallest fix: add an `else` branch that sets the thread back to 700 when `priority < THREAD_PRIORITY_TIME_CRITICAL`
  (same `getschedparam`/`setschedparam` pair, `sp.sched_priority = 700`), and log each TC request with the
  thread's start address instead of capping at 4. The second part answers the open question on the next log
  without another round trip.

### 4. wrong (unverified, medium-low): threads created by a 668 or 636 thread inherit that priority

- Where: `platform/win32_compat.c:1109-1111` (`thread_trampoline`: `getschedparam(self)` then
  `setschedparam(self, SCHED_RR, ...)`, which keeps whatever priority the creator had), and
  `kernel/kernel_bridge.c:644-645` (`xbox_PinToGuestCore`, same pattern).
- Scenario: the default pthread attribute is inherit-scheduling, so a thread created by the 668 audio
  writer, or by the 636 timer thread, starts at 668 or 636. The trampoline then re-applies RR at that
  inherited number. Whether the title or the runtime creates threads from those two threads at run time I
  did not find (the audio writer is a loop; the timer thread dispatches DPCs and APCs and does not create
  threads that I could see), so this is a risk, not an observed fault. The cost if it happens is the same as
  item 3: a thread that outranks the whole title.
- Smallest fix: in both places set `sp.sched_priority = 700;` before the `setschedparam` call. It is a
  one-line change and removes the dependence on who the creator was.

### 5. minor: the memory-layout watchdog ends in a no-op flush and `_exit(3)`, so its report is lost on the console

- Where: `kernel/xbox_memory_layout.c:2453-2454` (`fflush(stderr); _exit(3);`).
- Scenario: `ps5.cmake` includes the force-include header before `add_subdirectory(xboxrecomp)`, so this
  `fflush` becomes `ps5_quiet_fflush` and does nothing for stderr. `_exit` does not flush stdio. The
  watchdog's whole guest-stack dump (the `GS` lines above it) sits in the 64 KB stderr buffer and is dropped
  on exit, up to the last second of output. This only matters when the watchdog is armed, which is the case
  for a diagnostic run, and that is exactly when the dump is wanted.
- Smallest fix: `(fflush)(stderr);` on that line. The same applies to any other "flush then `_exit`/`abort`"
  site in toolkit code; this is the only one I found (`pc_settings.c:468-470` has the pattern but is not
  reachable on the console, which has no video launcher).

### 6. minor: ordering and coverage items in the exit and crash paths

- `ps5/src/host_ps5.c:892-900` (`ps5_exit_app`): `fprintf(stderr, ...)` comes before the flush. If the flush
  thread is inside its once-a-second `(fflush)`, which can hold the stream lock for about 0.3 s, the exit
  waits that long. Harmless, but the exit message should not be able to delay the return to the home screen.
  Fix: write the line with `ps5_logf` or `write(2, ...)`, or accept it.
- `ps5/src/host_ps5.c:498-509` (early fault handler): ends `sleep(2); _exit(1)` with no stdio flush. The
  crash note itself is written with a raw `write` to the crash fd, so the note survives; only the buffered
  stdout/stderr text of the last second is lost. Fix: `(fflush)(stderr);` before the `sleep`, if the handler
  is not entered from inside stdio (it can be, so keep it behind the existing `alarm`).
- `ps5/src/host_ps5.c` (log rotation): only one `.prev` generation is kept. A crash followed by two relaunches
  (a retry after a crash is the usual tester behaviour) overwrites the log of the crash. Fix: rotate
  `.prev` to `.prev2` before the move, or keep the older one if the newer is small (under 4 KB).
- `platform/win32_compat.c:1253-1266`: the interrupt/timer thread also calls `SetThreadPriority(TC)` first
  and so consumes one of the four "priority 668" log lines before it is moved to 636. The real audio writer
  line can fall out of the cap if a title thread also asks. Cosmetic; fixed by item 3's log change.
- `apu/apu_xaudio2.c:237-310` (fade ramp): the ramp length is derived from the frames available, so a very
  short buffer collapses the 64-frame ramp to a one-frame step. Rare (needs a submit under 64 frames while
  fading) and only audible as one click. No fix needed unless it shows in the log.
- `platform/win32_compat.c:1976-2040` (TSC clock): the 20 ms cross-check against the monotonic clock can fall
  back to the OS clock permanently after one descheduling blip. The fallback is correct, only slower to read;
  no timing error results. Fix if wanted: require two consecutive failures.
- `kernel/kernel_bridge.c` (`xbox_PinToGuestCore` and `kernel_timer_thread`) and the thread-pool
  `pthread_create` threads: they do not go through `thread_trampoline`, so they keep the creator's
  affinity (CPU 2, because `split_cpus()` ran first on the main thread and the guest threads moved there).
  They are all light waiters; I found no spin among them. If a future profile shows CPU 2 busy while the host
  CPUs idle, this is the place to look.

---

## Checked and found fine

1. Scheduling, TIME_CRITICAL to 668 (item 1 of the brief): lower number is higher priority and 668 is below 636
   (interrupt thread) in priority terms, so a spinning 668 thread cannot starve the interrupt thread; RR
   at equal priority rotates. The audio thread sleeps between refills. The precise `Sleep` path
   (`win32_compat.c:1327-1363`) ends in a `sched_yield()` spin of at most 0.2 ms per call, and that is only for
   an urgent thread, so the spin at 668 is negligible.
2. `kernel_timer_thread` (`kernel_bridge.c:3227-3260`): sets TC (668) then the absolute value 700-64 = 636, so
   the end state is 636. The change from "700 minus a constant relative to the inherited value" to an absolute
   value is correct, and the flush it used to do is removed under PROSPERO (`:3331-3347`) as intended.
3. `split_cpus()` (`host_ps5.c:840-877`): runs before any thread starts, so `g_ps5_guest_cpu` and
   `g_ps5_host_cpu_mask` are set before `thread_trampoline` and `xbox_PinToGuestCore` read them. A
   plain `pthread_create` thread keeps the main thread's host mask, which is the intent.
4. `bridge_KeSetBasePriorityThread` (`kernel_bridge.c:5105-5140`) and the `bridge_NtReadFile` guard
   (`:4225-4290`): no new blocking, no lock held across the guard.
5. `xbox_DispatchLockEnter` usleep path (`kernel_hal.c:270-300`): `usleep` is a sleep, not a spin, so it
   yields CPU 2 to every priority; the lock holder gets to run. No lost wake-up: the loop re-tests the lock.
6. `xbox_ReadTimeStampCounter` CAS fix (`kernel_hal.c:1208-1240`): monotonic, no double-advance under two
   readers.
7. Audio output thread at FIFO 650 (`apu_xaudio2.c:315-345`): it blocks in `sceAudioOutOutput` every 256
   frames, so it does not starve a CPU at 650 (it sleeps in the call). Ring sizes (RING_FRAMES 8192, PREROLL 3072, RING_MAX 5120): the maximum fill is below the
   ring size with room for one submit, and the pre-roll is below the maximum, so there is no deadlock between
   a full ring and a pre-roll that waits for more data.
8. Fade arithmetic (`XA2_FADE 64`, `g_last_out`, `g_fade_in`, `apu_xaudio2.c:237-310`): the ramp is applied
   once per underrun/recovery, the last sample is carried across callbacks, and gain stays within [0, 1].
9. Mixer period carry (`apu_core.c:583-594`): +1 microsecond every third period gives 5333.33 us as intended;
   no drift against the output thread, and the carry counter wraps harmlessly.
10. `ps5_quiet_fflush.h`: C only (guarded by `COMPILE_LANGUAGE:C`), so no C++ breakage; the static inline
    returns 0 for stdout/stderr and the real result for any other stream, so no caller sees a spurious error.
    I looked for callers that use the return value of `fflush(stdout/stderr)` and found none. Every real
    flush site that must write now (`ps5_fatal`, `ps5_exit_app`, `ps5_crash_note`, `fault_handler`, the flush
    thread) uses `(fflush)`, which is routed through the SDK `--wrap`. The header is included for the
    toolkit because `ps5.cmake` is read before `add_subdirectory(xboxrecomp)`.
11. `ps5_gamewatch.c`: the `pause_menu()` pointer chain is validated at each hop before the next read; the
    watch loop sleeps, so it cannot spin; the report window logic and log volume are bounded.
12. `host_ps5.c` pad bridge: `axis()` and `trigger()` conversions (including the script trigger rest value of
    -32768), the dead-pad release in `pad_update`, and the foreground ordering in `scan_pads`
    (`:171-233`). The chmod schedule and the `RECOMP_PS5_PROFILE=1` gate in `heartbeat_thread` are correct.
13. ExitProcess and TerminateProcess to `ps5_exit_app` (`win32_compat.c:2090-2104, 2286-2294`): the weak
    symbol test is correct, the exit code is passed through, and the one consumer that used `atexit`
    (`icall_feedback.c:164`, a diagnostic dump) is not needed on the console. `_exit(code)` after
    `sceSystemServiceLoadExec("exit")` is a safe fallback.
14. `main.c` `fault_handler` PS5 block (`:525-570`): `ps5_crash_note` first (raw write), `alarm(10)` as the
    safety net, `(fflush)`, notify, `sleep(3)`, `_exit(1)`. `alarm` is available in the SDK. A fault inside
    the handler ends in the alarm, not a hang.
15. `ohci.c:513` (`ohci_clock` split math): correct for the full 64-bit range; no overflow at the tick
    rates used.
16. `xbox_memory_layout.c:1708` PCRTC_START rate limit and the `d3d11_translator.c` 10 s report throttle: both
    only reduce log volume.
17. `input_host.c:895` and `input_map.c:295-304` trigger and deadzone code paths themselves (apart from
    item 2): no out-of-range value; 0 is accepted by the 0..90 setting range.

## Not examined

- No console run, no build, no measurement. Anything about timing (for example the real length of a GPU hold)
  is inferred from the code.
- `sub_000D1280`'s argument at `recomp_0006.c:16753` (item 3) was not resolved further.
