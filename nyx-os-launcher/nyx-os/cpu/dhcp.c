// cpu/dhcp.c — Minimal DHCP client
//
// Sends a DHCP DISCOVER as a UDP broadcast, polls for the OFFER
// response, and parses the DHCP options to extract IP, subnet mask,
// gateway (router), DNS server, and lease time. Configures the
// network stack with the offered values.
//
// This is a simplified implementation: we only do DISCOVER → OFFER
// (no REQUEST → ACK) — acceptable for client-side configuration.

#include <dhcp.h>
#include <netstack.h>
#include <nic.h>
#include <stdint.h>

// ─── Broadcast MAC for DHCP DISCOVER ─────────────────────────────
static const uint8_t dhcp_bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─── DHCP packet structure (BOOTP header + magic cookie) ────────
typedef struct __attribute__((packed)) {
    uint8_t  op;            // 1 = BOOTREQUEST, 2 = BOOTREPLY
    uint8_t  htype;         // 1 = Ethernet
    uint8_t  hlen;          // 6
    uint8_t  hops;          // 0
    uint32_t xid;           // Transaction ID
    uint16_t secs;          // Seconds elapsed
    uint16_t flags;         // 0x8000 = broadcast flag
    uint32_t ciaddr;        // Client IP (0 in DISCOVER)
    uint32_t yiaddr;        // Your IP (set by server)
    uint32_t siaddr;        // Server IP
    uint32_t giaddr;        // Relay agent IP
    uint8_t  chaddr[16];    // Client MAC address
    uint8_t  sname[64];     // Server hostname (unused)
    uint8_t  file[128];     // Boot file (unused)
    uint32_t magic_cookie;  // DHCP_MAGIC_COOKIE
} DhcpPacket;

// Total DHCP packet buffer (header + options)
#define DHCP_PKT_BUF 300

// ─── Network byte order helpers ─────────────────────────────────
static uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }

// ─── Append a DHCP option to the buffer ─────────────────────────
static void opt_add(uint8_t* buf, int* pos, int max, uint8_t code,
                    const uint8_t* data, int len) {
    if (*pos + 2 + len > max) return;
    buf[(*pos)++] = code;
    buf[(*pos)++] = (uint8_t)len;
    for (int i = 0; i < len; i++) buf[(*pos)++] = data[i];
}

static void opt_add_byte(uint8_t* buf, int* pos, int max,
                         uint8_t code, uint8_t val) {
    opt_add(buf, pos, max, code, &val, 1);
}

// ─── Response buffer (filled by dhcp_handle_udp) ────────────────
static uint8_t  dhcp_resp[DHCP_PKT_BUF];
static int      dhcp_resp_len = 0;
static int      dhcp_pending = 0;
static uint32_t dhcp_pending_xid = 0;

// ─── Send UDP packet for DHCP ───────────────────────────────────
// Builds raw IP/UDP packet with source IP 0.0.0.0 (we have no IP
// yet) and destination IP 255.255.255.255 (broadcast). The Ethernet
// frame goes to broadcast MAC (FF:FF:FF:FF:FF:FF) so any DHCP server
// on the LAN segment receives it.
static int dhcp_udp_send(const void* payload, int payload_len) {
    uint8_t pkt[ETH_HDR_LEN + sizeof(IpHeader) + 8 + DHCP_PKT_BUF];
    int total = 0;

    // ── Ethernet: broadcast destination ──
    EthHeader* eth = (EthHeader*)pkt;
    for (int i = 0; i < 6; i++) eth->dst_mac[i] = dhcp_bcast_mac[i];
    uint8_t our_mac[6];
    nic_get_mac(our_mac);
    for (int i = 0; i < 6; i++) eth->src_mac[i] = our_mac[i];
    eth->type = htons(ETH_TYPE_IP);
    total += ETH_HDR_LEN;

    // ── IP: 0.0.0.0 → 255.255.255.255 ──
    IpHeader* ip = (IpHeader*)(pkt + total);
    ip->ver_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_len = 0;
    ip->id = 0;
    ip->flags_frag = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->checksum = 0;
    ip->src_ip = 0;              // 0.0.0.0 — no IP yet
    ip->dst_ip = 0xFFFFFFFF;     // 255.255.255.255 — broadcast
    total += sizeof(IpHeader);

    // ── UDP: port 68 → 67 ──
    uint16_t udp_len = 8 + payload_len;
    uint16_t udp_src_port = htons(DHCP_CLIENT_PORT);
    uint16_t udp_dst_port = htons(DHCP_SERVER_PORT);
    uint16_t udp_length   = htons(udp_len);
    uint16_t udp_checksum = 0;
    for (int i = 0; i < 2; i++) pkt[total + 0 + i] = ((uint8_t*)&udp_src_port)[i];
    for (int i = 0; i < 2; i++) pkt[total + 2 + i] = ((uint8_t*)&udp_dst_port)[i];
    for (int i = 0; i < 2; i++) pkt[total + 4 + i] = ((uint8_t*)&udp_length)[i];
    for (int i = 0; i < 2; i++) pkt[total + 6 + i] = ((uint8_t*)&udp_checksum)[i];
    total += 8;

    // ── Payload ──
    for (int i = 0; i < payload_len; i++)
        pkt[total + i] = ((const uint8_t*)payload)[i];
    total += payload_len;

    // Fix IP total length and checksum
    int ip_total = sizeof(IpHeader) + udp_len;
    ip->total_len = htons(ip_total);
    ip->checksum = 0;
    ip->checksum = net_checksum(ip, sizeof(IpHeader), 0);

    return nic_send(pkt, total);
}

