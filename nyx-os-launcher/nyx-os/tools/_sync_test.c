// tools/_sync_test.c  –  Host-side test for Nyx OS synchronization primitives.
//
// Tests mutex, semaphore, critical section, and event implementations
// to verify they work correctly before running in the kernel.
//
// Build: gcc -O2 -o /tmp/sync_test tools/_sync_test.c
// Run:   /tmp/sync_test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ─── Types ──────────────────────────────────────────────────────
typedef int BOOL;
typedef unsigned long DWORD;
typedef long LONG;
typedef void* HANDLE;

#define TRUE  1
#define FALSE 0

#define WAIT_OBJECT_0    0
#define WAIT_TIMEOUT     0x102
#define WAIT_FAILED      0xFFFFFFFF
#define INFINITE         0xFFFFFFFF

#define SYNC_MAX_MUTEXES     32
#define SYNC_MAX_SEMAPHORES  32
#define SYNC_MAX_EVENTS      32
#define SYNC_MAX_CRIT_SECTIONS 32

// ─── Structures ─────────────────────────────────────────────────

typedef struct {
    int used;
    int owner;
    int count;
    uint32_t spin_count;
} mutex_t;

typedef struct {
    int used;
    int32_t count;
    int32_t max_count;
    int32_t waiters;
} semaphore_t;

typedef struct {
    int used;
    int owner;
    int count;
    uint32_t spin_count;
} critical_section_t;

typedef struct {
    int used;
    int signaled;
    int manual_reset;
    int waiters;
} event_t;

// ─── Global state ───────────────────────────────────────────────
static int current_task_id = 0;
static mutex_t mutexes[SYNC_MAX_MUTEXES];
static semaphore_t semaphores[SYNC_MAX_SEMAPHORES];
static critical_section_t crit_sections[SYNC_MAX_CRIT_SECTIONS];
static event_t events[SYNC_MAX_EVENTS];

// ─── Stub functions ─────────────────────────────────────────────
int task_current_id(void) { return current_task_id; }
void task_yield(void) { /* no-op in test */ }
uint32_t timer_ms(void) { return 0; }

// ─── Mutex implementation ───────────────────────────────────────
int mutex_create(const char* name) {
    (void)name;
    for (int i = 0; i < SYNC_MAX_MUTEXES; i++) {
        if (!mutexes[i].used) {
            mutexes[i].used = 1;
            mutexes[i].owner = -1;
            mutexes[i].count = 0;
            mutexes[i].spin_count = 100;
            return i + 1;
        }
    }
    return 0;
}

int mutex_release(int handle) {
    if (handle < 1 || handle > SYNC_MAX_MUTEXES) return 0;
    mutex_t* m = &mutexes[handle - 1];
    if (!m->used) return 0;
    int cur = task_current_id();
    if (m->owner != cur) return 0;
    m->count--;
    if (m->count == 0) m->owner = -1;
    return 1;
}

void mutex_destroy(int handle) {
    if (handle < 1 || handle > SYNC_MAX_MUTEXES) return;
    mutexes[handle - 1].used = 0;
}

// ─── Semaphore implementation ───────────────────────────────────
int semaphore_create(int32_t initial_count, int32_t max_count) {
    if (initial_count < 0 || max_count < 1 || initial_count > max_count) return 0;
    for (int i = 0; i < SYNC_MAX_SEMAPHORES; i++) {
        if (!semaphores[i].used) {
            semaphores[i].used = 1;
            semaphores[i].count = initial_count;
            semaphores[i].max_count = max_count;
            semaphores[i].waiters = 0;
            return i + 1;
        }
    }
    return 0;
}

int semaphore_release(int handle, int32_t release_count, int32_t* prev_count) {
    if (handle < 1 || handle > SYNC_MAX_SEMAPHORES) return 0;
    semaphore_t* s = &semaphores[handle - 1];
    if (!s->used || release_count < 1) return 0;
    if (prev_count) *prev_count = s->count;
    s->count += release_count;
    if (s->count > s->max_count) s->count = s->max_count;
    return 1;
}

