# PS5 audit: test 19 (parent 0bad32f, submodule 63d8cf4)

Read-only review. Nothing was built or run. The TSC frequency of the console was not available, so
horizons are given at 1.6, 2.0 and 3.5 GHz. "Guest" means the recompiled title, "host" means the runtime.

Result in one line: no crash-class defect found. The clock change is arithmetically safe everywhere
except one latent overflow (item 1), and its one real risk is that the TSC frequency is trusted without a
cross-check (item 2). The trigger fix is correct.

## Defects and risks, most severe first

1. **wrong behaviour (latent, one-off)** - `tools/xboxrecomp/src/usb/ohci.c:513`
   `scaled = (uint64_t)q.QuadPart * 1000u;` multiplies the raw counter. QPC is now the raw TSC since power-on
   (not an offset from process start), so the product passes 2^64 when the counter passes 1.8447e16:
   - 61.0 days of console uptime at 3.5 GHz
   - 106.7 days at 2.0 GHz
   - 133.4 days at 1.6 GHz
   (it was 213.5 days with the old 1e9 nanosecond counter).
   Effect when it wraps: `ohci_clock` runs backwards once, so HcFmNumber and the HCCA frame number jump, and
   `ohci_run_periodic` (ohci.c:1062-1076) sees `now - last` as a huge unsigned value and clamps it to 32
   frames, so the pad gets one 32-frame burst and then carries on. No crash and no hang. OHCI is on by default
   (`RECOMP_USB` is supplied in src/hooks/pc_settings.c:334-343). Realistic only for a console left up for
   months.
   Smallest fix: split the product the way `qemu_clock_get_us` does (apu_shim.h:56):
   `c = q.QuadPart; f = f.QuadPart; r = (c % f) * 1000u; millis = (c / f) * 1000u + r / f; phase = r % f;`
   (`r` is at most 3.5e12, no overflow).

2. **wrong behaviour if the assumption is false (unverified)** - `tools/xboxrecomp/src/platform/win32_compat.c:1955-1996`
   `ps5_tsc_hz()` takes `sceKernelGetTscFrequency()` on trust. Every clock the guest can see is now scaled by
   it: KeTickCount, KeQueryInterruptTime, KeQuerySystemTime, the 733 MHz rdtsc, the 60 Hz vblank, the PTIMER
   count, flip pacing. If the reported value differed from the real TSC rate by a factor k, the game would run
   k times too fast or slow. None of the new traces can show it, because [PTIMER], [VBLANK], [PACING] and
   [D3D] all read the same QPC. The only independent clock in the logs is the heartbeat's `sleep()`
   (host_ps5.c:739-749), and nobody compares them.
   Smallest fix: in `ps5_time_basics` (host_ps5.c:593) read the TSC around a roughly 50 ms
   `clock_gettime` interval, log "TSC measured N Hz, reported M Hz", and fall back to the clock_gettime path
   when they differ by more than about 2%. The log line alone is enough for the next test.
   Related, not verified: the code assumes the TSC is the same on every core. A cross-core skew would show up
   as a tiny negative `b - a` in d3d8_vk.c:2260-2263 and 2319-2323, which is added as a `uint64_t`. That wraps
   and recovers in the sum, so it would not show.

3. **cosmetic / misleading diagnostics** - `ps5/src/ps5_gamewatch.c:68-72, 83-84, 134-136`
   a. The report window closes after 480 samples, not after a second. `usleep(2000)` sleeps at least 2 ms
      (more with timer slack), so a "second" is 1.0 s or longer. `secs`, the tick count, the push and pop
      counts and the frame count are then per window while the log reads them as per second, so a
      60 ticks/s clock can look like 45. The ratios inside one line are still right.
   b. When `obj == 0` the block is skipped and `quiet` is not incremented, and when the second group of peeks
      fails the `continue` at line 84 skips the report check. Either way the log goes completely silent, which
      looks the same as "thread not running".
   Smallest fix: close the window on `sceKernelReadTsc()` or `clock_gettime` reaching 1 s, print the elapsed
   milliseconds in the line, and bump `quiet` before the `continue`.

