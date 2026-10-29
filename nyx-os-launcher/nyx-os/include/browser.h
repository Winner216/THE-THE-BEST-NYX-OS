// include/browser.h — Nyx OS Web Browser (desktop app)
#ifndef BROWSER_H
#define BROWSER_H

// Initialize browser state. Call before switching to MODE_BROWSER.
void browser_init(void);

// Draw the browser chrome (address bar, nav buttons, status bar)
// and delegate to htmlview for page content.
void browser_draw(void);

// Handle keyboard input. Routes to address bar or htmlview.
void browser_handle_key(char c);

// Handle mouse click. Routes to address bar, nav buttons, etc.
int  browser_handle_mouse(int mx, int my);

// Deferred navigation — called from ISR to save a URL for later
// processing in the main loop (avoiding blocking in interrupt context).
void browser_set_pending_url(const char* url);

// Process any pending navigation — call from desktop_heartbeat().
void browser_process_pending(void);

#endif
