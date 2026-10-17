/*
 * ps5_gamewatch.c -- [ps5 port] the title's own input path, sampled from
 * outside it.
 *
 * Def Jam reads the pads on a thread of its own: a 60 Hz frame clock (a
 * Direct3D timer callback that re-arms itself) wakes it, it polls the four
 * ports into a table at 0x3C92B8 and pushes a snapshot into a ten-entry ring
 * in its input object; the main loop pops one snapshot per game update. The
 * in-fight help popups and the pause menu ignore the pad on the console port
 * while the fight itself does not, so this says, once a second, whether each
 * stage of that path is moving: the clock's ticks, snapshots pushed, snapshots
 * popped (game updates), and whether A is down in the polled table and in the
 * snapshot the game is reading. Then the game's pad manager, which turns the
 * snapshot into held bits: the help popup closes only on a tick when nothing
 * at all is held and A was just let go, so "none" (samples with no held bit)
 * and the bits seen held say whether that can ever happen. Lines go to
 * logs/ps5.log as [WATCH].
 *
 * Read-only: it never writes guest memory.
 */
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void ps5_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int xbox_PeekRead32(uint32_t va, uint32_t *out);
extern volatile unsigned g_vk_frames_presented;

#define INPUT_OBJECT   0x003A0178u  /* the input object (also at 0x3A017C)  */
#define OFF_RING_READ  0x17C4u
#define OFF_RING_WRITE 0x17C8u
#define OFF_RING_COUNT 0x17CCu
#define OFF_CUR_STATUS 0x17D0u      /* port 0's status in the current snapshot */
#define OFF_CUR_PORT0  0x17E0u      /* port 0's record in the current snapshot */
#define REC_A          0x24u        /* {u8 valid; float A} */
#define REC_START      0x44u
#define REC_DOWN       0x6Cu
#define OFF_CLOCK_STATE 0x1A78u
#define OFF_CLOCK_LAST  0x1A88u     /* the clock's last tick time, low half */
#define PAD0_BUTTONS   0x003C92C8u  /* digital u16, then analog A, B */
#define PADMGR_SLOT    0x002FB1ACu  /* service 0x19 in the registry at 0x2FB148 */
#define PADMGR_ALL_HELD 0x7A8u      /* the combined pad's held bits (+0x724 + 0x84) */
#define PADMGR_P0_HELD 0x108u       /* pad slot 0's held bits (+0x84 + 0x84) */
#define FTH_OBJECT     0x00346A68u  /* the first-time help popup (a static object) */
/* The in-fight pause menu (docs/research/ps5-audit-pause.md): the run-module
 * manager is service 0x17, its current module at +0x18; a type-2 module
 * (vtable 0x29D5E4) keeps the menu at +0x2C, a type-3 one (0x29D604) at +0x34.
 * The menu: +0x23 enabled byte, +0x24 state (0 closed, 1 open, 4 confirm),
 * +0x28 row 0..5, +0x2C the pad slot that opened it. */
#define RUNMOD_SLOT    0x002FB1A4u
#define POPUP_SLOT     0x002FB1E8u  /* service 0x28, the generic popup; +0x206 busy */
#define AKI_FLAGS      0x0039C0C0u  /* & 0x40: the pause menu is not run */
#define PAUSE_A_LATCH  0x0039F650u  /* set by the first A the menu takes */
#define FTH_SHOWING    0x185u
#define FTH_LOCKOUT    0x24Cu

static uint32_t peek(uint32_t va, int *ok)
{
    uint32_t v = 0;
    if (!xbox_PeekRead32(va, &v))
        *ok = 0;
    return v;
}

static int heap_ptr(uint32_t v)
{
    return v >= 0x10000u && v < 0x04000000u;   /* inside the title's 64 MB */
}

