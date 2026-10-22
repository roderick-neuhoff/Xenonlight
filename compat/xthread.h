/*
 * xthread: cooperative user-mode threads for libxenon.
 *
 * libxenon has no OS scheduler, so every pthread-style thread used by
 * moonlight-common-c runs as a coroutine on hardware thread 0. A thread
 * gives up the CPU only when it blocks (socket wait, mutex, condvar, sleep)
 * or calls xthread_yield(). Each pass through the scheduler also pumps the
 * lwIP stack and the USB stack so that blocking waits make progress.
 *
 * None of these functions may be called from the secondary hardware threads
 * (the video decoder). Use the hw_* primitives in hwthread.h for those.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct xthread xthread_t;
typedef void *(*xthread_entry_t)(void *arg);

/* Must be called once from main() on hardware thread 0 before anything else. */
void xthread_init(void);

xthread_t *xthread_create(const char *name, xthread_entry_t entry, void *arg, size_t stack_size);
xthread_t *xthread_self(void);
/* A pointer-sized slot per thread for layers built on top (NULL initially). */
void **xthread_user(xthread_t *t);
void xthread_yield(void);
void xthread_exit(void *ret) __attribute__((noreturn));
int xthread_join(xthread_t *t, void **ret);
int xthread_detach(xthread_t *t);

/* Sleeps for at least the given time, running other threads meanwhile. */
void xthread_sleep_us(uint64_t us);

/* Monotonic clock derived from the PPC timebase. */
uint64_t xthread_now_us(void);

/*
 * Registers a function that runs on every scheduler pass (network and
 * USB polling). min_interval_us throttles expensive pumps.
 */
void xthread_add_pump(void (*fn)(void), uint64_t min_interval_us);

/* Number of live threads, for diagnostics. */
int xthread_count(void);
