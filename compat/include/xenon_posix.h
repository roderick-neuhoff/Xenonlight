/*
 * Force-included (-include xenon_posix.h) into every translation unit that
 * uses POSIX sockets/threads: moonlight-common-c, ENet and our own code.
 *
 * Functions that newlib/libxenon already provide (close, fcntl, select,
 * sleep...) are redirected with macros so socket fds reach xsock and
 * sleeps yield to other coroutines instead of spinning.
 */
#ifndef XENON_POSIX_H
#define XENON_POSIX_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

int xenon_close(int fd);
int xenon_fcntl(int fd, int cmd, ...);
int xenon_select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout);
int xenon_usleep(useconds_t us);
unsigned int xenon_sleep(unsigned int seconds);
int xenon_nanosleep(const struct timespec *req, struct timespec *rem);
int xenon_clock_gettime(clockid_t clk, struct timespec *ts);
int xenon_sched_yield(void);

#ifndef XENON_POSIX_IMPL
#define close(fd)       xenon_close(fd)
#define fcntl           xenon_fcntl
#define select          xenon_select
#define usleep          xenon_usleep
#define sleep           xenon_sleep
#define nanosleep       xenon_nanosleep
#define clock_gettime   xenon_clock_gettime
#define sched_yield     xenon_sched_yield
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME ((clockid_t)1)
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC ((clockid_t)4)
#endif
#ifndef HAVE_CLOCK_GETTIME
#define HAVE_CLOCK_GETTIME 1
#endif

#ifdef __cplusplus
}
#endif

#endif
