// cpu/store.c — Nyx App Store window.
//
// Graphical front-end for the package manager (cpu/pkg.c): shows the
// online catalog fetched from the host repo (tools/pkg_store.py) as a
// selectable shelf, with install / run / remove actions per app.
// Keyboard driven (like most Nyx windows): Up/Down select, ENTER
// installs-or-runs the selected app, U refreshes the catalog, D
// uninstalls, ESC closes (routed in desktop.c).
#include <store.h>
#include <desktop.h>
#include <vesa.h>
#include <mouse.h>
#include <applib.h>
#include <pkg.h>
#include <fs.h>
#include <stdint.h>
#include <stddef.h>

// Set when the last ENTER installed+ran (or ran) a package, so
// desktop.c can jump to the Terminal to show the app running.
static int st_auto_launch = 0;

int store_take_auto_launch(void) {
    int v = st_auto_launch;
    st_auto_launch = 0;
    return v;
}

// ─── Colors ───────────────────────────────────────────────────────
#define ST_BG      0x0B0A1E
#define ST_PANEL   0x151233
#define ST_BORDER  0x6E5BD0
#define ST_TITLE0  0x241A52
#define ST_TITLE1  0x3A2C86
#define ST_HEADING 0xA79AFF
#define ST_TEXT    0xDDD6F2
#define ST_DIM     0x6E6399
#define ST_SEL     0x2A2160
#define ST_SELROW  0x241B52
#define ST_GREEN   0x55FF88
#define ST_RED     0xFF5566
#define ST_AMBER   0xFFB84D
#define ST_ACCENT  0x8A7BFF
#define ST_HINT    0x3A2C66

// ─── VGA text helpers (self-contained fallback) ──────────────────
#define ST_VGAW 80
#define ST_VGAH 25
#define ST_VGAM ((uint16_t*)0xB8000)
static void st_vp(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if (x < 0 || x >= ST_VGAW || y < 0 || y >= ST_VGAH) return;
    ST_VGAM[y * ST_VGAW + x] = (uint16_t)c | (uint16_t)(fg | bg << 4) << 8;
}
static void st_vs(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
    for (int i = 0; s[i]; i++) st_vp(x + i, y, s[i], fg, bg);
}
static void st_vfill(int x, int y, int w, int h, char c, uint8_t fg, uint8_t bg) {
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++) st_vp(x + dx, y + dy, c, fg, bg);
}

// ─── Local helpers ────────────────────────────────────────────────
static int st_len(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void st_num(unsigned v, char* out) {   // decimal into out[]
    char tmp[12]; int p = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0) { tmp[p++] = (char)('0' + v % 10); v /= 10; }
    int i = 0;
    while (p > 0) out[i++] = tmp[--p];
    out[i] = 0;
}

static char st_status[128] = "Press U to fetch the catalog from the store.";
static int  st_sel = 0;

// ─── Window state ────────────────────────────────────────────────
static int st_open_once = 0;

// Fetch the catalog with a few retries — the NIC may still be coming
// up right after login, and a single immediate attempt often fails
// with "download failed (no route)".  Retrying here makes the store
// open reliably instead of showing a stale error shelf.
static int st_fetch_catalog(void) {
    int n = pkg_update();
    for (int try = 0; n < 0 && try < 3; try++) {
        // brief pause so the link/ARP settles before the next attempt
        for (volatile int d = 0; d < 4000000; d++) {}
        n = pkg_update();
    }
    return n;
}