/* The pause menu's fields, or 0 when there is none. */
static int pause_menu(uint32_t *state, uint32_t *row, uint32_t *owner, uint32_t *enabled)
{
    int ok = 1;
    uint32_t rm = peek(RUNMOD_SLOT, &ok), cur, vt, menu;
    if (!ok || !heap_ptr(rm))
        return 0;
    cur = peek(rm + 0x18, &ok);
    if (!ok || !heap_ptr(cur))
        return 0;
    vt = peek(cur, &ok);
    if (!ok || (vt != 0x0029D5E4u && vt != 0x0029D604u))
        return 0;
    menu = peek(cur + (vt == 0x0029D5E4u ? 0x2Cu : 0x34u), &ok);
    if (!ok || !heap_ptr(menu))
        return 0;
    *enabled = (peek(menu + 0x20, &ok) >> 24) & 0xFF;
    *state = peek(menu + 0x24, &ok);
    *row = peek(menu + 0x28, &ok);
    *owner = peek(menu + 0x2C, &ok);
    return ok;
}

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

void ps5_off_guest_cpu(void);

static void *watch_thread(void *unused)
{
    uint32_t last_tick = 0, last_r = 0, last_w = 0, last_obj = 0;
    unsigned ticks = 0, pushes = 0, pops = 0, max_n = 0, samples = 0;
    unsigned pad_a = 0, pad_a_press = 0, pad_start = 0, cur_a = 0, cur_a_press = 0;
    unsigned cur_start = 0, cur_down = 0, last_frames = 0, idle_windows = 0;
    int pad_a_was = 0, cur_a_was = 0, primed = 0;
    uint32_t state = 0, status = 0, held_or = 0, held_last = 0, p0_or = 0;
    unsigned held_zero = 0, fth_up = 0, fth_lock = 0;
    uint32_t p_state = 0xFFFFFFFFu, p_row = 0xFFFFFFFFu, p_owner = 0xFFFFFFFFu, p_en = 0xFFFFFFFFu;
    unsigned pause_lines = 0;
    uint64_t start, window;
    (void)unused;

    sleep(5);
    start = window = now_ms();
    last_frames = g_vk_frames_presented;
    for (;;) {
        int ok = 1;
        uint32_t obj;
        uint64_t t;

        usleep(2000);
        obj = peek(INPUT_OBJECT, &ok);
        if (ok && heap_ptr(obj)) {
            uint32_t tick = peek(obj + OFF_CLOCK_LAST, &ok);
            uint32_t r = peek(obj + OFF_RING_READ, &ok);
            uint32_t w = peek(obj + OFF_RING_WRITE, &ok);
            uint32_t n = peek(obj + OFF_RING_COUNT, &ok);
            uint32_t a_bits = peek(obj + OFF_CUR_PORT0 + REC_A, &ok);
            uint32_t st = peek(obj + OFF_CUR_PORT0 + REC_START, &ok);
            uint32_t dn = peek(obj + OFF_CUR_PORT0 + REC_DOWN, &ok);
            uint32_t pad = peek(PAD0_BUTTONS, &ok);
            uint32_t cstate = peek(obj + OFF_CLOCK_STATE, &ok);
            uint32_t cstatus = peek(obj + OFF_CUR_STATUS, &ok);
            if (ok) {
                float a;
                memcpy(&a, &a_bits, sizeof a);
                state = cstate;
                status = cstatus;
                if (primed && obj == last_obj) {
                    if (tick != last_tick) ticks++;
                    if (r < 10 && last_r < 10) pops += (r + 10 - last_r) % 10;
                    if (w < 10 && last_w < 10) pushes += (w + 10 - last_w) % 10;
                }
                primed = 1;
                last_obj = obj; last_tick = tick; last_r = r; last_w = w;
                if (n > max_n) max_n = n;
                samples++;
                {
                    /* The game's own pad manager: held bits are LT 1, White 2,
                     * RT 4, Black 8, A 0x10, B 0x20, X 0x40, Y 0x80, START 0x100,
                     * BACK 0x200, L3 0x400, R3 0x800, then UP, DOWN, LEFT, RIGHT. */
                    int ok2 = 1;
                    uint32_t mgr = peek(PADMGR_SLOT, &ok2);
                    uint32_t all = 0, p0 = 0, up, lock;
                    if (ok2 && heap_ptr(mgr)) {
                        all = peek(mgr + PADMGR_ALL_HELD, &ok2);
                        p0 = peek(mgr + PADMGR_P0_HELD, &ok2);
                    }
                    up = peek(FTH_OBJECT + FTH_SHOWING - 1, &ok2);
                    lock = peek(FTH_OBJECT + FTH_LOCKOUT, &ok2);
                    if (ok2) {
                        held_or |= all & 0xFFFF;
                        p0_or |= p0 & 0xFFFF;
                        held_last = all & 0xFFFF;
                        held_zero += (all & 0xFFFF) == 0;
                        if ((up >> 8) & 0xFF) {
                            fth_up++;
                            fth_lock = lock;
                        }
                    }
                }
                {
                    int down = ((pad >> 16) & 0xFF) > 0x1E;
                    pad_a += down;
                    pad_a_press += down && !pad_a_was;
                    pad_a_was = down;
                    pad_start += (pad & 0x10) != 0;
                }
                {
                    int down = a > 0.12f;
                    cur_a += down;
                    cur_a_press += down && !cur_a_was;
                    cur_a_was = down;
                    cur_start += st != 0;
                    cur_down += dn != 0;
                }
            }
        }

        {
            /* The pause menu: a line whenever its state, row, owner or
             * enabled byte changes, with the gates in front of its update,
             * the latch its first A sets, and the owner pad's held bits. */
            uint32_t st, row, own, en;
            if (pause_menu(&st, &row, &own, &en)
                    && (st != p_state || row != p_row || own != p_owner || en != p_en)) {
                int ok3 = 1;
                uint32_t rm = peek(RUNMOD_SLOT, &ok3);
                uint32_t popup = peek(POPUP_SLOT, &ok3);
                uint32_t aki = peek(AKI_FLAGS, &ok3);
                uint32_t latch = peek(PAUSE_A_LATCH, &ok3);
                uint32_t mgr = peek(PADMGR_SLOT, &ok3);
                uint32_t rm_busy = heap_ptr(rm) ? peek(rm + 0x24, &ok3) : 0xFFFFFFFFu;
                uint32_t pop_busy = heap_ptr(popup) ? (peek(popup + 0x204, &ok3) >> 16) & 0xFF : 0xFFu;
                uint32_t held = (heap_ptr(mgr) && own < 4) ? peek(mgr + 0x108 + 0xD4 * own, &ok3) : 0;
                if (pause_lines++ < 400)
                    ps5_logf("[WATCH] pause menu: state %u row %u owner %u enabled %u | gates: module busy %u,"
                             " popup busy %u, aki %u | first-A latch %u | owner held %04X\n",
                             (unsigned)st, (unsigned)row, (unsigned)own, (unsigned)en,
                             (unsigned)rm_busy, (unsigned)pop_busy, (unsigned)((aki >> 6) & 1),
                             (unsigned)(latch & 0xFF), (unsigned)(held & 0xFFFF));
                p_state = st; p_row = row; p_owner = own; p_en = en;
            }
        }

        t = now_ms();
        if (t - window < 1000)
            continue;
        ps5_off_guest_cpu();
        {
            unsigned frames = g_vk_frames_presented;
            unsigned secs = (unsigned)((t - start) / 1000) + 5;
            if (samples)
                ps5_logf("[WATCH] %u s (%u ms): clock %u ticks (state %u), %u pushed, %u popped"
                         " (ring max %u), %u frames | pad A %u/%u (%u presses), START %u |"
                         " game port0 %u: A %u (%u presses), START %u, DOWN %u |"
                         " held any %04X now %04X, none %u/%u, slot0 %04X | help popup %u (lockout %u)\n",
                         secs, (unsigned)(t - window), ticks, (unsigned)state, pushes, pops, max_n,
                         frames - last_frames, pad_a, samples, pad_a_press, pad_start,
                         (unsigned)status, cur_a, cur_a_press, cur_start, cur_down,
                         (unsigned)held_or, (unsigned)held_last, held_zero, samples, (unsigned)p0_or,
                         fth_up, (unsigned)fth_lock);
            else if (idle_windows++ % 10 == 0)
                ps5_logf("[WATCH] %u s: the game's input object is not up yet\n", secs);
            last_frames = frames;
        }
        window = t;
        ticks = pushes = pops = max_n = samples = 0;
        pad_a = pad_a_press = pad_start = cur_a = cur_a_press = cur_start = cur_down = 0;
        held_or = p0_or = 0;
        held_zero = fth_up = 0;
    }
    return NULL;
}

void ps5_gamewatch_start(void)
{
    pthread_t th;
    if (pthread_create(&th, NULL, watch_thread, NULL) == 0)
        pthread_detach(th);
}
