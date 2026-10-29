// cpu/clipboard.c — System-wide clipboard for Nyx OS
//
// A simple global text buffer shared across all apps.
// Any app can call clipboard_set() / clipboard_get() to share text.
// Works with Ctrl+C (copy), Ctrl+X (cut), Ctrl+V (paste) in any app.

#include <clipboard.h>
#include <string.h>

static char  clip_buf[CLIPBOARD_MAX];
static int   clip_size = 0;

int clipboard_set(const char* text) {
    if (!text || !*text) return 0;
    int len = 0;
    while (text[len] && len < CLIPBOARD_MAX - 1) len++;
    memcpy(clip_buf, text, len);
    clip_buf[len] = '\0';
    clip_size = len;
    return 1;
}

int clipboard_set_len(const char* text, int len) {
    if (!text || len <= 0) return 0;
    if (len > CLIPBOARD_MAX - 1) len = CLIPBOARD_MAX - 1;
    memcpy(clip_buf, text, len);
    clip_buf[len] = '\0';
    clip_size = len;
    return 1;
}

const char* clipboard_get(void) {
    if (clip_size == 0) return (const char*)0;
    return clip_buf;
}

int clipboard_len(void) {
    return clip_size;
}

void clipboard_clear(void) {
    clip_buf[0] = '\0';
    clip_size = 0;
}

int clipboard_has(void) {
    return clip_size > 0;
}

int clipboard_append(const char* text) {
    if (!text) return 0;
    int tlen = 0;
    while (text[tlen]) tlen++;
    if (clip_size + tlen >= CLIPBOARD_MAX - 1) return 0;
    memcpy(clip_buf + clip_size, text, tlen);
    clip_size += tlen;
    clip_buf[clip_size] = '\0';
    return 1;
}

int clipboard_append_line(const char* text) {
    if (clip_size > 0 && clip_size < CLIPBOARD_MAX - 1) {
        clip_buf[clip_size++] = '\n';
        clip_buf[clip_size] = '\0';
    }
    return clipboard_append(text);
}