// ─── Read a 4-byte IP from a DHCP option into LE wire format ───
// DHCP options carry IP addresses in network byte order on the
// wire. On a little-endian host, reading the raw bytes as a
// little-endian uint32 directly yields our internal LE wire
// format (which is also little-endian per-byte, matching x86).
static uint32_t read_ip_le(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

// ─── Parse DHCP options ─────────────────────────────────────────
// Walks the options buffer (starts after magic cookie), extracts
// fields we care about into result struct. IP values are stored
// in LE wire format matching our stack convention.
static void parse_opts(const uint8_t* opts_start, int opts_len,
                       DhcpResult* result) {
    int pos = 0;
    while (pos < opts_len) {
        uint8_t code = opts_start[pos];
        if (code == 0) { pos++; continue; }         // padding
        if (code == DHCP_OPT_END) break;             // end marker
        if (pos + 1 >= opts_len) break;
        uint8_t len = opts_start[pos + 1];
        if (pos + 2 + len > opts_len) break;

        const uint8_t* d = opts_start + pos + 2;

        switch (code) {
            case DHCP_OPT_SUBNET_MASK:
                if (len >= 4) result->subnet = read_ip_le(d);
                break;
            case DHCP_OPT_ROUTER:
                if (len >= 4) result->gateway = read_ip_le(d);
                break;
            case DHCP_OPT_DNS:
                if (len >= 4) result->dns = read_ip_le(d);
                break;
            case DHCP_OPT_SERVER_ID:
                if (len >= 4) result->server_id = read_ip_le(d);
                break;
            case DHCP_OPT_LEASE_TIME:
                // Lease time is a 4-byte integer in network byte order.
                // Reconstruct as big-endian, then byteswap to host.
                if (len >= 4) {
                    uint32_t raw_be = ((uint32_t)d[0] << 24) |
                                      ((uint32_t)d[1] << 16) |
                                      ((uint32_t)d[2] << 8) |
                                      (uint32_t)d[3];
                    // Byteswap from big-endian to little-endian host
                    result->lease = ((raw_be >> 24) & 0xFF) |
                                    ((raw_be >> 8)  & 0xFF00) |
                                    ((raw_be << 8)  & 0xFF0000) |
                                    ((raw_be << 24) & 0xFF000000);
                }
                break;
        }
        pos += 2 + len;
    }
}

// ─── Called from netstack when a UDP packet arrives ─────────────
void dhcp_handle_udp(const uint8_t* payload, int len, uint16_t dst_port) {
    (void)dst_port;
    if (!dhcp_pending) return;
    if (len < (int)sizeof(DhcpPacket)) return;

    const DhcpPacket* pkt = (const DhcpPacket*)payload;
    if (pkt->op != 2) return;              // not BOOTREPLY
    if (pkt->xid != dhcp_pending_xid) return;
    if (pkt->magic_cookie != DHCP_MAGIC_COOKIE) return;

    // Copy the response
    int cp = len;
    if (cp > DHCP_PKT_BUF) cp = DHCP_PKT_BUF;
    for (int i = 0; i < cp; i++) dhcp_resp[i] = payload[i];
    dhcp_resp_len = cp;
}

// ─── Public: DHCP DISCOVER → OFFER ──────────────────────────────
//
// Builds a DHCP DISCOVER packet:
//   - BOOTP header with our MAC address
//   - Broadcast flag set
//   - Magic cookie
//   - DHCP message type = DISCOVER (option 53)
//   - Parameter request list (option 55): subnet mask, router, DNS,
//     lease time, server ID
//   - Client identifier (option 61): our MAC
//   - Host name (option 12): "nyx"
//   - End marker (255)
//
// Sends it as a UDP broadcast, then polls for an OFFER for up to
// ~8 seconds. When an OFFER arrives, parses it and fills the result.
//
// Returns 1 on success, 0 on timeout/failure.
int dhcp_discover(DhcpResult* result) {
    if (!result) return 0;

    // Clear result
    result->yiaddr    = 0;
    result->subnet    = 0;
    result->gateway   = 0;
    result->dns       = 0;
    result->server_id = 0;
    result->lease     = 0;

    // ── Build DISCOVER packet ──
    uint8_t buf[DHCP_PKT_BUF];
    DhcpPacket* pkt = (DhcpPacket*)buf;

    pkt->op    = 1;    // BOOTREQUEST
    pkt->htype = 1;    // Ethernet
    pkt->hlen  = 6;
    pkt->hops  = 0;

    // Transaction ID: use tick count (from isr.c) as a simple random
    extern volatile uint32_t tick;
    uint32_t xid = tick + 0x1234ABCD;
    dhcp_pending_xid = xid;
    pkt->xid = xid;

    pkt->secs  = 0;
    pkt->flags = htons(0x8000);  // Broadcast flag
    pkt->ciaddr = 0;
    pkt->yiaddr = 0;
    pkt->siaddr = 0;
    pkt->giaddr = 0;

    // Client MAC
    uint8_t our_mac[6];
    nic_get_mac(our_mac);
    for (int i = 0; i < 6; i++) pkt->chaddr[i] = our_mac[i];
    for (int i = 6; i < 16; i++) pkt->chaddr[i] = 0;

    // Zero out sname and file
    for (int i = 0; i < 64; i++) pkt->sname[i] = 0;
    for (int i = 0; i < 128; i++) pkt->file[i] = 0;

    // Magic cookie
    pkt->magic_cookie = DHCP_MAGIC_COOKIE;

    // ── Append options ──
    int opt_pos = sizeof(DhcpPacket);

    // Option 53: DHCP message type = DISCOVER
    opt_add_byte(buf, &opt_pos, DHCP_PKT_BUF, DHCP_OPT_MSG_TYPE, DHCP_DISCOVER);

    // Option 61: Client identifier (MAC)
    opt_add(buf, &opt_pos, DHCP_PKT_BUF, DHCP_OPT_CLIENT_ID, our_mac, 6);

    // Option 12: Host name "nyx"
    opt_add(buf, &opt_pos, DHCP_PKT_BUF, DHCP_OPT_HOSTNAME,
            (const uint8_t*)"nyx", 3);

    // Option 55: Parameter request list
    uint8_t req_params[] = {
        DHCP_OPT_SUBNET_MASK,  // 1  - subnet mask
        DHCP_OPT_ROUTER,       // 3  - router/gateway
        DHCP_OPT_DNS,          // 6  - DNS server
        DHCP_OPT_LEASE_TIME,   // 51 - lease time
        DHCP_OPT_SERVER_ID     // 54 - server identifier
    };
    opt_add(buf, &opt_pos, DHCP_PKT_BUF, DHCP_OPT_PARAM_REQ,
            req_params, sizeof(req_params));

    // Option 255: End
    if (opt_pos < DHCP_PKT_BUF) buf[opt_pos++] = DHCP_OPT_END;

    // ── Send DISCOVER ──
    dhcp_pending = 1;
    dhcp_resp_len = 0;

    if (dhcp_udp_send(buf, opt_pos) < 0) {
        dhcp_pending = 0;
        return 0;
    }

    // We'll send 3 DISCOVER packets with delays to ensure at least
    // one gets through (networks sometimes drop the first broadcast)
    for (int retry = 0; retry < 3; retry++) {
        // Send DISCOVER
        dhcp_udp_send(buf, opt_pos);

        // Poll for response with real-time timeout (~2.2 seconds per retry)
        extern volatile uint32_t tick;
        uint32_t dhcp_deadline = tick + 40;
        while (dhcp_resp_len == 0 && (int)(tick - dhcp_deadline) < 0) {
            for (int p = 0; p < 100; p++) netstack_poll();
        }

        if (dhcp_resp_len > 0) break;
    }

    dhcp_pending = 0;

    // ── Process response ──
    if (dhcp_resp_len < (int)sizeof(DhcpPacket)) return 0;

    const DhcpPacket* resp = (const DhcpPacket*)dhcp_resp;

    // Verify it's a BOOTREPLY with matching xid
    if (resp->op != 2) return 0;
    if (resp->xid != dhcp_pending_xid) return 0;

    // Extract offered IP (yiaddr) in LE wire format
    // yiaddr is stored in network byte order in the packet;
    // read_ip_le will convert it to our LE wire format.
    {
        const uint8_t* y = (const uint8_t*)&resp->yiaddr;
        result->yiaddr = read_ip_le(y);
    }

    // Parse options for subnet, gateway, DNS, server_id, lease
    int opt_offset = sizeof(DhcpPacket);
    int opts_len = dhcp_resp_len - opt_offset;
    if (opts_len > 0)
        parse_opts(dhcp_resp + opt_offset, opts_len, result);

    // If yiaddr is 0 after parsing, consider it a failure
    if (result->yiaddr == 0) return 0;

    return 1;
}
