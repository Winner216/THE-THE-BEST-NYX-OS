// tools/fs_host_test.c — host-side round-trip test for the v7
// auto-grow disk format (NYXFS002).  Compiles the REAL cpu/fs.c
// against a stub ATA driver backed by a growable in-memory image that
// simulates QEMU's raw driver auto-extending the file on writes past
// the current end.
//
// Build & run:
//     gcc -O2 -I include tools/fs_host_test.c cpu/fs.c -o /tmp/fs_test
//     /tmp/fs_test
//
// Exit code 0 = all checks passed.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <fs.h>

// ── Stub ATA: growable in-memory "raw image" ─────────────────────
int ata_disk_present = 1;
static uint8_t* g_img = NULL;
static uint32_t g_nsectors = 0;
static uint32_t g_writes_high = 0;   // highest sector ever written

// Load a real image file into the stub buffer (for pack/migrate checks).
static int load_image_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }
    free(g_img);
    g_img = malloc((size_t)sz);
    if (!g_img) { fclose(f); return 0; }
    if (fread(g_img, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return 0; }
    fclose(f);
    g_nsectors = (uint32_t)(sz / 512);
    g_writes_high = 0;
    return 1;
}

int ata_read_sector(uint32_t lba, uint8_t* buf512) {
    if (lba >= g_nsectors) { memset(buf512, 0, 512); return 1; } // past EOF → zeros (QEMU raw)
    memcpy(buf512, g_img + (size_t)lba * 512, 512);
    return 1;
}
int ata_write_sector(uint32_t lba, const uint8_t* buf512) {
    // Simulate QEMU raw auto-grow: writing past the current end
    // extends the image.
    if (lba >= g_nsectors) {
        uint32_t news = lba + 1;
        g_img = realloc(g_img, (size_t)news * 512);
        memset(g_img + (size_t)g_nsectors * 512, 0, (size_t)(news - g_nsectors) * 512);
        g_nsectors = news;
    }
    memcpy(g_img + (size_t)lba * 512, buf512, 512);
    if (lba > g_writes_high) g_writes_high = lba;
    return 1;
}

static int failures = 0;
static void check(int cond, const char* msg) {
    if (cond) { printf("  ✔ %s\n", msg); }
    else      { printf("  ✗ FAIL: %s\n", msg); failures++; }
}

// Reset the RAM filesystem to empty.
static void wipe_fs(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        filesystem[i].used = 0;
        filesystem[i].size = 0;
        filesystem[i].name[0] = 0;
        filesystem[i].content[0] = 0;
    }
    file_count = 0;
}

// Add a file. Returns index.
static int add_file(const char* name, const void* data, int len) {
    int i;
    for (i = 0; i < MAX_FILES; i++) if (!filesystem[i].used) break;
    if (i >= MAX_FILES) return -1;
    strncpy(filesystem[i].name, name, MAX_FILENAME - 1);
    filesystem[i].name[MAX_FILENAME - 1] = 0;
    if (data && len > 0) memcpy(filesystem[i].content, data, len);
    filesystem[i].content[len] = 0;
    filesystem[i].size = len;
    filesystem[i].used = 1;
    file_count++;
    return i;
}

// Reset the in-memory image to a tiny "fresh disk" (like the new
// 4M disk.img — far smaller than the old 133 MB pre-sized image).
static void reset_image(void) {
    g_img = realloc(g_img, 4 * 1024 * 1024);
    memset(g_img, 0, 4 * 1024 * 1024);
    g_nsectors = 4 * 1024 * 1024 / 512;   // 8192 sectors
    g_writes_high = 0;
}

