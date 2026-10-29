// cpu/keyboard.c — Nyx OS Keyboard Driver
//
// Simple ISR-driven keyboard: scancodes are converted to characters
// and processed immediately. The critical fix: before calling
// shell_process() and desktop_draw() (which may block for ~300ms on
// networking), we enable interrupts via sti so the keyboard and
// timer ISRs can still fire — preventing the "screen freeze after
// ENTER" bug. After the blocking call completes, interrupts are
// re-disabled with cli before the ISR returns to the interrupted
// code (which will re-enable them via iret).
//
// This is the original design (proven to work for basic typing and
// commands), with only the sti fix added.

#include <keyboard.h>
#include <clipboard.h>
#include <game.h>
#include <applib.h>
#include <terminal.h>
#include <isr.h>
#include <io.h>
#include <desktop.h>
#include <shell.h>
#include <login.h>
#include <stdint.h>
#include <power.h>
#include <screensaver.h>
#include <fs.h>
#include <rtc.h>
#include <auth.h>
#include <net.h>
#include <netstack.h>
#include <nic.h>
#include <elfload.h>
#include <userprogs.h>
#include <task.h>
#include <http.h>
#include <dns.h>
#include <dhcp.h>
#include <browser.h>
#include <launcher.h>
#include <pkg.h>
#include <store.h>

#define BUFFER_SIZE 256

// ─── Exposed input line (read by desktop.c for VESA rendering) ───
char     cmd_buffer[BUFFER_SIZE];
uint32_t cmd_pos = 0;

// ─── Scancode state ──────────────────────────────────────────────
static int shift_pressed = 0;
static int e0_prefix     = 0;
static int kb_e0_game    = 0;   // E0 prefix pending for the game key ring
static int ctrl_pressed  = 0;
// Left Alt = 0x38 make / 0xB8 break.  Right Alt arrives as an E0
// prefix + 0x38 / 0xB8 (handled in the E0 block below).  While Alt
// is held, Tab (0x0F) drives the Alt+Tab task switcher in desktop.c.
static int alt_pressed  = 0;

static const char scancode_map[] = {
     0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', 8,
     9, 'q','w','e','r','t','y','u','i','o','p','[',']',13,   0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
    'z','x','c','v','b','n','m',',','.','/',  0, '*', 0, ' ',
};

static const char scancode_map_shift[] = {
     0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', 8,
     9, 'Q','W','E','R','T','Y','U','I','O','P','{','}',13,   0,
    'A','S','D','F','G','H','J','K','L',':','"','~',  0, '|',
    'Z','X','C','V','B','N','M','<','>','?',  0, '*', 0, ' ',
};

// ─── Forward declarations ────────────────────────────────────────
static void handle_shell_command(void);

// ─── ISR ─────────────────────────────────────────────────────────
// ─── Ring-3 user programs ───────────────────────────────────────
// `user` loads the embedded ELF into the reserved user slot and
// iret's to ring 3; `userfault` is the deliberate-crash demo proving
// a user-mode fault kills only the task; `sockget <host> <path>` is
// the BSD-socket demo (ring-3 DNS + TCP + HTTP).  Returns 1 if `cmd`
// was one of these (handled), 0 otherwise.
static int user_prog_launch(const char* cmd) {
    int prog = -1;
    const char* args = cmd;   // full command line, passed to the program
    if (cmd[0]=='u' && cmd[1]=='s' && cmd[2]=='e' && cmd[3]=='r' && cmd[4]=='\0')
        prog = 0;
    else if (cmd[0]=='u' && cmd[1]=='s' && cmd[2]=='e' && cmd[3]=='r' &&
             cmd[4]=='f' && cmd[5]=='a' && cmd[6]=='u' && cmd[7]=='l' &&
             cmd[8]=='t' && cmd[9]=='\0')
        prog = 1;
    else if (cmd[0]=='s' && cmd[1]=='o' && cmd[2]=='c' && cmd[3]=='k' &&
             cmd[4]=='g' && cmd[5]=='e' && cmd[6]=='t' &&
             (cmd[7]=='\0' || cmd[7]==' '))
        prog = 2;
    if (prog < 0 || prog >= userprog_count) return 0;

    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_print("  Loading ");
    terminal_print(userprogs[prog].name);
    terminal_print(" (");
    { char nb[12]; int np=0; unsigned v=userprogs[prog].len;
      if(!v){nb[np++]='0';} else { char t[12]; int q=0;
        while(v>0){t[q++]=(char)('0'+v%10); v/=10;}
        while(q>0) nb[np++]=t[--q]; }
      nb[np]=0; terminal_print(nb); }
    terminal_print(" bytes)...\n");
    int id = user_exec(userprogs[prog].data, userprogs[prog].len,
                       userprogs[prog].name, args);
    if (id < 0) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_print("  ELF load failed!\n");
    } else {
        terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        terminal_print("  Launched at ring 3 (pid=");
        { char nb[12]; int np=0; unsigned v=(unsigned)id;
          if(!v){nb[np++]='0';} else { char t[12]; int q=0;
            while(v>0){t[q++]=(char)('0'+v%10); v/=10;}
            while(q>0) nb[np++]=t[--q]; }
          nb[np]=0; terminal_print(nb); }
        terminal_print(")\n");
    }
    return 1;
}

// ─── Small helpers for the pkg command handler ────────────────────
// `s` starts with whole word `w` (followed by NUL or space)?
static int k_word(const char* s, const char* w) {
    int i = 0;
    while (w[i]) { if (s[i] != w[i]) return 0; i++; }
    return s[i] == '\0' || s[i] == ' ';
}
// If `s` starts with word `w`, return the argument after it (NUL if
// none); otherwise 0.
static const char* k_word_arg(const char* s, const char* w) {
    int i = 0;
    while (w[i]) { if (s[i] != w[i]) return 0; i++; }
    if (s[i] != ' ' && s[i] != '\0') return 0;
    while (s[i] == ' ') i++;
    return s + i;
}
// Print an unsigned decimal to the terminal.
static void k_print_u(unsigned v) {
    char nb[12]; int np = 0;
    if (!v) { nb[np++] = '0'; }
    else { char t[12]; int q = 0;
        while (v > 0) { t[q++] = (char)('0' + v % 10); v /= 10; }
        while (q > 0) nb[np++] = t[--q]; }
    nb[np] = 0;
    terminal_print(nb);
}

