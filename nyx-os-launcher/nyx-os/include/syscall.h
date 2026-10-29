// include/syscall.h  –  Nyx OS system calls (int 0x80)
//
// Tasks request kernel services through a single software-interrupt
// gate (INT 0x80, DPL 3 so ring-3 user tasks can call it too).  The
// ABI follows the classic kernel convention:
//     eax = syscall number
//     ebx = arg0    ecx = arg1    edx = arg2
//     return value in eax
// syscall_init() installs the IDT gate; cpu/interrupts.asm's
// syscall_stub builds an interrupt frame and calls
// syscall_handler(), which dispatches and writes the result back
// into the frame's eax so the iret delivers it.
//
// The wrappers below are usable from ANY task, including the demo
// background tasks — processes create/exit/yield/sleep through these.

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYS_EXIT        0   // terminate the calling task
#define SYS_YIELD       1   // cooperatively hand over the CPU
#define SYS_SLEEP       2   // WAITING for `ticks` PIT ticks (arg0)
#define SYS_CREATE      3   // spawn a new task (arg0=name, arg1=entry)
#define SYS_GETPID      4   // return the calling task's id
#define SYS_ALLOC       5   // bump-allocate in the task arena (arg0=size)
#define SYS_ALLOC_PAGE  6   // allocate an isolated private page (arg0=pages)
#define SYS_PAGES       7   // private pages mapped for the calling task
#define SYS_WRITE       8   // write `len` bytes at arg0 to the debug
                            // console (COM1) — a user task's only output
                            // channel until the framebuffer syscall lands
#define SYS_SOCKET      9   // open a BSD stream socket → fd (arg0 unused)
#define SYS_CONNECT    10   // connect fd(arg0) to ip(arg1):port(arg2)
#define SYS_SEND       11   // send len(arg2) bytes at arg1 on fd(arg0)
#define SYS_RECV       12   // recv up to arg2 bytes into arg1 on fd(arg0)
#define SYS_CLOSE      13   // close fd(arg0)
#define SYS_POLL       14   // buffered bytes on fd(arg0) (0 = none yet)
#define SYS_RESOLVE    15   // resolve hostname(arg0) → IP (LE wire format)
#define SYS_GAMESTART  16   // enter game mode: canvas at ret, title(arg0)
#define SYS_GAMEBLIT   17   // blit 640x400 BGRA frame from buf(arg0)
#define SYS_GETKEY     18   // pop one key event → -1 empty, else bit7=down
#define SYS_GAMESTOP   19   // leave game mode (game window closes)
#define SYS_SNDLOAD    20   // cache 8-bit PCM: ptr(arg0) len(arg1) rate(arg2) → handle
#define SYS_SNDPLAY    21   // trigger voice: handle(arg0) vol(arg1) chan|sep<<16(arg2)
#define SYS_SNDSTOP    22   // stop this task's voice on chan(arg0)
#define SYS_SNDPARAMS  23   // live vol(arg1)/sep(arg2 low 16) update on chan(arg0)
#define SYS_SNDISPLAY  24   // 1 if this task's voice on chan(arg0) is playing
#define SYS_GETMOUSE   25   // game mode: drain mouse dx→(arg0) dy→(arg1),
                            // ret = button bits (L=1 R=2 M=4), -1 off

void syscall_init(void);            // install the INT 0x80 IDT gate

// ─── Wrappers (int 0x80 from any task) ───────────────────────────
void  sys_exit(void) __attribute__((noreturn));
void  sys_yield(void);
void  sys_sleep(int ticks);
int   sys_create(const char* name, void (*entry)(void));
int   sys_getpid(void);
void* sys_alloc(int size);
void* sys_alloc_page(int pages);
int   sys_pages(void);
int   sys_write(const char* buf, int len);

// ─── BSD-socket syscalls (wrappers for the socket layer) ─────────
int   sys_socket(void);
int   sys_connect(int fd, uint32_t ip, uint16_t port);
int   sys_send(int fd, const void* buf, int len);
int   sys_recv(int fd, void* buf, int max_len);
int   sys_close(int fd);
int   sys_poll(int fd);

// ─── Game-mode syscalls (ring-3 game canvas + raw keys, DOOM) ─────
// See include/game.h and user/doom/doomgeneric_nyx.c.
void* sys_gamestart(const char* title);
void  sys_gameblit(const void* frame);
int   sys_getkey(void);
int   sys_getmouse(int* dx, int* dy);   // -1 when game mode is off
void  sys_gamestop(void);
uint32_t sys_resolve(const char* host);  // → IP (0 on failure)

#endif
