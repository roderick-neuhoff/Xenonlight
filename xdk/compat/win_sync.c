/*
 * SRWLOCK / CONDITION_VARIABLE (Windows Vista+) for the Xbox 360, whose
 * kernel predates them. Exclusive-mode locks map to critical sections;
 * condition variables are the classic semaphore + waiter-count scheme.
 * Spurious wakeups are possible, which callers must handle anyway.
 */
#include <windows.h>

static void ensure_init(volatile LONG *state, void (*init)(void *), void *obj)
{
    /* 0 = uninitialised, 1 = initialising, 2 = ready. */
    if (*state == 2)
        return;
    if (InterlockedCompareExchange(state, 1, 0) == 0) {
        init(obj);
        InterlockedExchange(state, 2);
    } else {
        while (*state != 2)
            Sleep(0);
    }
}

static void srw_init(void *p)
{
    InitializeCriticalSection(&((PSRWLOCK)p)->cs);
}

static void cv_init(void *p)
{
    PCONDITION_VARIABLE cv = (PCONDITION_VARIABLE)p;
    cv->sema = CreateSemaphore(NULL, 0, 0x7FFFFFFF, NULL);
    cv->waiters = 0;
}

void WINAPI InitializeSRWLock(PSRWLOCK lock)
{
    lock->initialized = 0;
    ensure_init(&lock->initialized, srw_init, lock);
}

void WINAPI AcquireSRWLockExclusive(PSRWLOCK lock)
{
    ensure_init(&lock->initialized, srw_init, lock);
    EnterCriticalSection(&lock->cs);
}

void WINAPI ReleaseSRWLockExclusive(PSRWLOCK lock)
{
    LeaveCriticalSection(&lock->cs);
}

void WINAPI InitializeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv->initialized = 0;
    ensure_init(&cv->initialized, cv_init, cv);
}

BOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG flags)
{
    DWORD r;
    (void)flags;

    ensure_init(&cv->initialized, cv_init, cv);
    InterlockedIncrement(&cv->waiters);
    ReleaseSRWLockExclusive(lock);
    r = WaitForSingleObject(cv->sema, ms);
    AcquireSRWLockExclusive(lock);
    if (r != WAIT_OBJECT_0) {
        /* Timed out: withdraw our wait (a racing wake leaves one spare count). */
        if (InterlockedDecrement(&cv->waiters) < 0)
            InterlockedIncrement(&cv->waiters);
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

void WINAPI WakeConditionVariable(PCONDITION_VARIABLE cv)
{
    ensure_init(&cv->initialized, cv_init, cv);
    if (cv->waiters > 0 && InterlockedDecrement(&cv->waiters) >= 0)
        ReleaseSemaphore(cv->sema, 1, NULL);
    else if (cv->waiters < 0)
        InterlockedIncrement(&cv->waiters);
}

void WINAPI WakeAllConditionVariable(PCONDITION_VARIABLE cv)
{
    LONG n;

    ensure_init(&cv->initialized, cv_init, cv);
    n = InterlockedExchange(&cv->waiters, 0);
    if (n > 0)
        ReleaseSemaphore(cv->sema, n, NULL);
}