void store_open(void) {
    st_open_once = 1;
    // Refresh silently so the shelf is current — unless we already
    // have a catalog (keeps opening fast after the first time).
    if (pkg_catalog_count() == 0) {
        int n = st_fetch_catalog();
        if (n < 0) {
            char tmp[96];
            int i = 0;
            const char* pre = "Store: ";
            while (*pre) tmp[i++] = *pre++;
            const char* e = pkg_last_error();
            while (*e && i < (int)sizeof(tmp) - 1) tmp[i++] = *e++;
            tmp[i] = 0;
            for (int j = 0; tmp[j] && j < (int)sizeof(st_status) - 1; j++)
                st_status[j] = tmp[j];
            st_status[st_len(tmp) > (int)sizeof(st_status) - 1 ?
                      (int)sizeof(st_status) - 1 : st_len(tmp)] = 0;
        } else {
            char tmp[64];
            int i = 0;
            const char* pre = "Catalog loaded: ";
            while (*pre) tmp[i++] = *pre++;
            st_num((unsigned)n, tmp + i);
            while (tmp[i]) i++;
            const char* rest = " apps. Use ENTER to install.";
            while (*rest && i < (int)sizeof(tmp) - 1) tmp[i++] = *rest++;
            tmp[i] = 0;
            for (int j = 0; j <= i && j < (int)sizeof(st_status); j++)
                st_status[j] = tmp[j];
        }
    }
}

// ─── Row geometry ────────────────────────────────────────────────
#define ST_ROW_H 34

static int st_visible_rows(int list_h) {
    int v = list_h / ST_ROW_H;
    if (v < 1) v = 1;
    return v;
}

static void st_panel(int x, int y, int w, int h, uint32_t c) {
    vesa_fill_rect(x + 1, y + 1, w - 2, h - 2, c);
    vesa_draw_rect(x, y, w, h, ST_BORDER);
}

// Status chip for the shelf rows.
static void st_chip(int x, int y, const char* label, uint32_t col) {
    vesa_draw_string(x, y + 3, label, col, 0);
}

