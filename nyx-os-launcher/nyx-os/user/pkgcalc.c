// user/pkgcalc.c — Nyx App Store package: "Calculator".
//
// A real number-theory toy for ring 3: it computes, from scratch and
// with plain 32-bit integer arithmetic only (no libc, no libgcc, so
// no 64-bit division — every op below is a u32 add/mul/div),
//
//    * pi    to 100 decimal digits (Machin: pi/4 = 4 atan(1/5) - atan(1/239))
//    * sqrt2 to  60 decimal digits (digit-by-digit integer square root)
//    * the 168 primes below 1000 (count, sum, first 20, largest)
//    * Fibonacci numbers F(1)..F(46) (last one still fits u32)
//    * factorials 1!..12! (12! = 479001600 is the last that fits u32)
//
// Big integers are plain base-10 digit arrays (most-significant first)
// with only three primitives: divide-by-small, multiply-by-small and
// subtract — so the whole file compiles freestanding with -nostdlib.
//
// Install:   pkg install pkgcalc
// Run:       pkg run pkgcalc     → output on the serial console (COM1)
#include "userlib.h"

// ─── Big integer: base-10 digit array, most-significant first ────
#define MAXD 240
struct big {
    unsigned char d[MAXD];   // digits, d[0] = most significant
    int n;                   // significant digits (>= 1; value 0 => n=1,d[0]=0)
};

static void b_set(struct big* a, unsigned v) {
    char tmp[16];
    int p = 0;
    if (v == 0) { a->n = 1; a->d[0] = 0; return; }
    while (v) { tmp[p++] = (char)(v % 10); v /= 10; }
    for (int i = 0; i < p; i++) a->d[i] = (unsigned char)tmp[p - 1 - i];
    a->n = p;
}

static void b_trim(struct big* a) {
    int i = 0;
    while (i < a->n - 1 && a->d[i] == 0) i++;   // keep >= 1 digit
    if (i > 0) {
        for (int j = 0; j < a->n - i; j++) a->d[j] = a->d[j + i];
        a->n -= i;
    }
}

// a = a * m   (m small: 2..100)
static void b_mul_small(struct big* a, unsigned m) {
    unsigned carry = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        unsigned prod = (unsigned)a->d[i] * m + carry;
        a->d[i] = (unsigned char)(prod % 10);
        carry = prod / 10;
    }
    while (carry) {                       // carry grows in the front
        for (int i = a->n; i > 0; i--) a->d[i] = a->d[i - 1];
        a->d[0] = (unsigned char)(carry % 10);
        carry /= 10;
        a->n++;
    }
}

// a = a + s   (s small: 0..9)
static void b_add_small(struct big* a, unsigned s) {
    int i = a->n - 1;
    unsigned carry = s;
    while (carry && i >= 0) {
        unsigned v = (unsigned)a->d[i] + carry;
        a->d[i] = (unsigned char)(v % 10);
        carry = v / 10;
        i--;
    }
    if (carry) {
        for (int j = a->n; j > 0; j--) a->d[j] = a->d[j - 1];
        a->d[0] = (unsigned char)carry;
        a->n++;
    }
}

// a = a / d   (floor, d small >= 2).  Runs most-significant first.
static void b_div_small(struct big* a, unsigned d) {
    unsigned rem = 0;
    for (int i = 0; i < a->n; i++) {
        unsigned cur = rem * 10 + (unsigned)a->d[i];
        a->d[i] = (unsigned char)(cur / d);
        rem = cur % d;
    }
    b_trim(a);
}

// 0 if a==b, <0 if a<b, >0 if a>b
static int b_cmp(const struct big* a, const struct big* b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = 0; i < a->n; i++) {
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    }
    return 0;
}

// c = a + b
static void b_add(struct big* c, const struct big* a, const struct big* b) {
    int an = a->n, bn = b->n;
    int cn = (an > bn ? an : bn) + 1;
    unsigned carry = 0;
    for (int i = 0; i < cn; i++) {
        int ai = an - 1 - i, bi = bn - 1 - i;
        unsigned v = (ai >= 0 ? (unsigned)a->d[ai] : 0)
                   + (bi >= 0 ? (unsigned)b->d[bi] : 0) + carry;
        c->d[cn - 1 - i] = (unsigned char)(v % 10);
        carry = v / 10;
    }
    c->n = cn;
    b_trim(c);
}

