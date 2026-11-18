/* fxjit.c (snes-gsu, -DLL_FXJIT): a GSU (Super FX) -> ARC600 translator. Included at the end of fxinst.c.

   fx_run(~0) (every uncounted GSU run) goes to fxj_runloop(): GSU code in ROM banks is translated, on first
   use, into ARC code that keeps R0-R14 and the flag values in ARC registers; anything the translator doesn't
   handle (stop, cmode, rpix, ljmp, code in RAM banks, odd pipeline states) is run by the C interpreter
   (FX_STEP), which hands back to translated code at the next "clean" point. Translated code does exactly
   what the C ops do (same RAM writes in the same order, same register and flag values as far as anything
   can observe them: the upper 16 bits of a GSU register and of vSign/vZero are never observed by the C
   core, see NOTES.md); -DFXJ_CHECK (fxemu.c) runs every GSU run both ways and compares.

   State: a translated block starts at a clean point: no ALT1/ALT2/B, Sreg = Dreg = R0, PIPE == the byte
   at R15-1 (so the point is fully described by the address R15-1 and the program bank). The translator
   simulates the interpreter statically (R15, PIPE, prefixes), so branch delay slots, operands fetched
   from a branch target, "to r15" etc. come out as the C core runs them.

   Registers in translated code: r4..r18 = GSU R0..R14, r19 vSign, r20 vZero, r21 vCarry (0/1),
   r23 pvRamBank, r24 pvRomBank, r25 &GSU, fp = vLastRamAdr; vOverflow stays in memory (GSU.vOverflow).
   Temps r0-r3, r22 (r3 = dynamic R15 / R15 operand). mul64 results are read from r58 (MMID): on the
   Leapster r57 is the MAC accumulator and mul64 leaves it alone (as the compiled C code does). No gp, no lp loops, no misaligned accesses
   (ldw/stw only at even addresses). Code is written with st.di and made visible with an ld.di + IC_IVIC.
   Flag values are only computed when a later instruction can read them (static scan of the ROM code;
   anything reaching a dynamic jump, stop or the interpreter counts as reading all flags). */
#include "fxjit_enc.h"
#include "fxjit_tab.h"
#include <stddef.h>
#include "snes9x.h"

#ifdef LL_CONSOLE
/* LeapLoader console: the app, its heap (a 4 MB ROM buffer) and these tables share the 8 MB load
   area, which the bench's sizes overflow. Star Fox / Yoshi's Island translate ~100 KB of code
   (no flush at 768 KB in the bench); when a table fills, the cache is flushed and translated again
   (the same emulation, only slower). */
#ifndef FXJ_CODE_WORDS
#define FXJ_CODE_WORDS (64 * 1024)             /* 256 KB of ARC code */
#endif
#define FXJ_PAGES 256
#define FXJ_HBITS 12
#define FXJ_MAXSTUBS 2048
#else
#ifndef FXJ_CODE_WORDS
#define FXJ_CODE_WORDS (192 * 1024)            /* 768 KB of ARC code */
#endif
#define FXJ_PAGES 768                          /* 256-address pages of the address -> code map */
#define FXJ_HBITS 12                           /* native dispatch cache: 4096 entries */
#define FXJ_MAXSTUBS 8192
#endif
#ifdef FXJ_LOCK1
#define FXJ_MAXOPS 1                           /* lockstep debugging: one op per block, no links */
#else
#define FXJ_MAXOPS 160                         /* GSU ops per block (soft limit) */
#endif
#define FXJ_ROOM 2048                          /* words that must be free before a block is started */

static fxu32 fxj_code[FXJ_CODE_WORDS] __attribute__((aligned(32)));
static fxu32 *fxj_p, *fxj_base;
static fxu32 *fxj_page[FXJ_PAGES][256];
static fxu32 **fxj_map[128][256];               /* [bank][addr >> 8] -> page */
static int fxj_npages;
static fxu32 fxj_hash[2 << FXJ_HBITS];          /* key, host */
typedef struct { fxu32 key; fxu32 *stub, *src; int cc; } FxjStub;
static FxjStub fxj_stubs[FXJ_MAXSTUBS];
static int fxj_nstubs;
static int fxj_ready;
static void *fxj_plot[4];
void *fxj_plotfn;
volatile fxu32 fxj_info[2];
typedef int (*fxj_enter_t)(void *);
static fxj_enter_t fxj_enter;
static fxu32 *fxj_exit, *fxj_disp;
unsigned FxjStat[8], FxjRawOp[1024];
int fxj_off;                                    /* FXJ_CHECK: 1 = run the interpreter */                            /* blocks, exits raw/lookup/stub, steps, flushes, words */
enum { X_RAW = 1, X_LOOKUP = 2, X_STUB = 3 };

/* ---------------------------------------------------------------- registers */
#define T0 0
#define T1 1
#define T2 2
#define T3 3
#define T4 22
#define G(k) (4 + (k))
#define RS 19
#define RZ 20
#define RC 21
#define RRAM 23
#define RROM 24
#define RGSU 25
#define RLAST 27
#define RSP 28
#define LIMM ARC_LIMM
#define FS 1
#define FZ 2
#define FC 4
#define FO 8
#define FALL 15
#define GOFF(f) ((int)offsetof(FxRegs_s, f))

/* ---------------------------------------------------------------- emitter */
static inline void fxj_w(fxu32 *p, fxu32 v)
{
    v = (v << 16) | (v >> 16);
    __asm__ volatile("st.di %0,[%1]" : : "r"(v), "r"(p) : "memory");
}
static inline void E(fxu32 i) { fxj_w(fxj_p++, i); }
static inline void EL(fxu32 i, fxu32 l) { E(i); fxj_w(fxj_p++, l); }
static void I3(int sub, int a, int b, int c) { E(enc_rrr(4, sub, a, b, c, 0)); }
static void I3F(int sub, int a, int b, int c) { E(enc_rrr(4, sub, a, b, c, 1)); }
static void IU(int sub, int a, int b, int u) { E(enc_rru(4, sub, a, b, u, 0)); }
static void IUF(int sub, int a, int b, int u) { E(enc_rru(4, sub, a, b, u, 1)); }
static void IK(int sub, int a, int b, fxu32 k)
{
    if (k < 64) IU(sub, a, b, k);
    else if (a == b && (int)k >= -2048 && (int)k < 2048) E(enc_rs12(4, sub, b, (int)k, 0));
    else EL(enc_rrr(4, sub, a, b, LIMM, 0), k);
}
static void MOV(int d, int s) { if (d != s) E(enc_mov(d, s)); }
static void MOVK(int d, fxu32 k)
{
    if ((int)k >= -2048 && (int)k < 2048) E(enc_movi(d, (int)k));
    else EL(enc_mov(d, LIMM), k);
}
static void SOP(int s2, int b, int c) { E(enc_sop(4, s2, b, c, 0)); }
static void SOPF(int s2, int b, int c) { E(enc_sop(4, s2, b, c, 1)); }
static void XU(int sub, int a, int b, int u) { E(enc_rru(5, sub, a, b, u, 0)); }
static void X3(int sub, int a, int b, int c) { E(enc_rrr(5, sub, a, b, c, 0)); }
static void LDO(int a, int b, int off, int zz) { E(enc_ld(a, b, off, zz, 0, 0)); }
static void STO(int c, int b, int off, int zz) { E(enc_st(c, b, off, zz, 0)); }
static void LDA(int a, const void *p) { EL(enc_ld(a, LIMM, 0, Z_W, 0, 0), (fxu32)p); }
static void STA(int c, const void *p, int zz) { EL(enc_st(c, LIMM, 0, zz, 0), (fxu32)p); }
static void LDG(int a, int off)
{
    if (off < 256) LDO(a, RGSU, off, Z_W);
    else LDA(a, (char *)&GSU + off);
}
static void STG(int c, int off, int zz)
{
    if (off < 256) STO(c, RGSU, off, zz);
    else STA(c, (char *)&GSU + off, zz);
}
static int OFF(fxu32 *to, fxu32 *from) { return (int)((char *)to - (char *)from); }
static void BCC(fxu32 *to, int cc) { E(cc ? enc_bcc(OFF(to, fxj_p), cc) : enc_b(OFF(to, fxj_p))); }
static fxu32 *FWD(void) { return fxj_p++; }
static void PBCC(fxu32 *at, fxu32 *to, int cc) { fxj_w(at, cc ? enc_bcc(OFF(to, at), cc) : enc_b(OFF(to, at))); }
/* bbit0/bbit1/brcc placeholders: patched with PBR */
static int fxj_fail;
static void PBRC(fxu32 *at, fxu32 *to, int b, int c, int isu6, int cc)
{
    int o = OFF(to, at);
    if (o < -256 || o > 254) fxj_fail = 1;      /* BRcc/BBIT reach only +-256 bytes */
    fxj_w(at, enc_br(b, c, isu6, cc, o));
}
static void JL(const void *fn) { EL(enc_jl(LIMM), (fxu32)fn); }

static void fxj_sync(void)
{
    /* ARC600 has no sync: an uncached read of the last code word waits for the st.di writes to land,
       then IC_IVIC empties the code cache */
    fxu32 t;
    __asm__ volatile("ld.di %0,[%1]\n\tsr 0,[0x10]\n\tnop\n\tnop\n\tnop" : "=r"(t) : "r"(fxj_p - 1) : "memory");
    (void)t;
}

/* ---------------------------------------------------------------- address map */
static fxu32 *fxj_find(int bank, unsigned a)
{
    fxu32 **pg = fxj_map[bank & 127][(a >> 8) & 255];
    return pg ? pg[a & 255] : 0;
}
static int fxj_reg(int bank, unsigned a, fxu32 *h)
{
    fxu32 ***m = &fxj_map[bank & 127][(a >> 8) & 255];
    if (!*m) {
        int i;
        if (fxj_npages >= FXJ_PAGES) return 0;
        *m = fxj_page[fxj_npages++];
        for (i = 0; i < 256; i++) (*m)[i] = 0;
    }
    (*m)[a & 255] = h;
    return 1;
}
static void fxj_hput(fxu32 key, fxu32 *h)
{
    fxu32 *e = &fxj_hash[(key & ((1 << FXJ_HBITS) - 1)) * 2];
    e[0] = key;
    e[1] = (fxu32)h;
}

