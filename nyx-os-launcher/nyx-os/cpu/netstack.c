// cpu/netstack.c — Minimal IPv4/TCP networking stack
#include <netstack.h>
#include <nic.h>
#include <dns.h>
#include <dhcp.h>
#include <timer.h>
#include <io.h>
#include <stdint.h>

// ─── Our MAC (set by driver) ────────────────────────────────────
static uint8_t our_mac[6] = {0};

void netstack_set_mac(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) our_mac[i] = mac[i];
}

// ─── Gateway MAC (resolved by ARP) ──────────────────────────────
static uint8_t gw_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static int     gw_resolved = 0;

// ─── ARP LAN scan buffer ────────────────────────────────────────
static ArpScanResult arp_scan_results[ARP_SCAN_MAX];
static int           arp_scan_count = 0;
static int           arp_scan_active = 0;  // 1 = collect all ARP replies

const uint8_t* netstack_gateway_mac(void) { return gw_mac; }

// ─── TCP connection table ──────────────────────────────────────
// Multiple simultaneous connections so the BSD-socket layer (and any
// future concurrent client) can hold several TCP streams at once.
// The legacy single-connection API (tcp_connect/send/recv/close)
// below operates on connection 0 for backward compatibility with
// http.c / tls.c / browser.c.
#define TCP_MAX_CONN 4

typedef struct {
    int      used;       // slot taken
    int      state;      // TCP_* state machine value
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    // 64 KB receive buffer: matches the 0xFFFF window we advertise,
    // so the peer can never out-run us — a full TLS record (16 KB +
    // overhead) or even several fit without dropping a byte.  The old
    // 8 KB buffer silently clamped overflow (ack claimed seq+len even
    // when bytes were dropped), which corrupted large HTTPS responses.
    uint8_t  rx_buf[65536];
    int      rx_len;
} tcp_conn_t;

static tcp_conn_t tcp_conns[TCP_MAX_CONN];
static uint32_t tcp_src_ip   = NET_OUR_IP;
static uint32_t tcp_gateway   = NET_GATEWAY_IP;
static uint16_t tcp_src_port_next = 40000;
static uint16_t udp_src_port_next = 41000;
static int legacy_conn = -1;   // lazily opened on first connect

static void send_arp_request(uint32_t target_ip);   // defined below

// ─── ARP cache: IP -> MAC learned from every ARP frame ───────────
// Any ARP request/reply we see tells us a real IP↔MAC mapping (the
// sender). Cached so the IP send path can reach same-subnet hosts
// directly instead of always bouncing off the gateway.
#define ARP_CACHE_MAX 8
typedef struct {
    uint32_t ip;            // wire-LE (raw) address, same as tcp_src_ip
    uint8_t  mac[6];
} ArpCacheEntry;
static ArpCacheEntry arp_cache[ARP_CACHE_MAX];

static void arp_cache_insert(uint32_t ip, const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        if (arp_cache[i].ip == ip) {                    // refresh existing
            for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_MAX; i++) {           // first free slot
        if (arp_cache[i].ip == 0) {
            arp_cache[i].ip = ip;
            for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
            return;
        }
    }
    arp_cache[0].ip = ip;                               // full: replace oldest
    for (int j = 0; j < 6; j++) arp_cache[0].mac[j] = mac[j];
}

const uint8_t* netstack_arp_lookup(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_MAX; i++)
        if (arp_cache[i].ip == ip) return arp_cache[i].mac;
    return 0;
}

// Pick the L2 destination for an IP packet: a cached ARP mapping for
// that exact host wins; otherwise everything routes through the
// gateway (the NAT default). Same-subnet unknowns get a fire-and-
// forget ARP request so the next frame can use the direct path.
static const uint8_t* resolve_dst_mac(uint32_t dst_ip) {
    const uint8_t* cached = netstack_arp_lookup(dst_ip);
    if (cached) return cached;
    if ((dst_ip & 0xFFFFFF00u) == (tcp_src_ip & 0xFFFFFF00u))
        send_arp_request(dst_ip);       // same /24: ask directly
    return gw_mac;
}

// ─── Network byte order helpers ─────────────────────────────────
static uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }
static uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}
static uint32_t ntohl(uint32_t v) { return htonl(v); }

