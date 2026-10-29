// cpu/browser.c — Nyx OS Web Browser (desktop app)
//
// A full-screen web browser with an address bar, navigation buttons
// (Back/Forward), loading spinner, and integration with the existing
// HTTP client, DNS resolver, and HTML viewer.
//
// The browser draws its chrome (address bar + status bar) as an
// overlay at the top of htmlview's panel. When the user navigates
// to a URL, the browser resolves DNS, performs an HTTP GET, and
// opens the response in the HTML viewer below the chrome.

#include <browser.h>
#include <vesa.h>
#include <stdint.h>
#include <stddef.h>
#include <clipboard.h>
#include <http.h>
#include <dns.h>
#include <htmlview.h>
#include <netstack.h>
#include <nic.h>
#include <tls.h>
#include <isr.h>
#include <desktop.h>
#include <mouse.h>
#include <io.h>

// ─── Browser state ───────────────────────────────────────────────
static int br_active = 0;

// URL bar
static char br_url[256];
static int  br_url_pos = 0;
static int  br_focus_addr = 1;  // 1 = address bar focused, 0 = page content

// Navigation history (single-level back/forward)
static char br_current_url[256];
static char br_prev_url[256];
static int  br_has_back = 0;
static char br_next_url[256];
static int  br_has_forward = 0;

// Status
static char br_status[64];
static int  br_loading = 0;

// ─── Image loading state ─────────────────────────────────────────
// After a page renders, its <img> slots are fetched one per heartbeat
// call (each fetch is a blocking DNS + TLS + transfer, so doing them
// all at once would freeze the desktop for minutes).  br_img_next is
// the next slot to try; a slot stays pending until decoded or failed.
static int br_img_active = 0;
static int br_img_next = 0;

// ─── Chrome layout constants ─────────────────────────────────────
// The address bar sits at the very top of the screen, overlaying
// htmlview's own title bar. The status bar sits directly below it.
#define BR_ADDR_Y      0
#define BR_ADDR_H      40
#define BR_STATUS_Y    (BR_ADDR_Y + BR_ADDR_H)
#define BR_STATUS_H    24
#define BR_CHROME_H    (BR_ADDR_H + BR_STATUS_H)

// ─── Built-in welcome page (shown on first launch) ──────────────
// A simple HTML page that's displayed when the browser starts up
// with no URL loaded. This gives the user something to see even
// without a network connection.
static const char* br_welcome_html =
    "<html><head><title>Welcome to Nyx Browser</title></head><body>"
    "<h1>\xF0 Nyx Web Browser</h1>"
    "<p>Welcome to the Nyx OS web browser. This lightweight browser "
    "renders basic HTML and supports navigation via the address bar "
    "above.</p>"
    "<h2>Getting Started</h2>"
    "<p>Type a URL in the address bar at the top and press ENTER to "
    "navigate. For example, try visiting:</p>"
    "<ul>"
    "  <li><b>example.com</b> — a simple test page</li>"
    "  <li><b>info.cern.ch</b> — the first web page</li>"
    "  <li>An IP address like <b>93.184.216.34</b> for a direct connection</li>"
    "</ul>"
    "<h2>Features</h2>"
    "<ul>"
    "  <li><b>HTML rendering</b>: headings, paragraphs, links, lists, bold/italic</li>"
    "  <li><b>Link navigation</b>: press TAB to cycle links, ENTER to follow</li>"
    "  <li><b>History</b>: press Z for back, X for forward</li>"
    "  <li><b>Scroll</b>: arrow keys to scroll the page</li>"
    "</ul>"
    "<h2>Network Setup</h2>"
    "<p>If pages don't load, open the Terminal app and run:</p>"
    "<pre>  http init\n  net config dhcp</pre>"
    "<p>This initialises the network adapter and auto-configures IP "
    "via DHCP. Then try the browser again.</p>"
    "<hr>"
    "<p><i>Nyx Browser v0.1 — Built for Nyx OS</i></p>"
    "</body></html>";

// ─── String helpers ──────────────────────────────────────────────
static int br_strlen(const char* s) {
    int i = 0; while (s[i]) i++; return i;
}
static void br_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static void br_cat_status(const char* s) {
    int sl = br_strlen(br_status);
    int si = 0;
    while (s[si] && sl < 63) { br_status[sl++] = s[si++]; }
    br_status[sl] = '\0';
}

// ─── URL parsing ─────────────────────────────────────────────────
// Extract host and path from a URL string.
// Handles: "example.com", "example.com/path", "http://example.com/path"
// Returns 0 on success, -1 on failure.
static int br_parse_url(const char* url, char* host, int host_max,
                        char* path, int path_max, int* port) {
    if (!url || !url[0]) return -1;
    if (port) *port = 80;

    int pos = 0;

    // Strip http:// or https:// prefix
    if (url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p') {
        if (url[4] == 's') pos = 5;
        else pos = 4;
        // Skip ://
        while (url[pos] && (url[pos] == ':' || url[pos] == '/')) pos++;
    }

    // Extract host (up to first /, :port, or end)
    int hi = 0;
    while (url[pos] && url[pos] != '/' && hi < host_max - 1) {
        if (url[pos] == ':' && port) {
            // host:port — parse the port, host ends at the ':'
            int pv = 0, p = pos + 1;
            while (url[p] >= '0' && url[p] <= '9' && pv < 60000) {
                pv = pv * 10 + (url[p] - '0');
                p++;
            }
            if (pv > 0) *port = pv;
            pos = p;
            break;
        }
        host[hi++] = url[pos++];
    }
    host[hi] = '\0';

    // Extract path (rest of URL, or / if empty)
    int pi = 0;
    if (!url[pos]) {
        if (pi < path_max - 1) { path[pi++] = '/'; path[pi] = '\0'; }
    } else {
        while (url[pos] && pi < path_max - 1) {
            path[pi++] = url[pos++];
        }
        path[pi] = '\0';
    }

    return (host[0] != '\0') ? 0 : -1;
}

