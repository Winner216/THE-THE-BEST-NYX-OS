// tools/bench/main.c — Freestanding decode-time benchmark for Nyx OS's
// JPEG decoder, booted under QEMU's TCG (software emulation) to measure
// the REAL in-guest decode cost of 704×704 frames — the number the ECO
// ladder in cpu/video.c actually faces during playback.
//
// Reports per-frame decode ms over COM1 serial (QEMU: -serial file:...).
// Uses the same PIT timing math as video.c's vd_ms_since(): a global
// tick counter (advanced by IRQ0 at ~50 Hz, divisor 23863) plus the
// channel-0 countdown for sub-tick resolution.

#include <stdint.h>
#include <stddef.h>
#include "frame.h"
#include <jpegdec.h>

// ─── x86 I/O ────────────────────────────────────────────────────
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

// ─── COM1 serial ────────────────────────────────────────────────
static void serial_init(void) {
    outb(0x3F8 + 1, 0x00);       // disable interrupts
    outb(0x3F8 + 3, 0x80);       // enable DLAB
    outb(0x3F8 + 0, 0x03);       // divisor low  (38400 baud)
    outb(0x3F8 + 1, 0x00);       // divisor high
    outb(0x3F8 + 3, 0x03);       // 8N1
    outb(0x3F8 + 2, 0xC7);       // FIFO clear/enable
    outb(0x3F8 + 4, 0x0B);       // IRQs enabled, RTS/DSR set
}
static void serial_putc(char c) {
    while (!(inb(0x3F8 + 5) & 0x20));   // wait for THR empty
    outb(0x3F8, (uint8_t)c);
}
static void serial_str(const char* s) {
    while (*s) serial_putc(*s++);
}
static void serial_num(int v) {
    char buf[12]; int i = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + v % 10; v /= 10; }
    while (i > 0) serial_putc(buf[--i]);
}
static void serial_hex32(uint32_t v) {
    for (int i = 7; i >= 0; i--) {
        int d = (v >> (i * 4)) & 0xF;
        serial_putc(d < 10 ? (char)('0' + d) : (char)('A' + d - 10));
    }
}
static void serial_u64(uint64_t v) {
    serial_hex32((uint32_t)(v >> 32));
    serial_putc(':');
    serial_hex32((uint32_t)v);
}
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ─── Tick counter + PIT timing (mirrors video.c) ────────────────
static volatile uint32_t bench_tick = 0;
void bench_tick_handler(void) { bench_tick++; }

#define VD_PIT_DIV  23863
#define VD_PIT_FREQ 1193182

static uint32_t pit_count(void) {
    outb(0x43, 0x00);
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint32_t)((hi << 8) | lo);
}
static int ms_since(uint32_t t0, uint32_t c0) {
    uint32_t t1 = bench_tick;
    uint32_t c1 = pit_count();
    // Same clamp + signed-subtraction fix as video.c: the latched
    // countdown can briefly exceed DIV right after a divisor load, and
    // `(int64_t)(since1 - since0)` wraps in unsigned arithmetic whenever
    // the countdown reloads between samples — casting each operand
    // before subtracting keeps the signed difference.
    uint32_t r0 = (c0 & 0xFFFF); if (r0 > VD_PIT_DIV) r0 = VD_PIT_DIV;
    uint32_t r1 = (c1 & 0xFFFF); if (r1 > VD_PIT_DIV) r1 = VD_PIT_DIV;
    uint32_t since0 = VD_PIT_DIV - r0;
    uint32_t since1 = VD_PIT_DIV - r1;
    int64_t counts = (int64_t)(t1 - t0) * VD_PIT_DIV
                   + ((int64_t)since1 - (int64_t)since0) * 2;
    if (counts < 0) counts = 0;
    return (int)(counts * 1000 / VD_PIT_FREQ);
}

// ─── GDT/IDT (reused verbatim from the kernel) ──────────────────
extern void gdt_install(void);
extern void idt_install(void);
extern void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);
extern void irq0_stub(void);

static void pic_remap(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    // Mask EVERYTHING except the timer (IRQ0): a stray keyboard/mouse
    // IRQ landing mid-decode would take the zeroed IDT gate 33/44 and
    // triple-fault, or corrupt the measurement.  Only the PIT tick is
    // needed, so 0x21 = 0xFE and 0xA1 = 0xFF.
    outb(0x21, 0xFE);  outb(0xA1, 0xFF);
}

static void pit_install(void) {
    uint16_t pit_div = 23863;
    outb(0x43, 0x36);
    outb(0x40, pit_div & 0xFF);
    outb(0x40, pit_div >> 8);
}

// ─── Entry point ────────────────────────────────────────────────
static uint8_t pixbuf[704 * 704 * 4] __attribute__((aligned(64)));

