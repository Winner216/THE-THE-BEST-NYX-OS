// cpu/syscall.c  –  system-call dispatcher (INT 0x80) + wrappers.
#include <syscall.h>
#include <isr.h>
#include <idt.h>
#include <task.h>
#include <paging.h>
#include <socket.h>
#include <dns.h>
#include <gamesnd.h>
#include <io.h>
#include <stdint.h>
#include <stddef.h>

extern void syscall_stub(void);          // cpu/interrupts.asm
// Game-mode backend (cpu/game.c) — declarations live in game.h but
// syscall.c only needs these three; keep the include graph light.
void* game_syscall_start(const char* title, int w, int h);
void  game_syscall_blit(const void* src);
void  game_syscall_stop(void);
int   game_key_pop(void);
int   game_mouse_poll(int* dx, int* dy);
// Writes `len` bytes to the COM1 debug console (defined in cpu/task.c).
int task_serial_write(const char* s, int len);

void syscall_init(void) {
    // DPL-3 32-bit interrupt gate: callable from ring 3 once user
    // mode lands, and from ring 0 (the current kernel tasks) today.
    idt_set_gate(0x80, (uint32_t)syscall_stub, 0x08, 0xEE);
}

// Called by syscall_stub with a pointer to the interrupt frame
// (registers_t layout: ds, pusha regs, int_no, err, CPU-pushed).
// Dispatches on eax, uses ebx/ecx/edx as args, returns via r->eax so
// the stub's popa/iret delivers it to the caller of `int $0x80`.
void syscall_handler(registers_t* r) {
    uint32_t num = r->eax;
    uint32_t a   = r->ebx;
    uint32_t b   = r->ecx;
    uint32_t c   = r->edx;
    int ret = -1;

    switch (num) {
        case SYS_EXIT:       task_exit();           break; // never returns
        case SYS_YIELD:      task_yield();  ret = 0; break;
        case SYS_SLEEP:      task_sleep((int)a); ret = 0; break;
        case SYS_CREATE:     ret = task_create((const char*)a,
                                               (void (*)(void))b); break;
        case SYS_GETPID:     ret = task_current_id(); break;
        case SYS_ALLOC:      ret = (int)(intptr_t)task_alloc((int)a); break;
        case SYS_ALLOC_PAGE: ret = (int)(intptr_t)task_alloc_page((int)a); break;
        case SYS_PAGES:      ret = paging_priv_count(task_current_id()); break;
        case SYS_WRITE:      ret = task_serial_write((const char*)a, (int)b); break;
        case SYS_SOCKET:     ret = sock_open();  break;
        case SYS_CONNECT:    ret = sock_connect((int)a, (uint32_t)b,
                                                 (uint16_t)c); break;
        case SYS_SEND:       ret = sock_send((int)a, (const void*)b, (int)c); break;
        case SYS_RECV:       ret = sock_recv((int)a, (void*)b, (int)c); break;
        case SYS_CLOSE:      ret = sock_close((int)a); break;
        case SYS_POLL:       ret = sock_poll((int)a); break;
        case SYS_RESOLVE:    ret = (int)dns_resolve((const char*)a); break;
        // ─── Game mode (DOOM) ───────────────────────────────────
        case SYS_GAMESTART:
            // Args b/c = requested canvas w/h (0/0 → 640×400 default).
            ret = (int)(intptr_t)game_syscall_start((const char*)a,
                                                    (int)b, (int)c);
            break;
        case SYS_GAMEBLIT:
            game_syscall_blit((const uint8_t*)a);
            ret = 0;
            break;
        case SYS_GETKEY:     ret = game_key_pop();       break;
        case SYS_GETMOUSE:
            // Writes through raw user pointers are safe: the drain
            // happens only while game mode is ON, and game mode's
            // only owner is the caller (verified at GAMESTART).
            ret = game_mouse_poll((int*)a, (int*)b);
            break;
        case SYS_GAMESTOP:
            game_syscall_stop();
            ret = 0;
            break;
        // ─── Game sound (ring-3 mixer submissions) ─────────────
        case SYS_SNDLOAD:
            ret = gamesnd_load(task_current_id(), (const uint8_t*)a,
                               (int)b, (int)c);
            break;
        case SYS_SNDPLAY:
            // c = chan (high 16) | sep (low 16, signed)
            ret = gamesnd_play(task_current_id(), (int)a, (int)b,
                               (int)(int16_t)(c & 0xFFFF), (int)(c >> 16));
            break;
        case SYS_SNDSTOP:
            ret = gamesnd_stop(task_current_id(), (int)a);
            break;
        case SYS_SNDPARAMS:
            ret = gamesnd_params(task_current_id(), (int)a, (int)b,
                                 (int)(int16_t)(c & 0xFFFF));
            break;
        case SYS_SNDISPLAY:
            ret = gamesnd_isplaying(task_current_id(), (int)a);
            break;
        default:             ret = -1;                 // unknown syscall
    }
    r->eax = (uint32_t)ret;
    (void)c;
}

