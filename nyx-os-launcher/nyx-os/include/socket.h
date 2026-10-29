// include/socket.h  –  Nyx OS BSD-socket-style layer (see cpu/socket.c).
//
// A thin POSIX-shaped socket API over the netstack's per-connection
// TCP, plus a poll() analog.  This is the in-kernel equivalent of the
// socket()/connect()/send()/recv()/close()/poll() surface that an
// NSPR-style portability layer (Firefox's PR_* functions) would call
// into — the same shapes, minus the option/address boilerplate.
//
// Available from ring 0 (kernel tasks) directly, and from ring-3 user
// programs through the int 0x80 syscall wrappers (sys_socket() etc. in
// user/userlib.c).
#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>

#define SOCK_MAX_FDS   8   // concurrent open sockets
#define SOCK_ERR      -1
#define SOCK_NO_DATA   0   // recv: nothing buffered (non-blocking)

// Open a new stream socket, returns an fd (>= 0) or SOCK_ERR.
// Auto-initializes the NIC/netstack on first use.
int  sock_open(void);

// Connect fd to ip:port (ip in host byte order). Blocks until the
// TCP handshake completes or fails. Returns 0 or SOCK_ERR.
int  sock_connect(int fd, uint32_t ip, uint16_t port);

// Send `len` bytes. Returns bytes sent or SOCK_ERR.
int  sock_send(int fd, const void* data, int len);

// Receive up to `max_len` bytes. Returns bytes read, SOCK_NO_DATA when
// nothing is buffered yet, SOCK_ERR on a bad fd.
int  sock_recv(int fd, void* buf, int max_len);

// poll() analog: return the number of buffered bytes available on fd,
// or 0 when none (the caller waits/polls the socket again).
int  sock_poll(int fd);

// Close fd and free its connection slot. Returns 0.
int  sock_close(int fd);

#endif
