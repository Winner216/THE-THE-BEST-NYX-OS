// cpu/smbus.c  –  SMBus host controller driver for QEMU PIIX4 PM
//
// QEMU emulates a PIIX4 PM device that exposes battery and AC adapter
// information through SMBus (System Management Bus).  The SMBus host
// controller registers live at I/O port 0x0E00 (ISA bridge, function 3).
//
// SMBus block-read protocol (what we use):
//   1. Write SMBus address register  (slave addr + read bit)
//   2. Write SMBus command register  (register offset, e.g. 0x00=status, 0x01=info)
//   3. Write SMBus control register  (START + block-read, value 0x18)
//   4. Wait for HOST_BUSY to clear   (timeout 10 ms)
//   5. Read data byte from SMBus data register
//
// Battery addresses and commands (standard ACPI SMBus):
//   Slave 0x14 = primary battery, command 0x00 = _BST (status word)
//   Slave 0x14 = primary battery, command 0x01 = _BIF (info block)
//
// _BST returns 4 uint16_t LE words: state, rate, voltage, remaining.
// _BIF returns 16 uint32_t LE words: design capacity, last full, tech,
//   design voltage, capacity warning, low warning, + 10 spare uint32_t.
//
// We keep the driver dead-simple: no IRQ, no DMA, just polled I/O with
// a 10 ms timeout.  Called once per frame from draw_taskbar() which runs
// at ~60 Hz, so the 10 ms timeout is well within the 16 ms frame budget.

#include <smbus.h>
#include <io.h>
#include <stdint.h>

// ─── PIIX4 PM SMBus host controller registers ────────────────────
#define SMB_BASE    0x0E00   // ISA bridge function 3, offset 0xE0

// Register offsets (16-bit I/O port spacing)
#define SMB_STS     (SMB_BASE + 0x00)   // Host Status (R/W)
#define SMB_HOSTCFG (SMB_BASE + 0x02)   // Host Configuration (R/W)
#define SMB_ADDR    (SMB_BASE + 0x04)   // Slave Address (R/W)
#define SMB_CMD     (SMB_BASE + 0x06)   // Command / Register Pointer (R/W)
#define SMB_DATA    (SMB_BASE + 0x08)   // Data (R/W, 8-bit)
#define SMB_BLOCK   (SMB_BASE + 0x0A)   // Block Data (R/W, 32-byte block)

// Host Status register bits
#define SMB_HOST_BUSY   0x01
#define SMB_SMB_ALERT   0x02
#define SMB_FAILED      0x04
#define SMB_STATUS      0x1C   // mask for status bits [4:2]
#define SMB_STATUS_OK   0x00   // completed successfully
#define SMB_STATUS_DONE 0x02   // byte transfer complete

// Host Control register values
#define SMB_CTRL_START  0x10   // generate START condition
#define SMB_CTRL_BLOCK  0x08   // block-read mode
#define SMB_CTRL_KILL   0x04   // kill current transaction

// Slave addresses (7-bit, shifted left by 1 for R/W bit)
#define SMB_ADDR_BAT0   0x14   // primary battery
#define SMB_ADDR_AC     0x09   // AC adapter / charger

// Battery register offsets
#define SMB_BAT_STATUS  0x00   // _BST: 4x uint16_t (state, rate, voltage, remaining)
#define SMB_BAT_INFO    0x01   // _BIF: 16x uint32_t (design cap, last full, ...)

// Timeout for SMBus transaction (iterations of a tight loop ≈ 10 ms)
#define SMB_TIMEOUT     50000

// ─── Internal state ──────────────────────────────────────────────
static int smbus_present = 0;   // 1 if PIIX4 PM SMBus was found

// ─── Low-level helpers ───────────────────────────────────────────

// Wait for HOST_BUSY bit to clear (transaction complete).
// Returns 0 on success, -1 on timeout.
static int smb_wait(void) {
    for (int i = 0; i < SMB_TIMEOUT; i++) {
        uint8_t st = inb(SMB_STS);
        if (!(st & SMB_HOST_BUSY)) return 0;
    }
    // Timeout — kill the hung transaction
    outb(SMB_STS, inb(SMB_STS) | SMB_CTRL_KILL);
    return -1;
}

// Reset the SMBus controller to a known-good state.
static void smb_reset(void) {
    outb(SMB_STS, 0xFF);    // clear all status bits
    outb(SMB_HOSTCFG, 0x00); // disable interrupts, enable controller
}

