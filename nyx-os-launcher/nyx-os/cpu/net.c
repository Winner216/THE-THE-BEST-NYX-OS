// cpu/net.c  –  PCI bus scan to detect network adapters
// Reads the PCI configuration space via the standard mechanism
// (0xCF8 = config address, 0xCFC = config data, both 32-bit).
// No driver code — just detection and identification.

#include <net.h>
#include <io.h>
#include <stdint.h>

NetAdapter net_adapters[NET_MAX_ADAPTERS];
int        net_adapter_count = 0;

// ─── PCI config-space read ────────────────────────────────────────
// Builds the 32-bit address register:
//   bit 31    = enable bit (must be 1)
//   bits 23-16 = bus number   (0-255)
//   bits 15-11 = device/slot  (0-31)
//   bits 10-8  = function     (0-7)
//   bits 7-2   = register     (DWord offset 0-63)
//   bits 1-0   = 0 (must be zero)
static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t reg) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func <<  8)
                  | ((uint32_t)(reg & 0xFC));
    outl(0xCF8, addr);
    return inl(0xCFC);
}

// ─── Known vendor IDs → name ──────────────────────────────────────
typedef struct { uint16_t id; const char* name; } VendorEntry;
static const VendorEntry known_vendors[] = {
    { 0x8086, "Intel"         },
    { 0x10EC, "Realtek"       },
    { 0x14E4, "Broadcom"      },
    { 0x1969, "Qualcomm Ath." },
    { 0x168C, "Atheros"       },
    { 0x10DE, "NVIDIA"        },
    { 0x1022, "AMD"           },
    { 0x1AF4, "VirtIO/QEMU"   },
    { 0x1234, "Bochs/QEMU"    },
    { 0x8139, "Realtek (old)" },
    { 0x100B, "NS/3Com"       },
    { 0x1000, "Symbios/LSI"   },
    { 0x0000, 0               },
};

// ─── Known network device IDs → name + type ───────────────────────
typedef struct {
    uint16_t vendor; uint16_t device;
    const char* name; const char* type; int supported;
} DeviceEntry;
static const DeviceEntry known_devices[] = {
    // Intel
    { 0x8086,0x100E,"82540EM GbE",      "Ethernet",1 },
    { 0x8086,0x100F,"82545EM GbE",      "Ethernet",1 },
    { 0x8086,0x10D3,"82574L GbE",       "Ethernet",1 },
    { 0x8086,0x1502,"82579LM GbE",      "Ethernet",1 },
    { 0x8086,0x1503,"82579V GbE",       "Ethernet",1 },
    { 0x8086,0x153A,"I217-LM GbE",      "Ethernet",1 },
    { 0x8086,0x1539,"I211 GbE",         "Ethernet",1 },
    { 0x8086,0x2526,"Wireless-AC 9260", "WiFi",    0 },
    { 0x8086,0x24FD,"Wireless 8265",    "WiFi",    0 },
    { 0x8086,0x24F3,"Wireless 8260",    "WiFi",    0 },
    { 0x8086,0x08B1,"Wireless 7260",    "WiFi",    0 },
    { 0x8086,0x095A,"Wireless 6005",    "WiFi",    0 },
    // Realtek
    { 0x10EC,0x8139,"RTL8139",          "Ethernet",1 },
    { 0x10EC,0x8168,"RTL8168/8111",     "Ethernet",1 },
    { 0x10EC,0x8169,"RTL8169 GbE",      "Ethernet",1 },
    { 0x10EC,0x8125,"RTL8125 2.5GbE",   "Ethernet",1 },
    { 0x10EC,0xB822,"RTL8822BE WiFi",   "WiFi",    0 },
    { 0x10EC,0xC821,"RTL8821CE WiFi",   "WiFi",    0 },
    // Broadcom
    { 0x14E4,0x1639,"BCM5709 GbE",      "Ethernet",1 },
    { 0x14E4,0x4311,"BCM4311 WiFi",     "WiFi",    0 },
    { 0x14E4,0x4727,"BCM4313 WiFi",     "WiFi",    0 },
    { 0x14E4,0x4365,"BCM43142 WiFi",    "WiFi",    0 },
    // Atheros/Qualcomm
    { 0x168C,0x002A,"AR928X WiFi",      "WiFi",    0 },
    { 0x168C,0x0042,"QCA9377 WiFi",     "WiFi",    0 },
    { 0x1969,0x1083,"AR8151 GbE",       "Ethernet",1 },
    // VirtIO (QEMU/KVM)
    { 0x1AF4,0x1000,"VirtIO Net",       "Ethernet",1 },
    { 0x1AF4,0x1041,"VirtIO Net v1",    "Ethernet",1 },
    // AMD
    { 0x1022,0x1458,"XGMAC 10GbE",     "Ethernet",1 },
    { 0x1022,0x7800,"PCNET Fast III",   "Ethernet",1 },
    // 3Com
    { 0x10B7,0x9200,"3c905C-TX",        "Ethernet",1 },
    // E1000e (common in VMs)
    { 0x8086,0x10EA,"82577LM GbE",      "Ethernet",1 },
    { 0x8086,0x10EB,"82577LC GbE",      "Ethernet",1 },
    { 0x0000,0x0000,0,0,0 },
};

