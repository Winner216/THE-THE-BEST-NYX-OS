// include/dns.h — Minimal DNS resolver (UDP to configurable DNS server)
// Default: QEMU DNS proxy (10.0.2.3:53). Override at runtime with:
//   netstack_set_dns(ip)  or shell:  net config dns <ip>
#ifndef DNS_H
#define DNS_H

#include <stdint.h>

#define DNS_SERVER_PORT 53

// Resolve a hostname to an IP address (in LE wire format, e.g. 10.0.2.2 → 0x0202000A).
// Returns the IP on success, or 0 on failure. Blocks until resolution completes or times out.
// The gateway must already be resolved (netstack_resolve_gateway) before calling this.
uint32_t dns_resolve(const char* hostname);

// Get a human-readable string describing why the last dns_resolve() call failed.
// Returns "" (empty string) if the last call succeeded.
const char* dns_get_last_error(void);

// Called from netstack when a UDP packet is received.
void dns_handle_udp(const uint8_t* payload, int len, uint16_t dst_port);

#endif