void semaphore_destroy(int handle) {
    if (handle < 1 || handle > SYNC_MAX_SEMAPHORES) return;
    semaphores[handle - 1].used = 0;
}

// ─── Critical Section implementation ────────────────────────────
int critical_section_init(void) {
    for (int i = 0; i < SYNC_MAX_CRIT_SECTIONS; i++) {
        if (!crit_sections[i].used) {
            crit_sections[i].used = 1;
            crit_sections[i].owner = -1;
            crit_sections[i].count = 0;
            crit_sections[i].spin_count = 4000;
            return i + 1;
        }
    }
    return 0;
}

void critical_section_enter(int handle) {
    if (handle < 1 || handle > SYNC_MAX_CRIT_SECTIONS) return;
    critical_section_t* cs = &crit_sections[handle - 1];
    int cur = task_current_id();
    if (cs->owner == cur) { cs->count++; return; }
    uint32_t spins = 0;
    while (cs->owner != -1) {
        if (spins++ > cs->spin_count) { task_yield(); spins = 0; }
    }
    cs->owner = cur;
    cs->count = 1;
}

int critical_section_try_enter(int handle) {
    if (handle < 1 || handle > SYNC_MAX_CRIT_SECTIONS) return 0;
    critical_section_t* cs = &crit_sections[handle - 1];
    int cur = task_current_id();
    if (cs->owner == cur) { cs->count++; return 1; }
    if (cs->owner == -1) { cs->owner = cur; cs->count = 1; return 1; }
    return 0;
}

void critical_section_leave(int handle) {
    if (handle < 1 || handle > SYNC_MAX_CRIT_SECTIONS) return;
    critical_section_t* cs = &crit_sections[handle - 1];
    if (cs->owner != task_current_id()) return;
    cs->count--;
    if (cs->count == 0) cs->owner = -1;
}

void critical_section_delete(int handle) {
    if (handle < 1 || handle > SYNC_MAX_CRIT_SECTIONS) return;
    crit_sections[handle - 1].used = 0;
}

// ─── Event implementation ───────────────────────────────────────
int event_create(int manual_reset, int initial_state) {
    for (int i = 0; i < SYNC_MAX_EVENTS; i++) {
        if (!events[i].used) {
            events[i].used = 1;
            events[i].signaled = initial_state ? 1 : 0;
            events[i].manual_reset = manual_reset ? 1 : 0;
            events[i].waiters = 0;
            return i + 1;
        }
    }
    return 0;
}

int event_set(int handle) {
    if (handle < 1 || handle > SYNC_MAX_EVENTS) return 0;
    events[handle - 1].signaled = 1;
    return 1;
}

int event_reset(int handle) {
    if (handle < 1 || handle > SYNC_MAX_EVENTS) return 0;
    events[handle - 1].signaled = 0;
    return 1;
}

void event_destroy(int handle) {
    if (handle < 1 || handle > SYNC_MAX_EVENTS) return;
    events[handle - 1].used = 0;
}

