// cpu/jpegasync.c  –  Background JPEG decode worker task
//
// The Nyx kernel is a single-CPU OS: QEMU is booted with one core and
// there is no SMP bring-up (no LAPIC, no SIPI/AP trampoline), so a
// true "second core" does not exist yet.  What the preemptive task
// scheduler (cpu/task.c, 50 Hz PIT round-robin) gives us instead is
// TIME-SLICING: the decode runs inside its own task, and the desktop
// task keeps getting its 40 ms slices, so a large image no longer
// monopolizes the CPU and freezes the UI thread while it decodes.
//
// Ownership: the main task writes the job fields and then sets
// job_state = JOB_RUNNING; the worker task reads them and writes the
// result (job_ok/job_w/job_h) before setting JOB_DONE/FAILED.  Each
// field is written by exactly one side before the state flag that
// publishes it, and all are `volatile` — the same single-writer-per-
// phase discipline as task_global_blinks.  No locks needed.
//
// SAFETY CONTRACT: the worker only reads the caller's image bytes
// (which may live in the filesystem array — read-only, the FS is
// never mutated while a job runs) and writes the caller's pixel
// buffer.  It never touches vesa.fb, desktop globals, or I/O, so it
// is safe to preempt at ring 0.
//
// IMPORTANT: the scheduler API must not be called from an IRQ.  The
// worker task is therefore created lazily by jpeg_async_ensure_worker
// from the main loop (via desktop_async_pump), never from a submit.

#include <jpegdec.h>
#include <task.h>
#include <stdint.h>

typedef enum {
    JOB_IDLE    = 0,
    JOB_RUNNING = 1,
    JOB_DONE    = 2,
    JOB_FAILED  = 3,
} jpeg_job_state_t;

static volatile jpeg_job_state_t job_state;
static volatile const uint8_t*   job_data;     // image file bytes (read-only)
static volatile int              job_len;
static volatile uint8_t*         job_out;      // caller's pixel buffer
static volatile int              job_stride;
static volatile int              job_mode;     // 0 full / 1 v-half / 2 both
static volatile int              job_ok;       // decode result
static volatile uint32_t         job_w;        // output dims
static volatile uint32_t         job_h;
static int                       worker_started;

static void jpeg_worker_entry(void) {
    for (;;) {
        if (job_state == JOB_RUNNING) {
            // Local snapshots: the worker is the only reader of the
            // job fields while RUNNING (the main task is idle), so
            // dropping the volatile qualifier here is safe and keeps
            // the decoder's own interface const-clean.
            const uint8_t* data = (const uint8_t*)job_data;
            int  len = (int)job_len;
            uint8_t* out = (uint8_t*)job_out;
            int  stride = (int)job_stride;
            int  mode = (int)job_mode;
            JpegImage img;
            img.pixels = out;
            img.stride = stride;
            img.width  = 0;
            img.height = 0;
            int ok = jpeg_decode_inner(data, len, &img, mode);
            job_w = img.width;
            job_h = img.height;
            job_ok = ok;
            job_state = ok ? JOB_DONE : JOB_FAILED;
        }
        task_sleep(1);   // idle until the next tick; wake when a job lands
    }
}

int jpeg_async_submit(const uint8_t* data, int datalen,
                      uint8_t* out, int stride, int mode) {
    // Only accept from IDLE: while a job is RUNNING the worker may be
    // mid-decode, and while DONE/FAILED the result has not been
    // consumed by desktop_async_pump yet — clobbering either state
    // would corrupt the completion handshake or the caller's buffer.
    if (job_state != JOB_IDLE) return 0;   // busy — caller retries/ignores
    job_data   = data;
    job_len    = datalen;
    job_out    = out;
    job_stride = stride;
    job_mode   = mode;
    job_ok     = 0;
    job_w      = 0;
    job_h      = 0;
    job_state  = JOB_RUNNING;   // publish all fields, then arm the worker
    return 1;
}

// Marks the finished job consumed so the next submit is accepted.
// Caller (desktop_async_pump) must hold interrupts disabled while it
// reads the result and resets — jpeg_async_submit can otherwise run
// inside a key/mouse IRQ and race the completion handshake.
void jpeg_async_reset(void) {
    job_state = JOB_IDLE;
}

// Main-loop only (never IRQ context): create the worker task on first
// use.  If the task table is full, fail any pending job so the caller
// gives up gracefully instead of waiting forever.
void jpeg_async_ensure_worker(void) {
    if (worker_started) return;
    if (task_create("jpegdec", jpeg_worker_entry) >= 0) {
        worker_started = 1;
    } else if (job_state == JOB_RUNNING) {
        job_ok = 0;
        job_state = JOB_FAILED;
    }
}

int jpeg_async_done(void) {
    return job_state == JOB_DONE || job_state == JOB_FAILED;
}

int jpeg_async_ok(void) {
    return job_ok;
}

uint32_t jpeg_async_width(void) {
    return job_w;
}

uint32_t jpeg_async_height(void) {
    return job_h;
}
