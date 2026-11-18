/* fxjit_enc.h: ARCompact (ARC600) 32-bit instruction encoders for the GSU translator (fxjit.c).
   Every value here is the instruction as the manual writes it (bits 31..0); fxj_emit() stores it in
   memory order (upper half first). Checked against GNU as by kern/fxenctest.c (tmp/enctest.sh). */
#ifndef FXJIT_ENC_H
#define FXJIT_ENC_H
typedef unsigned int fxu32;

#define ARC_LIMM 62
/* major 4 sub-opcodes */
enum { A_ADD = 0x00, A_ADC = 0x01, A_SUB = 0x02, A_SBC = 0x03, A_AND = 0x04, A_OR = 0x05, A_BIC = 0x06,
       A_XOR = 0x07, A_MAX = 0x08, A_MIN = 0x09, A_MOV = 0x0a, A_TST = 0x0b, A_CMP = 0x0c, A_RCMP = 0x0d,
       A_RSUB = 0x0e, A_BSET = 0x0f, A_BCLR = 0x10, A_BTST = 0x11, A_BXOR = 0x12, A_BMSK = 0x13,
       A_ADD1 = 0x14, A_ADD2 = 0x15, A_ADD3 = 0x16, A_SUB1 = 0x17, A_SUB2 = 0x18, A_SUB3 = 0x19,
       A_J = 0x20, A_JD = 0x21, A_JL = 0x22, A_JLD = 0x23, A_SR = 0x2b, A_SOP = 0x2f };
/* major 4 single-operand (sub 0x2f, A field) */
enum { S_ASL = 0x00, S_ASR = 0x01, S_LSR = 0x02, S_ROR = 0x03, S_RRC = 0x04, S_SEXB = 0x05, S_SEXW = 0x06,
       S_EXTB = 0x07, S_EXTW = 0x08, S_ABS = 0x09, S_NOT = 0x0a, S_RLC = 0x0b };
/* major 5 (barrel shifter, mul64) */
enum { X_ASL = 0x00, X_LSR = 0x01, X_ASR = 0x02, X_ROR = 0x03, X_MUL64 = 0x04, X_MULU64 = 0x05 };
/* condition codes */
enum { CC_AL = 0, CC_EQ = 1, CC_NE = 2, CC_PL = 3, CC_MI = 4, CC_CS = 5, CC_CC = 6, CC_VS = 7, CC_VC = 8,
       CC_GT = 9, CC_GE = 10, CC_LT = 11, CC_LE = 12, CC_HI = 13, CC_LS = 14 };
/* BRcc conditions */
enum { BR_EQ = 0, BR_NE = 1, BR_LT = 2, BR_GE = 3, BR_LO = 4, BR_HS = 5, BB_0 = 14, BB_1 = 15 };
/* load/store sizes */
enum { Z_W = 0, Z_B = 1, Z_H = 2 };

