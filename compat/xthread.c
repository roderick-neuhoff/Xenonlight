/*
 * Cooperative coroutine scheduler for hardware thread 0. See xthread.h.
 */
#include "xthread.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ppc/timebase.h>

#define XTHREAD_DEFAULT_STACK (256 * 1024)
#define XTHREAD_MAX_PUMPS 8

enum { XT_RUNNABLE, XT_DONE };

/* Register save area, layout shared with xthread_switch.S. */
struct xctx {
    uint64_t r1, lr, cr, r2;
    uint64_t gpr[19];        /* r13..r31 */
    double fpr[18];          /* f14..f31 */
    uint32_t vrsave, pad[1];
    uint8_t vr[12][16];      /* v20..v31 */
    double fpscr;
} __attribute__((aligned(16)));

/* Offsets hard-coded in xthread_switch.S. */
_Static_assert(offsetof(struct xctx, gpr) == 32, "xctx gpr offset");
_Static_assert(offsetof(struct xctx, fpr) == 184, "xctx fpr offset");
_Static_assert(offsetof(struct xctx, vrsave) == 328, "xctx vrsave offset");
_Static_assert(offsetof(struct xctx, vr) == 336, "xctx vr offset");
_Static_assert(offsetof(struct xctx, fpscr) == 528, "xctx fpscr offset");

struct xthread {
    struct xctx ctx;
    xthread_t *prev, *next;
    xthread_entry_t entry;
    void *arg;
    void *ret;
    void *stack;
    uint64_t wake_us;
    int state;
    bool detached;
    bool joined;
    const char *name;
    void *user;              /* owned by a layer on top (e.g. pthreads) */
};

extern void xthread_switch(struct xctx *from, struct xctx *to);

static xthread_t main_thread;
static xthread_t *current;
static xthread_t *reap_pending;
static int thread_count;

static struct {
    void (*fn)(void);
    uint64_t interval_us;
    uint64_t last_us;
} pumps[XTHREAD_MAX_PUMPS];
static int pump_count;
static bool in_pump;

uint64_t xthread_now_us(void)
{
    return mftb() / (PPC_TIMEBASE_FREQ / 1000000);
}

void xthread_init(void)
{
    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.prev = main_thread.next = &main_thread;
    main_thread.state = XT_RUNNABLE;
    main_thread.name = "main";
    current = &main_thread;
    thread_count = 1;
}

void xthread_add_pump(void (*fn)(void), uint64_t min_interval_us)
{
    if (pump_count < XTHREAD_MAX_PUMPS) {
        pumps[pump_count].fn = fn;
        pumps[pump_count].interval_us = min_interval_us;
        pumps[pump_count].last_us = 0;
        pump_count++;
    }
}

static void run_pumps(void)
{
    uint64_t now;
    int i;

    /* lwIP callbacks may wake threads but must never re-enter the pump. */
    if (in_pump)
        return;
    in_pump = true;
    now = xthread_now_us();
    for (i = 0; i < pump_count; i++) {
        if (now - pumps[i].last_us >= pumps[i].interval_us) {
            pumps[i].last_us = now;
            pumps[i].fn();
        }
    }
    in_pump = false;
}

static void unlink_thread(xthread_t *t)
{
    t->prev->next = t->next;
    t->next->prev = t->prev;
    t->next = t->prev = t;
    thread_count--;
}

static void reap(void)
{
    xthread_t *t = reap_pending;

    if (t && t != current) {
        reap_pending = NULL;
        unlink_thread(t);
        free(t->stack);
        free(t);
    }
}

/* Picks the next thread whose sleep deadline has passed and switches to it. */
static void schedule(void)
{
    xthread_t *prev = current;
    xthread_t *t;
    uint64_t now;

    for (;;) {
        run_pumps();
        now = xthread_now_us();
        t = prev->next;
        do {
            if (t->state == XT_RUNNABLE && t->wake_us <= now)
                goto found;
            t = t->next;
        } while (t != prev->next);
        /* Every thread is sleeping: keep pumping until one wakes. */
    }

found:
    if (t != prev) {
        current = t;
        xthread_switch(&prev->ctx, &t->ctx);
        reap();
    }
}

void xthread_yield(void)
{
    current->wake_us = 0;
    schedule();
}

void xthread_sleep_us(uint64_t us)
{
    uint64_t deadline = xthread_now_us() + us;

    while (xthread_now_us() < deadline) {
        current->wake_us = deadline;
        schedule();
    }
    current->wake_us = 0;
}

static void xthread_trampoline(void)
{
    /* First run of a thread doesn't return through schedule(), so reap here too. */
    reap();
    xthread_exit(current->entry(current->arg));
}

xthread_t *xthread_create(const char *name, xthread_entry_t entry, void *arg, size_t stack_size)
{
    xthread_t *t;
    uint8_t *top;
    uint32_t r2, r13;

    if (stack_size == 0)
        stack_size = XTHREAD_DEFAULT_STACK;

    t = memalign(16, sizeof(*t));
    if (t == NULL)
        return NULL;
    memset(t, 0, sizeof(*t));

    t->stack = memalign(16, stack_size);
    if (t->stack == NULL) {
        free(t);
        return NULL;
    }

    t->entry = entry;
    t->arg = arg;
    t->name = name;
    t->state = XT_RUNNABLE;

    /* Fresh stack: 16-byte aligned with a zero back-chain word. */
    top = (uint8_t *)t->stack + stack_size - 64;
    top = (uint8_t *)((uintptr_t)top & ~(uintptr_t)15);
    memset(top, 0, 64);

    /* r2 and r13 are ABI-fixed (TLS / small-data anchors): inherit them. */
    asm volatile("mr %0, 2" : "=r"(r2));
    asm volatile("mr %0, 13" : "=r"(r13));

    t->ctx.r1 = (uint32_t)(uintptr_t)top;
    t->ctx.lr = (uint32_t)(uintptr_t)xthread_trampoline;
    t->ctx.r2 = r2;
    t->ctx.gpr[0] = r13;
    t->ctx.vrsave = 0xFFFFFFFF;
    t->ctx.fpscr = 0;

    /* Insert just before the current thread so it runs after everyone else. */
    t->next = current;
    t->prev = current->prev;
    current->prev->next = t;
    current->prev = t;
    thread_count++;

    return t;
}

xthread_t *xthread_self(void)
{
    return current;
}

void **xthread_user(xthread_t *t)
{
    return &t->user;
}

void xthread_exit(void *ret)
{
    xthread_t *self = current;

    self->ret = ret;
    self->state = XT_DONE;

    /* A detached thread can't free its own stack; the next thread to run does. */
    if (self->detached && self != &main_thread)
        reap_pending = self;

    /* schedule() never picks a finished thread, so this never returns. */
    for (;;)
        schedule();
}

int xthread_join(xthread_t *t, void **ret)
{
    if (t == NULL || t == current || t->detached)
        return -1;
    while (t->state != XT_DONE)
        xthread_yield();
    if (ret)
        *ret = t->ret;
    unlink_thread(t);
    free(t->stack);
    free(t);
    return 0;
}

int xthread_detach(xthread_t *t)
{
    if (t == NULL)
        return -1;
    if (t->state == XT_DONE && t != current && t != reap_pending) {
        unlink_thread(t);
        free(t->stack);
        free(t);
        return 0;
    }
    t->detached = true;
    return 0;
}

int xthread_count(void)
{
    return thread_count;
}