// ─── VESA drawing ────────────────────────────────────────────────
static void st_vesa_draw(void) {
    int W = (int)vesa.width, H = (int)vesa.height;
    int cx = W / 10, cy = 42;                 // below the top taskbar
    int cw = W * 8 / 10, ch = H - cy - 14;
    g_win_x = cx; g_win_y = cy; g_win_w = cw; g_win_h = ch;

    vesa_fill_rect(0, 0, W, H, ST_BG);
    st_panel(cx, cy, cw, ch, ST_PANEL);

    // Title bar (gradient) + close button
    int th = 36;
    for (int dy = 0; dy < th; dy++) {
        int t = dy * 255 / th;
        int r = ((int)(ST_TITLE0 >> 16 & 0xFF) * (255 - t) +
                 (int)(ST_TITLE1 >> 16 & 0xFF) * t) / 255;
        int g = ((int)(ST_TITLE0 >> 8 & 0xFF) * (255 - t) +
                 (int)(ST_TITLE1 >> 8 & 0xFF) * t) / 255;
        int b = ((int)(ST_TITLE0 & 0xFF) * (255 - t) +
                 (int)(ST_TITLE1 & 0xFF) * t) / 255;
        vesa_hline(cx + 1, cy + 1 + dy, cw - 2, (uint32_t)(r << 16 | g << 8 | b));
    }
    vesa_hline(cx + 1, cy + th, cw - 2, ST_BORDER);
    vesa_draw_string(cx + 18, cy + 10, "Nyx App Store", 0xEDE6FF, 0);
    draw_close_btn(cx + cw - 32, cy + 10, ST_TITLE1, "App Store");

    int body_x = cx + 14, body_y = cy + th + 12;
    int body_w = cw - 28;
    int body_h = ch - th - 24;

    int n = pkg_catalog_count();

    // Status line under the title
    vesa_draw_string(body_x, body_y - 6, st_status,
                     st_status[0] == 'S' && st_status[1] == 't' &&
                     st_status[2] == 'o' && st_status[3] == 'r' &&
                     st_status[4] == 'e' ? ST_AMBER : ST_HEADING, 0);

    if (n == 0) {
        int y0 = cy + ch / 2 - 10;
        vesa_draw_string(cx + cw / 2 - 150, y0,
                         "The store is empty — networking may be down.", ST_DIM, 0);
        vesa_draw_string(cx + cw / 2 - 120, y0 + 24,
                         "Press U to retry, or run `pkg update` in the Terminal.", ST_DIM, 0);
        goto st_footer;
    }

    if (st_sel >= n) st_sel = n - 1;
    if (st_sel < 0) st_sel = 0;

    // ── Left: shelf list ────────────────────────────────────────
    int lw = body_w * 55 / 100;
    int list_y = body_y + 22;
    int list_h = body_h - 22 - 30;
    // column headers
    vesa_draw_string(body_x, body_y + 12, "App", ST_HEADING, 0);
    vesa_draw_string(body_x + lw - 220, body_y + 12, "Version", ST_HEADING, 0);
    vesa_draw_string(body_x + lw - 120, body_y + 12, "Size", ST_HEADING, 0);
    vesa_draw_string(body_x + lw - 52, body_y + 12, "State", ST_HEADING, 0);
    vesa_hline(body_x, body_y + 22, lw, 0x2E2660);

    int vis = st_visible_rows(list_h);
    // scrolling: page so st_sel stays visible
    int top = 0;
    if (st_sel >= top + vis) top = st_sel - vis + 1;
    if (st_sel < top) top = st_sel;
    for (int r = 0; r < vis; r++) {
        int idx = top + r;
        if (idx >= n) break;
        const PkgEntry* e = pkg_catalog_entry(idx);
        int ry = list_y + 6 + r * ST_ROW_H;
        int inst = pkg_is_installed(e->name);

        if (idx == st_sel) {
            // selected row backdrop + accent bar
            vesa_fill_rect(body_x, ry, lw, ST_ROW_H - 6, ST_SELROW);
            vesa_fill_rect(body_x, ry, 4, ST_ROW_H - 6, ST_ACCENT);
            vesa_draw_string(body_x + 12, ry + 5, e->name, 0xFFFFFF, 0);
        } else {
            vesa_draw_string(body_x + 12, ry + 5, e->name, ST_TEXT, 0);
        }
        // version / size columns
        char vb[16]; st_num((unsigned)(idx + 1), vb); // placeholder not used
        vesa_draw_string(body_x + lw - 220, ry + 5, e->version, ST_DIM, 0);
        char sb[16];
        st_num(e->size > 0 ? (e->size + 1023) / 1024 : 0, sb);
        char sizebuf[24];
        int si = 0;
        while (sb[si]) { sizebuf[si] = sb[si]; si++; }
        sizebuf[si++] = ' '; sizebuf[si++] = 'K'; sizebuf[si++] = 'B';
        sizebuf[si] = 0;
        vesa_draw_string(body_x + lw - 120, ry + 5, sizebuf, ST_DIM, 0);
        st_chip(body_x + lw - 52, ry + 5, inst ? "INSTALLED" : "available",
                inst ? ST_GREEN : 0x4A4470);
        (void)vb;
    }
    // scroll hint
    if (n > vis) {
        vesa_draw_string(body_x, list_y + vis * ST_ROW_H + 4,
                         n > vis ? "(scroll for more)" : "", ST_HINT, 0);
    }

    // ── Right: details of the selected app ─────────────────────
    int rx = cx + 14 + lw + 18;
    int rw = body_w - lw - 18;
    if (rw > 40) {
        const PkgEntry* e = pkg_catalog_entry(st_sel);
        int inst = pkg_is_installed(e->name);
        int dy = body_y + 12;
        vesa_draw_string(rx, dy, e->name, 0xFFFFFF, 0);
        dy += 28;
        char buf[128];
        int bi = 0;
        const char* pre = "Version   ";
        while (*pre) buf[bi++] = *pre++;
        const char* p = e->version;
        while (*p) buf[bi++] = *p++;
        buf[bi] = 0;
        vesa_draw_string(rx, dy, buf, ST_TEXT, 0); dy += 22;
        bi = 0; pre = "Author    ";
        while (*pre) buf[bi++] = *pre++;
        p = e->author;
        while (*p) buf[bi++] = *p++;
        buf[bi] = 0;
        vesa_draw_string(rx, dy, buf, ST_TEXT, 0); dy += 22;
        bi = 0; pre = "Size      ";
        while (*pre) buf[bi++] = *pre++;
        st_num(e->size, buf + bi);
        while (buf[bi]) bi++;
        buf[bi++] = ' '; buf[bi++] = 'b'; buf[bi++] = 'y'; buf[bi++] = 't'; buf[bi++] = 'e'; buf[bi++] = 's';
        buf[bi] = 0;
        vesa_draw_string(rx, dy, buf, ST_TEXT, 0); dy += 22;
        bi = 0; pre = "State     ";
        while (*pre) buf[bi++] = *pre++;
        // simple format without sprintf
        const char* q = inst ? "installed in " : "available from store";
        if (inst) {
            while (*q) buf[bi++] = *q++;
            buf[bi++] = '@';
            p = e->name;
            while (*p) buf[bi++] = *p++;
        } else {
            while (*q) buf[bi++] = *q++;
        }
        buf[bi] = 0;
        vesa_draw_string(rx, dy, buf, inst ? ST_GREEN : ST_DIM, 0); dy += 28;

        vesa_hline(rx, dy - 10, rw, 0x2E2660);
        // description (word-wrapped)
        int dl = st_len(e->desc);
        int row = 0, di = 0;
        while (di < dl && row < 4) {
            int cut = dl - di;
            if (cut > (int)(rw / 7)) cut = rw / 7;
            if (di + cut < dl) {
                int ls = 0;
                for (int i = 0; i < cut; i++) if (e->desc[di + i] == ' ') ls = i;
                if (ls > 0) cut = ls;
            }
            char ln[96];
            int lp = 0;
            for (int i = 0; i < cut && lp < (int)sizeof(ln) - 1; i++)
                ln[lp++] = e->desc[di + i];
            ln[lp] = 0;
            vesa_draw_string(rx, dy + row * 20, ln, ST_TEXT, 0);
            row++;
            di += cut;
            if (e->desc[di] == ' ') di++;
        }

        // action buttons box
        int ay = dy + row * 20 + 12;
        vesa_fill_rect(rx, ay, rw - 10, 74, ST_SEL);
        vesa_draw_rect(rx, ay, rw - 10, 74, ST_ACCENT);
        char line[128];
        int li = 0;
        const char* act = inst ? "ENTER  Run (ring 3)" : "ENTER  Install + Run";
        while (*act) line[li++] = *act++;
        line[li] = 0;
        vesa_draw_string(rx + 12, ay + 10, line, inst ? ST_GREEN : ST_ACCENT, 0);
        li = 0; act = inst ? "D      Uninstall" : "D      (nothing to remove)";
        while (*act) line[li++] = *act++;
        line[li] = 0;
        vesa_draw_string(rx + 12, ay + 30, line, ST_DIM, 0);
        li = 0; act = "U      Refresh catalog";
        while (*act) line[li++] = *act++;
        line[li] = 0;
        vesa_draw_string(rx + 12, ay + 50, line, ST_DIM, 0);
    }

st_footer:
    // Footer hint bar
    {
        int fy = cy + ch - 26;
        vesa_fill_rect(cx + 1, fy, cw - 2, 25, 0x100D28);
        vesa_hline(cx + 1, fy, cw - 2, 0x2E2660);
        const char* hint = "Up/Down select  ENTER install/run  I install all  U refresh  D remove  ESC close";
        vesa_draw_string(cx + 14, fy + 6, hint, ST_HINT, 0);
    }
}