// ─── Checksum ───────────────────────────────────────────────────
uint16_t net_checksum(const void* data, int len, uint32_t initial) {
    uint32_t sum = initial;
    const uint16_t* p = (const uint16_t*)data;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// ─── Build ethernet frame ───────────────────────────────────────
static void build_eth(uint8_t* buf, const uint8_t* dst, uint16_t type) {
    EthHeader* eth = (EthHeader*)buf;
    for (int i = 0; i < 6; i++) {
        eth->dst_mac[i] = dst[i];
        eth->src_mac[i] = our_mac[i];
    }
    eth->type = htons(type);
}

// Broadcast MAC for ARP requests — ARP is inherently a broadcast
// protocol at layer 2; the Ethernet destination MUST be FF:FF:FF:FF:FF:FF
// so every device on the subnet receives the request.
static const uint8_t arp_bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─── Build ARP request ──────────────────────────────────────────
static void send_arp_request(uint32_t target_ip) {
    uint8_t buf[ETH_HDR_LEN + sizeof(ArpPacket)];
    build_eth(buf, arp_bcast_mac, ETH_TYPE_ARP);
    ArpPacket* arp = (ArpPacket*)(buf + ETH_HDR_LEN);
    arp->htype = htons(ARP_HTYPE_ETH);
    arp->ptype = htons(0x0800);  // IPv4
    arp->hlen = ARP_HLEN;
    arp->plen = ARP_PLEN;
    arp->oper = htons(ARP_OP_REQUEST);
    for (int i = 0; i < 6; i++) { arp->sha[i] = our_mac[i]; arp->tha[i] = 0; }
    arp->spa = tcp_src_ip;
    arp->tpa = target_ip;
    nic_send(buf, ETH_HDR_LEN + sizeof(ArpPacket));
}

// ─── Build and send TCP/IP packet for a connection ───────────────
static int send_tcp_packet(tcp_conn_t* c, uint8_t flags,
                           const void* payload, int payload_len) {
    // 1536 = 14 (eth) + 20 (ip) + 20 (tcp) + 1482 payload.  The old
    // 256-byte payload cap silently truncated HTTP GETs with long
    // paths (e.g. Wikipedia image URLs with ?utm_* query strings) —
    // the request died mid-header ("Host: up"), so the server never
    // answered and every large-path image fetch timed out with an
    // empty response.  HTTP requests are built into 1024-byte buffers
    // (plus Host/UA headers), so 1482 bytes of payload covers them.
    uint8_t pkt[1536];
    if (payload_len > 1482) payload_len = 1482;
    int total = 0;

    build_eth(pkt, resolve_dst_mac(c->dst_ip), ETH_TYPE_IP);
    total += ETH_HDR_LEN;

    IpHeader* ip = (IpHeader*)(pkt + total);
    ip->ver_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_len = 0;
    ip->id = 0;
    ip->flags_frag = htons(0x4000); // DF
    ip->ttl = 64;
    ip->protocol = IP_PROTO_TCP;
    ip->checksum = 0;
    ip->src_ip = tcp_src_ip;
    ip->dst_ip = c->dst_ip;
    total += sizeof(IpHeader);

    TcpHeader* tcp = (TcpHeader*)(pkt + total);
    tcp->src_port = htons(c->src_port);
    tcp->dst_port = htons(c->dst_port);
    tcp->seq_num = htonl(c->seq);    // seq/ack are big-endian on the wire
    tcp->ack_num = htonl(c->ack);
    tcp->data_offset = 0x50; // 5 words = 20 bytes, no options
    tcp->flags = flags;
    tcp->window_size = 0xFFFF;
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;
    total += sizeof(TcpHeader);

    for (int i = 0; i < payload_len; i++)
        pkt[total + i] = ((const uint8_t*)payload)[i];
    total += payload_len;

    int ip_total = sizeof(IpHeader) + sizeof(TcpHeader) + payload_len;
    ip->total_len = htons(ip_total);

    // IP checksum
    ip->checksum = 0;
    ip->checksum = net_checksum(ip, sizeof(IpHeader), 0);

    // TCP checksum via pseudo-header
    uint32_t csum = 0;
    // Pseudo-header as 16-bit words manually
    uint16_t pseudo_words[] = {
        (uint16_t)(tcp_src_ip), (uint16_t)(tcp_src_ip >> 16),
        (uint16_t)(c->dst_ip), (uint16_t)(c->dst_ip >> 16),
        htons(IP_PROTO_TCP), htons(sizeof(TcpHeader) + payload_len)
    };
    for (int i = 0; i < 6; i++) csum += pseudo_words[i];

    // Zero the checksum field for the computation
    tcp->checksum = 0;
    tcp->checksum = net_checksum(tcp, sizeof(TcpHeader) + payload_len, csum);

    return nic_send(pkt, total);
}

// ─── Handle ARP reply ───────────────────────────────────────────
static void handle_arp(const uint8_t* pkt, int len) {
    if (len < ETH_HDR_LEN + (int)sizeof(ArpPacket)) return;
    const ArpPacket* arp = (const ArpPacket*)(pkt + ETH_HDR_LEN);
    uint16_t oper = htons(arp->oper);

    // Every ARP frame teaches us the sender's IP↔MAC mapping.
    arp_cache_insert(arp->spa, arp->sha);

    if (oper == ARP_OP_REQUEST) {
        // Answer "who has our IP?" so inbound packets (DNS replies,
        // TCP ACK/data) can reach us.  Without this the gateway ARPs
        // for our MAC and gets silence, then drops the packet it was
        // trying to deliver (seen live: QEMU slirp re-ARPing forever).
        if (arp->tpa == tcp_src_ip) {
            uint8_t buf[ETH_HDR_LEN + sizeof(ArpPacket)];
            build_eth(buf, arp->sha, ETH_TYPE_ARP);
            ArpPacket* rep = (ArpPacket*)(buf + ETH_HDR_LEN);
            rep->htype = htons(ARP_HTYPE_ETH);
            rep->ptype = htons(0x0800);  // IPv4
            rep->hlen = ARP_HLEN;
            rep->plen = ARP_PLEN;
            rep->oper = htons(ARP_OP_REPLY);
            for (int i = 0; i < 6; i++) {
                rep->sha[i] = our_mac[i];
                rep->tha[i] = arp->sha[i];
            }
            rep->spa = tcp_src_ip;
            rep->tpa = arp->spa;
            nic_send(buf, ETH_HDR_LEN + sizeof(ArpPacket));
        }
        return;
    }

    if (oper == ARP_OP_REPLY) {
        // Always check if this is the gateway (for TCP connectivity)
        if (arp->spa == tcp_gateway) {
            for (int i = 0; i < 6; i++) gw_mac[i] = arp->sha[i];
            gw_resolved = 1;
        }

        // If a LAN scan is active, collect every ARP reply
        if (arp_scan_active && arp_scan_count < ARP_SCAN_MAX) {
            // Don't add duplicates (same IP)
            int dup = 0;
            for (int i = 0; i < arp_scan_count; i++) {
                if (arp_scan_results[i].ip == arp->spa) { dup = 1; break; }
            }
            if (!dup) {
                arp_scan_results[arp_scan_count].ip = arp->spa;
                for (int i = 0; i < 6; i++)
                    arp_scan_results[arp_scan_count].mac[i] = arp->sha[i];
                arp_scan_count++;
            }
        }
    }
}

// Find the open connection whose source port this segment is
// addressed to (dst_port on the wire == our src_port).
static tcp_conn_t* conn_by_port(uint16_t dst_port) {
    for (int i = 0; i < TCP_MAX_CONN; i++)
        if (tcp_conns[i].used && tcp_conns[i].src_port == dst_port)
            return &tcp_conns[i];
    return 0;
}

// ─── Handle TCP segment ─────────────────────────────────────────
static void handle_tcp(const uint8_t* pkt, int len) {
    if (len < ETH_HDR_LEN + (int)sizeof(IpHeader) + (int)sizeof(TcpHeader)) return;
    const TcpHeader* tcp = (const TcpHeader*)(pkt + ETH_HDR_LEN + sizeof(IpHeader));
    int hdr_bytes = (tcp->data_offset >> 4) * 4;
    uint16_t dst_port = htons(tcp->dst_port);
    uint32_t seq = ntohl(tcp->seq_num);   // seq/ack are big-endian on the wire
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = tcp->flags;

    tcp_conn_t* c = conn_by_port(dst_port);
    if (!c) return;

    const uint8_t* payload = (const uint8_t*)tcp + hdr_bytes;
    int avail = len - ETH_HDR_LEN - (int)sizeof(IpHeader) - hdr_bytes;
    int payload_len = (avail > 0) ? avail : 0;

    if (c->state == TCP_SYN_SENT) {
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
            c->ack = seq + 1;
            c->seq = ack;
            c->state = TCP_ESTABLISHED;
            send_tcp_packet(c, TCP_FLAG_ACK, 0, 0);
        }
    } else if (c->state == TCP_ESTABLISHED) {
        if (payload_len > 0) {
            if (seq == c->ack) {
                // In-order data: buffer what fits, ack ONLY what was
                // actually accepted (seq + cp).  Acknowledging more
                // than we buffered made the peer believe bytes were
                // received that we silently dropped — corrupting the
                // stream when a response was larger than rx_buf.
                int room = (int)sizeof(c->rx_buf) - c->rx_len - 1;
                int cp = payload_len;
                if (cp > room) cp = room;
                for (int i = 0; i < cp; i++)
                    c->rx_buf[c->rx_len + i] = payload[i];
                c->rx_len += cp;
                c->rx_buf[c->rx_len] = '\0';
                c->ack = seq + cp;
                send_tcp_packet(c, TCP_FLAG_ACK, 0, 0);
            } else {
                // Out-of-order / retransmitted segment: don't append
                // (that would duplicate bytes).  Re-ack our position.
                send_tcp_packet(c, TCP_FLAG_ACK, 0, 0);
            }
        }
        if ((flags & TCP_FLAG_FIN) && seq + payload_len == c->ack) {
            // FIN is in order and every byte before it was accepted.
            c->ack = seq + payload_len + 1;
            c->state = TCP_CLOSE_WAIT;
            send_tcp_packet(c, TCP_FLAG_ACK, 0, 0);
        }
    } else if (c->state == TCP_FIN_WAIT) {
        if (flags & TCP_FLAG_ACK) {
            c->seq = ack;
            c->state = TCP_CLOSED;
        }
    }
}