/* ---------------------------------------------------------------- stubs and helpers made at start-up */
static void fxj_gen_plot(int mode)
{
    static const int off8[8] = { 0, 1, 0x10, 0x11, 0x20, 0x21, 0x30, 0x31 };
    int np = mode == 0 ? 2 : mode == 3 ? 8 : 4, k;
    fxu32 *ret, *f1, *f2, *f3, *f4, *f5, *f6, *f7;
    fxu32 *pend[8]; int npend = 0;
    fxj_plot[mode] = fxj_p;
    SOP(S_EXTB, T0, G(1));                      /* x */
    IU(A_ADD, G(1), G(1), 1);                   /* R1++ */
    SOP(S_EXTB, T1, G(2));                      /* y */
    LDA(T2, &GSU.vScreenHeight);
    E(enc_rrr(4, A_CMP, 0, T1, T2, 1));
    pend[npend++] = FWD();                      /* bhs (no borrow: y >= height) -> ret */
    LDG(T2, GOFF(vPlotOptionReg));
    LDG(T3, GOFF(vColorReg));
    f1 = f2 = f3 = f4 = f5 = f6 = f7 = 0;
    if (mode != 3) {
        f1 = FWD();                             /* bbit0 por,1 -> L1 */
        I3(A_XOR, T4, T0, T1);
        f2 = FWD();                             /* bbit0 x^y,0 -> L1 */
        XU(X_LSR, T3, T3, 4);
        PBRC(f1, fxj_p, T2, 1, 1, BB_0);
        PBRC(f2, fxj_p, T4, 0, 1, BB_0);
        SOP(S_EXTB, T3, T3);
        f3 = FWD();                             /* bbit1 por,0 -> L2 */
        IUF(A_AND, LIMM, T3, 15);
        pend[npend++] = FWD();                  /* beq ret */
        PBRC(f3, fxj_p, T2, 0, 1, BB_1);
    } else {
        SOP(S_EXTB, T3, T3);
        f4 = FWD();                             /* bbit1 por,4 -> L8 */
        f5 = FWD();                             /* bbit1 por,0 -> L2 */
        IUF(A_AND, LIMM, T3, 15);
        pend[npend++] = FWD();                  /* beq ret */
        f6 = FWD();                             /* b L2 */
        PBRC(f4, fxj_p, T2, 4, 1, BB_1);
        f7 = FWD();                             /* bbit1 por,0 -> L2 */
        E(enc_rrr(4, A_TST, 0, T3, T3, 1));
        pend[npend++] = FWD();                  /* beq ret */
        PBRC(f5, fxj_p, T2, 0, 1, BB_1);
        PBCC(f6, fxj_p, 0);
        PBRC(f7, fxj_p, T2, 0, 1, BB_1);
    }
    /* a = apvScreen[y >> 3] + x[x >> 3] + ((y & 7) << 1) */
    XU(X_LSR, T2, T1, 3);
    EL(enc_rrr(4, A_ADD2, T2, LIMM, T2, 0), (fxu32)&GSU.apvScreen[0]);
    LDO(T2, T2, 0, Z_W);
    XU(X_LSR, T4, T0, 3);
    EL(enc_rrr(4, A_ADD2, T4, LIMM, T4, 0), (fxu32)&GSU.x[0]);
    LDO(T4, T4, 0, Z_W);
    I3(A_ADD, T2, T2, T4);
    IU(A_AND, T4, T1, 7);
    I3(A_ADD1, T2, T2, T4);
    IU(A_AND, T0, T0, 7);
    MOVK(T1, 128);
    X3(X_LSR, T1, T1, T0);                      /* v = 128 >> (x & 7) */
    for (k = 0; k < np; k++) {
        LDO(T0, T2, off8[k], Z_B);
        I3(A_OR, T4, T0, T1);
        I3(A_BIC, T0, T0, T1);
        E(enc_rru(4, A_BTST, 0, T3, k, 1));
        E(enc_cc(4, A_MOV, T0, T4, 0, CC_NE, 0));
        STO(T0, T2, off8[k], Z_B);
    }
    ret = fxj_p;
    E(enc_j(31));
    /* patch the early exits */
    PBCC(pend[0], ret, CC_CC);
    for (k = 1; k < npend; k++) PBCC(pend[k], ret, CC_EQ);
}

/* n plots at R1, R1+1, ... (R2 fixed) with the plot function's per-pixel rules, a group of 8 pixels at a
   time: each pixel's colour only depends on its x parity (dither) and the transparency rule, every pixel of a
   group is written once, and a span longer than 256 writes the same values again (so n is capped at 256);
   the group's bit-plane bytes end up exactly as after the single plots. */
static const uint8 fxj_poff[8] = { 0, 1, 0x10, 0x11, 0x20, 0x21, 0x30, 0x31 };
static void fxj_span(uint32 r1, uint32 r2, uint32 n)
{
    uint32 y = r2 & 0xff, mode = GSU.vMode, np = mode == 0 ? 2 : mode == 3 ? 8 : 4;
    uint32 por = GSU.vPlotOptionReg, col = GSU.vColorReg, c0, c1, s0, s1, k, x, pm0;
    uint8 *rowb;
    if (y >= GSU.vScreenHeight)
        return;
    c0 = (uint8)col;
    c1 = (mode != 3 && (por & 2)) ? (uint8)(col >> 4) : c0;
    if (mode != 3) {
        s0 = !(por & 1) && !(c0 & 0xf);
        s1 = !(por & 1) && !(c1 & 0xf);
    } else {
        s0 = s1 = !(por & 0x10) ? (!(por & 1) && !(c0 & 0xf)) : (!(por & 1) && !c0);
    }
    if (s0 && s1)
        return;
    if (n > 256)
        n = 256;
    pm0 = (y & 1) ? 0x55 : 0xaa;               /* bits (128 >> (x & 7)) of the pixels with (x ^ y) & 1 == 0 */
    rowb = GSU.apvScreen[y >> 3] + ((y & 7) << 1);
    x = r1 & 0xff;
    while (n) {
        uint32 f = x & 7, cnt = 8 - f, mall, M0, M1, M;
        if (cnt > n)
            cnt = n;
        mall = ((1u << cnt) - 1) << (8 - f - cnt);
        M0 = s0 ? 0 : mall & pm0;
        M1 = s1 ? 0 : mall & ~pm0;
        M = M0 | M1;
        if (M) {
            uint8 *a = rowb + GSU.x[x >> 3];
            for (k = 0; k < np; k++) {
                uint32 bits = (((c0 >> k) & 1) ? M0 : 0) | (((c1 >> k) & 1) ? M1 : 0);
                a[fxj_poff[k]] = (a[fxj_poff[k]] & ~M) | bits;
            }
        }
        x = (x + cnt) & 0xff;
        n -= cnt;
    }
}
static fxu32 *fxj_spanstub;

static void fxj_init(void)
{
    int i;
    fxj_p = fxj_code;
    /* entry: int fxj_enter(void *code) */
    fxj_enter = (fxj_enter_t)fxj_p;
    E(enc_rs12(4, A_SUB, RSP, 64, 0));
    for (i = 13; i <= 25; i++) STO(i, RSP, (i - 13) * 4, Z_W);
    STO(27, RSP, 52, Z_W);
    STO(31, RSP, 56, Z_W);
    MOVK(RGSU, (fxu32)&GSU);
    for (i = 0; i < 15; i++) LDO(G(i), RGSU, GOFF(avReg) + 4 * i, Z_W);
    LDG(RS, GOFF(vSign));
    LDG(RZ, GOFF(vZero));
    LDG(RC, GOFF(vCarry));
    LDG(RRAM, GOFF(pvRamBank));
    LDG(RROM, GOFF(pvRomBank));
    LDG(RLAST, GOFF(vLastRamAdr));
    E(enc_j(T0));
    /* exit: r0 = reason, r1/r2 = info */
    fxj_exit = fxj_p;
    for (i = 0; i < 15; i++) STO(G(i), RGSU, GOFF(avReg) + 4 * i, Z_W);
    STG(RS, GOFF(vSign), Z_W);
    STG(RZ, GOFF(vZero), Z_W);
    STG(RC, GOFF(vCarry), Z_W);
    STG(RLAST, GOFF(vLastRamAdr), Z_W);
    STA(T1, (void *)&fxj_info[0], Z_W);
    STA(T2, (void *)&fxj_info[1], Z_W);
    for (i = 13; i <= 25; i++) LDO(i, RSP, (i - 13) * 4, Z_W);
    LDO(27, RSP, 52, Z_W);
    LDO(31, RSP, 56, Z_W);
    E(enc_rs12(4, A_ADD, RSP, 64, 0));
    E(enc_j(31));
    /* dispatch: r1 = target address, r2 = bank << 16 */
    {
        fxu32 *f, *miss;
        fxj_disp = fxj_p;
        SOP(S_EXTW, T1, T1);
        I3(A_OR, T3, T1, T2);
        IU(A_BMSK, T0, T1, FXJ_HBITS - 1);
        EL(enc_rrr(4, A_ADD3, T0, LIMM, T0, 0), (fxu32)fxj_hash);
        LDO(T2, T0, 0, Z_W);
        f = FWD();
        LDO(T0, T0, 4, Z_W);
        E(enc_j(T0));
        miss = fxj_p;
        MOV(T1, T3);
        MOVK(T0, X_LOOKUP);
        BCC(fxj_exit, 0);
        PBRC(f, miss, T2, T3, 0, BR_NE);
    }
    for (i = 0; i < 4; i++) fxj_gen_plot(i);
    /* C call from translated code: r0-r2 = args; keeps r4-r12 (GSU R0-R8) and blink */
    fxj_spanstub = fxj_p;
    E(enc_rs12(4, A_SUB, RSP, 40, 0));
    for (i = 4; i <= 12; i++) STO(i, RSP, (i - 4) * 4, Z_W);
    STO(31, RSP, 36, Z_W);
    JL((void *)fxj_span);
    for (i = 4; i <= 12; i++) LDO(i, RSP, (i - 4) * 4, Z_W);
    LDO(31, RSP, 36, Z_W);
    E(enc_rs12(4, A_ADD, RSP, 40, 0));
    E(enc_j(31));
    fxj_base = fxj_p;
    for (i = 0; i < (2 << FXJ_HBITS); i += 2) { fxj_hash[i] = ~0u; fxj_hash[i + 1] = 0; }
    fxj_sync();
    fxj_ready = 1;
}

