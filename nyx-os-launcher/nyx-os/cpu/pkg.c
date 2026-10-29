// cpu/pkg.c — Nyx Package Manager core.
//
// Fetches the App Store catalog from the host repo (tools/pkg_store.py,
// reachable at 10.0.2.2:PKG_STORE_PORT through the QEMU slirp gateway)
// and installs / runs ELF32 ring-3 packages stored in the filesystem.
//
// A package named "foo" is kept in the filesystem slot "@foo", so the
// Files app and `ls` show installed packages and deleting the file
// uninstalls it (pkg_find_slot() re-scans the filesystem every time —
// no separate registry to go stale).
#include <pkg.h>
#include <http.h>
#include <fs.h>
#include <elfload.h>
#include <nic.h>
#include <netstack.h>
#include <stdint.h>
#include <stddef.h>

// ─── Self-contained string helpers (kernel has no libc) ──────────
static int p_len(const char* s) { int i = 0; while (s[i]) i++; return i; }
static int p_cmp(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return (unsigned char)a[i] - (unsigned char)b[i];
}
static void p_cpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
// name → filesystem slot name ("foo" → "@foo")
static void p_fs_name(const char* name, char* out) {
    out[0] = '@';
    int i = 0;
    while (name[i] && i < MAX_FILENAME - 2) { out[i + 1] = name[i]; i++; }
    out[i + 1] = '\0';
}

// ─── State ───────────────────────────────────────────────────────
static PkgEntry catalog[PKG_MAX_CATALOG];
static int      catalog_n = 0;
static char     pkg_err[128] = "no operation yet";
static pkg_progress_fn pkg_progress = 0;   // optional bulk-install hook

static void p_set_err(const char* msg) { p_cpy(pkg_err, msg, (int)sizeof(pkg_err)); }

void pkg_set_progress_fn(pkg_progress_fn fn) { pkg_progress = fn; }

const char* pkg_last_error(void) { return pkg_err; }

int pkg_ensure_net(void) {
    // Same auto-init sequence the browser uses: probe PCI for the NIC
    // if it isn't up yet, then (re)init the TCP/IP stack.
    if (!nic_is_up())
        nic_init();
    netstack_init();
    return nic_is_up() ? 1 : 0;
}

// ─── Manifest parsing ────────────────────────────────────────────
static uint32_t p_atou(const char* s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    return v;
}

// Copy a manifest field into dst: stops at the '|' separator (fields
// are NOT NUL-terminated — they point into the response body), at a
// NUL, or at max-1.  Without the '|' stop the whole rest of the line
// ("pkghello|1.0|Nyx Core|...") leaked into every field.
static void p_cpyf(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i] && src[i] != '|') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// Split one manifest line "name|ver|author|size|desc" into an entry.
static void p_parse_line(const char* ln, PkgEntry* e) {
    e->name[0] = e->version[0] = e->author[0] = e->desc[0] = '\0';
    e->size = 0;
    const char* f[5];          // field starts
    int fi = 0;
    f[0] = ln;
    for (int i = 0; ln[i] && fi < 4; i++) {
        if (ln[i] == '|') { fi++; f[fi] = ln + i + 1; }
    }
    int nf = fi + 1;           // fields actually found
    if (nf >= 1) p_cpyf(e->name,    f[0], (int)sizeof(e->name));
    if (nf >= 2) p_cpyf(e->version, f[1], (int)sizeof(e->version));
    if (nf >= 3) p_cpyf(e->author,  f[2], (int)sizeof(e->author));
    if (nf >= 4) e->size = p_atou(f[3]);
    if (nf >= 5) p_cpyf(e->desc,    f[4], (int)sizeof(e->desc));
}