// ─── Handle UDP packet ──────────────────────────────────────────
static void handle_udp(const uint8_t* pkt, int len) {
    if (len < ETH_HDR_LEN + (int)sizeof(IpHeader) + 8) return;
    const uint8_t* udp_start = pkt + ETH_HDR_LEN + sizeof(IpHeader);
    uint16_t dst_port = 0;
    dst_port |= (uint16_t)udp_start[2] << 8;
    dst_port |= (uint16_t)udp_start[3];
    uint16_t udp_len = 0;
    udp_len |= (uint16_t)udp_start[4] << 8;
    udp_len |= (uint16_t)udp_start[5];
    if (udp_len < 8) return;
    int payload_len = udp_len - 8;
    if (payload_len < 0) payload_len = 0;
    const uint8_t* payload = udp_start + 8;
    int avail = len - (ETH_HDR_LEN + (int)sizeof(IpHeader) + 8);
    if (payload_len > avail) payload_len = avail;
    // Always route to DNS handler — DNS responses come to our
    // ephemeral source port (40000), not port 53. The handler
    // checks its own dhcp_pending flag and transaction ID so it's
    // a safe no-op when no DNS query is active.
    if (payload_len > 0)
        dns_handle_udp(payload, payload_len, dst_port);
    // Route DHCP replies (port 68 = client port) to DHCP handler.
    // The handler checks its own dhcp_pending flag and xid.
    if (dst_port == 68)
        dhcp_handle_udp(payload, payload_len, dst_port);
}

