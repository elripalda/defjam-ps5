/*
 * ps5_libc_compat.c -- [ps5 port] a few C library functions the SDK's
 * headers and the desktop code expect, which no module the app links against
 * exports (they live only in the payload SDK's static libc, which an app does
 * not link).
 */
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/* FreeBSD's <ctype.h> reaches the locale's tables through these. Sony's libc
 * exports the C locale's table as _CurrentRuneLocale. */
struct _RuneLocale;
extern const struct _RuneLocale *_CurrentRuneLocale;

__attribute__((weak)) const struct _RuneLocale *__getCurrentRuneLocale(void)
{
    return _CurrentRuneLocale;
}

/* Called by <ctype.h> only for values outside the table's 0-255. */
__attribute__((weak)) int ___toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
__attribute__((weak)) int ___tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* UTC broken-down time to seconds since 1970 (days from the civil date). */
__attribute__((weak)) time_t timegm(struct tm *tm)
{
    long long y = (long long)tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    long long era, yoe, doy, doe, days;

    /* Normalise the month into 1..12 first, as mktime would. */
    y += (m - 1) / 12;
    m = (m - 1) % 12 + 1;
    if (m <= 0) { m += 12; y--; }
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + (long long)tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

/* Only SDL's GTK helper asks; an app runs as one user. */
__attribute__((weak)) int getresuid(uid_t *r, uid_t *e, uid_t *s)
{
    if (r) *r = 0;
    if (e) *e = 0;
    if (s) *s = 0;
    return 0;
}

__attribute__((weak)) int getresgid(gid_t *r, gid_t *e, gid_t *s)
{
    if (r) *r = 0;
    if (e) *e = 0;
    if (s) *s = 0;
    return 0;
}

/* ── Functions an app must not import ─────────────────────────────────────
 * These come only from libkernel_sys and libScePosixForWebKit, which a normal
 * app cannot load: one import from either and the console closes the app
 * before any of its code runs. Their callers are desktop paths (SDL's process
 * and dialog code, terminal checks, temp files) that never run here.
 *
 * ps5/tools/ps5_package.sh leaves those two modules out of the link, so these
 * definitions are the only ones and nothing is imported (or exported). They
 * are weak only so the plain CMake link, which also sees the SDK's static
 * libc, does not count them twice. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

__attribute__((weak)) pid_t fork(void) { errno = ENOSYS; return -1; }
__attribute__((weak)) pid_t vfork(void) { errno = ENOSYS; return -1; }
__attribute__((weak)) pid_t setsid(void) { errno = ENOSYS; return -1; }
__attribute__((weak)) int link(const char *a, const char *b) { (void)a; (void)b; errno = ENOSYS; return -1; }
__attribute__((weak)) int symlink(const char *a, const char *b) { (void)a; (void)b; errno = ENOSYS; return -1; }
__attribute__((weak)) ssize_t readlink(const char *p, char *b, size_t n)
{ (void)p; (void)b; (void)n; errno = EINVAL; return -1; }
__attribute__((weak)) long pathconf(const char *p, int name) { (void)p; (void)name; return 255; }
__attribute__((weak)) int isatty(int fd) { (void)fd; errno = ENOTTY; return 0; }

__attribute__((weak)) int mkstemp(char *t)
{
    static unsigned n;
    size_t len = strlen(t);
    int tries;
    if (len < 6 || strcmp(t + len - 6, "XXXXXX")) { errno = EINVAL; return -1; }
    for (tries = 0; tries < 1000; tries++) {
        int fd;
        snprintf(t + len - 6, 7, "%06u", (n++ * 2654435761u + (unsigned)getpid()) % 1000000u);
        fd = open(t, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    return -1;
}

/* '*', '?' and literal characters; enough for SDL's file globbing. */
__attribute__((weak)) int fnmatch(const char *p, const char *s, int flags)
{
    (void)flags;
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return 0;
            for (; *s; s++)
                if (!fnmatch(p + 1, s, flags)) return 0;
            return 1;
        }
        if (!*s || (*p != '?' && *p != *s)) return 1;
    }
    return *s ? 1 : 0;
}
