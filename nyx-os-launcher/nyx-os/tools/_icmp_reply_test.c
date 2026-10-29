// tools/_icmp_reply_test.c — host harness for the kernel's ICMP echo
// reply path. Compiles cpu/netstack.c (statics exposed) with stubs for
// the NIC/DNS/DHCP/timer deps, feeds a synthetic echo request into
// handle_ip(), and validates the generated echo reply. Run with:
//   gcc -O2 -Iinclude -o /tmp/icmp_rt tools/_icmp_reply_test.c cpu/netstack.c
// (stubs below satisfy the linker; netstack.c's own functions come from
//  the .c being linked directly).
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// ── stubs for netstack.c's external deps ─────────────────────────
static uint8_t captured[2048];
static int captured_len = -1;

int nic_send(const void* data, int len) {
    captured_len = len;
    for (int i = 0; i < len; i++) captured[i] = ((const uint8_t*)data)[i];
    return len;
}
int  nic_recv(void* buf, int max_len) { (void)buf; (void)max_len; return 0; }
void nic_get_mac(uint8_t mac[6]) { static const uint8_t m[6] = {0x52,0x54,0x00,0x12,0x34,0x56}; for (int i=0;i<6;i++) mac[i]=m[i]; }
void dns_handle_udp(const uint8_t* p, int l, uint16_t d) { (void)p;(void)l;(void)d; }
void dhcp_handle_udp(const uint8_t* p, int l, uint16_t d) { (void)p;(void)l;(void)d; }
uint64_t timer_us(void) { return 1234567; }
volatile uint32_t tick = 1000;   // netstack's TCP timeouts reference this

// netstack.c exposes these (static defined away via the Makefile-less
// direct compile? no — we link netstack.c as a translation unit, so its
// statics are private). Instead #define static away is impossible here
// without including the file; we rely on the public API only:
//   netstack_set_mac / netstack_set_ip / netstack_poll? handle_ip is
//   static. So include the file with static neutralized:
#define static
#include "../cpu/netstack.c"
#undef static

// ── checksum helper — must mirror the kernel's net_checksum exactly:
// LE 16-bit word folding over the bytes as stored in memory. A valid
// header checksums to 0 (the kernel validates `!= 0`).
static uint16_t le_checksum(const uint8_t* p, int len) {
    uint32_t sum = 0;
    while (len > 1) { sum += (uint16_t)(p[0] | (p[1] << 8)); p += 2; len -= 2; }
    if (len) sum += (uint16_t)p[0];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

int main(void) {
    uint8_t our_mac[6] = {0x52,0x54,0x00,0x12,0x34,0x56};
    uint8_t gway_mac[6] = {0x52,0x54,0x00,0x12,0x34,0x02};
    netstack_set_mac(our_mac);
    netstack_set_ip(0x0F02000A, 0x0202000A);   // 10.0.2.15, gw 10.0.2.2

    // Build a synthetic echo request: 10.0.2.2 -> 10.0.2.15
    uint8_t f[14 + 20 + 40];
    int o = 0;
    for (int i = 0; i < 6; i++) f[o++] = our_mac[i];       // dst = us
    for (int i = 0; i < 6; i++) f[o++] = gway_mac[i];      // src = gw
    f[o++] = 0x08; f[o++] = 0x00;                          // ethertype IP
    f[o++] = 0x45; f[o++] = 0;                             // ver/ihl, dscp
    f[o++] = 0x00; f[o++] = 20 + 40;                       // total_len
    f[o++] = 0x00; f[o++] = 0x01;                          // id
    f[o++] = 0x40; f[o++] = 0x00;                          // DF
    f[o++] = 64; f[o++] = 1;                               // ttl, ICMP
    f[o++] = 0x00; f[o++] = 0x00;                          // ip csum (below)
    f[o++] = 0x0A; f[o++] = 0x00; f[o++] = 0x02; f[o++] = 0x02;  // src 10.0.2.2
    f[o++] = 0x0A; f[o++] = 0x00; f[o++] = 0x02; f[o++] = 0x0F;  // dst 10.0.2.15
    // ICMP echo request: type 8, code 0, csum 0, id 0x1234, seq 0x0056, 32B payload
    f[o++] = 8; f[o++] = 0; f[o++] = 0; f[o++] = 0;
    f[o++] = 0x12; f[o++] = 0x34; f[o++] = 0x00; f[o++] = 0x56;
    for (int i = 0; i < 32; i++) f[o++] = (uint8_t)i;
    // fix checksums (LE store, same as the kernel's send paths)
    f[24] = 0; f[25] = 0;
    uint16_t ipc = le_checksum(f + 14, 20);
    f[24] = (uint8_t)ipc; f[25] = (uint8_t)(ipc >> 8);
    f[36] = 0; f[37] = 0;                        // ICMP checksum field @ +2
    uint16_t icc = le_checksum(f + 34, 40);
    f[36] = (uint8_t)icc; f[37] = (uint8_t)(icc >> 8);

    handle_ip(f, o);

    if (captured_len < 0) { printf("FAIL: no reply sent\n"); return 1; }
    int fail = 0;
    // eth: dst = requester MAC, src = our MAC, type IP
    if (memcmp(captured, gway_mac, 6) != 0) { printf("FAIL: reply dst MAC\n"); fail = 1; }
    if (memcmp(captured + 6, our_mac, 6) != 0) { printf("FAIL: reply src MAC\n"); fail = 1; }
    if (captured[12] != 0x08 || captured[13] != 0x00) { printf("FAIL: reply ethertype\n"); fail = 1; }
    // IP: src=10.0.2.15 dst=10.0.2.2, protocol ICMP, checksum valid
    if (captured[23] != 1) { printf("FAIL: reply proto\n"); fail = 1; }
    if (captured[26]!=0x0A||captured[27]!=0x00||captured[28]!=0x02||captured[29]!=0x0F) { printf("FAIL: reply ip src\n"); fail=1; }
    if (captured[30]!=0x0A||captured[31]!=0x00||captured[32]!=0x02||captured[33]!=0x02) { printf("FAIL: reply ip dst\n"); fail=1; }
    if (le_checksum(captured+14, 20) != 0) { printf("FAIL: reply ip csum\n"); fail = 1; }
    // ICMP: type 0, id/seq echoed, payload echoed, checksum valid
    if (captured[34] != 0) { printf("FAIL: reply icmp type\n"); fail = 1; }
    if (captured[38]!=0x12||captured[39]!=0x34||captured[40]!=0x00||captured[41]!=0x56) { printf("FAIL: reply id/seq\n"); fail=1; }
    for (int i = 0; i < 32; i++)
        if (captured[42+i] != (uint8_t)i) { printf("FAIL: reply payload[%d]\n", i); fail = 1; break; }
    if (le_checksum(captured+34, 40) != 0) { printf("FAIL: reply icmp csum\n"); fail = 1; }

    if (!fail) { printf("PASS: echo request answered with valid reply (%d bytes)\n", captured_len); return 0; }
    return 1;
}