// ─── VGA fallback ────────────────────────────────────────────────
static void st_vga_draw(void) {
    st_vfill(0, 0, ST_VGAW, ST_VGAH, ' ', 0, 0);
    st_vfill(0, 0, ST_VGAW, 1, ' ', 15, 5);
    st_vs(2, 0, "[ Nyx App Store ]", 15, 5);
    int n = pkg_catalog_count();
    if (n == 0) {
        st_vs(2, 3, "Catalog empty — run `pkg update` or press U.", 14, 0);
    } else {
        if (st_sel >= n) st_sel = n - 1;
        if (st_sel < 0) st_sel = 0;
        st_vs(2, 2, "App                    Version     Size   State", 11, 0);
        for (int r = 0; r < 20 && r < n; r++) {
            int idx = r;
            const PkgEntry* e = pkg_catalog_entry(idx);
            uint8_t bg = (idx == st_sel) ? 4 : 0;
            uint8_t fg = (idx == st_sel) ? 15 : 7;
            st_vfill(2, 3 + r, 76, 1, ' ', fg, bg);
            st_vs(2, 3 + r, e->name, fg, bg);
            st_vs(30, 3 + r, e->version, fg, bg);
            char sb[16]; st_num((e->size + 1023) / 1024, sb);
            st_vs(40, 3 + r, sb, fg, bg);
            st_vs(48, 3 + r, pkg_is_installed(e->name) ? "installed" : "avail", fg, bg);
        }
        st_vs(2, 23, "Up/Down:sel ENTER:install/run I:all U:refresh D:remove", 7, 0);
    }
    st_vs(2, 24, "ESC: back to desktop", 8, 0);
}