// c = a - b   (requires a >= b)
static void b_sub(struct big* c, const struct big* a, const struct big* b) {
    int borrow = 0;
    int bi = b->n - 1;
    for (int i = a->n - 1; i >= 0; i--) {
        int av = a->d[i];
        int bv = (bi >= 0) ? b->d[bi--] : 0;
        int v = av - bv - borrow;
        if (v < 0) { v += 10; borrow = 1; } else { borrow = 0; }
        c->d[i] = (unsigned char)v;
    }
    c->n = a->n;
    b_trim(c);
}

// Print scaled value v (v represents v / 10^frac_digits) with the point.
static void b_print_scaled(const struct big* v, int frac_digits, char* out, int* olen) {
    int int_digits = v->n - frac_digits;
    if (int_digits <= 0) {
        out[(*olen)++] = '0';
        out[(*olen)++] = '.';
        for (int i = 0; i < -int_digits && (*olen) < 250; i++) out[(*olen)++] = '0';
        for (int i = 0; i < v->n && (*olen) < 250; i++) out[(*olen)++] = (char)('0' + v->d[i]);
    } else {
        int i = 0;
        for (; i < int_digits && (*olen) < 250; i++) out[(*olen)++] = (char)('0' + v->d[i]);
        out[(*olen)++] = '.';
        for (; i < v->n && (*olen) < 250; i++) out[(*olen)++] = (char)('0' + v->d[i]);
    }
}

// ─── pi = 4*atan(1/5) - atan(1/239) ──────────────────────────────
// scaled = 10^K * atan(1/x), K = frac_digits + guard digits.
static void atan_inv(unsigned x, int frac_digits, struct big* out) {
    int guard = 10;
    int K = frac_digits + guard;
    // out = 0
    b_set(out, 0);
    // T = 10^K
    struct big T;
    b_set(&T, 1);
    for (int i = 0; i < K; i++) b_mul_small(&T, 10);
    int sign = 1;
    for (unsigned k = 0; k < 400; k++) {
        b_div_small(&T, x);                 // T = 10^K / x^(2k+1)
        struct big term;
        term = T;                            // struct copy of digit array
        unsigned odd = 2 * k + 1;
        b_div_small(&term, odd);             // term = 10^K / (x^(2k+1) (2k+1))
        if (term.n == 1 && term.d[0] == 0) break;   // below 10^-K
        if (sign > 0) {
            struct big nb;
            b_add(&nb, out, &term);          // out += term
            *out = nb;
        } else {
            // out = out - term (out > term always: alternating series)
            struct big nb;
            b_sub(&nb, out, &term);
            *out = nb;
        }
        sign = -sign;
        b_div_small(&T, x);                 // next: divide again by x
    }
    (void)K;
}

// ─── sqrt(2) to frac_digits decimals via integer digit root ──────
// Computes floor(sqrt(2*10^(2f))) digit by digit: at each step find
// the largest d with (20p+d)*d <= R, then R = (R - (20p+d)d) * 100.
static void sqrt2_digits(int frac_digits, char* out, int* olen) {
    struct big R;                      // remainder (starts at 2)
    b_set(&R, 2);
    struct big p;                      // root so far
    b_set(&p, 0);
    int steps = 1 + frac_digits;       // integer digit + one per pair of 00
    for (int s = 0; s < steps; s++) {
        int best = 0;
        for (int d = 9; d >= 0; d--) {
            struct big q;
            q = p;
            b_mul_small(&q, 20);       // q = 20*p
            b_add_small(&q, (unsigned)d);
            struct big t;
            t = q;
            b_mul_small(&t, (unsigned)d);   // t = (20p+d)*d
            if (b_cmp(&t, &R) <= 0) { best = d; break; }
        }
        struct big q;
        q = p;
        b_mul_small(&q, 20);
        b_add_small(&q, (unsigned)best);
        struct big t;
        t = q;
        b_mul_small(&t, (unsigned)best);
        struct big nr;
        b_sub(&nr, &R, &t);
        if (s < steps - 1) b_mul_small(&nr, 100);   // next digit pair
        R = nr;
        // p = p*10 + best
        b_mul_small(&p, 10);
        b_add_small(&p, (unsigned)best);
        if (s == 0) out[(*olen)++] = (char)('0' + best);          // integer part
        else {
            if (s == 1) out[(*olen)++] = '.';
            out[(*olen)++] = (char)('0' + best);
        }
    }
}

