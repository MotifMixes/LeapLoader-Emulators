/* gameplaySP ARC (ARCompact, ARC600) dynarec emitter.
 *
 * Structure follows mips/mips_emit.h (GBA registers kept in host registers,
 * flags kept as 0/1 values in four registers), but there are no delay slots
 * and memory accesses go through the stubs in arc_stub.S.
 *
 * Host registers:
 *   r0-r2  a0-a2 (arguments, scratch; GBA "virtual" regs 15-17)
 *   r3     rv (stub return value), r4 temp, r12 temp2
 *   r5     reg[] base, r6 cycle counter
 *   r7-r10 N, Z, C, V (0/1)
 *   r11    GBA r14
 *   r13-r25 GBA r0-r12, r27 (fp) GBA r13
 * r13-r25 and r27 are callee-saved in the ARC ABI; the stubs save r5-r11
 * around C calls. r26 (gp) belongs to the BIOS and is never touched.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef ARC_EMIT_H
#define ARC_EMIT_H

#include "arc/arc_codegen.h"

/* Stubs (arc_stub.S). Jump targets, not callable from C. */
void arc_update_gba(u32 pc);
void arc_update_gba_w(void);
void arc_indirect_branch_arm(u32 address);
void arc_indirect_branch_thumb(u32 address);
void arc_indirect_branch_dual(u32 address);
void arc_cheat_hook(void);
void arc_hle_div(void);
void arc_hle_div_arm(void);
void execute_vram_arm(u32 address);
void execute_vram_thumb(u32 address);
u32 execute_read_cpsr(void);
u32 execute_read_spsr(void);
void execute_swi(u32 pc);
u32 execute_spsr_restore(u32 address);
void execute_store_cpsr(u32 new_cpsr, u32 store_mask);
void execute_store_spsr(u32 new_spsr, u32 store_mask);
void arc_load_u8(void);
void arc_load_s8(void);
void arc_load_u16(void);
void arc_load_s16(void);
void arc_load_u32(void);
void arc_load_aligned32(void);
void arc_store_u8(void);
void arc_store_u16(void);
void arc_store_u32(void);
void arc_store_aligned32(void);

#define execute_load_u8         arc_load_u8
#define execute_load_s8         arc_load_s8
#define execute_load_u16        arc_load_u16
#define execute_load_s16        arc_load_s16
#define execute_load_u32        arc_load_u32
#define execute_aligned_load32  arc_load_aligned32
#define execute_store_u8        arc_store_u8
#define execute_store_u16       arc_store_u16
#define execute_store_u32       arc_store_u32
#define execute_aligned_store32 arc_store_aligned32