// Parse a dotted decimal IP string into a uint32_t.
static uint32_t br_parse_ip(const char* s) {
    uint32_t ip = 0;
    int octet = 0, shift = 0;
    for (int ci = 0; s[ci] && ci < 32; ci++) {
        if (s[ci] >= '0' && s[ci] <= '9')
            octet = octet * 10 + (s[ci] - '0');
        else if (s[ci] == '.') {
            ip |= (octet << shift);
            octet = 0;
            shift += 8;
        }
    }
    ip |= (octet << shift);
    return ip;
}

// True if the URL uses the https scheme (needs the TLS bridge).
static int br_is_https(const char* url) {
    return (url[0]=='h' && url[1]=='t' && url[2]=='t' && url[3]=='p' &&
            url[4]=='s' && url[5]==':');
}

// Check whether a string is a dotted-quad IP address (exactly 3
// dots, only digits and dots).  A hostname like "example.com" has a
// dot but is NOT an IP — it must go through DNS.  (The old check
// returned 1 for any dot, so "example.com" was "parsed" as an IP,
// yielded 0, and every hostname fetch failed silently.)
static int br_is_dotted(const char* s) {
    int dots = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '.') dots++;
        else if (s[i] < '0' || s[i] > '9') return 0;
    }
    return dots == 3;
}

// Resolve a (possibly relative) link href against the current page
// URL, RFC 3986-style, so links like `/wiki/Page`, `page2.html`, or
// `//other.host/x` work instead of being treated as new hostnames:
//   http://h/p   https://h/p   → absolute, copied verbatim
//   //h/p                      → protocol-relative: "http:" + href
//   /p                         → current host + path
//   rel/p                      → current URL's directory + path
// Returns 1 on success (out filled), 0 if no current host to resolve
// against (caller should then treat href as a bare hostname).
static int br_resolve_link(const char* href, char* out, int max) {
    if (!href || !href[0] || max <= 0) return 0;

    // Absolute with scheme → copy verbatim.
    if ((href[0]=='h' && href[1]=='t' && href[2]=='t' && href[3]=='p' &&
         href[4]==':' && href[5]=='/' && href[6]=='/') ||
        (href[0]=='h' && href[1]=='t' && href[2]=='t' && href[3]=='p' &&
         href[4]=='s' && href[5]==':' && href[6]=='/' && href[7]=='/')) {
        int i = 0;
        while (href[i] && i < max - 1) { out[i] = href[i]; i++; }
        out[i] = '\0';
        return 1;
    }

    // Protocol-relative //host/path → http://host/path.
    if (href[0] == '/' && href[1] == '/') {
        int o = 0;
        const char* pfx = "http:";
        for (; *pfx && o < max - 1; pfx++) out[o++] = *pfx;
        int i = 0;
        while (href[i] && o < max - 1) { out[o++] = href[i++]; }
        out[o] = '\0';
        return 1;
    }

    // Host part of the current page URL (scheme stripped, up to '/')
    // and the index where its path begins.
    char host[64];
    int hi = 0, pos = 0;
    const char* cur = br_current_url;
    if (cur[0]=='h' && cur[1]=='t' && cur[2]=='t' && cur[3]=='p') {
        pos = (cur[4] == 's') ? 5 : 4;
        while (cur[pos] && (cur[pos] == ':' || cur[pos] == '/')) pos++;
    }
    while (cur[pos] && cur[pos] != '/' && hi < 63) host[hi++] = cur[pos++];
    host[hi] = '\0';
    if (!host[0]) return 0;                 // no current host to resolve

    int o = 0;
    while (host[o] && o < max - 1) { out[o] = host[o]; o++; }

    if (href[0] == '/') {
        // Root-relative: host + href.
        int i = 0;
        while (href[i] && o < max - 1) { out[o++] = href[i++]; }
        out[o] = '\0';
        return 1;
    }

    // Relative: current directory (path up to and including the last
    // '/') + href.  "example.com" (no path) → "example.com/" prefix.
    int last_slash = -1;
    for (int i = pos; cur[i] && i < 200; i++)
        if (cur[i] == '/') last_slash = i;
    if (last_slash >= pos) {
        for (int i = pos; i <= last_slash && o < max - 1; i++)
            out[o++] = cur[i];
    } else {
        if (o < max - 1) out[o++] = '/';
    }
    int i = 0;
    while (href[i] && o < max - 1) { out[o++] = href[i++]; }
    out[o] = '\0';
    return 1;
}

