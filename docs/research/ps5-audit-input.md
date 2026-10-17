# PS5 audit: input path (parent tree at 2026-10-17)

Read-only review. Nothing was built, edited or run in the repo. Every claim is marked READ (I read the code
or disassembly and followed it) or INFERRED (follows from what I read, but not shown by it).

Result in one line: a resting DualSense gives exactly zero at every stage, up is up, every Xbox control has a
DualSense source, and rumble order and scale are right. The real problems are the reserved keyboard slot, no
cleanup when a pad read fails, and a stacked stick dead zone. Nothing here stops a fight from working.

The trigger bug (`trigger(v) = v*65535/255 - 32768`, `ps5/src/host_ps5.c:100-103`) is already fixed and is not
reported again. Its test-only twin is defect 5.

## The chain, stage by stage

| Stage | Where | What it does |
|---|---|---|
| 1. scePad | `host_ps5.c:105-137` | `scePadReadState`: buttons bitmask, sticks 0..255 (Y: 0 is up), L2/R2 0..255 |
| 2. SDL virtual gamepad | `host_ps5.c:117-136`, SDL `SDL_virtualjoystick.c` | buttons by `SDL_GAMEPAD_BUTTON_*`, axes by `SDL_GAMEPAD_AXIS_*`; a virtual trigger -32768..32767 reads as 0..32767 from `SDL_GetGamepadAxis` |
| 3. InputRaw | `input_host.c:151-184` (`read_sdl`) | buttons 0 or 32767; triggers clamped at 0; sticks `clamp_axis`; Y becomes `-1 - SDL_Y` (up positive) |
| 4. InputPad | `input_map.c:342-384` (`input_pad_map`) | button at or above 50% -> digital bit / analog 255; trigger `>>7` then 5% threshold; stick radial 15% dead zone with rescale (`input_map.c:303-321`) |
| 5. USB report | `usb_gamepad.c:795-861` (`sample_now`), `:731-790` | 20 bytes: byte 2 digital bits, byte 3 zero, bytes 4..11 A B X Y Black White LT RT, bytes 12..19 LX LY RX RY (int16 LE). A 4 ms sampler latch and a 120 ms minimum hold sit on top (buttons and triggers only, not sticks) |
| 6. Game | XAPI copies the report unchanged; PadMgr (service 0x19) | sticks `(raw+0.5)/32767.5`, zero below 0.25, then `(v-0.25)/0.75`; analog buttons `byte/255`; held above 0.01, pressed above 0.5 |

Stage 6 is READ from the xdis disassembly and `docs/research/ps5-popup-input.md` (PadMgr contract).

## Controls: rest and full press at each stage

### Buttons

Rest is 0 at every stage for every row. Full press:

| Xbox / DualSense | 1. scePad bit | 2. SDL button | 3. InputRaw | 4. InputPad | 5. Report | 6. Game (PadMgr bit, float, held / press) |
|---|---|---|---|---|---|---|
| A / Cross | 0x4000 | SOUTH | 32767 | analog A = 255 | byte 4 = 255 | 0x10, 1.0, held + pressed |
| B / Circle | 0x2000 | EAST | 32767 | analog B = 255 | byte 5 = 255 | 0x20, 1.0, held + pressed |
| X / Square | 0x8000 | WEST | 32767 | analog X = 255 | byte 6 = 255 | 0x40, 1.0, held + pressed |
| Y / Triangle | 0x1000 | NORTH | 32767 | analog Y = 255 | byte 7 = 255 | 0x80, 1.0, held + pressed |
| Black / L1 | 0x400 | LEFT_SHOULDER | 32767 | analog Black = 255 | byte 8 = 255 | 0x8, 1.0, held + pressed |
| White / R1 | 0x800 | RIGHT_SHOULDER | 32767 | analog White = 255 | byte 9 = 255 | 0x2, 1.0, held + pressed |
| START / Options | 0x8 | START | 32767 | bit 0x10 | byte 2 bit 0x10 | 0x100, held + pressed |
| BACK / Touchpad click | 0x100000 | BACK | 32767 | bit 0x20 | byte 2 bit 0x20 | 0x200, held + pressed |
| L3 | 0x2 | LEFT_STICK | 32767 | bit 0x40 | byte 2 bit 0x40 | 0x400, held + pressed |
| R3 | 0x4 | RIGHT_STICK | 32767 | bit 0x80 | byte 2 bit 0x80 | 0x800, held + pressed |
| D-pad up | 0x10 | DPAD_UP | 32767 | bit 0x01 | byte 2 bit 0x01 | 0x1000, held + pressed |
| D-pad down | 0x40 | DPAD_DOWN | 32767 | bit 0x02 | byte 2 bit 0x02 | 0x2000, held + pressed |
| D-pad left | 0x80 | DPAD_LEFT | 32767 | bit 0x04 | byte 2 bit 0x04 | 0x4000, held + pressed |
| D-pad right | 0x20 | DPAD_RIGHT | 32767 | bit 0x08 | byte 2 bit 0x08 | 0x8000, held + pressed |