int main(int argc, char** argv) {
    // ── Dump mode: load an image produced by pack-files.py or
    // migrate-disk.py and list what fs_load_from_disk() sees. ──
    if (argc == 3 && strcmp(argv[1], "dump") == 0) {
        printf("Dumping %s as seen by fs_load_from_disk()...\n", argv[2]);
        if (!load_image_file(argv[2])) {
            printf("  ✗ cannot read %s\n", argv[2]);
            return 1;
        }
        wipe_fs();
        int ok = fs_load_from_disk();
        if (!ok) { printf("  ✗ fs_load_from_disk() rejected the volume\n"); return 1; }
        printf("  ✔ volume accepted, file_count = %d\n", file_count);
        int n = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (filesystem[i].used) {
                printf("     %-32s %d bytes\n", filesystem[i].name, filesystem[i].size);
                n++;
            }
        }
        printf("  total used: %d\n", n);
        return n == file_count ? 0 : 1;
    }

    printf("fs_host_test — auto-grow disk format (NYXFS002)\n");
    printf("(compiling the REAL cpu/fs.c against a stub ATA driver)\n\n");

    // ── Test 1: save a few files, image must stay TINY ─────────
    reset_image();
    wipe_fs();
    {
        const char* text = "hello nyx\nsecond line\n";
        uint8_t bin[3000];
        for (int i = 0; i < 3000; i++) bin[i] = (uint8_t)(i & 0xFF);  // binary, NULs inside
        add_file("readme.txt", text, (int)strlen(text));
        add_file("pic.bin", bin, 3000);
        add_file("empty.dat", "", 0);

        printf("[1] Save 3 files (text, 3000-byte binary, empty)…\n");
        int ok = fs_save_to_disk();
        check(ok == 1, "fs_save_to_disk() returned success");
        // 3 files: readme ~20B (1 sector), pic.bin 3000B (6 sectors), empty (0)
        // frontier = 13 + 1 + 6 + 0 = 20 sectors = 10 KB.  The image must NOT
        // be pre-sized to 133 MB — it should be exactly what was written.
        uint32_t expect = FS_DATA_START + 1 + 6;
        check(g_writes_high + 1 <= expect + 2,
              "image grew only to the actual data (frontier ~20 sectors), not 260123");
        printf("       highest written sector: %u (expected ≈ %u)\n",
               g_writes_high, expect - 1);
    }

    // ── Test 2: wipe RAM, reload, verify round-trip ────────────
    printf("\n[2] Wipe RAM, reload from disk, verify contents…\n");
    {
        // snapshot what we expect
        char names[3][32];
        int sizes[3];
        names[0][0] = 0; names[1][0] = 0; names[2][0] = 0;
        for (int i = 0; i < MAX_FILES && file_count > 0; i++) {
            if (filesystem[i].used) {
                int slot = 0;
                for (int k = 0; k < 3; k++) if (names[k][0] == 0) { slot = k; break; }
                strncpy(names[slot], filesystem[i].name, 31);
                sizes[slot] = filesystem[i].size;
            }
        }
        int ok = fs_load_from_disk();
        check(ok == 1, "fs_load_from_disk() found the NYXFS002 volume");
        check(file_count == 3, "file_count == 3 after reload");

        // verify each file
        int found[3] = {0,0,0};
        for (int i = 0; i < MAX_FILES; i++) {
            if (!filesystem[i].used) continue;
            if (strcmp(filesystem[i].name, "readme.txt") == 0) {
                found[0] = 1;
                check(strcmp(filesystem[i].content, "hello nyx\nsecond line\n") == 0,
                      "readme.txt content round-tripped");
            } else if (strcmp(filesystem[i].name, "pic.bin") == 0) {
                found[1] = 1;
                check(filesystem[i].size == 3000, "pic.bin size == 3000");
                int okb = 1;
                for (int k = 0; k < 3000; k++)
                    if ((uint8_t)filesystem[i].content[k] != (uint8_t)(k & 0xFF)) okb = 0;
                check(okb, "pic.bin binary content (with NULs) round-tripped");
            } else if (strcmp(filesystem[i].name, "empty.dat") == 0) {
                found[2] = 1;
                check(filesystem[i].size == 0, "empty.dat size == 0");
            }
        }
        check(found[0] && found[1] && found[2], "all 3 files present after reload");
    }

    // ── Test 3: auto-grow — add a big file, image must extend ──
    printf("\n[3] Add a 200 KB file and save — image auto-grows…\n");
    {
        uint8_t big[200 * 1024];
        for (int i = 0; i < (int)sizeof(big); i++) big[i] = (uint8_t)(i * 7);
        int idx = add_file("big.bin", big, sizeof(big));
        check(idx >= 0, "big.bin slot found");
        uint32_t before = g_writes_high;
        int ok = fs_save_to_disk();
        check(ok == 1, "save with big file returned success");
        // 200 KB = 400 sectors; 3 small files used 7 (frontier was 20);
        // new frontier = 20 + 400 = 420 sectors (highest written 419).
        check(g_writes_high + 1 == FS_DATA_START + 407,
              "image extended to hold the new file (frontier = 420 sectors)");
        printf("       highest written sector: %u (was %u, +400)\n",
               g_writes_high, before);

        // reload & verify big.bin
        wipe_fs();
        fs_load_from_disk();
        int found = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (filesystem[i].used && strcmp(filesystem[i].name, "big.bin") == 0) {
                found = 1;
                check(filesystem[i].size == 200 * 1024, "big.bin size round-tripped");
                int okb = 1;
                for (int k = 0; k < 200 * 1024; k++)
                    if ((uint8_t)filesystem[i].content[k] != (uint8_t)(k * 7)) okb = 0;
                check(okb, "big.bin content round-tripped");
            }
        }
        check(found, "big.bin found after reload");
    }

    // ── Test 4: deleted file's space is reclaimed (compact again) ─
    // QEMU raw never shrinks the file, so the high-water mark stays
    // at 419 — but the SUPERBLOCK FRONTIER (where the next file
    // lands) must snap back to ~20.  Proof: add a 100 KB file after
    // the delete; it must be placed at sector ~20, NOT at 419.
    printf("\n[4] Delete big.bin, add a 100 KB file — space is reclaimed…\n");
    {
        for (int i = 0; i < MAX_FILES; i++)
            if (filesystem[i].used && strcmp(filesystem[i].name, "big.bin") == 0)
                { filesystem[i].used = 0; file_count--; }
        fs_save_to_disk();

        // read the frontier back from the superblock (sector 0, +12)
        uint32_t frontier = (uint32_t)g_img[12] | ((uint32_t)g_img[13] << 8) |
                            ((uint32_t)g_img[14] << 16) | ((uint32_t)g_img[15] << 24);
        check(frontier == FS_DATA_START + 7,
              "superblock frontier snapped back to 20 sectors after delete");
        printf("       superblock frontier after delete: %u\n", frontier);

        uint8_t mid[100 * 1024];
        for (int i = 0; i < (int)sizeof(mid); i++) mid[i] = (uint8_t)(i + 1);
        add_file("mid.bin", mid, sizeof(mid));
        fs_save_to_disk();
        // The frontier (not the stale high-water mark from test 3)
        // is what matters: it must be 20 + 200 = 220, proving the new
        // file reused the reclaimed space instead of landing at 419.
        uint32_t frontier2 = (uint32_t)g_img[12] | ((uint32_t)g_img[13] << 8) |
                             ((uint32_t)g_img[14] << 16) | ((uint32_t)g_img[15] << 24);
        check(frontier2 == FS_DATA_START + 207,
              "new 100 KB file reused the reclaimed space (frontier = 220, not 619)");
        printf("       superblock frontier after reclaim: %u\n", frontier2);

        // reload once more to confirm everything still round-trips
        wipe_fs();
        fs_load_from_disk();
        int mid_ok = 0, tot = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (filesystem[i].used) {
                tot++;
                if (strcmp(filesystem[i].name, "mid.bin") == 0) {
                    mid_ok = 1;
                    int okb = 1;
                    for (int k = 0; k < 100 * 1024; k++)
                        if ((uint8_t)filesystem[i].content[k] != (uint8_t)(k + 1)) okb = 0;
                    check(okb && filesystem[i].size == 100 * 1024,
                          "mid.bin round-tripped after reclaim");
                }
            }
        }
        check(mid_ok && tot == 4, "all 4 files present after final reload");
    }

    // ── Test 5: icon-layout sector (12) is never clobbered ──────
    printf("\n[5] Icon-layout sector (12) survives save cycles…\n");
    {
        uint8_t mark[512];
        memset(mark, 0xAB, 512);
        memcpy(mark, "NYIX", 4);
        memcpy(g_img + (size_t)FS_ICON_LAYOUT_SECTOR * 512, mark, 512);
        // several save cycles with files present
        fs_save_to_disk();
        fs_save_to_disk();
        check(g_img[(size_t)FS_ICON_LAYOUT_SECTOR * 512] == 'N' &&
              g_img[(size_t)FS_ICON_LAYOUT_SECTOR * 512 + 1] == 'Y' &&
              g_img[(size_t)FS_ICON_LAYOUT_SECTOR * 512 + 2] == 'I' &&
              g_img[(size_t)FS_ICON_LAYOUT_SECTOR * 512 + 3] == 'X',
              "sector 12 still holds the desktop state (never overwritten)");
    }

    printf("\n%s (%d failure%s)\n",
           failures ? "❌ TESTS FAILED" : "✅ ALL TESTS PASSED",
           failures, failures == 1 ? "" : "s");
    free(g_img);
    return failures ? 1 : 0;
}