static void fxj_flush(void)
{
    int b, i;
    for (b = 0; b < 128; b++)
        for (i = 0; i < 256; i++) fxj_map[b][i] = 0;
    for (i = 0; i < (2 << FXJ_HBITS); i += 2) { fxj_hash[i] = ~0u; fxj_hash[i + 1] = 0; }
    fxj_npages = 0;
    fxj_nstubs = 0;
    fxj_p = fxj_base;
    FxjStat[5]++;
}

/* ---------------------------------------------------------------- translation */
typedef struct { fxu32 r15; int pipe, alt, b, sreg, dreg, dyn; } St;
static uint8 *fxj_bk;                           /* code bank */
static int fxj_counted;                         /* counted runs (WinterGold games): see fxj_reserve */
static int fxj_rsv;                             /* emit op reservations (counted runs; FXJ_LOCK: exact lockstep) */
static int fxj_bank;
#define MEM(a) (fxj_bk[(uint16)(a)])

static int fxj_clean(const St *s) { return !s->alt && !s->b && !s->sreg && !s->dreg && !s->dyn; }
static int fxj_normal(const St *s) { return s->pipe == MEM(s->r15 - 1); }
static void CLR(St *s) { s->alt = s->b = s->sreg = s->dreg = 0; }
static int fxj_idx(const St *s) { return s->pipe | (s->alt << 8); }
static int fxj_kind(const St *s) { return fxj_tab[s->pipe | (s->alt << 8)][0]; }

/* flags read / written per kind (static scan); writes to DREG */
static unsigned char kfr[K_NKINDS], kfw[K_NKINDS], kdw[K_NKINDS], ksimple[K_NKINDS];
static void fxj_kinds(void)
{
    static const unsigned char sz[] = { K_AND_R, K_AND_I, K_BIC_R, K_BIC_I, K_OR_R, K_OR_I, K_XOR_R, K_XOR_I, K_NOT,
        K_MULT_R, K_MULT_I, K_UMULT_R, K_UMULT_I, K_SEX, K_SWAP, K_LOB, K_HIB, K_INC_R, K_DEC_R, K_LOOP };
    static const unsigned char szc[] = { K_LSR, K_ASR, K_DIV2, K_FMULT, K_LMULT, K_ROL, K_ROR };
    static const unsigned char all[] = { K_ADD_R, K_ADD_I, K_ADC_R, K_ADC_I, K_SUB_R, K_SUB_I, K_SBC_R, K_CMP_R, K_MERGE };
    static const unsigned char dw[] = { K_LSR, K_ROL, K_LDW_R, K_LDB_R, K_SWAP, K_NOT, K_ADD_R, K_ADD_I, K_ADC_R, K_ADC_I,
        K_SUB_R, K_SUB_I, K_SBC_R, K_MERGE, K_AND_R, K_AND_I, K_BIC_R, K_BIC_I, K_MULT_R, K_MULT_I, K_UMULT_R,
        K_UMULT_I, K_SEX, K_ASR, K_DIV2, K_ROR, K_LOB, K_FMULT, K_LMULT, K_HIB, K_OR_R, K_OR_I, K_XOR_R, K_XOR_I,
        K_GETB, K_GETBH, K_GETBL, K_GETBS };
    /* simple = one byte, no R15 use of its own, no ARC flags, no helper call, no T3: may sit in a delay slot */
    static const unsigned char simple[] = { K_NOP, K_LSR, K_ROL, K_STW_R, K_STB_R, K_LDW_R, K_LDB_R, K_SWAP, K_NOT,
        K_ADD_R, K_ADD_I, K_ADC_R, K_ADC_I, K_SUB_R, K_SUB_I, K_SBC_R, K_CMP_R, K_AND_R, K_AND_I, K_BIC_R,
        K_BIC_I, K_MULT_R, K_MULT_I, K_UMULT_R, K_UMULT_I, K_SBK, K_SEX, K_ASR, K_ROR, K_LOB, K_FMULT, K_LMULT,
        K_HIB, K_OR_R, K_OR_I, K_XOR_R, K_XOR_I, K_INC_R, K_DEC_R, K_GETB, K_GETBH, K_GETBL, K_GETBS, K_COLOR,
        K_GETC };
    unsigned i;
    for (i = 0; i < sizeof sz; i++) kfw[sz[i]] = FS | FZ;
    for (i = 0; i < sizeof szc; i++) kfw[szc[i]] = FS | FZ | FC;
    for (i = 0; i < sizeof all; i++) kfw[all[i]] = FALL;
    for (i = 0; i < sizeof dw; i++) kdw[dw[i]] = 1;
    for (i = 0; i < sizeof simple; i++) ksimple[simple[i]] = 1;
    kfr[K_ADC_R] = kfr[K_ADC_I] = kfr[K_SBC_R] = kfr[K_ROL] = kfr[K_ROR] = FC;
    kfr[K_BNE] = kfr[K_BEQ] = FZ;
    kfr[K_BPL] = kfr[K_BMI] = FS;
    kfr[K_BCC] = kfr[K_BCS] = FC;
    kfr[K_BVC] = kfr[K_BVS] = FO;
    kfr[K_BLT] = kfr[K_BGE] = FS | FO;
}
static int fxj_isbranch(int k)
{
    return k == K_BNE || k == K_BEQ || k == K_BPL || k == K_BMI || k == K_BCC || k == K_BCS || k == K_BVC ||
           k == K_BVS || k == K_BLT || k == K_BGE;
}
/* the op in s->pipe can run in a delay slot natively (state s = before the step) */
static int fxj_simple(const St *s)
{
    int idx = fxj_idx(s), k = fxj_tab[idx][0], n = fxj_tab[idx][1];
    if (!ksimple[k]) return 0;
    if (s->sreg == 15) return 0;
    if (kdw[k] && s->dreg == 15) return 0;
    if (n == 15 && (k == K_ADD_R || k == K_ADC_R || k == K_SUB_R || k == K_SBC_R || k == K_CMP_R || k == K_AND_R ||
                    k == K_BIC_R || k == K_MULT_R || k == K_UMULT_R || k == K_OR_R || k == K_XOR_R)) return 0;
    return 1;
}

/* flags possibly read before written from state s on (static walk of the ROM code) */
static int fxj_scan(St s, int budget)
{
    int need = 0, wr = 0;
#if defined FXJ_LOCK || defined FXJ_NOLIVE
    return FALL;
#endif
    /* a counted run can stop before any reservation, and the flags are then seen in SFR: all live */
    if (fxj_counted)
        return FALL;
    while (budget-- > 0) {
        int idx, k, n, op;
        if (s.dyn) break;
        idx = fxj_idx(&s);
        op = s.pipe;
        k = fxj_tab[idx][0];
        n = fxj_tab[idx][1];
        s.pipe = MEM(s.r15);
        need |= kfr[k] & ~wr;
        switch (k) {
        case K_ALT1: s.alt |= 1; s.b = 0; s.r15++; continue;
        case K_ALT2: s.alt |= 2; s.b = 0; s.r15++; continue;
        case K_ALT3: s.alt = 3; s.b = 0; s.r15++; continue;
        case K_WITH_R: s.b = 1; s.sreg = s.dreg = n; s.r15++; continue;
        case K_TO_R:
            if (s.b) { if (n == 15) { s.dyn = 1; break; } CLR(&s); s.r15++; continue; }
            s.dreg = n; s.r15++; continue;
        case K_FROM_R:
            if (s.b) { wr |= FS | FZ | FO; s.r15++; if (s.dreg == 15) s.dyn = 1; CLR(&s); continue; }
            s.sreg = n; s.r15++; continue;
        case K_BRA: {
            int v = s.pipe; s.r15++; s.pipe = MEM(s.r15); s.r15 += (signed char)v; continue; }
        case K_IBT_R: case K_LMS_R: case K_SMS_R:
            s.r15++; s.pipe = MEM(s.r15); s.r15++; CLR(&s);
            if (n == 15 && k != K_SMS_R) { if (k == K_LMS_R) s.dyn = 1; }
            if (n == 15 && k == K_IBT_R) s.r15 = (fxu32)(int)(signed char)MEM(s.r15 - 3 + 1);
            continue;
        case K_IWT_R: case K_LM_R: case K_SM_R: {
            int lo = s.pipe, hi;
            s.r15++; s.pipe = MEM(s.r15); s.r15++; hi = s.pipe; s.pipe = MEM(s.r15); s.r15++; CLR(&s);
            if (n == 15 && k == K_IWT_R) s.r15 = (fxu32)(lo | hi << 8);
            if (n == 15 && k == K_LM_R) s.dyn = 1;
            continue; }
        case K_STOP: case K_LJMP_R: case K_JMP_R: case K_CMODE: case K_RPIX:
            return need | (FALL & ~wr);
        case K_LOOP:
            wr |= FS | FZ;
            return need | (FALL & ~wr);
        default:
            break;
        }
        if (fxj_isbranch(k)) {
            int v = s.pipe, t;
            St ts;
            s.r15++; s.pipe = MEM(s.r15);
            ts = s; ts.r15 += (signed char)v;
            t = fxj_scan(ts, budget >> 1);
            need |= t & ~wr;
            s.r15++;
            continue;
        }
        (void)op;
        wr |= kfw[k];
        s.r15++;
        if (kdw[k] && s.dreg == 15) s.dyn = 1;
        CLR(&s);
        if (wr == FALL) return need;
    }
    return need | (FALL & ~wr);
}

/* cold stubs of the block being made */
typedef struct { fxu32 key; fxu32 *src; int cc; } FxjCold;
static FxjCold fxj_cold[64];
static int fxj_ncold;

