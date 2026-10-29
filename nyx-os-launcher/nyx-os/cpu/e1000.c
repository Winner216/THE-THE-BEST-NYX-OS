// cpu/e1000.c — Intel e1000 (82540EM) PCI Gigabit Ethernet Driver
//
// Drives QEMU's `-device e1000` / `-nic user,model=e1000` (PCI
// 8086:100E).  Unlike the RTL8139, the e1000 exposes its registers
// through a MEMORY BAR (BAR0), so the driver maps that BAR via
// paging_map_device() — BARs live in the 0xF0000000+ PCI hole,
// above the kernel's 1 GiB identity map — and reads/writes them as
// volatile MMIO.
//
// Data path is DMA: the card reads/writes descriptor rings the
// driver placed in guest RAM (RDBA/TDBA + RDLEN/TDLEN) and DMA's
// packets into/out of fixed buffers.  RX is interrupt-driven: the
// PCI IRQ handler (e1000_irq_handler, installed via the IDT through
// irq_install_handler) drains descriptors with DD set into a small
// software queue the moment a packet arrives; e1000_recv() pops that
// queue.  TX stays polled (the stack is synchronous) — send copies
// into a TX buffer, hands the descriptor to the card via TDT, and
// spins on the RS write-back bit.
//
// The MAC address comes straight from the card's RAL/RAH registers
// (0x5400 / 0x5404), which QEMU preloads with the virtual MAC it
// assigned (an EEPROM read via EERD would be needed on real
// hardware; the registers are the authoritative source here).

#include <e1000.h>
#include <isr.h>          // registers_t, irq_install_handler
#include <io.h>
#include <paging.h>
#include <stdint.h>

static volatile uint32_t* e1000_mmio = 0;   // BAR0, mapped & cast
static uint8_t  e1000_mac[6];               // MAC address
static int      e1000_irq = 0;              // PCI IRQ line (0 = none)
static int      e1000_pci = 0;              // bus/slot valid
static uint8_t  e1000_bus = 0, e1000_slot = 0;
static int      e1000_ok  = 0;

// ─── Descriptor rings + packet buffers (BSS is identity-mapped, so
//     (uint32_t)&x IS the physical address the DMA engine reads).
//     16-byte alignment: the card walks descriptors 16 at a time and
//     real hardware faults on a misaligned ring.
static e1000_rx_desc rx_desc[E1000_RX_DESC_N]      __attribute__((aligned(16)));
static uint8_t       rx_buffers[E1000_RX_DESC_N][E1000_RX_BUF_SZ]
        __attribute__((aligned(16)));
static e1000_tx_desc tx_desc[E1000_TX_DESC_N]      __attribute__((aligned(16)));
static uint8_t       tx_buffers[E1000_TX_DESC_N][E1000_TX_BUF_SZ]
        __attribute__((aligned(16)));

static int rx_next = 0;      // next RX descriptor the ISR consumes
static int tx_cur  = 0;      // next TX descriptor to use
static int tx_handed = 0;    // total TX descriptors handed to the card

// ─── Software RX queue (single-producer/single-consumer) ──────────
// The ISR is the ONLY writer of rx_q_head; the main thread (recv) is
// the ONLY reader of rx_q_tail.  They never run concurrently (the
// IDT uses interrupt gates, so the ISR gates interrupts; recv wraps
// its drain in cli/sti) — no lock needed.
#define E1000_RX_QUEUE_PKTS  8
#define E1000_RX_QUEUE_MAX   2048
static volatile uint8_t  rx_queue[E1000_RX_QUEUE_PKTS][E1000_RX_QUEUE_MAX];
static volatile uint16_t rx_queue_len[E1000_RX_QUEUE_PKTS];
static volatile uint16_t rx_q_head = 0;   // next free slot (ISR writes)
static volatile uint16_t rx_q_tail = 0;   // next packet to pop (main reads)
static volatile uint32_t e1000_rx_irqs  = 0;  // IRQs that delivered RX
static volatile uint32_t e1000_rx_pkts  = 0;  // packets queued by the ISR
static volatile uint32_t e1000_rx_drops = 0;  // dropped (queue full)
static volatile uint32_t e1000_tx_done  = 0;  // TX completions seen by ISR
static volatile uint32_t e1000_rx_marker = 0; // first-RX-IRQ latch (serial)

