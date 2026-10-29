// include/rtl8139.h — Realtek RTL8139 PCI Ethernet Driver
#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include <isr.h>   // registers_t (for the IRQ handler signature)

// RTL8139 register offsets (I/O space)
#define RTL_IDR0      0x00  // MAC address bytes 0-5 (6 bytes)
#define RTL_TSD0      0x10  // TX status descriptor 0
#define RTL_TSAD0     0x20  // TX start address 0
#define RTL_TSAD1     0x24  // TX start address 1
#define RTL_TSAD2     0x28  // TX start address 2
#define RTL_TSAD3     0x2C  // TX start address 3
#define RTL_RBSTART   0x30  // RX ring buffer start address
#define RTL_CR        0x37  // Command register
#define RTL_CAPR      0x38  // Current address of packet read (rx)
#define RTL_IMR       0x3C  // Interrupt mask register
#define RTL_ISR       0x3E  // Interrupt status register
#define RTL_TCR       0x40  // TX configuration
#define RTL_RCR       0x44  // RX configuration
#define RTL_9346CR    0x50  // 93C46 (EEPROM) command register
#define RTL_CONFIG1   0x52  // Config register 1

// Command register (CR) bits
#define CR_RST        0x10  // Software reset
#define CR_RE         0x08  // Receiver enable
#define CR_TE         0x04  // Transmitter enable
#define CR_BUFE       0x01  // Buffer empty

// RX configuration (RCR) bits
#define RCR_AAP       0x01  // Accept all packets (promiscuous)
#define RCR_APM       0x02  // Accept physical match (our MAC)
#define RCR_AM        0x04  // Accept multicast
#define RCR_AB        0x08  // Accept broadcast
#define RCR_WRAP      0x80  // Wrap (enable RX)

// TX configuration (TCR) bits
#define TCR_MXDMA_256 0x0700 // Max DMA burst
#define TCR_IFG96     0x030000 // Interframe gap

// TX status bits
#define TSD_TOK       0x00008000 // TX OK
#define TSD_TUN       0x00002000 // TX underrun
#define TSD_OWN       0x00002000 // TX ownership (1 = host owns it)
#define TSD_SIZE_MASK 0x00001FFF // TX size mask

// RX packet header
#define RX_OK         0x0001  // Packet received without errors
#define RX_LONG       0x0004  // Packet too long

// Interrupt status/mask register (ISR @ 0x3E, IMR @ 0x3C) bits
#define RTL_ISR_ROK   0x0001  // Receive OK — packet(s) in the RX ring
#define RTL_ISR_RER   0x0002  // Receive error
#define RTL_ISR_TOK   0x0004  // Transmit OK — frame sent
#define RTL_ISR_TER   0x0008  // Transmit error
#define RTL_ISR_RXOVW 0x0010  // RX ring overflow (chip paused)
// Bits we unmask in IRQ-driven mode (ROK | RER | TOK | TER)
#define RTL_IMR_ALL   0x000F

// Buffer sizes
#define TX_BUF_SIZE   1536
#define RX_BUF_SIZE   8192  // 8KB RX ring buffer
#define RX_BUF_MASK   (RX_BUF_SIZE - 1)

// Initialize the RTL8139 NIC. Returns 1 on success, 0 on failure.
// io_base is the I/O port base from PCI BAR0 (0 = auto-scan PCI).
// On success the driver is interrupt-driven: the PCI IRQ handler
// (rtl8139_irq_handler) drains the hardware RX ring into a software
// queue as packets arrive. If the device has no usable PCI IRQ line
// it falls back to polled mode transparently.
int rtl8139_init(uint16_t io_base);

// Transmit a packet. Returns bytes sent, or -1 on error.
int rtl8139_send(const void* data, int len);

// Receive a packet into buf (max max_len bytes).
// Returns bytes received, or 0 if no packet available.
int rtl8139_recv(void* buf, int max_len);

// Get MAC address
void rtl8139_get_mac(uint8_t mac[6]);

// Get the I/O base
uint16_t rtl8139_get_io_base(void);

// PCI IRQ line the driver installed its handler on (0 = polled fallback)
int rtl8139_irq_line(void);

// Interrupt-driven counters: rx_irqs (ISR invocations that delivered
// packets), rx_pkts (packets queued by the ISR), rx_drops (dropped
// because the software queue was full), tx_done (TX completions the
// ISR observed). Any pointer may be NULL.
void rtl8139_stats(uint32_t* rx_irqs, uint32_t* rx_pkts,
                   uint32_t* rx_drops, uint32_t* tx_done);

// ISR entry point — installed via irq_install_handler() by rtl8139_init.
void rtl8139_irq_handler(registers_t regs);

#endif