/* jump (cc: condition, 0 = always) to the clean point key = bank << 16 | address */
static void fxj_exit_static(fxu32 a, int cc)
{
    fxu32 *h = fxj_find(fxj_bank, (uint16)a);
#ifdef FXJ_LOCK
    h = 0;
#endif
    if (h) { BCC(h, cc); return; }
    if (fxj_ncold >= 64) { fxj_fail = 1; return; }
    fxj_cold[fxj_ncold].key = (fxj_bank << 16) | (uint16)a;
    fxj_cold[fxj_ncold].src = FWD();
    fxj_cold[fxj_ncold].cc = cc;
    fxj_ncold++;
}
/* exit to the interpreter with the state s (before the step of the op in s->pipe); r15 in reg (or -1) */
static void fxj_exit_raw(const St *s, int r15reg)
{
    if (r15reg >= 0) MOV(T1, r15reg); else MOVK(T1, (uint16)s->r15);
    MOVK(T2, s->pipe | s->alt << 8 | s->b << 10 | s->sreg << 12 | s->dreg << 16);
    MOVK(T0, X_RAW);
    BCC(fxj_exit, 0);
}
/* dynamic jump to the address in T3 (guess: a likely target, -1 none) */
static void fxj_jump_dyn(int guess)
{
    if (guess >= 0) {
        fxu32 *f;
        SOP(S_EXTW, T1, T3);
        f = FWD();                              /* brne T1,limm,miss */
        fxj_p++;
        fxj_exit_static(guess, 0);
        fxj_w(f, enc_br(T1, LIMM, 0, BR_NE, OFF(fxj_p, f)));
        fxj_w(f + 1, (fxu32)guess);
    }
    MOV(T1, T3);
    MOVK(T2, fxj_bank << 16);
    BCC(fxj_disp, 0);
}

static void FL_SZ(int v, int live)
{
    if (live & FS) MOV(RS, v);
    if (live & FZ) MOV(RZ, v);
}
static int SRC(const St *s, int k, int tmp)
{
    if (k == 15) { MOVK(tmp, (uint16)s->r15); return tmp; }
    return G(k);
}
static int DST(St *s, int k)
{
    if (k == 15) { s->dyn = 1; return T3; }
    return G(k);
}
/* 16-bit word store / load at a runtime address (reg ar holds the full GSU address) the C way:
   lo -> [a], hi -> [a ^ 1] */
static void fxj_stw(int val, int ar)
{
    fxu32 *f, *g;
    SOP(S_EXTW, T0, ar);
    f = FWD();                                  /* bbit1 T0,0 -> odd */
    I3(A_ADD, T0, RRAM, T0);
    STO(val, T0, 0, Z_H);
    g = FWD();
    PBRC(f, fxj_p, T0, 0, 1, BB_1);
    I3(A_ADD, T0, RRAM, T0);
    STO(val, T0, 0, Z_B);
    XU(X_LSR, T1, val, 8);
    STO(T1, T0, -1, Z_B);
    PBCC(g, fxj_p, 0);
}
static void fxj_ldw(int out, int ar)
{
    fxu32 *f, *g;
    SOP(S_EXTW, T0, ar);
    if (ar != RLAST) MOV(RLAST, ar);
    f = FWD();
    E(enc_ldr(out, RRAM, T0, Z_H, 0, 0));
    g = FWD();
    PBRC(f, fxj_p, T0, 0, 1, BB_1);
    I3(A_ADD, T0, RRAM, T0);
    LDO(T1, T0, 0, Z_B);
    LDO(T2, T0, -1, Z_B);
    XU(X_ASL, T2, T2, 8);
    I3(A_OR, out, T1, T2);
    PBCC(g, fxj_p, 0);
}
/* colour register update from c in T0 (fx_color / fx_getc) */
static void fxj_colortail(void)
{
    fxu32 *f, *g;
    LDG(T1, GOFF(vPlotOptionReg));
    f = FWD();
    XU(X_LSR, T2, T0, 4);
    IK(A_AND, T0, T0, 0xf0);
    I3(A_OR, T0, T0, T2);
    PBRC(f, fxj_p, T1, 2, 1, BB_0);
    g = FWD();
    LDG(T2, GOFF(vColorReg));
    IK(A_AND, T2, T2, 0xf0);
    IU(A_AND, T0, T0, 15);
    I3(A_OR, T0, T2, T0);
    PBRC(g, fxj_p, T1, 3, 1, BB_0);
    STG(T0, GOFF(vColorReg), Z_W);
}

/* arithmetic: kind, a = SREG reg, b = reg (or imm when isimm) */
static void fxj_arith(St *s, int k, int a, int b, int imm, int isimm, int live)
{
    int sub = k == K_SUB_R || k == K_SUB_I || k == K_SBC_R || k == K_CMP_R;
    int cy = k == K_ADC_R || k == K_ADC_I || k == K_SBC_R;
    int cmp = k == K_CMP_R;
    int out, res;
    s->r15++;
    out = cmp ? T2 : DST(s, s->dreg);
    if (!(live & (FC | FO))) {
        if (cmp && !(live & (FS | FZ))) { CLR(s); return; }
        if (!cy) {
            if (isimm) IK(sub ? A_SUB : A_ADD, out, a, imm);
            else I3(sub ? A_SUB : A_ADD, out, a, b);
        } else {
            if (isimm) IK(sub ? A_SUB : A_ADD, T0, a, imm);
            else I3(sub ? A_SUB : A_ADD, T0, a, b);
            if (sub) { IU(A_XOR, T1, RC, 1); I3(A_SUB, out, T0, T1); }
            else I3(A_ADD, out, T0, RC);
        }
        FL_SZ(out, live);
        CLR(s);
        return;
    }
    SOP(S_EXTW, T0, a);
    if (isimm) MOVK(T1, imm); else SOP(S_EXTW, T1, b);
    I3(sub ? A_SUB : A_ADD, T2, T0, T1);
    if (cy) {
        if (sub) { IU(A_XOR, T4, RC, 1); I3(A_SUB, T2, T2, T4); }
        else I3(A_ADD, T2, T2, RC);
    }
    if (live & FO) {
        if (!sub) {                             /* ~(a ^ b) & (b ^ s) & 0x8000 */
            I3(A_XOR, T4, T0, T1);
            SOP(S_NOT, T4, T4);
            I3(A_XOR, T1, T1, T2);
        } else {                                /* (a ^ b) & (a ^ s) & 0x8000 */
            I3(A_XOR, T4, T0, T1);
            I3(A_XOR, T1, T0, T2);
        }
        I3(A_AND, T4, T4, T1);
        IK(A_AND, T4, T4, 0x8000);
        STG(T4, GOFF(vOverflow), Z_W);
    }
    if (live & FC) {
        if (!sub) XU(X_LSR, RC, T2, 16);
        else { XU(X_LSR, RC, T2, 31); IU(A_XOR, RC, RC, 1); }
    }
    res = T2;
    FL_SZ(res, live);
    if (!cmp) MOV(out, res);
    CLR(s);
}

