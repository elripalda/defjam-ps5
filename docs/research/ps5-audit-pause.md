# PS5 audit: why the in-fight pause menu ignores the d-pad

Read-only. Nothing was built, edited or run. I disassembled the original `default.xbe` (capstone) and followed
the code by hand. Addresses are guest addresses; `0xXXXXXXXX` is `sub_XXXXXXXX` in `src/recomp/gen/recomp_00NN.c`.
Tags: **READ** (the instructions say it), **INFERRED** (follows from what I read, not shown by it),
**NOT FOUND** (I looked and it is not there). Builds on `ps5-popup-input.md` and `ps5-audit-input.md`.

## 1. Pointer chain to the pause menu (the `this` of `0xCF6B0`)

```
RM   = MEM32(0x2FB1A4)          RunModuleMan, registry id 0x17          READ
CUR  = MEM32(RM  + 0x18)        current module                          READ
VT   = MEM32(CUR + 0x00)        0x29D5E4 = type 2, 0x29D604 = type 3    READ
MENU = MEM32(CUR + 0x2C)        if VT == 0x29D5E4                       READ
MENU = MEM32(CUR + 0x34)        if VT == 0x29D604                       READ
```

One expression: `[[[0x2FB1A4]+0x18]+0x2C]` for a type-2 module, `+0x34` for type 3. Read the vtable first and pick.

- The registry is the array at 0x2FB148, slot = `0x2FB148 + 4*id` (`0x12CF0` is `[ecx+id*4]`), so id 0x17 is 0x2FB1A4
  (READ). `0x77040`/`0x773A0` use `[RM+0x18]` as the module they call (READ).
- Type 2 builds the menu in its init `0x793A0`: `new(0x104)`, `0xCCE60`, stored at `[this+0x2C]` (READ,
  `0x794E3..0x79501`). Type 3 does the same in `0x7A170..0x7A19C` and stores at `[this+0x34]` (READ). Both
  `0x79B60` (type 2, calls `0xCF6B0` at `0x79C1D`) and `0x7A8F0` (type 3, `0x7A9C8`) call `0xCF6B0` with it (READ).
- Which type the fight uses: type 2 (INFERRED). The first-time-help popup `0xD6450` is called only from `0x79B60`, and
  that popup shows in fights. Check `VT` instead of trusting this.
- The menu pointer is null before init and after teardown (READ, `0x795E0..0x795FA` frees it). Sanity: a live one is
  in the guest heap and `MEM32(MENU+0x24)` is 0, 1 or 4.
- The menu has no vtable; `[MENU+0]` is a plain int (READ).

### Fields

| Offset | Meaning | Tag |
|---|---|---|
| `+0x23` byte | enabled. `0xCF6B0` returns at once when 0 (`0xCF6B6`). Set to 1 in the module's enter `0x798A0` (`0x799B2`); cleared by `0x79B60` at `0x79CAE` | READ |
| `+0x24` int | state: 0 closed, 1 open (main list), 4 quit-confirm sub-state. UP and DOWN need 1; LEFT, RIGHT and A also act in 4; START-open needs 0 | READ (names INFERRED) |
| `+0x28` int | row 0..5. UP: `row>0 ? row-1 : 5`. DOWN: `row<5 ? row+1 : 0`. START-open resets it to 0 | READ |
| `+0x2C` int | owner pad slot. Written once, at START-open, from the loop index of the pad that pressed START (`0xCFA6C`) | READ |
| `+0x08/+0x0C/+0x10` float | rows 0..2 values, 0..100, step 2.0 (LEFT/RIGHT). `+0x14/+0x18/+0x1C` are the copy B/BACK restores | READ (they are volumes: INFERRED, `0xCD020` feeds them to the audio service) |
| `+0x20`, `+0x21` byte | row 4's confirm flag and row 3's toggle; `+0x22` is the restore copy of `+0x21` | READ |
| `+0x00` int, `+0x04` float | highlight flash: `2*row(+1)` and a 15.0 countdown | INFERRED |
| `+0x30/+0x34` float, `+0x38` int, `+0x3C` int | left-stick path: previous x, previous y, previous 120 Hz tick, previous stick bits. One set per menu, not per pad | READ |

### Pad side, same style

```
PM   = MEM32(0x2FB1AC)                      PadMgr, id 0x19                  READ
slot present      = MEM32(PM + 0x64 + 4*i) != 0                             READ (0xDCBA0)
held(i)   = MEM32(PM + 0x108 + 0xD4*i)      pad i = PM+0x84+0xD4*i, +0x84    READ (0xDD2B0)
prev(i)   = MEM32(PM + 0x10C + 0xD4*i)                                       READ
repeat(i) = MEM32(PM + 0x110 + 0xD4*i)                                       READ
```