// ─── Subclass → human readable type ──────────────────────────────
static const char* subclass_name(uint8_t sub) {
    switch(sub) {
        case 0x00: return "Ethernet";
        case 0x01: return "Token Ring";
        case 0x02: return "FDDI";
        case 0x03: return "ATM";
        case 0x04: return "ISDN";
        case 0x05: return "WorldFip";
        case 0x20: return "InfiniBand";
        case 0x80: return "WiFi/Other";
        default:   return "Network";
    }
}

// ─── Small helpers (no libc) ─────────────────────────────────────
static void net_strcpy(char *d, const char *s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}
static void net_hex16(char *buf, uint16_t v) {
    /* writes exactly 4 hex digits */
    const char *h = "0123456789ABCDEF";
    buf[0] = h[(v >> 12) & 0xF];
    buf[1] = h[(v >>  8) & 0xF];
    buf[2] = h[(v >>  4) & 0xF];
    buf[3] = h[(v >>  0) & 0xF];
    buf[4] = '\0';
}

// ─── Main scan ───────────────────────────────────────────────────
// Walks every bus/slot, following the header-type multi-function bit
// so devices with multiple functions (e.g. combo NICs) are all seen.
// For each network-class device we capture the PCI IRQ line (0x3C)
// and BAR0 so a driver can bind to it without re-scanning.
void net_scan_pci(void) {
    net_adapter_count = 0;

    for (int bus = 0; bus <= 7 && net_adapter_count < NET_MAX_ADAPTERS; bus++) {
        for (int slot = 0; slot < 32 && net_adapter_count < NET_MAX_ADAPTERS; slot++) {
            /* Function 0 tells us whether this is a multi-function device
             * (header type bit 7 in config register 0x0C). */
            uint32_t id0 = pci_read32(bus, slot, 0, 0x00);
            uint16_t v0 = (uint16_t)(id0 & 0xFFFF);
            if (v0 == 0xFFFF || v0 == 0x0000) continue;  /* no device */
            int max_func = (pci_read32(bus, slot, 0, 0x0C) & 0x00800000) ? 8 : 1;

            for (int func = 0; func < max_func; func++) {
                uint32_t id_reg = pci_read32(bus, slot, func, 0x00);
                uint16_t vendor = (uint16_t)(id_reg & 0xFFFF);
                uint16_t device = (uint16_t)(id_reg >> 16);

                if (vendor == 0xFFFF || vendor == 0x0000) continue; /* no func */

                /* Read class/subclass/prog-IF/rev from register 0x08 */
                uint32_t class_reg = pci_read32(bus, slot, func, 0x08);
                uint8_t  class_code = (uint8_t)((class_reg >> 24) & 0xFF);
                uint8_t  subclass   = (uint8_t)((class_reg >> 16) & 0xFF);

                /* PCI class 0x02 = Network controller */
                if (class_code != 0x02) continue;
                if (net_adapter_count >= NET_MAX_ADAPTERS) break;

                NetAdapter *a = &net_adapters[net_adapter_count];
                a->vendor_id  = vendor;
                a->device_id  = device;
                a->bus        = (uint8_t)bus;
                a->slot       = (uint8_t)slot;
                a->func       = (uint8_t)func;
                a->class_code = class_code;
                a->subclass   = subclass;

                /* PCI interrupt line (0x3C) + BAR0 (0x10) so the driver
                 * can bind without re-scanning. */
                a->irq_line   = (uint8_t)(pci_read32(bus, slot, func, 0x3C) & 0xFF);
                a->bar0       = pci_read32(bus, slot, func, 0x10);
                a->bar0_is_io = (a->bar0 & 1) ? 1 : 0;

            /* Look up vendor name */
            a->vendor_name[0] = '\0';
            for (int v = 0; known_vendors[v].name; v++) {
                if (known_vendors[v].id == vendor) {
                    net_strcpy(a->vendor_name, known_vendors[v].name, 24);
                    break;
                }
            }
            if (!a->vendor_name[0]) {
                /* Unknown vendor: show hex ID */
                char hx[8] = "0x";
                net_hex16(hx + 2, vendor);
                net_strcpy(a->vendor_name, hx, 24);
            }

            /* Look up specific device */
            a->device_name[0] = '\0';
            a->supported = 0;
            net_strcpy(a->type_name, subclass_name(subclass), 16);
            for (int d = 0; known_devices[d].name; d++) {
                if (known_devices[d].vendor == vendor &&
                    known_devices[d].device == device) {
                    net_strcpy(a->device_name, known_devices[d].name,  32);
                    net_strcpy(a->type_name,   known_devices[d].type,  16);
                    a->supported = known_devices[d].supported;
                    break;
                }
            }
                if (!a->device_name[0]) {
                    /* Unknown device: show hex ID */
                    char hx[8] = "0x";
                    net_hex16(hx + 2, device);
                    net_strcpy(a->device_name, hx, 32);
                }

                net_adapter_count++;
            }
        }
    }
}

int net_best_adapter(void) {
    for (int i = 0; i < net_adapter_count; i++)
        if (net_adapters[i].supported) return i;
    return -1;
}
