/* fastfp.c (-DLL_FASTFP, C1): IEEE-754 single-precision soft-float for ARC600 with the mul64 unit, replacing the
 * hot entry points of libgcc's generic fp-bit (unpack/pack per op, shift-add 32x32 multiply). Round to nearest even,
 * subnormals as IEEE: the same results as fp-bit (checked against it on the ARC, port/fptest.c). NaN results:
 * fp-bit's (operand a's NaN, else b's, quiet bit set; 0x7fc00000 for invalid). libgcc's fp-bit.o is still linked
 * with its symbols weakened (core.sh), so the functions not here come from it.
 * Algorithms follow Berkeley SoftFloat 3e (f32_add/sub/mul/div, roundPackToF32). No lp loops (-fno-branch-count-reg). */
typedef unsigned int u32;
typedef int s32;

static inline u32 clz32(u32 a) /* a != 0 */
{
   s32 n;
   __asm__("norm %0,%1" : "=r"(n) : "r"(a)); /* leading sign bits - 1 of a signed value */
   return (s32) a < 0 ? 0 : (u32) n + 1;
}

static inline u32 mulhi(u32 a, u32 b, u32* lo)
{
   u32 h, l;
   __asm__ volatile("mulu64 0,%2,%3\n\tmov %0,r59\n\tmov %1,r58" : /* gcc's MLO is r58 */ "=r"(h), "=r"(l) : "r"(a), "r"(b));
   *lo = l;
   return h;
}

static inline u32 jam(u32 a, u32 dist)
{
   return dist < 31 ? (a >> dist) | ((u32)(a << (-dist & 31)) != 0) : (a != 0);
}

static u32 roundpack(u32 sign, s32 exp, u32 sig) /* sign = 0 or 0x80000000; sig: bit 30 = integer bit, 7 round bits */
{
   u32 rb = sig & 0x7f;
   if (0xfd <= (u32) exp)
   {
      if (exp < 0)
      {
         sig = jam(sig, (u32) -exp);
         exp = 0;
         rb = sig & 0x7f;
      }
      else if (0xfd < exp || 0x80000000u <= sig + 0x40)
         return sign | 0x7f800000;
   }
   sig = (sig + 0x40) >> 7;
   sig &= ~(u32)(!(rb ^ 0x40));
   if (!sig)
      exp = 0;
   return sign + ((u32) exp << 23) + sig;
}

/* mul/div results the way fp-bit packs them: its op pre-rounds the 31-bit quotient/product when the guard bits are
 * exactly half and the rest is non-zero, then a subnormal result is shifted with a sticky bit and rounded again */
static u32 fpbitpack(u32 sign, s32 exp, u32 f, u32 rest)
{
   u32 sh;
   if (exp >= 0)
      return roundpack(sign, exp, f | (rest != 0));
   if ((f & 0xff) == 0x40 && rest)
      f = (f + 0x40) & ~0x7fu;
   sh = (u32) -exp;
   if (sh > 25)
      return sign;
   f = (f >> sh) | ((f & ((1u << sh) - 1)) != 0);
   if ((f & 0x7f) == 0x40)
   {
      if (f & 0x80)
         f += 0x40;
   }
   else
      f += 0x3f;
   return sign + (f >> 7);
}

static u32 normroundpack(u32 sign, s32 exp, u32 sig)
{
   s32 sh = (s32) clz32(sig) - 1;
   exp -= sh;
   if (7 <= sh && (u32) exp < 0xfd)
      return sign + ((u32)(sig ? exp : 0) << 23) + (sig << (sh - 7));
   return roundpack(sign, exp, sig << sh);
}

static u32 propnan(u32 a, u32 b)
{
   if ((a & 0x7fffffff) > 0x7f800000)
      return a | 0x400000;
   if ((b & 0x7fffffff) > 0x7f800000)
      return b | 0x400000;
   return 0x7fc00000;
}

