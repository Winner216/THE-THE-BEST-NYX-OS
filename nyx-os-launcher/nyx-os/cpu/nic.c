// cpu/nic.c — generic NIC layer (see include/nic.h)
//
// nic_init() probes the PCI bus through each driver's own scan: the
// Intel e1000 driver first (Phase A target, QEMU's `-device e1000`
// default), then the RTL8139 driver as a fallback for the older
// `-nic user,model=rtl8139` setups.  Whichever card is present binds,
// and every subsequent nic_* call dispatches to that driver through
// a small function-pointer table.

#include <nic.h>
#include <e1000.h>
#include <rtl8139.h>
#include <stdint.h>

static int (*drv_send)(const void*, int)      = 0;
static int (*drv_recv)(void*, int)            = 0;
static void (*drv_get_mac)(uint8_t[6])        = 0;
static int (*drv_irq_line)(void)              = 0;
static void (*drv_stats)(uint32_t*, uint32_t*, uint32_t*, uint32_t*) = 0;
static const char* drv_name = "none";
static int drv_up = 0;

// Adapters: the drivers have different init signatures (rtl8139 takes
// an optional I/O base; e1000 takes nothing), so wrap them uniformly.
static int nic_init_e1000(void)   { return e1000_init(); }
static int nic_init_rtl8139(void) { return rtl8139_init(0); }

int nic_init(void) {
    if (drv_up) return 1;

    if (nic_init_e1000()) {
        drv_send     = e1000_send;
        drv_recv     = e1000_recv;
        drv_get_mac  = e1000_get_mac;
        drv_irq_line = e1000_irq_line;
        drv_stats    = e1000_stats;
        drv_name     = "e1000";
        drv_up       = 1;
        return 1;
    }
    if (nic_init_rtl8139()) {
        drv_send     = rtl8139_send;
        drv_recv     = rtl8139_recv;
        drv_get_mac  = rtl8139_get_mac;
        drv_irq_line = rtl8139_irq_line;
        drv_stats    = rtl8139_stats;
        drv_name     = "rtl8139";
        drv_up       = 1;
        return 1;
    }
    return 0;
}

int nic_is_up(void) { return drv_up; }

const char* nic_driver_name(void) { return drv_name; }

int nic_send(const void* data, int len) {
    return drv_send ? drv_send(data, len) : -1;
}

int nic_recv(void* buf, int max_len) {
    return drv_recv ? drv_recv(buf, max_len) : 0;
}

void nic_get_mac(uint8_t mac[6]) {
    if (drv_get_mac) { drv_get_mac(mac); return; }
    for (int i = 0; i < 6; i++) mac[i] = 0;
}

int nic_irq_line(void) {
    return drv_irq_line ? drv_irq_line() : 0;
}

void nic_stats(uint32_t* rx_irqs, uint32_t* rx_pkts,
               uint32_t* rx_drops, uint32_t* tx_done) {
    if (drv_stats) drv_stats(rx_irqs, rx_pkts, rx_drops, tx_done);
}
