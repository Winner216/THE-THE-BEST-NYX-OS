// cpu/sync.c  –  Nyx OS synchronization primitives implementation
//
// Provides kernel-level synchronization for multi-threaded Windows apps.
// All primitives use busy-waiting with yields for simplicity; a production
// OS would use proper wait queues.  The current scheduler supports
// SCHED_PRIORITY, so higher-priority threads get CPU first.
#include <sync.h>
#include <task.h>
#include <io.h>
#include <timer.h>
#include <stdint.h>
#include <stddef.h>

// ─── Mutex ──────────────────────────────────────────────────────
static mutex_t mutexes[SYNC_MAX_MUTEXES];

int mutex_create(const char* name) {
    (void)name;
    for (int i = 0; i < SYNC_MAX_MUTEXES; i++) {
        if (!mutexes[i].used) {
            mutexes[i].used = 1;
            mutexes[i].owner = -1;
            mutexes[i].count = 0;
            mutexes[i].spin_count = 100;
            return i + 1;   // 1-based handle
        }
    }
    return 0;
}

int mutex_release(int handle) {
    if (handle < 1 || handle > SYNC_MAX_MUTEXES) return 0;
    mutex_t* m = &mutexes[handle - 1];
    if (!m->used) return 0;
    int cur = task_current_id();
    if (m->owner != cur) return 0;  // not the owner
    m->count--;
    if (m->count == 0) {
        m->owner = -1;
    }
    return 1;
}

void mutex_destroy(int handle) {
    if (handle < 1 || handle > SYNC_MAX_MUTEXES) return;
    mutexes[handle - 1].used = 0;
}

// ─── Semaphore ──────────────────────────────────────────────────
static semaphore_t semaphores[SYNC_MAX_SEMAPHORES];

