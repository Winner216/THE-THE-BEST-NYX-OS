/* cpu/divdi3.c – 64-bit division helpers for 32-bit freestanding kernel.
   GCC emits __divdi3 / __udivdi3 / __moddi3 for 64-bit arithmetic on i386.
   These simple implementations cover the calculator's needs. */

typedef unsigned long long uint64;
typedef          long long  int64;

static uint64 udiv64(uint64 a, uint64 b) {
    if (b == 0) return 0;
    uint64 q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) { r -= b; q |= ((uint64)1 << i); }
    }
    return q;
}

int64 __divdi3(int64 a, int64 b) {
    int neg = 0;
    uint64 ua = (a < 0) ? (neg ^= 1, (uint64)(-a)) : (uint64)a;
    uint64 ub = (b < 0) ? (neg ^= 1, (uint64)(-b)) : (uint64)b;
    uint64 q  = udiv64(ua, ub);
    return neg ? -(int64)q : (int64)q;
}

uint64 __udivdi3(uint64 a, uint64 b) { return udiv64(a, b); }

int64 __moddi3(int64 a, int64 b) {
    return a - __divdi3(a, b) * b;
}

uint64 __umoddi3(uint64 a, uint64 b) { return a - udiv64(a, b) * b; }