static u32 addmags(u32 a, u32 b)
{
   s32 ea = (a >> 23) & 0xff, eb = (b >> 23) & 0xff, d = ea - eb, ez;
   u32 sa = a & 0x7fffff, sb = b & 0x7fffff, sign = a & 0x80000000, sz;
   if (!d)
   {
      if (!ea)
         return a + sb;
      if (ea == 0xff)
         return (sa | sb) ? propnan(a, b) : a;
      ez = ea;
      sz = 0x01000000 + sa + sb;
      if (!(sz & 1) && ez < 0xfe)
         return sign | (((u32) ez << 23) + (sz >> 1));
      sz <<= 6;
   }
   else
   {
      sa <<= 6;
      sb <<= 6;
      if (d < 0)
      {
         if (eb == 0xff)
            return sb ? propnan(a, b) : sign | 0x7f800000;
         ez = eb;
         sa += ea ? 0x20000000 : sa;
         sa = jam(sa, (u32) -d);
      }
      else
      {
         if (ea == 0xff)
            return sa ? propnan(a, b) : a;
         ez = ea;
         sb += eb ? 0x20000000 : sb;
         sb = jam(sb, (u32) d);
      }
      sz = 0x20000000 + sa + sb;
      if (sz < 0x40000000)
      {
         --ez;
         sz <<= 1;
      }
   }
   return roundpack(sign, ez, sz);
}

static u32 submags(u32 a, u32 b)
{
   s32 ea = (a >> 23) & 0xff, eb = (b >> 23) & 0xff, d = ea - eb, ez, sh;
   u32 sa = a & 0x7fffff, sb = b & 0x7fffff, sign = a & 0x80000000, sx, sy;
   if (!d)
   {
      s32 sd;
      if (ea == 0xff)
         return propnan(a, b);
      sd = (s32)(sa - sb);
      if (!sd)
         return 0;
      if (ea)
         --ea;
      if (sd < 0)
      {
         sign ^= 0x80000000;
         sd = -sd;
      }
      sh = (s32) clz32((u32) sd) - 8;
      ez = ea - sh;
      if (ez < 0)
      {
         sh = ea;
         ez = 0;
      }
      return sign + ((u32) ez << 23) + ((u32) sd << sh);
   }
   sa <<= 7;
   sb <<= 7;
   if (d < 0)
   {
      sign ^= 0x80000000;
      if (eb == 0xff)
         return sb ? propnan(a, b) : sign | 0x7f800000;
      ez = eb - 1;
      sx = sb | 0x40000000;
      sy = sa + (ea ? 0x40000000 : sa);
      d = -d;
   }
   else
   {
      if (ea == 0xff)
         return sa ? propnan(a, b) : a;
      ez = ea - 1;
      sx = sa | 0x40000000;
      sy = sb + (eb ? 0x40000000 : sb);
   }
   return normroundpack(sign, ez, sx - jam(sy, (u32) d));
}

u32 __addsf3(u32 a, u32 b)
{
   return ((a ^ b) & 0x80000000) ? submags(a, b) : addmags(a, b);
}

u32 __subsf3(u32 a, u32 b)
{
   b ^= 0x80000000;
   return ((a ^ b) & 0x80000000) ? submags(a, b) : addmags(a, b);
}

u32 __mulsf3(u32 a, u32 b)
{
   s32 ea = (a >> 23) & 0xff, eb = (b >> 23) & 0xff, ez, sh;
   u32 sa = a & 0x7fffff, sb = b & 0x7fffff, sign = (a ^ b) & 0x80000000, hi, lo;
   if (ea == 0xff)
   {
      if (sa || (eb == 0xff && sb))
         return (propnan(a, b) & 0x7fffffff) | sign; /* fp-bit: the product sign on a NaN too */
      return (eb | sb) ? sign | 0x7f800000 : 0x7fc00000;
   }
   if (eb == 0xff)
   {
      if (sb)
         return (propnan(a, b) & 0x7fffffff) | sign;
      return (ea | sa) ? sign | 0x7f800000 : 0x7fc00000;
   }
   if (!ea)
   {
      if (!sa)
         return sign;
      sh = (s32) clz32(sa) - 8;
      ea = 1 - sh;
      sa <<= sh;
   }
   if (!eb)
   {
      if (!sb)
         return sign;
      sh = (s32) clz32(sb) - 8;
      eb = 1 - sh;
      sb <<= sh;
   }
   ez = ea + eb - 0x7f;
   hi = mulhi((sa | 0x00800000) << 7, (sb | 0x00800000) << 8, &lo);
   if (hi < 0x40000000)
   {
      --ez;
      hi = hi << 1 | lo >> 31;
      lo <<= 1;
   }
   return fpbitpack(sign, ez, hi, lo);
}

