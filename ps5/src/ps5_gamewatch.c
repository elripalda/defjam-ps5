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
#define FTH_SHOWING    0x185u
#define FTH_LOCKOUT    0x24Cu

static uint32_t peek(uint32_t va, int *ok)
{
    uint32_t v = 0;
    if (!xbox_PeekRead32(va, &v))
        *ok = 0;
    return v;
}

static void *watch_thread(void *unused)
{
    uint32_t last_tick = 0, last_r = 0, last_w = 0, last_obj = 0;
    unsigned ticks = 0, pushes = 0, pops = 0, max_n = 0, samples = 0;
    unsigned pad_a = 0, pad_a_press = 0, pad_start = 0, cur_a = 0, cur_a_press = 0;
    unsigned cur_start = 0, cur_down = 0, last_frames = 0, secs = 0, quiet = 0;
    int pad_a_was = 0, cur_a_was = 0, primed = 0;
    uint32_t state = 0, status = 0, held_or = 0, held_last = 0, p0_or = 0;
    unsigned held_zero = 0, fth_up = 0, fth_lock = 0;
    (void)unused;

    sleep(5);
    for (;;) {
        int ok = 1;
        uint32_t obj = peek(INPUT_OBJECT, &ok);
        usleep(2000);
        if (ok && obj) {
            uint32_t tick = peek(obj + OFF_CLOCK_LAST, &ok);
            uint32_t r = peek(obj + OFF_RING_READ, &ok);
            uint32_t w = peek(obj + OFF_RING_WRITE, &ok);
            uint32_t n = peek(obj + OFF_RING_COUNT, &ok);
            uint32_t a_bits = peek(obj + OFF_CUR_PORT0 + REC_A, &ok);
            uint32_t st = peek(obj + OFF_CUR_PORT0 + REC_START, &ok);
            uint32_t dn = peek(obj + OFF_CUR_PORT0 + REC_DOWN, &ok);
            uint32_t pad = peek(PAD0_BUTTONS, &ok);
            float a;
            if (!ok)
                continue;
            state = peek(obj + OFF_CLOCK_STATE, &ok);
            status = peek(obj + OFF_CUR_STATUS, &ok);
            memcpy(&a, &a_bits, sizeof a);
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
                uint32_t all = ok2 && mgr ? peek(mgr + PADMGR_ALL_HELD, &ok2) : 0;
                uint32_t p0 = ok2 && mgr ? peek(mgr + PADMGR_P0_HELD, &ok2) : 0;
                uint32_t up = peek(FTH_OBJECT + FTH_SHOWING - 1, &ok2);
                uint32_t lock = peek(FTH_OBJECT + FTH_LOCKOUT, &ok2);
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
        if (samples >= 480 || (!ok && ++quiet >= 480)) {
            unsigned frames = g_vk_frames_presented;
            secs++;
            if (samples)
                ps5_logf("[WATCH] %u: clock %u ticks (state %u), %u pushed, %u popped (ring max %u),"
                         " %u frames | pad A %u/%u (%u presses), START %u |"
                         " game port0 %u: A %u (%u presses), START %u, DOWN %u |"
                         " held any %04X now %04X, none %u/%u, slot0 %04X | help popup %u (lockout %u)\n",
                         secs, ticks, (unsigned)state, pushes, pops, max_n, frames - last_frames,
                         pad_a, samples, pad_a_press, pad_start, (unsigned)status,
                         cur_a, cur_a_press, cur_start, cur_down,
                         (unsigned)held_or, (unsigned)held_last, held_zero, samples, (unsigned)p0_or,
                         fth_up, (unsigned)fth_lock);
            last_frames = frames;
            ticks = pushes = pops = max_n = samples = quiet = 0;
            pad_a = pad_a_press = pad_start = cur_a = cur_a_press = cur_start = cur_down = 0;
            held_or = p0_or = 0;
            held_zero = fth_up = 0;
        }
    }
    return NULL;
}

void ps5_gamewatch_start(void)
{
    pthread_t th;
    if (pthread_create(&th, NULL, watch_thread, NULL) == 0)
        pthread_detach(th);
}