// Perform an SMBus block read:
//   slave = 7-bit slave address (e.g. 0x14 for battery)
//   cmd   = register/command offset
//   buf   = output buffer (at least 32 bytes; only first byte used for simple reads)
// Returns 0 on success, -1 on failure.
static int smb_block_read(uint8_t slave, uint8_t cmd, uint8_t* buf) {
    // Clear status
    outb(SMB_STS, 0xFF);

    // Set slave address with read bit (bit 0 = 1)
    outb(SMB_ADDR, (slave << 1) | 0x01);

    // Set command / register pointer
    outb(SMB_CMD, cmd);

    // Start block read (START + block-read)
    outb(SMB_HOSTCFG, SMB_CTRL_START | SMB_CTRL_BLOCK);

    // Wait for transaction to complete
    if (smb_wait() < 0) {
        smb_reset();
        return -1;
    }

    // Check for errors
    uint8_t st = inb(SMB_STS);
    if (st & SMB_FAILED) {
        smb_reset();
        return -1;
    }

    // For a block read, the first byte in the block register is the
    // byte count.  Read it, then read that many data bytes.
    uint8_t block_data[33];
    for (int i = 0; i < 33; i++)
        block_data[i] = inb(SMB_BLOCK);

    uint8_t count = block_data[0];
    if (count > 32) count = 32;
    for (int i = 0; i < count; i++)
        buf[i] = block_data[i + 1];

    return count > 0 ? 0 : -1;
}

// ─── Public API ──────────────────────────────────────────────────

void smbus_init(void) {
    // Probe: try a dummy read from the battery device.
    // If the SMBus controller isn't present (non-QEMU or missing PIIX4),
    // the reads will time out and we disable ourselves.
    smb_reset();

    uint8_t buf[32];
    int rc = smb_block_read(SMB_ADDR_BAT0, SMB_BAT_STATUS, buf);
    if (rc == 0) {
        smbus_present = 1;
    } else {
        // Also try the AC adapter address as a secondary probe
        rc = smb_block_read(SMB_ADDR_AC, 0x00, buf);
        smbus_present = (rc == 0) ? 1 : 0;
    }
}

int smbus_battery_percent(void) {
    if (!smbus_present) return -1;

    uint8_t buf[32];
    int rc = smb_block_read(SMB_ADDR_BAT0, SMB_BAT_INFO, buf);
    if (rc < 0) return -1;

    // _BIF info block (16 x uint32_t LE):
    //   [0]  Design Capacity (mAh)
    //   [1]  Last Full Charge Capacity (mAh)
    //   [2]  Battery Technology (1=rechargeable)
    //   [3]  Design Voltage (mV)
    //   [4]  Capacity Warning (mAh)
    //   [5]  Low Capacity Warning (mAh)
    //   [6-15] spare
    uint32_t design_cap  = buf[0]  | (buf[1]  << 8) | (buf[2]  << 16) | (buf[23] << 24);
    uint32_t last_full   = buf[4]  | (buf[5]  << 8) | (buf[6]  << 16) | (buf[7]  << 24);

    // Now read _BST to get remaining capacity
    rc = smb_block_read(SMB_ADDR_BAT0, SMB_BAT_STATUS, buf);
    if (rc < 0) return -1;

    // _BST status block (4 x uint16_t LE):
    //   [0-1]  Battery State (1=discharging, 2=charging, 4=full)
    //   [2-3]  Present Rate (mA)
    //   [4-5]  Present Voltage (mV)
    //   [6-7]  Remaining Capacity (mAh)
    uint16_t state     = buf[0] | (buf[1] << 8);
    uint16_t remaining = buf[6] | (buf[7] << 8);

    // Calculate percentage
    uint32_t base = design_cap > 0 ? design_cap : last_full;
    if (base == 0) return -1;

    int pct = (int)((uint32_t)remaining * 100 / base);
    if (pct < 0)   pct = 0;
    if (pct > 100)  pct = 100;

    // If fully charged (state bit 2 = 4), report 100%
    if (state & 0x04) pct = 100;

    return pct;
}

int smbus_ac_present(void) {
    if (!smbus_present) return -1;

    uint8_t buf[32];
    int rc = smb_block_read(SMB_ADDR_AC, 0x00, buf);
    if (rc < 0) return -1;

    // AC adapter status: byte 0, bit 0 = 1 when present
    return (buf[0] & 0x01) ? 1 : 0;
}
