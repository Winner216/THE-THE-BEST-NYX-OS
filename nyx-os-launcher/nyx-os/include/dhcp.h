// include/dhcp.h — Minimal DHCP client (DISCOVER / OFFER)
#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>

// DHCP ports
#define DHCP_SERVER_PORT  67
#define DHCP_CLIENT_PORT  68

// DHCP message types (option 53)
#define DHCP_DISCOVER     1
#define DHCP_OFFER        2
#define DHCP_REQUEST      3
#define DHCP_ACK          5

// DHCP option codes
#define DHCP_OPT_SUBNET_MASK   1
#define DHCP_OPT_ROUTER        3
#define DHCP_OPT_DNS           6
#define DHCP_OPT_HOSTNAME     12
#define DHCP_OPT_REQ_IP       50
#define DHCP_OPT_LEASE_TIME   51
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_PARAM_REQ    55
#define DHCP_OPT_CLIENT_ID    61
#define DHCP_OPT_END         255

// DHCP magic cookie in LE wire format.
// DHCP wire bytes: 99, 130, 83, 99 (0x63, 0x82, 0x53, 0x63)
// Read as little-endian uint32: 0x63538263
#define DHCP_MAGIC_COOKIE  0x63538263

// Result of a DHCP transaction
typedef struct {
    uint32_t yiaddr;     // Offered IP address (LE wire format)
    uint32_t subnet;     // Subnet mask (LE wire format)
    uint32_t gateway;    // Router/gateway (LE wire format)
    uint32_t dns;        // DNS server (LE wire format)
    uint32_t server_id;  // DHCP server identifier (LE wire format)
    uint32_t lease;      // Lease time in seconds
} DhcpResult;

// Perform DHCP DISCOVER / OFFER exchange.
// Blocks until an OFFER is received or timeout (several seconds).
// Returns 1 on success and fills result with the offered configuration.
// Returns 0 on failure (timeout or no OFFER).
int dhcp_discover(DhcpResult* result);

// Called from netstack when a UDP packet arrives destined for port 68.
void dhcp_handle_udp(const uint8_t* payload, int len, uint16_t dst_port);

#endif
