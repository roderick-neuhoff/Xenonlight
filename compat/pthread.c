/*
 * pthread shim over xthread. Because scheduling is cooperative, a mutex is
 * just an owner field and a condition variable is a sequence counter that
 * waiters poll while yielding. Spurious wakeups are allowed by POSIX.
 */
#include <errno.h>
#include <string.h>
#include "include/pthread.h"
#include "include/xenon_posix.h"

int xp_create(xp_thread_t *t, const xp_attr_t *attr, void *(*fn)(void *), void *arg)
{
    xthread_t *x = xthread_create("pthread", fn, arg, attr ? attr->stack_size : 0);

    if (x == NULL)
        return EAGAIN;
    *t = x;
    return 0;
}

int xp_join(xp_thread_t t, void **ret)
{
    return xthread_join(t, ret) == 0 ? 0 : EINVAL;
}

int xp_detach(xp_thread_t t)
{
    return xthread_detach(t) == 0 ? 0 : EINVAL;
}

xp_thread_t xp_self(void)
{
    return xthread_self();
}

int xp_attr_init(xp_attr_t *a)
{
    a->stack_size = 0;
    return 0;
}

int xp_attr_destroy(xp_attr_t *a)
{
    (void)a;
    return 0;
}

int xp_attr_setstacksize(xp_attr_t *a, size_t s)
{
    a->stack_size = s;
    return 0;
}

int xp_mutexattr_init(xp_mutexattr_t *a)
{
    a->recursive = 0;
    return 0;
}

int xp_mutexattr_destroy(xp_mutexattr_t *a)
{
    (void)a;
    return 0;
}

int xp_mutexattr_settype(xp_mutexattr_t *a, int type)
{
    a->recursive = (type == PTHREAD_MUTEX_RECURSIVE);
    return 0;
}

int xp_mutex_init(xp_mutex_t *m, const xp_mutexattr_t *a)
{
    m->owner = NULL;
    m->count = 0;
    m->recursive = a ? a->recursive : 0;
    return 0;
}

int xp_mutex_destroy(xp_mutex_t *m)
{
    return m->owner ? EBUSY : 0;
}

int xp_mutex_trylock(xp_mutex_t *m)
{
    xthread_t *self = xthread_self();

    if (m->owner == NULL) {
        m->owner = self;
        m->count = 1;
        return 0;
    }
    if (m->owner == self && m->recursive) {
        m->count++;
        return 0;
    }
    return EBUSY;
}

int xp_mutex_lock(xp_mutex_t *m)
{
    xthread_t *self = xthread_self();

    if (m->owner == self && !m->recursive)
        return EDEADLK;
    while (xp_mutex_trylock(m) != 0)
        xthread_yield();
    return 0;
}

int xp_mutex_unlock(xp_mutex_t *m)
{
    if (m->owner != xthread_self())
        return EPERM;
    if (--m->count == 0)
        m->owner = NULL;
    return 0;
}

int xp_cond_init(xp_cond_t *c, const xp_condattr_t *a)
{
    (void)a;
    c->seq = 0;
    return 0;
}

int xp_cond_destroy(xp_cond_t *c)
{
    (void)c;
    return 0;
}

int xp_cond_signal(xp_cond_t *c)
{
    c->seq++;
    return 0;
}

int xp_cond_broadcast(xp_cond_t *c)
{
    c->seq++;
    return 0;
}

int xp_cond_wait(xp_cond_t *c, xp_mutex_t *m)
{
    unsigned int seq = c->seq;

    xp_mutex_unlock(m);
    while (c->seq == seq)
        xthread_yield();
    return xp_mutex_lock(m);
}

int xp_cond_timedwait(xp_cond_t *c, xp_mutex_t *m, const struct timespec *abstime)
{
    unsigned int seq = c->seq;
    uint64_t deadline = (uint64_t)abstime->tv_sec * 1000000 + abstime->tv_nsec / 1000;
    int ret = 0;

    xp_mutex_unlock(m);
    while (c->seq == seq) {
        if (xthread_now_us() >= deadline) {
            ret = ETIMEDOUT;
            break;
        }
        xthread_yield();
    }
    xp_mutex_lock(m);
    return ret;
}
