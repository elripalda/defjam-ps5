# PS5: what the first-time-help popup and the in-fight pause menu read, and what else gates them

Read-only investigation. Nothing was built, run or edited. I disassembled the original `default.xbe` (x86,
capstone) rather than reading the lifted C, because the lifted functions are named by guest address
(`sub_XXXXXXXX` = guest `0xXXXXXXXX`), so every address below is a guest address and maps one-to-one to a
`sub_` in `src/recomp/gen/recomp_00NN.c`. Tags used throughout: **READ** (I read the instructions and they
say it), **INFERRED** (implied by the code or by host-side code but not proven), **NOT FOUND** (searched,
no such thing, or purpose not determined).

Symptom being explained: on PS5, during a fight the first-time-help popup (BLAZIN', WEAPON, footer
"(A) CONTINUE") never closes on A; START opens the in-fight pause menu but its d-pad and A do nothing.
Gameplay and the Flash front-end menus work. The same lifted code works on Windows and Linux. The pad poll,
snapshot ring and input thread are already known and are not repeated here.

---

## Answer to question 3 first: is there any extra condition?

**No time, animation, sound, voice, fade or port-ownership wait gates either screen. The only waits are two
game-tick counters inside the popup, and both are already finished by the time the footer is on screen.**

- **FTH popup (READ).** Two counters live in the popup object, and both count calls of its Update (one call
  per game tick, so they are not wall-clock):
  - `+0x250` is the build delay. `D5560` (show request) sets it to 4. Update counts it down; on the tick it
    reaches 0 the popup is built (`D6180`) and the audio pause message is sent.
  - `+0x24c` is the input lockout. `D6180` sets it to 30 (`0x1e`). Update counts it down and returns early
    until it reaches 0; only then does it run the input check. The draw routine (`D57B0`, reads
    `+0x24c` at `0xD5A19`) is what shows the footer, so seeing "(A) CONTINUE" means the lockout is over
    (INFERRED from the draw reading the same counter; I did not trace which branch draws the footer).
  - No `QueryPerformanceCounter`, `rdtsc`, `KeTickCount`, the 120 Hz "now" clock or `dt` is read anywhere in
    `D6450`/`D5460`/`D5560`/`D6180`/`D57B0`. No sound-finished, voice-finished, animation-finished or fade
    flag is read. The sound cue `sub_334A0(1)` is fire-and-forget on the A path (it is called, not waited on).
  - What the check really requires is purely pad state (see Q1): **nothing held on any pad at all**, plus
    a release edge of **exactly the A bit and no other bit** on that same tick.
- **Pause menu (READ).** No timer gates a press edge. The conditions are: state is "open", the pad index
  that pressed START is the one being read, and the press value is **exactly one** of UP/DOWN/LEFT/RIGHT/A/
  B/START (the dispatcher compares for equality, not for a bit test). The 120 Hz "now" clock is used only for
  the left-stick auto-repeat acceleration, not for d-pad press edges.
- **Outer gates that can silently disable both (READ, purpose of the globals NOT FOUND).** The module
  manager must be idle, the generic popup (`D4ED0`) inactive, `[0x39C0C0] & 0x40` clear, and the pause
  menu's own enabled byte (`+0x23`) set. The popup also aborts itself and never shows if any of
  `[0x3BE13E]`, `[0x3BE14E]`, `[0x3BE15E]` has bit 0 set or the profile service byte `+0x338` is set; in that
  case it clears itself rather than hanging, so it does not explain a popup that is visible but stuck.
- **Port ownership (READ).** The popup has none: it reads the combined pad. The pause menu is owned by the
  pad index that pressed START and ignores the others (Q4).

Practical consequence: if the popup is visible and A does nothing, one of three things is true, all on the
pad-state side: some bit is held on any pad (including a trigger or stick-derived bit), the A release edge
arrives together with another bit, or the A release edge never arrives (A press and release collapse inside
one snapshot so the prev/current held words never differ). See "Ranked candidates" below.

---

## Per-tick call chain (READ)

main loop `11CD0` -> frame pop `D75E0` (up to 4 updates per frame) -> `2C530` (dt) -> PadMgr update `DD930`
-> RunModuleMan update `77040` -> module vtable slot 3 = `79B60` (RunModuleGame::Update; vtable `0x29D5E4`,
written by ctor `76DC0`). From `79B60` the three screens are called directly (none of them has a vtable;
no data pointer to a vtable exists in the image):

| Screen | Entry | Notes |
|---|---|---|
| First-time-help popup (FTH) | `D6450` | class ctor `D51C0`, init `D66E0`, message handler `D6670`, singleton getter `79D60` |
| In-fight pause menu | `CF6B0` | ctor `CCE60`, navigation `CD0D0`, rebuild `CD020`/`CCF20` |
| Generic popup | `D4ED0` (update), `D4E40` (toggle) | not the FTH; only gates it |

