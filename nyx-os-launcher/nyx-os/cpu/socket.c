// cpu/socket.c  –  Nyx OS BSD-socket-style layer (see include/socket.h).
//
// Maps a small fd table onto the netstack's per-connection TCP.  Each
// fd owns one connection slot; the first use of sock_open() pulls up
// the NIC and network stack (same lazy init the browser does), so a
// user program can go straight from `sock_open` to `sock_connect`
// without a separate `nic init` step.
#include <socket.h>
#include <netstack.h>
#include <nic.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int used;       // fd slot taken
    int conn;       // netstack connection slot, or -1
} sockfd_t;

static sockfd_t fds[SOCK_MAX_FDS];
static int      sock_net_up = 0;

// Bring up the NIC + netstack once (idempotent, mirrors browser_init).
static void sock_ensure_net(void) {
    if (sock_net_up) return;
    if (!nic_is_up())
        nic_init();
    netstack_init();
    sock_net_up = 1;
}

int sock_open(void) {
    sock_ensure_net();
    int ci = tcp_conn_open();
    if (ci < 0) return SOCK_ERR;
    for (int i = 0; i < SOCK_MAX_FDS; i++) {
        if (!fds[i].used) {
            fds[i].used = 1;
            fds[i].conn = ci;
            return i;
        }
    }
    tcp_conn_close(ci);   // no fd slot free — give the conn back
    return SOCK_ERR;
}

// The fd table lives in kernel BSS (identity-mapped in every per-task
// page directory), so the syscall handler can touch it while running
// on a user task's CR3 — same model as SYS_WRITE.
static sockfd_t* fd_get(int fd) {
    if (fd < 0 || fd >= SOCK_MAX_FDS) return 0;
    if (!fds[fd].used) return 0;
    return &fds[fd];
}

int sock_connect(int fd, uint32_t ip, uint16_t port) {
    sockfd_t* s = fd_get(fd);
    if (!s) return SOCK_ERR;
    return tcp_conn_connect(s->conn, ip, port);
}

int sock_send(int fd, const void* data, int len) {
    sockfd_t* s = fd_get(fd);
    if (!s) return SOCK_ERR;
    return tcp_conn_send(s->conn, data, len);
}

// Drain the NIC so any received packets get routed into the netstack
// connection buffers (recv/poll analog of waiting for data).  The
// e1000 ISR only moves frames off the card into its software queue;
// netstack_poll() is what parses them into the TCP rx buffers.
static void sock_drain(void) {
    for (int p = 0; p < 100; p++) netstack_poll();
}

int sock_recv(int fd, void* buf, int max_len) {
    sockfd_t* s = fd_get(fd);
    if (!s) return SOCK_ERR;
    sock_drain();
    return tcp_conn_recv(s->conn, buf, max_len);
}

int sock_poll(int fd) {
    sockfd_t* s = fd_get(fd);
    if (!s) return 0;
    sock_drain();
    return tcp_conn_avail(s->conn);
}

int sock_close(int fd) {
    sockfd_t* s = fd_get(fd);
    if (!s) return SOCK_ERR;
    tcp_conn_close(s->conn);
    s->used = 0;
    s->conn = -1;
    return 0;
}
