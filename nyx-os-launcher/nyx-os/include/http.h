// include/http.h — Minimal HTTP/1.1 client
#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

// Maximum response body size. 256 KB covers full Wikipedia articles
// and the wikipedia.org portal page (~120 KB) instead of truncating
// at 32 KB. The raw receive buffer, gzip scratch and response body
// each use this (3x in BSS — the kernel already carries ~800 MB of
// BSS arrays, so this is a rounding error). http_get's buffers are
// static (BSS) so they never touch the 16 KB kernel stack.
#define HTTP_MAX_RESPONSE 1048576

// HTTP response
typedef struct {
    int   status_code;        // 200, 404, etc.
    char  status_text[64];    // "OK", "Not Found", etc.
    char  location[256];      // Location header when a redirect was
                              // not followed (e.g. https:// target)
    char  body[HTTP_MAX_RESPONSE];  // Response body
    int   body_len;           // Length of body
} HttpResponse;

// Perform an HTTP GET request to the given IP and path.
// ip is in network byte order (e.g. host 10.0.2.2 → 0x0202000A)
// Returns 0 on success, -1 on failure.
// The response is written into the provided HttpResponse struct.
int http_get(uint32_t ip, uint16_t port, const char* host, const char* path, HttpResponse* resp);

// HTTPS variant: same GET over the in-kernel TLS 1.2 client (port 443
// fixed).  Redirects are followed and may switch schemes (https→http,
// http→https).  ip is in LE wire format (e.g. host 10.0.2.2 → 0x0202000A).
// Returns 0 on success, -1 on failure (tls_last_error() explains).
int http_get_secure(uint32_t ip, const char* host, const char* path, HttpResponse* resp);

// Get a pointer to a static HttpResponse (BSS-allocated, not on stack).
// Use this instead of stack-allocating HttpResponse when the body
// is large (HTTP_MAX_RESPONSE bytes) to avoid overflowing the
// kernel stack (only 16 KB).
HttpResponse* http_get_global_response(void);

// ─── Abort/cancel mechanism for user-visible HTTP requests ──────
// The keyboard ISR (or browser_handle_key) calls http_abort() when
// ESC is pressed while an HTTP request is in flight. The polling
// loops inside http_get() and tcp_connect() check http_is_aborted()
// and break immediately, returning control to the main event loop.
// This prevents the OS from freezing when the network is down.
//
// The flag is cleared by http_get_global_response() and by http_get
// at the start of each call.
void http_abort(void);
int  http_is_aborted(void);

#endif