void store_draw(void) {
    if (vesa.active) st_vesa_draw();
    else st_vga_draw();
}

// ─── Bulk-install progress hook ──────────────────────────────────
// Called by pkg_install_all() before each app's download (main-thread
// context).  Pushes "Installing <name> (i of N)..." into the status
// line and repaints immediately, so the shelf shows live progress
// instead of freezing until the whole batch finishes.
static void st_progress_cb(const char* name, int index, int total) {
    char tmp[128]; int i = 0;
    const char* pre = "Installing ";
    while (*pre && i < 120) tmp[i++] = *pre++;
    while (*name && i < 120) tmp[i++] = *name++;
    tmp[i++] = ' '; tmp[i++] = '(';
    st_num((unsigned)index, tmp + i);
    while (tmp[i]) i++;
    const char* mid = " of ";
    while (*mid && i < 120) tmp[i++] = *mid++;
    st_num((unsigned)total, tmp + i);
    while (tmp[i]) i++;
    tmp[i++] = ')'; tmp[i++] = '.'; tmp[i++] = '.'; tmp[i++] = '.';
    tmp[i] = 0;
    for (int j = 0; j <= i && j < (int)sizeof(st_status) - 1; j++)
        st_status[j] = tmp[j];
    // Repaint now so the user sees the update mid-batch.
    store_draw();
    if (vesa.active) vesa_present();
}

