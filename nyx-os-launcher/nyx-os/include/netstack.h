// include/netstack.h — Minimal IPv4/TCP networking stack
// Designed for point-to-point HTTP client use over a single NIC.
#ifndef NETSTACK_H
#define NETSTACK_H

#include <stdint.h>

// ─── Configuration ──────────────────────────────────────────────
// QEMU user-mode networking assigns these by default.
// The gateway (10.0.2.1) does NAT and port forwarding.
#define NET_OUR_IP      0x0F02000A  // 10.0.2.15 (host byte order: 10.0.2.15)
#define NET_GATEWAY_IP  0x0202000A  // 10.0.2.2  (QEMU slirp gateway/host alias)
// NOTE: QEMU user-mode networking routes through 10.0.2.2 (the host
// alias), NOT 10.0.2.1. The old 10.0.2.1 default made the guest ARP
// for an address nobody owns, so the gateway never resolved and all
// outbound TCP/DNS failed.
#define NET_PORT_HTTP   80

// ─── Ethernet frame ─────────────────────────────────────────────
#define ETH_TYPE_IP    0x0800
#define ETH_TYPE_ARP   0x0806
#define ETH_HDR_LEN    14

typedef struct __attribute__((packed)) {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t type;
} EthHeader;

// ─── ARP ─────────────────────────────────────────────────────────
#define ARP_HTYPE_ETH  1
#define ARP_PTYPE_IP   0x0800
#define ARP_HLEN       6
#define ARP_PLEN       4
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} ArpPacket;

// ─── IPv4 ────────────────────────────────────────────────────────
#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17
#define IP_FLAG_DF     0x4000  // Don't fragment

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;      // Version (4) + IHL (5)
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} IpHeader;

// ─── TCP ─────────────────────────────────────────────────────────
#define TCP_FLAG_FIN   0x01
#define TCP_FLAG_SYN   0x02
#define TCP_FLAG_RST   0x04
#define TCP_FLAG_PSH   0x08
#define TCP_FLAG_ACK   0x10

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;   // Upper 4 bits = header len / 4
    uint8_t  flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} TcpHeader;

// TCP pseudo-header for checksum
typedef struct __attribute__((packed)) {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_len;
} TcpPseudo;

// ─── TCP state machine ──────────────────────────────────────────
#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2
#define TCP_FIN_WAIT    3
#define TCP_CLOSE_WAIT  4

// ─── Public API ─────────────────────────────────────────────────

// Initialize networking. Call after NIC init.
// Returns 1 on success, 0 on failure.
int netstack_init(void);

// Configure the OS IP address and gateway at runtime.
// Both are in host byte order (e.g. 192.168.100.2 → 0x0264A8C0).
// Call after netstack_init() to override the default 10.0.2.0/24 IPs.
void netstack_set_ip(uint32_t ip, uint32_t gateway);

// Get the current OS IP address.
uint32_t netstack_get_ip(void);

// Configure the DNS server at runtime.
// ip is in LE wire format (e.g. 8.8.8.8 → 0x08080808).
// Default is QEMU DNS proxy (10.0.2.3) — override for real networking.
void netstack_set_dns(uint32_t dns_ip);

// Get the current DNS server IP (LE wire format).
uint32_t netstack_get_dns(void);

// Resolve gateway MAC via ARP. Returns 1 when resolved.
// May need multiple calls — keeps sending ARP requests.
int netstack_resolve_gateway(void);

// Get gateway MAC (after resolve succeeds).
const uint8_t* netstack_gateway_mac(void);

// TCP connection — legacy single-connection API (http/tls/browser).
// Operates on a dedicated connection slot kept open for those clients.
int  tcp_connect(uint32_t ip, uint16_t port);
int  tcp_send(const void* data, int len);
int  tcp_recv(void* buf, int max_len);
void tcp_close(void);
int  tcp_get_state(void);
int  tcp_avail(void);   // bytes buffered in the legacy connection

// Per-connection TCP API (BSD-socket layer).  Open a slot, connect,
// exchange, close.  Several connections can be open simultaneously.
int  tcp_conn_open(void);                    // returns slot index, -1 full
int  tcp_conn_connect(int ci, uint32_t ip, uint16_t port);
int  tcp_conn_send(int ci, const void* data, int len);
int  tcp_conn_recv(int ci, void* buf, int max_len);
int  tcp_conn_avail(int ci);                 // buffered bytes (poll())
void tcp_conn_close(int ci);
int  tcp_conn_state(int ci);

// UDP — one-shot datagram send (simple messages). The stack picks an
// ephemeral source port; replies are routed to dns/dhcp handlers by
// their own pending flags.
int  udp_send(uint32_t dst_ip, uint16_t dst_port,
              const void* payload, int payload_len);

// ARP cache: IP -> MAC mappings learned from every ARP frame seen
// (requests and replies). Returns 0 when the address isn't cached.
const uint8_t* netstack_arp_lookup(uint32_t ip);

// ICMP echo client (drives `net ping`). net_icmp_echo() sends one
// echo request; the shell polls netstack_poll() until
// net_icmp_reply_received() turns true, then reads the RTT.
int      net_icmp_echo(uint32_t dst_ip, uint16_t seq);
int      net_icmp_reply_received(void);
uint32_t net_icmp_reply_rtt_us(void);
void     net_icmp_reply_clear(void);

// Poll the NIC for incoming packets. Must be called regularly.
void netstack_poll(void);

// Set our MAC address (called from NIC driver after init)
void netstack_set_mac(const uint8_t mac[6]);

// Simple checksum
uint16_t net_checksum(const void* data, int len, uint32_t initial);

// ─── LAN scan ────────────────────────────────────────────────────
// Maximum devices discovered by one scan
#define ARP_SCAN_MAX 64

// A discovered device: IP (host byte order) + MAC address
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
} ArpScanResult;

// Scan a /24 subnet by sending ARP requests to every IP from
// subnet_base+1 to subnet_base+254. Blocks until the sweep
// completes (takes ~2-3 seconds). Returns number of devices found.
// Results can be read with netstack_scan_result().
int  netstack_arp_scan(uint32_t subnet_base);

// Number of devices found by the last scan.
int  netstack_scan_count(void);

// Get the i-th result (0-indexed). Safe to call before scan.
const ArpScanResult* netstack_scan_result(int i);

#endif