4. **perf (low)** - `ps5/src/ps5_gamewatch.c:160`, started from `heartbeat_thread` (host_ps5.c:733)
   The thread is a plain `pthread_create` from the heartbeat thread, which was itself created by the main
   thread before anything is pinned (host_ps5.c:832). It inherits the full CPU mask including CPU 2 and the
   console's default scheduling policy. `g_ps5_host_cpu_mask` is applied only in `thread_trampoline`
   (win32_compat.c:1102), so it never reaches this thread. About 500 wake-ups a second, roughly 1 microsecond
   of work each, can land on the guest CPU. The cost is tiny, and heartbeat, flush and the 1 kHz profiler
   sampler (ps5_profile.c:160) are placed the same way, so this is not worse than what is already there.
   Smallest fix: after `sleep(5)` in `watch_thread`, if `g_ps5_host_cpu_mask` is non-zero call
   `scePthreadSetaffinity(scePthreadSelf(), g_ps5_host_cpu_mask)` (the mask is set at
   kernel_bridge.c:591-593 when the first guest thread pins itself).

5. **wrong behaviour (test pads only)** - `tools/xboxrecomp/src/input/input_host.c:895-896`
   `xbox_HostInputScriptVirtual` writes the scripted trigger as `held ? 32767 : 0`. For a virtual gamepad the
   rest value is -32768, so a released scripted trigger reads half pulled, which is the same bug that
   host_ps5.c's `trigger()` just fixed. Only the harness's virtual pads use it (RECOMP_PAD_SCRIPT with virtual
   pads); a real DualSense does not.
   Smallest fix: `held ? 32767 : -32768`.

6. **cosmetic, pre-existing, not made worse** - `tools/xboxrecomp/src/kernel/kernel_hal.c:1200-1204`
   First-call race in `xbox_ReadTimeStampCounter`: `freq` is published before `origin` is set. A second thread
   arriving between the two statements sees `freq != 0`, skips the init, and computes against `origin == 0`.
   It returns the raw uptime scaled to 733 MHz (days of ticks) once. The window was about 1 microsecond with the
   old syscall clock and is about 25 ns now, so it is less likely than before.
   Smallest fix: set `origin` first, then `freq`, or use an InitOnce.