#define ENC_B(b) ((((fxu32)(b) & 7) << 24) | ((((fxu32)(b) >> 3) & 7) << 12))
/* op a,b,c */
static inline fxu32 enc_rrr(int maj, int sub, int a, int b, int c, int f)
{ return ((fxu32)maj << 27) | ENC_B(b) | ((fxu32)sub << 16) | ((fxu32)f << 15) | ((fxu32)c << 6) | (fxu32)a; }
/* op a,b,u6 */
static inline fxu32 enc_rru(int maj, int sub, int a, int b, int u6, int f)
{ return ((fxu32)maj << 27) | ENC_B(b) | (1u << 22) | ((fxu32)sub << 16) | ((fxu32)f << 15) | ((fxu32)(u6 & 63) << 6) | (fxu32)a; }
/* op b,b,s12 */
static inline fxu32 enc_rs12(int maj, int sub, int b, int s12, int f)
{ return ((fxu32)maj << 27) | ENC_B(b) | (2u << 22) | ((fxu32)sub << 16) | ((fxu32)f << 15) | ((fxu32)(s12 & 63) << 6) | (((fxu32)s12 >> 6) & 63); }
/* op.cc b,b,c (u6 when isu6) */
static inline fxu32 enc_cc(int maj, int sub, int b, int c, int isu6, int cc, int f)
{ return ((fxu32)maj << 27) | ENC_B(b) | (3u << 22) | ((fxu32)sub << 16) | ((fxu32)f << 15) | ((fxu32)(c & 63) << 6) | ((fxu32)isu6 << 5) | (fxu32)cc; }
/* single operand: op b,c / op b,u6 */
static inline fxu32 enc_sop(int maj, int s2, int b, int c, int f)
{ return enc_rrr(maj, A_SOP, s2, b, c, f); }
static inline fxu32 enc_sopu(int maj, int s2, int b, int u6, int f)
{ return enc_rru(maj, A_SOP, s2, b, u6, f); }
/* mov b,c / mov b,s12 / mov.cc b,c */
static inline fxu32 enc_mov(int b, int c) { return enc_rrr(4, A_MOV, 0, b, c, 0); }
static inline fxu32 enc_movi(int b, int s12) { return enc_rs12(4, A_MOV, b, s12, 0); }
/* ld a,[b,s9] (zz size, x sign-extend, di) */
static inline fxu32 enc_ld(int a, int b, int s9, int zz, int x, int di)
{ return (2u << 27) | ENC_B(b) | (((fxu32)s9 & 0xff) << 16) | ((((fxu32)s9 >> 8) & 1) << 15) | ((fxu32)di << 11) | ((fxu32)zz << 7) | ((fxu32)x << 6) | (fxu32)a; }
/* ld a,[b,c] */
static inline fxu32 enc_ldr(int a, int b, int c, int zz, int x, int di)
{ return (4u << 27) | ENC_B(b) | (6u << 19) | ((fxu32)zz << 17) | ((fxu32)x << 16) | ((fxu32)di << 15) | ((fxu32)c << 6) | (fxu32)a; }
/* st c,[b,s9] */
static inline fxu32 enc_st(int c, int b, int s9, int zz, int di)
{ return (3u << 27) | ENC_B(b) | (((fxu32)s9 & 0xff) << 16) | ((((fxu32)s9 >> 8) & 1) << 15) | ((fxu32)c << 6) | ((fxu32)di << 5) | ((fxu32)zz << 1); }
/* bcc s21 (offset from the branch's 4-aligned pc) */
static inline fxu32 enc_bcc(int off, int cc)
{ return ((((fxu32)off >> 1) & 0x3ff) << 17) | ((((fxu32)off >> 11) & 0x3ff) << 6) | (fxu32)cc; }
/* b s25 */
static inline fxu32 enc_b(int off)
{ return ((((fxu32)off >> 1) & 0x3ff) << 17) | (1u << 16) | ((((fxu32)off >> 11) & 0x3ff) << 6) | (((fxu32)off >> 21) & 15); }
/* bl s25 */
static inline fxu32 enc_bl(int off)
{ return (1u << 27) | ((((fxu32)off >> 2) & 0x1ff) << 18) | (1u << 17) | ((((fxu32)off >> 11) & 0x3ff) << 6) | (((fxu32)off >> 21) & 15); }
/* brcc b,c,s9 / brcc b,u6,s9 / bbit0/1 b,u6,s9 */
static inline fxu32 enc_br(int b, int c, int isu6, int cc, int off)
{ return (1u << 27) | ENC_B(b) | ((((fxu32)off >> 1) & 0x7f) << 17) | (1u << 16) | ((((fxu32)off >> 8) & 1) << 15) | ((fxu32)(c & 63) << 6) | ((fxu32)isu6 << 4) | (fxu32)cc; }
/* j [c] / jl [c] (c = ARC_LIMM: absolute address follows) */
static inline fxu32 enc_j(int c) { return enc_rrr(4, A_J, 0, 0, c, 0); }
static inline fxu32 enc_jl(int c) { return enc_rrr(4, A_JL, 0, 0, c, 0); }
/* sr b,[u6] */
static inline fxu32 enc_sru(int b, int u6) { return enc_rru(4, A_SR, 0, b, u6, 0); }
#endif