// ─── ICMP echo client (used by `net ping`) ──────────────────────
#define ICMP_TYPE_ECHO_REQ 8
#define ICMP_TYPE_ECHO_REP 0
static uint16_t icmp_ping_id = 0;
static int      icmp_ping_active = 0;   // 1 = waiting for a reply
static uint16_t icmp_ping_seq = 0;
static volatile int      icmp_ping_got = 0;   // matching reply received
static volatile uint32_t icmp_ping_rtt_us = 0;
static uint32_t icmp_ping_sent_us = 0;

// Send one ICMP echo request to dst_ip. Returns 0 on success.
int net_icmp_echo(uint32_t dst_ip, uint16_t seq) {
    uint8_t pkt[ETH_HDR_LEN + sizeof(IpHeader) + 48];
    int total = 0;

    build_eth(pkt, resolve_dst_mac(dst_ip), ETH_TYPE_IP);
    total += ETH_HDR_LEN;

    IpHeader* ip = (IpHeader*)(pkt + total);
    ip->ver_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_len = 0;
    ip->id = 0;
    ip->flags_frag = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = IP_PROTO_ICMP;
    ip->checksum = 0;
    ip->src_ip = tcp_src_ip;
    ip->dst_ip = dst_ip;
    total += sizeof(IpHeader);

    // ICMP echo request: type 8, code 0, id + seq, 32 bytes of payload.
    int icmp_len = 8 + 32;
    uint8_t* ic = pkt + total;
    ic[0] = ICMP_TYPE_ECHO_REQ;
    ic[1] = 0;
    ic[2] = ic[3] = 0;                         // checksum (below)
    ic[4] = (uint8_t)(icmp_ping_id >> 8);      // id (BE on the wire)
    ic[5] = (uint8_t)icmp_ping_id;
    ic[6] = (uint8_t)(seq >> 8);               // seq
    ic[7] = (uint8_t)seq;
    for (int i = 0; i < 32; i++) ic[8 + i] = (uint8_t)(0x20 + i);
    total += icmp_len;

    ip->total_len = htons(sizeof(IpHeader) + icmp_len);
    ip->checksum = net_checksum(ip, sizeof(IpHeader), 0);
    uint16_t ck = net_checksum(ic, icmp_len, 0);
    ic[2] = (uint8_t)ck;                       // LE-uint16 store (codebase
    ic[3] = (uint8_t)(ck >> 8);                // convention, same as TCP)

    icmp_ping_active = 1;
    icmp_ping_seq = seq;
    icmp_ping_got = 0;
    icmp_ping_rtt_us = 0;
    if (nic_send(pkt, total) < 0) { icmp_ping_active = 0; return -1; }
    icmp_ping_sent_us = (uint32_t)timer_us();
    return 0;
}

