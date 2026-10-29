// cpu/dns.c — Minimal DNS resolver
//
// Sends a DNS A-record query via UDP to the configured DNS server
// (default: QEMU's DNS proxy 10.0.2.3:53, configurable at runtime)
// and parses the response to extract the resolved IPv4 address.
// Blocks until resolution completes or times out.

#include <dns.h>
#include <netstack.h>
#include <nic.h>
#include <io.h>
#include <stdint.h>

// ─── DNS header (12 bytes) ──────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;     // 0x0100 = standard query, recursion desired
    uint16_t qdcount;   // questions
    uint16_t ancount;   // answers
    uint16_t nscount;
    uint16_t arcount;
} DnsHeader;

// ─── DNS question footer (type + class) ─────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t qtype;     // 1 = A record
    uint16_t qclass;    // 1 = IN (Internet)
} DnsQuestionFooter;

// ─── DNS answer (name compressed) ───────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t name_ptr;  // 0xC00C = pointer to name in query
    uint16_t type;      // 1 = A record
    uint16_t class_;
    uint32_t ttl;
    uint16_t rdlength;  // 4 for IPv4
    uint32_t rdata;     // The IP address (network byte order)
} DnsAnswer;

// ─── Network byte order helper ───────────────────────────────────
static uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }

// ─── Buffer for DNS packets ─────────────────────────────────────
#define DNS_BUF_SIZE 512

// ─── Last error message (visible to browser status bar) ─────────
static char dns_last_error[64];

const char* dns_get_last_error(void) {
    return dns_last_error;
}

static void dns_set_error(const char* msg) {
    int i = 0;
    while (msg[i] && i < 63) { dns_last_error[i] = msg[i]; i++; }
    dns_last_error[i] = '\0';
}

// ─── Encode a hostname into DNS label format ────────────────────
// e.g. "example.com" → "\x07example\x03com\x00"
// Returns the number of bytes written.
static int encode_name(uint8_t* buf, int max, const char* name) {
    int pos = 0;
    int seg_start = 0;  // start of current segment in source
    int seg_len = 0;

    for (int i = 0; ; i++) {
        char c = name[i];
        if (c == '.' || c == '\0') {
            if (pos + seg_len + 1 >= max) return 0;
            buf[pos++] = (uint8_t)seg_len;
            for (int j = 0; j < seg_len; j++)
                buf[pos++] = (uint8_t)name[seg_start + j];
            seg_start = i + 1;
            seg_len = 0;
            if (c == '\0') {
                if (pos >= max) return 0;
                buf[pos++] = 0; // root label
                break;
            }
        } else {
            seg_len++;
        }
    }
    return pos;
}

// ─── DNS query ID (increments each call) ────────────────────────
static uint16_t dns_query_id = 1;

// ─── UDP send helper ────────────────────────────────────────────
// Minimal: builds UDP/IP packet and sends it. Uses the same IP
// addressing conventions as the rest of netstack.
// The dest_ip is in LE wire format.
static int udp_send_dns(uint32_t dst_ip, uint16_t dst_port,
                         const void* payload, int payload_len) {
    // The DNS query is a plain UDP datagram — the stack's generic
    // UDP send builds the full ethernet/IP/UDP envelope.
    return udp_send(dst_ip, dst_port, payload, payload_len);
}

// ─── DNS response buffer (shared, filled by IP handler) ─────────
static uint8_t  dns_resp_buf[DNS_BUF_SIZE];
static int      dns_resp_len = 0;
static int      dns_pending = 0;  // 1 = waiting for response
static uint16_t dns_pending_id = 0;

// ─── Called from netstack_poll when a UDP packet arrives ─────────
void dns_handle_udp(const uint8_t* payload, int len, uint16_t dst_port) {
    (void)dst_port;
    if (!dns_pending) return;
    if (len < 12) return;

    const DnsHeader* hdr = (const DnsHeader*)payload;
    // The reply echoes the query's ID bytes (big-endian on the wire).
    // hdr->id reads them back little-endian (0x0100 for qid=1), so
    // compare in the same domain as dns_pending_id (host order).
    if (htons(hdr->id) != dns_pending_id) return;

    // Copy the response
    int cp = len;
    if (cp > DNS_BUF_SIZE) cp = DNS_BUF_SIZE;
    for (int i = 0; i < cp; i++) dns_resp_buf[i] = payload[i];
    dns_resp_len = cp;
}

