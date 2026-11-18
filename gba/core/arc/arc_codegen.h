/* gameplaySP ARC (ARCompact, ARC600) code generator: instruction encoders.
 *
 * 16-bit forms are emitted where the operands allow (MOV_S, ADD_S/SUB_S;
 * smaller code: generated code mostly waits on instruction fetch), so code
 * is only 2-byte aligned: branch offsets count from the instruction's PCL
 * (its address with bit 1 clear), and arc_emit_align4 pads with NOP_S where
 * a data word follows (memory calls, branch exits) and at a block's end.
 * ARCompact stores a 32-bit instruction or long immediate as two 16-bit
 * halves, high half first.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef ARC_CODEGEN_H
#define ARC_CODEGEN_H

typedef enum
{
  arc_reg_r0 = 0, arc_reg_r1, arc_reg_r2, arc_reg_r3, arc_reg_r4,
  arc_reg_r5, arc_reg_r6, arc_reg_r7, arc_reg_r8, arc_reg_r9,
  arc_reg_r10, arc_reg_r11, arc_reg_r12, arc_reg_r13, arc_reg_r14,
  arc_reg_r15, arc_reg_r16, arc_reg_r17, arc_reg_r18, arc_reg_r19,
  arc_reg_r20, arc_reg_r21, arc_reg_r22, arc_reg_r23, arc_reg_r24,
  arc_reg_r25, arc_reg_gp, arc_reg_fp, arc_reg_sp, arc_reg_ilink1,
  arc_reg_ilink2, arc_reg_blink,
  arc_reg_mlo = 57, arc_reg_mmid = 58, arc_reg_mhi = 59,
  arc_reg_limm = 62
} arc_reg_number;

/* condition codes */
#define ARC_AL 0x00
#define ARC_EQ 0x01
#define ARC_NE 0x02
#define ARC_PL 0x03
#define ARC_MI 0x04
#define ARC_CS 0x05  /* carry set (after SUB/CMP: borrow, unsigned lower) */
#define ARC_CC 0x06  /* carry clear */
#define ARC_VS 0x07
#define ARC_VC 0x08
#define ARC_GT 0x09
#define ARC_GE 0x0A
#define ARC_LT 0x0B
#define ARC_LE 0x0C
#define ARC_HI 0x0D
#define ARC_LS 0x0E

/* major 4 sub-opcodes */
#define ARC_ADD  0x00
#define ARC_ADC  0x01
#define ARC_SUB  0x02
#define ARC_SBC  0x03
#define ARC_AND  0x04
#define ARC_OR   0x05
#define ARC_BIC  0x06
#define ARC_XOR  0x07
#define ARC_MAX  0x08
#define ARC_MIN  0x09
#define ARC_MOV  0x0A
#define ARC_TST  0x0B
#define ARC_CMP  0x0C
#define ARC_RCMP 0x0D
#define ARC_RSUB 0x0E
#define ARC_BSET 0x0F
#define ARC_BCLR 0x10
#define ARC_BTST 0x11
#define ARC_BXOR 0x12
#define ARC_BMSK 0x13
#define ARC_ADD1 0x14
#define ARC_ADD2 0x15
#define ARC_ADD3 0x16
#define ARC_J    0x20
#define ARC_JL   0x22
#define ARC_SR   0x2B
#define ARC_SOP  0x2F
/* single-operand (major 4, sub 0x2F) */
#define ARC_SOP_ASL1 0x00
#define ARC_SOP_ASR1 0x01
#define ARC_SOP_LSR1 0x02
#define ARC_SOP_ROR1 0x03
#define ARC_SOP_RRC  0x04
#define ARC_SOP_SEXB 0x05
#define ARC_SOP_SEXW 0x06
#define ARC_SOP_EXTB 0x07
#define ARC_SOP_EXTW 0x08
#define ARC_SOP_NOT  0x0A
#define ARC_SOP_RLC  0x0B
/* major 5 sub-opcodes (barrel shifter, multiplier) */
#define ARC_ASL    0x00
#define ARC_LSR    0x01
#define ARC_ASR    0x02
#define ARC_ROR    0x03
#define ARC_MUL64  0x04
#define ARC_MULU64 0x05

#define arc_emit_raw32(value)                                                 \
{                                                                             \
  u32 v__ = (u32)(value);                                                     \
  *((u16 *)translation_ptr) = (u16)(v__ >> 16);                               \
  *((u16 *)(translation_ptr + 2)) = (u16)v__;                                 \
  translation_ptr += 4;                                                       \
}                                                                             \

