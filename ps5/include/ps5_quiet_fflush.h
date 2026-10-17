/*
 * ps5_quiet_fflush.h -- [ps5 port] force-included into every C file of the
 * console build (ps5/cmake/ps5.cmake).
 *
 * The runtime flushes stderr after hundreds of its messages, from whichever
 * thread wrote them: the title's own threads, the interrupt thread, the GPU
 * executor. On the PS5 a flush is a write to the app's file system that takes
 * about a millisecond and every few seconds about 0.3 s, and the stream stays
 * locked for all of it, so a flush on a game thread was a hitch. Here a flush
 * of stdout or stderr does nothing; the port's flush thread writes both out
 * once a second. Code that must write now (the crash report, the flush
 * thread) calls the real one as (fflush)(stream).
 */
#ifndef PS5_QUIET_FFLUSH_H
#define PS5_QUIET_FFLUSH_H
#include <stdio.h>

static inline int ps5_quiet_fflush(FILE *f)
{
    return (f == stdout || f == stderr) ? 0 : (fflush)(f);
}
#define fflush(f) ps5_quiet_fflush(f)

#endif