int pkg_update(void) {
    catalog_n = 0;
    if (!pkg_ensure_net()) {
        p_set_err("no network (NIC down)");
        return -1;
    }
    // Manifest is small, but always use the global static response —
    // its body buffer is BSS (1 MiB) and never touches the 16 KB stack.
    HttpResponse* r = http_get_global_response();
    int rc = http_get(PKG_STORE_IP, PKG_STORE_PORT, "10.0.2.2",
                      "/pkg/manifest", r);
    if (rc != 0) { p_set_err("download failed (no route)"); return -1; }
    if (r->status_code != 200) {
        p_set_err("repo error (HTTP ");  // truncated numeric suffix added below
        // append the numeric status for a readable message
        int n = p_len(pkg_err);
        char tmp[8]; int t = 0;
        uint32_t v = (uint32_t)r->status_code;
        if (v == 0) { tmp[t++] = '0'; }
        else { char rev[8]; int q = 0; while (v) { rev[q++] = (char)('0' + v % 10); v /= 10; }
               while (q) tmp[t++] = rev[--q]; }
        tmp[t] = '\0';
        int i = 0;
        while (tmp[i] && n < (int)sizeof(pkg_err) - 2) pkg_err[n++] = tmp[i++];
        pkg_err[n++] = ')'; pkg_err[n] = '\0';
        return -1;
    }

    int pos = 0;
    while (pos < r->body_len && catalog_n < PKG_MAX_CATALOG) {
        // one manifest line = up to next '\n'
        const char* ln = r->body + pos;
        int eol = 0;
        while (pos + eol < r->body_len && r->body[pos + eol] != '\n') eol++;
        int ln_len = eol;
        if (ln_len > 0 && ln[ln_len - 1] == '\r') ln_len--;
        if (ln_len > 0)
            p_parse_line(ln, &catalog[catalog_n++]);
        pos += eol + 1;   // skip the newline too
    }
    p_set_err(catalog_n > 0 ? "catalog loaded" : "repo returned no apps");
    return catalog_n;   // >0 entries ok, 0 = empty manifest
}

int pkg_catalog_count(void) { return catalog_n; }

const PkgEntry* pkg_catalog_entry(int i) {
    if (i < 0 || i >= catalog_n) return 0;
    return &catalog[i];
}

// ─── Filesystem state ────────────────────────────────────────────
int pkg_find_slot(const char* name) {
    char fsn[MAX_FILENAME];
    p_fs_name(name, fsn);
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].used && p_cmp(filesystem[i].name, fsn) == 0)
            return i;
    }
    return -1;
}

int pkg_is_installed(const char* name) { return pkg_find_slot(name) >= 0 ? 1 : 0; }