u32 __divsf3(u32 a, u32 b)
{
   s32 ea = (a >> 23) & 0xff, eb = (b >> 23) & 0xff, ez, sh, i;
   u32 sa = a & 0x7fffff, sb = b & 0x7fffff, sign = (a ^ b) & 0x80000000, q, r;
   if (ea == 0xff)
   {
      if (sa)
         return propnan(a, b);
      if (eb == 0xff)
         return sb ? propnan(a, b) : 0x7fc00000;
      return sign | 0x7f800000;
   }
   if (eb == 0xff)
      return sb ? propnan(a, b) : sign;
   if (!eb)
   {
      if (!sb)
         return (ea | sa) ? sign | 0x7f800000 : 0x7fc00000;
      sh = (s32) clz32(sb) - 8;
      eb = 1 - sh;
      sb <<= sh;
   }
   if (!ea)
   {
      if (!sa)
         return sign;
      sh = (s32) clz32(sa) - 8;
      ea = 1 - sh;
      sa <<= sh;
   }
   ez = ea - eb + 0x7e;
   sa |= 0x00800000;
   sb |= 0x00800000;
   /* q = floor(sa * 2^k / sb), k = 31 if sa < sb else 30; restoring division, 2 bits per pass */
   i = 31;
   if (sa < sb)
   {
      --ez;
      i = 32;
   }
   r = sa;
   q = 0;
   if (i & 1)
   {
      q = r >= sb;
      if (q)
         r -= sb;
      r <<= 1;
      i--;
   }
   do
   {
      q <<= 2;
      if (r >= sb)
      {
         r -= sb;
         q |= 2;
      }
      r <<= 1;
      if (r >= sb)
      {
         r -= sb;
         q |= 1;
      }
      r <<= 1;
      i -= 2;
   } while (i);
   return fpbitpack(sign, ez, q, r);
}

u32 __floatsisf(s32 a)
{
   u32 sign = (u32) a & 0x80000000, m;
   if (!(a & 0x7fffffff))
      return sign ? 0xcf000000 : 0;
   m = sign ? -(u32) a : (u32) a;
   return normroundpack(sign, 0x9c, m);
}

u32 __floatunsisf(u32 a)
{
   if (!a)
      return 0;
   if (a & 0x80000000)
      return roundpack(0, 0x9d, (a >> 1) | (a & 1));
   return normroundpack(0, 0x9c, a);
}

s32 __fixsfsi(u32 a) /* fp-bit: NaN 0, out of range / inf saturate, else truncate */
{
   s32 e = (s32)((a >> 23) & 0xff) - 127;
   u32 m = (a & 0x7fffff) | 0x800000, t;
   if (e < 0)
      return 0;
   if (e > 30)
   {
      if (e == 128 && (a & 0x7fffff))
         return 0;
      return (a & 0x80000000) ? (s32) 0x80000000 : 0x7fffffff;
   }
   t = e >= 23 ? m << (e - 23) : m >> (23 - e);
   return (a & 0x80000000) ? -(s32) t : (s32) t;
}

/* compares: <0, 0, >0 like fp-bit; unordered: gt/ge -1, lt/le/eq/ne/cmp 1 */
static inline s32 fcmp(u32 a, u32 b, s32 unord)
{
   if ((a & 0x7fffffff) > 0x7f800000 || (b & 0x7fffffff) > 0x7f800000)
      return unord;
   if (!((a | b) & 0x7fffffff) || a == b)
      return 0;
   if ((s32)(a & b) < 0) /* both negative */
      return (s32) a < (s32) b ? 1 : -1;
   return (s32) a < (s32) b ? -1 : 1;
}
s32 __cmpsf2(u32 a, u32 b) { return fcmp(a, b, 1); }
s32 __eqsf2(u32 a, u32 b) { return fcmp(a, b, 1); }
s32 __nesf2(u32 a, u32 b) { return fcmp(a, b, 1); }
s32 __gtsf2(u32 a, u32 b) { return fcmp(a, b, -1); }
s32 __gesf2(u32 a, u32 b) { return fcmp(a, b, -1); }
s32 __ltsf2(u32 a, u32 b) { return fcmp(a, b, 1); }
s32 __lesf2(u32 a, u32 b) { return fcmp(a, b, 1); }
s32 __unordsf2(u32 a, u32 b) { return (a & 0x7fffffff) > 0x7f800000 || (b & 0x7fffffff) > 0x7f800000; }

