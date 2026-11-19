/* Soft-float and libm pieces that libgcc/newlib only have with zero-overhead (lp) loops, which break
   native code on a real Leapster 2: __divsf3, __divdf3 (PicoDrive's timing and YM2612/PSG tables, at
   start-up), __ieee754_sqrt (pow) and __kernel_rem_pio2 (sin of |x| > ~1e6: never reached, the YM2612
   tables take sin of 0..2 pi). Plain C built with -fno-branch-count-reg; division and square root are
   correctly rounded (IEEE round to nearest even), so results equal libgcc's and newlib's. The
   divisions are the SNES console's (snes/console/llfloat.c). */
typedef unsigned u32;
typedef unsigned long long u64;
typedef union { float f; u32 u; } F32;
typedef union { double d; u64 u; } F64;

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

/* fdlibm's e_sqrt.c bit by bit (round to nearest), without its floating-point rounding test */
double __ieee754_sqrt(double x)
{
    F64 v = {x};
    const u32 sign = 0x80000000u;
    u32 r, t1, s1, ix1 = (u32)v.u, q1;
    int ix0 = (int)(v.u >> 32), s0, q, m, t, i;
    if ((ix0 & 0x7ff00000) == 0x7ff00000) return x * x + x;  /* inf, NaN */
    if (ix0 <= 0) {
        if (((ix0 & ~sign) | ix1) == 0) return x;  /* +-0 */
        if (ix0 < 0) { v.u = 0x7ff8000000000000ull; return v.d; }  /* (x - x) / (x - x) */
    }
    m = ix0 >> 20;
    if (m == 0) {  /* subnormal */
        while (ix0 == 0) { m -= 21; ix0 |= ix1 >> 11; ix1 <<= 21; }
        for (i = 0; (ix0 & 0x00100000) == 0; i++) ix0 <<= 1;
        m -= i - 1;
        if (i) ix0 |= ix1 >> (32 - i);
        ix1 <<= i;
    }
    m -= 1023;
    ix0 = (ix0 & 0x000fffff) | 0x00100000;
    if (m & 1) { ix0 += ix0 + ((ix1 & sign) >> 31); ix1 += ix1; }
    m >>= 1;
    ix0 += ix0 + ((ix1 & sign) >> 31);
    ix1 += ix1;
    q = q1 = s0 = s1 = 0;
    r = 0x00200000;
    while (r != 0) {
        t = s0 + r;
        if (t <= ix0) { s0 = t + r; ix0 -= t; q += r; }
        ix0 += ix0 + ((ix1 & sign) >> 31);
        ix1 += ix1;
        r >>= 1;
    }
    r = sign;
    while (r != 0) {
        t1 = s1 + r;
        t = s0;
        if (t < ix0 || (t == ix0 && t1 <= ix1)) {
            s1 = t1 + r;
            if ((t1 & sign) == sign && (s1 & sign) == 0) s0 += 1;
            ix0 -= t;
            if (ix1 < t1) ix0 -= 1;
            ix1 -= t1;
            q1 += r;
        }
        ix0 += ix0 + ((ix1 & sign) >> 31);
        ix1 += ix1;
        r >>= 1;
    }
    if ((ix0 | ix1) != 0) {  /* inexact: round to nearest even */
        if (q1 == 0xffffffffu) { q1 = 0; q += 1; }
        else q1 += q1 & 1;
    }
    ix0 = (q >> 1) + 0x3fe00000;
    ix1 = q1 >> 1;
    if (q & 1) ix1 |= sign;
    ix0 += m << 20;
    v.u = (u64)(u32)ix0 << 32 | ix1;
    return v.d;
}

/* newlib's argument reduction for huge |x| (k_rem_pio2.c: x in 24-bit chunks x[0..nx-1], x = sum x[i]
   2^(e0 - 24i)), only reached by sin/cos of |x| > ~1e6; done in double with pi/2 in three parts. */
int __kernel_rem_pio2(double *x, double *y, int e0, int nx, int prec, const int *ipio2)
{
    double z = 0, sc = 1, n, w;
    int i;
    (void)prec; (void)ipio2;
    for (i = 0; i < e0; i++) sc *= 2.0;
    for (i = 0; i > e0; i--) sc *= 0.5;
    for (i = 0; i < nx; i++, sc *= 1.0 / 16777216) z += x[i] * sc;
    n = z * 6.36619772367581382433e-01;
    n = (double)(long long)(n + 0.5);
    w = ((z - n * 1.57079632673412561417e+00) - n * 6.07710050650619224932e-11) - n * 2.02226624879595063154e-21;
    y[0] = w;
    y[1] = w - y[0];
    return (int)((long long)n & 7);
}
