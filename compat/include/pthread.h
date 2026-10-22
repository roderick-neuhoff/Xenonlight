/*
 * Minimal pthreads on top of xthread (cooperative, hardware thread 0 only).
 * Names are mapped with macros so they never clash with whatever newlib
 * may or may not declare.
 */
#ifndef XENON_COMPAT_PTHREAD_H
#define XENON_COMPAT_PTHREAD_H

#include <stddef.h>
#include <time.h>
#include "../xthread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef xthread_t *xp_thread_t;

typedef struct {
    size_t stack_size;
} xp_attr_t;

typedef struct {
    xthread_t *owner;
    int count;
    int recursive;
} xp_mutex_t;

typedef struct {
    int recursive;
} xp_mutexattr_t;

typedef struct {
    unsigned int seq;
} xp_cond_t;

typedef int xp_condattr_t;

#define XP_MUTEX_INITIALIZER { 0, 0, 0 }
#define XP_COND_INITIALIZER  { 0 }

int xp_create(xp_thread_t *t, const xp_attr_t *attr, void *(*fn)(void *), void *arg);
int xp_join(xp_thread_t t, void **ret);
int xp_detach(xp_thread_t t);
xp_thread_t xp_self(void);
int xp_attr_init(xp_attr_t *a);
int xp_attr_destroy(xp_attr_t *a);
int xp_attr_setstacksize(xp_attr_t *a, size_t s);

int xp_mutexattr_init(xp_mutexattr_t *a);
int xp_mutexattr_destroy(xp_mutexattr_t *a);
int xp_mutexattr_settype(xp_mutexattr_t *a, int type);
int xp_mutex_init(xp_mutex_t *m, const xp_mutexattr_t *a);
int xp_mutex_destroy(xp_mutex_t *m);
int xp_mutex_lock(xp_mutex_t *m);
int xp_mutex_trylock(xp_mutex_t *m);
int xp_mutex_unlock(xp_mutex_t *m);

int xp_cond_init(xp_cond_t *c, const xp_condattr_t *a);
int xp_cond_destroy(xp_cond_t *c);
int xp_cond_signal(xp_cond_t *c);
int xp_cond_broadcast(xp_cond_t *c);
int xp_cond_wait(xp_cond_t *c, xp_mutex_t *m);
int xp_cond_timedwait(xp_cond_t *c, xp_mutex_t *m, const struct timespec *abstime);

#define pthread_t               xp_thread_t
#define pthread_attr_t          xp_attr_t
#define pthread_mutex_t         xp_mutex_t
#define pthread_mutexattr_t     xp_mutexattr_t
#define pthread_cond_t          xp_cond_t
#define pthread_condattr_t      xp_condattr_t

#define PTHREAD_MUTEX_INITIALIZER XP_MUTEX_INITIALIZER
#define PTHREAD_COND_INITIALIZER  XP_COND_INITIALIZER
#define PTHREAD_MUTEX_NORMAL      0
#define PTHREAD_MUTEX_RECURSIVE   1
#define PTHREAD_MUTEX_DEFAULT     0

#define pthread_create          xp_create
#define pthread_join            xp_join
#define pthread_detach          xp_detach
#define pthread_self            xp_self
#define pthread_attr_init       xp_attr_init
#define pthread_attr_destroy    xp_attr_destroy
#define pthread_attr_setstacksize xp_attr_setstacksize
#define pthread_mutexattr_init  xp_mutexattr_init
#define pthread_mutexattr_destroy xp_mutexattr_destroy
#define pthread_mutexattr_settype xp_mutexattr_settype
#define pthread_mutex_init      xp_mutex_init
#define pthread_mutex_destroy   xp_mutex_destroy
#define pthread_mutex_lock      xp_mutex_lock
#define pthread_mutex_trylock   xp_mutex_trylock
#define pthread_mutex_unlock    xp_mutex_unlock
#define pthread_cond_init       xp_cond_init
#define pthread_cond_destroy    xp_cond_destroy
#define pthread_cond_signal     xp_cond_signal
#define pthread_cond_broadcast  xp_cond_broadcast
#define pthread_cond_wait       xp_cond_wait
#define pthread_cond_timedwait  xp_cond_timedwait

#ifdef __cplusplus
}
#endif

#endif