// ─── Public: resolve a hostname ─────────────────────────────────
uint32_t dns_resolve(const char* hostname) {
    uint8_t buf[DNS_BUF_SIZE];
    int pos = 0;

    dns_last_error[0] = '\0';

    // Build DNS query
    DnsHeader* hdr = (DnsHeader*)(buf + pos);
    uint16_t qid = dns_query_id++;
    // ID is big-endian on the wire; write it in network order so the
    // echoed reply (read back little-endian on x86) equals dns_pending_id.
    // Writing it host-order sent 01 00 for qid=1, slirp echoed it, and
    // dns_handle_udp's LE read compared 0x0100 != 0x0001 → every reply
    // was silently dropped ("DNS timeout").
    hdr->id = htons(qid);
    hdr->flags = htons(0x0100); // standard query, recursion desired
    hdr->qdcount = htons(1);    // 1 question
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;
    pos += sizeof(DnsHeader);

    // Encode hostname
    int elen = encode_name(buf + pos, DNS_BUF_SIZE - pos - 4, hostname);
    if (elen <= 0) {
        dns_set_error("Bad hostname encoding");
        return 0;
    }
    pos += elen;

    // Question type + class
    DnsQuestionFooter* qf = (DnsQuestionFooter*)(buf + pos);
    qf->qtype = htons(1);   // A record
    qf->qclass = htons(1);  // IN
    pos += sizeof(DnsQuestionFooter);

    // Send via UDP
    dns_pending = 1;
    dns_pending_id = qid;
    dns_resp_len = 0;

    // Send 3 DNS queries with delays for reliability
    uint32_t dns_srv = netstack_get_dns();
    for (int retry = 0; retry < 3 && dns_resp_len == 0; retry++) {
        if (udp_send_dns(dns_srv, DNS_SERVER_PORT, buf, pos) < 0) {
            dns_set_error("NIC TX failed");
            dns_pending = 0;
            return 0;
        }

        // Wait up to ~1 second per retry using tick (PIT_TARGET_HZ = 50 Hz)
        extern volatile uint32_t tick;
        uint32_t deadline = tick + 50;
        while (dns_resp_len == 0 && (int)(tick - deadline) < 0) {
            for (int p = 0; p < 100; p++) netstack_poll();
        }
    }

    dns_pending = 0;

    if (dns_resp_len == 0) {
        dns_set_error("DNS timeout (no response)");
        return 0;
    }

    if (dns_resp_len < (int)(sizeof(DnsHeader) + 4 + sizeof(DnsAnswer))) {
        dns_set_error("Response too short");
        return 0;
    }

    // Parse response
    const DnsHeader* rhdr = (const DnsHeader*)dns_resp_buf;
    uint16_t flags = htons(rhdr->flags);
    if (flags & 0x000F) {
        dns_set_error("DNS server error");
        return 0; // RCODE != 0 means error
    }

    uint16_t ancount = htons(rhdr->ancount);
    if (ancount == 0) {
        dns_set_error("DNS: no answers");
        return 0;
    }

    // Skip the question section to reach answers
    int rpos = sizeof(DnsHeader);
    // Skip one question (name + 4 bytes)
    while (rpos < dns_resp_len) {
        if (dns_resp_buf[rpos] == 0) { rpos++; break; } // root label
        if ((dns_resp_buf[rpos] & 0xC0) == 0xC0) { // compressed name
            rpos += 2;
            break;
        }
        rpos += dns_resp_buf[rpos] + 1;
    }
    rpos += 4; // skip qtype + qclass

    // Scan every answer (and then the additional section) for the
    // first A record.  Real servers answer with CNAME chains first
    // (e.g. en.wikipedia.org → dyna.wikimedia.org), so parsing just
    // the first record would fail with "not A record".  Each RR:
    //   name (compressed ptr or labels) + type(2) + class(2) +
    //   ttl(4) + rdlength(2) + rdata(rdlength)
    int an_total = htons(rhdr->ancount) + htons(rhdr->nscount) +
                   htons(rhdr->arcount);
    for (int rr = 0; rr < an_total && rpos + 4 < dns_resp_len; rr++) {
        // Skip the RR name (compressed pointer or label chain).
        if ((dns_resp_buf[rpos] & 0xC0) == 0xC0) {
            rpos += 2;
        } else {
            while (rpos < dns_resp_len && dns_resp_buf[rpos] != 0) {
                if ((dns_resp_buf[rpos] & 0xC0) == 0xC0) { rpos += 2; break; }
                rpos += dns_resp_buf[rpos] + 1;
            }
            if (rpos < dns_resp_len) rpos++;   // root label
        }
        if (rpos + 10 > dns_resp_len) {
            dns_set_error("DNS: answer body fail");
            return 0;
        }
        uint16_t atype  = (uint16_t)((dns_resp_buf[rpos]     << 8) | dns_resp_buf[rpos+1]);
        uint16_t aclass = (uint16_t)((dns_resp_buf[rpos+2]   << 8) | dns_resp_buf[rpos+3]);
        uint16_t ardlen = (uint16_t)((dns_resp_buf[rpos+8]   << 8) | dns_resp_buf[rpos+9]);
        if (atype == 1 && aclass == 1 && ardlen == 4 &&
            rpos + 10 + 4 <= dns_resp_len) {
            uint32_t ardata = (uint32_t)dns_resp_buf[rpos+10] |
                              ((uint32_t)dns_resp_buf[rpos+11] << 8) |
                              ((uint32_t)dns_resp_buf[rpos+12] << 16) |
                              ((uint32_t)dns_resp_buf[rpos+13] << 24);
            // The stack stores IPs in LE wire format: the uint32's LSB
            // is the first octet on the wire (AC 42 93 F3 for
            // 172.66.147.243 → 0xF39342AC) — already this convention.
            return ardata;
        }
        // Not an A record (CNAME etc.): skip its rdata and continue.
        rpos += 10 + ardlen;
    }

    dns_set_error("DNS: no A record");
    return 0;
}
