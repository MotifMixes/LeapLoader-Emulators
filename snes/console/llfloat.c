/* Soft-float and libm pieces that libgcc/newlib only have with zero-overhead (lp) loops, which break
   native code on a real Leapster 2: __divsf3, __divdf3 (DSP-1, Super FX/C4 helpers, tanf), sqrtf
   (C4), __kernel_rem_pio2f (sinf/cosf/tanf of |x| > ~200: never reached by the games' angles) and
   memchr (S9xUpdateScreen). Plain C built with -fno-branch-count-reg; division and sqrtf are
   correctly rounded (IEEE round to nearest even), so results equal libgcc's. With -DLL_FASTFP the
   divisions come from fastfp.c instead (bit-identical to libgcc's fp-bit/dp-bit, as the bench runs). */
typedef unsigned u32;
typedef unsigned long long u64;
typedef union { float f; u32 u; } F32;
typedef union { double d; u64 u; } F64;

#ifndef LL_FASTFP
float __divsf3(float a, float b)
{
    F32 x = {a}, y = {b}, r;
    u32 ua = x.u, ub = y.u, sign = (ua ^ ub) & 0x80000000u, ma = ua & 0x7FFFFF, mb = ub & 0x7FFFFF, q = 0, rem;
    int ea = ua >> 23 & 255, eb = ub >> 23 & 255, e, i, sticky;
    if (ea == 255) {
        if (ma) { r.u = ua | 0x400000; return r.f; }
        if (eb == 255) { r.u = mb ? ub | 0x400000 : 0x7FC00000; return r.f; }
        r.u = sign | 0x7F800000; return r.f;
    }
    if (eb == 255) {
        if (mb) { r.u = ub | 0x400000; return r.f; }
        r.u = sign; return r.f;
    }
    if (!eb && !mb) { r.u = (!ea && !ma) ? 0x7FC00000 : sign | 0x7F800000; return r.f; }
    if (!ea && !ma) { r.u = sign; return r.f; }
    if (!ea) { while (!(ma & 0x800000)) ma <<= 1, ea--; ea++; } else ma |= 0x800000;
    if (!eb) { while (!(mb & 0x800000)) mb <<= 1, eb--; eb++; } else mb |= 0x800000;
    e = ea - eb + 127;
    if (ma < mb) ma <<= 1, e--;
    rem = ma;
    for (i = 0; i < 26; i++) {  /* 1 + 23 bits, guard and round */
        q <<= 1;
        if (rem >= mb) rem -= mb, q |= 1;
        rem <<= 1;
    }
    sticky = rem != 0;
    if (e >= 255) { r.u = sign | 0x7F800000; return r.f; }
    if (e <= 0) {  /* denormal: shift to exponent 1 */
        int sh = 1 - e;
        if (sh > 27) { r.u = sign; return r.f; }
        sticky |= (q & ((1u << sh) - 1)) != 0;
        q >>= sh;
        e = 1;
    }
    {
        u32 m = q >> 2;
        if ((q & 2) && ((q & 1) || sticky || (m & 1))) m++;
        r.u = sign | (((u32)(e - 1) << 23) + m);
        if ((r.u & 0x7FFFFFFF) >= 0x7F800000) r.u = sign | 0x7F800000;
    }
    return r.f;
}

