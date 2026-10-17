# PS5 audit: audio output, scheduling, robustness

Read-only review of the console port. Nothing was built or run, no source was changed.

Evidence used:

- The test 18 logs: `defjam.log.err` (33,085 lines, 1,959,045 bytes, about 138 s of play) and `ps5.log`
  (14 `[PROF]` blocks). The profile blocks were summed and named against `test18-llvm-pie.elf`
  (`--functions=linkage`, address minus 0x400000).
- The source tree as it stood at 02:45 on 2026-10-17. That is test 19 plus uncommitted work (helper threads
  moved off the guest CPU, TSC cross-check, pad fixes). The files are still being edited, so line numbers
  can drift: use the function names.

The logs predate the tree. Where the tree already fixed something the log shows (QPC and GetTickCount now
read the TSC; helper threads call `ps5_off_guest_cpu`), the finding says so.

Tags: **observed** = visible in the log or profile, **code** = read in the source, **hypothesis** = reasoned,
needs the measurement named in the last section. Severity: crash / audible / stutter / minor.

## Findings, most severe first

| # | Severity | What |
|---|---|---|
| B1 | stutter, audible | The interrupt thread flushes both log streams once a second and spends about 14% of its time in it |
| B2 | stutter | Every `NtReadFile` logs a line and flushes on the guest thread |
| B3 | stutter | About 195 other `fflush` call sites; the flush holds the stderr lock for the whole write |
| B4 | audible | The title's time-critical audio writer gets no real priority and shares CPU 2 with two fence spins |
| B5, A5 | audible, stutter | Audio, mixer, tick, GPU executor and USB threads start before the CPU split and are never moved |
| A1 | audible | An underrun or overrun is a hard cut: zero fill mid-wave, 43 ms mute, then full-scale restart |
| A2 | audible | Mixer period is 5333 us, not 5333.33: +62.5 ppm, one dropped 21 ms chunk every ~6 min after ~11 min |
| A3 | audible | The three `[XA2] the device ran dry` lines follow late interrupt-thread timers |
| A4 | audible | 21 ms of margin is thin for this host |
| C1-C3 | minor | Crash handler through stdio, five different exit paths, saves written in place and never synced |
| C4, C5 | minor | chmod walk every minute; log rate 14.9 KB/s, about 107 MB per 2 h |
| A6, A7, B6-B8 | minor | See below |

---

## A. Audio output

How it is wired (code): VP voices -> `mcpx_apu_dsp_frame` (six-bin downmix, `apu_dsp.c:250-274`) ->
`mcpx_apu_monitor_frame` collects 4 x 256 frames and calls `xa2_submit_samples` (`apu_core.c:399-428`) ->
ring (`RING_FRAMES` 8192, `XA2_PREROLL` 2048, `XA2_RING_MAX` 4096) -> `ps5_out_thread` pulls 256 frames
and calls `sceAudioOutOutput`, which blocks at the device rate (`apu_xaudio2.c:262-276`). The mixer thread
`mcpx_apu_frame_thread` (`apu_core.c:642`) is paced from the TSC in `throttle()` (`apu_core.c:540`): an
absolute deadline per period and per slice (D120 item 3, on for every non-Windows build).

Format check (code): `sceAudioOutOpen(0xFF, 0, 0, 256, 48000, 1)` is the main port, 256-frame grain,
48 kHz, signed 16-bit interleaved stereo (L then R). The mixer writes int16 L,R at 48 kHz. Format, order
and nominal rate agree. Volume is never set, so the port plays at full scale.

SDL comparison: `sdl_callback` is the same function on desktop and PS5. SDL has its own device queue
(512 frames on SDL2, a stream on SDL3) between that callback and the hardware. On the PS5 there is only the
console's grain, so the PS5 has less slack for the same ring logic. D120 items 2 and 3 (`VRS_CHUNK` 8,
per-slice pacing) are active here, and so is the two-submission preroll described at `apu_xaudio2.c:197-203`.
D121 and D122 (tick thread, precise sleep, guest-CPU token priority for the audio writer) are only partly
active: the token is macOS only and nothing replaces it on the PS5 (B4). D120 also found that on the Deck the
remaining crackle was in the game's own mix (the writer late, the voice replaying the previous lap) and not in
the device, so `[XA2] ran dry` is only part of what can go wrong.