#define arc_emit_raw16(value)                                                 \
{                                                                             \
  *((u16 *)translation_ptr) = (u16)(value);                                   \
  translation_ptr += 2;                                                       \
}                                                                             \

/* NOP_S to a 4-byte boundary */
#define arc_emit_align4()                                                     \
  if(((u32)translation_ptr) & 2)                                              \
  {                                                                           \
    arc_emit_raw16(0x78E0);                                                   \
  }                                                                           \

/* r0-r3, r12-r15: the registers 16-bit forms name (as 0-7) */
#define arc_compact(r) (((u32)(r) < 4) || (((r) >= 12) && ((r) < 16)))
#define arc_cc(r) ((r) < 4 ? (r) : (r) - 8)

/* A data word (plain little-endian, for ld) */
#define arc_emit_data32(value)                                                \
{                                                                             \
  *((u32 *)translation_ptr) = (u32)(value);                                   \
  translation_ptr += 4;                                                       \
}                                                                             \

#define arc_read_raw32(ptr)                                                   \
  ((((u32)*((u16 *)(ptr))) << 16) | *((u16 *)((u8 *)(ptr) + 2)))             \

#define arc_write_raw32(ptr, value)                                           \
{                                                                             \
  u32 v__ = (u32)(value);                                                     \
  *((u16 *)(ptr)) = (u16)(v__ >> 16);                                         \
  *((u16 *)((u8 *)(ptr) + 2)) = (u16)v__;                                     \
}                                                                             \

#define arc_fmt_b(major, b)                                                   \
  (((major) << 27) | (((b) & 7) << 24) | ((((b) >> 3) & 7) << 12))            \

/* a = b op c */
#define arc_emit_op_rr(major, sub, f, a, b, c)                                \
  arc_emit_raw32(arc_fmt_b(major, b) | ((sub) << 16) | ((f) << 15) |         \
   ((c) << 6) | (a))                                                          \

/* a = b op u6 */
#define arc_emit_op_ru6(major, sub, f, a, b, u6)                              \
  arc_emit_raw32(arc_fmt_b(major, b) | (1 << 22) | ((sub) << 16) |           \
   ((f) << 15) | (((u6) & 0x3F) << 6) | (a))                                  \

/* b = b op s12 */
#define arc_emit_op_rs12(major, sub, f, b, s12)                               \
  arc_emit_raw32(arc_fmt_b(major, b) | (2 << 22) | ((sub) << 16) |           \
   ((f) << 15) | (((s12) & 0x3F) << 6) | (((s12) >> 6) & 0x3F))             \

/* if cc: b = b op c */
#define arc_emit_op_cc_rr(major, sub, f, cc, b, c)                            \
  arc_emit_raw32(arc_fmt_b(major, b) | (3 << 22) | ((sub) << 16) |           \
   ((f) << 15) | ((c) << 6) | (cc))                                           \

/* if cc: b = b op u6 */
#define arc_emit_op_cc_ru6(major, sub, f, cc, b, u6)                          \
  arc_emit_raw32(arc_fmt_b(major, b) | (3 << 22) | ((sub) << 16) |           \
   ((f) << 15) | (((u6) & 0x3F) << 6) | (1 << 5) | (cc))                      \

/* a = b op limm */
#define arc_emit_op_rl(major, sub, f, a, b, imm)                              \
  arc_emit_op_rr(major, sub, f, a, b, arc_reg_limm);                          \
  arc_emit_raw32(imm)                                                         \

/* a = limm op c */
#define arc_emit_op_lr(major, sub, f, a, imm, c)                              \
  arc_emit_op_rr(major, sub, f, a, arc_reg_limm, c);                          \
  arc_emit_raw32(imm)                                                         \