// ─── Image URL resolution ────────────────────────────────────────
// Resolve an <img> src against the current page URL (RFC 3986-style,
// mirroring br_resolve_link but returning host/path/scheme parts the
// fetcher needs).  Handles absolute http(s)://, protocol-relative
// //host/path, root-relative /path, and relative path.  data: URIs
// and empty srcs are rejected.  Returns 0 on success.
static int br_img_resolve(const char* src, char* host, int host_max,
                          char* path, int path_max, int* secure) {
    if (!src || !src[0]) return -1;
    if (src[0] == 'd' && src[1] == 'a' && src[2] == 't' && src[3] == 'a')
        return -1;   // data: URI — not fetchable

    // Current page base: scheme + host + path directory.
    int page_secure = br_is_https(br_current_url);
    char page_host[64];
    int ph = 0, pos = 0;
    const char* cur = br_current_url;
    if (cur[0]=='h' && cur[1]=='t' && cur[2]=='t' && cur[3]=='p') {
        pos = (cur[4] == 's') ? 5 : 4;
        while (cur[pos] && (cur[pos] == ':' || cur[pos] == '/')) pos++;
    }
    while (cur[pos] && cur[pos] != '/' && ph < 63) page_host[ph++] = cur[pos++];
    page_host[ph] = '\0';
    if (!page_host[0]) return -1;   // no page loaded

    // Build a full URL string.
    char full[512];
    int o = 0;
    int is_abs = (src[0]=='h' && src[1]=='t' && src[2]=='t' && src[3]=='p' &&
                  (src[4]==':' || (src[4]=='s' && src[5]==':')));
    if (is_abs) {
        int i = 0;
        while (src[i] && o < 510) { full[o++] = src[i++]; }
        full[o] = '\0';
    } else if (src[0] == '/' && src[1] == '/') {
        const char* pfx = page_secure ? "https:" : "http:";
        for (; *pfx && o < 510; pfx++) full[o++] = *pfx;
        int i = 0;
        while (src[i] && o < 510) { full[o++] = src[i++]; }
        full[o] = '\0';
    } else {
        // Root-relative or plain relative: same host, resolved path.
        const char* pfx = page_secure ? "https://" : "http://";
        for (; *pfx && o < 510; pfx++) full[o++] = *pfx;
        for (int i = 0; page_host[i] && o < 510; i++) full[o++] = page_host[i];
        int last_slash = -1;
        for (int i = pos; cur[i] && i < 300; i++)
            if (cur[i] == '/') last_slash = i;
        if (src[0] == '/') {
            int i = 0;
            while (src[i] && o < 510) { full[o++] = src[i++]; }
        } else {
            if (last_slash >= pos) {
                for (int i = pos; i <= last_slash && o < 510; i++)
                    full[o++] = cur[i];
            } else {
                if (o < 510) full[o++] = '/';
            }
            int i = 0;
            while (src[i] && o < 510) { full[o++] = src[i++]; }
        }
        full[o] = '\0';
    }

    int port = 80;
    char h[64], p[400];
    if (br_parse_url(full, h, 64, p, 400, &port) < 0) return -1;
    br_strcpy(host, h, host_max);
    br_strcpy(path, p, path_max);
    *secure = br_is_https(full);
    return 0;
}