7. **cosmetic, dead code** - `src/hooks/nv2a_regs.c:150-163`
   `ptimer_ticks` assumes a nanosecond QPC (`elapsed * num / den`, which would overflow after about 25 hours
   of process time at 3.5 GHz with the title's 56966 numerator). It is only reachable from
   `ptimer_special_read`, which no table entry references (the PTIMER entry at line 307 has a NULL reader), so
   it never runs. Delete it or fix it if it is ever revived.

8. **cosmetic, theoretical** - `tools/xboxrecomp/src/kernel/xbox_memory_layout.c:2088-2094`
   `xbox_PeekRead32` checks only `g_memory_base != NULL`. `g_memory_base` is set at the map call
   (about line 2604) and `g_memory_offset` about 40 lines later (2645). At exit `xbox_MemoryLayoutShutdown`
   (main.c:876) unmaps and then clears `g_memory_base` (3550-3552) while the watch thread keeps running. Each
   window is nanoseconds wide against a thread that is inside the read for about 0.5% of its time, and the
   thread starts 5 s after boot, so I would not spend effort here. If wanted: a volatile "guest memory ready"
   flag set after line 2645 and cleared before the unmap at 3550.

## Checked and fine

### The clock change

- `GetTickCount64` (win32_compat.c:1965): `(t / hz) * 1000 + ((t % hz) * 1000) / hz`. The remainder is below
  hz, so the product is at most about 3.5e12. No overflow for any realistic uptime.
- `QueryPerformanceCounter` returns the raw TSC as signed 64 bit: no overflow for 83 years at 3.5 GHz. Counter
  and frequency choose the TSC or the fallback from the same cached `hz`, so they cannot disagree. The
  fallback path is consistent (1e9 Hz, nanoseconds).
- The title does not call the host performance counter. Its kernel import table
  (`/home/claude/djdata/analysis/default_analysis.json`, 160 imports) has no KeQueryPerformanceCounter or
  KeQueryPerformanceFrequency (ordinals 126, 127). It reads time through `rdtsc`
  (`xbox_ReadTimeStampCounter`, rescaled to 733.33 MHz), KeTickCount, KeQueryInterruptTime and KeQuerySystemTime.
  So the guest never sees the TSC frequency or a raw TSC value, and cannot do 32-bit math on them. Those three
  kernel clocks are built from `xbox_GuestUptimeMs` (offset to a 10 s guest boot) or from an anchored delta.
- `xbox_ReadTimeStampCounter` (kernel_hal.c:1208-1213): `rem * 733333333` is at most 2.6e18 at 3.5 GHz, below
  1.8e19; safe up to about 25 GHz.
- `xbox_GuestUptimeMs` (kernel_hal.c:458-473) and `xbox_KeQuerySystemTime` (kernel_hal.c:520-535): the
  remainder is split from the quotient before scaling. Safe.
- `qemu_clock_get_ns` (qemu_shim.h:383-384): remainder times 1e9 is at most 3.5e18, below the signed 9.2e18;
  safe up to 9.2 GHz. `qemu_clock_get_us` (apu_shim.h:56-57): safe.
- `kernel_ptimer_tick` (kernel_bridge.c:2708-2721): `rem * rate` with the title's 56966/7629 ratio is about
  3.5e9 * 1.0e9 = 3.5e18, safe up to rate 5.3e9. The count it publishes is still a nanosecond counter,
  independent of the QPC frequency, and wraps its 32 bits every 4.29 s exactly as before. `secs * rate` is
  fine. KeTickCount (`(ULONG)xbox_GuestUptimeMs()`) wraps 49.7 days after process start, unchanged.
- `kernel_vblank_tick`, flip pacing (nv2a_pb_exec.c:575, 584: the delta is at most one period, so
  `delta * 1e9` is at most 5.8e16; it would only overflow at a flip rate below about 0.4 Hz), the GPU-hold
  loop (kernel_bridge.c:10562, 10580: bounded by 50 ms), the fence stall timers (xbox_memory_layout.c:1435,
  1553-1557), recomp_manual.c:869, and the doubles in d3d8_vk.c and d3d8_present.c all use deltas or floating
  point. Safe.
- `KeStallExecutionProcessor` (kernel_hal.c:578): `freq * MicroSeconds` is signed and would overflow only for a
  stall above 2.6e9 microseconds (43 minutes). Not reachable.
- `GetTickCount` callers (xbox_memory_layout.c, kernel_bridge.c:10649, d3d8_device.c:579, fb_present.c,
  video_pump.c) subtract as DWORD or unsigned, so wrap is handled. Not a regression: the old base was
  CLOCK_MONOTONIC, also uptime-like.
  Exception, pre-existing: `usb_gamepad.c:309, 318, 594` hold a 32-bit tick in an LP64 `unsigned long`, so
  `now - t0` explodes at the 49.7-day wrap; those are the RECOMP_PAD_PRESS and pad-script debug paths only.
- Nothing in the PS5 build compares a QPC or GetTickCount64 value with SDL_GetTicks or clock_gettime.
  `host_ps5.c:199` only stores `SDL_GetTicks`, and host_posix.c is not built for the PS5. Sleeps and
  timeouts in win32_compat.c use CLOCK_REALTIME or CLOCK_MONOTONIC internally and never compare against
  GetTickCount64.
- texture_pack.c:948 (the POSIX `now_ms`) still calls `clock_gettime`, and `texture_pack_bind` calls it at
  line 708 before its own active check. That is only reached through `try_texture_pack`
  (nv2a_pb_exec.c:2890), which returns early unless a texture pack is configured, so there is no per-bind
  syscall in a normal run. The two per-bind clock reads the commit message mentions are the GetTickCount64
  calls at nv2a_pgraph_d3d11.c:972 and 981, which are now cheap.
- `sceKernelReadTsc` and `sceKernelGetTscFrequency` are exported by libkernel (PS5_PayloadSDK
  sce_stubs/libkernel.c, kernel.h:68-69, same `uint64_t f(void)` signatures as the extern declarations), and
  test19-llvm-pie.elf imports both. fifa.imp (another port) imports both too.

### ps5_gamewatch.c

- Every address it reads is below 64 MB and inside `peek_readable`'s main-RAM window. The window test uses
  `va + 4 <= base + size` in 64 bits, so a dword read cannot straddle the end. The constants are 4-aligned;
  the help-popup byte at +0x185 is read through its containing dword.
- Guest pointers (`obj`, `mgr`) are not checked in the watch itself, only by that window test. A bad value
  below 0x10000 is rejected. A bad value inside the NV2A, MCPX or flash apertures would pass the test and touch
  a trapped register page from a non-guest thread, but both are heap pointers in practice. Hardening, if
  wanted: skip when `obj` or `mgr` is outside [0x10000, 64 MB).
- No division by zero: the only arithmetic is `% 10`, guarded by `r < 10 && last_r < 10`. Tearing between
  the read and write indexes only produces an occasional off-by-one count in a diagnostic.
- Format string: 23 conversions, 23 arguments, types match; the line is about 0.4 KB, under the 768-byte buffer
  in `ps5_logf`. Log volume is one such line a second, about 1.5 MB an hour into ps5.log, written with one
  `write()` from this thread only, so a slow disk write stalls only the watch.
- CPU cost: about 15 peeks per 2 ms sample, roughly 0.3 microseconds of work, well under 0.1% of a core.

### Trigger fix (`ps5/src/host_ps5.c:100-103`)

`(int)v * 65535 / 255 - 32768`. Since 65535 = 255 * 257 the integer division is exact, so this is
`v * 257 - 32768`, which spans -32768..32767. The maximum intermediate is 255 * 65535 = 16.7 million, no
overflow, and the cast to `Sint16` is always in range. SDL's virtual joystick starts trigger axes at
SDL_JOYSTICK_AXIS_MIN for a GAMEPAD-type descriptor (virtual/SDL_virtualjoystick.c:245-250) and the mapping
`lefttrigger:a4` binds input -32768..32767 to output 0..32767 (SDL_gamepad.c:1736-1742, 3471-3473). Simulated
with the same float arithmetic:

| v   | axis written | gamepad value | per 255 |
|-----|--------------|---------------|---------|
| 0   | -32768       | 0             | 0.00    |
| 1   | -32511       | 128           | 1.00    |
| 254 | 32510        | 32638         | 254.00  |
| 255 | 32767        | 32767         | 255.00  |

The old `v * 32767 / 255` rest value was 16383 (127.5 of 255), which confirms the commit's diagnosis.
Residual risk, unverified: the title's held threshold is 0.01 (about 2.6 of 255), so a DualSense trigger that
rests at 3 or more would still count as held; resting at 0 to 2 does not.

### ps5_time_basics

`tsc_hz` is `uint64_t` and printed with `%llu` and a cast; the format string has 10 conversions and 10
arguments; block-scope externs match the SDK declarations; the `volatile` sink stops the loop being removed.
The measured per-call figure carries about 4% overhead from the surrounding `clock_gettime` pair, which does
not matter at the precision printed.

### RECOMP_PTIMER_TRACE by default

It only adds prints: one [PTIMER] line every 5 s, one [VBLANK] line every 10 s, and the two anomaly lines
limited to one a second. It does not change control flow in `kernel_ptimer_tick` or `kernel_vblank_tick`.
These run on the high-priority interrupt thread through stderr, the same path other lines on that thread
already use.