double __divdf3(double a, double b)
{
    F64 x = {a}, y = {b}, r;
    const u64 H = 1ull << 52, M = H - 1, QN = 1ull << 51, INF = 0x7FFull << 52;
    u64 ua = x.u, ub = y.u, sign = (ua ^ ub) & (1ull << 63), ma = ua & M, mb = ub & M, q = 0, rem;
    int ea = ua >> 52 & 0x7FF, eb = ub >> 52 & 0x7FF, e, i, sticky;
    if (ea == 0x7FF) {
        if (ma) { r.u = ua | QN; return r.d; }
        if (eb == 0x7FF) { r.u = mb ? ub | QN : INF | QN; return r.d; }
        r.u = sign | INF; return r.d;
    }
    if (eb == 0x7FF) {
        if (mb) { r.u = ub | QN; return r.d; }
        r.u = sign; return r.d;
    }
    if (!eb && !mb) { r.u = (!ea && !ma) ? INF | QN : sign | INF; return r.d; }
    if (!ea && !ma) { r.u = sign; return r.d; }
    if (!ea) { while (!(ma & H)) ma <<= 1, ea--; ea++; } else ma |= H;
    if (!eb) { while (!(mb & H)) mb <<= 1, eb--; eb++; } else mb |= H;
    e = ea - eb + 1023;
    if (ma < mb) ma <<= 1, e--;
    rem = ma;
    for (i = 0; i < 55; i++) {  /* 1 + 52 bits, guard and round */
        q <<= 1;
        if (rem >= mb) rem -= mb, q |= 1;
        rem <<= 1;
    }
    sticky = rem != 0;
    if (e >= 0x7FF) { r.u = sign | INF; return r.d; }
    if (e <= 0) {
        int sh = 1 - e;
        if (sh > 56) { r.u = sign; return r.d; }
        sticky |= (q & ((1ull << sh) - 1)) != 0;
        q >>= sh;
        e = 1;
    }
    {
        u64 m = q >> 2;
        if ((q & 2) && ((q & 1) || sticky || (m & 1))) m++;
        r.u = sign | (((u64)(e - 1) << 52) + m);
        if ((r.u & ~(1ull << 63)) >= INF) r.u = sign | INF;
    }
    return r.d;
}
#endif

/* fdlibm's e_sqrtf.c bit by bit (round to nearest), without its divisions */
float sqrtf(float x)
{
    F32 v = {x};
    int ix = (int)v.u, m, i, s, t, r, q;
    if ((ix & 0x7F800000) == 0x7F800000) return x * x + x;
    if (ix <= 0) {
        if (!(ix & 0x7FFFFFFF)) return x;
        v.u = 0x7FC00000; return v.f;
    }
    m = ix >> 23;
    if (m == 0) {
        for (i = 0; !(ix & 0x00800000); i++) ix <<= 1;
        m -= i - 1;
    }
    m -= 127;
    ix = (ix & 0x007FFFFF) | 0x00800000;
    if (m & 1) ix += ix;
    m >>= 1;
    ix += ix;
    q = s = 0;
    r = 0x01000000;
    while (r != 0) {
        t = s + r;
        if (t <= ix) { s = t + r; ix -= t; q += r; }
        ix += ix;
        r >>= 1;
    }
    if (ix != 0) q += q & 1;
    v.u = (u32)((q >> 1) + 0x3F000000 + (m << 23));
    return v.f;
}

/* newlib's float argument reduction for huge |x| (ef_rem_pio2.c: x in 8-bit chunks tx[0..nx-1],
   x = sum tx[i] 2^(e0 - 8i)); done in double with pi/2 in three parts: right while |x| < ~2^30. */
int __kernel_rem_pio2f(float *tx, float *y, int e0, int nx, int prec, const int *ipio2)
{
    double z = 0, sc = 1, n, w;
    int i, k;
    (void)prec; (void)ipio2;
    for (i = 0; i < e0; i++) sc *= 2.0;
    for (i = 0; i > e0; i--) sc *= 0.5;
    for (i = 0; i < nx; i++, sc *= 1.0 / 256) z += tx[i] * sc;
    n = z * 6.36619772367581382433e-01;
    n = (double)(long long)(n + 0.5);
    w = ((z - n * 1.57079632673412561417e+00) - n * 6.07710050650619224932e-11) - n * 2.02226624879595063154e-21;
    y[0] = (float)w;
    y[1] = (float)(w - y[0]);
    k = (int)((long long)n & 7);
    return k;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memchr(const void *s, int c, unsigned n)
{
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c) return (void *)p;
    return 0;
}
