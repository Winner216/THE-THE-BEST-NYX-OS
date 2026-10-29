// cpu/rtl8139.c — Realtek RTL8139 PCI Ethernet Driver (IRQ-driven)
//
// PIO register access. RX is interrupt-driven: the PCI IRQ handler
// drains the hardware ring into a small software queue the moment a
// packet arrives, and rtl8139_recv() pops from that queue — so the
// network stack never busy-waits on the NIC. TX stays polled (the
// stack is synchronous), but the ISR still catches TX-complete
// interrupts and counts them. If the device has no usable PCI IRQ
// line (config 0x3C = 0), the driver transparently falls back to
// pure polled mode (IMR = 0, recv drains the ring directly).
//
// Registers referenced via io_base + offset, where io_base comes
// from PCI BAR0 (which is an I/O port for the RTL8139).

#include <rtl8139.h>
#include <isr.h>          // registers_t, irq_install_handler
#include <io.h>
#include <stdint.h>

static uint16_t rtl_io = 0;        // I/O base from PCI BAR0
static uint8_t  rtl_mac[6];        // MAC address
static int      rtl_irq = 0;       // PCI IRQ line (0 = none → poll mode)
static int      rtl_pci = 0;       // 1 = bus/slot valid (PCI scan found it)
static uint8_t  rtl_bus = 0, rtl_slot = 0;  // PCI location

// TX buffers (one at a time for simplicity)
static uint8_t  tx_buffer[TX_BUF_SIZE] __attribute__((aligned(4)));
static int      tx_cur = 0;        // current TX descriptor index (0-3)

// RX ring buffer (8KB, must be aligned to 4-byte boundary)
static uint8_t  rx_ring[RX_BUF_SIZE] __attribute__((aligned(4)));
static int      rx_offset = 0;     // current read offset in RX ring

// ─── Software RX queue (single-producer/single-consumer) ──────────
// The ISR is the ONLY writer of rx_q_head and the ONLY reader of the
// hardware ring; the main thread (rtl8139_recv) is the ONLY reader of
// rx_q_tail. They never run concurrently (interrupt gates disable
// interrupts), so no lock is needed. volatile: the queue is written
// by the ISR and read by the main thread and vice versa.
#define RX_QUEUE_PKTS   8
#define RX_QUEUE_MAX    1536
static volatile uint8_t  rx_queue[RX_QUEUE_PKTS][RX_QUEUE_MAX];
static volatile uint16_t rx_queue_len[RX_QUEUE_PKTS];
static volatile uint16_t rx_q_head = 0;    // next free slot (ISR writes)
static volatile uint16_t rx_q_tail = 0;    // next packet to pop (main reads)
static volatile uint32_t rtl_rx_irqs   = 0;  // IRQs that delivered RX
static volatile uint32_t rtl_rx_pkts   = 0;  // packets queued by the ISR
static volatile uint32_t rtl_rx_drops  = 0;  // packets dropped (queue full)
static volatile uint32_t rtl_tx_done   = 0;  // TX completions seen by ISR
static volatile uint32_t rtl_rx_marker = 0;  // first-RX-IRQ latch (serial)

// ─── COM1 debug markers (bounded spin; one-shot messages only) ────
static void rtl_serial_char(char c) {
    int spins = 0;
    while ((inb(0x3F8 + 5) & 0x20) == 0 && spins++ < 200000) { }
    outb(0x3F8, (uint8_t)c);
}
static void rtl_serial_str(const char* s) {
    for (; *s; s++) rtl_serial_char(*s);
}
static void rtl_serial_num(uint32_t v) {
    char b[12]; int n = 0;
    if (v == 0) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--) rtl_serial_char(b[i]);
}