The service registry is a pointer array at `0x2FB148`, accessor `sub_12CF0(id)`: 3 audio, `0x17`
RunModuleMan, `0x18` profile/options, `0x19` PadMgr, `0x1D` message bus, `0x27` input thread object,
`0x28` popup. Message `0xF0000000` = pause, `0xF0000001` = unpause (audio pause only, handled in `3A340`);
`0xF0000033` = show FTH; `0xF1000001` (handler `35AE0`) triggers it.

## What both screens read: the PadMgr contract (READ)

PadMgr is service `0x19`, size `0xAC4`. Per-port pad records are `0xD4` bytes at `mgr + 0x84 + idx*0xD4`.
A slot table at `mgr + 0x64 + idx*4` says whether index `idx` has a device; a combined pad is at
`mgr + 0x724` and a "null pad" at `mgr + 0x7F8`.

| Record field | Meaning |
|---|---|
| `+0x84` | held bits (current) |
| `+0x88` | held bits at previous update |
| `+0x8C` | auto-repeat bits |
| `+0xA8..` | left stick x,y floats (stick 0), then stick 1 |
| `+0xB8` / `+0xBC` | repeat delays, 30 and 20 |
| `+0xC0` / `+0xC4` | analog-to-held threshold 0.01 / analog press threshold 0.5 |
| `+0xCC` | stick dead zone 0.25 (what `DCD50` + 0x14 hands to the pause menu) |

Bit order: LT 0x1, White 0x2, RT 0x4, Black 0x8, **A 0x10**, B 0x20, X 0x40, Y 0x80, **START 0x100**, BACK 0x200,
L3 0x400, R3 0x800, **UP 0x1000, DOWN 0x2000, LEFT 0x4000, RIGHT 0x8000**.

State getter `DD2B0(rec, mode)`:

| mode | returns |
|---|---|
| 0 | held (`+0x84`) |
| 1 | pressed edge: `(held ^ prev) & held` |
| 2 | repeat bits (`+0x8C`, built by `DD770` from the 120 Hz clock) |
| 3 | released edge: `(held ^ prev) & prev` |

`DD390(n)` is the single-bit form of mode 3 ("button n was released this update"). Mode 3 and `DD390` are
used **only** by the FTH popup and the pause menu; everything the working paths use is held or pressed.
Pad lookup: `DCBA0(idx)` returns the record only if the slot at `mgr+0x64+idx*4` is non-null, otherwise the
null pad; `DCC50(pad)` is simply `pad == null pad`. `DCBE0(id)`: `-1` returns the combined pad, otherwise it
searches the slots for device id `id` (`[slot+8]`), falling back to the null pad.

---

## Q1. First-time-help popup: what closes it on A (READ)

`D6450` (FTH::Update), in order:

1. Return unless `+0x184` (alive) and `+0x185` (showing) are both non-zero.
2. Build delay `+0x250` (4 ticks): count down; when it reaches 0, check the abort flags above, send message
   `0xF0000000`, mark this help type as seen in the profile (bytes at profile+`0x50`+`0x10..0x12`, picked
   by `+0x254`), copy the page config from table `0x2B2E38 + type*0xC4`, call `D6180`.
3. Lockout `+0x24C` (30 ticks): count down and return until 0.
4. Input check, the part that matters:
   - `pad = DCBE0(PadMgr, FTH[+0x188])`. `+0x188` is `-1` from the ctor (`D51C0`, `0xD5338`) and is reset to
     `-1` by dismiss (`D5460`, `0xD54ED`). Nothing else writes it in this code, so this is always the
     **combined pad** (all ports merged by `DCFB0`).
   - `held = DD2B0(pad, 0)`, `rel = DD2B0(pad, 3)`.
   - Return if `rel == 0` or `held != 0`.
   - `mask = FTH[+0x18C]`. It is 0 from the ctor and reset to 0 by dismiss, so the no-mask branch is the live
     one: return unless `rel == 0x10`. If `mask` were non-zero it would require `rel & mask` instead.
   - If the audio service (reg 3) exists, play cue `sub_334A0(1)` (not on the mask branch). Then `D5460`.
5. `D5460` (dismiss): sends `0xF0000001`, frees the page objects, sets `+0x185 = 0`, restores port `-1` and
   mask 0. The `0xF0000000`/`0xF0000001` pair is only an audio pause on the bus side (`3A340`). How the fight
   simulation itself is held while the popup shows was not traced (NOT FOUND).

So the contract is: the **combined** held word must be exactly 0, the **combined** released-edge word must be
exactly `0x10`, evaluated once per game tick, no threshold beyond the 0.01 analog-to-held rule already baked
into `held`. Edge, not level; release, not press.