`0xCF6B0` visits `i = 0..3` and skips a slot whose pad is the null pad (`0xDCC50`, READ). `ps5_gamewatch.c` already
has `PADMGR_P0_HELD 0x108`.

### Static bytes that need no chain

- `0x39F650` (byte): zeroed at START-open (`0xCFA65`), set to 1 by `0xCD0D0` on the first A press while `state == 1`,
  after the cue returned (`0xCD262`). Only those two sites write it (no other immediate reference in the image).
  READ. So `0x39F650 == 1` after one A tap proves: `0xCF6B0` ran, the owner matched, the press was exactly 0x10,
  state was 1 and the cue `0x334A0` returned.
- `0x39C0C0` bit 0x40: `pause_use_aki`, loaded by `0xA41B0` from `aki_cfg.xml` (string at 0x2A6D94, READ). When set,
  `0x79B60` and `0x7A8F0` skip `0xCF6B0` (READ, `0x79BF6..0x79C01`). The file is not in `/home/claude/djdata`, so its
  value is NOT FOUND.
- `0x2E3250` (float, stick repeat timer, image value 20.0) and `0x3C9684` (120 Hz tick) matter only for section 3b.

## 2. What a d-pad press has to survive

For UP to change the row, all of these hold at once (READ unless noted):

1. `0x77040` calls the module's slot 3 on every step, in every RunModuleMan state (all paths reach `0x77121`).
2. `0x79B60` calls `0xCF6B0` only if `[RM+0x24] == 0` (`active`, `0x79B6B..0x79B70`), service 0x28's byte `+0x206`
   is 0 (`popupBusy`, from `0x79BC8..0x79BEC`), and `[0x39C0C0] & 0x40` is clear. Type 3 has the same `[RM+0x24]` and
   aki gates (`0x7A8F8`, `0x7A999`); I did not check its popup gate.
3. `[MENU+0x23] != 0`.
4. The pad's slot is present, and `pressed = (held ^ prev) & held` (`0xDD2B0` mode 1) is **exactly** 0x1000. `0xCD0D0` does
   `cmp eax, 0x1000` / `0x2000` and nothing else; a second bit in the same step drops it.
5. `[MENU+0x2C] == i` (`0xCFA07..0xCFA0E`).
6. `[MENU+0x24] == 1` (`0xCD26E`).
7. If the audio service (id 3) is non-null, the cue `0x334A0(3)` is called before the row moves and must return
   (`0xCD27C..0xCD28E`).

Also READ: the START-open path (`0xCFA59`) tests only `pressed & 0x100`, needs `state == 0`, and calls no cue. Every
other action calls the cue first. B or BACK release while `state == 1` (`0xCF9A8..0xCF9F1`) and A release on row 5
(`0xCF974`) close the menu **without** the owner check and without `0xCD0D0`.

A on rows 0..3 does nothing visible by design: `0xCD0D0` only plays the cue and sets `0x39F650` there (rows 0..3 react to
LEFT/RIGHT, row 4 to A, row 5 to A release). The cursor starts on row 0, so "A does nothing" before any cursor move is
expected. The real symptom is only the d-pad.

## 3. Q2: can anything stop UP/DOWN reaching `0xCD0D0` as 0x1000/0x2000

No unconditional blocker found in any of (a)..(d). The trigger bug is not a cause.

### Trigger bug (fixed): no effect on the pause d-pad

`pressed` is an edge word, so a steady LT|RT (0x5) is in both `held` and `prev` and never in `pressed` (READ, `0xDD2B0`).
A tap of UP while 0x5 is held gives `pressed == 0x1000` exactly. The repeat bits (0x5 every ~20 ticks, `0xDD770`) reach
`0xCD0D0` as a separate call with `pressed = repeat` (`0xCFA23..0xCFA3A`), where 0x5 matches nothing and returns at
`0xCD269` (READ). Repeat is per bit, so a held UP still repeats as 0x1000 except on a step where a trigger bit repeats too. A tap's
first move is unaffected. Not the cause of the pause report.

### (a) held/prev timing

- Per step, not per frame (READ, `0x11D40..0x11D9E`): pop a ring record (`0xD75E0`), `0xDD930`, `0x77040`
  (which calls `0x79B60` and so `0xCF6B0`), then pop again. One `DD930` and one module update per record.
- `0xDD930` per slot (READ, `0xDDB7D..0xDDBC3`): prev := old held, held := 0, repeat := 0, device fills a record,
  `0xDCE00` rebuilds held, `0xDD770` rebuilds repeat. So `prev == held` after a step only when two consecutive records
  carry the same bits, which is the normal "still held" case.
- Up to 4 pops per loop iteration: `0xD75E0` returns true for calls 1..4; the 5th pops a record and returns false, so
  that record is lost (READ, as in `simulation-tick.md`). A lost record cannot erase an edge: the next record popped
  carries the same bit against the older prev, so the edge appears one step later. An edge is lost only if every
  record holding the press is dropped, and a press is at least 120 ms (min-hold, `usb_gamepad.c`), about 7 records
  (INFERRED from READ). Not a blocker.