/* __divdf3 (C1b): dp-bit's divide (61-bit restoring division, NGARDS 8: implicit bit 60, pre-round when the guard
   byte is exactly half and the rest non-zero, subnormal results shifted with a sticky bit and rounded again), without
   its lp loop (no zero-overhead loops on the console). Bit-identical to dp-bit: tmp/fptest.sh. */
typedef unsigned long long u64;
static u64 dpropnan(u64 a, u64 b)
{
   if ((a & 0x7fffffffffffffffull) > 0x7ff0000000000000ull)
      return a | 0x8000000000000ull;
   if ((b & 0x7fffffffffffffffull) > 0x7ff0000000000000ull)
      return b | 0x8000000000000ull;
   return 0x7ff8000000000000ull;
}
u64 __divdf3(u64 a, u64 b)
{
   s32 ea = (s32)(a >> 52) & 0x7ff, eb = (s32)(b >> 52) & 0x7ff, ne, i;
   u64 ma = a & 0xfffffffffffffull, mb = b & 0xfffffffffffffull, sign = (a ^ b) & 0x8000000000000000ull, n, d, q, bit;
   if (ea == 0x7ff)
   {
      if (ma)
         return dpropnan(a, b);
      if (eb == 0x7ff)
         return mb ? dpropnan(a, b) : 0x7ff8000000000000ull;
      return sign | 0x7ff0000000000000ull;
   }
   if (eb == 0x7ff)
      return mb ? dpropnan(a, b) : sign;
   if (!eb && !mb)
      return (ea | ma) ? sign | 0x7ff0000000000000ull : 0x7ff8000000000000ull;
   if (!ea && !ma)
      return sign;
   /* a power of two divisor (DSP-1's / 65536.0): the quotient is exact, only the exponent moves */
   if (!mb && ea && (u32)(ea - eb + 1022) < 2046)
      return sign | ((u64)(ea - eb + 1023) << 52) | ma;
   /* unpack as dp-bit: fraction << 8 with the implicit bit at 60, normal_exp */
   if (ea)
      n = (ma << 8) | (1ull << 60), ea -= 1023;
   else
      for (n = ma << 8, ea = -1022; n < (1ull << 60); n <<= 1, ea--)
         ;
   if (eb)
      d = (mb << 8) | (1ull << 60), eb -= 1023;
   else
      for (d = mb << 8, eb = -1022; d < (1ull << 60); d <<= 1, eb--)
         ;
   ne = ea - eb;
   if (n < d)
   {
      n <<= 1;
      ne--;
   }
   q = 0;
   for (bit = 1ull << 60, i = 61; i; i--, bit >>= 1)
   {
      if (n >= d)
      {
         q |= bit;
         n -= d;
      }
      n <<= 1;
   }
   if ((q & 0xff) == 0x80 && !(q & 0x100) && n)
      q = (q + 0x80) & ~0xffull;
   if (ne < -1022)
   {
      u32 sh = (u32)(-1022 - ne);
      if (sh > 56)
         return sign;
      q = (q >> sh) | ((q & ((1ull << sh) - 1)) != 0);
      if ((q & 0xff) == 0x80)
      {
         if (q & 0x100)
            q += 0x80;
      }
      else
         q += 0x7f;
      return sign + (q >> 8); /* q >= 1 << 60 here makes the exponent 1 */
   }
   if (ne > 1023)
      return sign | 0x7ff0000000000000ull;
   if ((q & 0xff) == 0x80)
   {
      if (q & 0x100)
         q += 0x80;
   }
   else
      q += 0x7f;
   if (q >= (1ull << 61))
   {
      q >>= 1;
      ne++;
   }
   return sign | ((u64)(ne + 1023) << 52) | ((q >> 8) & 0xfffffffffffffull);
}
