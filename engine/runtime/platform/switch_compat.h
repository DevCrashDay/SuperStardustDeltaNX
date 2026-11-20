/*
 * Force-included into every host file of a Switch (libnx / devkitA64) build
 * (-include switch_compat.h): fills the few POSIX/newlib gaps so the runtime stays unmodified.
 */
#ifndef ATIV_SWITCH_COMPAT_H
#define ATIV_SWITCH_COMPAT_H
#ifdef __SWITCH__
#include <sys/types.h>
#include <pthread.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifndef _setjmp
#define _setjmp(buf) setjmp(buf)
#endif
#ifndef _longjmp
#define _longjmp(buf, v) longjmp((buf), (v))
#endif

static inline int switch_pthread_setname_np(pthread_t t, const char *n) { (void)t; (void)n; return 0; }
static inline int switch_pthread_getname_np(pthread_t t, char *b, size_t s) { (void)t; if (s) b[0] = 0; return 0; }
#define pthread_setname_np(t, n) switch_pthread_setname_np((t), (n))
#define pthread_getname_np(t, b, s) switch_pthread_getname_np((t), (b), (s))
#define sync() ((void)0)

#ifdef __cplusplus
extern "C" {
#endif

ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
char *realpath(const char *path, char *resolved);
time_t timegm(struct tm *tm);
void plat_map_guest(uint32_t gaddr, uint32_t size);

#ifdef __cplusplus
}
#endif
#endif
#endif