int semaphore_create(int32_t initial_count, int32_t max_count) {
    if (initial_count < 0 || max_count < 1 || initial_count > max_count)
        return 0;
    for (int i = 0; i < SYNC_MAX_SEMAPHORES; i++) {
        if (!semaphores[i].used) {
            semaphores[i].used = 1;
            semaphores[i].count = initial_count;
            semaphores[i].max_count = max_count;
            semaphores[i].waiters = 0;
            return i + 1;   // 1-based handle
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

// ─── Critical Section ───────────────────────────────────────────
static critical_section_t crit_sections[SYNC_MAX_CRIT_SECTIONS];

int critical_section_init(void) {
    for (int i = 0; i < SYNC_MAX_CRIT_SECTIONS; i++) {
        if (!crit_sections[i].used) {
            crit_sections[i].used = 1;
            crit_sections[i].owner = -1;
            crit_sections[i].count = 0;
            crit_sections[i].spin_count = 4000;
            return i + 1;   // 1-based handle
        }
    }
    return 0;
}

void critical_section_enter(int handle) {
    if (handle < 1 || handle > SYNC_MAX_CRIT_SECTIONS) return;
    critical_section_t* cs = &crit_sections[handle - 1];
    int cur = task_current_id();
    // Recursive: already own it
    if (cs->owner == cur) {
        cs->count++;
        return;
    }
    // Spin for a while, then yield
    uint32_t spins = 0;
    while (cs->owner != -1) {
        if (spins++ > cs->spin_count) {
            task_yield();
            spins = 0;
        }
    }
    cs->owner = cur;
    cs->count = 1;
}

int critical_section_try_enter(int handle) {
    if (handle < 1 || handle > SYNC_MAX_CRIT_SECTIONS) return 0;
    critical_section_t* cs = &crit_sections[handle - 1];
    int cur = task_current_id();
    if (cs->owner == cur) { cs->count++; return 1; }
    if (cs->owner == -1) {
        cs->owner = cur;
        cs->count = 1;
        return 1;
    }
    return 0;
}

void critical_section_leave(int handle) {
    if (handle < 1 || handle > SYNC_MAX_CRIT_SECTIONS) return;
    critical_section_t* cs = &crit_sections[handle - 1];
    if (cs->owner != task_current_id()) return;
    cs->count--;
    if (cs->count == 0) {
        cs->owner = -1;
    }
}

void critical_section_delete(int handle) {
    if (handle < 1 || handle > SYNC_MAX_CRIT_SECTIONS) return;
    crit_sections[handle - 1].used = 0;
}

// ─── Event ──────────────────────────────────────────────────────
static event_t events[SYNC_MAX_EVENTS];

int event_create(int manual_reset, int initial_state) {
    for (int i = 0; i < SYNC_MAX_EVENTS; i++) {
        if (!events[i].used) {
            events[i].used = 1;
            events[i].signaled = initial_state ? 1 : 0;
            events[i].manual_reset = manual_reset ? 1 : 0;
            events[i].waiters = 0;
            return i + 1;   // 1-based handle
        }
    }
    return 0;
}

int event_set(int handle) {
    if (handle < 1 || handle > SYNC_MAX_EVENTS) return 0;
    event_t* e = &events[handle - 1];
    if (!e->used) return 0;
    e->signaled = 1;
    return 1;
}

int event_reset(int handle) {
    if (handle < 1 || handle > SYNC_MAX_EVENTS) return 0;
    event_t* e = &events[handle - 1];
    if (!e->used) return 0;
    e->signaled = 0;
    return 1;
}

void event_destroy(int handle) {
    if (handle < 1 || handle > SYNC_MAX_EVENTS) return;
    events[handle - 1].used = 0;
}

// ─── Generic Wait ───────────────────────────────────────────────
uint32_t wait_for_single_object(int handle, uint32_t timeout_ms, int obj_type) {
    // Object types: 1=mutex, 2=semaphore, 3=event, 4=critical section
    uint32_t start = timer_ms();
    uint32_t timeout_tick = (timeout_ms == 0xFFFFFFFF) ? 0xFFFFFFFF : timeout_ms;

    for (;;) {
        int satisfied = 0;

        switch (obj_type) {
            case 1: { // Mutex
                if (handle < 1 || handle > SYNC_MAX_MUTEXES) return SYNC_WAIT_FAILED;
                mutex_t* m = &mutexes[handle - 1];
                if (!m->used) return SYNC_WAIT_FAILED;
                int cur = task_current_id();
                if (m->owner == -1) {
                    m->owner = cur;
                    m->count = 1;
                    satisfied = 1;
                } else if (m->owner == cur) {
                    m->count++;
                    satisfied = 1;
                }
                break;
            }
            case 2: { // Semaphore
                if (handle < 1 || handle > SYNC_MAX_SEMAPHORES) return SYNC_WAIT_FAILED;
                semaphore_t* s = &semaphores[handle - 1];
                if (!s->used) return SYNC_WAIT_FAILED;
                if (s->count > 0) {
                    s->count--;
                    satisfied = 1;
                }
                break;
            }
            case 3: { // Event
                if (handle < 1 || handle > SYNC_MAX_EVENTS) return SYNC_WAIT_FAILED;
                event_t* e = &events[handle - 1];
                if (!e->used) return SYNC_WAIT_FAILED;
                if (e->signaled) {
                    satisfied = 1;
                    if (!e->manual_reset) e->signaled = 0;  // auto-reset
                }
                break;
            }
            case 4: { // Critical Section
                if (handle < 1 || handle > SYNC_MAX_CRIT_SECTIONS) return SYNC_WAIT_FAILED;
                critical_section_t* cs = &crit_sections[handle - 1];
                if (!cs->used) return SYNC_WAIT_FAILED;
                int cur = task_current_id();
                if (cs->owner == -1) {
                    cs->owner = cur;
                    cs->count = 1;
                    satisfied = 1;
                } else if (cs->owner == cur) {
                    cs->count++;
                    satisfied = 1;
                }
                break;
            }
            default:
                return SYNC_WAIT_FAILED;
        }

        if (satisfied) return SYNC_WAIT_OBJECT_0;

        // Check timeout
        if (timeout_ms != 0xFFFFFFFF) {
            uint32_t elapsed = timer_ms() - start;
            if (elapsed >= timeout_tick) return SYNC_WAIT_TIMEOUT;
        }

        // Yield to other tasks
        task_yield();
    }
}
