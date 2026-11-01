/* 32-bit division in C, included by each console's libc.c, so that libgcc's hand-written ARC
   version isn't linked: its __udivmodsi4 (behind __udivsi3, __umodsi3, __divsi3, __modsi3)
   writes LP_COUNT a few instructions before a two-instruction zero-overhead loop, as newlib's
   string functions that broke on the console did (llstring.h). The NES games kept breaking
   (scrambled tiles, then a black screen) after those were replaced; QuickNES uses % in 183
   places. Shift-and-subtract from the divisor's top bit (norm: -mnorm); GCC's own loops keep to
   the ARC600's rules. Division by zero gives all ones and the dividend as the remainder. */

static unsigned ll_udivmod(unsigned n, unsigned d, unsigned *rem)
{
    unsigned q = 0;
    int s;
    if (!d) {
        *rem = n;
        return ~0u;
    }
    if (d > n) {
        *rem = n;
        return 0;
    }
    s = __builtin_clz(d) - __builtin_clz(n);
    d <<= s;
    for (; s >= 0; s--, d >>= 1) {
        q <<= 1;
        if (n >= d) n -= d, q |= 1;
    }
    *rem = n;
    return q;
}

unsigned __udivsi3(unsigned n, unsigned d)
{
    unsigned r;
    return ll_udivmod(n, d, &r);
}

unsigned __umodsi3(unsigned n, unsigned d)
{
    unsigned r;
    ll_udivmod(n, d, &r);
    return r;
}

int __divsi3(int a, int b)
{
    unsigned r, q = ll_udivmod(a < 0 ? 0u - (unsigned)a : (unsigned)a, b < 0 ? 0u - (unsigned)b : (unsigned)b, &r);
    return (a < 0) != (b < 0) ? (int)(0u - q) : (int)q;
}

int __modsi3(int a, int b)
{
    unsigned r;
    ll_udivmod(a < 0 ? 0u - (unsigned)a : (unsigned)a, b < 0 ? 0u - (unsigned)b : (unsigned)b, &r);
    return a < 0 ? (int)(0u - r) : (int)r;
}