// ─── int 0x80 wrappers ───────────────────────────────────────────
static inline int sc3(int n, uint32_t a, uint32_t b, uint32_t c) {
    int ret;
    asm volatile("int $0x80"
                 : "=a" (ret)
                 : "a" ((uint32_t)n), "b" (a), "c" (b), "d" (c)
                 : "memory");
    return ret;
}

void sys_exit(void) { sc3(SYS_EXIT, 0, 0, 0); for (;;) { } }
void sys_yield(void) { sc3(SYS_YIELD, 0, 0, 0); }
void sys_sleep(int ticks) { sc3(SYS_SLEEP, (uint32_t)ticks, 0, 0); }
int  sys_create(const char* name, void (*entry)(void)) {
    return sc3(SYS_CREATE, (uint32_t)name, (uint32_t)entry, 0);
}
int  sys_getpid(void) { return sc3(SYS_GETPID, 0, 0, 0); }
void* sys_alloc(int size) { return (void*)sc3(SYS_ALLOC, (uint32_t)size, 0, 0); }
void* sys_alloc_page(int pages) {
    return (void*)sc3(SYS_ALLOC_PAGE, (uint32_t)pages, 0, 0);
}
int  sys_pages(void) { return sc3(SYS_PAGES, 0, 0, 0); }
int  sys_write(const char* buf, int len) {
    return sc3(SYS_WRITE, (uint32_t)buf, (uint32_t)len, 0);
}
int  sys_socket(void) { return sc3(SYS_SOCKET, 0, 0, 0); }
int  sys_connect(int fd, uint32_t ip, uint16_t port) {
    return sc3(SYS_CONNECT, (uint32_t)fd, (uint32_t)ip, (uint32_t)port);
}
int  sys_send(int fd, const void* buf, int len) {
    return sc3(SYS_SEND, (uint32_t)fd, (uint32_t)buf, (uint32_t)len);
}
int  sys_recv(int fd, void* buf, int max_len) {
    return sc3(SYS_RECV, (uint32_t)fd, (uint32_t)buf, (uint32_t)max_len);
}
int  sys_close(int fd) { return sc3(SYS_CLOSE, (uint32_t)fd, 0, 0); }
int  sys_poll(int fd) { return sc3(SYS_POLL, (uint32_t)fd, 0, 0); }
uint32_t sys_resolve(const char* host) {
    return (uint32_t)sc3(SYS_RESOLVE, (uint32_t)host, 0, 0);
}

// ─── Game-mode wrappers (ring-3 side, used by DOOM.EXE) ─────────
void* sys_gamestart(const char* title) {
    return (void*)sc3(SYS_GAMESTART, (uint32_t)title, 0, 0);
}
void sys_gameblit(const void* frame) {
    sc3(SYS_GAMEBLIT, (uint32_t)frame, 0, 0);
}
int  sys_getkey(void) { return sc3(SYS_GETKEY, 0, 0, 0); }
int  sys_getmouse(int* dx, int* dy) {
    return sc3(SYS_GETMOUSE, (uint32_t)dx, (uint32_t)dy, 0);
}
void sys_gamestop(void) { sc3(SYS_GAMESTOP, 0, 0, 0); }
