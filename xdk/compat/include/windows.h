/*
 * <windows.h> for code written against desktop Windows, on the Xbox 360 SDK.
 * The SDK's <xtl.h> provides the Win32 subset the console has; this adds the
 * Vista-era synchronisation primitives (SRWLOCK, CONDITION_VARIABLE) that
 * moonlight-common-c uses, implemented in compat/win_sync.c.
 */
#ifndef XDK_WINDOWS_H
#define XDK_WINDOWS_H

#include <xtl.h>

/* Make moonlight-common-c take its "Windows app" path: it skips the desktop
 * WLAN API, which doesn't exist on the console. */
#ifndef WINAPI_FAMILY_APP
#define WINAPI_FAMILY_APP 2
#endif
#ifndef WINAPI_FAMILY
#define WINAPI_FAMILY WINAPI_FAMILY_APP
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    CRITICAL_SECTION cs;
    volatile LONG initialized;   /* 0 = not yet, 1 = in progress, 2 = ready */
} SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT { 0 }

typedef struct {
    HANDLE sema;                 /* waiters block here */
    volatile LONG waiters;
    volatile LONG initialized;
} CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT { 0 }

void WINAPI InitializeSRWLock(PSRWLOCK lock);
void WINAPI AcquireSRWLockExclusive(PSRWLOCK lock);
void WINAPI ReleaseSRWLockExclusive(PSRWLOCK lock);
void WINAPI InitializeConditionVariable(PCONDITION_VARIABLE cv);
BOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG flags);
void WINAPI WakeConditionVariable(PCONDITION_VARIABLE cv);
void WINAPI WakeAllConditionVariable(PCONDITION_VARIABLE cv);

/* Dynamic lookups (thread naming etc.) simply find nothing on the console. */
#define GetModuleHandleA(name) ((HMODULE)NULL)
#define GetProcAddress(mod, name) ((FARPROC)NULL)
#define IsDebuggerPresent() FALSE

#ifdef __cplusplus
}
#endif

#endif
