// include/security.h — Nyx OS Security Tools Suite
// Provides VESA pixel UI (with VGA text fallback) for:
//   - Port Scanner: scan common ports on localhost
//   - NetStat: view network adapter info and connection status
//   - Security Audit: check system security settings
#ifndef SECURITY_H
#define SECURITY_H

void security_init(void);
void security_draw(void);
void security_handle_key(char c);
int  security_handle_mouse(int mx, int my);

// Real-time file protection: scan a specific file by index.
// Returns the threat score (0-10). If the score indicates a
// threat (>=6), triggers a status bar alert and screen flash.
int security_scan_file_at(int file_idx);

// scanprotect CLI admin panel — query / toggle protection.
int  security_protection_status(void);          // 1 = enabled, 0 = disabled
void security_protection_toggle(void);          // flip on↔off
void security_protection_set(int on);           // force on/off

// Alert log — ring buffer of recent threat alerts.
#define SCAN_LOG_MAX 16
int           security_alert_count(void);       // total alerts this session
const char*   security_alert_file(int idx);     // file name (0 = newest)
int           security_alert_score(int idx);    // threat score

// Prompt indicator — [ON]/[OFF] in the shell prompt
int  security_prompt_indicator_status(void);    // 1 = showing, 0 = hidden
void security_prompt_indicator_toggle(void);    // flip on↔off

// Protection timeline — history of on/off toggles with timestamps
#define TIMELINE_MAX 16
int           security_timeline_count(void);     // total recorded changes
const char*   security_timeline_time(int idx);   // "HH:MM:SS" (0 = newest)
int           security_timeline_state(int idx);  // 0 or 1

// Config persistence — save/load protection + prompt state to .scanprotect
void security_config_save(void);
void security_config_load(void);

#endif