/* VRAM code is interpreted (see mips_emit.h for the rationale) */
#define HAVE_VRAM_INTERPRETER
#define generate_vram_interpreter(type)                                       \
  generate_block_prologue();                                                  \
  generate_load_pc(reg_a0, pc);                                               \
  arc_emit_j_abs(execute_vram_##type)                                         \

#define reg_a0      arc_reg_r0
#define reg_a1      arc_reg_r1
#define reg_a2      arc_reg_r2
#define reg_rv      arc_reg_r3
#define reg_temp    arc_reg_r4
#define reg_base    arc_reg_r5
#define reg_cycles  arc_reg_r6
#define reg_n_cache arc_reg_r7
#define reg_z_cache arc_reg_r8
#define reg_c_cache arc_reg_r9
#define reg_v_cache arc_reg_r10
#define reg_r14     arc_reg_r11
#define reg_temp2   arc_reg_r12
#define reg_r13     arc_reg_fp

static const u8 arm_to_arc_reg[] =
{
  arc_reg_r13, arc_reg_r14, arc_reg_r15, arc_reg_r16, arc_reg_r17,
  arc_reg_r18, arc_reg_r19, arc_reg_r20, arc_reg_r21, arc_reg_r22,
  arc_reg_r23, arc_reg_r24, arc_reg_r25, reg_r13, reg_r14,
  reg_a0, reg_a1, reg_a2
};

#define AREG(i) arm_to_arc_reg[i]

#define arm_reg_a0   15
#define arm_reg_a1   16
#define arm_reg_a2   17

/* Function calls: BL when in range, else JL limm */
#define generate_function_call(function_location)                             \
{                                                                             \
  s32 off__ = arc_rel(translation_ptr, function_location);                    \
  if((off__ > -(1 << 24)) && (off__ < (1 << 24)))                             \
  {                                                                           \
    arc_emit_bl(function_location);                                           \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arc_emit_jl_abs(function_location);                                       \
  }                                                                           \
}                                                                             \

#define generate_load_reg(ireg, reg_index)                                    \
  if((ireg) != AREG(reg_index))                                               \
  {                                                                           \
    arc_emit_mov(0, ireg, AREG(reg_index));                                   \
  }                                                                           \

#define generate_store_reg(ireg, reg_index)                                   \
  if((ireg) != AREG(reg_index))                                               \
  {                                                                           \
    arc_emit_mov(0, AREG(reg_index), ireg);                                   \
  }                                                                           \

#define generate_mov(ireg_dest, ireg_src)                                     \
  if((ireg_dest) != (ireg_src))                                               \
  {                                                                           \
    arc_emit_mov(0, ireg_dest, ireg_src);                                     \
  }                                                                           \

#define generate_load_imm(ireg, imm)                                          \
  arc_emit_mov_imm(ireg, imm)                                                 \

/* Memory stubs: the pc follows the call as a data word (they return past it;
   aligned, as the stubs read it with LD) */
#define generate_mem_call(function_location, pc_value)                        \
  arc_emit_align4();                                                          \
  generate_function_call(function_location);                                  \
  arc_emit_data32(pc_value)                                                   \

#define generate_load_pc(ireg, new_pc)                                        \
  arc_emit_mov_imm(ireg, new_pc)                                              \

#define generate_raw_u32(value)                                               \
  arc_emit_raw32(value)                                                       \

#define generate_cycle_update()                                               \
  if(cycle_count != 0)                                                        \
  {                                                                           \
    arc_emit_subi(reg_cycles, reg_cycles, cycle_count);                       \
    cycle_count = 0;                                                          \
  }                                                                           \

#define generate_cycle_update_force()                                         \
  arc_emit_subi(reg_cycles, reg_cycles, cycle_count);                         \
  cycle_count = 0                                                             \

/* Patch a Bcc (keeps its condition) / a B to jump to offset */
#define generate_branch_patch_conditional(dest, offset)                       \
  arc_write_raw32(dest, arc_enc_bcc(arc_rel(dest, offset),                    \
   arc_read_raw32(dest) & 0x1F))                                              \

#define generate_branch_patch_unconditional(dest, offset)                     \
  arc_write_raw32(dest, arc_enc_b(arc_rel(dest, offset)))                     \

#define generate_branch_no_cycle_update(writeback_location, new_pc)           \
  if(pc == idle_loop_target_pc)                                               \
  {                                                                           \
    generate_load_pc(reg_a0, new_pc);                                         \
    arc_emit_mov_imm(reg_cycles, 0);                                          \
    generate_function_call(arc_update_gba);                                   \
    arc_emit_b_filler(writeback_location);                                    \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arc_emit_cmpi(reg_cycles, 0);                                             \
    arc_emit_align4();                                                        \
    arc_emit_blcc(ARC_LT, update_trampoline + 8);                             \
    arc_emit_b_filler(writeback_location);                                    \
    arc_emit_data32(new_pc);                                                  \
  }                                                                           \

/* sub.f sets the flags for the bllt (a cmp would repeat it) */
#define generate_branch_cycle_update(writeback_location, new_pc)              \
  if((cycle_count != 0) && (pc != idle_loop_target_pc))                       \
  {                                                                           \
    arc_emit_op_ri(4, ARC_SUB, 1, reg_cycles, reg_cycles, cycle_count);       \
    cycle_count = 0;                                                          \
    arc_emit_align4();   /* (the word after the b is read with LD) */         \
    arc_emit_blcc(ARC_LT, update_trampoline + 8);                             \
    arc_emit_b_filler(writeback_location);                                    \
    arc_emit_data32(new_pc);                                                  \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_cycle_update();                                                  \
    generate_branch_no_cycle_update(writeback_location, new_pc);              \
  }                                                                           \

/* a0 holds the destination */
#define generate_indirect_branch_cycle_update(type)                           \
  generate_cycle_update_force();                                              \
  arc_emit_j_abs(arc_indirect_branch_##type)                                  \

#define generate_indirect_branch_no_cycle_update(type)                        \
  arc_emit_j_abs(arc_indirect_branch_##type)                                  \

/* Two 8-byte trampolines precede every block's entry point; the second is
   for branch exits, which keep the new pc in a word after their b */
#define block_prologue_size   16

#define generate_block_prologue()                                             \
  update_trampoline = translation_ptr;                                        \
  arc_emit_j_abs(arc_update_gba);                                             \
  arc_emit_j_abs(arc_update_gba_w)                                            \

#define generate_block_extra_vars()                                           \
  u8 *update_trampoline                                                       \

#define generate_block_extra_vars_arm()                                       \
  generate_block_extra_vars()                                                 \

#define generate_block_extra_vars_thumb()                                     \
  generate_block_extra_vars()                                                 \

#define check_generate_n_flag (flag_status & 0x08)
#define check_generate_z_flag (flag_status & 0x04)
#define check_generate_c_flag (flag_status & 0x02)
#define check_generate_v_flag (flag_status & 0x01)

#define generate_load_reg_pc(ireg, reg_index, pc_offset)                      \
  if(reg_index == REG_PC)                                                     \
  {                                                                           \
    generate_load_pc(ireg, (pc + pc_offset));                                 \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_load_reg(ireg, reg_index);                                       \
  }                                                                           \

#define check_load_reg_pc(arm_reg, reg_index, pc_offset)                      \
  if(reg_index == REG_PC)                                                     \
  {                                                                           \
    reg_index = arm_reg;                                                      \
    generate_load_pc(AREG(arm_reg), (pc + pc_offset));                        \
  }                                                                           \

#define check_store_reg_pc_no_flags(reg_index)                                \
  if(reg_index == REG_PC)                                                     \
  {                                                                           \
    generate_indirect_branch_arm();                                           \
  }                                                                           \

#define check_store_reg_pc_flags(reg_index)                                   \
  if(reg_index == REG_PC)                                                     \
  {                                                                           \
    generate_function_call(execute_spsr_restore);                             \
    generate_indirect_branch_dual();                                          \
  }                                                                           \

/* rt = bit pos of rs */
#define arc_extract_bit(rt, rs, pos)                                          \
  if((pos) != 0)                                                              \
  {                                                                           \
    arc_emit_lsri(rt, rs, pos);                                               \
    arc_emit_bmski(rt, rt, 0);                                                \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arc_emit_bmski(rt, rs, 0);                                                \
  }                                                                           \

/* after an op with .f: N and Z from the ARC flags / the result */
#define generate_op_logic_flags_f(_rd)                                        \
  if(check_generate_n_flag)                                                   \
  {                                                                           \
    arc_emit_lsri(reg_n_cache, _rd, 31);                                      \
  }                                                                           \
  if(check_generate_z_flag)                                                   \
  {                                                                           \
    arc_emit_mov_imm(reg_z_cache, 0);                                         \
    arc_emit_mov_cc_u6(ARC_EQ, reg_z_cache, 1);                               \
  }                                                                           \

#define generate_op_logic_flags(_rd)                                          \
  if(check_generate_z_flag)                                                   \
  {                                                                           \
    arc_emit_tst(_rd, _rd);                                                   \
  }                                                                           \
  generate_op_logic_flags_f(_rd)                                              \

/* C and V after an ARC subtraction (ARC C = borrow = !ARM C) */
#define generate_op_sub_cv_flags()                                            \
  if(check_generate_c_flag)                                                   \
  {                                                                           \
    arc_emit_mov_imm(reg_c_cache, 1);                                         \
    arc_emit_mov_cc_u6(ARC_CS, reg_c_cache, 0);                               \
  }                                                                           \
  if(check_generate_v_flag)                                                   \
  {                                                                           \
    arc_emit_mov_imm(reg_v_cache, 0);                                         \
    arc_emit_mov_cc_u6(ARC_VS, reg_v_cache, 1);                               \
  }                                                                           \

#define generate_op_add_cv_flags()                                            \
  if(check_generate_c_flag)                                                   \
  {                                                                           \
    arc_emit_mov_imm(reg_c_cache, 0);                                         \
    arc_emit_mov_cc_u6(ARC_CS, reg_c_cache, 1);                               \
  }                                                                           \
  if(check_generate_v_flag)                                                   \
  {                                                                           \
    arc_emit_mov_imm(reg_v_cache, 0);                                         \
    arc_emit_mov_cc_u6(ARC_VS, reg_v_cache, 1);                               \
  }                                                                           \

/* ARC carry = ARM C (for ADC) / = !ARM C (for SBC) */
#define arc_carry_from_c()                                                    \
  arc_emit_cmp_ri(ARC_RCMP, reg_c_cache, 0)                                   \

#define arc_borrow_from_c()                                                   \
  arc_emit_cmpi(reg_c_cache, 1)                                               \

/* Immediate shifts */

#define generate_shift_imm_lsl_no_flags(arm_reg, _rm, _shift)                 \
  check_load_reg_pc(arm_reg, _rm, 8);                                         \
  if(_shift != 0)                                                             \
  {                                                                           \
    arc_emit_asli(AREG(arm_reg), AREG(_rm), _shift);                          \
    _rm = arm_reg;                                                            \
  }                                                                           \

#define generate_shift_imm_lsr_no_flags(arm_reg, _rm, _shift)                 \
  if(_shift != 0)                                                             \
  {                                                                           \
    check_load_reg_pc(arm_reg, _rm, 8);                                       \
    arc_emit_lsri(AREG(arm_reg), AREG(_rm), _shift);                          \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arc_emit_mov_imm(AREG(arm_reg), 0);                                       \
  }                                                                           \
  _rm = arm_reg                                                               \

#define generate_shift_imm_asr_no_flags(arm_reg, _rm, _shift)                 \
  check_load_reg_pc(arm_reg, _rm, 8);                                         \
  arc_emit_asri(AREG(arm_reg), AREG(_rm), (_shift) ? (_shift) : 31);          \
  _rm = arm_reg                                                               \

#define generate_shift_imm_ror_no_flags(arm_reg, _rm, _shift)                 \
  check_load_reg_pc(arm_reg, _rm, 8);                                         \
  if(_shift != 0)                                                             \
  {                                                                           \
    arc_emit_rori(AREG(arm_reg), AREG(_rm), _shift);                          \
  }                                                                           \
  else                                                                        \
  { /* RRX (no carry update) */                                               \
    arc_emit_asli(reg_temp, reg_c_cache, 31);                                 \
    arc_emit_lsri(AREG(arm_reg), AREG(_rm), 1);                               \
    arc_emit_or(AREG(arm_reg), AREG(arm_reg), reg_temp);                      \
  }                                                                           \
  _rm = arm_reg                                                               \

#define generate_shift_imm_lsl_flags(arm_reg, _rm, _shift)                    \
  check_load_reg_pc(arm_reg, _rm, 8);                                         \
  if(_shift != 0)                                                             \
  {                                                                           \
    arc_extract_bit(reg_c_cache, AREG(_rm), (32 - _shift));                   \
    arc_emit_asli(AREG(arm_reg), AREG(_rm), _shift);                          \
    _rm = arm_reg;                                                            \
  }                                                                           \

#define generate_shift_imm_lsr_flags(arm_reg, _rm, _shift)                    \
  check_load_reg_pc(arm_reg, _rm, 8);                                         \
  if(_shift != 0)                                                             \
  {                                                                           \
    arc_extract_bit(reg_c_cache, AREG(_rm), (_shift - 1));                    \
    arc_emit_lsri(AREG(arm_reg), AREG(_rm), _shift);                          \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arc_emit_lsri(reg_c_cache, AREG(_rm), 31);                                \
    arc_emit_mov_imm(AREG(arm_reg), 0);                                       \
  }                                                                           \
  _rm = arm_reg                                                               \

#define generate_shift_imm_asr_flags(arm_reg, _rm, _shift)                    \
  check_load_reg_pc(arm_reg, _rm, 8);                                         \
  if(_shift != 0)                                                             \
  {                                                                           \
    arc_extract_bit(reg_c_cache, AREG(_rm), (_shift - 1));                    \
    arc_emit_asri(AREG(arm_reg), AREG(_rm), _shift);                          \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arc_emit_asri(AREG(arm_reg), AREG(_rm), 31);                              \
    arc_emit_bmski(reg_c_cache, AREG(arm_reg), 0);                            \
  }                                                                           \
  _rm = arm_reg                                                               \

#define generate_shift_imm_ror_flags(arm_reg, _rm, _shift)                    \
  check_load_reg_pc(arm_reg, _rm, 8);                                         \
  if(_shift != 0)                                                             \
  {                                                                           \
    arc_extract_bit(reg_c_cache, AREG(_rm), (_shift - 1));                    \
    arc_emit_rori(AREG(arm_reg), AREG(_rm), _shift);                          \
  }                                                                           \
  else                                                                        \
  { /* RRX (carry update) */                                                  \
    arc_emit_asli(reg_temp, reg_c_cache, 31);                                 \
    arc_emit_bmski(reg_c_cache, AREG(_rm), 0);                                \
    arc_emit_lsri(AREG(arm_reg), AREG(_rm), 1);                               \
    arc_emit_or(AREG(arm_reg), AREG(arm_reg), reg_temp);                      \
  }                                                                           \
  _rm = arm_reg                                                               \

/* Register shifts (amount = rs & 0xFF), result in a0 */

#define generate_shift_reg_lsl_no_flags(_rm, _rs)                             \
  arc_emit_bmski(reg_temp, AREG(_rs), 7);                                     \
  arc_emit_asl(reg_a0, AREG(_rm), reg_temp);                                  \
  arc_emit_cmpi(reg_temp, 32);                                                \
  arc_emit_mov_cc_u6(ARC_CC, reg_a0, 0)                                       \

#define generate_shift_reg_lsr_no_flags(_rm, _rs)                             \
  arc_emit_bmski(reg_temp, AREG(_rs), 7);                                     \
  arc_emit_lsr(reg_a0, AREG(_rm), reg_temp);                                  \
  arc_emit_cmpi(reg_temp, 32);                                                \
  arc_emit_mov_cc_u6(ARC_CC, reg_a0, 0)                                       \

#define generate_shift_reg_asr_no_flags(_rm, _rs)                             \
  arc_emit_bmski(reg_temp, AREG(_rs), 7);                                     \
  arc_emit_op_ru6(4, ARC_MIN, 0, reg_temp, reg_temp, 31);                     \
  arc_emit_asr(reg_a0, AREG(_rm), reg_temp)                                   \

#define generate_shift_reg_ror_no_flags(_rm, _rs)                             \
  arc_emit_ror(reg_a0, AREG(_rm), AREG(_rs))                                  \

#define generate_shift_reg_lsl_flags(_rm, _rs)                                \
{                                                                             \
  u32 shift_reg = _rs;                                                        \
  u8 *skip__;                                                                 \
  check_load_reg_pc(arm_reg_a1, shift_reg, 8);                                \
  generate_load_reg_pc(reg_a0, _rm, 12);                                      \
  arc_emit_bmski(reg_temp, AREG(shift_reg), 7);                               \
  arc_emit_tst(reg_temp, reg_temp);                                           \
  arc_emit_bcc_filler(ARC_EQ, skip__);                                        \
  arc_emit_subi(reg_temp, reg_temp, 1);                                       \
  arc_emit_asl(reg_a0, reg_a0, reg_temp);                                     \
  arc_emit_lsri(reg_c_cache, reg_a0, 31);                                     \
  arc_emit_asli(reg_a0, reg_a0, 1);                                           \
  arc_emit_cmpi(reg_temp, 32);                                                \
  arc_emit_mov_cc_u6(ARC_CC, reg_c_cache, 0);                                 \
  arc_emit_mov_cc_u6(ARC_CC, reg_a0, 0);                                      \
  generate_branch_patch_conditional(skip__, translation_ptr);                 \
}                                                                             \

#define generate_shift_reg_lsr_flags(_rm, _rs)                                \
{                                                                             \
  u32 shift_reg = _rs;                                                        \
  u8 *skip__;                                                                 \
  check_load_reg_pc(arm_reg_a1, shift_reg, 8);                                \
  generate_load_reg_pc(reg_a0, _rm, 12);                                      \
  arc_emit_bmski(reg_temp, AREG(shift_reg), 7);                               \
  arc_emit_tst(reg_temp, reg_temp);                                           \
  arc_emit_bcc_filler(ARC_EQ, skip__);                                        \
  arc_emit_subi(reg_temp, reg_temp, 1);                                       \
  arc_emit_lsr(reg_a0, reg_a0, reg_temp);                                     \
  arc_emit_bmski(reg_c_cache, reg_a0, 0);                                     \
  arc_emit_lsri(reg_a0, reg_a0, 1);                                           \
  arc_emit_cmpi(reg_temp, 32);                                                \
  arc_emit_mov_cc_u6(ARC_CC, reg_c_cache, 0);                                 \
  arc_emit_mov_cc_u6(ARC_CC, reg_a0, 0);                                      \
  generate_branch_patch_conditional(skip__, translation_ptr);                 \
}                                                                             \

#define generate_shift_reg_asr_flags(_rm, _rs)                                \
{                                                                             \
  u8 *skip__;                                                                 \
  generate_load_reg_pc(reg_a1, _rs, 8);                                       \
  generate_load_reg_pc(reg_a0, _rm, 12);                                      \
  arc_emit_bmski(reg_temp, reg_a1, 7);                                        \
  arc_emit_tst(reg_temp, reg_temp);                                           \
  arc_emit_bcc_filler(ARC_EQ, skip__);                                        \
  arc_emit_op_ru6(4, ARC_MIN, 0, reg_temp, reg_temp, 32);                     \
  arc_emit_subi(reg_temp, reg_temp, 1);                                       \
  arc_emit_asr(reg_a0, reg_a0, reg_temp);                                     \
  arc_emit_bmski(reg_c_cache, reg_a0, 0);                                     \
  arc_emit_asri(reg_a0, reg_a0, 1);                                           \
  generate_branch_patch_conditional(skip__, translation_ptr);                 \
}                                                                             \

#define generate_shift_reg_ror_flags(_rm, _rs)                                \
{                                                                             \
  u8 *skip__;                                                                 \
  arc_emit_mov(0, reg_a0, AREG(_rm));                                         \
  arc_emit_bmski(reg_temp, AREG(_rs), 7);                                     \
  arc_emit_tst(reg_temp, reg_temp);                                           \
  arc_emit_bcc_filler(ARC_EQ, skip__);                                        \
  arc_emit_subi(reg_temp, reg_temp, 1);                                       \
  arc_emit_lsr(reg_temp, reg_a0, reg_temp);                                   \
  arc_emit_bmski(reg_c_cache, reg_temp, 0);                                   \
  arc_emit_ror(reg_a0, reg_a0, AREG(_rs));                                    \
  generate_branch_patch_conditional(skip__, translation_ptr);                 \
}                                                                             \

#define generate_shift_imm(arm_reg, name, flags_op)                           \
  u32 shift = (opcode >> 7) & 0x1F;                                           \
  generate_shift_imm_##name##_##flags_op(arm_reg, rm, shift)                  \

/* PC as rm reads pc + 12 with a register shift (pc + 8 as rs) */
#define generate_shift_reg(arm_reg, name, flags_op)                           \
  u32 rs = ((opcode >> 8) & 0x0F);                                            \
  if(rm == REG_PC)                                                            \
  {                                                                           \
    generate_load_pc(reg_a0, (pc + 12));                                      \
    rm = arm_reg_a0;                                                          \
  }                                                                           \
  if(rs == REG_PC)                                                            \
  {                                                                           \
    generate_load_pc(reg_a1, (pc + 8));                                       \
    rs = arm_reg_a1;                                                          \
  }                                                                           \
  generate_shift_reg_##name##_##flags_op(rm, rs);                             \
  rm = arm_reg                                                                \

#define generate_load_rm_sh(rm, flags_op)                                     \
{                                                                             \
  switch((opcode >> 4) & 0x07)                                                \
  {                                                                           \
    case 0x0: { generate_shift_imm(arm_reg_a0, lsl, flags_op); break; }       \
    case 0x1: { generate_shift_reg(arm_reg_a0, lsl, flags_op); break; }       \
    case 0x2: { generate_shift_imm(arm_reg_a0, lsr, flags_op); break; }       \
    case 0x3: { generate_shift_reg(arm_reg_a0, lsr, flags_op); break; }       \
    case 0x4: { generate_shift_imm(arm_reg_a0, asr, flags_op); break; }       \
    case 0x5: { generate_shift_reg(arm_reg_a0, asr, flags_op); break; }       \
    case 0x6: { generate_shift_imm(arm_reg_a0, ror, flags_op); break; }       \
    case 0x7: { generate_shift_reg(arm_reg_a0, ror, flags_op); break; }       \
  }                                                                           \
}                                                                             \

#define generate_load_offset_sh(rm)                                           \
{                                                                             \
  switch((opcode >> 5) & 0x03)                                                \
  {                                                                           \
    case 0x0: { generate_shift_imm(arm_reg_a1, lsl, no_flags); break; }       \
    case 0x1: { generate_shift_imm(arm_reg_a1, lsr, no_flags); break; }       \
    case 0x2: { generate_shift_imm(arm_reg_a1, asr, no_flags); break; }       \
    case 0x3: { generate_shift_imm(arm_reg_a1, ror, no_flags); break; }       \
  }                                                                           \
}                                                                             \

#define generate_indirect_branch_arm()                                        \
{                                                                             \
  if(condition == 0x0E)                                                       \
  {                                                                           \
    generate_indirect_branch_cycle_update(arm);                               \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_indirect_branch_no_cycle_update(arm);                            \
  }                                                                           \
}                                                                             \

#define generate_indirect_branch_dual()                                       \
{                                                                             \
  if(condition == 0x0E)                                                       \
  {                                                                           \
    generate_indirect_branch_cycle_update(dual);                              \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_indirect_branch_no_cycle_update(dual);                           \
  }                                                                           \
}                                                                             \

u32 execute_spsr_restore_body(u32 address)
{
  set_cpu_mode(cpu_modes[reg[REG_CPSR] & 0xF]);
  if((io_registers[REG_IE] & io_registers[REG_IF]) &&
   io_registers[REG_IME] && ((reg[REG_CPSR] & 0x80) == 0))
  {
    REG_MODE(MODE_IRQ)[6] = address + 4;
    REG_SPSR(MODE_IRQ) = reg[REG_CPSR];
    reg[REG_CPSR] = 0xD2;
    address = 0x00000018;
    set_cpu_mode(MODE_IRQ);
  }

  if(reg[REG_CPSR] & 0x20)
    address |= 0x01;

  return address;
}

static const u8 arc_cc_inverse[16] =
 { 0, ARC_NE, ARC_EQ, ARC_MI, ARC_PL, ARC_CC, ARC_CS, ARC_VC, ARC_VS,
   ARC_LE, ARC_LT, ARC_GE, ARC_GT, ARC_LS, ARC_HI, 0 };

/* The ARC condition for ARM condition cond (0-13) when the thumb opcode
   prev was the last to set the flags, 0 if they are not in the ARC flags:
   after a subtraction (ARC C = borrow = !ARM C), an addition, or tst (N, Z
   only). */
static inline u32 arc_thumb_native_cc(u32 prev, u32 cond)
{
  static const u8 sub_cc[14] =
   { ARC_EQ, ARC_NE, ARC_CC, ARC_CS, ARC_MI, ARC_PL, ARC_VS, ARC_VC,
     ARC_HI, ARC_LS, ARC_GE, ARC_LT, ARC_GT, ARC_LE };
  static const u8 add_cc[14] =
   { ARC_EQ, ARC_NE, ARC_CS, ARC_CC, ARC_MI, ARC_PL, ARC_VS, ARC_VC,
     0, 0, ARC_GE, ARC_LT, ARC_GT, ARC_LE };
  u32 hi = prev >> 8, alu = (prev >> 6) & 3;

  if(cond > 13)
    return 0;
  /* sub rd, rs, rn / imm3; cmp rd, imm; sub rd, imm; cmp hi; cmp (alu) */
  if((hi == 0x1A) || (hi == 0x1B) || (hi == 0x1E) || (hi == 0x1F) ||
   ((hi >= 0x28) && (hi <= 0x2F)) || ((hi >= 0x38) && (hi <= 0x3F)) ||
   (hi == 0x45) || ((hi == 0x42) && (alu == 2)))
    return sub_cc[cond];
  /* add rd, rs, rn / imm3; add rd, imm; cmn (alu) */
  if((hi == 0x18) || (hi == 0x19) || (hi == 0x1C) || (hi == 0x1D) ||
   ((hi >= 0x30) && (hi <= 0x37)) || ((hi == 0x42) && (alu == 3)))
    return add_cc[cond];
  /* tst: eq, ne, mi, pl */
  if((hi == 0x42) && (alu == 0) && ((cond < 2) || (cond == 4) || (cond == 5)))
    return sub_cc[cond];
  return 0;
}

/* Conditions: branch over the conditional code when the condition fails.
   The cycle count is settled first (it is due on both paths). */

#define generate_condition_flag(flagreg, skip_cc)                             \
  generate_cycle_update_force();                                              \
  arc_emit_tst(flagreg, flagreg);                                             \
  arc_emit_bcc_filler(skip_cc, backpatch_address)                             \

#define generate_condition_eq() generate_condition_flag(reg_z_cache, ARC_EQ)
#define generate_condition_ne() generate_condition_flag(reg_z_cache, ARC_NE)
#define generate_condition_cs() generate_condition_flag(reg_c_cache, ARC_EQ)
#define generate_condition_cc() generate_condition_flag(reg_c_cache, ARC_NE)
#define generate_condition_mi() generate_condition_flag(reg_n_cache, ARC_EQ)
#define generate_condition_pl() generate_condition_flag(reg_n_cache, ARC_NE)
#define generate_condition_vs() generate_condition_flag(reg_v_cache, ARC_EQ)
#define generate_condition_vc() generate_condition_flag(reg_v_cache, ARC_NE)

/* hi: C && !Z; temp = !C | Z, skip if non-zero */
#define generate_condition_hi()                                               \
  generate_cycle_update_force();                                              \
  arc_emit_xori(reg_temp, reg_c_cache, 1);                                    \
  arc_emit_op_rr(4, ARC_OR, 1, reg_temp, reg_temp, reg_z_cache);              \
  arc_emit_bcc_filler(ARC_NE, backpatch_address)                              \

#define generate_condition_ls()                                               \
  generate_cycle_update_force();                                              \
  arc_emit_xori(reg_temp, reg_c_cache, 1);                                    \
  arc_emit_op_rr(4, ARC_OR, 1, reg_temp, reg_temp, reg_z_cache);              \
  arc_emit_bcc_filler(ARC_EQ, backpatch_address)                              \

#define generate_condition_ge()                                               \
  generate_cycle_update_force();                                              \
  arc_emit_cmp(reg_n_cache, reg_v_cache);                                     \
  arc_emit_bcc_filler(ARC_NE, backpatch_address)                              \

#define generate_condition_lt()                                               \
  generate_cycle_update_force();                                              \
  arc_emit_cmp(reg_n_cache, reg_v_cache);                                     \
  arc_emit_bcc_filler(ARC_EQ, backpatch_address)                              \

/* gt: !Z && N == V; temp = (N ^ V) | Z, skip if non-zero */
#define generate_condition_gt()                                               \
  generate_cycle_update_force();                                              \
  arc_emit_xor(reg_temp, reg_n_cache, reg_v_cache);                           \
  arc_emit_op_rr(4, ARC_OR, 1, reg_temp, reg_temp, reg_z_cache);              \
  arc_emit_bcc_filler(ARC_NE, backpatch_address)                              \

#define generate_condition_le()                                               \
  generate_cycle_update_force();                                              \
  arc_emit_xor(reg_temp, reg_n_cache, reg_v_cache);                           \
  arc_emit_op_rr(4, ARC_OR, 1, reg_temp, reg_temp, reg_z_cache);              \
  arc_emit_bcc_filler(ARC_EQ, backpatch_address)                              \

#define generate_condition()                                                  \
  switch(condition)                                                           \
  {                                                                           \
    case 0x0: generate_condition_eq(); break;                                 \
    case 0x1: generate_condition_ne(); break;                                 \
    case 0x2: generate_condition_cs(); break;                                 \
    case 0x3: generate_condition_cc(); break;                                 \
    case 0x4: generate_condition_mi(); break;                                 \
    case 0x5: generate_condition_pl(); break;                                 \
    case 0x6: generate_condition_vs(); break;                                 \
    case 0x7: generate_condition_vc(); break;                                 \
    case 0x8: generate_condition_hi(); break;                                 \
    case 0x9: generate_condition_ls(); break;                                 \
    case 0xA: generate_condition_ge(); break;                                 \
    case 0xB: generate_condition_lt(); break;                                 \
    case 0xC: generate_condition_gt(); break;                                 \
    case 0xD: generate_condition_le(); break;                                 \
    default: break;                                                           \
  }                                                                           \

#define generate_branch()                                                     \
{                                                                             \
  if(condition == 0x0E)                                                       \
  {                                                                           \
    generate_branch_cycle_update(                                             \
     block_exits[block_exit_position].branch_source,                          \
     block_exits[block_exit_position].branch_target);                         \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_branch_no_cycle_update(                                          \
     block_exits[block_exit_position].branch_source,                          \
     block_exits[block_exit_position].branch_target);                         \
  }                                                                           \
  block_exit_position++;                                                      \
}                                                                             \

/* Data processing, register operand (host registers) */

#define generate_op_and_reg(_rd, _rn, _rm) arc_emit_and(_rd, _rn, _rm)
#define generate_op_orr_reg(_rd, _rn, _rm) arc_emit_or(_rd, _rn, _rm)
#define generate_op_eor_reg(_rd, _rn, _rm) arc_emit_xor(_rd, _rn, _rm)
#define generate_op_bic_reg(_rd, _rn, _rm) arc_emit_bic(_rd, _rn, _rm)
#define generate_op_sub_reg(_rd, _rn, _rm) arc_emit_sub(_rd, _rn, _rm)
#define generate_op_rsb_reg(_rd, _rn, _rm) arc_emit_sub(_rd, _rm, _rn)
#define generate_op_add_reg(_rd, _rn, _rm) arc_emit_add(_rd, _rn, _rm)
#define generate_op_mov_reg(_rd, _rn, _rm) generate_mov(_rd, _rm)
#define generate_op_mvn_reg(_rd, _rn, _rm) arc_emit_not(_rd, _rm)

#define generate_op_sbc_reg(_rd, _rn, _rm)                                    \
  arc_borrow_from_c();                                                        \
  arc_emit_op_rr(4, ARC_SBC, 0, _rd, _rn, _rm)                                \

#define generate_op_rsc_reg(_rd, _rn, _rm)                                    \
  arc_borrow_from_c();                                                        \
  arc_emit_op_rr(4, ARC_SBC, 0, _rd, _rm, _rn)                                \

#define generate_op_adc_reg(_rd, _rn, _rm)                                    \
  arc_carry_from_c();                                                         \
  arc_emit_op_rr(4, ARC_ADC, 0, _rd, _rn, _rm)                                \

/* Data processing, immediate operand (in imm) */

#define generate_op_and_imm(_rd, _rn) arc_emit_op_ri(4, ARC_AND, 0, _rd, _rn, imm)
#define generate_op_orr_imm(_rd, _rn) arc_emit_op_ri(4, ARC_OR, 0, _rd, _rn, imm)
#define generate_op_eor_imm(_rd, _rn) arc_emit_op_ri(4, ARC_XOR, 0, _rd, _rn, imm)
#define generate_op_bic_imm(_rd, _rn) arc_emit_op_ri(4, ARC_BIC, 0, _rd, _rn, imm)
#define generate_op_sub_imm(_rd, _rn) arc_emit_op_ri(4, ARC_SUB, 0, _rd, _rn, imm)
#define generate_op_rsb_imm(_rd, _rn) arc_emit_op_ri(4, ARC_RSUB, 0, _rd, _rn, imm)
#define generate_op_add_imm(_rd, _rn) arc_emit_op_ri(4, ARC_ADD, 0, _rd, _rn, imm)
#define generate_op_mov_imm(_rd, _rn) generate_load_imm(_rd, imm)
#define generate_op_mvn_imm(_rd, _rn) generate_load_imm(_rd, (~imm))

#define generate_op_sbc_imm(_rd, _rn)                                         \
  arc_borrow_from_c();                                                        \
  arc_emit_op_ri(4, ARC_SBC, 0, _rd, _rn, imm)                                \

#define generate_op_rsc_imm(_rd, _rn)                                         \
  arc_borrow_from_c();                                                        \
  arc_emit_op_lr(4, ARC_SBC, 0, _rd, imm, _rn)                                \

#define generate_op_adc_imm(_rd, _rn)                                         \
  arc_carry_from_c();                                                         \
  arc_emit_op_ri(4, ARC_ADC, 0, _rd, _rn, imm)                                \

/* Flag setting versions */

#define generate_op_ands_reg(_rd, _rn, _rm)                                   \
  arc_emit_op_rr(4, ARC_AND, 1, _rd, _rn, _rm);                               \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_orrs_reg(_rd, _rn, _rm)                                   \
  arc_emit_op_rr(4, ARC_OR, 1, _rd, _rn, _rm);                                \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_eors_reg(_rd, _rn, _rm)                                   \
  arc_emit_op_rr(4, ARC_XOR, 1, _rd, _rn, _rm);                               \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_bics_reg(_rd, _rn, _rm)                                   \
  arc_emit_op_rr(4, ARC_BIC, 1, _rd, _rn, _rm);                               \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_subs_reg(_rd, _rn, _rm)                                   \
  arc_emit_op_rr(4, ARC_SUB, 1, _rd, _rn, _rm);                               \
  generate_op_sub_cv_flags();                                                 \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_rsbs_reg(_rd, _rn, _rm)                                   \
  generate_op_subs_reg(_rd, _rm, _rn)                                         \

#define generate_op_sbcs_reg(_rd, _rn, _rm)                                   \
  arc_borrow_from_c();                                                        \
  arc_emit_op_rr(4, ARC_SBC, 1, _rd, _rn, _rm);                               \
  generate_op_sub_cv_flags();                                                 \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_rscs_reg(_rd, _rn, _rm)                                   \
  generate_op_sbcs_reg(_rd, _rm, _rn)                                         \

#define generate_op_adds_reg(_rd, _rn, _rm)                                   \
  arc_emit_op_rr(4, ARC_ADD, 1, _rd, _rn, _rm);                               \
  generate_op_add_cv_flags();                                                 \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_adcs_reg(_rd, _rn, _rm)                                   \
  arc_carry_from_c();                                                         \
  arc_emit_op_rr(4, ARC_ADC, 1, _rd, _rn, _rm);                               \
  generate_op_add_cv_flags();                                                 \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_movs_reg(_rd, _rn, _rm)                                   \
  arc_emit_mov(1, _rd, _rm);                                                  \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_mvns_reg(_rd, _rn, _rm)                                   \
  arc_emit_sop_r(ARC_SOP_NOT, 1, _rd, _rm);                                   \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_neg_reg(_rd, _rn, _rm)                                    \
  arc_emit_op_ru6(4, ARC_RSUB, 1, _rd, _rm, 0);                               \
  generate_op_sub_cv_flags();                                                 \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_muls_reg(_rd, _rn, _rm)                                   \
  arc_emit_op_rr(5, ARC_MULU64, 0, arc_reg_limm, _rn, _rm);                   \
  arc_emit_mov(1, _rd, arc_reg_mmid);                                         \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_cmp_reg(_rd, _rn, _rm)                                    \
  generate_op_subs_reg(reg_temp, _rn, _rm)                                    \

#define generate_op_cmn_reg(_rd, _rn, _rm)                                    \
  generate_op_adds_reg(reg_temp, _rn, _rm)                                    \

#define generate_op_tst_reg(_rd, _rn, _rm)                                    \
  generate_op_ands_reg(reg_temp, _rn, _rm)                                    \

#define generate_op_teq_reg(_rd, _rn, _rm)                                    \
  generate_op_eors_reg(reg_temp, _rn, _rm)                                    \

#define generate_op_ands_imm(_rd, _rn)                                        \
  arc_emit_op_ri(4, ARC_AND, 1, _rd, _rn, imm);                               \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_orrs_imm(_rd, _rn)                                        \
  arc_emit_op_ri(4, ARC_OR, 1, _rd, _rn, imm);                                \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_eors_imm(_rd, _rn)                                        \
  arc_emit_op_ri(4, ARC_XOR, 1, _rd, _rn, imm);                               \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_bics_imm(_rd, _rn)                                        \
  arc_emit_op_ri(4, ARC_BIC, 1, _rd, _rn, imm);                               \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_subs_imm(_rd, _rn)                                        \
  arc_emit_op_ri(4, ARC_SUB, 1, _rd, _rn, imm);                               \
  generate_op_sub_cv_flags();                                                 \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_rsbs_imm(_rd, _rn)                                        \
  arc_emit_op_ri(4, ARC_RSUB, 1, _rd, _rn, imm);                              \
  generate_op_sub_cv_flags();                                                 \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_sbcs_imm(_rd, _rn)                                        \
  arc_borrow_from_c();                                                        \
  arc_emit_op_ri(4, ARC_SBC, 1, _rd, _rn, imm);                               \
  generate_op_sub_cv_flags();                                                 \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_rscs_imm(_rd, _rn)                                        \
  arc_borrow_from_c();                                                        \
  arc_emit_op_lr(4, ARC_SBC, 1, _rd, imm, _rn);                               \
  generate_op_sub_cv_flags();                                                 \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_adds_imm(_rd, _rn)                                        \
  arc_emit_op_ri(4, ARC_ADD, 1, _rd, _rn, imm);                               \
  generate_op_add_cv_flags();                                                 \
  generate_op_logic_flags_f(_rd)                                              \

#define generate_op_adcs_imm(_rd, _rn)                                        \
  arc_carry_from_c();                                                         \
  arc_emit_op_ri(4, ARC_ADC, 1, _rd, _rn, imm);                               \
  generate_op_add_cv_flags();                                                 \
  generate_op_logic_flags_f(_rd)                                              \

/* N and Z of a constant are known now */
#define generate_op_const_flags(value)                                        \
  if(check_generate_n_flag)                                                   \
  {                                                                           \
    arc_emit_mov_imm(reg_n_cache, ((u32)(value)) >> 31);                      \
  }                                                                           \
  if(check_generate_z_flag)                                                   \
  {                                                                           \
    arc_emit_mov_imm(reg_z_cache, ((u32)(value)) == 0);                       \
  }                                                                           \

#define generate_op_movs_imm(_rd, _rn)                                        \
  generate_load_imm(_rd, imm);                                                \
  generate_op_const_flags(imm)                                                \

#define generate_op_mvns_imm(_rd, _rn)                                        \
  generate_load_imm(_rd, (~imm));                                             \
  generate_op_const_flags(~imm)                                               \

#define generate_op_cmp_imm(_rd, _rn) generate_op_subs_imm(reg_temp, _rn)
#define generate_op_cmn_imm(_rd, _rn) generate_op_adds_imm(reg_temp, _rn)
#define generate_op_tst_imm(_rd, _rn) generate_op_ands_imm(reg_temp, _rn)
#define generate_op_teq_imm(_rd, _rn) generate_op_eors_imm(reg_temp, _rn)

#define arm_generate_op_load_yes()                                            \
  generate_load_reg_pc(reg_a1, rn, 8)                                         \

#define arm_generate_op_load_no()                                             \

#define arm_op_check_yes()                                                    \
  check_load_reg_pc(arm_reg_a1, rn, 8)                                        \

#define arm_op_check_no()                                                     \

#define arm_generate_op_reg_flags(name, load_op)                              \
  arm_decode_data_proc_reg(opcode);                                           \
  if(check_generate_c_flag)                                                   \
  {                                                                           \
    generate_load_rm_sh(rm, flags);                                           \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_load_rm_sh(rm, no_flags);                                        \
  }                                                                           \
                                                                              \
  arm_op_check_##load_op();                                                   \
  generate_op_##name##_reg(AREG(rd), AREG(rn), AREG(rm))                      \

#define arm_generate_op_reg(name, load_op)                                    \
  arm_decode_data_proc_reg(opcode);                                           \
  generate_load_rm_sh(rm, no_flags);                                          \
  arm_op_check_##load_op();                                                   \
  generate_op_##name##_reg(AREG(rd), AREG(rn), AREG(rm))                      \

#define arm_generate_op_imm(name, load_op)                                    \
  arm_decode_data_proc_imm(opcode);                                           \
  ror(imm, imm, imm_ror);                                                     \
  arm_op_check_##load_op();                                                   \
  generate_op_##name##_imm(AREG(rd), AREG(rn))                                \

#define arm_generate_op_imm_flags(name, load_op)                              \
  arm_decode_data_proc_imm(opcode);                                           \
  ror(imm, imm, imm_ror);                                                     \
  if(check_generate_c_flag && (imm_ror != 0))                                 \
  { /* Generate carry flag from integer rotation */                           \
    arc_emit_mov_imm(reg_c_cache, ((imm) >> 31));                             \
  }                                                                           \
  arm_op_check_##load_op();                                                   \
  generate_op_##name##_imm(AREG(rd), AREG(rn))                                \

#define arm_data_proc(name, type, flags_op)                                   \
{                                                                             \
  arm_generate_op_##type(name, yes);                                          \
  check_store_reg_pc_##flags_op(rd);                                          \
}                                                                             \

#define arm_data_proc_test(name, type)                                        \
{                                                                             \
  arm_generate_op_##type(name, yes);                                          \
}                                                                             \

#define arm_data_proc_unary(name, type, flags_op)                             \
{                                                                             \
  arm_generate_op_##type(name, no);                                           \
  check_store_reg_pc_##flags_op(rd);                                          \
}                                                                             \

/* Multiplies: the 64-bit product's low word is in MMID (r58) on the
   Leapster 2, the high word in MHI (r59) */

#define arm_multiply_flags_yes(_rd)                                           \
  generate_op_logic_flags_f(_rd)                                              \

#define arm_multiply_flags_no(_rd)                                            \

#define arm_multiply_add_no(f)                                                \
  arc_emit_mov(f, AREG(rd), arc_reg_mmid)                                     \

#define arm_multiply_add_yes(f)                                               \
  arc_emit_op_rr(4, ARC_ADD, f, AREG(rd), arc_reg_mmid, AREG(rn))             \

#define arc_multiply_f_yes 1
#define arc_multiply_f_no  0

#define arm_multiply(add_op, flags)                                           \
{                                                                             \
  arm_decode_multiply();                                                      \
  arc_emit_op_rr(5, ARC_MULU64, 0, arc_reg_limm, AREG(rm), AREG(rs));         \
  arm_multiply_add_##add_op(arc_multiply_f_##flags);                          \
  arm_multiply_flags_##flags(AREG(rd));                                       \
}                                                                             \

#define generate_multiply_s64()                                               \
  arc_emit_op_rr(5, ARC_MUL64, 0, arc_reg_limm, AREG(rm), AREG(rs))           \

#define generate_multiply_u64()                                               \
  arc_emit_op_rr(5, ARC_MULU64, 0, arc_reg_limm, AREG(rm), AREG(rs))          \

/* the accumulation is done by arm_multiply_long_add_yes */
#define generate_multiply_s64_add() generate_multiply_s64()
#define generate_multiply_u64_add() generate_multiply_u64()

#define arm_multiply_long_flags_yes(_rdlo, _rdhi)                             \
  if(check_generate_z_flag)                                                   \
  {                                                                           \
    arc_emit_op_rr(4, ARC_OR, 1, reg_temp, _rdlo, _rdhi);                     \
    arc_emit_mov_imm(reg_z_cache, 0);                                         \
    arc_emit_mov_cc_u6(ARC_EQ, reg_z_cache, 1);                               \
  }                                                                           \
  if(check_generate_n_flag)                                                   \
  {                                                                           \
    arc_emit_lsri(reg_n_cache, _rdhi, 31);                                    \
  }                                                                           \

#define arm_multiply_long_flags_no(_rdlo, _rdhi)                              \

#define arm_multiply_long_add_yes(name)                                       \
  generate_multiply_##name();                                                 \
  arc_emit_op_rr(4, ARC_ADD, 1, AREG(rdlo), AREG(rdlo), arc_reg_mmid);        \
  arc_emit_op_rr(4, ARC_ADC, 0, AREG(rdhi), AREG(rdhi), arc_reg_mhi)          \

#define arm_multiply_long_add_no(name)                                        \
  generate_multiply_##name();                                                 \
  arc_emit_mov(0, AREG(rdlo), arc_reg_mmid);                                  \
  arc_emit_mov(0, AREG(rdhi), arc_reg_mhi)                                    \

#define arm_multiply_long(name, add_op, flags)                                \
{                                                                             \
  arm_decode_multiply_long();                                                 \
  arm_multiply_long_add_##add_op(name);                                       \
  arm_multiply_long_flags_##flags(AREG(rdlo), AREG(rdhi));                    \
}                                                                             \

#define arm_psr_read(op_type, psr_reg)                                        \
  generate_function_call(execute_read_##psr_reg);                             \
  generate_store_reg(reg_rv, rd)                                              \

u32 execute_store_cpsr_body(u32 _cpsr, u32 address)
{
  set_cpu_mode(cpu_modes[_cpsr & 0xF]);
  if((io_registers[REG_IE] & io_registers[REG_IF]) &&
   io_registers[REG_IME] && ((_cpsr & 0x80) == 0))
  {
    REG_MODE(MODE_IRQ)[6] = address + 4;
    REG_SPSR(MODE_IRQ) = _cpsr;
    reg[REG_CPSR] = 0xD2;
    set_cpu_mode(MODE_IRQ);
    return 0x00000018;
  }

  return 0;
}

#define arm_psr_load_new_reg()                                                \
  generate_load_reg(reg_a0, rm)                                               \

#define arm_psr_load_new_imm()                                                \
  ror(imm, imm, imm_ror);                                                     \
  generate_load_imm(reg_a0, imm)                                              \

#define arm_psr_store_spsr()                                                  \
  generate_load_imm(reg_a1, spsr_masks[psr_pfield]);                          \
  generate_function_call(execute_store_spsr)                                  \

/* a0 new cpsr, a1 pc, a2 user mask, rv privileged mask */
#define arm_psr_store_cpsr()                                                  \
  generate_load_pc(reg_a1, (pc));                                             \
  generate_load_imm(reg_a2, cpsr_masks[psr_pfield][0]);                       \
  generate_load_imm(reg_rv, cpsr_masks[psr_pfield][1]);                       \
  generate_function_call(execute_store_cpsr)                                  \

#define arm_psr_store(op_type, psr_reg)                                       \
  arm_psr_load_new_##op_type();                                               \
  arm_psr_store_##psr_reg();                                                  \

#define arm_psr(op_type, transfer_type, psr_reg)                              \
{                                                                             \
  arm_decode_psr_##op_type(opcode);                                           \
  arm_psr_##transfer_type(op_type, psr_reg);                                  \
}                                                                             \

#define thumb_load_pc_pool_const(rd, value)                                   \
  generate_load_imm(AREG(rd), (value));                                       \

/* Memory: loads take a0 = address, a1 = pc and return in rv; stores take
   a0 = address, a1 = value, a2 = pc of the next instruction */

#define arm_access_memory_load(mem_type)                                      \
  cycle_count += 2;                                                           \
  generate_mem_call(execute_load_##mem_type, (pc));                           \
  generate_store_reg(reg_rv, rd);                                             \
  check_store_reg_pc_no_flags(rd)                                             \

#define arm_access_memory_store(mem_type)                                     \
  cycle_count++;                                                              \
  generate_load_reg_pc(reg_a1, rd, 12);                                       \
  generate_mem_call(execute_store_##mem_type, (pc + 4))                       \

#define arm_access_memory_reg_pre_up()                                        \
  arc_emit_add(reg_a0, AREG(rn), AREG(rm))                                    \

#define arm_access_memory_reg_pre_down()                                      \
  arc_emit_sub(reg_a0, AREG(rn), AREG(rm))                                    \

#define arm_access_memory_reg_pre(adjust_dir)                                 \
  check_load_reg_pc(arm_reg_a0, rn, 8);                                       \
  arm_access_memory_reg_pre_##adjust_dir()                                    \

#define arm_access_memory_reg_pre_wb(adjust_dir)                              \
  arm_access_memory_reg_pre(adjust_dir);                                      \
  generate_store_reg(reg_a0, rn)                                              \

#define arm_access_memory_reg_post_up()                                       \
  arc_emit_add(AREG(rn), AREG(rn), AREG(rm))                                  \

#define arm_access_memory_reg_post_down()                                     \
  arc_emit_sub(AREG(rn), AREG(rn), AREG(rm))                                  \

#define arm_access_memory_reg_post(adjust_dir)                                \
  generate_load_reg(reg_a0, rn);                                              \
  arm_access_memory_reg_post_##adjust_dir()                                   \

#define arm_access_memory_imm_pre_up()                                        \
  arc_emit_addi(reg_a0, AREG(rn), offset)                                     \

#define arm_access_memory_imm_pre_down()                                      \
  arc_emit_subi(reg_a0, AREG(rn), offset)                                     \

#define arm_access_memory_imm_pre(adjust_dir)                                 \
  check_load_reg_pc(arm_reg_a0, rn, 8);                                       \
  arm_access_memory_imm_pre_##adjust_dir()                                    \

#define arm_access_memory_imm_pre_wb(adjust_dir)                              \
  arm_access_memory_imm_pre(adjust_dir);                                      \
  generate_store_reg(reg_a0, rn)                                              \

#define arm_access_memory_imm_post_up()                                       \
  arc_emit_addi(AREG(rn), AREG(rn), offset)                                   \

#define arm_access_memory_imm_post_down()                                     \
  arc_emit_subi(AREG(rn), AREG(rn), offset)                                   \

#define arm_access_memory_imm_post(adjust_dir)                                \
  generate_load_reg(reg_a0, rn);                                              \
  arm_access_memory_imm_post_##adjust_dir()                                   \

#define arm_data_trans_reg(adjust_op, adjust_dir)                             \
  arm_decode_data_trans_reg();                                                \
  generate_load_offset_sh(rm);                                                \
  arm_access_memory_reg_##adjust_op(adjust_dir)                               \

#define arm_data_trans_imm(adjust_op, adjust_dir)                             \
  arm_decode_data_trans_imm();                                                \
  arm_access_memory_imm_##adjust_op(adjust_dir)                               \

#define arm_data_trans_half_reg(adjust_op, adjust_dir)                        \
  arm_decode_half_trans_r();                                                  \
  arm_access_memory_reg_##adjust_op(adjust_dir)                               \

#define arm_data_trans_half_imm(adjust_op, adjust_dir)                        \
  arm_decode_half_trans_of();                                                 \
  arm_access_memory_imm_##adjust_op(adjust_dir)                               \

#define arm_access_memory(access_type, direction, adjust_op, mem_type,        \
 offset_type)                                                                 \
{                                                                             \
  arm_data_trans_##offset_type(adjust_op, direction);                         \
  arm_access_memory_##access_type(mem_type);                                  \
}                                                                             \

#define word_bit_count(word)                                                  \
  (bit_count[word >> 8] + bit_count[word & 0xFF])                             \

/* Clear the low 2 bits */
#define emit_align_reg(reg, numbits)                                          \
  arc_emit_lsri(reg, reg, numbits);                                           \
  arc_emit_asli(reg, reg, numbits)                                            \

/* Block transfers: a2 holds the base address throughout (the aligned
   stubs keep it); a0 = a2 + offset for each word */

#define arm_block_memory_load()                                               \
  generate_function_call(execute_aligned_load32);                             \
  generate_store_reg(reg_rv, i)                                               \

#define arm_block_memory_store()                                              \
  generate_load_reg_pc(reg_a1, i, 8);                                         \
  generate_function_call(execute_aligned_store32)                             \

#define arm_block_memory_final_load(writeback_type)                           \
  arm_block_memory_load()                                                     \

#define arm_block_memory_final_store(writeback_type)                          \
  generate_load_reg_pc(reg_a1, i, 12);                                        \
  arm_block_memory_writeback_post_store(writeback_type);                      \
  generate_mem_call(execute_store_u32, (pc + 4))                              \

#define arm_block_memory_adjust_pc_store()                                    \

#define arm_block_memory_adjust_pc_load()                                     \
  if(reg_list & 0x8000)                                                       \
  {                                                                           \
    generate_mov(reg_a0, reg_rv);                                             \
    generate_indirect_branch_arm();                                           \
  }                                                                           \

#define arm_block_memory_offset_down_a()                                      \
  arc_emit_subi(reg_a2, base_reg, ((word_bit_count(reg_list) * 4) - 4))       \

#define arm_block_memory_offset_down_b()                                      \
  arc_emit_subi(reg_a2, base_reg, (word_bit_count(reg_list) * 4))             \

#define arm_block_memory_offset_no()                                          \
  arc_emit_mov(0, reg_a2, base_reg)                                           \

#define arm_block_memory_offset_up()                                          \
  arc_emit_addi(reg_a2, base_reg, 4)                                          \

#define arm_block_memory_writeback_down()                                     \
  arc_emit_subi(base_reg, base_reg, (word_bit_count(reg_list) * 4))           \

#define arm_block_memory_writeback_up()                                       \
  arc_emit_addi(base_reg, base_reg, (word_bit_count(reg_list) * 4))           \

#define arm_block_memory_writeback_no()

/* Only emit writeback if the register is not in the list */
#define arm_block_memory_writeback_post_load(writeback_type)

#define arm_block_memory_writeback_pre_load(writeback_type)                   \
  if(!((reg_list >> rn) & 0x01))                                              \
  {                                                                           \
    arm_block_memory_writeback_##writeback_type();                            \
  }                                                                           \

#define arm_block_memory_writeback_pre_store(writeback_type)

#define arm_block_memory_writeback_post_store(writeback_type)                 \
  arm_block_memory_writeback_##writeback_type()                               \

#define arm_block_memory(access_type, offset_type, writeback_type, s_bit)     \
{                                                                             \
  arm_decode_block_trans();                                                   \
  u32 i;                                                                      \
  u32 offset = 0;                                                             \
  u32 base_reg = AREG(rn);                                                    \
                                                                              \
  arm_block_memory_offset_##offset_type();                                    \
  arm_block_memory_writeback_pre_##access_type(writeback_type);               \
  emit_align_reg(reg_a2, 2);                                                  \
                                                                              \
  for(i = 0; i < 16; i++)                                                     \
  {                                                                           \
    if((reg_list >> i) & 0x01)                                                \
    {                                                                         \
      cycle_count++;                                                          \
      arc_emit_addi(reg_a0, reg_a2, offset);                                  \
      if(reg_list & ~((2 << i) - 1))                                          \
      {                                                                       \
        arm_block_memory_##access_type();                                     \
        offset += 4;                                                          \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        arm_block_memory_final_##access_type(writeback_type);                 \
        break;                                                                \
      }                                                                       \
    }                                                                         \
  }                                                                           \
                                                                              \
  arm_block_memory_adjust_pc_##access_type();                                 \
}                                                                             \

/* ARM: rn must differ from rm and rd; rm may equal rd */
#define arm_swap(type)                                                        \
{                                                                             \
  arm_decode_swap();                                                          \
  cycle_count += 3;                                                           \
  generate_load_reg(reg_a0, rn);                                              \
  generate_mem_call(execute_load_##type, (pc));                               \
  generate_load_reg(reg_a0, rn);                                              \
  generate_load_reg(reg_a1, rm);                                              \
  generate_store_reg(reg_rv, rd);                                             \
  generate_mem_call(execute_store_##type, (pc + 4));                          \
}                                                                             \

#define thumb_generate_op_load_yes(_rs)                                       \
  generate_load_reg(reg_a1, _rs)                                              \

#define thumb_generate_op_load_no(_rs)                                        \

#define thumb_generate_op_reg(name, _rd, _rs, _rn)                            \
  generate_op_##name##_reg(AREG(_rd), AREG(_rs), AREG(_rn))                   \

#define thumb_generate_op_imm(name, _rd, _rs, _rn)                            \
  generate_op_##name##_imm(AREG(_rd), AREG(_rs))                              \

#define thumb_data_proc(type, name, rn_type, _rd, _rs, _rn)                   \
{                                                                             \
  thumb_decode_##type();                                                      \
  thumb_generate_op_##rn_type(name, _rd, _rs, _rn);                           \
}                                                                             \

#define thumb_data_proc_test(type, name, rn_type, _rs, _rn)                   \
{                                                                             \
  thumb_decode_##type();                                                      \
  thumb_generate_op_##rn_type(name, 0, _rs, _rn);                             \
}                                                                             \

#define thumb_data_proc_unary(type, name, rn_type, _rd, _rn)                  \
{                                                                             \
  thumb_decode_##type();                                                      \
  thumb_generate_op_##rn_type(name, _rd, 0, _rn);                             \
}                                                                             \

#define check_store_reg_pc_thumb(_rd)                                         \
  if(_rd == REG_PC)                                                           \
  {                                                                           \
    generate_indirect_branch_cycle_update(thumb);                             \
  }                                                                           \

#define thumb_data_proc_hi(name)                                              \
{                                                                             \
  thumb_decode_hireg_op();                                                    \
  u32 dest_rd = rd;                                                           \
  check_load_reg_pc(arm_reg_a0, rs, 4);                                       \
  check_load_reg_pc(arm_reg_a1, rd, 4);                                       \
  generate_op_##name##_reg(AREG(dest_rd), AREG(rd), AREG(rs));                \
  check_store_reg_pc_thumb(dest_rd);                                          \
}                                                                             \

#define thumb_data_proc_test_hi(name)                                         \
{                                                                             \
  thumb_decode_hireg_op();                                                    \
  check_load_reg_pc(arm_reg_a0, rs, 4);                                       \
  check_load_reg_pc(arm_reg_a1, rd, 4);                                       \
  generate_op_##name##_reg(reg_temp, AREG(rd), AREG(rs));                     \
}                                                                             \

#define thumb_data_proc_mov_hi()                                              \
{                                                                             \
  thumb_decode_hireg_op();                                                    \
  check_load_reg_pc(arm_reg_a0, rs, 4);                                       \
  generate_mov(AREG(rd), AREG(rs));                                           \
  check_store_reg_pc_thumb(rd);                                               \
}                                                                             \

#define thumb_load_pc(_rd)                                                    \
{                                                                             \
  thumb_decode_imm();                                                         \
  generate_load_pc(AREG(_rd), (((pc & ~2) + 4) + (imm * 4)));                 \
}                                                                             \

#define thumb_load_sp(_rd)                                                    \
{                                                                             \
  thumb_decode_imm();                                                         \
  arc_emit_addi(AREG(_rd), reg_r13, (imm * 4));                               \
}                                                                             \

#define thumb_adjust_sp_up()                                                  \
  arc_emit_addi(reg_r13, reg_r13, (imm * 4))                                  \

#define thumb_adjust_sp_down()                                                \
  arc_emit_subi(reg_r13, reg_r13, (imm * 4))                                  \

#define thumb_adjust_sp(direction)                                            \
{                                                                             \
  thumb_decode_add_sp();                                                      \
  thumb_adjust_sp_##direction();                                              \
}                                                                             \

#define thumb_generate_shift_imm(name)                                        \
  if(check_generate_c_flag)                                                   \
  {                                                                           \
    generate_shift_imm_##name##_flags(rd, rs, imm);                           \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_shift_imm_##name##_no_flags(rd, rs, imm);                        \
  }                                                                           \
  if(rs != rd)                                                                \
  {                                                                           \
    generate_mov(AREG(rd), AREG(rs));                                         \
  }                                                                           \

#define thumb_generate_shift_reg(name)                                        \
{                                                                             \
  u32 original_rd = rd;                                                       \
  if(check_generate_c_flag)                                                   \
  {                                                                           \
    generate_shift_reg_##name##_flags(rd, rs);                                \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_shift_reg_##name##_no_flags(rd, rs);                             \
  }                                                                           \
  generate_mov(AREG(original_rd), reg_a0);                                    \
}                                                                             \

#define thumb_shift(decode_type, op_type, value_type)                         \
{                                                                             \
  thumb_decode_##decode_type();                                               \
  thumb_generate_shift_##value_type(op_type);                                 \
  generate_op_logic_flags(AREG(rd));                                          \
}                                                                             \

#define thumb_access_memory_load(mem_type, reg_rd)                            \
  cycle_count += 2;                                                           \
  generate_mem_call(execute_load_##mem_type, (pc));                           \
  generate_store_reg(reg_rv, reg_rd)                                          \

#define thumb_access_memory_store(mem_type, reg_rd)                           \
  cycle_count++;                                                              \
  generate_load_reg(reg_a1, reg_rd);                                          \
  generate_mem_call(execute_store_##mem_type, (pc + 2))                       \

#define thumb_access_memory_generate_address_pc_relative(offset, reg_rb,      \
 reg_ro)                                                                      \
  generate_load_pc(reg_a0, (offset))                                          \

#define thumb_access_memory_generate_address_reg_imm(offset, reg_rb, reg_ro)  \
  arc_emit_addi(reg_a0, AREG(reg_rb), (offset))                               \

#define thumb_access_memory_generate_address_reg_imm_sp(offset, reg_rb,       \
 reg_ro)                                                                      \
  arc_emit_addi(reg_a0, AREG(reg_rb), (offset * 4))                           \

#define thumb_access_memory_generate_address_reg_reg(offset, reg_rb, reg_ro)  \
  arc_emit_add(reg_a0, AREG(reg_rb), AREG(reg_ro))                            \

#define thumb_access_memory(access_type, op_type, reg_rd, reg_rb, reg_ro,     \
 address_type, offset, mem_type)                                              \
{                                                                             \
  thumb_decode_##op_type();                                                   \
  thumb_access_memory_generate_address_##address_type(offset, reg_rb,         \
   reg_ro);                                                                   \
  thumb_access_memory_##access_type(mem_type, reg_rd);                        \
}                                                                             \

#define thumb_block_address_preadjust_no(base_reg)                            \
  arc_emit_mov(0, reg_a2, AREG(base_reg))                                     \

#define thumb_block_address_preadjust_down(base_reg)                          \
  arc_emit_subi(reg_a2, AREG(base_reg), (bit_count[reg_list] * 4));           \
  arc_emit_mov(0, AREG(base_reg), reg_a2)                                     \

#define thumb_block_address_preadjust_push_lr(base_reg)                       \
  arc_emit_subi(reg_a2, AREG(base_reg), ((bit_count[reg_list] + 1) * 4));     \
  arc_emit_mov(0, AREG(base_reg), reg_a2)                                     \

#define thumb_block_address_postadjust_no(base_reg)                           \

#define thumb_block_address_postadjust_up(base_reg)                           \
  arc_emit_addi(AREG(base_reg), reg_a2, (bit_count[reg_list] * 4))            \

#define thumb_block_address_postadjust_pop_pc(base_reg)                       \
  arc_emit_addi(AREG(base_reg), reg_a2, ((bit_count[reg_list] * 4) + 4))      \

#define thumb_block_address_postadjust_push_lr(base_reg)                      \

#define thumb_block_memory_load()                                             \
  generate_function_call(execute_aligned_load32);                             \
  generate_store_reg(reg_rv, i)                                               \

#define thumb_block_memory_store()                                            \
  generate_load_reg(reg_a1, i);                                               \
  generate_function_call(execute_aligned_store32)                             \

#define thumb_block_memory_final_load()                                       \
  thumb_block_memory_load()                                                   \

#define thumb_block_memory_final_store()                                      \
  generate_load_reg(reg_a1, i);                                               \
  generate_mem_call(execute_store_u32, (pc + 2))                              \

#define thumb_block_memory_final_no(access_type)                              \
  thumb_block_memory_final_##access_type()                                    \

#define thumb_block_memory_final_up(access_type)                              \
  thumb_block_memory_final_##access_type()                                    \

#define thumb_block_memory_final_down(access_type)                            \
  thumb_block_memory_final_##access_type()                                    \

#define thumb_block_memory_final_push_lr(access_type)                         \
  thumb_block_memory_##access_type()                                          \

#define thumb_block_memory_final_pop_pc(access_type)                          \
  thumb_block_memory_##access_type()                                          \

#define thumb_block_memory_extra_no()                                         \

#define thumb_block_memory_extra_up()                                         \

#define thumb_block_memory_extra_down()                                       \

#define thumb_block_memory_extra_push_lr()                                    \
  arc_emit_addi(reg_a0, reg_a2, (bit_count[reg_list] * 4));                   \
  generate_load_reg(reg_a1, REG_LR);                                          \
  generate_mem_call(execute_store_u32, (pc + 2))                              \

#define thumb_block_memory_extra_pop_pc()                                     \
  arc_emit_addi(reg_a0, reg_a2, (bit_count[reg_list] * 4));                   \
  generate_function_call(execute_aligned_load32);                             \
  generate_mov(reg_a0, reg_rv);                                               \
  generate_indirect_branch_cycle_update(thumb)                                \

#define thumb_block_memory(access_type, pre_op, post_op, base_reg)            \
{                                                                             \
  thumb_decode_rlist();                                                       \
  u32 i;                                                                      \
  u32 offset = 0;                                                             \
                                                                              \
  thumb_block_address_preadjust_##pre_op(base_reg);                           \
  thumb_block_address_postadjust_##post_op(base_reg);                         \
  emit_align_reg(reg_a2, 2);                                                  \
                                                                              \
  for(i = 0; i < 8; i++)                                                      \
  {                                                                           \
    if((reg_list >> i) & 0x01)                                                \
    {                                                                         \
      cycle_count++;                                                          \
      arc_emit_addi(reg_a0, reg_a2, offset);                                  \
      if(reg_list & ~((2 << i) - 1))                                          \
      {                                                                       \
        thumb_block_memory_##access_type();                                   \
        offset += 4;                                                          \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        thumb_block_memory_final_##post_op(access_type);                      \
        break;                                                                \
      }                                                                       \
    }                                                                         \
  }                                                                           \
                                                                              \
  thumb_block_memory_extra_##post_op();                                       \
}                                                                             \

/* A bcc right after a cmp/sub/add/tst (none of whose code after the .f op
   touches the ARC flags) can branch on the ARC flags: unless it is a branch
   target (entered with other flags) or the cheat hook (a call before it).
   The dead flag pass knows this too (thumb_branch_flags). */
#define thumb_conditional_branch(condition)                                   \
{                                                                             \
  u32 native_cc = 0;                                                          \
  if(block_data_position && !block_data[block_data_position].update_cycles && \
   (pc != cheat_master_hook))                                                 \
    native_cc = arc_thumb_native_cc(last_opcode, (opcode >> 8) & 0xF);        \
  if(native_cc)                                                               \
  {                                                                           \
    generate_cycle_update_force();                                            \
    arc_emit_bcc_filler(arc_cc_inverse[native_cc], backpatch_address);        \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_condition_##condition();                                         \
  }                                                                           \
  generate_branch_no_cycle_update(                                            \
   block_exits[block_exit_position].branch_source,                            \
   block_exits[block_exit_position].branch_target);                           \
  generate_branch_patch_conditional(backpatch_address, translation_ptr);      \
  block_exit_position++;                                                      \
}                                                                             \

#define arm_conditional_block_header()                                        \
  generate_condition();                                                       \

#define arm_b()                                                               \
  generate_branch()                                                           \

#define arm_bl()                                                              \
  generate_load_pc(reg_r14, (pc + 4));                                        \
  generate_branch()                                                           \

#define arm_bx()                                                              \
  arm_decode_branchx(opcode);                                                 \
  generate_load_reg_pc(reg_a0, rn, 8);                                        \
  generate_indirect_branch_dual()                                             \

#define arm_swi()                                                             \
  generate_load_pc(reg_a0, (pc + 4));                                         \
  generate_function_call(execute_swi);                                        \
  generate_branch()                                                           \

#define thumb_b()                                                             \
  generate_branch_cycle_update(                                               \
   block_exits[block_exit_position].branch_source,                            \
   block_exits[block_exit_position].branch_target);                           \
  block_exit_position++                                                       \

#define thumb_bl()                                                            \
  generate_load_pc(reg_r14, ((pc + 2) | 0x01));                               \
  generate_branch_cycle_update(                                               \
   block_exits[block_exit_position].branch_source,                            \
   block_exits[block_exit_position].branch_target);                           \
  block_exit_position++                                                       \

#define thumb_blh()                                                           \
{                                                                             \
  thumb_decode_branch();                                                      \
  arc_emit_addi(reg_a0, reg_r14, (offset * 2));                               \
  generate_load_pc(reg_r14, ((pc + 2) | 0x01));                               \
  generate_indirect_branch_cycle_update(thumb);                               \
}                                                                             \

#define thumb_bx()                                                            \
{                                                                             \
  thumb_decode_hireg_op();                                                    \
  generate_load_reg_pc(reg_a0, rs, 4);                                        \
  generate_indirect_branch_cycle_update(dual);                                \
}                                                                             \

#define thumb_process_cheats()                                                \
  generate_function_call(arc_cheat_hook);

#define arm_process_cheats()                                                  \
  generate_function_call(arc_cheat_hook);

#define emit_trace_thumb_instruction(pc)
#define emit_trace_arm_instruction(pc)

#define thumb_swi()                                                           \
  generate_load_pc(reg_a0, (pc + 2));                                         \
  generate_function_call(execute_swi);                                        \
  generate_branch_cycle_update(                                               \
   block_exits[block_exit_position].branch_source,                            \
   block_exits[block_exit_position].branch_target);                           \
  block_exit_position++                                                       \

/* BIOS Div (r0 / r1) and DivArm (r1 / r0), done in the stub */
#define arm_hle_div(cpu_mode)                                                 \
  generate_function_call(arc_hle_div)                                         \

#define arm_hle_div_arm(cpu_mode)                                             \
  generate_function_call(arc_hle_div_arm)                                     \

#define generate_translation_gate(type)                                       \
  generate_load_pc(reg_a0, pc);                                               \
  generate_indirect_branch_no_cycle_update(type)                              \

#define generate_update_pc_reg()                                              \
  generate_load_pc(reg_a0, pc);                                               \
  arc_emit_st(reg_a0, reg_base, (REG_PC * 4))                                 \

void init_emitter(bool must_swap)
{
  (void)must_swap;
  rom_cache_watermark = INITIAL_ROM_WATERMARK;
  init_bios_hooks();
}

u32 execute_arm_translate_internal(u32 cycles, void *regptr);
u32 execute_arm_translate(u32 cycles)
{
  return execute_arm_translate_internal(cycles, &reg[0]);
}

#endif
