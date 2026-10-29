// include/net.h  –  PCI network adapter detection
#ifndef NET_H
#define NET_H
#include <stdint.h>

#define NET_MAX_ADAPTERS  8

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  bus, slot, func;
    uint8_t  class_code;    /* 02 = network */
    uint8_t  subclass;      /* 00=Ethernet 02=Token Ring 03=FDDI 80=WiFi */
    uint8_t  irq_line;      /* PCI interrupt line (config 0x3C); 0 = none */
    uint32_t bar0;          /* BAR0 raw value (I/O port base or MMIO addr) */
    uint8_t  bar0_is_io;    /* 1 if BAR0 is I/O space, 0 if MMIO */
    char     vendor_name[24];
    char     device_name[32];
    char     type_name[16];  /* "Ethernet", "WiFi", "Unknown" */
    int      supported;      /* 1 if we have a stub driver concept */
} NetAdapter;

extern NetAdapter net_adapters[NET_MAX_ADAPTERS];
extern int        net_adapter_count;

void net_scan_pci(void);

/* Returns the first adapter index with supported=1, or -1 */
int  net_best_adapter(void);

#endif