// Reply state for the shell's wait loop.
int      net_icmp_reply_received(void) { return icmp_ping_got; }
uint32_t net_icmp_reply_rtt_us(void)   { return icmp_ping_rtt_us; }
void     net_icmp_reply_clear(void)     { icmp_ping_got = 0; icmp_ping_active = 0; }

// ─── Handle ICMP (echo request → reply) ─────────────────────────
// Echo requests are answered so the box is reachable/testable; echo
// replies are matched against an outstanding `net ping` and recorded.
// Both swap/echo fields and rely on handle_ip's header checksum check.
static void handle_icmp(const uint8_t* pkt, int len) {
    int off = ETH_HDR_LEN + (int)sizeof(IpHeader);
    if (len < off + 8) return;
    const IpHeader* ip = (const IpHeader*)(pkt + ETH_HDR_LEN);
    const uint8_t* icmp = pkt + off;
    int icmp_len = htons(ip->total_len) - (int)sizeof(IpHeader);
    if (icmp_len < 8 || off + icmp_len > len) return;

    if (icmp[0] == ICMP_TYPE_ECHO_REP && icmp_ping_active) {
        // Match an outstanding echo request by id + seq.
        uint16_t id  = (uint16_t)((icmp[4] << 8) | icmp[5]);
        uint16_t seq = (uint16_t)((icmp[6] << 8) | icmp[7]);
        if (id == icmp_ping_id && seq == icmp_ping_seq) {
            icmp_ping_got = 1;
            icmp_ping_rtt_us = (uint32_t)(timer_us() - icmp_ping_sent_us);
        }
        return;
    }
    if (icmp[0] != ICMP_TYPE_ECHO_REQ) return;

    uint8_t rep[ETH_HDR_LEN + sizeof(IpHeader) + 256];
    int total = 0;
    // Reply straight back to whoever sent it (its eth src == next hop).
    const EthHeader* eth = (const EthHeader*)pkt;
    build_eth(rep, eth->src_mac, ETH_TYPE_IP);
    total += ETH_HDR_LEN;

    IpHeader* rip = (IpHeader*)(rep + total);
    rip->ver_ihl = 0x45;
    rip->dscp_ecn = 0;
    rip->total_len = 0;
    rip->id = 0;
    rip->flags_frag = htons(0x4000);
    rip->ttl = 64;
    rip->protocol = IP_PROTO_ICMP;
    rip->checksum = 0;
    rip->src_ip = tcp_src_ip;
    rip->dst_ip = ip->src_ip;
    total += sizeof(IpHeader);

    // ICMP echo reply: type/code swapped, id+seq+payload echoed.
    uint8_t* ri = rep + total;
    ri[0] = ICMP_TYPE_ECHO_REP;
    ri[1] = 0;                              // code
    ri[2] = ri[3] = 0;                      // checksum (below)
    for (int i = 4; i < icmp_len; i++) ri[i] = icmp[i];  // id+seq+data
    total += icmp_len;

    rip->total_len = htons(sizeof(IpHeader) + icmp_len);
    rip->checksum = net_checksum(rip, sizeof(IpHeader), 0);
    uint16_t ck = net_checksum(ri, icmp_len, 0);
    // Same LE-uint16 store convention as the IP/TCP checksum fields.
    ri[2] = (uint8_t)ck;
    ri[3] = (uint8_t)(ck >> 8);
    nic_send(rep, total);
}