## Q2. In-fight pause menu: d-pad up/down and A (READ)

`CF6B0` (PauseMenu::Update; `this + 0x23` must be non-zero), per tick:

1. Reads the 120 Hz clock (`1EDB70`), services 3 and `0x1D`.
2. Loops pad index 0..3. `pad = DCBA0(idx)`; skip when `DCC50(pad)` (no device in that slot).
3. Reads `pressed = DD2B0(pad,1)`, `repeat = DD2B0(pad,2)`, and the left stick via `DD3C0(0)`.
4. Stick path: if the left stick changed by more than the dead zone (0.25, `DCD50`+0x14) between two
   updates, it derives direction bits from the stick (x > 0.25 = RIGHT, x < -0.25 = LEFT, y > 0.25 = UP,
   y < -0.25 = DOWN) and, when the elapsed 120 Hz ticks exceed an accelerating timer (global float
   `0x2E3250`, starts 20.0, steps down by 1.0 per accepted step, floored at 3.0), **replaces** `pressed`
   with the newly set stick bits. A steady stick resets the timer to 20.0. The d-pad is not affected unless
   the stick is moving by more than 0.25 per tick.
5. Released edges: `DD390(5)` (B), `DD390(4)` (A), `DD390(9)` (BACK). In the open state, B or BACK release
   restores the saved settings (`+0x14..` copied back to `+8..`) and closes; **A release on row 5 closes**.
6. START press (`pressed & 0x100`, a bit test) with state 0 opens it: state = 1, row (`+0x28`) = 0,
   `[0x39F650] = 0`, owner (`+0x2C`) = this pad index, sends `0xF0000000`, builds via `CCF20`, and returns.
7. For the owner pad only (`+0x2C == idx`): call `CD0D0(pressed, audio, bus, repeat)`, then, if `repeat`
   is non-zero, `CD0D0(repeat, audio, bus, repeat)`.

`CD0D0` (navigation) dispatches on its first argument by **equality**, so a press word with an extra bit set
matches nothing. State is `+0x24` (0 closed, 1 list, 4 confirm); row is `+0x28` (0..5):

| Input (exact) | State 1 (list) | State 4 (confirm) |
|---|---|---|
| UP `0x1000` | row-1, wraps 0 -> 5; cue 3 | ignored |
| DOWN `0x2000` | row+1, wraps 5 -> 0; cue 3 | ignored |
| LEFT/RIGHT | rows 0-2 step a slider float (`+8+row*4`) by 2.0 within 0..100 (cue 2 at the limit); row 3 toggles `+0x21`; rows 4-5 do nothing | toggles `+0x20` (yes/no) |
| A `0x10` | first press plays cue 1 and sets latch `[0x39F650]`; row 4 additionally enters state 4. Rows 0-3 and 5 do nothing else | `+0x20 == 0`: back to state 1. Otherwise close + unpause + a quit-style action through the profile service (INFERRED purpose) |
| B `0x20` | cue 2 only | ignored |
| START `0x100` | closes (state 0, unpause) but only if `repeat` is 0 | closes |

Row 5's close is on **A release** (step 5), not on A press. A on rows 0-3 doing nothing is by design.

## Q4. Who owns the popup / menu in single-player Story (READ for the mechanism, INFERRED for Story specifics)

- **FTH**: no owner. Port `-1` = combined pad, so a button on any connected pad counts, and a stuck bit on
  any connected pad blocks it. This is identical in Story and VS.
- **Pause menu**: owner = the PadMgr index (0..3, the slot index, not the device port id) of the pad whose
  START press opened it; that index is stored at `+0x2C` at the instant of opening. Other indices are
  read but dispatched to nothing.
