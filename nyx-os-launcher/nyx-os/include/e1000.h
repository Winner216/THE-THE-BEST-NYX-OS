// include/e1000.h — Intel e1000 (82540EM/82545EM/82574L family) PCI
// Gigabit Ethernet driver.  The driver QEMU's `-device e1000` /
// `-nic user,model=e1000` emulates: vendor 0x8086, device 0x100E.
//
// Design mirrors rtl8139.c: RX is interrupt-driven (the PCI IRQ
// handler drains the hardware descriptor ring into a small software
// queue the moment a packet arrives), TX stays polled (the network
// stack is synchronous).  Registers are accessed through the MMIO
// BAR (BAR0 is a 32-bit memory BAR, unlike the RTL8139's I/O BAR);
// the driver maps it via paging_map_device() because it sits in the
// PCI hole above the kernel's 1 GiB identity map.
#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include <isr.h>   // registers_t (for the IRQ handler signature)

// ─── Register offsets (relative to the MMIO BAR0 base) ───────────
#define E1000_CTRL    0x0000   // Device control
#define E1000_STATUS  0x0008   // Device status
#define E1000_EERD    0x0014   // EEPROM read
#define E1000_ICR     0x00C0   // Interrupt cause read (write = clear)
#define E1000_ITR     0x00C4   // Interrupt throttling
#define E1000_ICS     0x00C8   // Interrupt cause set
#define E1000_IMS     0x00D0   // Interrupt mask set
#define E1000_IMC     0x00D8   // Interrupt mask clear
#define E1000_RCTL    0x0100   // Receive control
#define E1000_TCTL    0x0400   // Transmit control
#define E1000_TIPG    0x0410   // Transmit inter-packet gap
#define E1000_RDBAL   0x2800   // RX descriptor ring base (low)
#define E1000_RDBAH   0x2804   // RX descriptor ring base (high)
#define E1000_RDLEN   0x2808   // RX descriptor ring length
#define E1000_RDH     0x2810   // RX descriptor head
#define E1000_RDT     0x2818   // RX descriptor tail
#define E1000_TDBAL   0x3800   // TX descriptor ring base (low)
#define E1000_TDBAH   0x3804   // TX descriptor ring base (high)
#define E1000_TDLEN   0x3808   // TX descriptor ring length
#define E1000_TDH     0x3810   // TX descriptor head
#define E1000_TDT     0x3818   // TX descriptor tail
#define E1000_RAL     0x5400   // Receive address low (MAC bytes 0-3)
#define E1000_RAH     0x5404   // Receive address high (MAC bytes 4-5 + AV)
#define E1000_MTA     0x5200   // Multicast table array (128 dwords)

// CTRL bits
#define E1000_CTRL_FD   0x00000001u   // Full duplex
#define E1000_CTRL_SLU  0x00000040u   // Set link up
#define E1000_CTRL_RST  0x04000000u   // Device reset (self-clearing)

// EERD bits (EEPROM read)
#define E1000_EERD_START 0x00000001u
#define E1000_EERD_DONE  0x00000010u

// ICR / IMS bits (interrupt causes / mask)
#define E1000_ICR_TXDW   0x00000001u   // TX descriptor written back
#define E1000_ICR_TXQE   0x00000002u   // TX queue empty
#define E1000_ICR_LSC    0x00000004u   // Link status changed
#define E1000_ICR_RXSEQ  0x00000008u   // RX sequence error
#define E1000_ICR_RXDMT0 0x00000010u   // RX descriptor minimum threshold
#define E1000_ICR_RXO    0x00000040u   // RX overrun
#define E1000_ICR_RXT0   0x00000080u   // RX timer expired
// What the driver unmask+handles: TXDW|TXQE|LSC|RXSEQ|RXDMT0|RXO|RXT0.
// RXT0 (bit 7) is essential: QEMU raises it on EVERY received packet
// (RXDMT0 only fires once the ring is nearly full), so an IMS without
// it never delivers RX interrupts.
#define E1000_IMS_ALL    0x000000DFu

// RCTL bits
#define E1000_RCTL_EN    0x00000002u   // Receiver enable
#define E1000_RCTL_SBP   0x00000004u   // Store bad packets
#define E1000_RCTL_UPE   0x00000008u   // Unicast promiscuous
#define E1000_RCTL_MPE   0x00000010u   // Multicast promiscuous
#define E1000_RCTL_BAM   0x00008000u   // Broadcast accept
#define E1000_RCTL_SECRC 0x04000000u   // Strip CRC from received frames

// TCTL bits
#define E1000_TCTL_EN    0x00000002u   // Transmit enable
#define E1000_TCTL_PSP   0x00000008u   // Pad short packets

// Legacy (pre-82574) 16-byte descriptor formats, little-endian.
typedef struct __attribute__((packed)) {
    uint64_t addr;        // physical address of the packet buffer
    uint16_t length;      // received length (CRC stripped if SECRC)
    uint16_t checksum;
    uint8_t  status;      // E1000_RXD_STAT_*
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc;

typedef struct __attribute__((packed)) {
    uint64_t addr;        // physical address of the packet buffer
    uint16_t length;      // length to transmit
    uint8_t  cso;
    uint8_t  cmd;         // E1000_TXD_CMD_*
    uint8_t  status;      // E1000_TXD_STAT_DD when sent
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc;

#define E1000_RXD_STAT_DD   0x01   // descriptor done (packet received)
#define E1000_RXD_STAT_EOP  0x02   // end of packet (single-descriptor frame)

#define E1000_TXD_CMD_EOP   0x01   // end of packet
#define E1000_TXD_CMD_RS    0x08   // report status (set DD on completion)
#define E1000_TXD_CMD_IFCS  0x10   // insert FCS/CRC

#define E1000_TXD_STAT_DD   0x01   // transmit done

// Ring geometry (16-byte descriptors, 2048-byte packet buffers).
#define E1000_RX_DESC_N   32
#define E1000_RX_BUF_SZ   2048
#define E1000_TX_DESC_N   8
#define E1000_TX_BUF_SZ   2048

// Initialize the e1000 NIC.  Scans PCI for vendor 0x8086 device
// 0x100E (QEMU's default e1000; also matches 82545EM/82574L/82577
// family IDs), maps the MMIO BAR, reads the MAC from the RAL/RAH
// registers (0x5400/0x5404), arms the RX/TX descriptor rings, and
// installs the IRQ handler on the card's PCI IRQ line.  Returns 1 on
// success, 0 if no supported card is present or init failed.
int e1000_init(void);

// Transmit a packet.  Returns bytes sent, or -1 on error.
int e1000_send(const void* data, int len);

// Receive a packet into buf (max max_len bytes).
// Returns bytes received, or 0 if no packet available.
int e1000_recv(void* buf, int max_len);

// Get MAC address
void e1000_get_mac(uint8_t mac[6]);

// PCI IRQ line the driver installed its handler on (0 = none)
int e1000_irq_line(void);

// Interrupt-driven counters (same contract as rtl8139_stats).
void e1000_stats(uint32_t* rx_irqs, uint32_t* rx_pkts,
                 uint32_t* rx_drops, uint32_t* tx_done);

// ISR entry point — installed via irq_install_handler() by e1000_init.
void e1000_irq_handler(registers_t regs);

#endif