/* one op at state s (pipe already = op); returns 1 if emitted, 0 if not handled */
static int fxj_op(St *s, int idx, int live)
{
    int k = fxj_tab[idx][0], n = fxj_tab[idx][1];
    int a, b, out;
#ifdef FXJ_NOROMB
    if (k == K_ROMB) return 0;
#endif
    switch (k) {
    case K_NOP: CLR(s); s->r15++; return 1;
    case K_CACHE:
        MOVK(T0, s->r15 & 0xfff0);
        STG(T0, GOFF(vCacheBaseReg), Z_W);
        MOVK(T0, 1);
        STG(T0, GOFF(bCacheActive), Z_B);
        s->r15++; CLR(s); return 1;
    case K_LSR: case K_ASR: case K_DIV2:
        a = SRC(s, s->sreg, T3);
        SOP(k == K_LSR ? S_EXTW : S_SEXW, T0, a);
        if (live & FC) IU(A_AND, RC, T0, 1);
        s->r15++;
        out = DST(s, s->dreg);
        if (k == K_DIV2) {
            SOP(S_ASR, T1, T0);
            IUF(A_ADD, LIMM, T0, 1);
            E(enc_cc(4, A_MOV, T1, 0, 1, CC_EQ, 0));
            MOV(out, T1);
        } else SOP(k == K_LSR ? S_LSR : S_ASR, out, T0);
        FL_SZ(out, live); CLR(s); return 1;
    case K_ROL:
        a = SRC(s, s->sreg, T3);
        XU(X_ASL, T0, a, 1);
        I3(A_ADD, T0, T0, RC);
        SOP(S_EXTW, T0, T0);
        if (live & FC) { XU(X_LSR, RC, a, 15); IU(A_AND, RC, RC, 1); }
        s->r15++;
        out = DST(s, s->dreg);
        MOV(out, T0); FL_SZ(out, live); CLR(s); return 1;
    case K_ROR:
        a = SRC(s, s->sreg, T3);
        SOP(S_EXTW, T0, a);
        SOP(S_LSR, T0, T0);
        XU(X_ASL, T1, RC, 15);
        I3(A_OR, T0, T0, T1);
        if (live & FC) IU(A_AND, RC, a, 1);
        s->r15++;
        out = DST(s, s->dreg);
        MOV(out, T0); FL_SZ(out, live); CLR(s); return 1;
    case K_TO_R:
        if (s->b) {
            a = SRC(s, s->sreg, T3);
            if (n == 15) { MOV(T3, a); s->dyn = 1; CLR(s); return 1; }
            MOV(G(n), a); CLR(s); s->r15++; return 1;
        }
        s->dreg = n; s->r15++; return 1;
    case K_WITH_R: s->b = 1; s->sreg = s->dreg = n; s->r15++; return 1;
    case K_ALT1: s->alt |= 1; s->b = 0; s->r15++; return 1;
    case K_ALT2: s->alt |= 2; s->b = 0; s->r15++; return 1;
    case K_ALT3: s->alt = 3; s->b = 0; s->r15++; return 1;
    case K_STW_R:
        a = SRC(s, s->sreg, T3);
        MOV(RLAST, G(n));
        fxj_stw(a, G(n));
        CLR(s); s->r15++; return 1;
    case K_SBK:
        a = SRC(s, s->sreg, T3);
        fxj_stw(a, RLAST);
        CLR(s); s->r15++; return 1;
    case K_STB_R:
        a = SRC(s, s->sreg, T3);
        SOP(S_EXTW, T0, G(n));
        MOV(RLAST, G(n));
        I3(A_ADD, T0, RRAM, T0);
        STO(a, T0, 0, Z_B);
        CLR(s); s->r15++; return 1;
    case K_LDW_R:
        s->r15++;
        out = DST(s, s->dreg);
        fxj_ldw(out, G(n));
        CLR(s); return 1;
    case K_LDB_R:
        s->r15++;
        out = DST(s, s->dreg);
        SOP(S_EXTW, T0, G(n));
        MOV(RLAST, G(n));
        E(enc_ldr(out, RRAM, T0, Z_B, 0, 0));
        CLR(s); return 1;
    case K_PLOT:
        LDA(T0, &fxj_plotfn);
        E(enc_jl(T0));
        s->r15++; CLR(s); return 1;
    case K_SWAP:
        a = SRC(s, s->sreg, T3);
        SOP(S_EXTB, T0, a);
        XU(X_ASL, T0, T0, 8);
        XU(X_LSR, T1, a, 8);
        SOP(S_EXTB, T1, T1);
        s->r15++;
        out = DST(s, s->dreg);
        I3(A_OR, out, T0, T1);
        FL_SZ(out, live); CLR(s); return 1;
    case K_COLOR:
        a = SRC(s, s->sreg, T3);
        SOP(S_EXTB, T0, a);
        fxj_colortail();
        CLR(s); s->r15++; return 1;
    case K_GETC:
        LDO(T0, RGSU, GOFF(vRomBuffer), Z_B);
        fxj_colortail();
        CLR(s); s->r15++; return 1;
    case K_NOT:
        a = SRC(s, s->sreg, T3);
        s->r15++;
        out = DST(s, s->dreg);
        SOP(S_NOT, out, a);
        FL_SZ(out, live); CLR(s); return 1;
    case K_ADD_R: case K_ADC_R: case K_SUB_R: case K_SBC_R: case K_CMP_R:
        a = SRC(s, s->sreg, T3);
        b = SRC(s, n, T4);
        fxj_arith(s, k, a, b, 0, 0, live);
        return 1;
    case K_ADD_I: case K_ADC_I: case K_SUB_I:
        a = SRC(s, s->sreg, T3);
        fxj_arith(s, k, a, 0, n, 1, live);
        return 1;
    case K_MERGE: {
        fxu32 *f;
        s->r15++;
        out = DST(s, s->dreg);
        IK(A_AND, T0, G(7), 0xff00);
        XU(X_LSR, T1, G(8), 8);
        SOP(S_EXTB, T1, T1);
        I3(A_OR, T0, T0, T1);
        if (live & FO) { IK(A_AND, T1, T0, 0xc0c0); XU(X_ASL, T1, T1, 16); STG(T1, GOFF(vOverflow), Z_W); }
        if (live & FZ) {
            EL(enc_rrr(4, A_AND, LIMM, T0, LIMM, 1), 0xf0f0);
            MOVK(RZ, 0);
            E(enc_cc(4, A_MOV, RZ, 1, 1, CC_EQ, 0));
        }
        if (live & FS) { XU(X_ASL, T1, T0, 8); I3(A_OR, RS, T1, T0); }
        if (live & FC) {
            EL(enc_rrr(4, A_AND, LIMM, T0, LIMM, 1), 0xe0e0);
            MOVK(RC, 0);
            E(enc_cc(4, A_MOV, RC, 1, 1, CC_NE, 0));
        }
        (void)f;
        MOV(out, T0); CLR(s); return 1; }
    case K_AND_R: case K_BIC_R: case K_OR_R: case K_XOR_R:
    case K_AND_I: case K_BIC_I: case K_OR_I: case K_XOR_I: {
        int sub = (k == K_AND_R || k == K_AND_I) ? A_AND : (k == K_BIC_R || k == K_BIC_I) ? A_BIC :
                  (k == K_OR_R || k == K_OR_I) ? A_OR : A_XOR;
        int isimm = k == K_AND_I || k == K_BIC_I || k == K_OR_I || k == K_XOR_I;
        a = SRC(s, s->sreg, T3);
        b = isimm ? 0 : SRC(s, n, T4);
        s->r15++;
        out = DST(s, s->dreg);
        if (isimm) IU(sub, out, a, n); else I3(sub, out, a, b);
        FL_SZ(out, live); CLR(s); return 1; }
    case K_MULT_R: case K_UMULT_R: case K_MULT_I: case K_UMULT_I: {
        int sg = k == K_MULT_R || k == K_MULT_I;
        a = SRC(s, s->sreg, T3);
        SOP(sg ? S_SEXB : S_EXTB, T0, a);
        if (k == K_MULT_R || k == K_UMULT_R) {
            b = SRC(s, n, T4);
            SOP(sg ? S_SEXB : S_EXTB, T1, b);
            X3(X_MUL64, LIMM, T0, T1);
        } else XU(X_MUL64, LIMM, T0, n);
        s->r15++;
        out = DST(s, s->dreg);
        MOV(out, 58);
        FL_SZ(out, live); CLR(s); return 1; }
    case K_LINK_I:
        MOVK(G(11), (uint16)(s->r15 + n));
        CLR(s); s->r15++; return 1;
    case K_SEX:
        a = SRC(s, s->sreg, T3);
        s->r15++;
        out = DST(s, s->dreg);
        SOP(S_SEXB, out, a);
        FL_SZ(out, live); CLR(s); return 1;
    case K_JMP_R:
        MOV(T3, G(n));
        s->dyn = 1; CLR(s); return 1;
    case K_LOB: case K_HIB:
        a = SRC(s, s->sreg, T3);
        if (k == K_LOB) SOP(S_EXTB, T0, a);
        else { XU(X_LSR, T0, a, 8); SOP(S_EXTB, T0, T0); }
        if (live & (FS | FZ)) { XU(X_ASL, T1, T0, 8); FL_SZ(T1, live); }
        s->r15++;
        out = DST(s, s->dreg);
        MOV(out, T0); CLR(s); return 1;
    case K_FMULT: case K_LMULT:
        a = SRC(s, s->sreg, T3);
        SOP(S_SEXW, T0, a);
        SOP(S_SEXW, T1, G(6));
        X3(X_MUL64, LIMM, T0, T1);
        MOV(T0, 58);
        s->r15++;
        out = DST(s, s->dreg);
        if (k == K_FMULT) {
            if (live & FC) { XU(X_LSR, RC, T0, 15); IU(A_AND, RC, RC, 1); }
            XU(X_LSR, out, T0, 16);
            FL_SZ(out, live);
        } else {
            MOV(G(4), T0);
            XU(X_LSR, T1, T0, 16);
            MOV(out, T1);
            FL_SZ(T1, live);
            if (live & FC) { XU(X_LSR, RC, G(4), 15); IU(A_AND, RC, RC, 1); }
        }
        CLR(s); return 1;
    case K_IBT_R: {
        int v = s->pipe;
        s->r15++; s->pipe = MEM(s->r15); s->r15++;
        if (n == 15) s->r15 = (fxu32)(int)(signed char)v;
        else MOVK(G(n), (fxu32)(int)(signed char)v);
        CLR(s); return 1; }
    case K_LMS_R: {
        int adr = s->pipe << 1;
        s->r15++; s->pipe = MEM(s->r15); s->r15++;
        out = DST(s, n);
        MOVK(RLAST, adr);
        MOVK(T0, adr);
        E(enc_ldr(out, RRAM, T0, Z_H, 0, 0));
        CLR(s); return 1; }
    case K_SMS_R: {
        int adr = s->pipe << 1;
        a = SRC(s, n, T3);
        s->r15++; s->pipe = MEM(s->r15);
        MOVK(RLAST, adr);
        IK(A_ADD, T0, RRAM, adr);
        STO(a, T0, 0, Z_H);
        CLR(s); s->r15++; return 1; }
    case K_FROM_R:
        if (s->b) {
            a = SRC(s, n, T4);
            if (live & FO) { IK(A_AND, T0, a, 0x80); XU(X_ASL, T0, T0, 16); STG(T0, GOFF(vOverflow), Z_W); }
            FL_SZ(a, live);
            s->r15++;
            out = DST(s, s->dreg);
            MOV(out, a);
            CLR(s); return 1;
        }
        s->sreg = n; s->r15++; return 1;
    case K_INC_R: case K_DEC_R:
        IU(k == K_INC_R ? A_ADD : A_SUB, G(n), G(n), 1);
        FL_SZ(G(n), live);
        CLR(s); s->r15++; return 1;
    case K_RAMB:
        a = SRC(s, s->sreg, T3);
        IU(A_AND, T0, a, 3);
        STG(T0, GOFF(vRamBankReg), Z_W);
        EL(enc_rrr(4, A_ADD2, T1, LIMM, T0, 0), (fxu32)&GSU.apvRamBank[0]);
        LDO(RRAM, T1, 0, Z_W);
        STG(RRAM, GOFF(pvRamBank), Z_W);
        CLR(s); s->r15++; return 1;
    case K_ROMB:
        a = SRC(s, s->sreg, T3);
        IU(A_BMSK, T0, a, 6);
        STG(T0, GOFF(vRomBankReg), Z_W);
        EL(enc_rrr(4, A_ADD2, T1, LIMM, T0, 0), (fxu32)&GSU.apvRomBank[0]);
        LDO(RROM, T1, 0, Z_W);
        STG(RROM, GOFF(pvRomBank), Z_W);
        CLR(s); s->r15++; return 1;
    case K_GETB: case K_GETBS:
        s->r15++;
        out = DST(s, s->dreg);
        E(enc_ld(out, RGSU, GOFF(vRomBuffer), Z_B, k == K_GETBS, 0));
        CLR(s); return 1;
    case K_GETBH: case K_GETBL:
        a = SRC(s, s->sreg, T3);
        LDO(T0, RGSU, GOFF(vRomBuffer), Z_B);
        if (k == K_GETBH) { XU(X_ASL, T0, T0, 8); SOP(S_EXTB, T1, a); }
        else IK(A_AND, T1, a, 0xff00);
        s->r15++;
        out = DST(s, s->dreg);
        I3(A_OR, out, T1, T0);
        CLR(s); return 1;
    case K_IWT_R: {
        int v = s->pipe;
        s->r15++; s->pipe = MEM(s->r15); s->r15++;
        v |= s->pipe << 8; s->pipe = MEM(s->r15); s->r15++;
        if (n == 15) s->r15 = v;
        else MOVK(G(n), v);
        CLR(s); return 1; }
    case K_LM_R: {
        int adr = s->pipe;
        s->r15++; s->pipe = MEM(s->r15); s->r15++;
        adr |= s->pipe << 8; s->pipe = MEM(s->r15); s->r15++;
        out = DST(s, n);
        MOVK(RLAST, adr);
        if (!(adr & 1)) { MOVK(T0, adr); E(enc_ldr(out, RRAM, T0, Z_H, 0, 0)); }
        else {
            IK(A_ADD, T0, RRAM, adr);
            LDO(T1, T0, 0, Z_B);
            LDO(T2, T0, -1, Z_B);
            XU(X_ASL, T2, T2, 8);
            I3(A_OR, out, T1, T2);
        }
        CLR(s); return 1; }
    case K_SM_R: {
        int adr = s->pipe;
        a = SRC(s, n, T3);
        s->r15++; s->pipe = MEM(s->r15); s->r15++;
        adr |= s->pipe << 8; s->pipe = MEM(s->r15);
        MOVK(RLAST, adr);
        IK(A_ADD, T0, RRAM, adr);
        if (!(adr & 1)) STO(a, T0, 0, Z_H);
        else { STO(a, T0, 0, Z_B); XU(X_LSR, T1, a, 8); STO(T1, T0, -1, Z_B); }
        CLR(s); s->r15++; return 1; }
    default:
        return 0;                               /* stop, cmode, rpix, ljmp, branches (caller) */
    }
}