// ─── Small numeric sections ──────────────────────────────────────
static void primes_section(void) {
    u_print("[PKGCALC] primes < 1000: ...\n");
    char nb[16];
    int count = 0;
    unsigned sum = 0;
    int first20[20];
    int largest = 0;
    for (int v = 2; v < 1000; v++) {
        int prime = 1;
        for (int d = 2; d * d <= v; d++)
            if (v % d == 0) { prime = 0; break; }
        if (prime) {
            if (count < 20) first20[count] = v;
            largest = v;
            sum += (unsigned)v;
            count++;
        }
    }
    u_print("    first 20: ");
    for (int i = 0; i < 20; i++) {
        u_dec((unsigned)first20[i], nb);
        u_print(nb);
        u_print(i < 19 ? ", " : "\n");
    }
    u_print("    count = ");
    u_dec((unsigned)count, nb); u_print(nb);
    u_print(", sum = ");
    u_dec(sum, nb); u_print(nb);
    u_print(", largest = ");
    u_dec((unsigned)largest, nb); u_print(nb);
    u_print("\n");
}

static void fib_section(void) {
    u_print("[PKGCALC] Fibonacci F(1)..F(46):\n");
    char nb[16];
    unsigned f0 = 0, f1 = 1;
    for (int i = 1; i <= 46; i++) {
        unsigned f = f0 + f1;
        if (i == 46 && f < f1) break;      // overflow guard (not reached)
        f0 = f1; f1 = f;
        if (i % 5 == 0 || i >= 44) {
            u_print("    F(");
            u_dec((unsigned)i, nb); u_print(nb);
            u_print(") = ");
            u_dec(f, nb); u_print(nb);
            u_print("\n");
        }
    }
}

static void fact_section(void) {
    u_print("[PKGCALC] factorials:\n");
    char nb[16];
    unsigned f = 1;
    for (int i = 1; i <= 12; i++) {
        f *= (unsigned)i;
        u_print("    ");
        u_dec((unsigned)i, nb); u_print(nb);
        u_print("! = ");
        u_dec(f, nb); u_print(nb);
        u_print("\n");
    }
    u_print("    13! overflows u32 (6,227,020,800) — stop here.\n");
}

void _start(void) {
    u_print("[PKGCALC] ring-3 calculator starting (pid=");
    char nb[16];
    u_dec((unsigned)sys_getpid(), nb);
    u_print(nb);
    u_print(")\n");

    // ── pi ──
    u_print("[PKGCALC] pi to 100 digits (Machin 4 atan(1/5) - atan(1/239)):\n    ");
    {
        struct big a5, a239, r;
        atan_inv(5, 100, &a5);
        atan_inv(239, 100, &a239);
        b_mul_small(&a5, 4);                 // 4 * atan(1/5) scaled
        b_sub(&r, &a5, &a239);               // pi/4 scaled by 10^(100+guard)
        b_mul_small(&r, 4);                  // pi scaled (Machin gives pi/4)
        // guard digits: drop the last `guard` digits (they are noise)
        // by integer-dividing by 10^10... simplest: divide by 10 ten times.
        for (int i = 0; i < 10; i++) b_div_small(&r, 10);
        char out[280];
        int ol = 0;
        b_print_scaled(&r, 100, out, &ol);   // now exactly scaled by 10^100
        out[ol] = 0;
        u_print(out);
        u_print("\n");
    }

    // ── sqrt2 ──
    u_print("[PKGCALC] sqrt(2) to 60 digits (digit-by-digit root):\n    ");
    {
        char out[160];
        int ol = 0;
        sqrt2_digits(60, out, &ol);
        out[ol] = 0;
        u_print(out);
        u_print("\n");
    }

    primes_section();
    fib_section();
    fact_section();

    u_print("[PKGCALC] all done — exiting.\n");
    sys_exit();
}
