// include/pkg.h — Nyx Package Manager core.
//
// Downloads and installs apps ("packages") from the Nyx App Store
// repository over plain HTTP (the same in-kernel netstack the browser
// uses).  A package is a prebuilt ELF32 ring-3 program (same ABI as
// the embedded `user` programs): installing stores the ELF in a
// filesystem slot named "@<name>", and running it hands the bytes to
// user_exec() which maps them at USER_LINK_BASE and starts a user
// task.  Packages are genuinely installable at runtime — nothing is
// compiled into the kernel.
#ifndef PKG_H
#define PKG_H

#include <stdint.h>

// Repository location.  10.0.2.2 is the QEMU slirp gateway = the
// HOST, where tools/pkg_store.py listens (see `make pkgstore`).
#define PKG_STORE_PORT 8123
#define PKG_STORE_IP   0x0202000Au      // 10.0.2.2, network byte order

#define PKG_MAX_CATALOG 24              // max apps in one manifest
#define PKG_NAME_MAX    24

// A catalog entry (one line of the repo manifest).
typedef struct {
    char     name[PKG_NAME_MAX];
    char     version[16];
    char     author[24];
    uint32_t size;                      // ELF byte count (from manifest)
    char     desc[96];
} PkgEntry;

// ─── Catalog ─────────────────────────────────────────────────────
// Fetch the manifest from the repo and parse it into the catalog.
// Returns the number of entries (>0 ok), 0 if the repo returned an
// empty manifest, -1 on network/protocol failure.
// Call this before pkg_catalog() / pkg_install().
int  pkg_update(void);
int  pkg_catalog_count(void);
const PkgEntry* pkg_catalog_entry(int i);

// ─── Install state (lives in the filesystem) ─────────────────────
// A package named "foo" is stored in filesystem slot named "@foo".
int  pkg_find_slot(const char* name);  // -1 = not installed
int  pkg_is_installed(const char* name);

// ─── Operations ──────────────────────────────────────────────────
// Download <name>.elf from the repo and store it in the filesystem.
// Returns 0 ok, -1 failure (pkg_last_error() explains).
int  pkg_install(const char* name);
int  pkg_uninstall(const char* name);

// Install every catalog app that isn't already installed.  Ensures
// the catalog is loaded first (retrying a cold network).  Returns
// the number of apps installed (0 = none needed/available), or -1
// if the catalog could not be fetched; pkg_last_error() carries a
// human summary either way.
int  pkg_install_all(void);

// Optional per-app progress hook for bulk installs.  Called from
// pkg_install_all() right before each app's download with the app
// name and a 1-based index/total.  The hook runs in the caller's
// context, so it may repaint UI.  Pass NULL (the default) to
// disable.
typedef void (*pkg_progress_fn)(const char* name, int index, int total);
void pkg_set_progress_fn(pkg_progress_fn fn);

// Run an installed package as a ring-3 task (kills any previous
// user task, exactly like the shell `user` command).  Returns pid >= 0
// on success, -1 on failure.
int  pkg_run(const char* name);

// Human-readable status of the last pkg operation ("ok", "not
// installed", "download failed", ...).
const char* pkg_last_error(void);

// Must networking be up before pkg calls?  pkg_ensure_net() probes
// the PCI bus and brings the NIC + stack up if needed (like the
// browser's auto-init).  Returns 1 when networking is usable.
int  pkg_ensure_net(void);

#endif
