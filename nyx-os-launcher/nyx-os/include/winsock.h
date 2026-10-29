// include/winsock.h  –  Winsock (ws2_32.dll) compatibility layer for Nyx OS.
//
// Maps Windows Sockets API calls to Nyx OS's existing BSD-socket-style
// networking stack.  PE executables import from ws2_32.dll; the import
// resolver in peload.c maps these names to our implementations.
//
// Supported APIs:
//   WSAStartup, WSACleanup, WSAGetLastError
//   socket, closesocket, connect, send, recv, select
//   gethostbyname, gethostname
//   inet_addr, inet_ntoa, htons, ntohs
//   ioctlsocket (stub)
//
// Architecture:
//   winsock.c wraps sock_open/sock_connect/sock_send/sock_recv/sock_close
//   for TCP, and dns_resolve for hostname resolution.
//
// NOTE: This is a minimal Winsock implementation — enough for Telegram
// to establish TCP connections.  UDP, multicast, and raw sockets are
// not implemented.

#ifndef WINSOCK_H
#define WINSOCK_H

#include <stdint.h>

// ─── Winsock type definitions ───────────────────────────────────
typedef int SOCKET;
typedef uint32_t WSOCK_ULONG;
typedef uint16_t WSOCK_USHORT;
typedef short WSOCK_SHORT;
typedef unsigned long u_long;

// Invalid socket
#define INVALID_SOCKET  ((SOCKET)(-1))
#define SOCKET_ERROR    (-1)

// Socket types
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3

// Protocol
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

// Address families
#define AF_UNSPEC       0
#define AF_INET         2
#define AF_INET6        23

#define PF_UNSPEC       AF_UNSPEC
#define PF_INET         AF_INET
#define PF_INET6        AF_INET6

// Socket flags
#define MSG_OOB         0x001
#define MSG_PEEK        0x002
#define MSG_DONTROUTE   0x004
#define MSG_DONTWAIT    0x040

// Shutdown modes
#define SD_RECEIVE      0
#define SD_SEND         1
#define SD_BOTH         2

// Select constants
#define FD_SETSIZE      64

// Winsock errors
#define WSAEWOULDBLOCK      10035
#define WSAEINPROGRESS      10036
#define WSAEALREADY         10037
#define WSAENOTSOCK         10038
#define WSAEDESTADDRREQ     10039
#define WSAEMSGSIZE         10040
#define WSAEPROTOTYPE       10041
#define WSAENOPROTOOPT      10042
#define WSAEPROTONOSUPPORT  10043
#define WSAESOCKTNOSUPPORT  10044
#define WSAEOPNOTSUPP       10045
#define WSAEPFNOSUPPORT     10046
#define WSAEAFNOSUPPORT     10047
#define WSAEADDRINUSE       10048
#define WSAEADDRNOTAVAIL    10049
#define WSAENETDOWN         10050
#define WSAENETUNREACH      10051
#define WSAENETRESET        10052
#define WSAECONNABORTED     10053
#define WSAECONNRESET       10054
#define WSAENOBUFS          10055
#define WSAEISCONN          10056
#define WSAENOTCONN         10057
#define WSAESHUTDOWN        10058
#define WSAETOOMANYREFS     10059
#define WSAETIMEDOUT        10060
#define WSAECONNREFUSED     10061
#define WSAELOOP            10062
#define WSAENAMETOOLONG     10063
#define WSAEHOSTDOWN        10064
#define WSAEHOSTUNREACH     10065
#define WSAENOTEMPTY        10066
#define WSAEPROCLIM         10067
#define WSAEUSERS           10068
#define WSAEDQUOT           10069
#define WSAESTALE           10070
#define WSASYSNOTREADY      10091
#define WSAVERNOTSUPPORTED  10092
#define WSANOTINITIALISED   10093
#define WSAEDISCON          10101
#define WSATYPE_NOT_FOUND    10109
#define WSAHOST_NOT_FOUND   11001
#define WSATRY_AGAIN        11002
#define WSANO_RECOVERY      11003
#define WSANO_DATA          11004

// Windows structures
#pragma pack(push, 1)

typedef struct {
    unsigned short sa_family;
    char           sa_data[14];
} SOCKADDR;

typedef struct {
    unsigned short sin_family;
    unsigned short sin_port;
    uint32_t       sin_addr;
    char           sin_zero[8];
} SOCKADDR_IN;

typedef struct {
    uint32_t s_addr;
} IN_ADDR;

// hostent structure for gethostbyname
typedef struct hostent {
    char*  h_name;
    char** h_aliases;
    short  h_addrtype;
    short  h_length;
    char** h_addr_list;
} HOSTENT;

// fd_set for select()
typedef struct {
    uint32_t fd_count;
    SOCKET   fd_array[FD_SETSIZE];
} FD_SET;

// timeval for select()
typedef struct {
    long tv_sec;
    long tv_usec;
} TIMEVAL;

#pragma pack(pop)

// ─── Byte-order function declarations ───────────────────────────
// Declared here so other files can use them before the macros
uint16_t ws_htons(uint16_t x);
uint16_t ws_ntohs(uint16_t x);
uint32_t ws_htonl(uint32_t x);
uint32_t ws_ntohl(uint32_t x);

// ─── Byte-order macros ──────────────────────────────────────────
// x86 is little-endian - these map to the functions declared above
#define htons  ws_htons
#define ntohs  ws_ntohs
#define htonl  ws_htonl
#define ntohl  ws_ntohl

// ─── Winsock function prototypes ────────────────────────────────

// Initialization
int  WSAStartup(uint16_t wVersionRequired, void* lpWSAData);
int  WSACleanup(void);
int  WSAGetLastError(void);
void WSASetLastError(int iError);

// Socket operations (ws_ prefix to avoid macro conflicts)
SOCKET ws_socket(int af, int type, int protocol);
int    ws_closesocket(SOCKET s);
int    ws_connect(SOCKET s, const SOCKADDR* name, int namelen);
int    ws_send(SOCKET s, const char* buf, int len, int flags);
int    ws_recv(SOCKET s, char* buf, int len, int flags);
int    ws_shutdown(SOCKET s, int how);
int    ws_ioctlsocket(SOCKET s, long cmd, uint32_t* argp);

// Address conversion
uint32_t ws_inet_addr(const char* cp);
char*    ws_inet_ntoa(uint32_t in);
uint32_t ws_gethostbyname(const char* name, char* buf, int buflen);

// Host info
int  ws_gethostname(char* name, int namelen);

// Select
int  ws_select(int nfds, FD_SET* readfds, FD_SET* writefds, FD_SET* exceptfds, const TIMEVAL* timeout);

// Macros that map to ws_ functions
#define socket       ws_socket
#define closesocket  ws_closesocket
#define connect      ws_connect
#define send         ws_send
#define recv         ws_recv
#define shutdown     ws_shutdown
#define ioctlsocket  ws_ioctlsocket
#define inet_addr    ws_inet_addr
#define inet_ntoa    ws_inet_ntoa
#define gethostbyname ws_gethostbyname
#define gethostname  ws_gethostname
#define select       ws_select

// FD_SET macros
#define FD_ZERO(s)    do { (s)->fd_count = 0; } while(0)
#define FD_SET(fd,s)  do { if ((s)->fd_count < FD_SETSIZE) (s)->fd_array[(s)->fd_count++] = (fd); } while(0)
#define FD_ISSET(fd,s) ({ int _found = 0; for (uint32_t _i = 0; _i < (s)->fd_count; _i++) { if ((s)->fd_array[_i] == (fd)) { _found = 1; break; } } _found; })

#endif