READ: `host_ps5.c:65-71, 117-130`, `input_map.c:283-296` (identity map: Black = left shoulder, White = right
shoulder), `input_map.h:60-67`. The face buttons and shoulders are analog on the console but binary here, so
the game's 0.01 and 0.5 thresholds are both crossed at 255 and the game cannot tell the difference.

### Triggers

| Xbox / DualSense | State | 1. scePad | 2. SDL virtual -> gamepad | 3. InputRaw | 4. InputPad | 5. Report | 6. Game |
|---|---|---|---|---|---|---|---|
| LT / L2 | rest | 0 | -32768 -> 0 | 0 | 0 | byte 10 = 0 | 0.0000, not held |
| LT / L2 | full | 255 | 32767 -> 32767 | 32767 | 255 | byte 10 = 255 | 1.0000, held + pressed (bit 0x1) |
| RT / R2 | rest | 0 | -32768 -> 0 | 0 | 0 | byte 11 = 0 | 0.0000, not held |
| RT / R2 | full | 255 | 32767 -> 32767 | 32767 | 255 | byte 11 = 255 | 1.0000, held + pressed (bit 0x4) |

Travel (READ, `input_map.c:323-333`, numeric model): 5% threshold is 12, so DualSense values 0..12 give byte 0;
13 gives 1; 15 gives byte 3 = 0.0118, the first value above the game's 0.01 held threshold; 134 gives byte 128 =
0.502, the first pressed. A resting DualSense trigger is never held.

### Sticks

X and Y of the left and right stick run through the same code; only the source stick differs.

| Control | State | 1. scePad | 2. SDL (`axis()`) | 3. InputRaw | 4. After 15% dead zone | 5. Report int16 | 6. Game float |
|---|---|---|---|---|---|---|---|
| Left/right X | rest (120..136) | 120..136 | -2048..+2048 | same | 0 | 0 | 0.0000 |
| Left/right X | full left | 0 | -32768 | -32768 | -32767 | -32767 | -1.0000 |
| Left/right X | full right | 255 | +32512 | +32512 | +32467 | +32467 | +0.9878 |
| Left/right Y | rest (120..136) | 120..136 | -2048..+2048 | `-1 - v`: -2049..+2047 (rest centre -1) | 0 | 0 | 0.0000 |
| Left/right Y | full up | 0 (0 is up) | -32768 | +32767 | +32767 | +32767 | +1.0000 |
| Left/right Y | full down | 255 | +32512 | -32513 | -32468 | -32468 | -0.9878 |

Sign (READ): scePad up is 0, so SDL gets negative for up (SDL Y is positive down), `read_sdl` flips it
(`input_host.c:180, 182`), so the report is positive for up, which is the Xbox convention. The fighter reader
then negates Y for its own screen-space atan2, so up is up in game. Left/right is not flipped anywhere and
agrees.

Samples through the whole chain (numeric model of the code above, X axis; Y is the mirror):

| DualSense X | Report | Game | Note |
|---|---|---|---|
| 120..136 | 0 | 0.0000 | rest range, exactly zero |
| 150 | 843 | 0.0000 | below the game's 0.25 |
| 176 | 8674 | 0.0196 | first movement |
| 207 | 18011 | 0.3996 | just under the fighter's 0.4 direction threshold |
| 255 | 32467 | 0.9878 | full right |

## Specific checks

- **Resting stick is exactly zero (READ).** Worst rest corner (120 or 136 on both axes) is 2896 in magnitude;
  the 15% dead zone is 4915 (`input_map.c:303-311`, `mag <= dz` returns 0,0). The margin is 19.2 counts of
  radial travel. The `-1 - v` flip leaves a 1-count offset on Y at rest, which is inside the dead zone. The
  game's own 0.25 hard zero would absorb it anyway.
- **Dead zone math (READ).** Radial, rescaled `(mag - dz) / (32767 - dz)`, clamped at 1, no divide by zero
  (`mag == 0` returns first, `dz` capped at 95%). The direction is preserved. It is correct as code.
