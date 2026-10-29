// cpu/fs.c — Nyx OS virtual filesystem + disk persistence
//
// The RAM-resident filesystem[] array (defined below) is what every
// other subsystem (shell, editor, paint, file manager, trash) reads
// and writes directly — that part is unchanged from before. What's
// new is fs_save_to_disk()/fs_load_from_disk(), which mirror that
// RAM array to/from a real ATA disk so files survive a reboot.
//
// On-disk layout (v7 — NYXFS002, AUTO-GROW):
//   sector  0          superblock: magic[8] + file_count[4] + frontier[4]
//   sectors 1..11      directory:  MAX_FILES entries x 44 bytes
//                      (name[32] + used[4] + size[4] + start_lba[4])
//   sector 12          desktop icon-layout sector (FIXED, owned by
//                      desktop.c — fs.c never touches it)
//   sectors 13..       file data:  packed at a growing frontier
//
// Every save REWRITES all used files compactly starting at
// FS_DATA_START, so the written region is exactly as large as the
// live data — no pre-sized 133 MB image, no fixed per-file regions.
// QEMU's raw driver extends the image automatically when a write
// lands past the current end of the file, so capacity scales by
// simply writing further into the image (auto-grow).  Deleted files'
// sectors are simply not rewritten on the next save, so their space
// is reclaimed for free, and no separate allocation table has to be
// kept in sync — each directory entry carries its file's absolute
// start sector.
//
// Directory I/O is deliberately sector-by-sector with small 512-byte
// buffers: the kernel stack is only 16 KB and fs_save_to_disk() runs
// inside the PIT ISR (periodic autosave), so a multi-KB dirbuf would
// risk a stack overflow there.  No static buffers either — the ISR
// autosave can preempt a main-context save, so state must stay on
// the stack.

#include <fs.h>
#include <ata.h>
#include <heap.h>
#include <stdint.h>

File filesystem[MAX_FILES];
int  file_count = 0;

// ─── Dynamic content allocation ───────────────────────────────────
// Each file's content buffer lives on the heap, allocated on demand
// and freed when the file slot is released.  This replaces the old
// fixed char content[MAX_FILESIZE] array that consumed 8 MiB per slot.
void file_content_alloc(int i, int min_cap) {
    if (i < 0 || i >= MAX_FILES) return;
    if (filesystem[i].content && filesystem[i].content_cap >= min_cap) return;
    int cap = min_cap;
    if (cap < 4096) cap = 4096;  // minimum one page
    // Round up to next power-of-two for efficient reuse
    int p = 4096; while (p < cap) p *= 2;
    cap = p;
    if (cap > MAX_FILESIZE) cap = MAX_FILESIZE;
    if (filesystem[i].content) {
        filesystem[i].content = (char*)krealloc(filesystem[i].content, cap);
    } else {
        filesystem[i].content = (char*)kmalloc(cap);
    }
    filesystem[i].content_cap = cap;
}
void file_content_free(int i) {
    if (i < 0 || i >= MAX_FILES) return;
    // content_cap == 0 means static .rodata (embedded file) — don't kfree
    if (filesystem[i].content && filesystem[i].content_cap > 0) {
        kfree(filesystem[i].content);
        filesystem[i].content = 0;
        filesystem[i].content_cap = 0;
    }
}

#define FS_MAGIC        "NYXFS002"   // 8 bytes, no null terminator needed
// FS_SB_SECTOR / FS_DIR_SECTOR / FS_DIR_ENTRY_SZ / FS_DIR_SECTORS /
// FS_ICON_LAYOUT_SECTOR / FS_DATA_START now live in include/fs.h
// (single source of truth) so desktop.c derives the icon-layout
// sector from the very same constants and can never collide with
// file data again.

static void put_u32(uint8_t* buf, uint32_t v) {
    buf[0]=(uint8_t)(v & 0xFF); buf[1]=(uint8_t)((v>>8)&0xFF);
    buf[2]=(uint8_t)((v>>16)&0xFF); buf[3]=(uint8_t)((v>>24)&0xFF);
}
static uint32_t get_u32(const uint8_t* buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1]<<8) |
           ((uint32_t)buf[2]<<16) | ((uint32_t)buf[3]<<24);
}
static int fs_strlen(const char* s, int max) {
    int i=0; while(i<max && s[i]) i++; return i;
}