/* the C core runs with FX_DO_ROMBUFFER (fxinst.c): getb/getbh/getbl/getbs/getc read GSU.vRomBuffer, which is
   ROM(R14) as of the last write to R14 (TESTR14 after a DREG write, READR14 in the *_r14 ops); romb does
   not reload it. fxj_op2 = fxj_op + that reload. */
static int fxj_writes14(const St *pre, int idx)
{
    int k = fxj_tab[idx][0], n = fxj_tab[idx][1];
    if (kdw[k] && pre->dreg == 14) return 1;
    if (k == K_FROM_R && pre->b && pre->dreg == 14) return 1;
    if (k == K_TO_R && pre->b && n == 14) return 1;
    if (n == 14 && (k == K_INC_R || k == K_DEC_R || k == K_IBT_R || k == K_IWT_R || k == K_LMS_R || k == K_LM_R)) return 1;
    return 0;
}
static int fxj_op2(St *s, int idx, int live)
{
    St pre = *s;
    int r = fxj_op(s, idx, live);
    if (r && fxj_writes14(&pre, idx)) {
        SOP(S_EXTW, T0, G(14));
        E(enc_ldr(T0, RROM, T0, Z_B, 0, 0));
        STG(T0, GOFF(vRomBuffer), Z_B);
    }
    return r;
}

/* branch condition into the ARC flags; returns the ARC cc for "taken" */
static int fxj_test(int k)
{
    switch (k) {
    case K_BEQ: case K_BNE:
        SOPF(S_EXTW, LIMM, RZ);
        return k == K_BEQ ? CC_EQ : CC_NE;
    case K_BPL: case K_BMI:
        E(enc_rru(4, A_BTST, 0, RS, 15, 1));
        return k == K_BMI ? CC_NE : CC_EQ;
    case K_BCC: case K_BCS:
        E(enc_rru(4, A_BTST, 0, RC, 0, 1));
        return k == K_BCS ? CC_NE : CC_EQ;
    case K_BVC: case K_BVS:
        LDG(T0, GOFF(vOverflow));
        E(enc_rrr(4, A_TST, 0, T0, T0, 1));
        return k == K_BVS ? CC_NE : CC_EQ;
    default:                                    /* blt / bge: (S != 0) != (OV != 0) */
        XU(X_LSR, T0, RS, 15);
        IU(A_AND, T0, T0, 1);
        LDG(T1, GOFF(vOverflow));
        E(enc_rrr(4, A_TST, 0, T1, T1, 1));
        E(enc_cc(4, A_MOV, T1, 1, 1, CC_NE, 0));
        E(enc_rrr(4, A_XOR, LIMM, T0, T1, 1));
        return k == K_BLT ? CC_NE : CC_EQ;
    }
}
static int fxj_inv(int cc) { return cc == CC_EQ ? CC_NE : CC_EQ; }

static int fxj_trace(St s, int path, int start, fxu32 entry);

/* the state after an op that set R15 dynamically (in T3): the delay op, then a dispatch */
/* counted runs (Settings.WinterGold games: FxEmulate(350/700)): translated code takes the ops it is about to
   run from GSU.vCounter in advance ("reservations" of a run of ops with no exit in between); when the
   counter can't cover them it leaves to the interpreter before them, which counts op by op as fx_run.
   fxj_resv = ops still covered by the current reservation; refunded if a block ends before using them. */
static int fxj_resv;
static void fxj_reserve(const St *s, int k, int r15reg)
{
    fxu32 *f;
    LDG(T0, GOFF(vCounter));
    f = FWD();
    fxj_exit_raw(s, r15reg);
    PBRC(f, fxj_p, T0, k, 1, BR_HS);
    IU(A_SUB, T0, T0, k);
    STG(T0, GOFF(vCounter), Z_W);
    fxj_resv = k;
}
static void fxj_refund_n(int n)
{
    if (fxj_rsv && n > 0) {
        LDG(T0, GOFF(vCounter));
        IU(A_ADD, T0, T0, n);
        STG(T0, GOFF(vCounter), Z_W);
    }
}
static void fxj_refund(void)
{
    if (fxj_rsv && fxj_resv > 0) {
        LDG(T0, GOFF(vCounter));
        IU(A_ADD, T0, T0, fxj_resv);
        STG(T0, GOFF(vCounter), Z_W);
    }
    fxj_resv = 0;
}
static int fxj_native(int k)
{
    return k != K_STOP && k != K_RPIX && k != K_LJMP_R;
}
/* ops from s on that run with no exit (natively, no branch), at most max */
static int fxj_plain(St s, int max)
{
    int c = 0;
    while (c < max && !s.dyn) {
        int idx = fxj_idx(&s), k = fxj_tab[idx][0], n = fxj_tab[idx][1];
        if (!fxj_native(k) || fxj_isbranch(k) || k == K_BRA || k == K_LOOP || k == K_JMP_R) break;
        s.pipe = MEM(s.r15);
        c++;
        switch (k) {
        case K_ALT1: s.alt |= 1; s.b = 0; s.r15++; continue;
        case K_ALT2: s.alt |= 2; s.b = 0; s.r15++; continue;
        case K_ALT3: s.alt = 3; s.b = 0; s.r15++; continue;
        case K_WITH_R: s.b = 1; s.sreg = s.dreg = n; s.r15++; continue;
        case K_TO_R:
            if (s.b) { if (n == 15) return c; CLR(&s); s.r15++; continue; }
            s.dreg = n; s.r15++; continue;
        case K_FROM_R:
            if (s.b) { if (s.dreg == 15) return c; CLR(&s); s.r15++; continue; }
            s.sreg = n; s.r15++; continue;
        case K_IBT_R: case K_LMS_R: case K_SMS_R:
            if (n == 15 && k != K_SMS_R) return c;
            s.r15++; s.pipe = MEM(s.r15); s.r15++; CLR(&s); continue;
        case K_IWT_R: case K_LM_R: case K_SM_R:
            if (n == 15 && k != K_SM_R) return c;
            s.r15++; s.pipe = MEM(s.r15); s.r15++; s.pipe = MEM(s.r15); s.r15++; CLR(&s); continue;
        default:
            if (kdw[k] && s.dreg == 15) return c;
            s.r15++; CLR(&s); continue;
        }
    }
    return c;
}

static void fxj_after_dyn(St *s, int guess)
{
    if (fxj_rsv && fxj_resv < 1 && fxj_simple(s)) fxj_reserve(s, 1, T3);
    if (fxj_simple(s)) {
        fxj_resv--;
        St d = *s;
        int idx = fxj_idx(&d);
        d.dyn = 0;
        d.pipe = 0;                             /* fetched from the dynamic address: unused by simple ops */
        fxj_op2(&d, idx, FALL);
        fxj_jump_dyn(guess);
    } else fxj_exit_raw(s, T3);
}

/* translate from state s; path = 1: a branch's taken side (stop at the first clean point).
   Returns 0 when done. */