static int pkg_name_ok(const char* name) {
    if (!name[0] || p_len(name) >= PKG_NAME_MAX) return 0;
    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

int pkg_install(const char* name) {
    if (!pkg_name_ok(name)) { p_set_err("bad package name"); return -1; }
    if (pkg_is_installed(name)) {
        p_set_err("already installed");
        return -1;
    }
    if (!pkg_ensure_net()) { p_set_err("no network (NIC down)"); return -1; }

    // GET /pkg/<name>.elf  (paths only ever contain [a-z0-9._-])
    char path[64];
    int pi = 0;
    const char* pre = "/pkg/";
    while (*pre) path[pi++] = *pre++;
    for (int i = 0; name[i] && pi < (int)sizeof(path) - 1; i++) path[pi++] = name[i];
    const char* ext = ".elf";
    while (*ext && pi < (int)sizeof(path) - 1) path[pi++] = *ext++;
    path[pi] = '\0';

    HttpResponse* r = http_get_global_response();
    int rc = http_get(PKG_STORE_IP, PKG_STORE_PORT, "10.0.2.2", path, r);
    if (rc != 0) { p_set_err("download failed (no route)"); return -1; }
    if (r->status_code != 200) { p_set_err("repo: file not found"); return -1; }
    if (r->body_len < 64) { p_set_err("repo: empty file"); return -1; }

    // Sanity: must look like an ELF32 before we trust it.
    const unsigned char* b = (const unsigned char*)r->body;
    if (!(b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')) {
        p_set_err("download is not an ELF");
        return -1;
    }

    // Find a free filesystem slot.
    int slot = -1;
    for (int i = 0; i < MAX_FILES; i++)
        if (!filesystem[i].used) { slot = i; break; }
    if (slot < 0) { p_set_err("filesystem full"); return -1; }

    // Install: "@name" slot holding the raw ELF bytes.
    char fsn[MAX_FILENAME];
    p_fs_name(name, fsn);
    int ni = 0;
    while (fsn[ni] && ni < MAX_FILENAME - 1) { filesystem[slot].name[ni] = fsn[ni]; ni++; }
    filesystem[slot].name[ni] = '\0';
    filesystem[slot].used = 1;
    file_content_alloc(slot, r->body_len + 1);
    if (!filesystem[slot].content) {
        filesystem[slot].used = 0;
        p_set_err("out of memory");
        return -1;
    }
    for (int i = 0; i < r->body_len; i++)
        filesystem[slot].content[i] = r->body[i];
    filesystem[slot].content[r->body_len] = '\0';
    filesystem[slot].size = r->body_len;
    file_count++;
    fs_save_to_disk();

    p_set_err("installed");
    return 0;
}

// ─── Bulk install ────────────────────────────────────────────────
// Small number-append helpers for the summary message.
static void p_apnum(char* buf, int* p, int max, uint32_t v) {
    char rev[12]; int q = 0;
    if (!v) { if (*p < max - 1) buf[(*p)++] = '0'; return; }
    while (v) { rev[q++] = (char)('0' + v % 10); v /= 10; }
    while (q && *p < max - 1) buf[(*p)++] = rev[--q];
}
static void p_apstr(char* buf, int* p, int max, const char* s) {
    while (*s && *p < max - 1) buf[(*p)++] = *s++;
}

int pkg_install_all(void) {
    int n = pkg_catalog_count();
    if (n == 0) {
        // Cold network (store opened right after login): retry a few
        // times so a single failed first fetch doesn't abort the bulk
        // install.
        n = pkg_update();
        for (int try = 0; n < 0 && try < 3; try++) {
            for (volatile int d = 0; d < 4000000; d++) {}
            n = pkg_update();
        }
        if (n < 0) { p_set_err("catalog fetch failed"); return -1; }
        if (n == 0) { p_set_err("repo returned no apps"); return -1; }
    }
    int done = 0, failed = 0, idx = 0;
    for (int i = 0; i < n; i++) {
        const PkgEntry* e = pkg_catalog_entry(i);
        if (!e || pkg_is_installed(e->name)) continue;
        idx++;
        if (pkg_progress) pkg_progress(e->name, idx, n);
        if (pkg_install(e->name) == 0) done++;
        else failed++;
    }
    // Summary (overwrites the last single-app pkg_err).
    char tmp[64]; int p = 0;
    if (done == 0 && failed == 0) {
        p_set_err("all apps already installed");
    } else {
        p_apstr(tmp, &p, (int)sizeof(tmp), "installed ");
        p_apnum(tmp, &p, (int)sizeof(tmp), (uint32_t)done);
        p_apstr(tmp, &p, (int)sizeof(tmp), " of ");
        p_apnum(tmp, &p, (int)sizeof(tmp), (uint32_t)n);
        if (failed > 0) {
            p_apstr(tmp, &p, (int)sizeof(tmp), " (");
            p_apnum(tmp, &p, (int)sizeof(tmp), (uint32_t)failed);
            p_apstr(tmp, &p, (int)sizeof(tmp), " failed)");
        }
        tmp[p] = '\0';
        p_set_err(tmp);
    }
    return done;
}

int pkg_uninstall(const char* name) {
    int slot = pkg_find_slot(name);
    if (slot < 0) { p_set_err("not installed"); return -1; }
    filesystem[slot].used = 0;
    file_content_free(slot);
    file_count--;
    fs_save_to_disk();
    p_set_err("uninstalled");
    return 0;
}

int pkg_run(const char* name) {
    int slot = pkg_find_slot(name);
    if (slot < 0) { p_set_err("not installed — run `pkg install <name>` first"); return -1; }
    int len = filesystem[slot].size > 0 ? filesystem[slot].size
                                        : p_len(filesystem[slot].content);
    if (len < 64 || !filesystem[slot].content) { p_set_err("package file empty"); return -1; }
    // args = the bare name (user programs read USER_ARGS_BASE if they want)
    int pid = user_exec((const unsigned char*)filesystem[slot].content,
                        (uint32_t)len, name, name);
    if (pid < 0) { p_set_err("launch failed (bad ELF)"); return -1; }
    p_set_err("launched");
    return pid;
}
