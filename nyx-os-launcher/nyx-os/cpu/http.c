// cpu/http.c — Minimal HTTP/1.1 client
//
// Sends an HTTP GET request over TCP (or TLS) and collects the
// response. Designed for the Nyx OS minimal TCP/IP stack.
// This is a blocking implementation — it polls the NIC in a loop
// until the response arrives.
//
// Beyond the minimal client it also handles:
//   - Redirect following: 301/302/303/307/308 + Location header,
//     up to 5 hops. Absolute URLs (http://host[:port]/path) re-resolve
//     DNS; relative Locations are resolved against the current path.
//     Redirects may switch schemes: https:// targets are fetched with
//     the in-kernel TLS client, so plain http pages that bounce to
//     https (example.com, wikipedia) work end to end.
//   - gzip Content-Encoding: the body is inflated with pngdec's
//     DEFLATE decompressor (single-member gzip streams).

#include <http.h>
#include <netstack.h>
#include <dns.h>
#include <pngdec.h>
#include <tls.h>
#include <io.h>
#include <stdint.h>

// ─── Static receive buffer (BSS, not stack) ───────────────────────
// The raw receive buffer is kept in BSS so it doesn't eat the
// kernel stack. HTTP_MAX_RESPONSE must match the definition in http.h.
static char http_raw_buf[HTTP_MAX_RESPONSE];

// gzip inflate scratch (BSS): the raw gzip bytes land in resp->body,
// are inflated into here, then copied back. Kept off the kernel stack.
static char http_gz_buf[HTTP_MAX_RESPONSE];

// ─── Static HTTP response (BSS, not stack) — browser.c uses this
static HttpResponse http_global_resp;

HttpResponse* http_get_global_response(void) {
    http_global_resp.status_code = 0;
    http_global_resp.status_text[0] = '\0';
    http_global_resp.body[0] = '\0';
    http_global_resp.body_len = 0;
    return &http_global_resp;
}

// ─── String helpers ───────────────────────────────────────────────
static int s_len(const char* s) { int i=0; while(s[i]) i++; return i; }
static int s_ncmp(const char* a, const char* b, int n) {
    for (int i=0; i<n; i++) if (a[i] != b[i]) return 1;
    return 0;
}
static int s_find(const char* haystack, const char* needle) {
    int hl = s_len(haystack);
    int nl = s_len(needle);
    if (nl == 0 || nl > hl) return -1;
    for (int i = 0; i <= hl - nl; i++) {
        if (s_ncmp(haystack + i, needle, nl) == 0) return i;
    }
    return -1;
}

// Case-insensitive find of 'needle' within haystack[0..max).
static int s_ifind(const char* haystack, int max, const char* needle) {
    int nl = s_len(needle);
    if (nl == 0 || nl > max) return -1;
    for (int i = 0; i <= max - nl; i++) {
        int m = 1;
        for (int j = 0; j < nl; j++) {
            char a = haystack[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { m = 0; break; }
        }
        if (m) return i;
    }
    return -1;
}

// Parse a dotted-quad IP "a.b.c.d" into the codebase's LE wire format
// (host 10.0.2.2 → 0x0202000A). Returns 0 on failure.
static uint32_t http_parse_dotted_ip(const char* s) {
    uint32_t ip = 0;
    int oct = 0, val = 0, have = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') { val = val * 10 + (c - '0'); have = 1; }
        else if (c == '.' && have) {
            if (oct >= 3 || val > 255) return 0;
            ip |= (uint32_t)val << (8 * oct);
            oct++; val = 0; have = 0;
        } else {
            return 0;
        }
    }
    if (oct != 3 || !have || val > 255) return 0;
    ip |= (uint32_t)val << 24;
    return ip;
}

// Resolve a relative URL 'rel' against the absolute path 'base'
// (which starts with '/'). Writes into 'out'.
static void http_resolve_path(const char* base, const char* rel,
                              char* out, int out_max) {
    if (rel[0] == '/') {
        int i = 0;
        while (rel[i] && i < out_max - 1) { out[i] = rel[i]; i++; }
        out[i] = '\0';
        return;
    }
    // Strip base's last segment but keep the trailing '/'
    int bl = s_len(base);
    int cut = bl;
    while (cut > 0 && base[cut - 1] != '/') cut--;
    int o = 0;
    for (int i = 0; i < cut && o < out_max - 1; i++) out[o++] = base[i];
    for (int i = 0; rel[i] && o < out_max - 1; i++) out[o++] = rel[i];
    out[o] = '\0';
}