// ─── PCI config read/write (ported from net.c) ─────────────────────
static uint32_t pci_read_cfg(uint8_t bus, uint8_t slot, uint8_t func,
                              uint8_t reg) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                    (reg & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_write_cfg(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t reg, uint32_t val) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                    (reg & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

// ─── Find RTL8139 on PCI bus ────────────────────────────────────
// Scans for vendor 0x10EC (Realtek) device 0x8139.
// Returns I/O base from BAR0, or 0 if not found. Out-params carry
// the device's bus/slot so the caller can read its IRQ line.
static uint16_t pci_find_rtl8139(uint8_t* out_bus, uint8_t* out_slot) {
    for (int bus = 0; bus <= 7; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read_cfg(bus, slot, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t device = (uint16_t)(id >> 16);
            if (vendor == 0x10EC && device == 0x8139) {
                uint32_t bar0 = pci_read_cfg(bus, slot, 0, 0x10);
                if (bar0 & 1) { // I/O space
                    uint16_t io = (uint16_t)(bar0 & ~0x03);
                    // Enable bus mastering + I/O space
                    uint32_t cmd = pci_read_cfg(bus, slot, 0, 0x04);
                    cmd |= 5; // I/O space + bus master
                    pci_write_cfg(bus, slot, 0, 0x04, cmd);
                    *out_bus  = (uint8_t)bus;
                    *out_slot = (uint8_t)slot;
                    return io;
                }
            }
        }
    }
    return 0;
}

// ─── Drain the hardware RX ring into the software queue ───────────
// Called from the ISR (IRQ mode) or from rtl8139_recv (poll fallback
// mode, where the ISR never runs so there is no concurrency).
// Bounded: processes at most 32 packets per invocation so the ISR
// stays short. Re-process guard: if the same (offset,status,size)
// triplet appears twice in a row, the slot held stale data (the chip
// leaves old descriptors in place after the driver advances CAPR) —
// stop rather than re-deliver a duplicate.
static void rtl_hw_drain_to_queue(void) {
    int guard = 0, last_off = -1;
    uint16_t last_status = 0, last_size = 0;
    while (guard++ < 32) {
        int off = rx_offset;
        uint16_t status = *(volatile uint16_t*)(rx_ring + off);
        uint16_t size   = *(volatile uint16_t*)(rx_ring + off + 2);
        if (status == 0 || status == 0xFFFF) break;       // ring empty
        if (off == last_off && status == last_status && size == last_size)
            break;                                        // stale slot
        last_off = off; last_status = status; last_size = size;

        int pkt_len = (size + 4 + 3) & ~3;                // round to 4
        if (!(status & RX_OK)) {                          // error pkt: skip
            rx_offset = (rx_offset + pkt_len) & RX_BUF_MASK;
            outw(rtl_io + RTL_CAPR, (uint16_t)(rx_offset - 0x10));
            continue;
        }
        int data_len = size - 4;                          // strip CRC
        if (data_len < 0) data_len = 0;
        if (data_len > RX_QUEUE_MAX) data_len = RX_QUEUE_MAX;

        uint16_t h = rx_q_head;
        uint16_t next = (uint16_t)((h + 1) % RX_QUEUE_PKTS);
        if (next == rx_q_tail) {
            rtl_rx_drops++;          // queue full — drop, keep hw ring moving
        } else {
            for (int i = 0; i < data_len; i++)
                rx_queue[h][i] = rx_ring[(off + 4 + i) & RX_BUF_MASK];
            rx_queue_len[h] = (uint16_t)data_len;
            rx_q_head = next;
            rtl_rx_pkts++;
        }
        rx_offset = (rx_offset + pkt_len) & RX_BUF_MASK;
        outw(rtl_io + RTL_CAPR, (uint16_t)(rx_offset - 0x10));
    }
}

// Non-destructive check: is a packet waiting in the hardware ring?
static int rtl_hw_has_packet(void) {
    if (!rtl_io) return 0;
    uint16_t status = *(volatile uint16_t*)(rx_ring + rx_offset);
    return (status != 0 && status != 0xFFFF);
}

// ─── ISR: packet arrival + transmission events ────────────────────
// The PIC is edge-triggered; the RTL8139 asserts its IRQ line while
// ISR bits are set. A packet that arrives between our read-ISR and
// our ack-write would have its ROK bit cleared by the ack with no new
// edge to re-deliver it — it would strand in the ring until the next
// packet. The drain→ack→recheck loop closes that window: anything the
// recheck finds was cleared by our ack, so we drain it before leaving.
void rtl8139_irq_handler(registers_t regs) {
    (void)regs;
    if (!rtl_io) return;

    int rounds = 0;
    do {
        uint16_t isr = inw(rtl_io + RTL_ISR);
        uint16_t ours = (uint16_t)(isr & RTL_IMR_ALL);  // ROK|RER|TOK|TER
        if (!ours) break;                               // shared/spurious IRQ

        if (isr & (RTL_ISR_ROK | RTL_ISR_RER)) {
            rtl_rx_irqs++;
            if (!rtl_rx_marker) {                       // one-shot marker
                rtl_rx_marker = 1;
                rtl_serial_str("NET RX IRQ\n");
            }
            rtl_hw_drain_to_queue();
        }
        if (isr & (RTL_ISR_TOK | RTL_ISR_TER))
            rtl_tx_done++;

        outw(rtl_io + RTL_ISR, ours);                   // ack what we handled
    } while (++rounds < 8 && rtl_hw_has_packet());
}

// ─── Initialization ──────────────────────────────────────────────
int rtl8139_init(uint16_t io_base) {
    uint8_t bus = 0, slot = 0;
    // If io_base is 0, try PCI scan
    if (io_base == 0) {
        io_base = pci_find_rtl8139(&bus, &slot);
        if (io_base == 0) return 0;
        rtl_pci  = 1;
        rtl_bus  = bus;
        rtl_slot = slot;
    }
    rtl_io = io_base;

    // Software reset — use tick-based timeout (up to ~1 second)
    extern volatile uint32_t tick;
    uint32_t rst_deadline = tick + 18;
    outb(rtl_io + RTL_CR, CR_RST);
    while ((inb(rtl_io + RTL_CR) & CR_RST) && (int)(tick - rst_deadline) < 0)
        inb(rtl_io + RTL_ISR); // small I/O delay

    if (inb(rtl_io + RTL_CR) & CR_RST) return 0;

    // Turn on the EEPROM (93C46) so we can configure
    outb(rtl_io + RTL_9346CR, 0xC0); // eeprom programming mode
    // Actually for simple operation, just set config register
    outb(rtl_io + RTL_CONFIG1, 0x00);

    // Read MAC address from IDR0
    for (int i = 0; i < 6; i++)
        rtl_mac[i] = inb(rtl_io + RTL_IDR0 + i);

    // Initialize RX ring buffer
    rx_offset = 0;
    rx_q_head = rx_q_tail = 0;
    uint32_t rx_phys = (uint32_t)(uintptr_t)rx_ring;
    outl(rtl_io + RTL_RBSTART, rx_phys);

    // Enable TX and RX
    outl(rtl_io + RTL_TCR, TCR_MXDMA_256 | TCR_IFG96); // TX config
    outl(rtl_io + RTL_RCR, RCR_AB | RCR_APM | RCR_AM); // RX: broadcast + physical match + multicast
    outb(rtl_io + RTL_CR, CR_TE | CR_RE); // Enable TX and RX

    // ── Interrupts ────────────────────────────────────────────────
    // Read the PCI interrupt line (config 0x3C). If valid, go
    // IRQ-driven: install the ISR and unmask ROK|RER|TOK|TER. The
    // ISR drains the hardware ring into the software queue; recv()
    // only pops the queue. If there's no usable line, mask all
    // interrupts and stay polled (recv drains the ring directly).
    // RTL8139_FORCE_POLL: test hook to A/B the IRQ path vs the old
    // polled path without changing hardware.
    rtl_irq = 0;
#ifndef RTL8139_FORCE_POLL
    if (rtl_pci) {
        uint8_t irq = (uint8_t)(pci_read_cfg(rtl_bus, rtl_slot, 0, 0x3C) & 0xFF);
        if (irq > 0 && irq < 16) {
            rtl_irq = irq;
            irq_install_handler(rtl_irq, rtl8139_irq_handler);
        }
    }
#endif
    outw(rtl_io + RTL_IMR, rtl_irq ? RTL_IMR_ALL : 0x0000);

    // Acknowledge any pending IRQ
    outw(rtl_io + RTL_ISR, 0xFFFF);

    // One-shot COM1 marker so the E2E harness can assert driver state.
    rtl_serial_str("NIC UP irq=");
    rtl_serial_num((uint32_t)rtl_irq);
    rtl_serial_str(rtl_irq ? " irq-driven\n" : " polled\n");

    return 1;
}

// ─── Get MAC ─────────────────────────────────────────────────────
void rtl8139_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = rtl_mac[i];
}

uint16_t rtl8139_get_io_base(void) { return rtl_io; }

int rtl8139_irq_line(void) { return rtl_irq; }

void rtl8139_stats(uint32_t* rx_irqs, uint32_t* rx_pkts,
                   uint32_t* rx_drops, uint32_t* tx_done) {
    if (rx_irqs)  *rx_irqs  = rtl_rx_irqs;
    if (rx_pkts)  *rx_pkts  = rtl_rx_pkts;
    if (rx_drops) *rx_drops = rtl_rx_drops;
    if (tx_done)  *tx_done  = rtl_tx_done;
}

// ─── Transmit (polled) ──────────────────────────────────────────
// The stack is synchronous, so TX completion is waited on here; the
// ISR additionally observes TOK/TER and counts them.
int rtl8139_send(const void* data, int len) {
    if (!rtl_io || len <= 0 || len > TX_BUF_SIZE) return -1;

    // Copy data to TX buffer
    for (int i = 0; i < len; i++)
        tx_buffer[i] = ((const uint8_t*)data)[i];

    // Pad to minimum ethernet frame size (60 bytes)
    if (len < 60) {
        for (int i = len; i < 60; i++)
            tx_buffer[i] = 0;
        len = 60;
    }

    // Wait for TX descriptor to be available (poll OWN bit)
    int timeout = 5000;
    while (timeout--) {
        uint32_t tsd = inl(rtl_io + RTL_TSD0 + tx_cur * 4);
        if ((inb(rtl_io + RTL_CR) & CR_BUFE) || (tsd & TSD_TOK)) {
            // TX descriptor is free
            break;
        }
    }
    if (timeout <= 0) return -1;

    // Set TX buffer address
    outl(rtl_io + RTL_TSAD0 + tx_cur * 4, (uint32_t)(uintptr_t)tx_buffer);

    // Set TX status (size + start TX)
    outl(rtl_io + RTL_TSD0 + tx_cur * 4, len | 0x80000000); // bit 31 = early TX

    // Advance to next descriptor
    tx_cur = (tx_cur + 1) % 4;

    // Wait for TX completion (poll TOK bit)
    timeout = 5000;
    while (timeout--) {
        uint32_t tsd = inl(rtl_io + RTL_TSD0 + ((tx_cur - 1 + 4) % 4) * 4);
        if (tsd & TSD_TOK) return len;
    }
    return -1; // TX timeout
}

// ─── Receive ─────────────────────────────────────────────────────
// IRQ mode: pops the software queue (the ISR is the only hardware-ring
// consumer, so there is no race). Poll fallback (no IRQ line): drains
// the hardware ring into the queue inline, then pops — same code path,
// just driven by the caller instead of the ISR.
int rtl8139_recv(void* buf, int max_len) {
    if (!rtl_io || max_len <= 0) return 0;

    // The RTL8139 is a level-triggered PCI device on the edge-triggered
    // 8259 PIC: a packet that lands while the ISR is acking can lose its
    // edge and never reach the ISR. recv() is therefore the authoritative
    // ring consumer — it ALWAYS drains the hardware ring, ISR or not.
    // cli makes this race-free: the ISR runs with interrupts gated (IDT
    // interrupt gate), so it can never touch the ring mid-drain. After
    // draining, ack the NIC's ISR register so the asserted level drops
    // and the NEXT packet's edge can be delivered to the PIC.
    asm volatile("cli");
    rtl_hw_drain_to_queue();
    if (rtl_irq != 0 && rx_q_tail == rx_q_head && rtl_hw_has_packet())
        rtl_hw_drain_to_queue();   // drained while an edge was in flight
    outw(rtl_io + RTL_ISR, 0xFFFF);  // drop the level; packets drained above
    asm volatile("sti");

    if (rx_q_tail == rx_q_head) return 0;

    uint16_t t = rx_q_tail;
    int data_len = rx_queue_len[t];
    if (data_len > max_len) data_len = max_len;
    for (int i = 0; i < data_len; i++)
        ((uint8_t*)buf)[i] = rx_queue[t][i];
    rx_q_tail = (uint16_t)((t + 1) % RX_QUEUE_PKTS);
    return data_len;
}