// ─── Key handling ────────────────────────────────────────────────
void store_handle_key(char c) {
    st_auto_launch = 0;
    int n = pkg_catalog_count();
    if (n <= 0) {
        // With no catalog, U retries, I fetches + installs everything,
        // everything else does nothing.
        if (c == 'i' || c == 'I') {
            int rc = st_fetch_catalog();
            if (rc < 0) {
                int i = 0;
                const char* e = pkg_last_error();
                while (*e && i < (int)sizeof(st_status) - 1) st_status[i++] = *e++;
                st_status[i] = 0;
                return;
            }
            st_sel = 0;
            pkg_set_progress_fn(st_progress_cb);
            int cnt = pkg_install_all();
            pkg_set_progress_fn(0);
            if (cnt > 0) applib_refresh();
            int i = 0;
            const char* e = pkg_last_error();
            while (*e && i < (int)sizeof(st_status) - 1) st_status[i++] = *e++;
            st_status[i] = 0;
            return;
        }
        if (c == 'u' || c == 'U') {
            int rc = pkg_update();
            char tmp[96];
            int i = 0;
            const char* e = rc < 0 ? pkg_last_error() :
                            (rc == 0 ? "repo returned no apps" : "catalog loaded");
            while (*e && i < (int)sizeof(tmp) - 1) tmp[i++] = *e++;
            tmp[i] = 0;
            for (int j = 0; j <= i && j < (int)sizeof(st_status) - 1; j++)
                st_status[j] = tmp[j];
            if (n == 0 && rc > 0) st_sel = 0;
        }
        return;
    }
    if (c == 'i' || c == 'I') {          // install every catalog app
        pkg_set_progress_fn(st_progress_cb);
        int cnt = pkg_install_all();
        pkg_set_progress_fn(0);
        if (cnt > 0) applib_refresh();
        int i = 0;
        const char* e = pkg_last_error();
        while (*e && i < (int)sizeof(st_status) - 1) st_status[i++] = *e++;
        st_status[i] = 0;
        return;
    }
    if (c == 'B' || c == 'D') { st_sel++; if (st_sel >= n) st_sel = n - 1; return; }
    if (c == 'A' || c == 'C') { st_sel--; if (st_sel < 0) st_sel = 0; return; }
    if (c == 'u' || c == 'U') {
        int rc = pkg_update();
        if (rc > 0 && st_sel >= pkg_catalog_count())
            st_sel = pkg_catalog_count() - 1;
        const char* e = rc < 0 ? pkg_last_error()
                        : (rc == 0 ? "repo returned no apps" : "catalog refreshed");
        int i = 0;
        while (e[i] && i < (int)sizeof(st_status) - 1) { st_status[i] = e[i]; i++; }
        st_status[i] = 0;
        return;
    }
    // ENTER with an empty catalog (failed first fetch): retry the
    // catalog fetch before giving up, so the shelf heals itself
    // instead of silently ignoring the key.
    if (c == 13 && pkg_catalog_count() == 0) {
        int n = st_fetch_catalog();
        int i = 0;
        const char* m = n < 0 ? pkg_last_error()
                        : (n == 0 ? "repo returned no apps"
                                  : "catalog loaded — press ENTER to install");
        while (*m && i < (int)sizeof(st_status) - 1) st_status[i++] = *m++;
        st_status[i] = 0;
        return;
    }
    const PkgEntry* e = pkg_catalog_entry(st_sel);
    if (!e) return;
    if (c == 13) {            // ENTER: install when missing, else run
        int i = 0;
        if (pkg_is_installed(e->name)) {
            int pid = pkg_run(e->name);
            const char* m;
            char tmp[80];
            if (pid >= 0) {
                st_auto_launch = 1;
                const char* pre = "Launched pid=";
                while (*pre) tmp[i++] = *pre++;
                st_num((unsigned)pid, tmp + i);
                while (tmp[i]) i++;
                tmp[i] = 0;
                m = tmp;
            } else {
                m = pkg_last_error();
            }
            while (*m && i < (int)sizeof(st_status)) st_status[i++] = *m++;
            st_status[i] = 0;
        } else {
            // One ENTER = install AND run, so the app is immediately
            // functional (its output appears in the Terminal window
            // that desktop.c opens after store_take_auto_launch()).
            int rc = pkg_install(e->name);
            if (rc == 0) {
                applib_refresh();  /* launcher list stays current */
                int pid = pkg_run(e->name);
                char tmp[80];
                const char* m;
                if (pid >= 0) {
                    st_auto_launch = 1;
                    const char* pre = "installed & launched pid=";
                    while (*pre) tmp[i++] = *pre++;
                    st_num((unsigned)pid, tmp + i);
                    while (tmp[i]) i++;
                    tmp[i] = 0;
                    m = tmp;
                } else {
                    m = pkg_last_error();
                }
                while (*m && i < (int)sizeof(st_status)) st_status[i++] = *m++;
                st_status[i] = 0;
            } else {
                const char* m = pkg_last_error();
                while (*m && i < (int)sizeof(st_status) - 1) st_status[i++] = *m++;
                st_status[i] = 0;
            }
        }
        return;
    }
    if (c == 'd' || c == 'D') {   // uninstall
        int rc = pkg_uninstall(e->name);
        if (rc == 0)
            applib_refresh();  /* launcher list stays current */
        const char* m = rc ? pkg_last_error() : "uninstalled";
        int i = 0;
        while (*m && i < (int)sizeof(st_status) - 1) st_status[i++] = *m++;
        st_status[i] = 0;
        return;
    }
    if (c == 'r' || c == 'R' || c == 'o' || c == 'O') {  // run explicitly
        int pid = pkg_run(e->name);
        int i = 0;
        if (pid >= 0) {
            const char* pre = "Launched pid=";
            while (*pre && i < (int)sizeof(st_status) - 1) st_status[i++] = *pre++;
            char sb[16]; st_num((unsigned)pid, sb);
            for (int j = 0; sb[j] && i < (int)sizeof(st_status) - 1; i++, j++)
                st_status[i] = sb[j];
            st_status[i] = 0;
        } else {
            const char* m = pkg_last_error();
            while (*m && i < (int)sizeof(st_status) - 1) st_status[i++] = *m++;
            st_status[i] = 0;
        }
        return;
    }
}