/* Generic ALU op with any immediate: picks ADD_S/SUB_S, u6, s12 (a == b) or limm */
#define arc_emit_op_ri(major, sub, f, a, b, imm)                              \
{                                                                             \
  s32 i__ = (s32)(imm);                                                       \
  if(((major) == 4) && !(f) && (((sub) == ARC_ADD) || ((sub) == ARC_SUB)) &&  \
     arc_compact(a) && arc_compact(b) && (i__ >= 0) && (i__ < 8))             \
  {                                                                           \
    arc_emit_raw16(((sub) == ARC_ADD ? 0x6800 : 0x6808) | (arc_cc(b) << 8) |  \
     (arc_cc(a) << 5) | i__);                                                 \
  }                                                                           \
  else if(((major) == 4) && !(f) && ((sub) == ARC_ADD) && ((a) == (b)) &&     \
     arc_compact(a) && (i__ >= 0) && (i__ < 128))                             \
  {                                                                           \
    arc_emit_raw16(0xE000 | (arc_cc(b) << 8) | i__);                          \
  }                                                                           \
  else if((i__ >= 0) && (i__ < 64))                                           \
  {                                                                           \
    arc_emit_op_ru6(major, sub, f, a, b, i__);                                \
  }                                                                           \
  else if(((a) == (b)) && (i__ >= -2048) && (i__ < 2048))                     \
  {                                                                           \
    arc_emit_op_rs12(major, sub, f, b, i__);                                  \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arc_emit_op_rl(major, sub, f, a, b, (u32)i__);                            \
  }                                                                           \
}                                                                             \

/* Single operand: b = op(c) */
#define arc_emit_sop_r(sop, f, b, c)                                          \
  arc_emit_raw32(arc_fmt_b(4, b) | (ARC_SOP << 16) | ((f) << 15) |           \
   ((c) << 6) | (sop))                                                        \

/* MOV b, c / MOV b, imm (mov has no a field: dest is b); MOV_S b,h or
   MOV_S h,b when either register is a compact one */
#define arc_emit_mov(f, b, c)                                                 \
{                                                                             \
  if(!(f) && ((c) != arc_reg_limm) && arc_compact(b))                         \
  {                                                                           \
    arc_emit_raw16(0x7008 | (arc_cc(b) << 8) | (((c) & 7) << 5) | ((c) >> 3)); \
  }                                                                           \
  else if(!(f) && ((b) != arc_reg_limm) && arc_compact(c))                    \
  {                                                                           \
    arc_emit_raw16(0x7018 | (arc_cc(c) << 8) | (((b) & 7) << 5) | ((b) >> 3)); \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arc_emit_raw32(arc_fmt_b(4, b) | (ARC_MOV << 16) | ((f) << 15) |         \
     ((c) << 6));                                                             \
  }                                                                           \
}                                                                             \

#define arc_emit_mov_imm(b, imm)                                              \
{                                                                             \
  s32 i__ = (s32)(imm);                                                       \
  if(arc_compact(b) && (i__ >= 0) && (i__ < 256))                             \
  {                                                                           \
    arc_emit_raw16(0xD800 | (arc_cc(b) << 8) | i__);                          \
  }                                                                           \
  else if((i__ >= -2048) && (i__ < 2048))                                     \
  {                                                                           \
    arc_emit_op_rs12(4, ARC_MOV, 0, b, i__);                                  \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arc_emit_raw32(arc_fmt_b(4, b) | (ARC_MOV << 16) | (arc_reg_limm << 6));  \
    arc_emit_raw32((u32)i__);                                                 \
  }                                                                           \
}                                                                             \

/* mov.cc b, u6 / mov.cc b, c */
#define arc_emit_mov_cc_u6(cc, b, u6)                                         \
  arc_emit_op_cc_ru6(4, ARC_MOV, 0, cc, b, u6)                                \

#define arc_emit_mov_cc(cc, b, c)                                             \
  arc_emit_op_cc_rr(4, ARC_MOV, 0, cc, b, c)                                  \

/* Compare-type (no destination): op b, c / op b, imm */
#define arc_emit_cmp_rr(sub, b, c)                                            \
  arc_emit_raw32(arc_fmt_b(4, b) | ((sub) << 16) | (1 << 15) | ((c) << 6))    \

#define arc_emit_cmp_ri(sub, b, imm)                                          \
{                                                                             \
  s32 i__ = (s32)(imm);                                                       \
  if((i__ >= 0) && (i__ < 64))                                                \
  {                                                                           \
    arc_emit_raw32(arc_fmt_b(4, b) | (1 << 22) | ((sub) << 16) | (1 << 15) |  \
     ((i__ & 0x3F) << 6));                                                    \
  }                                                                           \
  else if((i__ >= -2048) && (i__ < 2048))                                     \
  {                                                                           \
    arc_emit_op_rs12(4, sub, 1, b, i__);                                      \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arc_emit_raw32(arc_fmt_b(4, b) | ((sub) << 16) | (1 << 15) |             \
     (arc_reg_limm << 6));                                                    \
    arc_emit_raw32((u32)i__);                                                 \
  }                                                                           \
}                                                                             \