static int fxj_trace(St s, int path, int start, fxu32 entry)
{
    int nops = 0, jumped = 0, r13 = -1;
    for (;;) {
        St pre;
        int idx, k, n;
        if (fxj_fail || fxj_p > fxj_code + FXJ_CODE_WORDS - 256) { fxj_fail = 1; return 0; }
        if (s.dyn) { fxj_after_dyn(&s, -1); return 0; }
        if (fxj_clean(&s) && fxj_normal(&s)) {
            fxu32 a = (uint16)(s.r15 - 1);
            fxu32 *h = fxj_find(fxj_bank, a);
            if (nops && (path || jumped || nops >= FXJ_MAXOPS || h)) { fxj_refund(); fxj_exit_static(a, 0); return 0; }
            if (!h && !(fxj_rsv && fxj_resv > 0) && !fxj_reg(fxj_bank, a, fxj_p)) { fxj_fail = 1; return 0; }
        }
        pre = s;
        idx = fxj_idx(&s);
        k = fxj_tab[idx][0];
        n = fxj_tab[idx][1];
        if (fxj_rsv && fxj_native(k)) {
            St nx = s;
            int two;
            nx.pipe = MEM(nx.r15);
            if (fxj_isbranch(k)) { nx.r15++; nx.pipe = MEM(nx.r15); }
            else if (k == K_LOOP) CLR(&nx);
            two = (fxj_isbranch(k) || k == K_LOOP) && (fxj_simple(&nx) || fxj_kind(&nx) == K_PLOT);
            if (fxj_isbranch(k) || k == K_LOOP || k == K_BRA || k == K_JMP_R) {
                if (fxj_resv > 0) fxj_refund();
                fxj_reserve(&pre, two ? 2 : 1, -1);
            } else if (fxj_resv < 1) {
                int r = fxj_plain(s, 32);
                fxj_reserve(&pre, r > 0 ? r : 1, -1);
            }
            fxj_resv -= two ? 2 : 1;
        }
        s.pipe = MEM(s.r15);                    /* FETCHPIPE of the step */
        nops++;
        if (k == K_BRA) {
            int v = s.pipe;
            s.r15++; s.pipe = MEM(s.r15); s.r15 += (signed char)v;
            jumped = 1;
            continue;
        }
        if (fxj_isbranch(k)) {
            int v = s.pipe, cc;
            St ts, ns;
            s.r15++; s.pipe = MEM(s.r15);
            ts = s; ts.r15 += (signed char)v;
            ns = s; ns.r15++;
            if (fxj_kind(&s) == K_PLOT) {
                /* plot in the delay slot: it doesn't touch the flags the test reads, so it goes first */
                St d = s;
                fxj_op2(&d, fxj_idx(&d), FALL);
                cc = fxj_test(k);
                ts.pipe = MEM(ts.r15); ts.r15++; CLR(&ts);
                fxj_exit_static((uint16)(ts.r15 - 1), cc);
                ns.pipe = MEM(ns.r15); ns.r15++; CLR(&ns);
                s = ns;
                nops++;
                continue;
            }
            cc = fxj_test(k);
            if (fxj_simple(&s)) {
                /* the delay op once, then the conditional jump */
                St d = s;
                int live, didx = fxj_idx(&d);
                {
                    St a1 = ts, a2 = ns;
                    a1.pipe = MEM(a1.r15); a1.r15++; CLR(&a1);
                    a2.pipe = MEM(a2.r15); a2.r15++; CLR(&a2);
                    live = fxj_scan(a1, 24) | fxj_scan(a2, 24);
                }
                fxj_op2(&d, didx, live);
                ts.pipe = MEM(ts.r15); ts.r15++; CLR(&ts);
                fxj_exit_static((uint16)(ts.r15 - 1), cc);
                ns.pipe = MEM(ns.r15); ns.r15++; CLR(&ns);
                s = ns;
                nops++;
                continue;
            }
            {
                fxu32 *f = FWD();
                fxj_resv = 0;
                fxj_trace(ts, 1, 0, entry);
                fxj_resv = 0;
                PBCC(f, fxj_p, fxj_inv(cc));
            }
            s = ns;
            continue;
        }
        if (k == K_LOOP) {
            int live;
            fxu32 *spanf = 0;
            St ts = s, ns = s;
            /* R12--, flags; taken: R15 = R13 (dynamic) else R15++; the next byte runs either way */
            CLR(&ts); CLR(&ns);
            ns.r15++;
            live = FALL;                        /* the taken side is a dynamic jump */
            if (fxj_kind(&ns) == K_PLOT && !fxj_rsv) {
                /* "loop; plot" looping on itself (R13 == the loop's address): the whole span at once */
                fxu32 *g, *f, *z;
                SOP(S_EXTW, T1, G(13));
                g = FWD();
                fxj_p++;
                SOP(S_EXTW, T2, G(12));
                z = FWD();                      /* brne T2,0 -> keep */
                MOVK(T2, 0x10000);
                PBRC(z, fxj_p, T2, 0, 1, BR_NE);
                MOV(T0, G(1));
                MOV(T1, G(2));
                JL(fxj_spanstub);
                SOP(S_EXTW, T2, G(12));
                z = FWD();
                MOVK(T2, 0x10000);
                PBRC(z, fxj_p, T2, 0, 1, BR_NE);
                I3(A_ADD, G(1), G(1), T2);
                I3(A_SUB, G(12), G(12), T2);
                FL_SZ(G(12), FALL);
                f = FWD();                      /* b -> after the loop code below */
                fxj_w(g, enc_br(T1, LIMM, 0, BR_NE, OFF(fxj_p, g)));
                fxj_w(g + 1, (fxu32)(uint16)(s.r15 - 1));
                spanf = f;
            }
            IU(A_SUB, G(12), G(12), 1);
            FL_SZ(G(12), live);
            if (fxj_kind(&ns) == K_PLOT) {
                /* plot after loop (the classic span loop): plot, then the test */
                St d = ns;
                fxu32 *f;
                fxj_op2(&d, fxj_idx(&d), FALL);
                SOPF(S_EXTW, LIMM, G(12));
                MOV(T3, G(13));
                f = FWD();
                fxj_jump_dyn(r13 >= 0 ? r13 : (int)entry);
                PBCC(f, fxj_p, CC_EQ);
                if (spanf) PBCC(spanf, fxj_p, 0);
                ns.pipe = MEM(ns.r15); ns.r15++; CLR(&ns);
                s = ns;
                nops++;
                continue;
            }
            SOPF(S_EXTW, LIMM, G(12));
            MOV(T3, G(13));
            if (fxj_simple(&ns)) {
                St d = ns;
                fxu32 *f;
                int didx = fxj_idx(&d);
                fxj_op2(&d, didx, FALL);
                f = FWD();                      /* beq -> not taken */
                fxj_jump_dyn(r13 >= 0 ? r13 : (int)entry);
                PBCC(f, fxj_p, CC_EQ);
                ns.pipe = MEM(ns.r15); ns.r15++; CLR(&ns);
                s = ns;
                nops++;
                continue;
            } else {
                /* taken side: if R13 is the likely target, the delay op and what follows are static code
                   (the delay op's operands come from the target stream), else the interpreter */
                fxu32 *f = FWD(), *g;
                int gs = r13 >= 0 ? r13 : (int)entry;
                St t2 = ts;
                t2.r15 = gs;
                SOP(S_EXTW, T1, T3);
                EL(enc_rrr(4, A_CMP, 0, T1, LIMM, 1), (fxu32)gs);
                g = FWD();
                fxj_resv = 0;
                fxj_trace(t2, 1, 0, entry);
                fxj_resv = 0;
                PBCC(g, fxj_p, CC_NE);
                ts.dyn = 1;
                fxj_exit_raw(&ts, T3);
                PBCC(f, fxj_p, CC_EQ);
                s = ns;
                continue;
            }
        }
        if (k == K_CMODE) {
            /* POR = SREG; the screen pointers stay as they are unless height / mode / SCBR changed
               (then the interpreter does it: fx_computeScreenPointers) */
            fxu32 *f, *b1, *b2, *b3, *done;
            int a = SRC(&s, s.sreg, T3);
            MOVK(T0, 256);
            f = FWD();                          /* bbit1 a,4 -> keep 256 */
            LDA(T0, &GSU.vScreenRealHeight);
            PBRC(f, fxj_p, a, 4, 1, BB_1);
            LDA(T1, &GSU.vPrevScreenHeight);
            b1 = FWD();                         /* brne T0,T1 -> slow */
            LDA(T1, &GSU.vSCBRDirty);
            b2 = FWD();                         /* brne T1,0 -> slow */
            LDA(T1, &GSU.vMode);
            LDA(T2, &GSU.vPrevMode);
            b3 = FWD();                         /* brne T1,T2 -> slow */
            STG(a, GOFF(vPlotOptionReg), Z_W);
            STA(T0, &GSU.vScreenHeight, Z_W);
            done = FWD();
            PBRC(b1, fxj_p, T0, T1, 0, BR_NE);
            PBRC(b2, fxj_p, T1, 0, 1, BR_NE);
            PBRC(b3, fxj_p, T1, T2, 0, BR_NE);
            fxj_refund_n(fxj_resv + 1);
            fxj_exit_raw(&pre, -1);
            PBCC(done, fxj_p, 0);
            s.r15++;
            CLR(&s);
            continue;
        }
        if (k == K_IWT_R && n == 13) r13 = s.pipe | MEM(s.r15 + 1) << 8;
        else if (k == K_TO_R && n == 13 && s.b && s.sreg == 15) r13 = (uint16)s.r15;
        {
            St after = s;
            int live = FALL;
            if (kfw[k] || (k == K_FROM_R && s.b)) {
                /* flags live after this op: simulate it without emitting (cheap: no code) */
                St t = pre;
                fxu32 *save = fxj_p;
                int ok;
                (void)t;
                after = s;
                ok = fxj_op(&after, idx, FALL);  /* emitted, then rewound below */
                fxj_p = save;
                if (ok) live = after.dyn ? FALL : fxj_scan(after, 32);
                after = s;
            }
            if (!fxj_op2(&after, idx, live)) {
                fxj_refund();
                fxj_exit_raw(&pre, -1);
                return 0;
            }
            s = after;
        }
        if (k == K_IBT_R && n == 15) jumped = 1;
        if (k == K_IWT_R && n == 15) jumped = 1;
    }
}

static fxu32 *fxj_translate(int bank, unsigned a)
{
    St s;
    fxu32 *start;
    int i, again = 0;
retry:
    if (fxj_p > fxj_code + FXJ_CODE_WORDS - FXJ_ROOM || fxj_npages > FXJ_PAGES - 8 || fxj_nstubs > FXJ_MAXSTUBS - 64)
        fxj_flush();
    fxj_bank = bank;
    fxj_bk = GSU.apvRomBank[bank];
    fxj_ncold = 0;
    fxj_fail = 0;
    start = fxj_p;
    s.r15 = a + 1; s.pipe = MEM(a); s.alt = s.b = s.sreg = s.dreg = s.dyn = 0;
    fxj_resv = 0;
    fxj_trace(s, 0, 1, a);
    for (i = 0; !fxj_fail && i < fxj_ncold; i++) {
        FxjCold *c = &fxj_cold[i];
        fxu32 *h = fxj_find(bank, c->key & 0xffff);
        if (h) { PBCC(c->src, h, c->cc); continue; }
        if (fxj_nstubs >= FXJ_MAXSTUBS) { fxj_fail = 1; break; }
        PBCC(c->src, fxj_p, c->cc);
        fxj_stubs[fxj_nstubs].key = c->key;
        fxj_stubs[fxj_nstubs].stub = fxj_p;
        fxj_stubs[fxj_nstubs].src = c->src;
        fxj_stubs[fxj_nstubs].cc = c->cc;
        MOVK(T1, fxj_nstubs);
        MOVK(T0, X_STUB);
        BCC(fxj_exit, 0);
        fxj_nstubs++;
    }
    if (fxj_fail) {
        if (again) return 0;
        fxj_flush();
        again = 1;
        goto retry;
    }
    FxjStat[0]++;
    FxjStat[6] = fxj_p - fxj_code;
    return start;
}

static fxu32 *fxj_get(int bank, unsigned a)
{
    fxu32 *h = fxj_find(bank, a);
    if (!h) {
        h = fxj_translate(bank, a);
        if (!h) return 0;
        h = fxj_find(bank, a);
        fxj_sync();
    }
#ifndef FXJ_LOCK
    if (h) fxj_hput((bank << 16) | a, h);
#endif
    return h;
}

