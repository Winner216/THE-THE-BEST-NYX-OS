#ifndef CLIPBOARD_H
#define CLIPBOARD_H

// System-wide clipboard — works across Terminal, Editor, Browser, etc.
// Stores text data that any app can read/write via Ctrl+C / Ctrl+V / Ctrl+X.

#define CLIPBOARD_MAX  8192   // Max clipboard text size (8 KB)

// Set clipboard contents (replaces any previous content).
// Returns 1 on success, 0 if text is NULL or empty.
int  clipboard_set(const char* text);

// Set clipboard from a buffer with explicit length.
int  clipboard_set_len(const char* text, int len);

// Get pointer to clipboard text (read-only). Returns NULL if empty.
const char* clipboard_get(void);

// Get clipboard text length. Returns 0 if empty.
int  clipboard_len(void);

// Clear the clipboard.
void clipboard_clear(void);

// Returns 1 if clipboard has content.
int  clipboard_has(void);

// Append text to clipboard (for multi-line copy across apps).
int  clipboard_append(const char* text);

// Append a newline + text to clipboard.
int  clipboard_append_line(const char* text);

#endif