// Effective content length of a used file: honour the explicit size
// field (set by embed/editor/shell for exact bytes — binary files
// contain NULs that strlen would truncate at), falling back to
// strlen for legacy text files whose size is 0.
static int fs_clen(int i) {
    if (!filesystem[i].used) return 0;
    if (filesystem[i].size > 0) {
        int n = filesystem[i].size;
        int cap = filesystem[i].content_cap;
        return (cap > 0 && n > cap-1) ? cap-1 : n;
    }
    if (!filesystem[i].content) return 0;
    return fs_strlen(filesystem[i].content, filesystem[i].content_cap ? filesystem[i].content_cap : MAX_FILESIZE);
}

// ─── Save ─────────────────────────────────────────────────────────
// Returns 1 on success, 0 if no disk is present or a write failed.
// Safe to call unconditionally (e.g. from a periodic autosave tick)
// — it's a no-op whenever ata_disk_present is false.
//
// Files are packed at a growing frontier starting at FS_DATA_START.
// CRASH NOTE (honest tradeoff of compact-rewrite): every save
// rewrites ALL used files in place from sector 13, so the previous
// save's data is overwritten BEFORE the directory is updated.  A
// crash between the data pass and the directory pass leaves a new
// data layout under an old directory — files whose sector count (or
// whose neighbor's) changed may read back corrupted.  This is the
// price of an image that is exactly as big as the live data and needs
// no migration tools; it is acceptable here because the disk is
// currently disabled at boot and the ~11s autosave is best-effort.
// The superblock is written LAST only so a fresh-disk first save
// atomically produces a recognizable volume; the frontier field is
// informational — load recomputes placement from the directory
// entries, so a stale frontier can never corrupt a subsequent load.
int fs_save_to_disk(void) {
    if (!ata_disk_present) return 0;

    uint32_t frontier = FS_DATA_START;

    // ── Pass 1: file data, compactly from the frontier ───────────
    // Only the sectors that actually hold content are written, so the
    // image stays exactly as big as the live data and QEMU extends it
    // on demand when new files land past the current end.
    for (int i=0;i<MAX_FILES;i++) {
        if (!filesystem[i].used || !filesystem[i].content) continue;
        int clen = fs_clen(i);
        int nsec = (clen + 511) / 512;
        uint32_t start = frontier;
        frontier += (uint32_t)nsec;
        int written = 0;
        for (int s=0;s<nsec;s++) {
            uint8_t data[512];
            for (int j=0;j<512;j++) data[j]=0;
            for (int j=0;j<512 && written < clen;j++)
                data[j] = (uint8_t)filesystem[i].content[written++];
            if (!ata_write_sector(start + s, data)) return 0;
        }
    }

    // ── Pass 2: directory — one sector at a time ─────────────────
    // Entries pack linearly at byte i*FS_DIR_ENTRY_SZ and may straddle
    // a sector boundary; each sector copies only its overlapping
    // bytes.  The running frontier is recomputed for every file so the
    // start_lba written here exactly matches pass 1's placement,
    // without needing to store an array in between.
    for (int s=0;s<FS_DIR_SECTORS;s++) {
        uint8_t dirsec[512];
        for (int j=0;j<512;j++) dirsec[j]=0;
        int s_lo = s*512, s_hi = s_lo+512;
        uint32_t run = FS_DATA_START;
        for (int i=0;i<MAX_FILES;i++) {
            int e_lo = i*FS_DIR_ENTRY_SZ, e_hi = e_lo + FS_DIR_ENTRY_SZ;
            uint8_t ent[FS_DIR_ENTRY_SZ];
            for (int j=0;j<FS_DIR_ENTRY_SZ;j++) ent[j]=0;
            if (filesystem[i].used) {
                int nlen = fs_strlen(filesystem[i].name, MAX_FILENAME);
                for (int j=0;j<nlen;j++) ent[j]=(uint8_t)filesystem[i].name[j];
                int clen = fs_clen(i);
                int nsec = (clen + 511) / 512;
                put_u32(ent+32, 1);
                put_u32(ent+36, (uint32_t)clen);
                put_u32(ent+40, run);
                run += (uint32_t)nsec;
            }
            if (e_hi <= s_lo || e_lo >= s_hi) continue;   // entry lives elsewhere
            int start = (e_lo > s_lo) ? e_lo : s_lo;      // overlap range
            int end   = (e_hi < s_hi) ? e_hi : s_hi;
            for (int b=start;b<end;b++) dirsec[b-s_lo] = ent[b-e_lo];
        }
        if (!ata_write_sector(FS_DIR_SECTOR+s, dirsec)) return 0;
    }

    // ── Pass 3: superblock (LAST = crash-safe commit point) ─────
    {
        uint8_t sb[512];
        for (int i=0;i<512;i++) sb[i]=0;
        for (int i=0;i<8;i++) sb[i]=(uint8_t)FS_MAGIC[i];
        put_u32(sb+8, (uint32_t)file_count);
        put_u32(sb+12, frontier);
        if (!ata_write_sector(FS_SB_SECTOR, sb)) return 0;
    }
    return 1;
}