// ─── Handle IP packet ───────────────────────────────────────────
static void handle_ip(const uint8_t* pkt, int len) {
    if (len < ETH_HDR_LEN + (int)sizeof(IpHeader)) return;
    const IpHeader* ip = (const IpHeader*)(pkt + ETH_HDR_LEN);
    // Validate the header before trusting any field in it.
    if (net_checksum(ip, sizeof(IpHeader), 0) != 0) return;
    if (ip->dst_ip != tcp_src_ip && ip->dst_ip != 0xFFFFFFFF) return;
    if (ip->protocol == IP_PROTO_TCP) handle_tcp(pkt, len);
    else if (ip->protocol == IP_PROTO_UDP) handle_udp(pkt, len);
    else if (ip->protocol == IP_PROTO_ICMP) handle_icmp(pkt, len);
}

// ─── Poll the NIC for incoming packets ──────────────────────────
void netstack_poll(void) {
    uint8_t buf[2048];
    int len = nic_recv(buf, sizeof(buf));
    if (len <= ETH_HDR_LEN) return;
    const EthHeader* eth = (const EthHeader*)buf;
    uint16_t eth_type = htons(eth->type);

    // Accept broadcast or our MAC
    int is_broadcast = (eth->dst_mac[0] == 0xFF);
    int is_ours = 1;
    if (!is_broadcast) {
        is_ours = 0;
        for (int i = 0; i < 6; i++) {
            if (eth->dst_mac[i] == our_mac[i]) is_ours = 1;
            else { is_ours = 0; break; }
        }
    }
    if (!is_ours && !is_broadcast) return;

    if (eth_type == ETH_TYPE_ARP) handle_arp(buf, len);
    else if (eth_type == ETH_TYPE_IP) {
        // Clamp to the real IP packet length. The RTL8139 delivers
        // short frames padded to the 60-byte ethernet minimum, and a
        // zero-payload TCP ACK then looks like it carries (60-54)=6
        // bytes of "data" — the guest ACKs bytes the server never
        // sent and the server retransmits forever. total_len excludes
        // the padding, so 14+total_len is the true packet length.
        if (len >= ETH_HDR_LEN + (int)sizeof(IpHeader)) {
            const IpHeader* iph = (const IpHeader*)(buf + ETH_HDR_LEN);
            int iplen = htons(iph->total_len);
            if (iplen + ETH_HDR_LEN < len) len = iplen + ETH_HDR_LEN;
        }
        handle_ip(buf, len);
    }
}

// ─── Runtime DNS server ──────────────────────────────────────────
static uint32_t tcp_dns_server = 0x0302000A;  // default: 10.0.2.3 (QEMU DNS proxy)

void netstack_set_dns(uint32_t dns_ip) {
    tcp_dns_server = dns_ip;
}

uint32_t netstack_get_dns(void) {
    return tcp_dns_server;
}

// ─── Runtime IP configuration ───────────────────────────────────
void netstack_set_ip(uint32_t ip, uint32_t gateway) {
    tcp_src_ip = ip;
    tcp_gateway = gateway;
    gw_resolved = 0;
}

uint32_t netstack_get_ip(void) {
    return tcp_src_ip;
}

// ─── LAN ARP scan ──────────────────────────────────────────────
int netstack_arp_scan(uint32_t subnet_base) {
    // subnet_base is the base IP of the subnet in host byte order.
    // e.g. 192.168.100.0 → 0x0064A8C0
    // We'll scan all IPs from subnet_base+1 to subnet_base+254.

    arp_scan_count = 0;
    arp_scan_active = 1;

    // Send ARP requests in batches of 10, polling between each
    for (int i = 1; i <= 254; i++) {
        uint32_t target = subnet_base | (uint32_t)i;
        send_arp_request(target);

        // Every 10 requests, poll the NIC to gather replies
        if ((i % 10) == 0) {
            for (int p = 0; p < 500; p++) netstack_poll();
        }
    }

    // Final polling burst to catch any late replies
    for (int p = 0; p < 2000; p++) netstack_poll();

    arp_scan_active = 0;
    return arp_scan_count;
}

int netstack_scan_count(void) {
    return arp_scan_count;
}

const ArpScanResult* netstack_scan_result(int i) {
    if (i < 0 || i >= arp_scan_count) return 0;
    return &arp_scan_results[i];
}

// ─── Public API ─────────────────────────────────────────────────
int netstack_resolve_gateway(void) {
    if (!gw_resolved) send_arp_request(tcp_gateway);
    return gw_resolved;
}

// ─── Per-connection TCP API (used by the BSD-socket layer) ──────