// ─── COM1 debug markers (bounded spin; one-shot messages only) ────
static void e1000_serial_char(char c) {
    int spins = 0;
    while ((inb(0x3F8 + 5) & 0x20) == 0 && spins++ < 200000) { }
    outb(0x3F8, (uint8_t)c);
}
static void e1000_serial_str(const char* s) {
    for (; *s; s++) e1000_serial_char(*s);
}
static void e1000_serial_num(uint32_t v) {
    char b[12]; int n = 0;
    if (v == 0) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--) e1000_serial_char(b[i]);
}

// ─── MMIO register access ────────────────────────────────────────
static inline uint32_t e1000_read(uint32_t reg) {
    return e1000_mmio[reg >> 2];
}
static inline void e1000_write(uint32_t reg, uint32_t val) {
    e1000_mmio[reg >> 2] = val;
}

// ─── PCI config read/write (same 0xCF8/0xCFC mechanism) ───────────
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

// ─── Find an e1000 on the PCI bus ────────────────────────────────
// Scans for Intel vendor 0x8086 with an e1000-family device ID
// (QEMU's default is the 82540EM, 0x100E; the 82545EM/82574L/82577
// variants share the same register interface).  Returns the MMIO
// BAR0 base, or 0 if not found.  Out-params carry bus/slot.
static uint32_t pci_find_e1000(uint8_t* out_bus, uint8_t* out_slot) {
    static const uint16_t ids[] = { 0x100E, 0x100F, 0x10D3, 0x10EA, 0x10EB };
    for (int bus = 0; bus <= 7; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read_cfg((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t device = (uint16_t)(id >> 16);
            if (vendor != 0x8086) continue;
            int is_e1000 = 0;
            for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
                if (device == ids[i]) { is_e1000 = 1; break; }
            if (!is_e1000) continue;

            uint32_t bar0 = pci_read_cfg((uint8_t)bus, (uint8_t)slot, 0, 0x10);
            if (bar0 & 1) continue;          // e1000 uses a MEMORY BAR
            // Enable memory space + bus mastering (the card must be a
            // bus master to DMA packets into the RX ring).
            uint32_t cmd = pci_read_cfg((uint8_t)bus, (uint8_t)slot, 0, 0x04);
            cmd |= 0x0006;                    // memory space + bus master
            pci_write_cfg((uint8_t)bus, (uint8_t)slot, 0, 0x04, cmd);
            *out_bus  = (uint8_t)bus;
            *out_slot = (uint8_t)slot;
            return bar0 & ~0xFu;
        }
    }
    return 0;
}

// ─── Drain the RX descriptor ring into the software queue ─────────
// Called from the ISR (IRQ mode) or from e1000_recv (belt-and-
// suspenders drain).  Bounded: processes at most 64 descriptors per
// invocation so the ISR stays short.  Descriptors are re-armed as
// they're consumed: status is cleared first, then RDT is advanced so
// the card can refill from the next slot.
static void e1000_drain_rx(void) {
    int guard = 0;
    while (guard++ < 64) {
        e1000_rx_desc* d = &rx_desc[rx_next];
        if (!(d->status & E1000_RXD_STAT_DD)) break;

        int len = d->length;
        // Frames spanning multiple descriptors (packet > 2048) or
        // error/oversized frames can't be queued — drop and re-arm.
        if (!(d->status & E1000_RXD_STAT_EOP) || len <= 0 ||
            len > E1000_RX_QUEUE_MAX) {
            e1000_rx_drops++;
            d->status = 0;
            e1000_write(E1000_RDT, (uint32_t)rx_next);
            rx_next = (rx_next + 1) % E1000_RX_DESC_N;
            continue;
        }

        uint16_t h = rx_q_head;
        uint16_t next = (uint16_t)((h + 1) % E1000_RX_QUEUE_PKTS);
        if (next == rx_q_tail) {
            e1000_rx_drops++;          // queue full — drop, keep ring moving
        } else {
            const uint8_t* src = rx_buffers[rx_next];
            for (int i = 0; i < len; i++)
                rx_queue[h][i] = src[i];
            rx_queue_len[h] = (uint16_t)len;
            rx_q_head = next;
            e1000_rx_pkts++;
        }
        d->status = 0;
        e1000_write(E1000_RDT, (uint32_t)rx_next);
        rx_next = (rx_next + 1) % E1000_RX_DESC_N;
    }
}

// Non-destructive check: is a packet waiting in the descriptor ring?
static int e1000_rx_pending(void) {
    return (rx_desc[rx_next].status & E1000_RXD_STAT_DD) ? 1 : 0;
}

// ─── ISR: packet arrival + transmission events ────────────────────
// Reads ICR, handles the bits we unmasked, then write-1-to-clears
// them so the card's interrupt line drops (the PIC edge for the NEXT
// packet can then be delivered).  The drain→ack→recheck loop closes
// the edge window: a packet that lands while we're acking is found
// by the recheck and drained before we leave.  If ICR has no bits we
// unmasked, the interrupt belongs to ANOTHER device sharing this
// PCI line (e.g. AC'97) — return without touching ICR.
void e1000_irq_handler(registers_t regs) {
    (void)regs;
    if (!e1000_ok) return;

    int rounds = 0;
    do {
        uint32_t icr = e1000_read(E1000_ICR);
        uint32_t ours = icr & E1000_IMS_ALL;
        if (!ours) break;                        // shared/spurious IRQ

        if (icr & (E1000_ICR_RXDMT0 | E1000_ICR_RXT0 | E1000_ICR_RXO)) {
            e1000_rx_irqs++;
            if (!e1000_rx_marker) {              // one-shot marker
                e1000_rx_marker = 1;
                e1000_serial_str("NET RX IRQ\n");
            }
            e1000_drain_rx();
        }
        if (icr & (E1000_ICR_TXDW | E1000_ICR_TXQE))
            e1000_tx_done++;

        e1000_write(E1000_ICR, ours);            // ack what we handled
    } while (++rounds < 8 && e1000_rx_pending());
}

// ─── Initialization ──────────────────────────────────────────────
int e1000_init(void) {
    if (e1000_ok) return 1;

    uint8_t bus = 0, slot = 0;
    uint32_t bar0 = pci_find_e1000(&bus, &slot);
    if (!bar0) return 0;
    e1000_pci  = 1;
    e1000_bus  = bus;
    e1000_slot = slot;

    // Map the MMIO BAR (identity, so the physical address is the
    // virtual address).  The BAR is 128 KB; 0x20000 covers it.
    paging_map_device(bar0, 0x20000);
    e1000_mmio = (volatile uint32_t*)(uintptr_t)bar0;

    // Device reset — bounded by the tick clock.
    extern volatile uint32_t tick;
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);
    uint32_t rst_deadline = tick + 50;
    while ((e1000_read(E1000_CTRL) & E1000_CTRL_RST) &&
           (int)(tick - rst_deadline) < 0) { }
    if (e1000_read(E1000_CTRL) & E1000_CTRL_RST) return 0;

    // Force the link up (QEMU's default link is up; SLU makes it
    // explicit and avoids waiting for an LSC interrupt).  QEMU's
    // e1000 gates BOTH directions on STATUS.LU, and the bit only
    // becomes set after the emulated PHY's autonegotiation completes
    // (a few ticks after reset) — transmitting before it is set makes
    // e1000_tx_burst() silently drop the frame and never write back
    // DD, so the driver spins until timeout.  Wait for it here and
    // again in send().
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_SLU);
    for (int li = 0; li < 5000000 && !(e1000_read(E1000_STATUS) & 0x02); li++) { }

    // ── MAC address from the RAL/RAH registers (0x5400/0x5404) ────
    // QEMU preloads these with the virtual MAC it assigned the card.
    // RAL = MAC bytes 0-3 (little-endian dword); RAH = bytes 4-5 in
    // bits 31:16 with the AV (address valid) bit set.
    uint32_t ral = e1000_read(E1000_RAL);
    uint32_t rah = e1000_read(E1000_RAH);
    e1000_mac[0] = (uint8_t)(ral & 0xFF);
    e1000_mac[1] = (uint8_t)((ral >> 8) & 0xFF);
    e1000_mac[2] = (uint8_t)((ral >> 16) & 0xFF);
    e1000_mac[3] = (uint8_t)((ral >> 24) & 0xFF);
    e1000_mac[4] = (uint8_t)((rah >> 16) & 0xFF);
    e1000_mac[5] = (uint8_t)((rah >> 24) & 0xFF);

    // ── RX ring: point RDBA/RDLEN at the descriptors, arm every
    //    buffer (RDT = N-1), then enable the receiver. ─────────────
    rx_next = 0;
    for (int i = 0; i < E1000_RX_DESC_N; i++) {
        rx_desc[i].addr   = (uint32_t)(uintptr_t)rx_buffers[i];
        rx_desc[i].status = 0;
    }
    e1000_write(E1000_RDBAL, (uint32_t)(uintptr_t)rx_desc);
    e1000_write(E1000_RDBAH, 0);
    e1000_write(E1000_RDLEN, (uint32_t)(E1000_RX_DESC_N * 16));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, (uint32_t)(E1000_RX_DESC_N - 1));
    // EN + SBP + UPE + MPE + BAM (accept everything the slirp gateway
    // sends us) + SECRC (strip CRC so length == payload length).
    // BSIZE bits 16-17 = 00 → 2048-byte buffers, matching RX_BUF_SZ.
    e1000_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_SBP |
                            E1000_RCTL_UPE | E1000_RCTL_MPE |
                            E1000_RCTL_BAM | E1000_RCTL_SECRC);

    // ── TX ring: point TDBA/TDLEN at the descriptors. ─────────────
    tx_cur = 0;
    tx_handed = 0;
    for (int i = 0; i < E1000_TX_DESC_N; i++) {
        tx_desc[i].addr   = (uint32_t)(uintptr_t)tx_buffers[i];
        tx_desc[i].status = 0;
    }
    e1000_write(E1000_TDBAL, (uint32_t)(uintptr_t)tx_desc);
    e1000_write(E1000_TDBAH, 0);
    e1000_write(E1000_TDLEN, (uint32_t)(E1000_TX_DESC_N * 16));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);
    // EN + PSP (pad short frames) + CT=0x0F + COLD=0x40 — the
    // standard 10/100 configuration values.
    e1000_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                            (0x0Fu << 4) | (0x40u << 12));
    e1000_write(E1000_TIPG, 0x0060200Au);

    // ── Interrupts ────────────────────────────────────────────────
    // Mask everything, clear any pending cause, then unmask the RX/TX
    // set.  The PCI IRQ line comes from config 0x3C; without a usable
    // line we stay masked (recv drains the ring directly — polled).
    e1000_write(E1000_IMC, 0xFFFFFFFFu);
    e1000_write(E1000_ICR, 0xFFFFFFFFu);
    e1000_irq = 0;
    if (e1000_pci) {
        uint8_t irq = (uint8_t)(pci_read_cfg(e1000_bus, e1000_slot, 0, 0x3C) & 0xFF);
        if (irq > 0 && irq < 16) {
            e1000_irq = irq;
            irq_install_handler(irq, e1000_irq_handler);
            e1000_write(E1000_IMS, E1000_IMS_ALL);
        }
    }

    e1000_ok = 1;
    rx_q_head = rx_q_tail = 0;

    // One-shot COM1 marker so the E2E harness can assert driver state.
    e1000_serial_str("E1000 NIC UP irq=");
    e1000_serial_num((uint32_t)e1000_irq);
    e1000_serial_str(e1000_irq ? " irq-driven" : " polled");
    e1000_serial_str(" lu=");
    e1000_serial_num((e1000_read(E1000_STATUS) & 0x02) ? 1 : 0);
    e1000_serial_str("\n");

    return 1;
}

