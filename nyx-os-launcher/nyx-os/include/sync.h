// include/sync.h  –  Nyx OS synchronization primitives
//
// Provides kernel-level synchronization primitives for multi-threaded
// Windows applications running via the PE loader:
//   - Mutex: mutual exclusion lock (recursive supported)
//   - Semaphore: counting semaphore with wait/release
//   - Critical Section: fast same-process mutex (user-mode spinning + fallback)
//   - Event: signal/wait synchronization object
//
// These map to the Windows API functions:
//   CreateMutex, ReleaseMutex, WaitForSingleObject
//   CreateSemaphore, ReleaseSemaphore
//   InitializeCriticalSection, EnterCriticalSection, LeaveCriticalSection
//   CreateEvent, SetEvent, ResetEvent, WaitForSingleObject

#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>

#define SYNC_MAX_MUTEXES     32
#define SYNC_MAX_SEMAPHORES  32
#define SYNC_MAX_EVENTS      32
#define SYNC_MAX_CRIT_SECTIONS 32

#define SYNC_WAIT_OBJECT_0    0
#define SYNC_WAIT_TIMEOUT     0x102
#define SYNC_WAIT_FAILED      0xFFFFFFFF
#define SYNC_INFINITE         0xFFFFFFFF

#define EVENT_MANUAL_RESET  1
#define EVENT_INITIAL_STATE 2

// ─── Mutex ──────────────────────────────────────────────────────
typedef struct {
    int       used;
    int       owner;          // task id that owns the mutex, -1 = free
    int       count;          // recursion count (same owner)
    uint32_t  spin_count;     // spins before blocking
} mutex_t;

// Create or open a named mutex.  Returns handle (1-based index) or 0 on error.
int  mutex_create(const char* name);
// Release a mutex.  Returns 1 on success, 0 on error.
int  mutex_release(int handle);
// Destroy a mutex.
void mutex_destroy(int handle);

// ─── Semaphore ──────────────────────────────────────────────────
typedef struct {
    int       used;
    int32_t   count;          // current count
    int32_t   max_count;      // maximum count
    int32_t   waiters;        // tasks waiting to decrement
} semaphore_t;

// Create a semaphore with initial and max count.  Returns handle or 0.
int  semaphore_create(int32_t initial_count, int32_t max_count);
// Release (increment) semaphore.  Returns 1 on success.
int  semaphore_release(int handle, int32_t release_count, int32_t* prev_count);
// Destroy a semaphore.
void semaphore_destroy(int handle);

// ─── Critical Section ───────────────────────────────────────────
// Fast mutex for same-process threads: uses spin count before blocking.
typedef struct {
    int       used;
    int       owner;          // task id that owns, -1 = free
    int       count;          // recursion count
    uint32_t  spin_count;     // spins before yielding
} critical_section_t;

// Initialize a critical section.  Returns handle or 0.
int  critical_section_init(void);
// Enter (acquire) a critical section.
void critical_section_enter(int handle);
// Try to enter without blocking.  Returns 1 if acquired.
int  critical_section_try_enter(int handle);
// Leave (release) a critical section.
void critical_section_leave(int handle);
// Delete a critical section.
void critical_section_delete(int handle);

// ─── Event ──────────────────────────────────────────────────────
typedef struct {
    int       used;
    int       signaled;       // 1 = signaled, 0 = non-signaled
    int       manual_reset;   // 1 = manual reset, 0 = auto-reset
    int       waiters;        // tasks waiting on this event
} event_t;

// Create an event.  Returns handle or 0.
int  event_create(int manual_reset, int initial_state);
// Set (signal) an event, waking all waiters.
int  event_set(int handle);
// Reset (clear) an event.
int  event_reset(int handle);
// Destroy an event.
void event_destroy(int handle);

// ─── Generic Wait ───────────────────────────────────────────────
// Wait on any object (mutex, semaphore, event, critical section).
// Returns WAIT_OBJECT_0 on success, WAIT_TIMEOUT on timeout.
// timeout_ms: milliseconds to wait, INFINITE for forever.
uint32_t wait_for_single_object(int handle, uint32_t timeout_ms, int obj_type);

#endif