/* Loads: zz 0 = word, 1 = byte, 2 = halfword; x = sign extend */
#define arc_emit_ld_s9(zz, x, a, b, s9)                                       \
  arc_emit_raw32((2 << 27) | (((b) & 7) << 24) | (((s9) & 0xFF) << 16) |      \
   ((((s9) >> 8) & 1) << 15) | ((((b) >> 3) & 7) << 12) | ((zz) << 7) |       \
   ((x) << 6) | (a))                                                          \

/* ld a, [b, c] */
#define arc_emit_ld_rr(zz, x, a, b, c)                                        \
  arc_emit_raw32(arc_fmt_b(4, b) | (6 << 19) | ((zz) << 17) | ((x) << 16) |   \
   ((c) << 6) | (a))                                                          \

/* ld a, [limm] */
#define arc_emit_ld_abs(zz, x, a, addr)                                       \
  arc_emit_ld_s9(zz, x, a, arc_reg_limm, 0);                                  \
  arc_emit_raw32((u32)(addr))                                                 \

#define arc_emit_st_s9(zz, c, b, s9)                                          \
  arc_emit_raw32((3 << 27) | (((b) & 7) << 24) | (((s9) & 0xFF) << 16) |      \
   ((((s9) >> 8) & 1) << 15) | ((((b) >> 3) & 7) << 12) | ((c) << 6) |        \
   ((zz) << 1))                                                               \

/* st c, [limm] */
#define arc_emit_st_abs(zz, c, addr)                                          \
  arc_emit_st_s9(zz, c, arc_reg_limm, 0);                                     \
  arc_emit_raw32((u32)(addr))                                                 \

/* Branch offsets are relative to PCL (the branch's address, bit 1 clear) */
#define arc_rel(from, to) ((s32)((u8 *)(to) - (u8 *)(((u32)(from)) & ~3U)))

#define arc_enc_bcc(off, cc)                                                  \
  ((((((u32)(off)) >> 1) & 0x3FF) << 17) |                                    \
   (((((u32)(off)) >> 11) & 0x3FF) << 6) | (cc))                              \

#define arc_enc_b(off)                                                        \
  ((((((u32)(off)) >> 1) & 0x3FF) << 17) | (1 << 16) |                        \
   (((((u32)(off)) >> 11) & 0x3FF) << 6) | ((((u32)(off)) >> 21) & 0xF))      \

#define arc_enc_bl(off)                                                       \
  ((1 << 27) | (((((u32)(off)) >> 2) & 0x1FF) << 18) | (1 << 17) |           \
   (((((u32)(off)) >> 11) & 0x3FF) << 6) | ((((u32)(off)) >> 21) & 0xF))      \

#define arc_enc_blcc(off, cc)                                                 \
  ((1 << 27) | (((((u32)(off)) >> 2) & 0x1FF) << 18) |                        \
   (((((u32)(off)) >> 11) & 0x3FF) << 6) | (cc))                              \

/* Bcc to an address (+-1 MB) */
#define arc_emit_bcc(cc, target)                                              \
  arc_emit_raw32(arc_enc_bcc(arc_rel(translation_ptr, target), cc))           \

/* B to an address (+-16 MB) */
#define arc_emit_b(target)                                                    \
  arc_emit_raw32(arc_enc_b(arc_rel(translation_ptr, target)))                 \

#define arc_emit_bl(target)                                                   \
  arc_emit_raw32(arc_enc_bl(arc_rel(translation_ptr, target)))                \

#define arc_emit_blcc(cc, target)                                             \
  arc_emit_raw32(arc_enc_blcc(arc_rel(translation_ptr, target), cc))          \

/* Branch emitted with a zero offset, to be patched later */
#define arc_emit_bcc_filler(cc, writeback)                                    \
  (writeback) = translation_ptr;                                              \
  arc_emit_raw32(arc_enc_bcc(0, cc))                                          \

#define arc_emit_b_filler(writeback)                                          \
  (writeback) = translation_ptr;                                              \
  arc_emit_raw32(arc_enc_b(0))                                                \

/* j [c], jl [c], j limm, jl limm */
#define arc_emit_j_r(c)                                                       \
  arc_emit_raw32(arc_fmt_b(4, 0) | (ARC_J << 16) | ((c) << 6))                \