// ─── Get MAC ─────────────────────────────────────────────────────
void e1000_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = e1000_mac[i];
}

int e1000_irq_line(void) { return e1000_irq; }

void e1000_stats(uint32_t* rx_irqs, uint32_t* rx_pkts,
                 uint32_t* rx_drops, uint32_t* tx_done) {
    if (rx_irqs)  *rx_irqs  = e1000_rx_irqs;
    if (rx_pkts)  *rx_pkts  = e1000_rx_pkts;
    if (rx_drops) *rx_drops = e1000_rx_drops;
    if (tx_done)  *tx_done  = e1000_tx_done;
}

// ─── Transmit (polled) ──────────────────────────────────────────
int e1000_send(const void* data, int len) {
    if (!e1000_ok || len <= 0 || len > E1000_TX_BUF_SZ) return -1;

    e1000_tx_desc* d = &tx_desc[tx_cur];
    // VOLATILE: the card DMA-writes DD into this byte.  A plain
    // non-volatile read lets GCC prove the spin loop has no side
    // effects and DELETE it (both waits below compiled away as a
    // single read — send then "timed out" instantly even though the
    // frame went out).  The RTL8139 driver never hit this because its
    // waits spin on I/O-port reads, which are always observable.
    volatile uint8_t* st = &d->status;

    // QEMU's e1000 drops frames until STATUS.LU is set (see init) —
    // wait it out here too, so a send racing the link-up event can't
    // strand a descriptor.  Bounded: give up after ~5M reads.
    for (int li = 0; li < 5000000 && !(e1000_read(E1000_STATUS) & 0x02); li++) { }
    if (!(e1000_read(E1000_STATUS) & 0x02)) return -1;   // link down

    // Wait for the descriptor we're about to reuse to be free.  A slot
    // only ever needs a wait once it has actually been handed to the
    // card (tx_handed > tx_cur); a fresh slot has status 0 and would
    // spin forever.  Sends are synchronous — we wait for each DD
    // below — so by the time a slot is reused the card has written it
    // back.
    int timeout = 200000;
    if (tx_handed > tx_cur) {
        while (timeout-- && !(*st & E1000_TXD_STAT_DD)) { }
        if (!(*st & E1000_TXD_STAT_DD)) return -1;       // ring stalled
    }

    uint8_t* dst = tx_buffers[tx_cur];
    for (int i = 0; i < len; i++) dst[i] = ((const uint8_t*)data)[i];
    d->length = (uint16_t)len;
    d->cso = 0; d->css = 0; d->special = 0;
    d->status = 0;
    d->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS | E1000_TXD_CMD_IFCS;
    // The descriptor fields above must be visible to the card BEFORE
    // the TDT write that hands it over — the card reads them via DMA.
    asm volatile("" ::: "memory");

    // Hand the frame to the card: TDT is the EXCLUSIVE tail — the card
    // transmits descriptors TDH..TDT-1 (QEMU's start_xmit runs while
    // TDH != TDT), so the tail points at the next free slot AFTER the
    // one we just filled.  Writing TDT == TDH transmits nothing.
    e1000_write(E1000_TDT, (uint32_t)((tx_cur + 1) % E1000_TX_DESC_N));
    tx_handed++;

    // Wait for completion (RS makes the card write DD back).
    timeout = 200000;
    while (timeout-- && !(*st & E1000_TXD_STAT_DD)) { }
    if (!(*st & E1000_TXD_STAT_DD)) return -1;           // TX timeout

    tx_cur = (tx_cur + 1) % E1000_TX_DESC_N;
    return len;
}

// ─── Receive ─────────────────────────────────────────────────────
// IRQ mode: pops the software queue (the ISR is the only descriptor-
// ring consumer, so there is no race).  Poll fallback (no IRQ line):
// drains the ring inline, then pops — same code path, driven by the
// caller instead of the ISR.  recv() is also the authoritative ring
// consumer, exactly like rtl8139_recv: cli makes the drain race-free
// against the ISR, and the second drain catches anything that landed
// while an edge was in flight.
int e1000_recv(void* buf, int max_len) {
    if (!e1000_ok || max_len <= 0) return 0;

    asm volatile("cli");
    e1000_drain_rx();
    if (rx_q_tail == rx_q_head && e1000_rx_pending())
        e1000_drain_rx();
    asm volatile("sti");

    if (rx_q_tail == rx_q_head) return 0;

    uint16_t t = rx_q_tail;
    int data_len = rx_queue_len[t];
    if (data_len > max_len) data_len = max_len;
    for (int i = 0; i < data_len; i++)
        ((uint8_t*)buf)[i] = rx_queue[t][i];
    rx_q_tail = (uint16_t)((t + 1) % E1000_RX_QUEUE_PKTS);
    return data_len;
}
