// cpu/winsock.c  –  Winsock (ws2_32.dll) compatibility for Nyx OS.
//
// Wraps Nyx OS's BSD-socket-style networking (sock_*), DNS (dns_*),
// and TLS (tls_*) into Windows Sockets API shapes.
//
// PE executables import from ws2_32.dll; the import resolver in
// peload.c maps names to these via winapi_lookup().
//
// Limitations:
//   - TCP only (no UDP/raw sockets)
//   - Non-blocking mode stubbed (always returns EWOULDBLOCK for unconnected)
//   - No multicast, multicast options, or raw sockets
//   - Single-threaded: no concurrent socket I/O

#include <winsock.h>
#include <socket.h>
#include <dns.h>
#include <tls.h>
#include <io.h>
#include <string.h>

// ─── Byte-order functions ───────────────────────────────────────
// x86 is little-endian, so we need to swap bytes for network order
uint16_t ws_htons(uint16_t x) { return (x >> 8) | ((x & 0xFF) << 8); }
uint16_t ws_ntohs(uint16_t x) { return ws_htons(x); }
uint32_t ws_htonl(uint32_t x) {
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) | 
           ((x & 0xFF00) << 8) | ((x & 0xFF) << 24);
}
uint32_t ws_ntohl(uint32_t x) { return ws_htonl(x); }

// ─── Global state ───────────────────────────────────────────────
static int wsa_initialized = 0;
static int wsa_last_error = 0;

// Internal socket-to-fd mapping
typedef struct {
    SOCKET winsock_handle;
    int    nyx_fd;          // sock_open() fd
    int    in_use;
} ws_socket_t;

#define MAX_WS_SOCKETS 8
static ws_socket_t ws_sockets[MAX_WS_SOCKETS];
static int ws_next_handle = 1;

// DNS result buffer
static uint32_t last_dns_ip = 0;
static char last_dns_name[64] = {0};

// ─── Winsock initialization ─────────────────────────────────────

int WSAStartup(uint16_t wVersionRequired, void* lpWSAData) {
    (void)wVersionRequired;
    (void)lpWSAData;
    wsa_initialized = 1;
    wsa_last_error = 0;
    memset(ws_sockets, 0, sizeof(ws_sockets));
    return 0;
}

int WSACleanup(void) {
    wsa_initialized = 0;
    return 0;
}

int WSAGetLastError(void) {
    return wsa_last_error;
}

void WSASetLastError(int iError) {
    wsa_last_error = iError;
}

// ─── Socket operations ──────────────────────────────────────────