- If `0xDCE00` returns false for a slot, held stays 0 for that step (READ, `0xDDBBD`). A dropout then fakes a release
  and a new press. It adds edges; it does not remove the first one.

### (b) left-stick path and the 120 Hz clock

- Stick path, per slot (READ, `0xCF808..0xCF943`): x, y come from `0xDD3C0(0)` (already zero below 0.25 per
  `ps5-audit-input.md`). If both |dx| and |dy| against the stored previous are at or below the pad threshold `T`
  (`pad+0xCC`), the timer `0x2E3250` is set to 20.0 and the path is skipped; `pressed` stays the pad's. At rest the
  stick is exactly 0, so this is the path taken. `T`'s value is NOT FOUND (set in pad init, not traced).
- Only when the stick moved by more than `T` **and** `(tick - [MENU+0x38]) > timer` does `pressed` get replaced by the
  stick's new direction bits (`0xCF909..0xCF91C`), which would swallow a d-pad edge in that same step. Needs a
  deflected stick. INFERRED side effect: the stick state is per menu, so with the idle second pad on slot 1
  (`ps5-audit-input.md` defect 1) a deflected stick on slot 0 makes the stored previous alternate each step. Stick
  navigation only.
- Frozen clock: `tick - last` stays 0, never above the timer, so the stick path never fires, and `DD770` never reaches
  its repeat deadline (`0xDD7BF..0xDD7CF`). A tap still works (READ). Racing clock: the stick path fires on every
  movement, and a held bit repeats every step, so a tap of ~7 records could move the row ~7 times (6 rows: looks random
  or null). A wrap is handled by the `+2^32` fix (`0x286384` = 4294967296.0, READ) in `CF6B0`; `DD770`'s unsigned
  compare would misfire once per 2^32 ticks, 414 days. Not a blocker; at worst a repeat storm.
- What drives it (READ): `0x1A890` -> `0x1ED6D0(0x78)`; a kernel timer (`KeSetTimerEx`, 150) wakes the guest timer
  thread `0x1F98A2` (`KeWaitForMultipleObjects`, 158); callback `0x1ED620` re-arms via `0x1F9A8A` and sets the event
  at `0x3C9614`; counter thread `0x1ED5A0` does `++[0x3C9684]` once per event. A slow event rate gives a slow clock,
  not a fast one. Live check: `0x3C9684` should gain about 120 per second.

### (c) gates before `0xCF6B0`

Listed in section 2, items 2 and 3. Blocking cases, none shown to occur:

- `[RM+0x24] != 0` (a module change in progress). Not expected mid-fight.
- Service 0x28 (slot 0x2FB1E8) `+0x206` set. It is the generic popup; who sets it during a fight is NOT FOUND. START
  opening the menu means it was 0 at that tick; it could be set afterwards.
- `0x39C0C0 & 0x40`. Then START could not open this menu, so a visible pause menu would be a different one.
- `[MENU+0x23] == 0`, or the module `0x79B60` isn't the running one (type mismatch, section 1).

Once the menu is open, `0x79B60` clears its own `active` flag for the rest of the tick (`0x79C19`) after calling
`0xCF6B0`; that skips the fight update and the help popup, not the next tick's `0xCF6B0` (READ).

### (d) owner index vs the real pad slot

Owner is the loop index of whichever slot saw START (`0xCFA6C`). START and the d-pad sit in the same held word of that
slot, so one physical pad cannot split them (READ). It can mismatch only if a slot remap between START and the d-pad
press moves the pad (`0xDD630`, `0x1FC10`; not seen at runtime, INFERRED unlikely). A second slot mapped to the same
port loses the START race and is not owner (READ: START-open needs `state == 0`). Not a cause.

## 4. Q3: ranking

Nothing in the guest is proven to block. The only differences between START (works) and UP (fails) are: exact
equality, `state == 1`, owner match, and a cue before the move. So:

1. **The d-pad never forms a lone 0x1000/0x2000 edge on the owner slot.** Either the d-pad bit is not in that slot's
   held word (host side: `host_ps5.c` -> SDL virtual joystick -> `usb_gamepad.c`; the chain is clean on paper per
   `ps5-audit-input.md`, never observed on hardware), or it arrives with a second bit in the same step (a diagonal
   press, or a chord). Exact equality drops the second case silently. START cannot hit it. Highest, and cheapest to
   settle.
2. **`0xCF6B0` is no longer running after the open tick**, or the visible menu is not this one: `[RM+0x24]`, service
   0x28 `+0x206`, `0x39C0C0 & 0x40`, wrong module type.