- Which index the single pad gets is decided below the game, by XAPI/USB enumeration. `m4b-controller-ports.md`
  records that with the current topology the pad is slot 0 and P1 (`Key.getController` subtracts 2 from the
  event's `slot + 2`); that is the front-end Key path (`5F9B0`), which is separate from PadMgr, but both are
  fed from the same device. I did not find a Story-specific owner decision in these classes (NOT FOUND).

---

## Ranked candidates for the PS5-only failure (INFERRED, host side)

Everything above is identical on Windows/Linux, so the difference is in what the host feeds PadMgr.

1. **A trigger bit is permanently held, so `held != 0` forever (explains the popup only).**
   - `ps5/src/host_ps5.c` `pad_update` writes the triggers as `l2*32767/255` into an SDL virtual
     joystick declared as a gamepad.
   - The SDL virtual-joystick source documents that trigger axes are at their *minimum* value at rest, i.e.
     -32768. A resting 0 therefore reads as about 50% to anything that maps the full axis range.
   - `tools/xboxrecomp/src/input/input_host.c` / `input_map.c` clamp negatives and use a 5% trigger
     threshold; the Xbox report then carries an analog byte near 127-128. The held conversion (`DCE00`)
     uses 0.01, i.e. a byte of 3 or more, so that sets the LT/RT held bits all the time.
   - Gameplay could still work if its trigger actions use the 0.5 analog threshold (`+0xC4`) rather than the
     held bit, since 127/255 is just under 0.5. I did not verify which one gameplay reads (NOT FOUND).
   - The FTH needs `held == 0` and so would never fire; its press/held-based neighbours would be unaffected.
   - Does not explain the pause menu d-pad (edges ignore steady held bits).
2. **A or d-pad press arrives with extra bits, so exact-match dispatch fails.** Pause nav (`CD0D0`) and the
   FTH (`rel == 0x10`) both need a single bit. Any second bit flipping on the same tick (a noisy trigger
   crossing byte 3, a stick-derived hat bit) defeats it.
3. **A release edge is never delivered.** FTH needs a release; if a quick A tap is latched as press and
   release inside one 120 ms minimum-hold window (`usb_gamepad.c` latch sampler) the PadMgr may see no
   prev/current difference. This would hit release-based actions (FTH close, pause A-release close, B/BACK
   close) but not press-based gameplay or Flash menus.
4. **Stick noise exceeding 0.25 per tick** replaces `pressed` with stick bits in the pause menu (step 4 of
   Q2), which would eat d-pad presses. Low likelihood with a normal pad.
5. **Cue path blocking** (`sub_334A0` takes a mutex and waits on a named parameter). Called before the action
   on both screens. Low confidence; the cue is `PauseMenu_SFX` and runs on the game thread.

Not explained: why the pause d-pad fails while the front-end d-pad works. Candidates 2, 3 and 4 are the only
ones that fit; none is proven.

## Runtime probes that would settle it (no code change implied here)

Hook points, all guest addresses, to log from a debug build:

| Address | Log |
|---|---|
| `DD2B0` entry, for `ecx == mgr+0x724` | modes 0, 1 and 3 values every tick while the FTH is up |
| `D6450` at `0xD65DE` | `eax` (released) and `edi` (held) |
| `CF6B0` at `0xCF7C7` | the pressed word, and `mgr+0x64+idx*4` per index |
| `CD0D0` entry | `[esp+4]` (press word), `[esi+0x24]`, `[esi+0x28]`, `[esi+0x2C]` |
| `DCE00` output | held bits for idle, A tap, d-pad tap |

What to look for: idle held at `0x1`/`0x4`/`0x5` (trigger candidate); the released word on an A tap being
`0x10` plus extra bits, or never non-zero; the pause press word on a d-pad tap not equalling `0x1000`/`0x2000`.

## Address index

| Address | Role | Tag |
|---|---|---|
| `D6450` | FTH::Update, the dismiss check | READ |
| `D5460` | FTH dismiss | READ |
| `D5560` | FTH show request (sets build delay 4) | READ |
| `D6180` / `D5B30` | FTH build (sets lockout 30) | READ |
| `D57B0` | FTH draw (reads both counters) | READ |
| `D51C0` / `D66E0` / `D6670` | FTH ctor / init / message handler | READ |
| `79D60` | FTH singleton getter | READ |
| `79B60` / `793A0` / `76DC0` | RunModuleGame Update / init / ctor | READ |
| `CF6B0` | PauseMenu::Update | READ |
| `CD0D0` | PauseMenu navigation | READ |
| `CCE60` / `CD020` / `CCF20` | PauseMenu ctor / rebuild / show | READ |
| `D4ED0` / `D4E40` | generic popup update / toggle | READ |
| `DD930` | PadMgr update | READ |
| `DCE00` / `DD770` / `DCFB0` | snapshot to held / repeat bits / merge to combined pad | READ |
| `DCBA0` / `DCBE0` / `DCC50` / `DCD50` / `DD3C0` | pad by index / by id / is-null / config ptr / stick read | READ |
| `DD2B0` / `DD390` / `DD360` | state getter / single-bit release / single-bit press | READ |
| `DC970` / `D7BF0` | device ReadSnapshot / snapshot port record | READ |
| `334A0` / `41C50` | UI cue ("PauseMenu_SFX", mutex wait) | READ |
| `5F9B0` | front-end Key event generator | READ |
| `0x2FB148` | service registry array | READ |
| `[0x3C9684]` via `1EDB70` | 120 Hz "now" tick counter (timer thread) | READ |
| `[0x39F650]`, `[0x2E3250]` | pause A latch, stick-repeat timer | READ |
| `[0x3BE13E]`, `[0x3BE14E]`, `[0x3BE15E]`, `[0x39C0C0]` | gate flags, meaning | NOT FOUND |