// SSE2 16-byte copy used by the PRESENT bench — mirrors vesa_copy16
// in cpu/vesa.c.  target("sse2") confines XMM usage to this function
// (the bench needs SSE enabled, boot.asm sets CR4.OSFXSR).
__attribute__((target("sse2")))
static void bench_copy16(uint8_t* dst, const uint8_t* src, uint32_t bytes) {
    uint32_t q = bytes >> 6;
    __asm__ volatile (
        "test %2, %2\n\t"
        "jz 2f\n\t"
        "1:\n\t"
        "movdqu  0(%0), %%xmm0\n\t"
        "movdqu 16(%0), %%xmm1\n\t"
        "movdqu 32(%0), %%xmm2\n\t"
        "movdqu 48(%0), %%xmm3\n\t"
        "movdqu %%xmm0,  0(%1)\n\t"
        "movdqu %%xmm1, 16(%1)\n\t"
        "movdqu %%xmm2, 32(%1)\n\t"
        "movdqu %%xmm3, 48(%1)\n\t"
        "add $64, %0\n\t"
        "add $64, %1\n\t"
        "dec %2\n\t"
        "jnz 1b\n\t"
        "2:\n\t"
        : "+r"(src), "+r"(dst), "+r"(q)
        :
        : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
}

void kernel_main(void) {
    serial_init();
    serial_str("\r\nTCGBENCH\n");

    gdt_install();
    idt_install();
    pic_remap();
    idt_set_gate(32, (uint32_t)irq0_stub, 0x08, 0x8E);
    pit_install();
    __asm__ volatile("sti");

    // Warm-up decode (fills caches; first decode is always slower)
    {
        JpegImage w = {0}; w.pixels = pixbuf;
        jpeg_decode(bench_frames[0], bench_lens[0], &w);
    }

    // 3 modes x 3 frames: full, vertical-half (eco_v), both-axes (eco)
    const char* modes[3] = { "FULL", "ECOV", "ECO2" };
    for (int m = 0; m < 3; m++) {
        for (int f = 0; f < 3; f++) {
            JpegImage img = {0}; img.pixels = pixbuf;
            uint32_t t0 = bench_tick;
            uint32_t c0 = pit_count();
            int ok;
            if (m == 0)
                ok = jpeg_decode(bench_frames[f], bench_lens[f], &img);
            else if (m == 1)
                ok = jpeg_decode_eco_v(bench_frames[f], bench_lens[f], &img);
            else
                ok = jpeg_decode_eco(bench_frames[f], bench_lens[f], &img);
            int ms = ms_since(t0, c0);
            // A stray IRQ0 landing mid-decode occasionally corrupts the
            // decode in this minimal (non-kernel) IRQ environment — the
            // CLI-variant run proved the decoder itself is deterministic
            // (9/9 ok).  Retry once so the bench output is stable; two
            // consecutive failures still surface as a real regression.
            if (!ok) {
                JpegImage img2 = {0}; img2.pixels = pixbuf;
                uint32_t tr = bench_tick, cr = pit_count();
                int ok2;
                if (m == 0)
                    ok2 = jpeg_decode(bench_frames[f], bench_lens[f], &img2);
                else if (m == 1)
                    ok2 = jpeg_decode_eco_v(bench_frames[f], bench_lens[f], &img2);
                else
                    ok2 = jpeg_decode_eco(bench_frames[f], bench_lens[f], &img2);
                serial_str("  retry ok="); serial_num(ok2);
                serial_str(" ms="); serial_num(ms_since(tr, cr));
                serial_str("\n");
                img = img2; ok = ok2;
                t0 = tr; c0 = cr;
            }
            ms = ms_since(t0, c0);
            serial_str(modes[m]); serial_str(" f"); serial_num(f);
            serial_str(" ok="); serial_num(ok);
            serial_str(" dims="); serial_num(img.width);
            serial_putc('x'); serial_num(img.height);
            serial_str(" ms="); serial_num(ms);
            serial_str("\n");
        }
    }
    // ── Zero-copy stride correctness ──────────────────────────
    // Decode one full-res frame twice — packed, then into a buffer
    // with a wider row stride + x offset (the way cpu/video.c hands
    // the decoder vesa.draw with vesa.pitch) — and verify every
    // sampled pixel lands at the same RGB.  This is what makes the
    // player's direct-to-framebuffer decode possible.
    {
        static uint8_t strided[704 * (704 * 4 + 64)] __attribute__((aligned(64)));
        JpegImage a = {0}; a.pixels = pixbuf;
        int oka = jpeg_decode(bench_frames[0], bench_lens[0], &a);
        uint32_t W = a.width, H = a.height;
        int sstride = (int)(W * 4 + 64);
        JpegImage b = {0};
        b.pixels = strided + 32;        // start the frame 32 bytes in
        b.stride = sstride;
        int okb = jpeg_decode(bench_frames[0], bench_lens[0], &b);
        int bad = 0;
        for (uint32_t y = 0; y < H && y < 704; y += 13)
            for (uint32_t x = 0; x < W; x += 17) {
                const uint8_t* pa = pixbuf + (y * W + x) * 4;
                const uint8_t* pb = strided + y * (uint32_t)sstride + 32 + x * 4;
                if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]) bad++;
            }
        serial_str("STRIDE ok="); serial_num(oka && okb);
        serial_str(" dims="); serial_num(W); serial_putc('x'); serial_num(H);
        serial_str(" mismatches="); serial_num(bad);
        serial_str("\n");
    }

    // ── Cache-alignment timing (rdtsc — real TSC under KVM) ──
    // Decode one frame repeatedly and report the MIN cycle cost — the
    // best (least host-interference) decode of the batch, since a
    // one-shot average is swamped by KVM scheduling noise (~±20%).
    // The PIT-based ms above is too coarse to see cache-line effects.
    // NOTE: cycles are host-frequency-dependent — only compare runs
    // on the same machine/session, never across hosts.
    {
        JpegImage t = {0}; t.pixels = pixbuf;
        jpeg_decode(bench_frames[1], bench_lens[1], &t);   // warm
        const int reps = 8;
        uint64_t best = ~0ULL;
        for (int r = 0; r < reps; r++) {
            uint64_t t0 = rdtsc();
            jpeg_decode(bench_frames[1], bench_lens[1], &t);
            uint64_t d = rdtsc() - t0;
            if (d < best) best = d;
        }
        serial_str("TSCFULL reps="); serial_num(reps);
        serial_str(" min_cycles="); serial_u64(best);
        serial_str("\n");
    }
    // Deblock cost on a real 704×704 frame: min cycles over reps.
    // Measures jpeg_deblock_rgba on the FULL decoded frame — the pass
    // the video player runs after every decode.
    {
        JpegImage t = {0}; t.pixels = pixbuf;
        jpeg_decode(bench_frames[1], bench_lens[1], &t);
        const int reps = 8;
        uint64_t best = ~0ULL;
        for (int r = 0; r < reps; r++) {
            uint64_t t0 = rdtsc();
            jpeg_deblock_rgba(pixbuf, (int)t.width, (int)t.height, 0);
            uint64_t d = rdtsc() - t0;
            if (d < best) best = d;
        }
        serial_str("DBFULL dims="); serial_num((int)t.width);
        serial_str("x"); serial_num((int)t.height);
        serial_str(" reps="); serial_num(reps);
        serial_str(" min_cycles="); serial_u64(best);
        serial_str("\n");
    }
    // PRESENT copy — the vesa_present() back-buffer blit, which copies
    // pitch×height ≈ 8.3 MB per frame at 1920×1080×32.  Bench on a 2 MiB
    // chunk (the full 8.3 MB is 4.15× these numbers): scalar 4-byte word
    // loop (the old vesa_present) vs the SSE2 16-byte copy now in use.
    // SSE requires CR4.OSFXSR — the bench boot sets it in boot.asm too.
    {
        static uint8_t dstbuf[2 * 1024 * 1024] __attribute__((aligned(64)));
        const uint32_t nbytes = 2 * 1024 * 1024;
        const int reps = 8;
        // scalar word loop
        uint64_t best_s = ~0ULL;
        for (int r = 0; r < reps; r++) {
            uint32_t* s = (uint32_t*)pixbuf, *d = (uint32_t*)dstbuf;
            uint32_t w = nbytes / 4;
            uint64_t t0 = rdtsc();
            while (w--) *d++ = *s++;
            uint64_t dt = rdtsc() - t0;
            if (dt < best_s) best_s = dt;
        }
        // SSE2 16-byte copy (same asm as vesa_copy16, confined with
        // target("sse2") so the rest of the bench stays SSE-free)
        uint64_t best_v = ~0ULL;
        for (int r = 0; r < reps; r++) {
            uint64_t t0 = rdtsc();
            bench_copy16(dstbuf, pixbuf, nbytes);
            uint64_t dt = rdtsc() - t0;
            if (dt < best_v) best_v = dt;
        }
        serial_str("PRESENT copy2MiB scalar_min="); serial_u64(best_s);
        serial_str(" sse16_min="); serial_u64(best_v);
        serial_str("\n");
    }
    serial_str("TCGBENCH DONE\n");
    for (;;) __asm__ volatile("hlt");
}