3. **The first cue never returns.** `0x334A0` takes a mutex (`0x3AE10C`), walks listeners and uses the "PauseMenu_SFX"
   param; the START-open path has none of that, so this fails only after the open. The picture would freeze, not just
   ignore input (INFERRED).
4. **Input works, the highlight does not redraw.** Row moves in memory, nothing shows. The row only changes a variable
   for UP/DOWN (READ), so this is a draw question.
5. **Owner mismatch.** Section 3d.

Ruled out as causes (READ): the fixed trigger bug, held/prev collapse and ring record loss, the stick path and the
120 Hz clock (they can only add repeats or swallow an edge during a deflected stick).

## 5. What separates them

**Single peek**: the owner pad's held word, with the menu row and state, logged on every change while the menu is open
and the user taps UP once, then DOWN once. In the style of `ps5_gamewatch.c` (`xbox_PeekRead32`, read-only):

```
[PAUSE] st=%u row=%d own=%d en=%u | held[own]=%04x prev=%04x | RM.st=%u pop=%u aki=%u popup=%u
```

With `MENU` from section 1, `held[own]` from `PM+0x108+0xD4*own`, `RM.st = MEM32(RM+0x24)`, `pop` = the ring read index
already logged, `aki = MEM32(0x39C0C0)>>6 & 1`, `popup = MEM8(MEM32(0x2FB1E8)+0x206)`. `en` is the high byte of
`MEM32(MENU+0x20)`. Sample at about 4 ms so a tap is not missed; print only on change, plus a 1 Hz heartbeat.

| What the line shows on a UP tap | Cause |
|---|---|
| `held[own]` never gets 0x1000 | rank 1, host/mapping side |
| `held[own]` gets 0x1000 plus another bit, row unchanged | rank 1, exact equality |
| `held[own]` gets exactly 0x1000, row moves | input fine, rank 4 (draw) |
| exactly 0x1000, row unchanged, `st=1`, `own` = this slot, `pop` advancing, `RM.st`/`aki`/`popup`/`en` fine | not located by this line; dump `0x39F650` after an A tap |
| exactly 0x1000, row unchanged, `RM.st != 0` or `popup != 0` or `aki == 1` or `en == 0` | rank 2 |
| exactly 0x1000, `pop` stops advancing | rank 3 |
| `st` stays 0 after START but a menu is on screen | different menu (`aki`) |
| `held` changes on a slot other than `own` | rank 5 |

No code needed first: with the menu open, tap B. It closes the menu through a path with no owner check and no exact
bits (section 2), so a close means `0xCF6B0` is running, the gates are open and face buttons reach PadMgr. Then, after
reopening, tap A once and read `0x39F650`: 1 means the cue returned and the whole dispatch is sound, which leaves
only the d-pad bits (rank 1) and the draw (rank 4).

## 6. Not found

- `aki_cfg.xml` contents (`pause_use_aki`). Not extracted locally.
- Who sets service 0x28 `+0x206` during a fight; listeners of bus messages 0xF0000000 / 0xF0000001 (posted at open and
  close).
- Pad threshold `T` (`pad+0xCC`) and the delay and interval at `pad+0xB8` / `+0xBC`.
- Whether the fight module is type 2 (only inferred from the help-popup call site).
- Names of rows 3 and 4 (a toggle at `+0x21`, a quit confirm at `+0x20`).

## Address index

| Guest address | What |
|---|---|
| 0x2FB148 / 0x2FB1A4 / 0x2FB1AC / 0x2FB1E8 | registry / RunModuleMan / PadMgr / generic popup |
| 0x29D5E4 / 0x29D604 | module vtables, type 2 / type 3 |
| 0x793A0, 0x798A0, 0x79B60 | type 2 init, enter, update |
| 0x7A2B0, 0x7A730, 0x7A8F0 | type 3 init, enter, update |
| 0x77040 | RunModuleMan step, calls module slot 3 |
| 0x11CD0 | main loop, `0xD75E0` pop -> `0xDD930` -> `0x77040` |
| 0xCF6B0 | pause menu per-tick (START, close, stick, dispatch) |
| 0xCD0D0 | pause menu action dispatch, exact equality |
| 0xCCE60 / 0xCCF20 / 0xCD020 | constructor / open (reads volumes) / apply volumes |
| 0x334A0 | UI cue, mutex 0x3AE10C |
| 0xDD930 / 0xDCE00 / 0xDD770 / 0xDCFB0 | pad update / held / repeat / combine |
| 0xDD2B0 / 0xDD390 / 0xDD3C0 / 0xDCBA0 / 0xDCC50 | read mode / bit release / stick / slot lookup / null pad test |
| 0x1EDB70 / 0x3C9684 | 120 Hz tick read / counter |
| 0x39F650 / 0x39C0C0 / 0x2E3250 | A latch / flags (0x40 = `pause_use_aki`) / stick timer |