// Open a new connection slot. Returns its index, or -1 when full.
int tcp_conn_open(void) {
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (!tcp_conns[i].used) {
            tcp_conns[i].used = 1;
            tcp_conns[i].state = TCP_CLOSED;
            tcp_conns[i].src_port = 0;
            tcp_conns[i].rx_len = 0;
            return i;
        }
    }
    return -1;
}

int tcp_conn_connect(int ci, uint32_t ip, uint16_t port) {
    if (ci < 0 || ci >= TCP_MAX_CONN || !tcp_conns[ci].used) return -1;
    tcp_conn_t* c = &tcp_conns[ci];
    if (c->state != TCP_CLOSED) return -1;

    c->dst_ip = ip;
    c->dst_port = port;
    c->src_port = tcp_src_port_next++;
    c->seq = 1000 + (c->src_port * 137);
    c->ack = 0;
    c->rx_len = 0;
    c->state = TCP_SYN_SENT;

    // Resolve gateway MAC with real-time timeout. tick is PIT_TARGET_HZ
    // (50 Hz), so tick + 50 is ~1 second.
    extern volatile uint32_t tick;
    uint32_t arp_deadline = tick + 50;
    while (!gw_resolved && (int)(tick - arp_deadline) < 0) {
        send_arp_request(tcp_gateway);
        for (int i = 0; i < 200; i++) netstack_poll();
    }
    if (!gw_resolved) { c->state = TCP_CLOSED; return -1; }

    // Send SYN, retransmitting every ~2 s if no SYN-ACK, up to ~10 s
    // total (tick is 50 Hz). Slow hosts/slirp can take seconds to
    // answer; a single 1-3 s wait failed on a 4.3 s RTT.
    for (int attempt = 0; attempt < 5 && c->state == TCP_SYN_SENT; attempt++) {
        send_tcp_packet(c, TCP_FLAG_SYN, 0, 0);
        uint32_t syn_deadline = tick + 100;
        while (c->state == TCP_SYN_SENT && (int)(tick - syn_deadline) < 0) {
            for (int p = 0; p < 100; p++) netstack_poll();
        }
    }

    if (c->state != TCP_ESTABLISHED) {
        c->state = TCP_CLOSED;
        return -1;
    }
    return 0;
}

int tcp_conn_send(int ci, const void* data, int len) {
    if (ci < 0 || ci >= TCP_MAX_CONN || !tcp_conns[ci].used) return -1;
    tcp_conn_t* c = &tcp_conns[ci];
    if (c->state != TCP_ESTABLISHED) return -1;
    if (send_tcp_packet(c, TCP_FLAG_PSH | TCP_FLAG_ACK, data, len) < 0) return -1;
    c->seq += len;
    return len;
}

int tcp_conn_recv(int ci, void* buf, int max_len) {
    if (ci < 0 || ci >= TCP_MAX_CONN || !tcp_conns[ci].used) return -1;
    tcp_conn_t* c = &tcp_conns[ci];
    if (c->rx_len <= 0) return 0;
    int cp = c->rx_len;
    if (cp > max_len) cp = max_len;
    for (int i = 0; i < cp; i++) ((uint8_t*)buf)[i] = c->rx_buf[i];
    for (int i = cp; i < c->rx_len; i++) c->rx_buf[i - cp] = c->rx_buf[i];
    c->rx_len -= cp;
    return cp;
}

// Bytes currently buffered for `ci` (0 when none — poll() analog).
int tcp_conn_avail(int ci) {
    if (ci < 0 || ci >= TCP_MAX_CONN || !tcp_conns[ci].used) return 0;
    return tcp_conns[ci].rx_len;
}

void tcp_conn_close(int ci) {
    if (ci < 0 || ci >= TCP_MAX_CONN || !tcp_conns[ci].used) return;
    tcp_conn_t* c = &tcp_conns[ci];
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
        send_tcp_packet(c, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        c->state = TCP_FIN_WAIT;
        int timeout = 500;
        while (c->state != TCP_CLOSED && timeout-- > 0) netstack_poll();
    }
    c->state = TCP_CLOSED;
    c->used = 0;   // free the slot so sockets can reopen
}

int tcp_conn_state(int ci) {
    if (ci < 0 || ci >= TCP_MAX_CONN || !tcp_conns[ci].used) return TCP_CLOSED;
    return tcp_conns[ci].state;
}

// ─── Legacy single-connection API (conn 0) ─────────────────────
// Used by http.c, tls.c and browser.c — unchanged semantics.

