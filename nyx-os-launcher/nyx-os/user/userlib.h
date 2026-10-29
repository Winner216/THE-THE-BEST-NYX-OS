// user/userlib.h  –  ring-3 syscall wrappers for user programs.
// The beginnings of a userspace libc: each function is a plain
// `int $0x80` (Nyx OS syscall ABI: eax = number, ebx/ecx = args).
// Numbers match include/syscall.h (kept in sync by hand — user code
// intentionally does not include kernel headers).
#ifndef USERLIB_H
#define USERLIB_H

// Address where the kernel loader copies the command line (must match
// USER_ARGS_BASE in include/elfload.h — kept in sync by hand since
// user code intentionally does not include kernel headers).
#define USER_ARGS_BASE   0xC1E00000u

int  sys_getpid(void);                          // SYS_GETPID = 4
void sys_sleep(int ticks);                      // SYS_SLEEP  = 2
void sys_exit(void) __attribute__((noreturn));  // SYS_EXIT   = 0
int  sys_write(const char* buf, int len);       // SYS_WRITE  = 8

// ─── BSD-socket syscalls (socket layer, SYS_SOCKET=9 .. SYS_POLL=14)
// ip is host byte order; port is a plain integer (0-65535).
int  sys_socket(void);                          // SYS_SOCKET = 9
int  sys_connect(int fd, unsigned ip, int port);    // SYS_CONNECT = 10
int  sys_send(int fd, const char* buf, int len);    // SYS_SEND = 11
int  sys_recv(int fd, char* buf, int max_len);      // SYS_RECV = 12
int  sys_close(int fd);                         // SYS_CLOSE = 13
int  sys_poll(int fd);                          // SYS_POLL = 14
unsigned sys_resolve(const char* host);         // SYS_RESOLVE = 15 → IP

// ─── Tiny freestanding string/print helpers (shared by store apps) ──
// No libc exists in ring 3; these keep the simple apps from each
// re-defining their own copies.
int      u_strlen(const char* s);          // NUL-terminated length
void     u_print(const char* s);           // write a whole string to COM1
// Format unsigned v (decimal) into buf; returns length, NUL-terminates.
int      u_dec(unsigned v, char* buf);
// Format unsigned v (hex, lowercase, no prefix, min digits) into buf;
// returns length, NUL-terminates.  Min width zero-pads.
int      u_hex(unsigned v, char* buf, int min_digits);

#endif