SOCKET ws_socket(int af, int type, int protocol) {
    (void)af;
    if (type != SOCK_STREAM || (protocol != IPPROTO_TCP && protocol != 0)) {
        wsa_last_error = WSAEPROTONOSUPPORT;
        return INVALID_SOCKET;
    }

    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_WS_SOCKETS; i++) {
        if (!ws_sockets[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        wsa_last_error = 0;
        return INVALID_SOCKET;
    }

    int fd = sock_open();
    if (fd < 0) {
        wsa_last_error = WSAENOBUFS;
        return INVALID_SOCKET;
    }

    ws_sockets[slot].winsock_handle = ws_next_handle++;
    ws_sockets[slot].nyx_fd = fd;
    ws_sockets[slot].in_use = 1;

    return ws_sockets[slot].winsock_handle;
}

int ws_closesocket(SOCKET s) {
    for (int i = 0; i < MAX_WS_SOCKETS; i++) {
        if (ws_sockets[i].in_use && ws_sockets[i].winsock_handle == s) {
            sock_close(ws_sockets[i].nyx_fd);
            ws_sockets[i].in_use = 0;
            return 0;
        }
    }
    wsa_last_error = WSAENOTSOCK;
    return SOCKET_ERROR;
}

static int find_nyx_fd(SOCKET s) {
    for (int i = 0; i < MAX_WS_SOCKETS; i++) {
        if (ws_sockets[i].in_use && ws_sockets[i].winsock_handle == s) {
            return ws_sockets[i].nyx_fd;
        }
    }
    return -1;
}

int ws_connect(SOCKET s, const SOCKADDR* name, int namelen) {
    (void)namelen;
    int fd = find_nyx_fd(s);
    if (fd < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }

    if (!name || name->sa_family != AF_INET) {
        wsa_last_error = WSAEAFNOSUPPORT;
        return SOCKET_ERROR;
    }

    const SOCKADDR_IN* addr = (const SOCKADDR_IN*)name;
    // sin_addr and sin_port are already in network byte order from the PE
    // sock_connect expects host byte order, so convert
    uint32_t ip = ntohl(addr->sin_addr);
    uint16_t port = ntohs(addr->sin_port);

    int ret = sock_connect(fd, ip, port);
    if (ret < 0) {
        wsa_last_error = WSAECONNREFUSED;
        return SOCKET_ERROR;
    }
    return 0;
}

int ws_send(SOCKET s, const char* buf, int len, int flags) {
    (void)flags;
    int fd = find_nyx_fd(s);
    if (fd < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }

    int ret = sock_send(fd, buf, len);
    if (ret < 0) {
        wsa_last_error = WSAENETRESET;
        return SOCKET_ERROR;
    }
    return ret;
}

int ws_recv(SOCKET s, char* buf, int len, int flags) {
    (void)flags;
    int fd = find_nyx_fd(s);
    if (fd < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }

    int ret = sock_recv(fd, buf, len);
    if (ret < 0) {
        wsa_last_error = WSAENETRESET;
        return SOCKET_ERROR;
    }
    if (ret == 0) {
        wsa_last_error = WSAEWOULDBLOCK;
        return SOCKET_ERROR;
    }
    return ret;
}

int ws_shutdown(SOCKET s, int how) {
    (void)how;
    int fd = find_nyx_fd(s);
    if (fd < 0) {
        wsa_last_error = WSAENOTSOCK;
        return SOCKET_ERROR;
    }
    sock_close(fd);
    return 0;
}

int ws_ioctlsocket(SOCKET s, long cmd, uint32_t* argp) {
    (void)s; (void)cmd; (void)argp;
    return 0;
}

// ─── Address conversion ─────────────────────────────────────────

uint32_t ws_inet_addr(const char* cp) {
    if (!cp) return 0;
    uint32_t addr = 0;
    int shift = 24;
    const char* p = cp;
    while (*p && shift >= 0) {
        uint32_t octet = 0;
        while (*p >= '0' && *p <= '9') {
            octet = octet * 10 + (*p - '0');
            p++;
        }
        addr |= (octet << shift);
        shift -= 8;
        if (*p == '.') p++;
    }
    return htonl(addr);
}

static char inet_ntoa_buf[16];
char* ws_inet_ntoa(uint32_t in) {
    uint32_t addr = ntohl(in);
    uint8_t* bytes = (uint8_t*)&addr;
    int n = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t b = bytes[i];
        char tmp[4];
        int p = 0;
        if (b == 0) { tmp[p++] = '0'; }
        else {
            while (b > 0) { tmp[p++] = '0' + (b % 10); b /= 10; }
        }
        for (int j = p - 1; j >= 0; j--) inet_ntoa_buf[n++] = tmp[j];
        if (i < 3) inet_ntoa_buf[n++] = '.';
    }
    inet_ntoa_buf[n] = 0;
    return inet_ntoa_buf;
}

uint32_t ws_gethostbyname(const char* name, char* buf, int buflen) {
    if (!name) return 0;

    int is_ip = 1;
    for (const char* p = name; *p; p++) {
        if ((*p < '0' || *p > '9') && *p != '.') {
            is_ip = 0;
            break;
        }
    }
    if (is_ip) {
        uint32_t ip = ws_inet_addr(name);
        if (buf && buflen >= 20) {
            memset(buf, 0, 16);
            uint32_t ip_net = ip;
            memcpy(buf + 12, &ip_net, 4);
        }
        return ip;
    }

    uint32_t ip = dns_resolve(name);
    if (ip == 0) {
        wsa_last_error = WSAHOST_NOT_FOUND;
        return 0;
    }

    last_dns_ip = ip;
    int n = 0;
    while (*name && n < 63) { last_dns_name[n++] = *name++; }
    last_dns_name[n] = 0;

    if (buf && buflen >= 20) {
        memset(buf, 0, 16);
        memcpy(buf + 12, &ip, 4);
    }

    return ip;
}

int ws_gethostname(char* name, int namelen) {
    if (!name || namelen < 1) return SOCKET_ERROR;
    const char* host = "nyx-os";
    int n = 0;
    while (*host && n < namelen - 1) { name[n++] = *host++; }
    name[n] = 0;
    return 0;
}

// ─── Select ─────────────────────────────────────────────────────

int ws_select(int nfds, FD_SET* readfds, FD_SET* writefds, FD_SET* exceptfds, const TIMEVAL* timeout) {
    (void)nfds; (void)timeout;
    int ready = 0;
    if (readfds) {
        for (uint32_t i = 0; i < readfds->fd_count; i++) {
            int fd = find_nyx_fd(readfds->fd_array[i]);
            if (fd >= 0 && sock_poll(fd) > 0) ready++;
        }
    }
    if (writefds) ready += writefds->fd_count;
    if (exceptfds) exceptfds->fd_count = 0;
    return ready;
}