static void keyboard_callback(registers_t regs) {
    (void)regs;
    uint8_t scancode = inb(0x60);

    // ─── Game-mode raw input ────────────────────────────────────
    // While a ring-3 game owns the screen (MODE_GAME), ALL keyboard
    // events — presses and releases, including E0-prefixed arrows —
    // are queued for it first.  The game polls via SYS_GETKEY; the
    // event word: bit0..7 scancode, bit8 E0 prefix, bit9 release.
    // Desktop keys (shortcuts etc.) are fully consumed here so the
    // game sees the unfiltered hardware.
    if (game_active()) {
        screensaver_reset_idle();          // playing counts as activity
        if (scancode == 0xE0) { kb_e0_game = 1; return; }
        uint16_t ev = scancode & 0xFF;
        if (kb_e0_game) { ev |= 0x100; kb_e0_game = 0; }
        if (scancode & 0x80) ev |= 0x200;    // break/release
        game_key_push(ev);
        // EOI was already sent by the ISR stub; nothing else to do.
        return;
    }

    screensaver_reset_idle();

    // Screensaver wake — restore full desktop
    if (screensaver_is_active()) {
        desktop_draw();
        return;
    }

    // ═══════════════════════════════════════════════════════════════
    //  LAUNCHER KEY HANDLING — ABSOLUTE FIRST PRIORITY
    // ═══════════════════════════════════════════════════════════════
    // When the Quick Launch panel is open, ALL key handling is
    // handled here — BEFORE any mode checks, Ctrl checks, F-keys,
    // or any other dispatch. This eliminates every possible path
    // where launcher keys could be intercepted by other handlers.
    //
    // The launcher only handles these keys:
    //   • E0 prefix + 0x48 (Up)    → navigate previous item
    //   • E0 prefix + 0x50 (Down)  → navigate next item
    //   • Enter (0x1C)             → launch selected app
    //   • Q / ESC                  → close launcher
    // All other keys are silently consumed (ignored) while the
    // launcher is open.
    //
    // Everything below this block belongs to the non-launcher path
    // and is unreachable while launcher_is_open() returns true.

    // ─── Launcher: E0 prefix (arrow keys) ────────────────────────
    // Check E0 before any other handler so arrows are caught even
    // if Ctrl is stuck or another check would intercept them.
    if (launcher_is_open()) {
        // Handle E0 prefix for arrows
        if (scancode == 0xE0) { e0_prefix = 1; return; }
        if (e0_prefix) {
            e0_prefix = 0;
            int nav = -1;
            if (scancode == 0x48) nav = 0;  // Up
            else if (scancode == 0x50) nav = 1;  // Down
            else return;  // E0 but not arrow → ignore

            int sel = launcher_get_sel();
            if (nav == 0)  // Up
                launcher_set_sel(sel ? sel - 1 : PINNED_COUNT - 1);
            else  // Down
                launcher_set_sel(sel < PINNED_COUNT - 1 ? sel + 1 : 0);
            launcher_draw();
            return;
        }
        // ─── Launcher: Ctrl press/release — still track Ctrl state
        // but then return so Ctrl doesn't affect launcher behavior.
        if (scancode == 0x1D) { ctrl_pressed = 1; return; }
        if (scancode == 0x9D) { ctrl_pressed = 0; return; }
        // Shift — track but consume
        if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return; }
        if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return; }
        // F1-F8: Launch pinned app directly (closes launcher first)
        if (scancode >= 0x3B && scancode <= 0x42) {
            launcher_launch_pinned(scancode - 0x3B);
            return;
        }
        // F10: Ignored while launcher open
        if (scancode == 0x44) return;
        // Ignore key releases
        if (scancode & 0x80) return;
        // Convert scancode → ASCII for Enter, Q, ESC
        char lc = 0;
        if (scancode < sizeof(scancode_map))
            lc = shift_pressed ? scancode_map_shift[scancode] : scancode_map[scancode];
        if (lc == 0) return;
        // Handle Enter (13) — launch selected app
        if (lc == 13) {
            int sel = launcher_get_sel();
            if (sel >= 0) {
                int lid = launcher_get_launch_id(sel);
                launcher_set_open(0);
                desktop_launch(lid);
                desktop_draw();
            }
            return;
        }
        // Handle Q / ESC — close launcher
        if (lc == 'q' || lc == 'Q' || lc == 27) {
            launcher_set_open(0);
            desktop_draw();
            return;
        }
        // All other keys → silently consumed
        return;
    }

    // ═══════════════════════════════════════════════════════════════
    //  NORMAL KEY PROCESSING (launcher is closed)
    // ═══════════════════════════════════════════════════════════════

    // ─── Alt+Tab task switcher ──────────────────────────────────
    // Only meaningful after login (before that, keys go to the
    // login screen; the switcher would draw over it).  Left Alt is
    // 0x38 make / 0xB8 break; right Alt is E0 0x38 / E0 0xB8
    // (handled in the E0 prefix block below).  Committing on the
    // Alt BREAK is what makes Alt+Tab feel natural: hold Alt, tap
    // Tab to cycle, let go of Alt to switch.
    if (login_is_done()) {
        // IMPORTANT: guard with !e0_prefix.  Right Alt arrives as
        // E0 0x38 / E0 0xB8 — the E0 byte sets e0_prefix, then the
        // 0x38/0xB8 must fall through to the E0 block below (which
        // clears e0_prefix).  If we caught them here, the stale
        // prefix would swallow the NEXT non-E0 key (letters would
        // vanish after pressing right Alt).
        if (!e0_prefix && scancode == 0x38) { alt_pressed = 1; return; }
        if (!e0_prefix && scancode == 0xB8) {
            alt_pressed = 0;
            desktop_alt_tab_commit();
            return;
        }
        // While the switcher overlay is open, swallow EVERYTHING
        // except Tab taps (advance) and the Alt release handled
        // above — so the app underneath can't react mid-switch.
        if (desktop_alt_tab_active()) {
            if (scancode == 0x0F) desktop_alt_tab();  // Tab make → advance
            return;                                   // swallow Tab break + rest
        }
        // Alt+Tab with Alt held: Tab make opens/advances.
        if (alt_pressed && scancode == 0x0F) {
            desktop_alt_tab();
            return;
        }
        // Tab break while Alt held — swallow so it can't reach apps.
        if (alt_pressed && scancode == 0x8F) return;
    }

    // Ctrl press/release
    if (scancode == 0x1D) { ctrl_pressed = 1; return; }

    if (scancode == 0x9D) {
        ctrl_pressed = 0;
        // If undo-browse mode is active, commit the current preview
        // on Ctrl release so the user can just let go of the key.
        desktop_undo_browse_commit();
        return;
    }

    // ─── F1-F8: Quick Launch shortcuts ───────────────────────────
    // F1-F8 each launch the corresponding pinned app (index 0-7),
    // from the desktop OR over an already-open app (real taskbar-pin
    // behavior — the previous app stays open behind it). Requires
    // login. F10 (0x44) powers the menu (unchanged).
    if (scancode >= 0x3B && scancode <= 0x42) {
        if (!login_is_done()) return;
        // F2 while the Files window is focused = inline rename of the
        // selected row (the Files hint bar advertises "F2 rename").
        // Don't relaunch Files over itself — the rename handler in
        // desktop.c listens on char 6, and without this the physical
        // F2 key would never reach it.
        if (scancode == 0x3C && current_mode == MODE_FILES) {
            desktop_handle_key(6);
            return;
        }
        {
            static const int f_to_launch_id[8] = {0, 1, 3, 8, 6, 7, 9, 10};
            desktop_launch(f_to_launch_id[scancode - 0x3B]);
            desktop_draw();   // redraw so the new window + tray appear
        }
        return;
    }

    // F10 = power menu (only after login)
    if (scancode == 0x44) {
        if (!login_is_done()) return;
        desktop_handle_key(16);
        return;
    }

    // E0 prefix → next scancode is an extended key (arrows)
    if (scancode == 0xE0) { e0_prefix = 1; return; }

    if (e0_prefix) {
        e0_prefix = 0;
        // ── Right Alt (E0 0x38 make / E0 0xB8 break) ──────────
        // Same Alt+Tab behavior as the left Alt handled above.
        if (login_is_done()) {
            if (scancode == 0x38) { alt_pressed = 1; return; }
            if (scancode == 0xB8) {
                alt_pressed = 0;
                desktop_alt_tab_commit();
                return;
            }
        }
        char arrow = 0;
        switch (scancode) {
            case 0x48: arrow = 'A'; break;  // Up
            case 0x50: arrow = 'B'; break;  // Down
            case 0x4D: arrow = 'C'; break;  // Right
            case 0x4B: arrow = 'D'; break;  // Left
            default:   return;
        }
        // During login, route arrows to login handler
        if (!login_is_done()) {
            login_handle_key(arrow);
            return;
        }
        // In terminal mode, arrows = history navigation
        if (current_mode == MODE_TERMINAL) {
            // Up/Down navigate history
            if (arrow == 'A' || arrow == 'B') {
                int i;
                for (i = 0; i < (int)cmd_pos; i++) {
                    terminal_putchar(8);
                    terminal_putchar(' ');
                    terminal_putchar(8);
                }
                if (arrow == 'A')
                    shell_history_up(cmd_buffer, (int*)&cmd_pos);
                else
                    shell_history_down(cmd_buffer, (int*)&cmd_pos);
                terminal_print(cmd_buffer);
                return;
            }
            return;
        }
        // Other modes → desktop handler
        desktop_handle_key(arrow);
        return;
    }

    // Shift press/release
    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return; }
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return; }

    // Ignore key releases
    if (scancode & 0x80) return;

    // Convert scancode → ASCII
    char c = 0;
    if (scancode < sizeof(scancode_map))
        c = shift_pressed ? scancode_map_shift[scancode] : scancode_map[scancode];
    if (c == 0) return;

    // Ctrl combinations
    if (ctrl_pressed) {
        switch (c) {
            case 's': case 'S': c = 19; break;
            case 'f': case 'F': c = 6;  break;
            case 'r': case 'R': c = 18; break;
            case 'z': c = 26; break;  // Ctrl+Z → undo
            case 'Z': c = 25; break;  // Ctrl+Shift+Z → redo
            case 'x': case 'X': c = 24; break;
            case 'c': case 'C': c = 3;  break;
            case 'v': case 'V': c = 22; break;
            case 'w': case 'W': c = 23; break;
            case 't': case 'T': c = 20; break;
            case 'l': case 'L': c = 12; break;
            case 'a': case 'A': c = 1;  break;
            case 'e': case 'E': c = 5;  break;
            default: return;
        }
    }

    // ─── Route character based on current mode ─────────────────────

    // Login screen: all keys go to login handler
    if (!login_is_done()) {
        login_handle_key(c);
        return;
    }

    // ─── Terminal mode ────────────────────────────────────────────
    if (current_mode == MODE_TERMINAL) {
        // ESC / TAB / F2 (6) / F10 (16) → desktop handler
        if (c == 27 || c == 9 || c == 6 || c == 16) {
            desktop_handle_key(c);
            return;
        }

        // Left/Right arrows → ignore in terminal
        if (c == 'C' || c == 'D') return;

        // Backspace
        if (c == 8) {
            if (cmd_pos > 0) {
                cmd_pos--;
                cmd_buffer[cmd_pos] = '\0';
            }
            desktop_draw();
            return;
        }

        // ENTER → inline command handling (no shell_process call)
        if (c == 13) {
            cmd_buffer[cmd_pos] = '\0';

            terminal_print("nyx> ");
            terminal_print(cmd_buffer);
            terminal_putchar('\n');

            // ─── Nyx App Store / package manager ─────────────────
            // `pkg` subcommands: update, available, list, install,
            // remove, run.  `store` opens the graphical App Store
            // window (cpu/store.c).
            if (cmd_buffer[0]=='p'&&cmd_buffer[1]=='k'&&cmd_buffer[2]=='g'&&
                (cmd_buffer[3]=='\0'||cmd_buffer[3]==' ')) {
                const char* a = cmd_buffer + 3;
                while (*a == ' ') a++;
                // pkg alone / pkg help
                if (a[0]=='\0' || (a[0]=='h'&&a[1]=='e'&&a[2]=='l'&&a[3]=='p'&&a[4]=='\0')) {
                    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                    terminal_print("  Nyx App Store package manager\n");
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                    terminal_print("    pkg update            refresh the app catalog\n");
                    terminal_print("    pkg available         list apps in the store\n");
                    terminal_print("    pkg list              list installed apps\n");
                    terminal_print("    pkg install <name>    download & install an app (or `all`)\n");
                    terminal_print("    pkg remove <name>     uninstall an app\n");
                    terminal_print("    pkg run <name>        launch an installed app\n");
                    terminal_print("    store                 open the App Store window\n");
                    cmd_pos = 0; desktop_draw(); return;
                }
                // pkg update / refresh — pull the catalog
                if (k_word(a, "update") || k_word(a, "refresh")) {
                    terminal_print("  Contacting store at 10.0.2.2:8123 ...\n");
                    int n = pkg_update();
                    if (n < 0) {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  "); terminal_print(pkg_last_error());
                        terminal_print("\n");
                    } else {
                        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                        terminal_print("  Catalog loaded: "); k_print_u((unsigned)n);
                        terminal_print(" app"); if (n != 1) terminal_print("s");
                        terminal_print("\n");
                    }
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                    cmd_pos = 0; desktop_draw(); return;
                }
                // pkg available — show what the store sells
                if (k_word(a, "available") || k_word(a, "apps") ||
                    k_word(a, "search") || k_word(a, "catalog")) {
                    if (pkg_catalog_count() == 0) {
                        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                        terminal_print("  Catalog empty — run `pkg update` first.\n");
                    } else {
                        terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                        terminal_print("\n  Available apps in the Nyx Store:\n");
                        terminal_print("  --------------------------------\n");
                        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                        for (int i = 0; i < pkg_catalog_count(); i++) {
                            const PkgEntry* e = pkg_catalog_entry(i);
                            terminal_print("  "); terminal_print(e->name);
                            terminal_print("  v"); terminal_print(e->version);
                            terminal_print("  ");
                            terminal_print(e->author);
                            terminal_print("\n      "); terminal_print(e->desc);
                            terminal_print("\n");
                        }
                        terminal_print("\n");
                    }
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                    cmd_pos = 0; desktop_draw(); return;
                }
                // pkg list — installed packages (fs slots named "@name")
                if (k_word(a, "list") || k_word(a, "installed")) {
                    terminal_print("\n  Installed packages:\n");
                    int found = 0;
                    for (int fi = 0; fi < MAX_FILES; fi++) {
                        if (filesystem[fi].used && filesystem[fi].name[0]=='@') {
                            terminal_print("    ");
                            terminal_print(filesystem[fi].name + 1);
                            terminal_print("\n");
                            found = 1;
                        }
                    }
                    if (!found)
                        terminal_print("    (none — try `pkg available` and `pkg install <name>`)\n");
                    terminal_print("\n");
                    cmd_pos = 0; desktop_draw(); return;
                }
                // pkg install <name>
                if (k_word(a, "install") || k_word(a, "i") ||
                    k_word(a, "add") || k_word(a, "get")) {
                    const char* nm2 = 0;
                    if (k_word(a, "install")) nm2 = k_word_arg(a, "install");
                    else if (k_word(a, "i"))   nm2 = k_word_arg(a, "i");
                    else if (k_word(a, "add")) nm2 = k_word_arg(a, "add");
                    else if (k_word(a, "get")) nm2 = k_word_arg(a, "get");
                    if (!nm2 || !nm2[0]) {
                        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                        terminal_print("  Usage: pkg install <name|all>\n");
                    } else if (k_word(nm2, "all")) {
                        // Bulk-install every catalog app.
                        terminal_print("  Installing all store apps ...\n");
                        int cnt = pkg_install_all();
                        if (cnt > 0)
                            applib_refresh();  /* launcher list stays current */
                        terminal_set_color(cnt < 0 ? COLOR_LIGHT_RED : COLOR_LIGHT_GREEN,
                                          COLOR_BLACK);
                        terminal_print("  "); terminal_print(pkg_last_error());
                        terminal_print("\n");
                    } else {
                        terminal_print("  Downloading "); terminal_print(nm2);
                        terminal_print(" ...\n");
                        int rc = pkg_install(nm2);
                        if (rc == 0)
                            applib_refresh();  /* launcher list stays current */
                        terminal_set_color(rc ? COLOR_LIGHT_RED : COLOR_LIGHT_GREEN,
                                          COLOR_BLACK);
                        terminal_print("  "); terminal_print(pkg_last_error());
                        terminal_print("\n");
                    }
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                    cmd_pos = 0; desktop_draw(); return;
                }
                // pkg remove / uninstall <name>
                if (k_word(a, "remove") || k_word(a, "rm") ||
                    k_word(a, "uninstall")) {
                    const char* nm2 = 0;
                    if (k_word(a, "remove"))     nm2 = k_word_arg(a, "remove");
                    else if (k_word(a, "rm"))    nm2 = k_word_arg(a, "rm");
                    else if (k_word(a, "uninstall")) nm2 = k_word_arg(a, "uninstall");
                    if (!nm2 || !nm2[0]) {
                        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                        terminal_print("  Usage: pkg remove <name>\n");
                    } else {
                        int rc = pkg_uninstall(nm2);
                        if (rc == 0)
                            applib_refresh();  /* launcher list stays current */
                        terminal_set_color(rc ? COLOR_LIGHT_RED : COLOR_LIGHT_GREEN,
                                          COLOR_BLACK);
                        terminal_print("  "); terminal_print(pkg_last_error());
                        terminal_print("\n");
                    }
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                    cmd_pos = 0; desktop_draw(); return;
                }
                // pkg run <name>
                if (k_word(a, "run") || k_word(a, "launch") || k_word(a, "open")) {
                    const char* nm2 = 0;
                    if (k_word(a, "run"))         nm2 = k_word_arg(a, "run");
                    else if (k_word(a, "launch")) nm2 = k_word_arg(a, "launch");
                    else if (k_word(a, "open"))   nm2 = k_word_arg(a, "open");
                    if (!nm2 || !nm2[0]) {
                        terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                        terminal_print("  Usage: pkg run <name>\n");
                    } else {
                        int id = pkg_run(nm2);
                        if (id >= 0) {
                            terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                            terminal_print("  Launched "); terminal_print(nm2);
                            terminal_print(" at ring 3 (pid=");
                            k_print_u((unsigned)id);
                            terminal_print(")\n  Program output goes to the serial console (COM1).\n");
                        } else {
                            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                            terminal_print("  "); terminal_print(pkg_last_error());
                            terminal_print("\n");
                        }
                    }
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                    cmd_pos = 0; desktop_draw(); return;
                }
                terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                terminal_print("  Unknown pkg subcommand — type `pkg` for help.\n");
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                cmd_pos = 0; desktop_draw(); return;
            }

            // store → open the graphical App Store window
            if (cmd_buffer[0]=='s'&&cmd_buffer[1]=='t'&&cmd_buffer[2]=='o'&&
                cmd_buffer[3]=='r'&&cmd_buffer[4]=='e'&&cmd_buffer[5]=='\0') {
                store_open();
                current_mode = MODE_STORE;
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle clear/cls instantly
            if ((cmd_buffer[0]=='c'&&cmd_buffer[1]=='l'&&cmd_buffer[2]=='e'&&
                 cmd_buffer[3]=='a'&&cmd_buffer[4]=='r'&&cmd_buffer[5]=='\0') ||
                (cmd_buffer[0]=='c'&&cmd_buffer[1]=='l'&&cmd_buffer[2]=='s'&&
                 cmd_buffer[3]=='\0')) {
                desktop_terminal_clear();
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle help inline — formatted command list
            if (cmd_buffer[0]=='h'&&cmd_buffer[1]=='e'&&cmd_buffer[2]=='l'&&
                cmd_buffer[3]=='p'&&cmd_buffer[4]=='\0') {
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("\n  ─── System ───\n");
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print("    help          Show this help\n");
                terminal_print("    date          Current date\n");
                terminal_print("    time          Current time\n");
                terminal_print("    uname         System info\n");
                terminal_print("    version       OS version\n");
                terminal_print("    uptime        System uptime\n");
                terminal_print("    whoami        Current user\n");
                terminal_print("    pwd           Print directory\n");
                terminal_print("    mem           Memory info\n");
                terminal_print("    ps            Process list\n");
                terminal_print("    clear/cls     Clear screen\n");
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  ─── Files ───\n");
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print("    ls            List files\n");
                terminal_print("    cat <file>    View file\n");
                terminal_print("    echo <text>   Print text\n");
                terminal_print("    touch <name>  Create file\n");
                terminal_print("    delf <name>   Delete file\n");

                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  ─── Network ───\n");
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print("    pci list           List PCI devices\n");
                terminal_print("    http init          Init NIC\n");
                terminal_print("    http get <h> <p>   HTTP GET\n");
                terminal_print("    net config <i> <g>  Set IP/gateway\n");
                terminal_print("    net config dns <i>  Set DNS\n");
                terminal_print("    net config dhcp    Auto-config via DHCP\n");
                terminal_print("    net status         Show network config\n");
                terminal_print("    net scan           Scan LAN\n");
                terminal_print("    net ping <h>       Ping a host or IP\n");
                terminal_print("    rawwrite           Send raw Ethernet frame\n");
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  ─── Apps ───\n");
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print("    browser        Launch web browser\n");
                terminal_print("    store          Open the App Store\n");
                terminal_print("    pkg update     Refresh app catalog\n");
                terminal_print("    pkg install <n>  Install app from the store\n");
                terminal_print("    pkg run <n>    Launch an installed app\n");
                terminal_print("  \n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle ls inline
            if (cmd_buffer[0]=='l'&&cmd_buffer[1]=='s'&&cmd_buffer[2]=='\0') {
                terminal_print("\n  User files:\n");
                int found = 0;
                for (int fi = 0; fi < MAX_FILES; fi++) {
                    if (filesystem[fi].used) {
                        terminal_print("    ");
                        terminal_print(filesystem[fi].name);
                        terminal_print("\n");
                        found = 1;
                    }
                }
                if (!found)
                    terminal_print("    (no files)\n");
                terminal_print("\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle echo inline
            if (cmd_buffer[0]=='e'&&cmd_buffer[1]=='c'&&cmd_buffer[2]=='h'&&
                cmd_buffer[3]=='o'&&(cmd_buffer[4]==' '||cmd_buffer[4]=='\0')) {
                terminal_print("  ");
                if (cmd_buffer[4]==' ')
                    terminal_print(cmd_buffer + 5);
                terminal_print("\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle cat <file> inline
            if (cmd_buffer[0]=='c'&&cmd_buffer[1]=='a'&&cmd_buffer[2]=='t'&&
                cmd_buffer[3]==' ') {
                const char* cname = cmd_buffer + 4;
                int found = -1;
                for (int fi = 0; fi < MAX_FILES; fi++) {
                    if (filesystem[fi].used) {
                        int match = 1;
                        for (int ci = 0; cname[ci] && filesystem[fi].name[ci]; ci++) {
                            if (cname[ci] != filesystem[fi].name[ci]) { match = 0; break; }
                        }
                        if (match) { found = fi; break; }
                    }
                }
                if (found >= 0) {
                    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                    terminal_print("  --- "); terminal_print(filesystem[found].name); terminal_print(" ---\n");
                    terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                    terminal_print(filesystem[found].content);
                    terminal_print("\n");
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  File not found.\n");
                }
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle date inline
            if (cmd_buffer[0]=='d'&&cmd_buffer[1]=='a'&&cmd_buffer[2]=='t'&&
                cmd_buffer[3]=='e'&&cmd_buffer[4]=='\0') {
                char ds[9];
                rtc_get_date_str(ds);
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  Date: "); terminal_print(ds); terminal_print("\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle time inline
            if (cmd_buffer[0]=='t'&&cmd_buffer[1]=='i'&&cmd_buffer[2]=='m'&&
                cmd_buffer[3]=='e'&&cmd_buffer[4]=='\0') {
                char ts[9];
                rtc_get_time_str(ts);
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  Time: "); terminal_print(ts); terminal_print("\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle uname inline
            if (cmd_buffer[0]=='u'&&cmd_buffer[1]=='n'&&cmd_buffer[2]=='a'&&
                cmd_buffer[3]=='m'&&cmd_buffer[4]=='e'&&cmd_buffer[5]=='\0') {
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  Nyx OS 0.1 x86 32-bit\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle version inline
            if (cmd_buffer[0]=='v'&&cmd_buffer[1]=='e'&&cmd_buffer[2]=='r'&&
                cmd_buffer[3]=='s'&&cmd_buffer[4]=='i'&&cmd_buffer[5]=='o'&&
                cmd_buffer[6]=='n'&&cmd_buffer[7]=='\0') {
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  Nyx OS v0.1\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle whoami inline
            if (cmd_buffer[0]=='w'&&cmd_buffer[1]=='h'&&cmd_buffer[2]=='o'&&
                cmd_buffer[3]=='a'&&cmd_buffer[4]=='m'&&cmd_buffer[5]=='i'&&
                cmd_buffer[6]=='\0') {
                terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                if (logged_in_user >= 0) {
                    terminal_print(users[logged_in_user].username);
                } else {
                    terminal_print("guest");
                }
                terminal_print("\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle pwd inline
            if (cmd_buffer[0]=='p'&&cmd_buffer[1]=='w'&&cmd_buffer[2]=='d'&&
                cmd_buffer[3]=='\0') {
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  /home/nyx\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle uptime inline
            if (cmd_buffer[0]=='u'&&cmd_buffer[1]=='p'&&cmd_buffer[2]=='t'&&
                cmd_buffer[3]=='i'&&cmd_buffer[4]=='m'&&cmd_buffer[5]=='e'&&
                cmd_buffer[6]=='\0') {
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  Running since boot\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle mem inline
            if (cmd_buffer[0]=='m'&&cmd_buffer[1]=='e'&&cmd_buffer[2]=='m'&&
                cmd_buffer[3]=='\0') {
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("\n  Memory Info:\n");
                terminal_print("  Total  : 512 MB\n");
                terminal_print("  Used   : ~64 MB\n");
                terminal_print("  Free   : ~448 MB\n");
                terminal_print("  Kernel : ~2 MB\n\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle ps inline
            if (cmd_buffer[0]=='p'&&cmd_buffer[1]=='s'&&cmd_buffer[2]=='\0') {
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("\n  PID  Name          Status\n");
                terminal_print("  ---- ------------- -------\n");
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print("  1    kernel        running\n");
                terminal_print("  2    desktop       running\n");
                terminal_print("  3    keyboard      waiting\n");
                terminal_print("  4    timer         running\n");
                terminal_print("  5    shell         waiting\n");
                terminal_print("  6    rtc           running\n\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // ─── Networking commands ──────────────────────────────

            // Handle pci list
            if (cmd_buffer[0]=='p'&&cmd_buffer[1]=='c'&&cmd_buffer[2]=='i'&&
                cmd_buffer[3]==' '&&cmd_buffer[4]=='l'&&cmd_buffer[5]=='i'&&
                cmd_buffer[6]=='s'&&cmd_buffer[7]=='t'&&cmd_buffer[8]=='\0') {
                terminal_print("  Scanning PCI bus...\n");
                net_scan_pci();
                if (net_adapter_count == 0) {
                    terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                    terminal_print("  No PCI network adapters found.\n");
                } else {
                    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                    terminal_print("\n  PCI Network Adapters:\n");
                    for (int ai = 0; ai < net_adapter_count && ai < NET_MAX_ADAPTERS; ai++) {
                        const NetAdapter* na = &net_adapters[ai];
                        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                        terminal_print("  "); terminal_print(na->vendor_name);
                        terminal_print(" "); terminal_print(na->device_name);
                        terminal_print(" (") ;terminal_print(na->type_name);
                        terminal_print(")");
                        if (na->irq_line > 0) {
                            terminal_print("  irq ");
                            char ib[4]; int ip_ = 0, x = na->irq_line;
                            while (x) { ib[ip_++] = (char)('0' + x % 10); x /= 10; }
                            for (int i = 0; i < ip_/2; i++) { char t = ib[i]; ib[i] = ib[ip_-1-i]; ib[ip_-1-i] = t; }
                            ib[ip_] = 0;
                            terminal_print(ib);
                        } else {
                            terminal_print("  irq --");
                        }
                        terminal_print("\n");
                    }
                }
                terminal_print("\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle http init
            if (cmd_buffer[0]=='h'&&cmd_buffer[1]=='t'&&cmd_buffer[2]=='t'&&
                cmd_buffer[3]=='p'&&cmd_buffer[4]==' '&&cmd_buffer[5]=='i'&&
                cmd_buffer[6]=='n'&&cmd_buffer[7]=='i'&&cmd_buffer[8]=='t'&&
                cmd_buffer[9]=='\0') {
                terminal_print("  Initializing NIC...\n");
                int ret = nic_init();
                if (ret) {
                    netstack_init();
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  NIC initialized.\n");
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  NIC init failed. Try 'pci list'.\n");
                }
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle net status
            if (cmd_buffer[0]=='n'&&cmd_buffer[1]=='e'&&cmd_buffer[2]=='t'&&
                cmd_buffer[3]==' '&&cmd_buffer[4]=='s'&&cmd_buffer[5]=='t'&&
                cmd_buffer[6]=='a'&&cmd_buffer[7]=='t'&&cmd_buffer[8]=='u'&&
                cmd_buffer[9]=='s'&&cmd_buffer[10]=='\0') {
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("\n  Network Status:\n");
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                // NIC driver line: mode (irq-driven vs polled) + counters
                if (nic_is_up()) {
                    terminal_print("  NIC:  ");
                    terminal_print(nic_driver_name());
                    terminal_print(" ");
                    if (nic_irq_line() > 0) {
                        terminal_print("IRQ ");
                        char ib[4]; int ip_ = 0, x = nic_irq_line();
                        while (x) { ib[ip_++] = (char)('0' + x % 10); x /= 10; }
                        for (int i = 0; i < ip_/2; i++) { char t = ib[i]; ib[i] = ib[ip_-1-i]; ib[ip_-1-i] = t; }
                        ib[ip_] = 0;
                        terminal_print(ib);
                        terminal_print(" (interrupt-driven)\n");
                    } else {
                        terminal_print("(polled mode)\n");
                    }
                    uint32_t rx_irqs, rx_pkts, rx_drops, tx_done;
                    nic_stats(&rx_irqs, &rx_pkts, &rx_drops, &tx_done);
                    terminal_print("  RX:  ");
                    { char nb[12]; int np_ = 0; uint32_t v = rx_pkts;
                      if (!v) nb[np_++] = '0';
                      while (v) { nb[np_++] = (char)('0' + v % 10); v /= 10; }
                      for (int i = 0; i < np_/2; i++) { char t = nb[i]; nb[i] = nb[np_-1-i]; nb[np_-1-i] = t; }
                      nb[np_] = 0; terminal_print(nb); }
                    terminal_print(" packets via ");
                    { char nb[12]; int np_ = 0; uint32_t v = rx_irqs;
                      if (!v) nb[np_] = '0';
                      while (v) { nb[np_++] = (char)('0' + v % 10); v /= 10; }
                      for (int i = 0; i < np_/2; i++) { char t = nb[i]; nb[i] = nb[np_-1-i]; nb[np_-1-i] = t; }
                      nb[np_] = 0; terminal_print(nb); }
                    terminal_print(" IRQs");
                    if (rx_drops) {
                        terminal_print(" (") ;
                        { char nb[12]; int np_ = 0; uint32_t v = rx_drops;
                          if (!v) nb[np_] = '0';
                          while (v) { nb[np_++] = (char)('0' + v % 10); v /= 10; }
                          for (int i = 0; i < np_/2; i++) { char t = nb[i]; nb[i] = nb[np_-1-i]; nb[np_-1-i] = t; }
                          nb[np_] = 0; terminal_print(nb); }
                        terminal_print(" dropped)");
                    }
                    terminal_print("\n");
                    terminal_print("  TX:  ");
                    { char nb[12]; int np_ = 0; uint32_t v = tx_done;
                      if (!v) nb[np_] = '0';
                      while (v) { nb[np_++] = (char)('0' + v % 10); v /= 10; }
                      for (int i = 0; i < np_/2; i++) { char t = nb[i]; nb[i] = nb[np_-1-i]; nb[np_-1-i] = t; }
                      nb[np_] = 0; terminal_print(nb); }
                    terminal_print(" completions via IRQ\n");
                } else {
                    terminal_print("  NIC:  not initialized — run 'http init'\n");
                }
                uint32_t ip = netstack_get_ip();
                uint32_t dns_ip = netstack_get_dns();
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                terminal_print("  IP:   ");
                for (int bi = 0; bi < 4; bi++) {
                    char n[8]; int np = 0;
                    int b = (ip >> (bi * 8)) & 0xFF;
                    if (b >= 100) n[np++] = '0' + b/100;
                    if (b >= 10) n[np++] = '0' + (b/10)%10;
                    n[np++] = '0' + b%10; n[np] = '\0';
                    terminal_print(n);
                    if (bi < 3) terminal_print(".");
                }
                terminal_print("\n");
                terminal_print("  DNS:  ");
                for (int bi = 0; bi < 4; bi++) {
                    char n[8]; int np = 0;
                    int b = (dns_ip >> (bi * 8)) & 0xFF;
                    if (b >= 100) n[np++] = '0' + b/100;
                    if (b >= 10) n[np++] = '0' + (b/10)%10;
                    n[np++] = '0' + b%10; n[np] = '\0';
                    terminal_print(n);
                    if (bi < 3) terminal_print(".");
                }
                terminal_print("\n\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // ─── More networking commands ─────────────────────────

            // Handle bare "pci" (no args) — same as "pci list"
            if (cmd_buffer[0]=='p'&&cmd_buffer[1]=='c'&&cmd_buffer[2]=='i'&&
                cmd_buffer[3]=='\0') {
                terminal_print("  Scanning PCI bus...\n");
                net_scan_pci();
                if (net_adapter_count == 0) {
                    terminal_set_color(COLOR_YELLOW,COLOR_BLACK);
                    terminal_print("  No PCI network adapters found.\n");
                } else {
                    terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                    terminal_print("\n  PCI Network Adapters:\n");
                    for (int ai = 0; ai < net_adapter_count && ai < NET_MAX_ADAPTERS; ai++) {
                        const NetAdapter* na = &net_adapters[ai];
                        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                        terminal_print("  "); terminal_print(na->vendor_name);
                        terminal_print(" "); terminal_print(na->device_name);
                        terminal_print(" (") ;terminal_print(na->type_name);
                        terminal_print(")");
                        if (na->irq_line > 0) {
                            terminal_print("  irq ");
                            char ib[4]; int ip_ = 0, x = na->irq_line;
                            while (x) { ib[ip_++] = (char)('0' + x % 10); x /= 10; }
                            for (int i = 0; i < ip_/2; i++) { char t = ib[i]; ib[i] = ib[ip_-1-i]; ib[ip_-1-i] = t; }
                            ib[ip_] = 0;
                            terminal_print(ib);
                        } else {
                            terminal_print("  irq --");
                        }
                        terminal_print("\n");
                    }
                }
                terminal_print("\n");
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Helper: parse dotted IP string → uint32_t host-order
            // Returns 0 on failure (or if IP is 0.0.0.0)
            // Actually returns 1 on success, 0 on failure
            // Manual inline parse to avoid function call issues

            // Handle net config <ip> <gw> [dns]
            // Detect by checking for "net config " prefix
            { int _match_nc = 1;
              const char* _nc = "net config ";
              int _ni;
              for (_ni = 0; _nc[_ni]; _ni++)
                  if (cmd_buffer[_ni] != _nc[_ni]) { _match_nc = 0; break; }
              if (_match_nc) {
                // Check for "dhcp" subcommand
                if (cmd_buffer[_ni]=='d'&&cmd_buffer[_ni+1]=='h'&&
                    cmd_buffer[_ni+2]=='c'&&cmd_buffer[_ni+3]=='p'&&
                    cmd_buffer[_ni+4]=='\0') {
                    terminal_print("  DHCP DISCOVER...\n");
                    DhcpResult dr;
                    asm volatile("sti");
                    int dhcp_ok = dhcp_discover(&dr);
                    asm volatile("cli");
                    if (dhcp_ok) {
                        netstack_set_ip(dr.yiaddr, dr.gateway);
                        if (dr.dns) netstack_set_dns(dr.dns);
                        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                        terminal_print("  DHCP configured!\n");
                    } else {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  DHCP failed.\n");
                    }
                    cmd_pos = 0; desktop_draw(); return;
                }
                // Check for "dns " subcommand
                if (cmd_buffer[_ni]=='d'&&cmd_buffer[_ni+1]=='n'&&
                    cmd_buffer[_ni+2]=='s'&&cmd_buffer[_ni+3]==' ') {
                    // Parse DNS IP from after "dns "
                    const char* ds = cmd_buffer + _ni + 4;
                    uint32_t dns_ip = 0;
                    int oct = 0, sh = 0, ok = 1;
                    for (int di = 0; ds[di] && di < 20; di++) {
                        if (ds[di] >= '0' && ds[di] <= '9')
                            oct = oct * 10 + (ds[di] - '0');
                        else if (ds[di] == '.') {
                            dns_ip |= (oct << sh);
                            oct = 0; sh += 8;
                        } else { ok = 0; break; }
                    }
                    if (ok) dns_ip |= (oct << sh);
                    netstack_set_dns(dns_ip);
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  DNS configured.\n");
                    cmd_pos = 0; desktop_draw(); return;
                }
                // Parse IP and gateway
                char ip_str[20], gw_str[20];
                int ip_pos = 0, gw_pos = 0;
                // Read first arg (IP)
                const char* src = cmd_buffer + _ni;
                while (*src && *src != ' ' && ip_pos < 19) ip_str[ip_pos++] = *src++;
                ip_str[ip_pos] = '\0';
                while (*src == ' ') src++;
                while (*src && *src != ' ' && gw_pos < 19) gw_str[gw_pos++] = *src++;
                gw_str[gw_pos] = '\0';
                if (ip_str[0] && gw_str[0]) {
                    uint32_t ip = 0, gw = 0;
                    int oct = 0, sh = 0;
                    for (int di = 0; ip_str[di] && di < 20; di++) {
                        if (ip_str[di] >= '0' && ip_str[di] <= '9') oct = oct*10 + (ip_str[di]-'0');
                        else if (ip_str[di] == '.') { ip |= (oct << sh); oct = 0; sh += 8; }
                    }
                    ip |= (oct << sh);
                    oct = 0; sh = 0;
                    for (int di = 0; gw_str[di] && di < 20; di++) {
                        if (gw_str[di] >= '0' && gw_str[di] <= '9') oct = oct*10 + (gw_str[di]-'0');
                        else if (gw_str[di] == '.') { gw |= (oct << sh); oct = 0; sh += 8; }
                    }
                    gw |= (oct << sh);
                    netstack_set_ip(ip, gw);
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  Network configured.\n");
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Usage: net config <ip> <gw> [dns]\n");
                }
                cmd_pos = 0; desktop_draw(); return;
              }}

            // Handle net scan
            if (cmd_buffer[0]=='n'&&cmd_buffer[1]=='e'&&cmd_buffer[2]=='t'&&
                cmd_buffer[3]==' '&&cmd_buffer[4]=='s'&&cmd_buffer[5]=='c'&&
                cmd_buffer[6]=='a'&&cmd_buffer[7]=='n'&&cmd_buffer[8]=='\0') {
                terminal_print("  Scanning LAN... (takes ~3s)\n");
                // Get subnet from current IP
                uint32_t my_ip = netstack_get_ip();
                uint32_t subnet = my_ip & 0xFFFFFF00;
                asm volatile("sti");
                int count = netstack_arp_scan(subnet);
                asm volatile("cli");
                terminal_set_color(COLOR_LIGHT_CYAN,COLOR_BLACK);
                terminal_print("  Found ");
                char cnt[8]; int cpos = 0; int cv = count;
                if (cv == 0) { cnt[cpos++] = '0'; }
                while (cv > 0) { cnt[cpos++] = '0' + cv%10; cv /= 10; }
                cnt[cpos] = '\0';
                // Reverse
                for (int ci = 0; ci < cpos/2; ci++) {
                    char t = cnt[ci]; cnt[ci] = cnt[cpos-1-ci]; cnt[cpos-1-ci] = t;
                }
                terminal_print(cnt);
                terminal_print(" device(s)\n");
                for (int si = 0; si < count && si < ARP_SCAN_MAX; si++) {
                    const ArpScanResult* r = netstack_scan_result(si);
                    if (r) {
                        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                        terminal_print("  ");
                        for (int bi = 0; bi < 4; bi++) {
                            char n[8]; int np = 0;
                            int b = (r->ip >> (bi * 8)) & 0xFF;
                            if (b >= 100) n[np++] = '0' + b/100;
                            if (b >= 10) n[np++] = '0' + (b/10)%10;
                            n[np++] = '0' + b%10; n[np] = '\0';
                            terminal_print(n);
                            if (bi < 3) terminal_print(".");
                        }
                        terminal_print("\n");
                    }
                }
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle net ping <host|ip> — ICMP echo client
            if (cmd_buffer[0]=='n'&&cmd_buffer[1]=='e'&&cmd_buffer[2]=='t'&&
                cmd_buffer[3]==' '&&cmd_buffer[4]=='p'&&cmd_buffer[5]=='i'&&
                cmd_buffer[6]=='n'&&cmd_buffer[7]=='g'&&cmd_buffer[8]==' ') {
                const char* host = cmd_buffer + 9;
                // Resolve: dotted quad → wire-LE uint32, else DNS.
                uint32_t ip = 0;
                int is_ip = 1, seg = 0, val = 0, parts = 0;
                for (const char* p = host; *p; p++) {
                    if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); }
                    else if (*p == '.' && seg < 4) {
                        ip |= (uint32_t)val << (8 * parts++); val = 0;
                    }
                    else { is_ip = 0; break; }
                }
                if (is_ip && *host && val <= 255) { ip |= (uint32_t)val << (8 * parts); }
                else { is_ip = 0; }
                if (!is_ip) {
                    ip = dns_resolve(host);
                    if (ip == 0) {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  Could not resolve host\n");
                        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                        cmd_pos = 0; desktop_draw(); return;
                    }
                    { const char* m = "PING DNS OK\n";
                      for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
                }
                terminal_print("  Pinging "); terminal_print(host); terminal_print("...\n");
                asm volatile("sti");
                extern volatile uint32_t tick;
                for (int pi = 0; pi < 3; pi++) {
                    if (net_icmp_echo(ip, (uint16_t)(pi + 1)) < 0) {
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  Send failed\n");
                        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                        break;
                    }
                    uint32_t dline = tick + 75;   // ~1.5 s (tick = 50 Hz)
                    while (!net_icmp_reply_received() && (int)(tick - dline) < 0)
                        for (int p = 0; p < 100; p++) netstack_poll();
                    if (net_icmp_reply_received()) {
                        { const char* m = "PING REPLY OK\n";
                          for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
                        terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                        terminal_print("  Reply from ");
                        for (int bi = 0; bi < 4; bi++) {
                            char nb[8]; int np = 0;
                            int b = (ip >> (bi * 8)) & 0xFF;
                            if (b >= 100) nb[np++] = '0' + b/100;
                            if (b >= 10) nb[np++] = '0' + (b/10)%10;
                            nb[np++] = '0' + b%10; nb[np] = '\0';
                            terminal_print(nb);
                            if (bi < 3) terminal_print(".");
                        }
                        terminal_print(": time=");
                        { uint32_t rtt = net_icmp_reply_rtt_us() / 1000;
                          char rb[12]; int rp = 0;
                          if (rtt == 0) rb[rp++] = '0';
                          while (rtt) { rb[rp++] = (char)('0' + rtt % 10); rtt /= 10; }
                          rb[rp] = '\0';
                          for (int ri = 0; ri < rp/2; ri++) { char t = rb[ri]; rb[ri] = rb[rp-1-ri]; rb[rp-1-ri] = t; }
                          terminal_print(rb); }
                        terminal_print(" ms\n");
                    } else {
                        terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                        terminal_print("  Request timed out.\n");
                    }
                    net_icmp_reply_clear();
                    uint32_t gap = tick + 5;   // ~100 ms between pings
                    while ((int)(tick - gap) < 0) for (int p = 0; p < 100; p++) netstack_poll();
                }
                terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle http get bare minimum (trigger fetch)
            if (cmd_buffer[0]=='h'&&cmd_buffer[1]=='t'&&cmd_buffer[2]=='t'&&
                cmd_buffer[3]=='p'&&cmd_buffer[4]==' '&&cmd_buffer[5]=='g'&&
                cmd_buffer[6]=='e'&&cmd_buffer[7]=='t'&&cmd_buffer[8]==' ') {
                terminal_print("  HTTP GET (may take ~3s)...\n");
                // Parse host[:port] and path
                const char* hstart = cmd_buffer + 9;
                char host[64]; int hp = 0;
                while (hstart[hp] && hstart[hp] != ' ' && hp < 63) {
                    host[hp] = hstart[hp]; hp++;
                }
                host[hp] = '\0';
                int http_port = 80;
                for (int hi = 0; host[hi]; hi++) {
                    if (host[hi] == ':') {
                        int pv = 0;
                        for (int pj = hi + 1; host[pj] >= '0' && host[pj] <= '9'; pj++)
                            pv = pv * 10 + (host[pj] - '0');
                        if (pv > 0) http_port = pv;
                        host[hi] = '\0';
                        break;
                    }
                }
                const char* path = hstart + hp;
                while (*path == ' ') path++;
                if (!*path) path = "/";
                
                if (host[0]) {
                    // Dotted IPs are used directly; everything else goes
                    // through DNS (may block ~1s)
                    uint32_t resolved = 0;
                    int hdp = 0, hdots = 0, hdigits = 0;
                    const char* hc = host;
                    while (hc[hdp]) {
                        if (hc[hdp] == '.') hdots++;
                        else if (hc[hdp] >= '0' && hc[hdp] <= '9') hdigits++;
                        else { hdots = -1; break; }
                        hdp++;
                    }
                    if (hdots == 3 && hdigits + hdots == hdp) {
                        // dotted quad: build LE wire IP (10.0.2.2 → 0x0202000A)
                        int oct = 0, val = 0;
                        for (const char* cp = host; ; cp++) {
                            if (*cp >= '0' && *cp <= '9') val = val * 10 + (*cp - '0');
                            else {
                                resolved |= (uint32_t)(val & 0xFF) << (8 * oct);
                                oct++; val = 0;
                                if (*cp != '.' || oct > 3) break;
                            }
                        }
                    } else {
                        asm volatile("sti");
                        resolved = dns_resolve(host);
                        asm volatile("cli");
                    }
                    if (resolved) {
                        HttpResponse* resp = http_get_global_response();
                        asm volatile("sti");
                        int ret = http_get(resolved, (uint16_t)http_port, host, path, resp);
                        asm volatile("cli");
                        if (ret == 0 && resp->status_code == 200) {
                            // Serial marker (E2E test hook): success + code
                            { const char* m = "HTTP OK ";
                              for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
                            { int code = resp->status_code; char nb[6]; int np=0;
                              while (code) { nb[np++] = (char)('0' + code%10); code/=10; }
                              for (int i=np-1;i>=0;i--){ int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)nb[i]); } }
                            { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,'\n'); }
                            // Serial marker: first 60 bytes of the body, so
                            // E2E tests can assert the actual rendered content
                            // (e.g. that gzip decoding produced the right text)
                            { const char* m = "HTTP BODY: ";
                              for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
                            { int bl = resp->body_len; if (bl > 60) bl = 60;
                              for (int bi = 0; bi < bl; bi++) {
                                  char bc = resp->body[bi];
                                  if (bc == '\n' || bc == '\r' || bc == '\t') bc = ' ';
                                  if (bc == 0) break;
                                  int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)bc);
                              } }
                            { const char* m = " (len ";
                              for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
                            { int v = resp->body_len; char nb[8]; int np=0;
                              if (!v) nb[np++] = '0';
                              while (v) { nb[np++] = (char)('0' + v%10); v/=10; }
                              for (int i=np-1;i>=0;i--){ int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)nb[i]); } }
                            { const char* m = ")\n";
                              for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
                            terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                            terminal_print("  HTTP 200 OK\n");
                            terminal_set_color(COLOR_WHITE,COLOR_BLACK);
                            terminal_print(resp->body);
                            terminal_print("\n");
                        } else {
                            { const char* m = "HTTP FAIL\n";
                              for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
                            terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                            terminal_print("  HTTP request failed.\n");
                        }
                    } else {
                        { const char* m = "HTTP DNS FAIL: ";
                          for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
                        { const char* e = dns_get_last_error();
                          for (; *e; e++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*e); } }
                        { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,'\n'); }
                        terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                        terminal_print("  Could not resolve host.\n");
                    }
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Usage: http get <host> <path>\n");
                }
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle rawwrite — Phase B raw-packet test: craft a
            // single Ethernet frame by hand and push it straight to
            // the NIC, bypassing the netstack. The frame carries a
            // recognizable ASCII payload so it's easy to spot in a
            // Wireshark capture (QEMU `-object filter-dump`).
            if (cmd_buffer[0]=='r'&&cmd_buffer[1]=='a'&&cmd_buffer[2]=='w'&&
                cmd_buffer[3]=='w'&&cmd_buffer[4]=='r'&&cmd_buffer[5]=='i'&&
                cmd_buffer[6]=='t'&&cmd_buffer[7]=='e'&&cmd_buffer[8]=='\0') {
                if (!nic_is_up()) {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  NIC not initialized — run 'http init' first.\n");
                    cmd_pos = 0; desktop_draw(); return;
                }
                // ── Craft the raw frame ────────────────────────────
                uint8_t raw[14 + 32];
                // Destination: gateway MAC (defaults to broadcast
                // FF:FF:FF:FF:FF:FF until ARP resolves it).
                const uint8_t* dmac = netstack_gateway_mac();
                for (int k = 0; k < 6; k++) raw[k] = dmac[k];
                // Source: our own MAC from the NIC driver.
                uint8_t smac[6];
                nic_get_mac(smac);
                for (int k = 0; k < 6; k++) raw[6 + k] = smac[k];
                // Ethernet type: 0x88B5 (local/experimental) so the
                // frame is easy to find in Wireshark by eth.type.
                raw[12] = 0x88; raw[13] = 0xB5;
                // Payload: recognizable ASCII marker.
                const char* pl = "NYX-RAW-PACKET-TEST-0001";
                int plen = 0; while (pl[plen]) { raw[14 + plen] = (uint8_t)pl[plen]; plen++; }
                int framelen = 14 + plen;

                int sent = nic_send(raw, framelen);
                if (sent > 0) {
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  Raw frame sent (") ;
                    { char fb[6]; int fp=0, fv=sent; if(!fv) fb[fp++]='0';
                      while (fv) { fb[fp++]=(char)('0'+fv%10); fv/=10; }
                      for (int ci=0; ci<fp/2; ci++){char t=fb[ci];fb[ci]=fb[fp-1-ci];fb[fp-1-ci]=t;}
                      fb[fp]=0; terminal_print(fb); }
                    terminal_print(" bytes)\n");
                    // Serial marker for the E2E test.
                    { const char* m = "RAW TX OK\n";
                      for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Raw send failed.\n");
                    { const char* m = "RAW TX FAIL\n";
                      for (; *m; m++) { int sp=0; while ((inb(0x3F8+5)&0x20)==0 && sp++<200000){} outb(0x3F8,(uint8_t)*m); } }
                }
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // ─── File management commands ─────────────────────────

            // Handle touch <name>
            if (cmd_buffer[0]=='t'&&cmd_buffer[1]=='o'&&cmd_buffer[2]=='u'&&
                cmd_buffer[3]=='c'&&cmd_buffer[4]=='h'&&cmd_buffer[5]==' ') {
                const char* fname = cmd_buffer + 6;
                int slot = -1;
                for (int fi = 0; fi < MAX_FILES; fi++)
                    if (!filesystem[fi].used) { slot = fi; break; }
                if (slot < 0) {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Filesystem full!\n");
                } else {
                    int ni = 0;
                    while (fname[ni] && ni < MAX_FILENAME-1) {
                        filesystem[slot].name[ni] = fname[ni];
                        ni++;
                    }
                    filesystem[slot].name[ni] = '\0';
                    filesystem[slot].content[0] = '\0';
                    filesystem[slot].size = 0;
                    filesystem[slot].used = 1;
                    file_count++;
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  Created: "); terminal_print(fname); terminal_print("\n");
                }
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle delf <name>
            if (cmd_buffer[0]=='d'&&cmd_buffer[1]=='e'&&cmd_buffer[2]=='l'&&
                cmd_buffer[3]=='f'&&cmd_buffer[4]==' ') {
                const char* fname = cmd_buffer + 5;
                if (!fname[0]) {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  Usage: delf <filename>\n");
                    cmd_pos = 0; desktop_draw(); return;
                }
                int found = -1;
                for (int fi = 0; fi < MAX_FILES; fi++) {
                    if (filesystem[fi].used) {
                        int match = 1;
                        for (int ci = 0; fname[ci] && filesystem[fi].name[ci]; ci++) {
                            if (fname[ci] != filesystem[fi].name[ci]) { match = 0; break; }
                        }
                        if (match) { found = fi; break; }
                    }
                }
                if (found >= 0) {
                    filesystem[found].used = 0;
                    file_count--;
                    terminal_set_color(COLOR_LIGHT_GREEN,COLOR_BLACK);
                    terminal_print("  Deleted.\n");
                } else {
                    terminal_set_color(COLOR_LIGHT_RED,COLOR_BLACK);
                    terminal_print("  File not found.\n");
                }
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle browser command — launch the web browser app
            if (cmd_buffer[0]=='b'&&cmd_buffer[1]=='r'&&cmd_buffer[2]=='o'&&
                cmd_buffer[3]=='w'&&cmd_buffer[4]=='s'&&cmd_buffer[5]=='e'&&
                cmd_buffer[6]=='r'&&cmd_buffer[7]=='\0') {
                browser_init();
                current_mode = MODE_BROWSER;
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Handle user / userfault — launch the ring-3 user program
            if (user_prog_launch(cmd_buffer)) {
                cmd_pos = 0;
                desktop_draw();
                return;
            }

            // Unknown command
            terminal_set_color(COLOR_WHITE,COLOR_BLACK);
            terminal_print("  Unknown command. Try: help\n");
            cmd_pos = 0;
            desktop_draw();
            return;
        }

        // ── Clipboard shortcuts (Terminal) ──────────────────────
        // Ctrl+C (3) → copy command buffer to clipboard
        if (c == 3) {
            if (cmd_pos > 0) {
                clipboard_set(cmd_buffer);
                terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
                terminal_print("  [copied]");
                terminal_set_color(COLOR_WHITE, COLOR_BLACK);
                terminal_print("");
                desktop_draw();
            }
            return;
        }
        // Ctrl+X (24) → cut command buffer to clipboard
        if (c == 24) {
            if (cmd_pos > 0) {
                clipboard_set(cmd_buffer);
                cmd_pos = 0;
                cmd_buffer[0] = '\0';
                terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
                terminal_print("  [cut]");
                terminal_set_color(COLOR_WHITE, COLOR_BLACK);
                terminal_print("");
                desktop_draw();
            }
            return;
        }
        // Ctrl+V (22) → paste clipboard into command buffer
        if (c == 22) {
            const char* clip = clipboard_get();
            if (clip) {
                int clen = 0;
                while (clip[clen]) clen++;
                // Paste only the first line (up to newline or end)
                int pasted = 0;
                for (int i = 0; i < clen && clip[i] != '\n'; i++) {
                    if (cmd_pos < 255) {
                        cmd_buffer[cmd_pos++] = clip[i];
                        pasted++;
                    }
                }
                cmd_buffer[cmd_pos] = '\0';
                if (pasted > 0) {
                    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
                    terminal_print("  [pasted]");
                    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
                    terminal_print("");
                }
                desktop_draw();
            }
            return;
        }

        // Regular printable character
        if (c >= 32 && c < 127 && cmd_pos < 255) {
            cmd_buffer[cmd_pos++] = c;
            cmd_buffer[cmd_pos] = '\0';
            desktop_draw();
            return;
        }

        // Unknown character in terminal mode — ignore silently
        return;
    }

    // ─── All other modes → desktop key handler ─────────────────
    desktop_handle_key(c);
}

void keyboard_install() {
    // Flush any stale PS/2 scancodes left by GRUB/BIOS.
    // Bounded to 32 reads; also has an output-buffer-empty check
    // so it can't spin forever on stuck hardware.
    for (int i = 0; i < 32; i++) {
        if ((inb(0x64) & 1) == 0) break;
        inb(0x60);
    }
    irq_install_handler(1, keyboard_callback);
}
