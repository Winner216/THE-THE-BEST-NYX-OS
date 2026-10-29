// kernel/kernel.c
#include <stdint.h>
#include <stddef.h>
#include <terminal.h>
#include <gdt.h>
#include <idt.h>
#include <isr.h>
#include <keyboard.h>
#include <shell.h>
#include <desktop.h>
#include <splash.h>
#include <auth.h>
#include <login.h>
#include <vesa.h>
#include <vbe.h>
#include <power.h>
#include <ata.h>
#include <fs.h>
#include <security.h>
#include <mouse.h>
#include <ac97.h>
#include <smbus.h>
#include <task.h>
#include <paging.h>
#include <syscall.h>
#include <timer.h>
#include <heap.h>
extern char _end[];

void kernel_main(uint32_t magic, multiboot_info_t* mbi) {
    boot_tick_start = tick;   // t0 for the boot-time benchmark

    // Initialize VGA text terminal first (always works as fallback)
    terminal_init();

    // Try to initialize VESA framebuffer from multiboot info
    // magic should be 0x2BADB002 if loaded by a multiboot-compliant loader
    if (magic == 0x2BADB002 && mbi) {
        vesa_init(mbi);
        vesa_enable_double_buffer();
        // Boot-time byte-order self-test: writes red/green/blue test
        // pixels through every blit path (put_pixel, blit_rgba,
        // blit_bgra, convert_rgba_bgra, scaled) and reads them back,
        // halting with a VGA-text diagnostic if any channel is in the
        // wrong position.  Restores the 6 test pixels afterwards, so
        // it leaves zero visual artifacts.  Holds BEFORE any drawing
        // so the framebuffer is guaranteed clean when it runs.
        vesa_self_test();
        vbe_init(mbi);
    }

    // Core CPU setup
    gdt_install();
    idt_install();
    isrs_install();
    irq_install();
    keyboard_install();
    mouse_install();
    auth_init();

    // ── Virtual memory ────────────────────────────────────────────
    // Enable paging BEFORE interrupts fire: identity-maps the kernel
    // (0..1 GiB, 4 KiB pages) plus the VESA LFB above 1 GiB, builds a
    // per-task page directory for every task slot, and installs the
    // physical frame allocator.  Each task gets its own address space
    // via CR3 switching in the scheduler hook; cross-task private-page
    // access now faults instead of reading the other task's memory.
    paging_init((uint32_t)vesa.fb);

    // Heap allocator: identity-mapped region after the kernel image.
    // Must run AFTER paging_init() (heap pages need identity-map) and
    // BEFORE fs_load_embedded() (which kmalloc's content buffers).
    heap_init((uint32_t)_end);

    // System calls: INT 0x80 gate (DPL 3) — tasks request kernel
    // services (create/exit/yield/sleep/alloc) through it.
    syscall_init();

    asm volatile("sti");

    // High-resolution clock: calibrate the TSC against the PIT now
    // that IRQ0 is live (timer_init needs `tick` to advance).  The
    // resulting µs clock is the video player's master clock and PTS
    // time base (cpu/video.c); falls back to PIT ~1 ms when absent.
    timer_init();

    // Start the multitasking scheduler: registers the kernel's own
    // loop as task 0 ("main") and spawns the demo background tasks
    // (worker, blinker).  From here on the PIT IRQ preempts between
    // tasks; the demo tasks are pure computation and never touch the
    // framebuffer/filesystem, so they're safe to run during the
    // splash and login too.
    task_init();

    // AC'97 audio driver (PCI probe + codec reset + BDL setup).  Runs
    // AFTER sti so the PIT tick is live (the driver's refill pacing
    // uses it) and the codec's IRQ line is serviced from the start.
    // Silently no-ops when QEMU lacks the sound card — audio then
    // falls back to the PC-speaker path.
    ac97_init();

    // SMBus probe — reads real battery % from QEMU PIIX4 PM device.
    // No-ops on real hardware or when the PM device is absent.
    smbus_init();

    // Filesystem + icon pre-decode run BEFORE the splash so the boot
    // animation covers ALL of the init work.  By the time the splash
    // finishes, every desktop icon tile is already cached in RAM and
    // login is ready — no dark pause between splash and login.
    // Decoding writes only to static buffers, never the framebuffer,
    // and interrupts are already enabled, so nothing is disturbed.
    // (Worst case — a stale icons.cache falls back to decoding the
    // SVGs synchronously right here — that time is hidden too.)
    fs_load_embedded();
    desktop_icon_warmup();

    // Boot splash — only shown once, not replayed on Lock/Restart
    splash_show();
    boot_tick_splash = tick;   // splash finished — benchmark mark 2

    // Load persisted scanprotect state from .scanprotect
    security_init();

    // Disk + persisted filesystem — DISABLED FOR NOW.
    //
    // Three separate boots froze here (or right around here) even
    // after two different reasoned fixes: first moving this call
    // after sti+splash, then adding a hard global work-budget to the
    // ATA driver so it fails fast instead of grinding through a long
    // timeout. Neither resolved it. That pattern — genuine fixes
    // that don't help — means the actual cause isn't the thing I've
    // been adjusting, and I have no way to see what's really
    // happening on screen or in QEMU's output from here. Continuing
    // to guess at ATA/QEMU internals blind isn't productive and was
    // costing real time on a system that used to just work.
    //
    // Restoring a working OS matters more than this feature landing
    // today. The driver and filesystem persistence code are still
    // fully intact below (cpu/ata.c, cpu/fs.c) — re-enable by
    // uncommenting the two lines below once we can actually diagnose
    // what QEMU/the machine is doing at the point it freezes (e.g.
    // does the login screen ever draw? any output on QEMU's own
    // console/terminal, separate from the guest window?).
    //
    // ata_init();
    // fs_load_from_disk();

    // ── Session loop ──────────────────────────────────────────────
    // The actual event loop is keyboard-IRQ-driven (keypresses are
    // handled asynchronously, not inside this function's call
    // stack), so Lock/Restart/Shutdown can't "return" out of a deep
    // call chain the way a normal function would. Instead they set
    // a flag (power_get_session_action()) that this outer loop
    // polls between hlt cycles, and reacts to here at the top level
    // where it's safe to tear down and restart the relevant stage.
    for (;;) {
        login_show();
        while (!login_is_done()) {
            asm volatile("hlt");
        }

        desktop_init();

        int action = SESSION_RUNNING;
        while (action == SESSION_RUNNING) {
            asm volatile("hlt");
            desktop_heartbeat();
            // Incremental icon-picture decoding runs HERE in
            // main-loop context (interrupts enabled), NOT inside
            // desktop_heartbeat() — a slow SVG decode inside the
            // PIT timer IRQ would block the mouse/keyboard IRQs.
            desktop_icon_pump();
            // Video per-frame redraw (JPEG decode) is deferred the
            // same way — it must never run inside the PIT IRQ.
            desktop_video_pump();
            // Completes async JPEG opens: the decode ran on the
            // background jpegdec task (time-sliced by the scheduler),
            // so opening a large image never freezes the UI thread.
            desktop_async_pump();
            action = power_get_session_action();
        }
        power_clear_session_action();

        if (action == SESSION_RESTART) {
            splash_show();
            continue; // back to login_show() with a fresh splash
        }
        // SESSION_LOCK falls through to the top of the loop too,
        // straight back to login_show() with no splash replay.
    }
}