// Find the Location header value in the header block [0, hdr_end).
// Returns 1 and copies the value into out on success, 0 if absent.
static int http_find_location(char* out, int out_max, int hdr_end) {
    int i = s_ifind(http_raw_buf, hdr_end, "location:");
    if (i < 0) return 0;
    int j = i + 9;  // skip "location:"
    while (j < hdr_end && (http_raw_buf[j] == ' ' || http_raw_buf[j] == '\t'))
        j++;
    int o = 0;
    while (j < hdr_end && http_raw_buf[j] != '\r' && http_raw_buf[j] != '\n' &&
           o < out_max - 1)
        out[o++] = http_raw_buf[j++];
    // Trim trailing whitespace
    while (o > 0 && (out[o-1] == ' ' || out[o-1] == '\t')) o--;
    out[o] = '\0';
    return out[0] != '\0';
}

// ─── Transport abstraction (TCP vs TLS) ───────────────────────────
typedef struct {
    int (*connect)(uint32_t ip, uint16_t port, const char* host);
    int (*send)(const void* data, int len);
    int (*recv)(void* buf, int max_len);
    void (*close)(void);
    int  (*closed)(void);
} HttpTrans;

static int tr_tcp_connect(uint32_t ip, uint16_t port, const char* host) {
    (void)host;
    return tcp_connect(ip, port);
}
static int tr_tcp_send(const void* d, int l) { return tcp_send(d, l); }
static int tr_tcp_recv(void* b, int m) { return tcp_recv(b, m); }
static void tr_tcp_close(void) { tcp_close(); }
static int tr_tcp_closed(void) {
    int s = tcp_get_state();
    return s == TCP_CLOSED || s == TCP_CLOSE_WAIT;
}

static int tr_tls_connect(uint32_t ip, uint16_t port, const char* host) {
    return tls_connect(ip, port, host);
}
static int tr_tls_send(const void* d, int l) { return tls_send(d, l); }
static int tr_tls_recv(void* b, int m) { return tls_recv(b, m); }
static void tr_tls_close(void) { tls_close(); }
static int tr_tls_closed(void) { return !tls_is_connected(); }

static void http_pick_transport(int use_tls, HttpTrans* t) {
    if (use_tls) {
        t->connect = tr_tls_connect;
        t->send    = tr_tls_send;
        t->recv    = tr_tls_recv;
        t->close   = tr_tls_close;
        t->closed  = tr_tls_closed;
    } else {
        t->connect = tr_tcp_connect;
        t->send    = tr_tcp_send;
        t->recv    = tr_tcp_recv;
        t->close   = tr_tcp_close;
        t->closed  = tr_tcp_closed;
    }
}