- **Black / White (READ for the mapping, INFERRED for the taunt use).** Default map sends L1 to Black and R1 to
  White (`input_map.c:289`); both reach the game as 255 on bits 0x8 and 0x2. L2/R2 are separate (LT/RT), so
  the four shoulder inputs do not collide. I could not confirm from the game which fight action Black/White
  start; the web search found nothing, and the fight pad-word remap at `0xDC4F0` keeps the 0x2 and 0x8 bits.
  The taunt use is your premise. Remap in `[gamepad]` (`black`, `white`) if you want them elsewhere.
- **The hub's second "keyboard" controller (READ).** It is all zero at rest (nothing writes it on PS5: no
  keyboard events, `host_posix_*` stubs at `host_ps5.c:336-339`), so it never presses anything. It does matter:
  see defect 1.
- **Rumble (READ).** `usb_gamepad.c:226-257` takes left (large) motor from bytes 2-3 and right (small) from
  bytes 4-5; the toolkit passes them to `SDL_RumbleGamepad(gp, low, high, ms)` (`input_host.c:444`) as large, small; `pad_rumble`
  (`host_ps5.c:139-148`) hands `large = low >> 8`, `small = high >> 8` to `scePadSetVibration(large, small)`.
  The order is right and 16 bit to 8 bit is a straight scale (0xFFFF -> 255, 0x0100 -> 1). The toolkit's
  `rumble_floor` 60 and `rumble_min_ms` 200 raise weak pulses before this.
- **Controls a DualSense cannot produce (READ).** None. All 16 Xbox controls above have a source. The PS button
  and Create button are not readable through scePad and the game does not use them (the overlay's `guide`
  alternative is unreachable on PS5, harmless). Face buttons are binary here by design.

## Defects, most severe first

1. **A permanent, always-zero second controller; a later second pad lands on player 3.** (medium)
   `tools/xboxrecomp/src/input/input_host.c:299` (`else H.kb_slot = pads;`), `:303` (`n = pads + 1`), `:251`
   (`assign_device` skips the keyboard slot). Default mode is `auto` (`src/hooks/pc_input.c:81, 151`, setting
   default 0), read at `src/hooks/pc_settings.c:542`.
   Wrong values: with one DualSense at launch, `kb_slot = 1`, `H.players = 2`, `RECOMP_USB_PADS = 2`, so
   the game sees controller 2 connected and every report byte 2..19 of it is 0 (READ). A DualSense that appears
   after launch (second user signs in; `pad_scan_thread`, `host_ps5.c:202-209`, rescans each second) is given
   slot 2, which is player 3, not player 2 (READ). What the title does with a connected idle port 2, such as a
   versus screen treating it as a human, is INFERRED. `docs/10-input.md:36-37` already calls the keyboard
   player "an extra controller" and says to turn it off.
   Smallest fix: in `host_posix_run`, next to `setenv("RECOMP_PRESENT_PACING", "1", 0)` (`host_ps5.c:824`),
   add `setenv("RECOMP_KEYBOARD_PLAYER", "off", 0);`. It is set before `pc_settings_init`, and an env value wins
   over the ini (`recomp_settings.c` `apply_env`). With `off` the hub starts with one controller per DualSense
   (at least 1) and a later pad takes the lowest free player through the hot-plug path (`xbox_UsbSetPadCount`).

2. **A failed pad read leaves the last state in place, and nothing ever detaches a pad.** (low to medium,
   INFERRED effect) `ps5/src/host_ps5.c:114` (`if (scePadReadState(...) != 0) return;`); `scePadClose` is declared
   at `:51` and never called; no `SDL_DetachVirtualJoystick` anywhere; the `connected` flag of `ScePadData`
   sits in the unread `rest[]` (`:62`).
   Wrong value: if the read starts failing while a stick is at 255 or a button is down (user signs out, handle
   goes invalid), the virtual joystick keeps that value, so the game sees `+32467` or a held button until
   restart (READ that nothing clears it; INFERRED that the SDK returns an error instead of a zeroed state).
   Smallest fix: on a nonzero return, write once: all buttons 0, all four sticks 0, both triggers -32768
   (a `dead` flag in `Ps5Pad` avoids rewriting every update).

3. **Player 1 is the first logged-in user, not the user holding the pad.** (low, INFERRED)
   `host_ps5.c:158-166`: attaches every user in `sceUserServiceGetLoginUserIdList` order and keeps going. A user
   who is still signed in but has no pad on gets a virtual pad that never moves (if `scePadOpen` succeeds without a
   connected pad, which I could not check), so the real pad becomes player 2. Smallest fix: put
   `sceUserServiceGetForegroundUser` first in the list, and skip a user whose `connected` flag is false once
   the offset of that field is verified on hardware.

