// include/nic.h — generic NIC layer: auto-detects the first supported
// PCI ethernet card (e1000 first, then RTL8139 fallback) and routes
// the network stack's send/recv/MAC calls to whichever driver bound.
//
// The rest of the kernel (netstack, DNS, DHCP, browser, shell, Quick
// Settings) talks ONLY to this layer, never to a specific driver, so
// adding another card (VirtIO, e1000e…) is just another driver +
// one line in nic_init().
#ifndef NIC_H
#define NIC_H

#include <stdint.h>

// Auto-detect and initialize the NIC.  Tries the Intel e1000 driver
// first (the QEMU `-device e1000` default), then falls back to the
// RTL8139.  Returns 1 if a card came up, 0 if none was found.
// Idempotent: if a driver is already bound it returns 1 immediately.
int nic_init(void);

// Is any NIC driver currently bound and initialized?
int nic_is_up(void);

// Name of the bound driver ("e1000", "rtl8139", or "none").
const char* nic_driver_name(void);

// Transmit a packet.  Returns bytes sent, or -1 on error.
int nic_send(const void* data, int len);

// Receive a packet into buf (max max_len bytes).
// Returns bytes received, or 0 if no packet available.
int nic_recv(void* buf, int max_len);

// Get MAC address
void nic_get_mac(uint8_t mac[6]);

// PCI IRQ line the bound driver installed its handler on (0 = polled)
int nic_irq_line(void);

// Interrupt-driven counters (same contract as the driver stats).
void nic_stats(uint32_t* rx_irqs, uint32_t* rx_pkts,
               uint32_t* rx_drops, uint32_t* tx_done);

#endif