### A1. Underrun and overrun are hard cuts (audible)

`apu_xaudio2.c:244-252` (callback), `:444-447` (submit).

- Underrun: the callback copies what is left, zero fills the rest of the grain mid-waveform, sets
  `g_primed = 0` and plays silence until 2048 frames are queued again (at least two submissions, 43 ms),
  then restarts at full amplitude. That is a step at the start of the hole, a step at the end, and a hole
  longer than the stall that caused it.
- Overrun: when `have + n > XA2_RING_MAX` the whole new 1024-frame chunk is dropped (`g_xa2_dropped`),
  which skips 21 ms of the mix and clicks.

Smallest fix: keep the last output frame, and when the callback runs short or leaves silence apply a
64-frame linear fade (1.3 ms) from that frame to zero, and the same fade-in on the first frames after
silence. About 20 lines inside `sdl_callback`. It does not remove the gap, it removes the two clicks.

### A2. The mixer runs 62.5 ppm fast (audible, long sessions)

`apu_regs.h:363` (`EP_FRAME_US 5333`), `apu_core.c:583` (`next_frame_time_us += EP_FRAME_US`),
`apu_core.c:546` (slice deadline).

256 frames per 5333 us is 48003.0 frames per TSC second against 48000 consumed. The ring gains 3 frames a
second. In steady state it cycles between about 1024 and 2048 frames; a submission is refused when the
level before it exceeds 3072. That is about 11 minutes after start, then one dropped 1024-frame chunk every
1024 / 3 = 341 s. The log is 138 s long and shows `0 submissions refused`, which fits. Any extra offset
between the console's audio clock and the TSC adds to or cancels this; the sign is unknown (hypothesis), and
if the device runs more than 62 ppm fast the ring drains instead and A1 hits every few minutes.

Smallest fix, three lines in `throttle()`: carry the fraction.

```c
static unsigned n;                                  /* periods since start */
d->next_frame_time_us += EP_FRAME_US + (++n % 3 == 0);   /* 5333, 5333, 5334 */
...
due = period_base_us + (int64_t)(d->ep_frame_div % 8) * 2000 / 3;
```

If the next run (M3) shows the fill level still wandering, add a trim: each time a submission lands, read
the fill and nudge the next deadline by `clamp((fill - 2048) / 16, -100, +100)` us. That follows the device
clock, whatever it is. The cost is that the voice processor's clock then drifts from the TSC by the same
ppm the device does. D121 says the title's writer keeps about 990 samples ahead of the play cursor, so it
should follow the voices rather than the tick count, but that was not checked here; if it follows the tick
count, 62.5 ppm eats the 990-sample lead in 990 / 3 = 330 s, which the fraction fix above also removes.

### A3. The dry events follow interrupt-thread stalls (audible, inferred)

The three lines, with the audio time they correspond to (frames written x 21.33 ms) and the nearest late
title timer:

| `[XA2] the device ran dry` | err line | audio time | nearest `[TIMER] ... us past` |
|---|---|---|---|
| gap 1 | 5943 | 28.3 s | 5877: 15,570 us |
| gap 2 | 6256 | 30.2 s | 6185: 5,296 us |
| gap 3 | 6363 | 31.0 s | 6357: 8,224 us |

All three sit in one scene transition, 6 to 71 lines after a late frame-clock callback. The mixer does not
depend on the interrupt thread, except through the front-end trap wait: when the front end is trapped the
frame thread waits up to 20 ms for the title to service the APU interrupt (`apu_core.c:739-777`), and that
interrupt is delivered by the 636 thread (`kernel_bridge.c:3300-3315`), the same thread that is stuck in
`fflush` in B1. A wait that expires plays silence for the rest of the period. This is consistent with the
log, not proven: `RECOMP_APU_LEVEL` was off, so the `front-end stops waited out: N (M outlasted the wait)`
line that would settle it was not printed (M2).