4. **The toolkit's 15% stick dead zone stacks on the game's own 25%.** (low, READ numbers, INFERRED feel)
   `tools/xboxrecomp/src/input/input_map.c:295-296` (defaults 15, 15), set from `src/hooks/pc_input.c:171-172`.
   Wrong value: the fighter needs a PadMgr value above 0.4 for a direction, which is 55% of travel on a pad
   that sends raw values and 61.5% through the 15% dead zone. A stick at 60% travel gives 0.373
   and no direction; 62% gives 0.404. A DualSense's rest noise (8.8% at worst) is already absorbed by the
   game's 25%, so the toolkit's dead zone adds nothing on PS5.
   Smallest fix: `left_deadzone = 0` and `right_deadzone = 0` in the PS5 settings (or default them to 0 under
   `__PROSPERO__` in `input_padmap_defaults`). Zero is safe: worst rest is 0.088 against the game's 0.25.

5. **Test harness trigger copy writes 0 for released.** (low, not on PS5, READ)
   `tools/xboxrecomp/src/input/input_host.c:895-896`:
   `SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, held[...] ? 32767 : 0);` and the same for
   right. Wrong value: a virtual axis of 0 is half way, so SDL reports 16383, the toolkit gets byte 120, and
   both triggers read 0.4706 (held, not pressed) at rest in every scripted virtual-pad run
   (`RECOMP_PAD_SCRIPT_VIRTUAL`). This is the same bug as the one fixed in `host_ps5.c`.
   Smallest fix: `? 32767 : -32768` on both lines.

## Checked and fine, or not worth a fix

- **Stick range is asymmetric by 1.2% (READ).** `axis()` (`host_ps5.c:87-91`) maps 0 to -32768 but 255 to
  +32512, so full right/down reads 0.9878 and full left/up 1.0000. Harmless for the game; a fix would be
  `(v - 128) * 258` with clamping at both ends.
- **Minimum hold of 120 ms merges fast taps (READ, INFERRED effect).** `usb_gamepad.c:742-780`: a press is
  reported for at least 120 ms from its first sample and the hold is not extended, so two taps closer than about
  125 ms run together and a gap shorter than the game's poll may never be seen. Roughly 8 taps per second is the
  ceiling. `RECOMP_PAD_MIN_HOLD_MS=50` lowers it. Not PS5-specific.
- **Overlay chord (READ, INFERRED effect).** L3 + R3 held 500 ms opens the overlay and zeroes game input
  (`src/hooks/pc_ui_settings.c:30-33`, `pc_ui.cpp:925-935`, `:952`). The PS5 build compiles it
  (`ps5/cmake/ps5.cmake:26`). The fight pad-word remap at `0xDC4F0` has no L3/R3 bit, so it should not collide
  with fight inputs; I did not check menus.
- **Focus and background events (READ).** No SDL windows exist on PS5 and the toolkit sets the allow-background
  hint, so `SDL_PrivateJoystickShouldIgnoreEvent` (`SDL_joystick.c:2307-2318`) never drops a pad event.
- **Poll rates (READ).** Reader thread 2 ms (`input_host.c:31`), sampler 4 ms (`usb_gamepad.c:712`), so a
  short press is not lost between polls.
- **Where `scePadReadState` runs (READ).** `pad_update` runs inside `SDL_UpdateGamepads` (`input_host.c:574, 591,
  685`) on the toolkit's reader thread, not the game thread.

## Method

Read: `ps5/src/host_ps5.c`, `tools/xboxrecomp/src/input/{input_map.c,input_map.h,input_host.c,xinput_device.c}`,
`tools/xboxrecomp/src/usb/{usb_gamepad.c,usb_hub.c,ohci.c}`, `src/hooks/{pc_input.c,pc_settings.c,pc_ui.cpp,
pc_ui_settings.c}`, SDL3 `SDL_virtualjoystick.c`, `SDL_gamepad.c`, `SDL_joystick.c` at `/home/claude/ps5/sdl3`,
and the PadMgr and fighter functions at `0x27E075, 0x1EBBE0, 0xD7BF0, 0xDCE00, 0xDD3C0, 0xDC4F0, 0x2D9E0,
0x1BAA00, 0x1BADC0`. The numbers in the tables come from a numeric model that repeats the arithmetic of the
cited lines; it is scratch work and not in the repo. I did not run the game or a pad.
