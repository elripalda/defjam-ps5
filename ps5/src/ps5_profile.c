/*
 * ps5_profile.c -- [ps5 port] a sampling profiler for the console build.
 *
 * Every thread the runtime starts registers here. A sampler thread signals
 * them one at a time, about a thousand times a second in all, and the signal
 * handler records where the thread was: its instruction pointer and the first
 * return address on its stack that points into this program (so time spent
 * in a system call is charged to the code that made it). Every ten seconds
 * each thread's busiest places go to logs/ps5.log, with how often the thread
 * was found waiting in the system. The addresses are named offline against
 * the build's llvm-pie.elf (the image starts at 0x400000).
 */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void ps5_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int host_posix_run(int (*)(void));

#define PROF_THREADS 48
#define PROF_SLOTS   1024
#define PROF_TOP     10

typedef struct {
    uint64_t pc, caller;
    uint32_t count;
} prof_slot;

typedef struct {
    pthread_t thread;
    char      name[40];
    int       live;
    uint32_t  samples, in_system, missed, lost;
    prof_slot slot[PROF_SLOTS];
} prof_thread;

static prof_thread s_threads[PROF_THREADS];
static volatile int s_count;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int s_target = -1;
static volatile int s_seq;
static uint64_t s_base, s_end;

static int in_image(uint64_t v)
{
    return v >= s_base && v < s_end;
}

/* Registers the calling thread, or renames it when it is already there (a
 * guest thread starts as a host thread, then joins the guest CPU). */
void ps5_prof_register(const char *name)
{
    pthread_t self = pthread_self();
    int i, n;

    pthread_mutex_lock(&s_lock);
    n = s_count;
    for (i = 0; i < n; i++)
        if (s_threads[i].live && pthread_equal(s_threads[i].thread, self))
            break;
    if (i == n) {
        if (n >= PROF_THREADS) {
            pthread_mutex_unlock(&s_lock);
            return;
        }
        memset(&s_threads[n], 0, sizeof s_threads[n]);
        s_threads[n].thread = self;
        s_threads[n].live = 1;
        __atomic_store_n(&s_count, n + 1, __ATOMIC_RELEASE);
    }
    snprintf(s_threads[i].name, sizeof s_threads[i].name, "%s", name);
    pthread_mutex_unlock(&s_lock);
}

void ps5_prof_unregister(void)
{
    pthread_t self = pthread_self();
    int i;
    pthread_mutex_lock(&s_lock);
    for (i = 0; i < s_count; i++)
        if (s_threads[i].live && pthread_equal(s_threads[i].thread, self))
            s_threads[i].live = 0;
    pthread_mutex_unlock(&s_lock);
}

static void prof_handler(int sig, siginfo_t *si, void *uctx)
{
    const uint64_t *regs = (const uint64_t *)((uint8_t *)uctx + 64);
    int t = s_target;
    (void)sig; (void)si;
    if (t >= 0 && t < PROF_THREADS) {
        prof_thread *pt = &s_threads[t];
        uint64_t pc = regs[20], caller = 0;
        const uint64_t *sp = (const uint64_t *)regs[23];
        /* Only inside the 16 KB page rsp is in: the top of a stack can be the
         * end of its mapping. */
        int i, n = (int)((0x4000 - ((uintptr_t)sp & 0x3FFF)) / 8);
        uint32_t h, k;
        if (n > 64)
            n = 64;
        for (i = 0; i < n; i++)
            if (in_image(sp[i])) {
                caller = sp[i];
                break;
            }
        pt->samples++;
        if (!in_image(pc))
            pt->in_system++;
        h = (uint32_t)((pc * 0x9E3779B97F4A7C15ull) ^ (caller * 0xC2B2AE3D27D4EB4Full)) >> 0;
        for (k = 0; k < 16; k++) {
            prof_slot *s = &pt->slot[(h + k) & (PROF_SLOTS - 1)];
            if (s->count == 0) {
                s->pc = pc;
                s->caller = caller;
                s->count = 1;
                break;
            }
            if (s->pc == pc && s->caller == caller) {
                s->count++;
                break;
            }
        }
        if (k == 16)
            pt->lost++;
    }
    __atomic_add_fetch(&s_seq, 1, __ATOMIC_RELEASE);
}

static void *sampler_thread(void *unused)
{
    (void)unused;
    for (;;) {
        int n = __atomic_load_n(&s_count, __ATOMIC_ACQUIRE), i;
        for (i = 0; i < n; i++) {
            int seq, w;
            if (!s_threads[i].live)
                continue;
            seq = __atomic_load_n(&s_seq, __ATOMIC_ACQUIRE);
            s_target = i;
            if (pthread_kill(s_threads[i].thread, SIGUSR1) != 0) {
                s_threads[i].live = 0;
                continue;
            }
            /* Wait for the handler, which the thread runs when it next gets a
             * CPU: a long wait here is itself the measurement (missed). */
            for (w = 0; w < 40 && __atomic_load_n(&s_seq, __ATOMIC_ACQUIRE) == seq; w++)
                usleep(100);
            if (__atomic_load_n(&s_seq, __ATOMIC_ACQUIRE) == seq) {
                s_threads[i].missed++;
                /* Let the late handler land on nobody rather than the next
                 * thread's table. */
                s_target = -1;
                for (w = 0; w < 200 && __atomic_load_n(&s_seq, __ATOMIC_ACQUIRE) == seq; w++)
                    usleep(100);
            }
            s_target = -1;
            usleep(1000);
        }
        if (n == 0)
            usleep(10000);
    }
    return NULL;
}

void ps5_prof_start(void)
{
    struct sigaction sa;
    pthread_t th;

    s_base = (uint64_t)(uintptr_t)&host_posix_run & ~0xFFFFFull;
    s_end = s_base + 0x1C00000;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    if (pthread_create(&th, NULL, sampler_thread, NULL) == 0)
        pthread_detach(th);
}

/* Each thread's busiest places since the last report, then a fresh start.
 * The tables are read while the sampler may be adding to them: a count off by
 * one is fine for this. */
void ps5_prof_report(unsigned secs)
{
    int n = __atomic_load_n(&s_count, __ATOMIC_ACQUIRE), i;
    ps5_logf("[PROF] --- %u s ---\n", secs);
    for (i = 0; i < n; i++) {
        prof_thread *pt = &s_threads[i];
        prof_slot top[PROF_TOP];
        int j, k;
        if (!pt->samples && !pt->missed)
            continue;
        memset(top, 0, sizeof top);
        for (j = 0; j < PROF_SLOTS; j++) {
            prof_slot s = pt->slot[j];
            if (!s.count)
                continue;
            for (k = 0; k < PROF_TOP; k++)
                if (s.count > top[k].count) {
                    memmove(&top[k + 1], &top[k], (PROF_TOP - 1 - k) * sizeof top[0]);
                    top[k] = s;
                    break;
                }
        }
        ps5_logf("[PROF] %-24s samples %u, in system %u, not running %u\n",
                 pt->name, pt->samples, pt->in_system, pt->missed);
        for (k = 0; k < PROF_TOP && top[k].count; k++)
            ps5_logf("[PROF]   %5u  %lx < %lx\n", top[k].count,
                     (unsigned long)top[k].pc, (unsigned long)top[k].caller);
        memset(pt->slot, 0, sizeof pt->slot);
        pt->samples = pt->in_system = pt->missed = pt->lost = 0;
    }
}