/* the interpreter state is a clean point the translator can start from */
static int fxj_cleanstate(void)
{
    return !(GSU.vStatusReg & (FLG_ALT1 | FLG_ALT2 | FLG_B)) && GSU.pvSreg == &R0 && GSU.pvDreg == &R0 &&
           GSU.vPrgBankReg < 0x60 && GSU.vPipe == GSU.pvPrgBank[(uint16)(R15 - 1)];
}

static int fxj_step(void);
#ifdef FXJ_LOCK
/* -DFXJ_LOCK: in run FXJ_LOCK (FXJ_CHECK's numbering) every block is run natively, then from the same
   state by the interpreter until it reaches the native exit state; the first difference is printed */
extern unsigned FxjChkRuns;
static int fxj_lock_on, fxj_lock_bad;
static FxRegs_s fxl_g0, fxl_g1;
static uint8 fxl_ram0[0x10000], fxl_ram1[0x10000];
static void fxj_apply(int why)
{
    if (why == X_RAW) {
        fxu32 i1 = fxj_info[1];
        R15 = fxj_info[0];
        GSU.vPipe = i1 & 0xff;
        GSU.vStatusReg = (GSU.vStatusReg & ~(FLG_ALT1 | FLG_ALT2 | FLG_B)) | ((i1 >> 8) & 3) << 8 | ((i1 >> 10) & 1) << 12;
        GSU.pvSreg = &GSU.avReg[(i1 >> 12) & 15];
        GSU.pvDreg = &GSU.avReg[(i1 >> 16) & 15];
    } else {
        fxu32 key = why == X_LOOKUP ? fxj_info[0] : fxj_stubs[fxj_info[0]].key;
        R15 = (key & 0xffff) + 1;
        GSU.vPipe = GSU.pvPrgBank[(uint16)key];
    }
}
static void fxj_lockstep(fxu32 *h)
{
    int why, i, k;
    uint16 pc0 = (uint16)(R15 - 1);
    fxl_g0 = GSU; memcpy(fxl_ram0, GSU.pvRam, 0x10000);
    why = fxj_enter(h);
    fxj_apply(why);
    fxl_g1 = GSU; memcpy(fxl_ram1, GSU.pvRam, 0x10000);
    GSU = fxl_g0; memcpy(GSU.pvRam, fxl_ram0, 0x10000);
    if (fxj_rsv) {
        /* counted runs: the native code took exactly its op count from vCounter */
        uint32 nn = fxl_g0.vCounter - fxl_g1.vCounter;
        for (k = 0; k < (int)nn && TF(G); k++) FX_STEP;
        GSU.vCounter = fxl_g1.vCounter;
    } else
    for (k = 0; k < 4000; k++) {
        if ((uint16)R15 == (uint16)fxl_g1.avReg[15] && GSU.vPipe == fxl_g1.vPipe && GSU.pvSreg == fxl_g1.pvSreg &&
            GSU.pvDreg == fxl_g1.pvDreg && (GSU.vStatusReg & 0x1300) == (fxl_g1.vStatusReg & 0x1300) && (k || why == X_RAW)) break;
        if (!TF(G)) break;
        FX_STEP;
    }
    {
        int bad = 0;
        for (i = 0; i < 15; i++) if ((uint16)GSU.avReg[i] != (uint16)fxl_g1.avReg[i]) bad |= 1;
        if (!(GSU.vSign & 0x8000) != !(fxl_g1.vSign & 0x8000)) bad |= 2;
        if (!(uint16)GSU.vZero != !(uint16)fxl_g1.vZero) bad |= 4;
        if (!GSU.vCarry != !fxl_g1.vCarry) bad |= 8;
        if (!GSU.vOverflow != !fxl_g1.vOverflow) bad |= 16;
        if ((uint16)GSU.vLastRamAdr != (uint16)fxl_g1.vLastRamAdr || GSU.vColorReg != fxl_g1.vColorReg) bad |= 32;
        if (k == 4000 || (uint16)R15 != (uint16)fxl_g1.avReg[15]) bad |= 64;
        if (memcmp(GSU.pvRam, fxl_ram1, 0x10000)) bad |= 128;
        if (bad) {
            printf("LOCK run %u pc %02x:%04x why %d steps %d bad %x op %02x %02x %02x %02x\n", FxjChkRuns, (unsigned)GSU.vPrgBankReg, pc0, why, k, bad,
                   GSU.pvPrgBank[pc0], GSU.pvPrgBank[(uint16)(pc0 + 1)], GSU.pvPrgBank[(uint16)(pc0 + 2)], GSU.pvPrgBank[(uint16)(pc0 + 3)]);
            for (i = 0; i < 16; i++) printf(" %04x/%04x", (uint16)fxl_g0.avReg[i], (uint16)GSU.avReg[i] ^ (uint16)fxl_g1.avReg[i]);
            printf("\n flags int %x %x %x %x jit %x %x %x %x\n", GSU.vSign, GSU.vZero, GSU.vCarry, GSU.vOverflow, fxl_g1.vSign, fxl_g1.vZero, fxl_g1.vCarry, fxl_g1.vOverflow);
            { fxu32 *q = h; printf(" code"); for (i = 0; i < 24; i++) printf(" %08x", (q[i] << 16) | (q[i] >> 16)); printf(" rom %08x pv %08x\n", (unsigned)GSU.pvRomBank, (unsigned)fxl_g1.pvRomBank); }
            for (i = 0; i < 0x10000; i++) if (GSU.pvRam[i] != fxl_ram1[i]) { printf(" ram %04x int %02x jit %02x\n", i, GSU.pvRam[i], fxl_ram1[i]); break; }
            if (++fxj_lock_bad >= 3) fxj_lock_on = 0;
        }
    }
    if (why == X_RAW) fxj_step();   /* keep going like the normal loop (from the interpreter's state) */
}
#endif

/* one interpreter step the way fx_run counts them; 0: the run's op budget is used up */
static int fxj_step(void)
{
    if (GSU.vCounter-- > 0) { FX_STEP; FxjStat[4]++; return 1; }
    return 0;
}

static int fxj_usable(uint32 n)
{
    if (!fxj_ready) { fxj_kinds(); fxj_init(); fxj_counted = Settings.WinterGold && !Settings.StarfoxHack;
#ifdef FXJ_LOCK
                      fxj_rsv = 1;
#else
                      fxj_rsv = fxj_counted;
#endif
                    }
#ifdef FXJ_NOCOUNTED
    return n == ~0u && !fxj_counted;           /* counted runs in the interpreter */
#else
    return n == ~0u || fxj_counted;
#endif
}

static int fxj_trok;                            /* counted runs: may translate at the current point */
static uint32 fxj_runloop(void)
{
    /* a run that starts where the last one ran out of budget resumes mid-program */
    fxj_trok = !fxj_counted || GSU.vPipe == 1;
    fxj_plotfn = fxj_plot[GSU.vMode & 3];
#ifdef FXJ_LOCK
    fxj_lock_on = !fxj_lock_bad && FxjChkRuns + 1 >= FXJ_LOCK;
#endif
    while (TF(G)) {
        fxu32 *h;
        int why;
        if (!fxj_cleanstate()) { if (!fxj_step()) break; continue; }
        /* counted runs: no native entry for the last few ops of the budget (a reservation would fail),
           and no new translations where a run merely resumed (budget-cut points are arbitrary addresses:
           translating at each would flood the code buffer); jump targets are translated as usual */
        if (fxj_counted && GSU.vCounter < 40) { if (!fxj_step()) break; continue; }
        if (fxj_counted && !fxj_trok) {
            h = fxj_find(GSU.vPrgBankReg, (uint16)(R15 - 1));
            if (h) fxj_hput((GSU.vPrgBankReg << 16) | (uint16)(R15 - 1), h);
        } else
            h = fxj_get(GSU.vPrgBankReg, (uint16)(R15 - 1));
        if (!h) { if (!fxj_step()) break; continue; }
#ifdef FXJ_LOCK
        if (fxj_lock_on) { fxj_lockstep(h); continue; }
#endif
        why = fxj_enter(h);
        FxjStat[why]++;
        if (why == X_RAW) {
            fxu32 i1 = fxj_info[1];
            FxjRawOp[(i1 & 0xff) | ((i1 >> 8) & 3) << 8]++;
#ifdef FXJ_RAWDBG
            { static int nd; if (((i1 & 0xff) | ((i1 >> 8) & 3) << 8) == FXJ_RAWDBG && nd++ < 6) printf("RAWDBG r15 %02x:%04x i1 %x\n", (unsigned)GSU.vPrgBankReg, fxj_info[0], i1); }
#endif
            R15 = fxj_info[0];
            GSU.vPipe = i1 & 0xff;
            GSU.vStatusReg = (GSU.vStatusReg & ~(FLG_ALT1 | FLG_ALT2 | FLG_B)) | ((i1 >> 8) & 3) << 8 |
                             ((i1 >> 10) & 1) << 12;
            GSU.pvSreg = &GSU.avReg[(i1 >> 12) & 15];
            GSU.pvDreg = &GSU.avReg[(i1 >> 16) & 15];
            if (!fxj_step()) break;
        } else if (why == X_LOOKUP) {
            fxj_trok = 1;
            fxu32 key = fxj_info[0];
            R15 = (key & 0xffff) + 1;
            GSU.vPipe = GSU.pvPrgBank[(uint16)key];
        } else {
            FxjStub *st = &fxj_stubs[fxj_info[0]];
            fxu32 key = st->key, *t;
            R15 = (key & 0xffff) + 1;
            GSU.vPipe = GSU.pvPrgBank[(uint16)key];
            fxj_trok = 1;
            t = fxj_get(key >> 16, key & 0xffff);
            /* fxj_get may have flushed everything (then st is stale) */
#ifndef FXJ_LOCK
            if (t && fxj_info[0] < (fxu32)fxj_nstubs && st->key == key && st->stub >= fxj_base && st->stub < fxj_p) {
                fxj_w(st->stub, enc_b(OFF(t, st->stub)));
                if (st->src) PBCC(st->src, t, st->cc);
                fxj_sync();
            }
#endif
        }
    }
    return 0;
}
