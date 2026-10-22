/*
 * pthreads on Xenon hardware threads, for FFmpeg only.
 *
 * Each pthread_create() claims a free hardware thread from a fixed pool
 * (see hwpthread.c) and runs to completion there; there is no preemption or
 * time slicing. Mutexes and condition variables spin with lowered SMT
 * priority, so they are only suitable for short waits between cores.
 *
 * The app itself uses compat/include/pthread.h (cooperative coroutines on
 * hardware thread 0) whose functions have different symbol names, so the two
 * implementations can be linked into the same binary.
 */
#ifndef HWPTHREAD_H
#define HWPTHREAD_H

#include <stddef.h>
#include <sys/types.h>   /* newlib typedefs pthread_* itself; ours are renamed below */
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Map every pthread name onto hwp_* so newlib's own typedefs don't clash. */
#define pthread_t                   hwp_pthread_t
#define pthread_attr_t              hwp_pthread_attr_t
#define pthread_mutex_t             hwp_pthread_mutex_t
#define pthread_mutexattr_t         hwp_pthread_mutexattr_t
#define pthread_cond_t              hwp_pthread_cond_t
#define pthread_condattr_t          hwp_pthread_condattr_t
#define pthread_once_t              hwp_pthread_once_t
#define pthread_create              hwp_pthread_create
#define pthread_join                hwp_pthread_join
#define pthread_self                hwp_pthread_self
#define pthread_attr_init           hwp_pthread_attr_init
#define pthread_attr_destroy        hwp_pthread_attr_destroy
#define pthread_attr_setstacksize   hwp_pthread_attr_setstacksize
#define pthread_mutexattr_init      hwp_pthread_mutexattr_init
#define pthread_mutexattr_destroy   hwp_pthread_mutexattr_destroy
#define pthread_mutexattr_settype   hwp_pthread_mutexattr_settype
#define pthread_mutex_init          hwp_pthread_mutex_init
#define pthread_mutex_destroy       hwp_pthread_mutex_destroy
#define pthread_mutex_lock          hwp_pthread_mutex_lock
#define pthread_mutex_trylock       hwp_pthread_mutex_trylock
#define pthread_mutex_unlock        hwp_pthread_mutex_unlock
#define pthread_cond_init           hwp_pthread_cond_init
#define pthread_cond_destroy        hwp_pthread_cond_destroy
#define pthread_cond_signal         hwp_pthread_cond_signal
#define pthread_cond_broadcast      hwp_pthread_cond_broadcast
#define pthread_cond_wait           hwp_pthread_cond_wait
#define pthread_cond_timedwait      hwp_pthread_cond_timedwait
#define pthread_once                hwp_pthread_once

#undef PTHREAD_MUTEX_INITIALIZER
#undef PTHREAD_COND_INITIALIZER
#undef PTHREAD_ONCE_INIT
#undef PTHREAD_MUTEX_NORMAL
#undef PTHREAD_MUTEX_RECURSIVE
#undef PTHREAD_MUTEX_DEFAULT

typedef struct hwp_thread *pthread_t;

typedef struct {
    size_t stack_size;
} pthread_attr_t;

typedef struct {
    volatile unsigned int locked;
    int recursive;
    volatile int owner;     /* hardware thread id, for recursive mutexes */
    int count;
} pthread_mutex_t;

typedef struct {
    int type;
} pthread_mutexattr_t;

typedef struct {
    volatile unsigned int seq;
} pthread_cond_t;

typedef int pthread_condattr_t;

typedef struct {
    volatile int state;
} pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, 0, -1, 0 }
#define PTHREAD_COND_INITIALIZER  { 0 }
#define PTHREAD_ONCE_INIT         { 0 }
#define PTHREAD_MUTEX_NORMAL      0
#define PTHREAD_MUTEX_RECURSIVE   1
#define PTHREAD_MUTEX_DEFAULT     0

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
pthread_t pthread_self(void);
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t s);

int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime);

int pthread_once(pthread_once_t *once, void (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif
