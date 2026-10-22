/*
 * pthreads on Xenon hardware threads (FFmpeg's slice-decoding workers).
 * See include/pthread.h for the model and its limits.
 */
#include "include/pthread.h"

#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xenon_soc/xenon_power.h>

#define STACK_SIZE (1024 * 1024)

/*
 * Hardware threads FFmpeg may use, in order of preference. The app uses
 * 0 (network/Moonlight), 2 (decoder main) and 4 (renderer). Thread 5 shares
 * core 2 with the renderer, 3 shares core 1 with the decoder and 1 shares
 * core 0 with networking.
 */
static const int pool[] = { 3, 5, 1 };
#define POOL_SIZE ((int)(sizeof(pool) / sizeof(pool[0])))

struct hwp_thread {
    int hw;
    void *(*fn)(void *);
    void *arg;
    void *ret;
    volatile int done;
    uint8_t *stack;
};

static volatile int pool_busy[POOL_SIZE];
static struct hwp_thread *volatile running_on[6];
static struct hwp_thread self_threads[6];  /* pthread_self() for non-pthread threads */

static inline int current_hw(void)
{
    unsigned int pir;
    __asm__ volatile("mfspr %0, 1023" : "=r"(pir));
    return (int)(pir & 7);
}

/* Spin politely: lower SMT priority so the sibling thread keeps its speed. */
static inline void relax(void)
{
    __asm__ volatile("or 1,1,1\n\tnop\n\tnop\n\tnop\n\tor 2,2,2" ::: "memory");
}

static void trampoline(void)
{
    struct hwp_thread *t = running_on[current_hw()];

    t->ret = t->fn(t->arg);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    struct hwp_thread *t;
    int i;
    (void)attr;

    t = calloc(1, sizeof(*t));
    if (t == NULL)
        return EAGAIN;
    t->stack = memalign(128, STACK_SIZE);
    if (t->stack == NULL) {
        free(t);
        return EAGAIN;
    }
    t->fn = fn;
    t->arg = arg;

    for (i = 0; i < POOL_SIZE; i++) {
        if (__atomic_exchange_n(&pool_busy[i], 1, __ATOMIC_ACQUIRE) == 0)
            break;
    }
    if (i == POOL_SIZE || xenon_is_thread_task_running(pool[i])) {
        if (i < POOL_SIZE)
            __atomic_store_n(&pool_busy[i], 0, __ATOMIC_RELEASE);
        free(t->stack);
        free(t);
        return EAGAIN;  /* FFmpeg then decodes with fewer threads */
    }

    t->hw = pool[i];
    running_on[t->hw] = t;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (xenon_run_thread_task(t->hw, t->stack + STACK_SIZE - 256, (void *)trampoline) != 0) {
        running_on[t->hw] = NULL;
        __atomic_store_n(&pool_busy[i], 0, __ATOMIC_RELEASE);
        free(t->stack);
        free(t);
        return EAGAIN;
    }
    *out = t;
    return 0;
}

int pthread_join(pthread_t t, void **ret)
{
    int i;

    while (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE))
        relax();
    /* The task's wrapper clears its slot after we return; wait for that too. */
    while (xenon_is_thread_task_running(t->hw))
        relax();
    if (ret)
        *ret = t->ret;

    running_on[t->hw] = NULL;
    for (i = 0; i < POOL_SIZE; i++)
        if (pool[i] == t->hw)
            __atomic_store_n(&pool_busy[i], 0, __ATOMIC_RELEASE);
    free(t->stack);
    free(t);
    return 0;
}

pthread_t pthread_self(void)
{
    int hw = current_hw();

    if (running_on[hw])
        return running_on[hw];
    self_threads[hw].hw = hw;
    return &self_threads[hw];
}

int pthread_attr_init(pthread_attr_t *a) { a->stack_size = STACK_SIZE; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t s) { a->stack_size = s; return 0; }

int pthread_mutexattr_init(pthread_mutexattr_t *a) { a->type = PTHREAD_MUTEX_NORMAL; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) { a->type = type; return 0; }

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    m->locked = 0;
    m->recursive = a && a->type == PTHREAD_MUTEX_RECURSIVE;
    m->owner = -1;
    m->count = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    return m->locked ? EBUSY : 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    int hw = current_hw();

    if (m->recursive && m->owner == hw) {
        m->count++;
        return 0;
    }
    if (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE) != 0)
        return EBUSY;
    m->owner = hw;
    m->count = 1;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    while (pthread_mutex_trylock(m) != 0) {
        while (__atomic_load_n(&m->locked, __ATOMIC_RELAXED))
            relax();
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (--m->count > 0)
        return 0;
    m->owner = -1;
    __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    (void)a;
    c->seq = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c)
{
    /* Waking every waiter is allowed (spurious wakeups are legal). */
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    /* Read the sequence while holding the mutex so no signal is lost. */
    unsigned int seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);

    pthread_mutex_unlock(m);
    while (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == seq)
        relax();
    return pthread_mutex_lock(m);
}

static long long now_us(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000 + tv.tv_usec;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    unsigned int seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    long long deadline = (long long)abstime->tv_sec * 1000000 + abstime->tv_nsec / 1000;
    int ret = 0;

    pthread_mutex_unlock(m);
    while (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == seq) {
        if (now_us() >= deadline) {
            ret = ETIMEDOUT;
            break;
        }
        relax();
    }
    pthread_mutex_lock(m);
    return ret;
}

int pthread_once(pthread_once_t *once, void (*fn)(void))
{
    int expected = 0;

    if (__atomic_compare_exchange_n(&once->state, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        fn();
        __atomic_store_n(&once->state, 2, __ATOMIC_RELEASE);
        return 0;
    }
    while (__atomic_load_n(&once->state, __ATOMIC_ACQUIRE) != 2)
        relax();
    return 0;
}
