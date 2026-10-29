// include/ata.h — ATA PIO disk driver (primary bus, master drive, 28-bit LBA)
#ifndef ATA_H
#define ATA_H
#include <stdint.h>

// Returns 1 if a disk was detected and responded to IDENTIFY, 0 if not
// (e.g. QEMU launched without -hda). Safe to call even if no disk is
// present — every wait loop in this driver is bounded, never infinite,
// so a missing/unresponsive drive can never hang the boot sequence.
int  ata_init(void);

// Read/write exactly one 512-byte sector. Returns 1 on success, 0 on
// failure (timeout, error bit set, or no disk detected by ata_init).
int  ata_read_sector(uint32_t lba, uint8_t* buf512);
int  ata_write_sector(uint32_t lba, const uint8_t* buf512);

extern int ata_disk_present;

#endif