// ─── Wait implementation ────────────────────────────────────────
uint32_t wait_for_single_object(int handle, uint32_t timeout_ms, int obj_type) {
    // Simplified: just check if we can acquire immediately
    switch (obj_type) {
        case 1: { // Mutex
            if (handle < 1 || handle > SYNC_MAX_MUTEXES) return WAIT_FAILED;
            mutex_t* m = &mutexes[handle - 1];
            if (!m->used) return WAIT_FAILED;
            int cur = task_current_id();
            if (m->owner == -1) { m->owner = cur; m->count = 1; return WAIT_OBJECT_0; }
            if (m->owner == cur) { m->count++; return WAIT_OBJECT_0; }
            return (timeout_ms == 0) ? WAIT_TIMEOUT : WAIT_OBJECT_0;
        }
        case 2: { // Semaphore
            if (handle < 1 || handle > SYNC_MAX_SEMAPHORES) return WAIT_FAILED;
            semaphore_t* s = &semaphores[handle - 1];
            if (!s->used) return WAIT_FAILED;
            if (s->count > 0) { s->count--; return WAIT_OBJECT_0; }
            return (timeout_ms == 0) ? WAIT_TIMEOUT : WAIT_OBJECT_0;
        }
        case 3: { // Event
            if (handle < 1 || handle > SYNC_MAX_EVENTS) return WAIT_FAILED;
            event_t* e = &events[handle - 1];
            if (!e->used) return WAIT_FAILED;
            if (e->signaled) {
                if (!e->manual_reset) e->signaled = 0;
                return WAIT_OBJECT_0;
            }
            return (timeout_ms == 0) ? WAIT_TIMEOUT : WAIT_OBJECT_0;
        }
        case 4: { // Critical Section
            if (handle < 1 || handle > SYNC_MAX_CRIT_SECTIONS) return WAIT_FAILED;
            critical_section_t* cs = &crit_sections[handle - 1];
            if (!cs->used) return WAIT_FAILED;
            int cur = task_current_id();
            if (cs->owner == -1) { cs->owner = cur; cs->count = 1; return WAIT_OBJECT_0; }
            if (cs->owner == cur) { cs->count++; return WAIT_OBJECT_0; }
            return (timeout_ms == 0) ? WAIT_TIMEOUT : WAIT_OBJECT_0;
        }
    }
    return WAIT_FAILED;
}

// ─── Test macros ────────────────────────────────────────────────
#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 0; \
    } \
} while(0)
#define PASS() do { printf("  PASS\n"); return 1; } while(0)

// ─── Mutex Tests ────────────────────────────────────────────────

TEST(mutex_create_release) {
    printf("  Testing mutex create/release...\n");
    int h = mutex_create("test_mutex");
    ASSERT(h > 0);
    ASSERT(mutex_release(h) == 0);  // not owner
    ASSERT(mutex_create("test_mutex2") > 0);
    mutex_destroy(h);
    PASS();
}

TEST(mutex_acquire_release) {
    printf("  Testing mutex acquire/release...\n");
    current_task_id = 1;
    int h = mutex_create("test_mutex");
    ASSERT(h > 0);

    // Acquire
    uint32_t result = wait_for_single_object(h, INFINITE, 1);
    ASSERT(result == WAIT_OBJECT_0);
    ASSERT(mutex_release(h) == 1);

    // Recursive acquire
    result = wait_for_single_object(h, INFINITE, 1);
    ASSERT(result == WAIT_OBJECT_0);
    result = wait_for_single_object(h, INFINITE, 1);
    ASSERT(result == WAIT_OBJECT_0);
    ASSERT(mutex_release(h) == 1);
    ASSERT(mutex_release(h) == 1);

    mutex_destroy(h);
    PASS();
}

TEST(mutex_owner_check) {
    printf("  Testing mutex owner check...\n");
    current_task_id = 1;
    int h = mutex_create("test_mutex");
    wait_for_single_object(h, INFINITE, 1);

    // Different task tries to release
    current_task_id = 2;
    ASSERT(mutex_release(h) == 0);

    // Original owner releases
    current_task_id = 1;
    ASSERT(mutex_release(h) == 1);

    mutex_destroy(h);
    PASS();
}

// ─── Semaphore Tests ────────────────────────────────────────────

TEST(semaphore_basic) {
    printf("  Testing semaphore basic operations...\n");
    int h = semaphore_create(2, 5);
    ASSERT(h > 0);

    int32_t prev;
    ASSERT(semaphore_release(h, 1, &prev) == 1);
    ASSERT(prev == 2);
    ASSERT(semaphore_release(h, 10, NULL) == 1);  // capped at max

    semaphore_destroy(h);
    PASS();
}