// The legacy slot is freed by tcp_conn_close, and a concurrent socket
// may have taken it — so always re-open lazily if the slot is gone.
static int legacy_idx(void) {
    if (legacy_conn < 0 || !tcp_conns[legacy_conn].used)
        legacy_conn = tcp_conn_open();
    return legacy_conn;
}

int tcp_connect(uint32_t ip, uint16_t port) {
    int ci = legacy_idx();
    if (ci < 0) return -1;
    return tcp_conn_connect(ci, ip, port);
}

int tcp_send(const void* data, int len) {
    int ci = legacy_idx();
    if (ci < 0) return -1;
    return tcp_conn_send(ci, data, len);
}

int tcp_recv(void* buf, int max_len) {
    int ci = legacy_idx();
    if (ci < 0) return -1;
    return tcp_conn_recv(ci, buf, max_len);
}

void tcp_close(void) {
    if (legacy_conn >= 0 && tcp_conns[legacy_conn].used &&
        tcp_conns[legacy_conn].state != TCP_CLOSED) {
    }
    tcp_conn_close(legacy_conn);
    legacy_conn = -1;   // slot freed — next connect re-opens a fresh one
}

int tcp_get_state(void) {
    int ci = legacy_idx();
    if (ci < 0) return TCP_CLOSED;
    return tcp_conn_state(ci);
}

// Bytes still buffered in the legacy connection (0 when none).
int tcp_avail(void) {
    int ci = legacy_idx();
    if (ci < 0) return 0;
    return tcp_conn_avail(ci);
}

// ─── UDP: one-shot datagram send ────────────────────────────────
// Builds and sends a UDP/IP datagram to dst_ip:dst_port. The stack
// picks an ephemeral source port; the UDP checksum is computed over
// the pseudo-header (same convention as the verified TCP path, which
// slirp's checksum validation accepted).
int udp_send(uint32_t dst_ip, uint16_t dst_port,
             const void* payload, int payload_len) {
    uint8_t pkt[ETH_HDR_LEN + sizeof(IpHeader) + 8 + 512];
    if (payload_len > 512) payload_len = 512;
    if (payload_len < 0) payload_len = 0;
    int total = 0;

    build_eth(pkt, resolve_dst_mac(dst_ip), ETH_TYPE_IP);
    total += ETH_HDR_LEN;

    IpHeader* ip = (IpHeader*)(pkt + total);
    ip->ver_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_len = 0;
    ip->id = 0;
    ip->flags_frag = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->checksum = 0;
    ip->src_ip = tcp_src_ip;
    ip->dst_ip = dst_ip;
    total += sizeof(IpHeader);

    uint16_t udp_len = (uint16_t)(8 + payload_len);
    uint8_t* u = pkt + total;
    uint16_t sport = udp_src_port_next++;
    // Header is built as BE bytes on the wire.
    u[0] = (uint8_t)(sport >> 8);   u[1] = (uint8_t)sport;
    u[2] = (uint8_t)(dst_port >> 8); u[3] = (uint8_t)dst_port;
    u[4] = (uint8_t)(udp_len >> 8); u[5] = (uint8_t)udp_len;
    u[6] = 0; u[7] = 0;                        // checksum (below)
    for (int i = 0; i < payload_len; i++)
        u[8 + i] = ((const uint8_t*)payload)[i];
    total += 8 + payload_len;

    ip->total_len = htons(sizeof(IpHeader) + udp_len);
    ip->checksum = net_checksum(ip, sizeof(IpHeader), 0);

    // UDP checksum over the pseudo-header + datagram (LE-word folding,
    // identical to the TCP path).
    uint32_t csum = 0;
    uint16_t pseudo[6] = {
        (uint16_t)tcp_src_ip, (uint16_t)(tcp_src_ip >> 16),
        (uint16_t)dst_ip, (uint16_t)(dst_ip >> 16),
        htons(IP_PROTO_UDP), htons(udp_len)
    };
    for (int i = 0; i < 6; i++) csum += pseudo[i];
    uint16_t ck = net_checksum(u, 8 + payload_len, csum);
    if (ck != 0) {                              // 0 means "no checksum"
        u[6] = (uint8_t)ck;
        u[7] = (uint8_t)(ck >> 8);
    }
    return nic_send(pkt, total);
}

int netstack_init(void) {
    uint8_t mac[6];
    nic_get_mac(mac);
    netstack_set_mac(mac);
    gw_resolved = 0;
    send_arp_request(tcp_gateway);
    for (int i = 0; i < 200; i++) netstack_poll();
    return 1;
}