// ─── Load ─────────────────────────────────────────────────────────
// Returns 1 if a valid Nyx filesystem was found and loaded, 0 if
// there's no disk, or the disk doesn't have our magic yet (a blank
// / freshly-created disk image, or one from a different OS) — in
// either case we leave filesystem[] exactly as it already was
// (freshly zeroed by static initialization), which is the correct
// "first boot" behavior.
int fs_load_from_disk(void) {
    if (!ata_disk_present) return 0;

    uint8_t sb[512];
    if (!ata_read_sector(FS_SB_SECTOR, sb)) return 0;
    for (int i=0;i<8;i++) if (sb[i] != (uint8_t)FS_MAGIC[i]) return 0;

    int loaded_count = (int)get_u32(sb+8);
    if (loaded_count < 0 || loaded_count > MAX_FILES) loaded_count = 0;

    // Directory — entry-major read through a small 512-byte cached
    // sector buffer (kernel stack is only 16 KB, and no static buffers
    // because this can be called from main context while the PIT ISR
    // autosaves).  Entries pack linearly at byte i*FS_DIR_ENTRY_SZ and
    // may straddle a 512-byte boundary, so each entry is assembled from
    // its 1-2 containing sectors via the cache — never re-zeroed per
    // sector (that would blank the name of a straddling entry).  The
    // cache keeps total sector reads at ~FS_DIR_SECTORS regardless of
    // entry count.
    int actual_count = 0;
    int cached_sec = -1;
    uint8_t dirsec[512];
    for (int i=0;i<MAX_FILES;i++) {
        int e_lo = i*FS_DIR_ENTRY_SZ, e_hi = e_lo + FS_DIR_ENTRY_SZ;
        uint8_t ent[FS_DIR_ENTRY_SZ];
        for (int j=0;j<FS_DIR_ENTRY_SZ;j++) ent[j]=0;
        for (int b=e_lo;b<e_hi;b++) {
            int s = b / 512;
            if (s != cached_sec) {
                if (s < 0 || s >= FS_DIR_SECTORS) return 0;
                if (!ata_read_sector(FS_DIR_SECTOR+s, dirsec)) return 0;
                cached_sec = s;
            }
            ent[b - e_lo] = dirsec[b % 512];
        }

        int used = (int)get_u32(ent+32);
        int clen = (int)get_u32(ent+36);
        uint32_t start = get_u32(ent+40);
        if (clen < 0 || clen > MAX_FILESIZE-1) clen = 0;   // corrupt guard

        filesystem[i].used = used ? 1 : 0;
        for (int j=0;j<32;j++) filesystem[i].name[j] = (char)ent[j];
        filesystem[i].name[31] = '\0'; // guarantee termination regardless
        filesystem[i].size = used ? clen : 0;

        if (used && clen > 0) {
            // Absolute start_lba from the entry; anything that points
            // into the superblock/directory/icon region is corrupt.
            if (start < FS_DATA_START) { used = 0; filesystem[i].used = 0; }
            else {
                file_content_alloc(i, clen + 1);
                if (!filesystem[i].content) { used = 0; filesystem[i].used = 0; }
                else {
                    int copied = 0;
                    int nsec = (clen + 511) / 512;
                    for (int s2=0;s2<nsec;s2++) {
                        uint8_t data[512];
                        if (!ata_read_sector(start + s2, data)) { used = 0; break; }
                        for (int j=0;j<512 && copied < clen && copied < filesystem[i].content_cap-1;j++)
                            filesystem[i].content[copied++] = (char)data[j];
                    }
                    if (used) {
                        filesystem[i].content[copied] = '\0';
                        actual_count++;
                    } else {
                        file_content_free(i);
                        filesystem[i].used = 0;
                        filesystem[i].size = 0;
                    }
                }
            }
        } else if (used) {
            file_content_alloc(i, 1);
            if (filesystem[i].content) filesystem[i].content[0] = '\0';
            actual_count++;
        }
    }
    // Trust what we actually found rather than the stored counter,
    // in case the two ever disagree (e.g. an interrupted save).
    file_count = actual_count;
    (void)loaded_count;
    return 1;
}
