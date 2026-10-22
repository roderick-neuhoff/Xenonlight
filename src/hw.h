/*
 * Helpers for work that runs on the secondary hardware threads (1-5).
 * Hardware thread 0 runs the xthread scheduler; nothing here may call
 * xthread, lwIP or moonlight-common-c functions.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Threads 1, 3 and 5 are FFmpeg's slice workers (hwpthread/hwpthread.c). */
#define HW_THREAD_DECODE 2   /* core 1, thread 0 */
#define HW_THREAD_RENDER 4   /* core 2, thread 0 */

/* Starts fn on the given hardware thread with a private 1 MiB stack. */
bool hw_start(int hw_thread, void (*fn)(void));
bool hw_running(int hw_thread);

/* Full memory barrier, for publishing data between hardware threads. */
static inline void hw_sync(void) { __asm__ volatile("sync" ::: "memory"); }
static inline void hw_lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }

/* Lowers SMT priority while spinning so the sibling thread runs faster. */
static inline void hw_relax(void) { __asm__ volatile("or 1,1,1\n\tor 2,2,2" ::: "memory"); }

uint64_t hw_now_us(void);