// ─── Low-level fetch (no history management) ─────────────────────
// Resolves DNS, performs HTTP GET (plain or TLS), and renders HTML.
// Updates the address bar and status but does NOT touch
// br_prev_url / br_next_url / br_has_back / br_has_forward.
// Returns 0 on success, -1 on failure.
// https:// URLs use the in-kernel TLS 1.2 client (ECDHE + GCM); the
// host TLS bridge is only a last-resort fallback when that fails.
static int br_fetch(const char* url, int bridge) {
    char host[64], path[320];
    int port = 80;
    int is_secure = br_is_https(url);
    int via_bridge = (bridge || is_secure);

    if (bridge) {
        // Fallback: route the whole fetch through the host bridge
        // (tools/https_proxy.py), which does TLS + redirect handling.
        // The bridge lives on the host loopback at 10.0.2.2:8443.
        const char* pfx = "/fetch?u=";
        int o = 0;
        for (; *pfx && o < 310; pfx++) path[o++] = *pfx;
        for (int i = 0; url[i] && o < 310; i++) path[o++] = url[i];
        path[o] = '\0';
        br_strcpy(host, "10.0.2.2", 64);
        port = 8443;
    } else if (br_parse_url(url, host, 64, path, 320, &port) < 0) {
        br_strcpy(br_status, "Invalid URL", 64);
        br_loading = 0;
        return -1;
    }

    // Update the address bar
    br_strcpy(br_url, url, 256);
    br_url_pos = br_strlen(url);

    // Quick check: if no NIC was found, bail immediately
    if (!nic_is_up()) {
        br_strcpy(br_status, "No NIC — run 'http init' in Terminal", 64);
        br_loading = 0;
        return -1;
    }

    // Set loading status
    if (via_bridge && !is_secure)
        br_strcpy(br_status, "https via host bridge...", 64);
    else if (is_secure)
        br_strcpy(br_status, "Connecting (TLS)...", 64);
    else
        br_strcpy(br_status, "Connecting...", 64);
    br_loading = 1;

    // [BR TRACE] entering br_fetch
    { const char* m = "BR ENTER\n";
      for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }

    // Resolve IP
    uint32_t ip = 0;
    if (br_is_dotted(host)) {
        ip = br_parse_ip(host);
        br_strcpy(br_status, "Using IP directly...", 64);
    } else {
        br_strcpy(br_status, "Resolving DNS...", 64);
        // [BR TRACE] DNS resolve start
        { const char* m = "BR DNS\n";
          for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
        ip = dns_resolve(host);
    }

    if (ip == 0) {
        // [BR TRACE] DNS failed — print error to serial
        { const char* m = "BR DNS FAIL ";
          for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
        { const char* e = dns_get_last_error();
          for (; *e; e++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*e); } }
        outb(0x3F8, '\n');
        // Show specific DNS failure reason in status bar
        const char* dns_err = dns_get_last_error();
        if (dns_err[0]) {
            br_strcpy(br_status, "DNS: ", 64);
            br_cat_status(dns_err);
        } else {
            br_strcpy(br_status, "DNS failed", 64);
        }
        br_loading = 0;
        return -1;
    }
    // [BR TRACE] DNS OK
    { const char* m = "BR DNS OK\n";
      for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }

    // Update status
    br_strcpy(br_status, "Fetching page...", 64);

    // HTTP GET (use global static response to avoid stack overflow)
    HttpResponse* resp = http_get_global_response();
    int result;
    // [BR TRACE] HTTP GET start
    { const char* m = "BR HTTP\n";
      for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
    if (is_secure)
        result = http_get_secure(ip, host, path, resp);
    else
        result = http_get(ip, (uint16_t)port, host, path, resp);

    br_loading = 0;

    if (result != 0) {
        // https that failed in-kernel: show why, then retry via the
        // host bridge as a last resort.
        if (is_secure && !bridge) {
            const char* terr = tls_last_error();
            br_strcpy(br_status, "TLS: ", 64);
            if (terr[0]) br_cat_status(terr);
            else br_cat_status("handshake failed");
            return br_fetch(url, 1);
        }
        br_strcpy(br_status, "Connection failed", 64);
        { const char* m = "\n";
          for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
        return -1;
    }

    // Serial marker (E2E test hook): browser fetch succeeded — status
    // + URL + first 40 body bytes so tests can assert the exact page.
    // KEEP this one: tools/_browser_https_e2e_test.py waits on it.
    { const char* m = "BR HTTP OK ";
      for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
    { int code = resp->status_code; char nb[6]; int np=0;
      while (code) { nb[np++] = (char)('0' + code%10); code/=10; }
      for (int i=np-1;i>=0;i--){ int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)nb[i]); } }
    { const char* m = " ";
      for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
    { const char* u = url; for (; *u; u++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*u); } }
    { const char* m = " | ";
      for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
    { int bl = resp->body_len; if (bl > 40) bl = 40;
      for (int bi = 0; bi < bl; bi++) {
          char bc = resp->body[bi];
          if (bc == '\n' || bc == '\r' || bc == '\t') bc = ' ';
          if (bc == 0) break;
          int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)bc);
      } }

    { const char* m = " | body_len=";
      for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
    { int v = resp->body_len; char nb[10]; int np=0;
      if (v == 0) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,'0'); }
      while (v) { nb[np++] = (char)('0' + v % 10); v /= 10; }
      for (int i = np - 1; i >= 0; i--) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)nb[i]); } }
    { const char* m = "\n";
      for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }

    // ── Unfollowable redirect (3xx with no Location, or redirect
    //     loop).  Scheme-switching redirects (http→https and back) are
    //     already followed inside http_get via the TLS transport, so
    //     this is a rare edge case — just show a plain message. ─────
    if (resp->status_code >= 300 && resp->status_code < 400) {
        br_status[0] = '\0';
        br_cat_status("HTTP ");
        { int sc = resp->status_code;
          if (sc >= 100) { br_status[br_strlen(br_status)] = '0' + sc/100; sc %= 100; }
          if (sc >= 10)  { br_status[br_strlen(br_status)] = '0' + sc/10; sc %= 10; }
          br_status[br_strlen(br_status)] = '0' + sc; }
        br_cat_status(" — redirect not followed");

        char msg[640];
        int m = 0;
        const char* p1 = "<html><head><title>Redirect</title></head><body>"
                         "<h2>Redirect — cannot follow</h2>"
                         "<p>The server redirected to an address with no "
                         "Location header, so the browser could not follow "
                         "it:</p><p><b>";
        for (; *p1 && m < 620; p1++) msg[m++] = *p1;
        if (resp->location[0]) {
            for (int li = 0; resp->location[li] && m < 620; li++)
                msg[m++] = resp->location[li];
        } else {
            const char* unk = "(unknown)";
            for (; *unk && m < 620; unk++) msg[m++] = *unk;
        }
        const char* p2 = "</b></p></body></html>";
        for (; *p2 && m < 639; p2++) msg[m++] = *p2;
        msg[m] = '\0';

        htmlview_open(msg, "Redirect");
        br_focus_addr = 0;
        return 0;
    }

    // Build status: "HTTP 200 OK (8192 B)"
    br_status[0] = '\0';
    br_cat_status("HTTP ");
    int sc = resp->status_code;
    if (sc >= 100) { br_status[br_strlen(br_status)] = '0' + sc/100; sc %= 100; }
    if (sc >= 10)  { br_status[br_strlen(br_status)] = '0' + sc/10; sc %= 10; }
    br_status[br_strlen(br_status)] = '0' + sc;
    br_cat_status(" ");
    br_cat_status(resp->status_text);
    br_cat_status(" (");

    int cnt = resp->body_len;
    if (cnt >= 999999) cnt = 999999;
    if (cnt >= 100000) { br_status[br_strlen(br_status)] = '0' + cnt/100000; cnt %= 100000; }
    if (cnt >= 10000)  { br_status[br_strlen(br_status)] = '0' + cnt/10000;  cnt %= 10000; }
    if (cnt >= 1000)   { br_status[br_strlen(br_status)] = '0' + cnt/1000;   cnt %= 1000; }
    if (cnt >= 100)    { br_status[br_strlen(br_status)] = '0' + cnt/100;    cnt %= 100; }
    if (cnt >= 10)     { br_status[br_strlen(br_status)] = '0' + cnt/10;     cnt %= 10; }
    br_status[br_strlen(br_status)] = '0' + cnt;
    br_cat_status(" B)");

    // Auto-detect HTML and open in viewer
    int looks_like_html = 0;
    int bl = resp->body_len;
    for (int bi = 0; bi < bl && bi < 80; bi++) {
        if (resp->body[bi] == '<') {
            if ((resp->body[bi+1] >= 'a' && resp->body[bi+1] <= 'z') ||
                (resp->body[bi+1] >= 'A' && resp->body[bi+1] <= 'Z') ||
                resp->body[bi+1] == '!' || resp->body[bi+1] == '/') {
                looks_like_html = 1;
            }
            break;
        }
        if (resp->body[bi] != ' ' && resp->body[bi] != '\t' &&
            resp->body[bi] != '\n' && resp->body[bi] != '\r')
            break;
    }

    if (looks_like_html && bl > 10) {
        if (htmlview_is_open()) htmlview_close();
        htmlview_open(resp->body, host);
        br_focus_addr = 0;  // Focus content so the user can scroll immediately
        // Kick off image loading — slots are fetched one per heartbeat
        // call (see browser_process_pending) so the desktop stays alive.
        br_img_active = (htmlview_img_count() > 0);
        br_img_next = 0;
    } else {
        br_cat_status(" [not HTML]");
    }

    return 0;
}

