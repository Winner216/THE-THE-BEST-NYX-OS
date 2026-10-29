// cpu/ata.c — ATA PIO disk driver
// Primary bus, master drive, 28-bit LBA addressing, polled PIO mode.
//
// SAFETY MODEL: the previous version bounded each individual wait
// loop by iteration count, on the assumption that a bounded loop
// can't hang. That assumption was wrong in practice — it doesn't
// account for how expensive each port read can be. Under QEMU's TCG
// software emulation (no KVM/hardware acceleration), every inb/outw
// traps out to the emulator, and that trap can cost far more than on
// real hardware. fs_load_from_disk() alone can chain up to ~40 wait
// calls (init + up to 19 sector reads x 2 waits each) before the
// user sees anything interactive, so even a "bounded" per-call loop
// can compound into a boot that looks and feels completely frozen.
//
// The fix: a single global work budget shared across the ENTIRE
// boot-time detection+load sequence, not a separate budget per call.
// Every wait loop iteration spends from this one shared pool, and
// once it's gone, every remaining wait in the sequence fails in O(1)
// — no more waiting, no more port I/O, just an immediate "give up".
// This guarantees a hard ceiling on total boot-time disk work no
// matter how many sectors are involved or how the drive behaves.
// If the budget runs out, the disk is treated as unusable for the
// rest of the session (ata_disk_present stays 0) rather than
// repeatedly gambling on I/O that's already shown itself unreliable
// — persistence simply resumes on the next boot instead.

#include <ata.h>
#include <io.h>

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_FLUSH   0xE7
#define ATA_CMD_IDENTIFY 0xEC

#define STATUS_ERR  0x01
#define STATUS_DRQ  0x08
#define STATUS_DF   0x20
#define STATUS_BSY  0x80

int ata_disk_present = 0;

// Shared budget for the boot-time detection+load window. Reset once
// at the start of ata_init(); every wait-loop iteration everywhere
// (init, and every sector read/write during the initial fs load)
// spends one unit. A real, working PIO transfer clears BSY/sets DRQ
// within a handful to a few dozen iterations — this budget is sized
// to fail FAST in the pathological case (drive never responds) since
// no amount of extra budget fixes that; it only prolongs how long
// the machine appears frozen before giving up. 3,000 total units
// across the WHOLE sequence, not per call.
static uint32_t ata_boot_budget = 3000;

static void ata_io_delay(void) {
    inb(ATA_STATUS); inb(ATA_STATUS); inb(ATA_STATUS); inb(ATA_STATUS);
}

// Bounded poll for BSY clearing, spending from the shared budget.
// Returns 1 if BSY cleared, 0 if it never did OR the budget ran out
// (checked first, so an exhausted budget fails instantly with zero
// port I/O rather than trying and immediately failing anyway).
static int ata_wait_not_busy(void) {
    while (ata_boot_budget > 0) {
        ata_boot_budget--;
        if (!(inb(ATA_STATUS) & STATUS_BSY)) return 1;
    }
    return 0;
}
static int ata_wait_drq(void) {
    while (ata_boot_budget > 0) {
        ata_boot_budget--;
        uint8_t st = inb(ATA_STATUS);
        if (st & (STATUS_ERR | STATUS_DF)) return 0;
        if (st & STATUS_DRQ) return 1;
    }
    return 0;
}

static void ata_select_lba(uint32_t lba, uint8_t sector_count) {
    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F))); // LBA mode, master
    ata_io_delay();
    outb(ATA_SECCOUNT, sector_count);
    outb(ATA_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
}

int ata_init(void) {
    ata_disk_present = 0;
    ata_boot_budget = 3000;    // fresh budget for this boot attempt

    // A floating/absent bus reads back 0xFF on every port. Check
    // that first — cheapest possible way to bail out immediately on
    // a VM with no -hda attached, before spending any budget at all.
    if (inb(ATA_STATUS) == 0xFF) return 0;

    outb(ATA_DRIVE_HEAD, 0xA0); // select master, no LBA bits set yet
    ata_io_delay();
    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LO,  0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI,  0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    if (inb(ATA_STATUS) == 0) return 0;      // no drive on this bus at all
    if (!ata_wait_not_busy()) return 0;      // wedged, or budget exhausted

    // A non-ATA (e.g. ATAPI/CD) device reports a different signature
    // in LBA_MID/LBA_HI after IDENTIFY; we only care about plain ATA
    // hard disks here, so require both to stay 0.
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) return 0;

    if (!ata_wait_drq()) return 0;

    // Drain the 256-word IDENTIFY response so the drive isn't left
    // mid-transfer for the next command. This loop is NOT budget
    // gated — once DRQ is confirmed set, the data is already latched
    // and ready; reading it off is fast, fixed-length (always
    // exactly 256 words), and not something that can hang.
    for (int i = 0; i < 256; i++) inw(ATA_DATA);

    ata_disk_present = 1;
    return 1;
}

int ata_read_sector(uint32_t lba, uint8_t* buf512) {
    if (!ata_disk_present) return 0;
    if (!ata_wait_not_busy()) { ata_disk_present = 0; return 0; }

    ata_select_lba(lba, 1);
    outb(ATA_COMMAND, ATA_CMD_READ);

    if (!ata_wait_not_busy()) { ata_disk_present = 0; return 0; }
    if (!ata_wait_drq())      { ata_disk_present = 0; return 0; }

    uint16_t* p = (uint16_t*)buf512;
    for (int i = 0; i < 256; i++) p[i] = inw(ATA_DATA);
    return 1;
}

int ata_write_sector(uint32_t lba, const uint8_t* buf512) {
    if (!ata_disk_present) return 0;
    if (!ata_wait_not_busy()) { ata_disk_present = 0; return 0; }

    ata_select_lba(lba, 1);
    outb(ATA_COMMAND, ATA_CMD_WRITE);

    if (!ata_wait_not_busy()) { ata_disk_present = 0; return 0; }
    if (!ata_wait_drq())      { ata_disk_present = 0; return 0; }

    const uint16_t* p = (const uint16_t*)buf512;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, p[i]);

    if (!ata_wait_not_busy()) { ata_disk_present = 0; return 0; }
    outb(ATA_COMMAND, ATA_CMD_FLUSH);
    ata_wait_not_busy();
    return 1;
}
