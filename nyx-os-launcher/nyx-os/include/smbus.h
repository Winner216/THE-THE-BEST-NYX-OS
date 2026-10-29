// include/smbus.h
// SMBus (System Management Bus) host controller driver for QEMU PIIX4 PM.
// Provides battery charge percentage and AC adapter status.
#ifndef SMBUS_H
#define SMBUS_H

#include <stdint.h>

// Initialize the SMBus host controller.
// Probes for PIIX4 PM at I/O 0x0E00; no-ops if not found.
void smbus_init(void);

// Read battery charge percentage (0–100).
// Returns -1 if no battery is present or the read fails.
int  smbus_battery_percent(void);

// 1 if AC adapter is plugged in, 0 if on battery, -1 if unknown.
int  smbus_ac_present(void);

#endif