// ─── Fetch one pending image ─────────────────────────────────────
// Resolves the slot's src against the current page, does DNS + HTTP
// (TLS when the URL is https), decodes the bytes into htmlview, and
// updates the status bar.  Blocking — call from the main loop only,
// one image per heartbeat.  Returns 1 while more slots remain.
static int br_fetch_next_image(void) {
    int n = htmlview_img_count();
    while (br_img_next < n && !htmlview_img_pending(br_img_next))
        br_img_next++;
    if (br_img_next >= n) {
        br_img_next = 0;
        return 0;
    }
    int slot = br_img_next++;

    // Status: "Image 3/5 ..."
    br_status[0] = '\0';
    br_cat_status("Image ");
    { int cn = slot + 1; char nb[6]; int np = 0;
      while (cn) { nb[np++] = (char)('0' + cn % 10); cn /= 10; }
      for (int i = np - 1; i >= 0; i--) br_status[br_strlen(br_status)] = nb[i]; }
    br_cat_status("/");
    { int cn = n; char nb[6]; int np = 0;
      while (cn) { nb[np++] = (char)('0' + cn % 10); cn /= 10; }
      for (int i = np - 1; i >= 0; i--) br_status[br_strlen(br_status)] = nb[i]; }

    const char* src = htmlview_img_get_src(slot);
    char host[64], path[400];
    int secure = 0;
    if (src[0] && br_img_resolve(src, host, 64, path, 400, &secure) == 0) {
        br_cat_status(" ...");
        uint32_t ip = 0;
        if (br_is_dotted(host))
            ip = br_parse_ip(host);
        else
            ip = dns_resolve(host);

        if (ip != 0) {
            HttpResponse* resp = http_get_global_response();
            int result;
            if (secure)
                result = http_get_secure(ip, host, path, resp);
            else
                result = http_get(ip, 80, host, path, resp);
            if (result == 0 && resp->body_len > 4) {
                if (htmlview_img_decode(slot, (const uint8_t*)resp->body,
                                        resp->body_len)) {
                    br_status[0] = '\0';
                    br_cat_status("Image ");
                    { int cn = slot + 1; char nb[6]; int np = 0;
                      while (cn) { nb[np++] = (char)('0' + cn % 10); cn /= 10; }
                      for (int i = np - 1; i >= 0; i--) br_status[br_strlen(br_status)] = nb[i]; }
                    br_cat_status(" loaded");
                }
                // decode failure already marked the slot FAIL
            } else {
                // Fetch failed — mark the slot failed so it shows a box.
                static const uint8_t bad[4] = {0, 0, 0, 0};
                htmlview_img_decode(slot, bad, 4);
                br_status[0] = '\0';
                br_cat_status("Image ");
                { int cn = slot + 1; char nb[6]; int np = 0;
                  while (cn) { nb[np++] = (char)('0' + cn % 10); cn /= 10; }
                  for (int i = np - 1; i >= 0; i--) br_status[br_strlen(br_status)] = nb[i]; }
                br_cat_status(" failed");
            }
        } else {
            static const uint8_t bad[4] = {0, 0, 0, 0};
            htmlview_img_decode(slot, bad, 4);
            br_status[0] = '\0';
            br_cat_status("Image DNS failed");
        }
    } else {
        // Unresolvable src — mark failed so it stops being pending.
        static const uint8_t bad[4] = {0, 0, 0, 0};
        htmlview_img_decode(slot, bad, 4);
        br_status[0] = '\0';
        br_cat_status("Image skipped");
    }

    // Advance past any slots the decode/fail loop just resolved.
    return br_img_next < n;
}

// ─── Deferred navigation (process in main loop, not ISR) ───────
static char  br_pending_url[256];
static int   br_pending = 0;

void browser_set_pending_url(const char* url) {
    if (br_pending) return;
    int i = 0;
    while (url[i] && i < 255) {
        br_pending_url[i] = url[i];
        i++;
    }
    br_pending_url[i] = '\0';
    br_pending = 1;
}

// Re-entrancy guard: browser_process_pending() runs from
// desktop_heartbeat(), which is invoked from BOTH the main loop and
// the PIT timer IRQ (isr.c).  The image fetcher re-enables interrupts
// while blocking on DNS/TCP/TLS, so the PIT can fire mid-fetch and
// re-enter this function — a second tcp_connect on the same legacy
// slot then dies with "slot not closed" and marks images failed.
static int br_deferred_busy = 0;