TEST(semaphore_wait) {
    printf("  Testing semaphore wait...\n");
    current_task_id = 1;
    int h = semaphore_create(1, 1);

    // Wait should succeed (count = 1)
    uint32_t result = wait_for_single_object(h, INFINITE, 2);
    ASSERT(result == WAIT_OBJECT_0);

    // Wait should fail (count = 0)
    result = wait_for_single_object(h, 0, 2);
    ASSERT(result == WAIT_TIMEOUT);

    // Release and wait again
    ASSERT(semaphore_release(h, 1, NULL) == 1);
    result = wait_for_single_object(h, INFINITE, 2);
    ASSERT(result == WAIT_OBJECT_0);

    semaphore_destroy(h);
    PASS();
}

// ─── Critical Section Tests ─────────────────────────────────────

TEST(critical_section_basic) {
    printf("  Testing critical section basic...\n");
    current_task_id = 1;
    int h = critical_section_init();
    ASSERT(h > 0);

    critical_section_enter(h);
    ASSERT(critical_section_try_enter(h) == 1);

    critical_section_leave(h);
    ASSERT(critical_section_try_enter(h) == 1);

    critical_section_leave(h);
    critical_section_leave(h);

    critical_section_delete(h);
    PASS();
}

TEST(critical_section_contention) {
    printf("  Testing critical section contention...\n");
    current_task_id = 1;
    int h = critical_section_init();
    critical_section_enter(h);

    // Different task tries to enter
    current_task_id = 2;
    ASSERT(critical_section_try_enter(h) == 0);

    // Original task leaves
    current_task_id = 1;
    critical_section_leave(h);

    // Now different task can enter
    current_task_id = 2;
    ASSERT(critical_section_try_enter(h) == 1);
    critical_section_leave(h);

    critical_section_delete(h);
    PASS();
}

// ─── Event Tests ────────────────────────────────────────────────

TEST(event_basic) {
    printf("  Testing event basic...\n");
    int h = event_create(1, 0);
    ASSERT(h > 0);

    ASSERT(event_set(h) == 1);
    ASSERT(event_reset(h) == 1);

    event_destroy(h);
    PASS();
}

TEST(event_wait) {
    printf("  Testing event wait...\n");
    current_task_id = 1;
    int h = event_create(1, 0);

    // Wait should timeout
    uint32_t result = wait_for_single_object(h, 0, 3);
    ASSERT(result == WAIT_TIMEOUT);

    // Set and wait
    event_set(h);
    result = wait_for_single_object(h, INFINITE, 3);
    ASSERT(result == WAIT_OBJECT_0);

    // Auto-reset event
    int h2 = event_create(0, 1);
    result = wait_for_single_object(h2, INFINITE, 3);
    ASSERT(result == WAIT_OBJECT_0);
    result = wait_for_single_object(h2, 0, 3);
    ASSERT(result == WAIT_TIMEOUT);

    event_destroy(h);
    event_destroy(h2);
    PASS();
}

// ─── Main ───────────────────────────────────────────────────────

int main(void) {
    printf("Nyx OS Synchronization Primitives Test\n");
    printf("=======================================\n\n");

    int passed = 0, failed = 0;

    if (test_mutex_create_release()) passed++; else failed++;
    if (test_mutex_acquire_release()) passed++; else failed++;
    if (test_mutex_owner_check()) passed++; else failed++;

    if (test_semaphore_basic()) passed++; else failed++;
    if (test_semaphore_wait()) passed++; else failed++;

    if (test_critical_section_basic()) passed++; else failed++;
    if (test_critical_section_contention()) passed++; else failed++;

    if (test_event_basic()) passed++; else failed++;
    if (test_event_wait()) passed++; else failed++;

    printf("\n=======================================\n");
    printf("Results: %d passed, %d failed\n", passed, failed);

    return (failed == 0) ? 0 : 1;
}