#define arc_emit_jl_r(c)                                                      \
  arc_emit_raw32(arc_fmt_b(4, 0) | (ARC_JL << 16) | ((c) << 6))               \

#define arc_emit_j_abs(addr)                                                  \
  arc_emit_j_r(arc_reg_limm);                                                 \
  arc_emit_raw32((u32)(addr))                                                 \

#define arc_emit_jl_abs(addr)                                                 \
  arc_emit_jl_r(arc_reg_limm);                                                \
  arc_emit_raw32((u32)(addr))                                                 \

#define arc_emit_nop()                                                        \
  arc_emit_raw32(0x264A7000)  /* mov 0,0 */                                   \

/* Common shorthands (a = b op c / imm) */
#define arc_emit_add(a, b, c)  arc_emit_op_rr(4, ARC_ADD, 0, a, b, c)
#define arc_emit_sub(a, b, c)  arc_emit_op_rr(4, ARC_SUB, 0, a, b, c)
#define arc_emit_and(a, b, c)  arc_emit_op_rr(4, ARC_AND, 0, a, b, c)
#define arc_emit_or(a, b, c)   arc_emit_op_rr(4, ARC_OR, 0, a, b, c)
#define arc_emit_xor(a, b, c)  arc_emit_op_rr(4, ARC_XOR, 0, a, b, c)
#define arc_emit_bic(a, b, c)  arc_emit_op_rr(4, ARC_BIC, 0, a, b, c)
#define arc_emit_addi(a, b, i) arc_emit_op_ri(4, ARC_ADD, 0, a, b, i)
#define arc_emit_subi(a, b, i) arc_emit_op_ri(4, ARC_SUB, 0, a, b, i)
#define arc_emit_andi(a, b, i) arc_emit_op_ri(4, ARC_AND, 0, a, b, i)
#define arc_emit_ori(a, b, i)  arc_emit_op_ri(4, ARC_OR, 0, a, b, i)
#define arc_emit_xori(a, b, i) arc_emit_op_ri(4, ARC_XOR, 0, a, b, i)
#define arc_emit_asl(a, b, c)  arc_emit_op_rr(5, ARC_ASL, 0, a, b, c)
#define arc_emit_lsr(a, b, c)  arc_emit_op_rr(5, ARC_LSR, 0, a, b, c)
#define arc_emit_asr(a, b, c)  arc_emit_op_rr(5, ARC_ASR, 0, a, b, c)
#define arc_emit_ror(a, b, c)  arc_emit_op_rr(5, ARC_ROR, 0, a, b, c)
#define arc_emit_asli(a, b, i) arc_emit_op_ru6(5, ARC_ASL, 0, a, b, i)
#define arc_emit_lsri(a, b, i) arc_emit_op_ru6(5, ARC_LSR, 0, a, b, i)
#define arc_emit_asri(a, b, i) arc_emit_op_ru6(5, ARC_ASR, 0, a, b, i)
#define arc_emit_rori(a, b, i) arc_emit_op_ru6(5, ARC_ROR, 0, a, b, i)
#define arc_emit_bmski(a, b, i) arc_emit_op_ru6(4, ARC_BMSK, 0, a, b, i)
#define arc_emit_bclri(a, b, i) arc_emit_op_ru6(4, ARC_BCLR, 0, a, b, i)
#define arc_emit_bseti(a, b, i) arc_emit_op_ru6(4, ARC_BSET, 0, a, b, i)
#define arc_emit_cmp(b, c)     arc_emit_cmp_rr(ARC_CMP, b, c)
#define arc_emit_cmpi(b, i)    arc_emit_cmp_ri(ARC_CMP, b, i)
#define arc_emit_tst(b, c)     arc_emit_cmp_rr(ARC_TST, b, c)
#define arc_emit_tsti(b, i)    arc_emit_cmp_ri(ARC_TST, b, i)
#define arc_emit_btsti(b, i)   arc_emit_cmp_ri(ARC_BTST, b, i)
#define arc_emit_ld(a, b, s9)  arc_emit_ld_s9(0, 0, a, b, s9)
#define arc_emit_st(c, b, s9)  arc_emit_st_s9(0, c, b, s9)
#define arc_emit_not(b, c)     arc_emit_sop_r(ARC_SOP_NOT, 0, b, c)

#endif