void browser_process_pending(void) {
    if (br_deferred_busy) return;   // already mid-fetch — drop re-entry
    br_deferred_busy = 1;

    // Page navigation (set by the keyboard ISR).  The fetch is blocking
    // (DNS + TCP/TLS + transfer), so run it with interrupts enabled and
    // re-disable before touching the framebuffer.
    if (br_pending) {
        // [BR TRACE] pending URL detected
        { const char* m = "BR PENDING\n";
          for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
        br_pending = 0;
        // CRITICAL: Copy to local buffer before fetching. The ISR could
        // overwrite br_pending_url if another ENTER arrives while the
        // network operation blocks. History is already managed by the
        // ISR (address-bar, back, forward, link-follow).
        char url_local[256];
        int si = 0;
        while (br_pending_url[si] && si < 255) {
            url_local[si] = br_pending_url[si];
            si++;
        }
        url_local[si] = '\0';

        asm volatile("sti");
        br_fetch(url_local, 0);
        asm volatile("cli");

        // The fetched page must reach the VISIBLE framebuffer now —
        // desktop_draw() redraws AND presents (vesa_present flips the
        // back buffer to the screen).  Drawing only browser_draw() here
        // wrote the new page to the back buffer, but desktop_heartbeat
        // only presents on input events, so the screen kept showing the
        // previous frame until the user pressed another key.
        asm volatile("cli");
        desktop_draw();
        asm volatile("sti");
    }

    // ── Image loading (one fetch per heartbeat call) ────────────
    // Runs even when no navigation is pending — this is the steady
    // state that drains the page's <img> slots.  Exactly one fetch per
    // call (each blocks for seconds), then redraw + present so the
    // newly decoded picture (or its failed box) appears immediately.
    // br_fetch_next_image returns 1 while more slots remain.
    if (br_img_active) {
        asm volatile("sti");
        int more = br_fetch_next_image();
        asm volatile("cli");
        desktop_draw();
        asm volatile("sti");
        if (!more) br_img_active = 0;
    }

    br_deferred_busy = 0;
}

// ─── Public API ──────────────────────────────────────────────────

void browser_init(void) {
    br_active = 1;
    br_url[0] = '\0';
    br_url_pos = 0;
    br_focus_addr = 1;
    br_current_url[0] = '\0';
    br_prev_url[0] = '\0';
    br_has_back = 0;
    br_next_url[0] = '\0';
    br_has_forward = 0;
    br_loading = 0;

    // Auto-initialize the NIC and network stack.
    // Probes PCI bus for RTL8139 hardware and initialises it (if not
    // already done), then sets up the TCP/IP stack and resolves the
    // gateway via ARP — no manual `http init` needed before browsing.
    if (!nic_is_up())
        nic_init();
    netstack_init();

    // Auto-load the built-in welcome page so the user sees something
    // immediately — no network needed.
    br_current_url[0] = '\0';  // Don't add "about:welcome" to history
    htmlview_open(br_welcome_html, "Welcome to Nyx Browser");
    br_strcpy(br_status, "Loaded welcome page — type a URL above and press ENTER", 64);
}

void browser_draw(void) {
    if (!br_active) return;

    int W = (int)vesa.width;
    int H = (int)vesa.height;

    // ── Window rect (preserves dragged position) ──────────────────
    // The width is sized from the window's left edge so a non-default
    // position (inherited from another app like the Terminal) never
    // pushes the window past the right screen edge.
    int wx = (g_win_x >= 0) ? g_win_x : 16;
    int wy = (g_win_y >= 0) ? g_win_y : 40;
    int ww = W - wx - 32;
    int wh = H - wy - 16;                // total window height
    int content_h = wh - BR_CHROME_H;    // area below the chrome

    // ── Render the page content inside the window rect ────────────
    if (htmlview_is_open()) {
        // Constrain the htmlview panel to the window and reserve the
        // top strip for our chrome (address + status bars).  In this
        // mode htmlview skips its own title bar.
        htmlview_set_window(wx, wy, ww, wh, BR_CHROME_H + 4);
        htmlview_draw();
    } else {
        // No page loaded — welcome message centred in the window
        vesa_fill_rect(wx, wy + BR_CHROME_H, ww, content_h, 0x0E0C22);

        const char* welcome = "Nyx Web Browser";
        int wlen = br_strlen(welcome);
        int cx = wx + (ww - wlen * 8) / 2;
        int cy = wy + BR_CHROME_H + content_h / 2 - 32;
        vesa_draw_string(cx, cy, welcome, 0x3A2860, 0x0E0C22);

        const char* hint = "Type a URL above and press ENTER";
        int hlen = br_strlen(hint);
        int hx = wx + (ww - hlen * 8) / 2;
        vesa_draw_string(hx, cy + 24, hint, 0x2A2050, 0x0E0C22);

        // Nyx OS branding
        const char* subtitle = "Built on Nyx OS";
        int slen = br_strlen(subtitle);
        int sx = wx + (ww - slen * 8) / 2;
        vesa_draw_string(sx, cy + 52, subtitle, 0x1E1840, 0x0E0C22);
    }

    // ── Window frame ───────────────────────────────────────────────
    vesa_draw_rect(wx, wy, ww, wh, 0x3A2860);

    // ── Draw chrome OVERLAY on top of htmlview ──────────────────────
    // The address bar and status bar sit at the top of the window; the
    // htmlview panel fills the window below them (no own title bar).
    vesa_fill_rect(wx, wy, ww, BR_CHROME_H + 4, 0x1A1A2E);

    // ── Address bar background ──
    vesa_fill_rect(wx, wy + BR_ADDR_Y, ww, BR_ADDR_H, 0x1A1A2E);
    // Accent line at the bottom of the address bar
    vesa_hline(wx, wy + BR_ADDR_Y + BR_ADDR_H - 1, ww, 0x3A3A5E);

    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;
    draw_close_btn(wx + ww - 32, wy + BR_ADDR_Y + 12, 0x1A1A2E, "Browser");

    // ── Back button (with hover glow) ──
    int back_hover = mouse_in_rect(wx + 6, wy + BR_ADDR_Y + 8, 16, BR_ADDR_H - 12) && br_has_back;
    if (back_hover) {
        vesa_fill_rect(wx + 6, wy + BR_ADDR_Y + 8, 16, BR_ADDR_H - 12, 0x282848);
        vesa_draw_rect(wx + 6, wy + BR_ADDR_Y + 8, 16, BR_ADDR_H - 12, 0x6A4FBF);
    }
    uint32_t back_col = br_has_back ? (back_hover ? 0xFFFFFF : 0x8899BB) : 0x444466;
    vesa_draw_string(wx + 8, wy + BR_ADDR_Y + 12, "\x1A", back_col, back_hover ? 0x282848 : 0x1A1A2E);

    // ── Forward button (with hover glow) ──
    int fwd_hover = mouse_in_rect(wx + 18, wy + BR_ADDR_Y + 8, 16, BR_ADDR_H - 12) && br_has_forward;
    if (fwd_hover) {
        vesa_fill_rect(wx + 18, wy + BR_ADDR_Y + 8, 16, BR_ADDR_H - 12, 0x282848);
        vesa_draw_rect(wx + 18, wy + BR_ADDR_Y + 8, 16, BR_ADDR_H - 12, 0x6A4FBF);
    }
    uint32_t fwd_col = br_has_forward ? (fwd_hover ? 0xFFFFFF : 0x8899BB) : 0x444466;
    vesa_draw_string(wx + 20, wy + BR_ADDR_Y + 12, "\x1B", fwd_col, fwd_hover ? 0x282848 : 0x1A1A2E);

    // ── URL input field (with hover glow) ──
    int addr_x = 36;
    int addr_w = ww - addr_x - 80;
    if (addr_w < 100) addr_w = 100;

    int addr_hover = mouse_in_rect(wx + addr_x, wy + BR_ADDR_Y + 6, addr_w, BR_ADDR_H - 12);
    uint32_t addr_bg = br_focus_addr ? 0x282848 : (addr_hover ? 0x222244 : 0x1E1E36);
    uint32_t addr_border = br_focus_addr ? 0x8A6FDF : (addr_hover ? 0x7A5FCF : 0x444466);
    vesa_fill_rect(wx + addr_x, wy + BR_ADDR_Y + 6, addr_w, BR_ADDR_H - 12, addr_bg);
    vesa_draw_rect(wx + addr_x, wy + BR_ADDR_Y + 6, addr_w, BR_ADDR_H - 12, addr_border);

    // URL text (truncate if too wide)
    int max_url_vis = (addr_w - 12) / 8;
    int url_display_len = br_url_pos;
    int url_scroll = 0;
    if (url_display_len > max_url_vis) {
        url_scroll = url_display_len - max_url_vis;
        if (url_scroll < 0) url_scroll = 0;
    }
    vesa_draw_string(wx + addr_x + 6, wy + BR_ADDR_Y + 12, br_url + url_scroll,
                     0xD0D0E8, addr_bg);

    // Blinking cursor in address bar
    if (br_focus_addr && (tick / 8) % 2 == 0) {
        int vis_pos = br_url_pos - url_scroll;
        int cursor_x = wx + addr_x + 6 + vis_pos * 8;
        if (cursor_x >= wx + addr_x + 4 && cursor_x < wx + addr_x + addr_w - 4) {
            vesa_vline(cursor_x, wy + BR_ADDR_Y + 10, BR_ADDR_H - 20, 0xB088FF);
        }
    }

    // ── "Go" button (with hover glow) ──
    int go_x = wx + addr_x + addr_w + 4;
    int go_hover = mouse_in_rect(go_x, wy + BR_ADDR_Y + 6, 68, BR_ADDR_H - 12);
    uint32_t go_bg = go_hover ? 0x4A3A7E : 0x3A2A5E;
    uint32_t go_fg = go_hover ? 0xFFFFFF : 0xCCBBFF;
    vesa_fill_rect(go_x, wy + BR_ADDR_Y + 6, 68, BR_ADDR_H - 12, go_bg);
    vesa_draw_rect(go_x, wy + BR_ADDR_Y + 6, 68, BR_ADDR_H - 12, go_hover ? 0x9A7FDF : 0x6A4FBF);
    vesa_draw_string(go_x + 12, wy + BR_ADDR_Y + 12, "Go", go_fg, go_bg);

    // ── Status bar ──
    vesa_fill_rect(wx, wy + BR_STATUS_Y, ww, BR_STATUS_H, 0x14142A);
    vesa_hline(wx, wy + BR_STATUS_Y, ww, 0x2A2050);

    // Loading spinner
    if (br_loading) {
        char sc[2] = {"-\\|/"[(tick / 4) % 4], '\0'};
        vesa_draw_string(wx + 8, wy + BR_STATUS_Y + 4, sc, 0x55CCFF, 0x14142A);
        vesa_draw_string(wx + 20, wy + BR_STATUS_Y + 4, br_status, 0x88AACC, 0x14142A);
    } else {
        vesa_draw_string(wx + 8, wy + BR_STATUS_Y + 4, br_status, 0x667788, 0x14142A);
    }

    // Keyboard hint on the right side of the status bar
    const char* hint = "Z=back X=fwd  ESC=addr  TAB=content";
    int hl = br_strlen(hint);
    vesa_draw_string(wx + ww - hl * 8 - 12, wy + BR_STATUS_Y + 4, hint, 0x445566, 0x14142A);
}

void browser_handle_key(char c) {
    if (!br_active) return;

    // ── Address bar is focused — handle URL input ──
    if (br_focus_addr) {
        if (c == 27) {  // ESC → exit browser
            if (htmlview_is_open()) htmlview_close();
            br_active = 0;
            current_mode = MODE_DESKTOP;
            return;
        }

        if (c == '\n' || c == '\r' || c == 13) {  // ENTER → navigate (deferred)
            if (br_url[0]) {
                // History update (fast, no blocking) happens in the ISR
                if (br_current_url[0]) {
                    br_strcpy(br_prev_url, br_current_url, 256);
                    br_has_back = 1;
                }
                br_has_forward = 0;
                br_next_url[0] = '\0';
                br_strcpy(br_current_url, br_url, 256);
                // Defer the actual network fetch to main loop
                browser_set_pending_url(br_url);
                browser_draw();
            }
            return;
        }

        if (c == 9) {  // TAB → switch focus to page content
            if (htmlview_is_open()) {
                br_focus_addr = 0;
                browser_draw();
            }
            return;
        }

        if (c == '\b' || c == 127) {  // BACKSPACE
            if (br_url_pos > 0) {
                br_url_pos--;
                br_url[br_url_pos] = '\0';
                browser_draw();
            }
            return;
        }

        // Ctrl+V (22) → paste URL from clipboard
        if (c == 22) {
            const char* clip = clipboard_get();
            if (clip) {
                int clen = 0;
                while (clip[clen] && clip[clen] != '\n' && clip[clen] != '\r') clen++;
                int space = 255 - br_url_pos;
                if (clen > space) clen = space;
                for (int i = 0; i < clen; i++)
                    br_url[br_url_pos++] = clip[i];
                br_url[br_url_pos] = '\0';
                browser_draw();
            }
            return;
        }

        // Ctrl+A (1) → select all text in address bar (clear and ready for paste)
        if (c == 1) {
            br_url_pos = 0;
            br_url[0] = '\0';
            browser_draw();
            return;
        }

        // Printable character → append to URL
        if (c >= 32 && c <= 126) {
            if (br_url_pos < 255) {
                br_url[br_url_pos++] = c;
                br_url[br_url_pos] = '\0';
                browser_draw();
            }
            return;
        }

        return;
    }

    // ── Page content is focused — delegate to htmlview ──
    if (htmlview_is_open()) {
        // ESC in content area → go back to address bar
        if (c == 27) {
            br_focus_addr = 1;
            browser_draw();
            return;
        }

        // Ctrl+C (3) → copy current URL to clipboard
        if (c == 3 && br_current_url[0]) {
            clipboard_set(br_current_url);
            return;
        }

        // Ctrl+V (22) → paste URL and navigate
        if (c == 22) {
            const char* clip = clipboard_get();
            if (clip) {
                int clen = 0;
                while (clip[clen] && clip[clen] != '\n') clen++;
                if (clen > 255) clen = 255;
                for (int i = 0; i < clen; i++)
                    br_url[i] = clip[i];
                br_url[clen] = '\0';
                br_url_pos = clen;
                br_focus_addr = 1;
                browser_draw();
            }
            return;
        }

        // Back/Forward navigation (deferred — fetch is in main loop)
        if ((c == 'D' || c == 'z' || c == 'Z') && br_has_back) {
            // Navigate back: save current as forward, load previous
            char target[256];
            br_strcpy(target, br_prev_url, 256);
            // History update (fast) in ISR
            br_strcpy(br_next_url, br_current_url, 256);
            br_has_forward = 1;
            br_strcpy(br_current_url, target, 256);
            br_has_back = 0;
            br_prev_url[0] = '\0';
            // Defer the fetch
            browser_set_pending_url(target);
            browser_draw();
            return;
        }

        if ((c == 'C' || c == 'x' || c == 'X') && br_has_forward) {
            // Navigate forward: save current as back, load next
            char target[256];
            br_strcpy(target, br_next_url, 256);
            // History update (fast) in ISR
            br_strcpy(br_prev_url, br_current_url, 256);
            br_has_back = 1;
            br_strcpy(br_current_url, target, 256);
            br_has_forward = 0;
            br_next_url[0] = '\0';
            // Defer the fetch
            browser_set_pending_url(target);
            browser_draw();
            return;
        }

        // Delegate all other keys to the HTML viewer
        htmlview_handle_key(c);

        // After htmlview processes a key, check if it followed a link
        // (htmlview closes itself when ENTER is pressed on a link)
        if (!htmlview_is_open()) {
            const char* pending = htmlview_get_pending_url();
            if (pending && pending[0]) {
                // A link was followed — resolve it against the current
                // page URL so relative hrefs (/wiki/Page, page2.html,
                // //other.host/x) work.  If there's no current host
                // (bare hostname typed, first page), fall back to
                // treating the href as the full URL.
                char resolved[256];
                if (!br_resolve_link(pending, resolved, 256))
                    br_strcpy(resolved, pending, 256);
                // Navigate to it (deferred)
                if (br_current_url[0]) {
                    br_strcpy(br_prev_url, br_current_url, 256);
                    br_has_back = 1;
                }
                br_has_forward = 0;
                br_next_url[0] = '\0';
                br_strcpy(br_current_url, resolved, 256);
                browser_set_pending_url(resolved);
            } else {
                // Viewer closed for another reason — focus address bar
                br_focus_addr = 1;
            }
        }

        browser_draw();
    }
}

// ─── Mouse handler ───────────────────────────────────────────────//
// Routes clicks to the address bar, back/forward buttons, Go button,
// or content area. Returns 1 if the click was consumed, 0 otherwise.
int browser_handle_mouse(int mx, int my) {
    if (!vesa.active) return 0;
    if (!br_active) return 0;

    int W = (int)vesa.width;

    // ── Back button (CP437 ← at x=8..18, y=BR_ADDR_Y+12..+28) ───
    if (my >= BR_ADDR_Y + 8 && my < BR_ADDR_Y + BR_ADDR_H - 4 &&
        mx >= 6 && mx < 22) {
        if (br_has_back && br_prev_url[0]) {
            char tmp_url[256];
            br_strcpy(tmp_url, br_current_url, 256);
            br_strcpy(br_current_url, br_prev_url, 256);
            br_strcpy(br_prev_url, tmp_url, 256);
            br_has_forward = 1;
            br_fetch(br_current_url, 0);
            return 1;
        }
        return 1;
    }

    // ── Forward button (CP437 → at x=20..30) ─────────────────────
    if (my >= BR_ADDR_Y + 8 && my < BR_ADDR_Y + BR_ADDR_H - 4 &&
        mx >= 18 && mx < 34) {
        if (br_has_forward && br_next_url[0]) {
            char tmp_url[256];
            br_strcpy(tmp_url, br_current_url, 256);
            br_strcpy(br_current_url, br_next_url, 256);
            br_strcpy(br_next_url, tmp_url, 256);
            br_has_back = 1;
            br_fetch(br_current_url, 0);
            return 1;
        }
        return 1;
    }

    // ── Address bar + Go button ──────────────────────────────────
    int addr_x = 36;
    int addr_w = W - addr_x - 80;
    if (addr_w < 100) addr_w = 100;
    int go_x = addr_x + addr_w + 4;

    // Go button
    if (mx >= go_x && mx < go_x + 68 &&
        my >= BR_ADDR_Y + 6 && my < BR_ADDR_Y + BR_ADDR_H - 6) {
        if (br_url[0]) {
            br_focus_addr = 0;
            if (br_current_url[0]) {
                br_strcpy(br_prev_url, br_current_url, 256);
                br_has_back = 1;
            }
            br_has_forward = 0;
            br_strcpy(br_current_url, br_url, 256);
            browser_set_pending_url(br_url);
        }
        return 1;
    }

    // Address bar
    if (mx >= addr_x && mx < addr_x + addr_w &&
        my >= BR_ADDR_Y + 6 && my < BR_ADDR_Y + BR_ADDR_H - 6) {
        br_focus_addr = 1;
        return 1;
    }

    // ── Content area (below chrome) ──────────────────────────────
    // Switch focus from address bar to page content so arrow keys
    // and TAB cycle links instead of editing the URL
    if (my >= BR_CHROME_H) {
        br_focus_addr = 0;
        return 1;
    }

    return 0;
}