// ─── HTTP GET (shared core) ───────────────────────────────────────
// use_tls selects the initial transport; redirects may switch it.
static int http_get_inner(int use_tls, uint32_t ip, uint16_t port,
                          const char* host, const char* path,
                          HttpResponse* resp) {
    if (!resp) return -1;

    // Mutable request parameters (redirects update these)
    uint32_t cur_ip = ip;
    uint16_t cur_port = port;
    const char* cur_host = host;
    const char* cur_path = path;

    // Redirect targets persist across loop iterations
    char redirect_host[256];
    char redirect_path[512];

    for (int redirects = 0; redirects < 5; redirects++) {
        resp->status_code = 0;
        resp->status_text[0] = '\0';
        resp->location[0] = '\0';
        resp->body[0] = '\0';
        resp->body_len = 0;

        HttpTrans tr;
        http_pick_transport(use_tls, &tr);

        // Connect to server (TLS does the full handshake here)
        if (tr.connect(cur_ip, cur_port, cur_host) < 0) {
            return -1;
        }

        // Build HTTP GET request
        char request[1024];
        int rp = 0;

        // "GET /path HTTP/1.1\r\n"
        const char* get_prefix = "GET ";
        while (*get_prefix) request[rp++] = *get_prefix++;

        const char* pp = cur_path;
        while (*pp && rp < 1000) request[rp++] = *pp++;

        const char* http_ver = " HTTP/1.1\r\n";
        while (*http_ver) request[rp++] = *http_ver++;

        // "Host: host\r\n"
        const char* host_hdr = "Host: ";
        while (*host_hdr) request[rp++] = *host_hdr++;
        const char* hp = cur_host;
        while (*hp && rp < 1000) request[rp++] = *hp++;

        // A descriptive User-Agent — Wikimedia and other sites refuse
        // requests without one (403 "Please set a user-agent").
        const char* ua_hdr = "\r\nUser-Agent: NyxOS/1.0 (hobbyist OS browser; contact admin@nyx.local)";
        while (*ua_hdr && rp < 1000) request[rp++] = *ua_hdr++;

        const char* conn_hdr = "\r\nConnection: close\r\n\r\n";
        while (*conn_hdr) request[rp++] = *conn_hdr++;
        request[rp] = '\0';

        // Send the request
        int sent = tr.send(request, rp);
        if (sent < 0) { tr.close(); return -1; }

        // Read response — poll until we have headers + some body
        int raw_len = 0;
        int last_len = -1;

        // Response read timeout: a ROLLING idle timeout (~8 s with no
        // progress; tick is PIT_TARGET_HZ = 50 Hz).  Every time new
        // data arrives the clock resets, so a slow-but-flowing 256 KB
        // download never gets cut off mid-transfer — only a genuine
        // 8 s stall aborts the read.
        extern volatile uint32_t tick;
        uint32_t http_deadline = tick + 400;
        while ((int)(tick - http_deadline) < 0) {
            // Poll multiple times per tick for responsiveness.
            // (tls_recv polls internally; the extra polls are harmless.)
            for (int p = 0; p < 50; p++) netstack_poll();

            char chunk[512];
            int chunk_len = tr.recv(chunk, sizeof(chunk) - 1);
            if (chunk_len < 0) break;      // error or connection closed
            if (chunk_len > 0) {
                http_deadline = tick + 400;   // progress → reset the clock
                // Append to raw buffer
                for (int i = 0; i < chunk_len && raw_len < HTTP_MAX_RESPONSE - 1; i++)
                    http_raw_buf[raw_len++] = chunk[i];
                http_raw_buf[raw_len] = '\0';
                last_len = raw_len;
            }

            // Check if we have the full response (connection closed).
            // Only bail when the recv above found NOTHING: a server
            // commonly sends the final data + FIN together, so the
            // connection can report closed while the just-returned
            // record is only partially consumed and more bytes still
            // sit in the TLS/netstack buffers.  Breaking then would
            // truncate the tail of a large response (seen as
            // body_len ~12 KB short of the pcap's unique payload).
            // The FIN is only accepted in-order, so once closed() is
            // true no new data can arrive — whatever remains is
            // already buffered and will drain over the next recvs.
            if (chunk_len == 0 && tr.closed()) break;

            // If we got data but no more is arriving, also break
            if (last_len == raw_len && raw_len > 0 && chunk_len == 0) {
                // Small delay then check again
                for (int d = 0; d < 100; d++) netstack_poll();
            }
        }

        tr.close();

        if (raw_len == 0) { return -1; }

        // Parse response
        // Find "HTTP/1.1 " and read status code
        int status_start = s_find(http_raw_buf, "HTTP/1.1 ");
        if (status_start < 0) {
            status_start = s_find(http_raw_buf, "HTTP/1.0 ");
            if (status_start < 0) return -1;
        }

        status_start += 9; // skip past "HTTP/1.1 "

        // Read status code (3 digits)
        if (raw_len > status_start + 2) {
            int sc = 0;
            for (int i = 0; i < 3 && http_raw_buf[status_start + i] >= '0' &&
                 http_raw_buf[status_start + i] <= '9'; i++)
                sc = sc * 10 + (http_raw_buf[status_start + i] - '0');
            resp->status_code = sc;

            // Read status text (until \r\n)
            int st_pos = status_start + 3;
            while (http_raw_buf[st_pos] == ' ') st_pos++;
            int sti = 0;
            while (http_raw_buf[st_pos] && http_raw_buf[st_pos] != '\r' &&
                   http_raw_buf[st_pos] != '\n' && sti < 63) {
                resp->status_text[sti++] = http_raw_buf[st_pos++];
            }
            resp->status_text[sti] = '\0';
        }

        // Find double CRLF (\r\n\r\n) — that's the end of headers
        int hdr_end = -1;
        for (int i = 0; i < raw_len - 3; i++) {
            if (http_raw_buf[i] == '\r' && http_raw_buf[i+1] == '\n' &&
                http_raw_buf[i+2] == '\r' && http_raw_buf[i+3] == '\n') {
                hdr_end = i + 4;
                break;
            }
        }
        // Also try \n\n
        if (hdr_end < 0) {
            for (int i = 0; i < raw_len - 1; i++) {
                if (http_raw_buf[i] == '\n' && http_raw_buf[i+1] == '\n') {
                    hdr_end = i + 2;
                    break;
                }
            }
        }

        if (hdr_end < 0) return -1;

        // Check for Transfer-Encoding: chunked
        int is_chunked = 0;
        int te_max = hdr_end - 16;
        if (te_max < 0) te_max = 0;
        for (int i = 0; i < te_max; i++) {
            if ((http_raw_buf[i] == 't' || http_raw_buf[i] == 'T') &&
                (http_raw_buf[i+1] == 'r' || http_raw_buf[i+1] == 'R') &&
                (http_raw_buf[i+2] == 'a' || http_raw_buf[i+2] == 'A') &&
                (http_raw_buf[i+3] == 'n' || http_raw_buf[i+3] == 'N') &&
                (http_raw_buf[i+4] == 's' || http_raw_buf[i+4] == 'S') &&
                (http_raw_buf[i+5] == 'f' || http_raw_buf[i+5] == 'F') &&
                (http_raw_buf[i+6] == 'e' || http_raw_buf[i+6] == 'E') &&
                (http_raw_buf[i+7] == 'r' || http_raw_buf[i+7] == 'R') &&
                http_raw_buf[i+8] == '-' &&
                (http_raw_buf[i+9] == 'E' || http_raw_buf[i+9] == 'e') &&
                (http_raw_buf[i+10] == 'n' || http_raw_buf[i+10] == 'N') &&
                (http_raw_buf[i+11] == 'c' || http_raw_buf[i+11] == 'C') &&
                (http_raw_buf[i+12] == 'o' || http_raw_buf[i+12] == 'O') &&
                (http_raw_buf[i+13] == 'd' || http_raw_buf[i+13] == 'D') &&
                (http_raw_buf[i+14] == 'i' || http_raw_buf[i+14] == 'I') &&
                (http_raw_buf[i+15] == 'n' || http_raw_buf[i+15] == 'N') &&
                (http_raw_buf[i+16] == 'g' || http_raw_buf[i+16] == 'G')) {
                // Found "Transfer-Encoding", check for "chunked"
                int j = i + 17;
                while (j < hdr_end && http_raw_buf[j] != ':') j++;
                j++; // skip ":"
                while (j < hdr_end && (http_raw_buf[j] == ' ' || http_raw_buf[j] == '\t')) j++;
                if (j + 6 < hdr_end) {
                    if ((http_raw_buf[j]   == 'c' || http_raw_buf[j]   == 'C') &&
                        (http_raw_buf[j+1] == 'h' || http_raw_buf[j+1] == 'H') &&
                        (http_raw_buf[j+2] == 'u' || http_raw_buf[j+2] == 'U') &&
                        (http_raw_buf[j+3] == 'n' || http_raw_buf[j+3] == 'N') &&
                        (http_raw_buf[j+4] == 'k' || http_raw_buf[j+4] == 'K') &&
                        (http_raw_buf[j+5] == 'e' || http_raw_buf[j+5] == 'E') &&
                        (http_raw_buf[j+6] == 'd' || http_raw_buf[j+6] == 'D')) {
                        is_chunked = 1;
                    }
                }
                break;
            }
        }

        // Read Content-Length if present
        int content_len = -1;
        for (int i = 0; i < hdr_end - 4; i++) {
            if ((http_raw_buf[i] == 'c' || http_raw_buf[i] == 'C') &&
                (http_raw_buf[i+1] == 'o' || http_raw_buf[i+1] == 'O') &&
                (http_raw_buf[i+2] == 'n' || http_raw_buf[i+2] == 'N') &&
                (http_raw_buf[i+3] == 't' || http_raw_buf[i+3] == 'T') &&
                (http_raw_buf[i+4] == 'e' || http_raw_buf[i+4] == 'E') &&
                (http_raw_buf[i+5] == 'n' || http_raw_buf[i+5] == 'N') &&
                (http_raw_buf[i+6] == 't' || http_raw_buf[i+6] == 'T') &&
                http_raw_buf[i+7] == '-') {
                // Found "Content-", skip to ":"
                int j = i + 8;
                while (j < hdr_end && http_raw_buf[j] != ':') j++;
                j++; // skip ":"
                while (j < hdr_end && http_raw_buf[j] == ' ') j++;
                // Read number
                content_len = 0;
                while (j < hdr_end && http_raw_buf[j] >= '0' && http_raw_buf[j] <= '9') {
                    content_len = content_len * 10 + (http_raw_buf[j] - '0');
                    j++;
                }
                break;
            }
        }

        // Copy body — handle chunked transfer encoding if present
        if (is_chunked) {
            // Parse chunked body: strip hex size lines and trailing CRLFs.
            // Each chunk: hex_size\r\n <data> \r\n
            // End marker: 0\r\n\r\n
            int src = hdr_end;
            int dst = 0;
            while (src < raw_len) {
                // Read hex chunk size
                int chunk_size = 0;
                int got_size = 0;
                while (src < raw_len && http_raw_buf[src] != '\r' && http_raw_buf[src] != '\n') {
                    char c = http_raw_buf[src];
                    chunk_size = chunk_size * 16;
                    if (c >= '0' && c <= '9') chunk_size += (c - '0');
                    else if (c >= 'a' && c <= 'f') chunk_size += (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') chunk_size += (c - 'A' + 10);
                    src++;
                    got_size = 1;
                }
                // Skip \r\n after size
                if (src < raw_len && http_raw_buf[src] == '\r') src++;
                if (src < raw_len && http_raw_buf[src] == '\n') src++;

                if (!got_size || chunk_size == 0) break;  // last chunk

                // Copy chunk data
                int copy_max = HTTP_MAX_RESPONSE - 1 - dst;
                if (copy_max > chunk_size) copy_max = chunk_size;
                if (copy_max > raw_len - src) copy_max = raw_len - src;
                for (int i = 0; i < copy_max; i++)
                    resp->body[dst++] = http_raw_buf[src++];
                // Skip trailing \r\n after chunk data
                if (src < raw_len && http_raw_buf[src] == '\r') src++;
                if (src < raw_len && http_raw_buf[src] == '\n') src++;
            }
            resp->body[dst] = '\0';
            resp->body_len = dst;
        } else {
            // Non-chunked: use Content-Length if available, else copy all
            int body_start = hdr_end;
            int body_avail = raw_len - body_start;
            if (body_avail < 0) body_avail = 0;

            int body_copy = body_avail;
            if (content_len > 0 && content_len < body_copy)
                body_copy = content_len;

            if (body_copy > HTTP_MAX_RESPONSE - 1)
                body_copy = HTTP_MAX_RESPONSE - 1;

            for (int i = 0; i < body_copy; i++)
                resp->body[i] = http_raw_buf[body_start + i];
            resp->body[body_copy] = '\0';
            resp->body_len = body_copy;
        }

        // ─── Content-Encoding: gzip ──────────────────────────────
        // The body (already de-chunked) holds the raw gzip bytes.
        // Inflate them into http_gz_buf and copy back.
        int is_gzip = 0;
        int ge = s_ifind(http_raw_buf, hdr_end, "content-encoding:");
        if (ge >= 0) {
            int j = ge + 17;  // skip "content-encoding:"
            while (j < hdr_end && (http_raw_buf[j] == ' ' || http_raw_buf[j] == '\t'))
                j++;
            if (j + 4 <= hdr_end &&
                (http_raw_buf[j]   == 'g' || http_raw_buf[j]   == 'G') &&
                (http_raw_buf[j+1] == 'z' || http_raw_buf[j+1] == 'Z') &&
                (http_raw_buf[j+2] == 'i' || http_raw_buf[j+2] == 'I') &&
                (http_raw_buf[j+3] == 'p' || http_raw_buf[j+3] == 'P'))
                is_gzip = 1;
        }

        if (is_gzip && resp->body_len >= 18) {
            const uint8_t* gz = (const uint8_t*)resp->body;
            int gz_len = resp->body_len;
            // Fixed gzip header: 1F 8B 08 FLG MTIME(4) XFL OS
            if (gz[0] == 0x1F && gz[1] == 0x8B && gz[2] == 8) {
                int h = 10;
                uint8_t flg = gz[3];
                if (flg & 0x04) {  // FEXTRA
                    int xlen = gz[h] | (gz[h + 1] << 8);
                    h += 2 + xlen;
                }
                if (flg & 0x08) { while (h < gz_len && gz[h]) h++; h++; }  // FNAME
                if (flg & 0x10) { while (h < gz_len && gz[h]) h++; h++; }  // FCOMMENT
                if (flg & 0x02) h += 2;  // FHCRC
                if (h < gz_len) {
                    int dec = png_inflate(gz + h, gz_len - h,
                                          (uint8_t*)http_gz_buf,
                                          HTTP_MAX_RESPONSE - 1);
                    if (dec > 0 && dec <= HTTP_MAX_RESPONSE - 1) {
                        // ISIZE trailer (last 4 bytes, LE) is the expected
                        // uncompressed size — use it to detect truncation.
                        int trailer = gz_len - 8;
                        if (trailer >= h) {
                            uint32_t isize = (uint32_t)gz[trailer] |
                                             ((uint32_t)gz[trailer+1] << 8) |
                                             ((uint32_t)gz[trailer+2] << 16) |
                                             ((uint32_t)gz[trailer+3] << 24);
                            if ((uint32_t)dec > isize && isize < (uint32_t)HTTP_MAX_RESPONSE)
                                dec = (int)isize;
                        }
                        for (int i = 0; i < dec; i++)
                            resp->body[i] = http_gz_buf[i];
                        resp->body[dec] = '\0';
                        resp->body_len = dec;
                    }
                }
            }
        }

        // ─── Redirect following ──────────────────────────────────
        if (resp->status_code >= 300 && resp->status_code < 400) {
            char loc[512];
            if (!http_find_location(loc, sizeof(loc), hdr_end)) {
                resp->location[0] = '\0';
                return 0;  // 3xx without Location: return as-is
            }

            // Surface the Location to the caller (for a friendly
            // "redirected" message when we can't follow it).
            { int li = 0;
              while (loc[li] && li < 255) { resp->location[li] = loc[li]; li++; }
              resp->location[li] = '\0'; }

            int is_https = (loc[0]=='h'||loc[0]=='H') && (loc[1]=='t'||loc[1]=='T') &&
                           (loc[2]=='t'||loc[2]=='T') && (loc[3]=='p'||loc[3]=='P') &&
                           (loc[4]=='s'||loc[4]=='S') && loc[5]==':' &&
                           loc[6]=='/' && loc[7]=='/';
            int is_http  = (loc[0]=='h'||loc[0]=='H') && (loc[1]=='t'||loc[1]=='T') &&
                           (loc[2]=='t'||loc[2]=='T') && (loc[3]=='p'||loc[3]=='P') &&
                           loc[4]==':' && loc[5]=='/' && loc[6]=='/';

            if (!is_https && !is_http) {
                // Relative Location: resolve against the current path
                http_resolve_path(cur_path, loc, redirect_path, sizeof(redirect_path));
                cur_path = redirect_path;
                continue;
            }

            // Absolute URL: scheme://host[:port]/path — switch the
            // transport when the scheme flips.
            use_tls = is_https;

            const char* p = loc + (is_https ? 8 : 7);
            char newhost[256];
            int nh = 0;
            while (p[nh] && p[nh] != '/' && p[nh] != ':' && nh < 255) {
                newhost[nh] = p[nh];
                nh++;
            }
            newhost[nh] = '\0';

            uint16_t newport = is_https ? 443 : 80;
            if (p[nh] == ':') {
                int portv = 0;
                nh++;
                while (p[nh] >= '0' && p[nh] <= '9' && portv < 65535) {
                    portv = portv * 10 + (p[nh] - '0');
                    nh++;
                }
                if (portv > 0) newport = (uint16_t)portv;
            }

            const char* np = p + nh;
            if (!np[0]) np = "/";
            int pi = 0;
            while (np[pi] && pi < 511) { redirect_path[pi] = np[pi]; pi++; }
            redirect_path[pi] = '\0';

            int hi = 0;
            while (newhost[hi] && hi < 255) {
                redirect_host[hi] = newhost[hi];
                hi++;
            }
            redirect_host[hi] = '\0';

            cur_path = redirect_path;
            cur_host = redirect_host;
            cur_port = newport;

            // Resolve the new host (dotted IP or DNS)
            uint32_t nip = http_parse_dotted_ip(newhost);
            if (!nip) {
                asm volatile("sti");
                nip = dns_resolve(newhost);
                asm volatile("cli");
            }
            if (nip) cur_ip = nip;  // else keep old IP (best effort)
            continue;
        }

        return 0;
    }

    return -1;  // too many redirects
}

// ─── Public entry points ──────────────────────────────────────────
int http_get(uint32_t ip, uint16_t port, const char* host, const char* path,
             HttpResponse* resp) {
    return http_get_inner(0, ip, port, host, path, resp);
}

int http_get_secure(uint32_t ip, const char* host, const char* path,
                    HttpResponse* resp) {
    return http_get_inner(1, ip, 443, host, path, resp);
}