Fix: B1. Nothing to change here until the next log says otherwise.

### A4. Margin (audible, cheap mitigation)

After a normal start the ring holds 1024 to 2048 frames, so the mixer can be about 21 ms late before the
device runs dry. The observed delays on the interrupt side are 5 to 15 ms, with outliers of 160 to 283 ms
(err lines 14366-14369).

Smallest fix, PS5 only: `XA2_PREROLL (3 * XA2_BUF_SAMPLES)` and `XA2_RING_MAX (5 * XA2_BUF_SAMPLES)`
(`apu_xaudio2.c:201-202`). `RING_FRAMES` 8192 holds it. The margin goes to about 43 ms and the latency goes up
by 21 ms (roughly 50 to 70 ms, estimated). That is a trade: fewer gaps for a later hit sound. The alternative that does
not add latency is to submit every 256 frames instead of every 1024 (the `acc` collection in
`mcpx_apu_monitor_frame` exists for XAudio2's buffer size, not for the PS5) and use a preroll of 1280,
which keeps 21 ms of margin and removes about 15 ms of average latency. Try the first one, because it is a
two-line change, and decide after M2.

### A5. Audio threads are not on the host CPUs (audible, hypothesis)

`ps5_out_thread` is made with plain `pthread_create` (`apu_xaudio2.c:311`); the mixer through `CreateThread`
(`apu_core.c:899`). Both start inside `xa2_init` / `mcpx_apu_init_standalone`, before the first guest thread
pins itself and publishes `g_ps5_host_cpu_mask` (`kernel_bridge.c:591-593`). Log order: err line 81
`[XA2] PS5 audio output initialized`, line 106 `CPU mask 0x1FFF`. `thread_trampoline` only moves a thread
when the mask is already set (`win32_compat.c:1102`), so both keep the full mask (CPU 2 included) and the
console's default policy (FIFO, 700, err line 107). The new `ps5_off_guest_cpu` is called only from the
heartbeat, flush, profiler and game-watch threads.

The device has about one grain (5.3 ms) of its own slack (hypothesis, the SDK does not say), so an output
thread that waits 6 ms behind a round-robin guest thread on CPU 2 glitches the audio without ever emptying
our ring. `[XA2] ran dry` does not count that: only a ring underrun is counted.

Negative evidence: `audio out` and the mixer each have 4663 profile samples with `not running 0`, so
neither was found stuck for more than 4 to 8 ms at any sample (the sampler waits 40 polls of 100 us). That
rules out regular starvation, not three rare events in 140 s.

Smallest fix: B5 (set the host mask before any thread exists), plus `ps5_off_guest_cpu()` at the top of the
`ps5_out_thread` loop, because that thread does not go through `thread_trampoline`. Also give the output
thread a priority above the guest's (for example `SCHED_FIFO` 650, it blocks 99% of the time) and log
the result of `pthread_setschedparam`.

### A6. `sceAudioOutOutput` result is ignored (minor)

`apu_xaudio2.c:273`. If the call ever fails at once (port closed, bad pointer, audio service restarted) the
loop spins, pulls the ring dry in a few milliseconds and keeps a CPU at 100% for the rest of the run.
`buf` (`:264`) has natural alignment only; the SDK documentation was not checked for a required alignment,
and audio works, so it is met in practice. Fix: check the return, log the first non-zero value, `usleep(2000)`
on failure; add `__attribute__((aligned(256)))` to `buf` at no cost.

### A7. Clipping and truncation (minor)

`apu_dsp.c:264-274`: the six-bin downmix is scaled by 0.7, hard clamped to +/-1 and truncated to int16. The
sum of up to six bins can clip in a loud fight, and there is no limiter. This is shared with the desktop
build. `RECOMP_APU_LEVEL=1` prints the peak of each second; if it reads 32767 in fights, add a soft knee
above 0.9 before the clamp.

---

## B. Scheduling and stalls

### B1. The interrupt thread flushes the logs (stutter, audible; observed)

`kernel_bridge.c:3322-3327` in `kernel_timer_thread`:

```c
static long long last_flush;
if (now - last_flush >= 1000) { last_flush = now; fflush(stdout); fflush(stderr); }
```

It is not guarded for the PS5, and `flush_thread` (`host_ps5.c`) already does the same every second.

Evidence:

- Profile, "guest 1" (the second thread through `xbox_PinToGuestCore`: err lines 128 and 130 show it taking
  the pin message and then `interrupt thread priority 636`): 4632 samples, 447 in `write` from
  `__wrap_fflush` (9.6%) and 195 waiting on a lock inside `__wrap_fflush` (4.2%). About 14% of its wall time.
  At one flush a second that is about 140 ms per flush on average, against 0.83 ms for a tiny line measured at
  start (err line 30).
- The title's frame clock callback is delivered late several times: `[TIMER] callback 0x000D7EA0 asked for a
  time N us past` at err lines 2448 (2.6 ms), 5877 (15.6 ms), 6185 (5.3 ms), 6357 (8.2 ms), 7969 (11.9 ms),
  14366 (**283 ms**) and 14369 (162 ms). The cause of each was not proven; this thread is the one that
  delivers them and it is the one stuck in the log write.
- `[PACING]` maximum present intervals of 213 ms (err line 11357) and 308 ms (line 14490).
- The title is held for this thread by design (`[GPU] title held for the GPU's interrupt: 10000 times,
  28115 ms in all`, err line 24661, 2.8 ms a time), so any time the 636 thread spends in a write comes off
  the frame directly.

Smallest fix, one line: wrap the block in `#if !defined(__PROSPERO__)`, as the neighbouring code in
`xbox_memory_layout.c:1686-1717` already does for its flushes.

### B2. `NtReadFile` flushes on the guest thread (stutter; observed)

`kernel_bridge.c:4210-4250` in `bridge_NtReadFile`: every read formats a `[READ]` line and runs
`fputs(line, stderr); fflush(stderr);` (4243-4244). `guest_turn_leave/return` around it are no-ops on the PS5.
There is no rate limit: 4663 `[READ]` lines in 138 s, 19% of the log bytes. "guest 4", the streaming
thread, is 8.5% in `__wrap_fflush` (385 + 4 of 4591 samples). A scene load is a burst of reads, each costing
a flush that is at least 0.8 ms and sometimes 0.3 s, with the stderr lock held for it. D122 (3) moved this
write off the macOS guest-CPU token; on the PS5 there is no token, so the thread itself still waits.

Smallest fix: delete the `fflush` on the PS5 (the flush thread covers it), and gate the whole line behind
`RECOMP_READ_LOG` (default off), since the first bytes of each read are a bring-up diagnostic.

### B3. Other flush sites, and the lock the flush holds (stutter; code, partly observed)

There are about 195 `fflush(stderr|stdout)` calls in `src/` and `tools/xboxrecomp/src`; 54 in
`kernel_bridge.c` alone, 21 in `ohci.c` (the USB thread, 69 profile samples = 1.5% in `__wrap_fflush`), 13 in
`xbox_memory_layout.c`, 9 each in `usb_gamepad.c` and `nv2a_pb_exec.c`. Most are after rare messages; the hot
ones have been guarded one at a time (`xbox_memory_layout.c`, `d3d11_translator.c:727`). The SDK's
`__wrap_fflush` adds only a short list lookup under a global mutex after the real flush
(`PS5_PayloadSDK/platform/src/memstream.c`); the long wait is inside `__real_fflush`, on the FILE lock.

Smallest fix, covering all of them: a header force-included into the game targets on the PS5 only,

```c
#define fflush(f) ps5_log_fflush(f)
static inline int ps5_log_fflush(FILE *f)
{ return (f == stdout || f == stderr) ? 0 : (fflush)(f); }   /* the flush thread does these */
```

and `(fflush)(...)` in the places that must really flush: `flush_thread`, `host_posix_run`'s tail,
`ps5_fatal`, the crash handler. Add `-include .../ps5_log_fflush.h` to the `defjam_recomp` and `xboxrecomp`
targets in the PS5 CMake.

Remaining issue, and the reason this is not the whole answer: `fflush(stderr)` on the flush thread holds the
stderr FILE lock for the entire `write()`, so any thread that logs during that time waits for it. Cut
the volume first (C5, which also makes each write smaller), move the flush thread to every 5 s, and time the
flush (M1) before building anything heavier. Heavier would be a ring buffer behind `stderr->_write`
(the BSD FILE hook); the SDK says `funopen` and `fopencookie` are not exported, and the FILE layout of Sony's
libc was not checked, so treat that as an experiment.

### B4. The title's audio writer has no real priority (audible; code and observed, effect inferred)

`SetThreadPriority` only records the value on the PS5 (`win32_compat.c:1235-1246`);
`bridge_KeSetBasePriorityThread` (`kernel_bridge.c:5077-5110`) feeds it. Only the interrupt thread is given
a real priority (`kernel_bridge.c:3230-3246`, 700 - 64 = 636). The log shows exactly one thread asked for
TIME_CRITICAL: err line 932, object `0x01404120`, increment 16.

That is the audio writer (D121). In the profile "guest 7" is 3960 of 4576 samples in `Sleep`'s `nanosleep` and
171 (3.7%) in the `sched_yield` tail of the precise-sleep path (`win32_compat.c:1301-1335`), which only a
TIME_CRITICAL thread takes. The writer refills a ring on a 10 ms grid about 21 ms ahead. On CPU 2 it is one
of nine round-robin guest threads, and the main thread ("guest 0", 4696 samples) spends 67% of its time in two
fence waits:

- `sub_0021EB90_enter` (`src/recomp_manual.c:842-880`): a `SwitchToThread()` loop, up to 100 ms, 1871 samples
  (40%) in `sched_yield`. It hands the CPU on every 3 us, so it is harmless to a waking writer.
- `sub_0021EC50` at 0x0021ED87 (`docs/research/d3d-pushbuffer-wrap.md`): a tight loop with no yield, about
  1280 samples (27%). A thread waking from sleep waits behind it until the round-robin quantum ends. The
  quantum was not measured; the comment at `kernel_bridge.c:3230-3237` ("9 ms a time") suggests about 9 ms.

One late wake of about 9 ms is inside the 21 ms lead. Two in a row, or one on top of a delay elsewhere, is the
"previous lap replayed" hole of D121. Those holes are not counted by `[XA2] ran dry`; `scripts/audio-holes.py`
finds them in a PCM capture (M2).

Smallest fix, in the PS5 branch of `SetThreadPriority`, for the target thread (not only the caller):

```c
#if defined(__PROSPERO__)
    if (o && o->kind == K_THREAD && priority >= THREAD_PRIORITY_TIME_CRITICAL) {
        struct sched_param sp; int pol;
        if (pthread_getschedparam(o->thread, &pol, &sp) == 0) {
            sp.sched_priority -= 32;                      /* 668: above the title, below the 636 thread */
            pthread_setschedparam(o->thread, SCHED_RR, &sp);
        }
    }
#endif
```

The interrupt thread sets 636 after its own `SetThreadPriority`, so it stays above. The writer sleeps
almost all the time, so it cannot starve the others. Its `sched_yield` tail then spins for 0.2 ms a wake
(about 2% of CPU 2), because nothing of equal priority is queued to yield to; shorten that tail to 0.1 ms on the
PS5 if it shows up. Log the result of `pthread_setschedparam` once.

### B5. Helper threads created before the CPU split (stutter; code and observed)

Same cause as A5, wider. `g_ps5_host_cpu_mask` is set when the first guest thread pins itself (err line 106).
Created before it, so never moved (log lines in brackets): the tick thread (71,
`xbox_memory_layout.c:1823-1829`), the GPU executor `nv2a_ack_thread` (right after,
`xbox_memory_layout.c:1838-1839`; 27% of its samples are user code, the busiest helper), the APU frame thread
and audio output (81-82), the USB controller thread `ohci_thread` (87; input reports, the thing the
`win32_compat.c:1076-1082` comment says was late). The helper-thread comment there describes exactly this
problem for threads started after the pin.

Smallest fix: set the mask once, before the first `pthread_create` in `host_posix_run` (after
`ps5_time_basics`): `scePthreadGetaffinity` on the calling thread, and if CPU 2 is in it,
`g_ps5_host_cpu_mask = mask & ~(1ull << 2)`. `thread_trampoline` then places every `CreateThread` thread. The
`pthread_create` ones (audio output) call `ps5_off_guest_cpu()` in their loops. Guest threads pin themselves
again as they start, so nothing else changes. Log affinity and policy of the audio-out, mixer, tick, ohci
and nv2a_ack threads once after 15 s (M4) so the result is visible.

### B6. DPC runner can invert priority against a guest thread (minor; code, not seen)

`xbox_DispatchLockEnter` (`kernel_hal.c:269-290`) is used by the 636 thread to run a DPC
(`kernel_bridge.c:2248-2256`). If a guest thread is at DISPATCH_LEVEL it holds `g_dispatch_cs`; the DPC runner
tries 50 times with `SwitchToThread()`, then with `Sleep(0)`, and from try 1000 on adds `Sleep(1)`, for at
most 200 ms. On the PS5 `sched_yield` from a 636 thread does not hand the CPU to a 700 thread, so the first
1000 tries (about 3 ms) burn CPU 2 at the higher priority while the holder, on the same CPU, cannot run. It
is not seen: "guest 1" has no `sched_yield` samples in the profile and there is no
`[IRQL] a thread stayed at DISPATCH_LEVEL` line. Fix when convenient: after 8 tries use `usleep(100)`.

### B7. The profiler and game watch run all the time (minor; code)

`heartbeat_thread` starts `ps5_prof_start()` and `ps5_gamewatch_start()` unconditionally. The sampler
(`ps5_profile.c:134-168`) sends about 600 `SIGUSR1` a second; each one runs a handler on a thread that may be
a guest thread on CPU 2, and a thread that does not answer is waited for with `usleep(100)` polls. The game
watch wakes 500 times a second (`ps5_gamewatch.c:92`). Both now move themselves off the guest CPU; the signals
still land on it. Once the findings here are acted on, start both only when `RECOMP_PS5_PROFILE=1` /
`RECOMP_PS5_WATCH=1`.

### B8. `WaitForMultipleObjectsEx` polls (minor; code and observed)

`win32_compat.c:915-951` loops over `WaitForSingleObject(h, 0)` and `usleep(1000)` (about 1.1 ms on this
console). "guest 3" spends 4409 of 4601 samples there: about 900 wakeups a second for one idle thread and up to
1.1 ms of added latency on whatever it waits for. Smallest fix: sleep 250 us for the first 10 ms of a wait,
then 1 ms.

Other sleeps with 100 us granularity: slice pacing `usleep(rem)` at `apu_core.c:555` (up to +99 us per slice
against absolute deadlines, so it does not accumulate), the sampler's `usleep(100)` polls, the game watch
`usleep(2000)`. None needs changing for sound.

---

## C. Robustness

### C1. The crash handler goes through stdio (crash path; code, not seen)

`src/main.c` `fault_handler` (about 505-558, PS5 branch at the end): about fifteen `fprintf` calls to stderr,
`fflush`, `ps5_notify`, `sleep(3)`, `_exit(1)`. If the faulting thread already holds the stderr lock, or the
flush thread is in one of its 0.3 s writes, the report waits; a fault inside `fprintf` itself deadlocks and the
console is left with a frozen app. The early handler in `host_ps5.c` (`early_fault`) does it properly with raw
`write()` to `s_crash_fd`, but the runtime's handler replaces it for SIGSEGV. The 3 s sleep also leaves the
other threads running, which can be in the middle of a file write when `_exit` arrives (C3).

Smallest fix: at the top of the PS5 branch write one line to `ps5.log` with the raw `write` (export a
`ps5_crash_write(const char *)` from `host_ps5.c`), and start a 5 s `alarm` whose SIGALRM handler is
`_exit(1)`, so a stuck report still ends the process. Not seen in any log so far.

### C2. Exit paths differ (minor; code, hypothesis)

- Normal return: `host_posix_run` tail flushes, `open_up_app_files()`, `sleep(1)`,
  `sceSystemServiceLoadExec("exit")`.
- `ps5_fatal`: flush, notify, `sleep(3)`, `LoadExec("exit")`, `_exit(1)`.
- Guest asks to leave (`ExitProcess`, `KeBugCheck*`, `HalReturnToFirmware`, `kernel_hal.c:613-697`):
  `exit()` (`win32_compat.c:2064`). `TerminateProcess` (`:2246`) likewise.
- Crash handlers: `_exit(1)` only.

`exit()` runs the C library teardown with the audio output thread inside `sceAudioOutOutput` and the GPU thread
inside the Vulkan driver, and never reaches `LoadExec`. Whether the console then returns to the home screen,
hangs, or leaves a black screen has not been tested here. Smallest fix: one `ps5_exit_app(int)` in
`host_ps5.c` (real flush, chmod, `LoadExec("exit")`, `_exit`) called from all of the above.

### C3. Saves are written in place and never synced (minor, but the only item that can lose data)

The save files are plain host files under `save/UserData` and `save/TitleData`
(`kernel_path.c:64-66`). A write open with SUPERSEDE, OVERWRITE or OVERWRITE_IF truncates the file
(`kernel_file.c:795-806`); `fsync` runs only on an explicit `NtFlushBuffersFile` (`kernel_file.c:1159`), not
on close. A power cut, a console crash, or the `_exit` in C1 between the truncate and the end of the title's
rewrite leaves a short or empty file. No log shows it happening, and the game saves correctly.

Smallest fix: in the PS5 `NtClose` path, `fsync` any descriptor that was opened for writing (saves are
small, and this runs on a guest thread, so check it with M1's timing first), and at start copy `save/UserData`
to `save/UserData.bak` when every file in it is non-empty. That gives a manual recovery.

The `Partition0..5.img` files are not saves. They are sparse stand-ins for the disk the title opens: the runtime
writes only the partition table sector (`kernel_path.c:677-682`) and extends the others with `ftruncate` to
2.3 GB, 0.5 GB and 3 x 0.77 GB (`:650-692`), 5.1 GB apparent. If the save folder is on a file system that does
not keep holes, that allocates 5 GB at first start. Check with `du` against `ls -l`. Whether the title ever
writes the raw device was not checked.

### C4. The chmod walk (minor; code)

`open_up_app_files` / `open_up_tree` (`host_ps5.c`) run on the heartbeat thread at 10 s, then every 60 s for
the rest of the session, over `save`, `logs` and `radv-shader-cache` (depth up to 6), with an unconditional
`chmod` per file and directory. On a shader cache with thousands of files that is thousands of metadata
syscalls a minute on a file system where a write is sometimes 0.3 s. Files made in the last minute before a
kill keep their original mode, so the FTP user still cannot delete them.

Smallest fix: compare `st.st_mode & 0777` (already in the `lstat` result) with the wanted mode and chmod only
when different; run every 300 s after the first 5 minutes. Three lines.

### C5. Log volume (minor; observed)

stderr rate over the last two thirds of the run (45 `[PACING]` windows, 90 s, 1,339,458 bytes): **14.9 KB/s**,
260 lines/s. The whole file averages 14.2 KB/s. Projected size:

- one hour: 54 MB, two hours: **107 MB** for `defjam.log.err`; `ps5.log` adds about 2 to 4 MB.
- not a space problem for the console. It is a flush problem: the bytes per flush are the same at any time of
  the run, so the cost does not grow with the file, but B1 to B3 show what each flush costs. Appending to a
  100 MB file instead of a 2 MB one may be slower on this file system; that was not measured.

What it is made of (steady part): `[TRANS]` frames 44.3%, `[READ]` 19.3%, `[NV2A]` 13.6%, `[GPU]` 11.4%,
`[KERNEL]` 4.8%, `[OHCI0]` 4.1%. The first four are per frame or per read, 88.6%:

- `[TRANS] frames` (`src/hooks/d3d11_translator.c:723`): one line per changed counter, so every frame.
- `[NV2A] PCRTC_START` (`xbox_memory_layout.c:1712`): on every flip.
- `[GPU] draw #` (`nv2a_pb_exec.c:3104`): every 200 draws, about once per frame.
- `[READ]` (B2).

Smallest fix: limit each of the first three to one line a second, and `[READ]` off. The rate falls to about
1.7 KB/s, 12 MB in two hours.

Two related points. `freopen(path, "w")` in `log_to_file` and `O_TRUNC` on `ps5.log` mean the next launch
erases the previous run's logs, so a crash followed by an immediate relaunch loses the crash log: rename to
`.prev` first. And the pipe pump in `log_to_file` is dead code on this console (`logs through 0 pipe(s)`),
with comments that still describe it; harmless.

---

## Measurements for the next run

1. **M1, flush cost.** Time `fflush(stderr)` in `flush_thread` and print to `ps5.log` when it takes more than
   20 ms, with the bytes it wrote. This decides whether B3 needs more than B1 and C5.
2. **M2, APU counters and the mix.** Run with `RECOMP_APU_LEVEL=1`: peak per second (A7), `device ran dry`
   count, and the `front-end stops waited out ... outlasted the wait` line every ten (A3). For the holes the
   writer causes (B4), capture the mix and run `scripts/audio-holes.py` on it. `RECOMP_APU_PCM=<file>` does that
   with 192 KB/s of stdio writes on the mixer thread, which on this console would itself cause gaps: capture
   into memory (two minutes is 23 MB) and write the file once, from the heartbeat thread or at exit.
3. **M3, ring fill.** Log the fill after each submission every 10 s, plus the min and max in the window; the
   slope over a few minutes is the real clock offset in ppm (A2). Add the monotonic millisecond and the fill to
   the `[XA2] ran dry` line so it lines up with `[TIMER]` and `[PACING]`.
4. **M4, thread placement.** After 15 s, log `scePthreadGetaffinity` and `pthread_getschedparam` of the
   audio-out, mixer, tick, nv2a_ack, ohci and writer threads.
5. **M5, output thread period.** In `ps5_out_thread` time consecutive returns of `sceAudioOutOutput` and log
   any interval over 7 ms. This counts the device-level glitches that `ran dry` cannot see (A5).
6. **M6, quantum.** Two threads spinning on CPU 2 at the same priority, each logging the longest gap between
   its own iterations. Gives the round-robin quantum that B4 assumes is about 9 ms.
7. **M7, exit.** Leave the game through `HalReturnToFirmware` (if the title reaches it) and through a forced
   crash, and see where the console ends up (C2). Also `du -sh save/Partition*.img` against `ls -l` (C3).

## Checked, no change needed

- Sample format, channel order, rate, grain size: consistent between the mixer and `sceAudioOutOpen`.
- The ring lock is held for a 4 KB copy on the mixer side and a 1 KB copy on the output side; `sceAudioOutOutput`
  is called outside it. No inversion there.
- `xa2_submit_samples` returns a refusal the mixer ignores, which is right: there is nothing to retry.
- D120 item 3 (absolute slice deadlines) is in effect on the PS5; the per-slice `usleep` granularity of 99 us
  does not accumulate.
- The mixer and output thread are almost always asleep (96% and 100% of samples in a system call) and use
  almost no CPU.
- `early_fault` writes only with `write()`; `ps5_logf` writes directly to `ps5.log` from its own threads.
- `ps5_package.sh` and `ps5_libc_compat.c`: nothing found.
- QPC and GetTickCount at about 0.9 us each took 11% of "guest 0" in the log; test 19 reads the TSC instead.
