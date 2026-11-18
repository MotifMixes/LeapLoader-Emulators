/* gameplaySP ARC (ARC600) dynarec stubs.
 *
 * Register use (see arc_emit.h): r0-r2 a0-a2, r3 rv, r4 temp, r12 temp2,
 * r5 &reg[0], r6 cycles, r7-r10 N/Z/C/V, r11 GBA r14, r13-r25 GBA r0-r12,
 * fp GBA r13. gp (r26) is the BIOS's and is never used.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include "../gpsp_config.h"

#define REG_PC          (15 * 4)
#define REG_CPSR        (16 * 4)
#define CPU_MODE        (17 * 4)
#define CPU_HALT_STATE  (18 * 4)
#define REG_BUS_VALUE   (19 * 4)
#define REG_SAVE        (26 * 4)
#define REG_SAVE2       (27 * 4)
#define REG_SAVE3       (28 * 4)
#define REG_SAVE4       (29 * 4)
#define OAM_UPDATED_OFS (25 * 4)        /* reg[OAM_UPDATED] */

#define CPU_ALERT_HALT  1
#define CPU_ALERT_SMC   2
#define CPU_ALERT_IRQ   4

#define SUPERVISOR_SPSR (3 * 4)
#define SUPERVISOR_LR   ((3 * 7 + 6) * 4)


  .text
  .align 4

.macro defsymbl sym
  .global \sym
  .type \sym, @function
\sym:
.endm

.macro push r
  st.a \r, [sp, -4]
.endm

.macro pop r
  ld.ab \r, [sp, 4]
.endm

/* GBA registers <-> reg[] */
.macro save_registers
  st r13, [r5, 0]
  st r14, [r5, 4]
  st r15, [r5, 8]
  st r16, [r5, 12]
  st r17, [r5, 16]
  st r18, [r5, 20]
  st r19, [r5, 24]
  st r20, [r5, 28]
  st r21, [r5, 32]
  st r22, [r5, 36]
  st r23, [r5, 40]
  st r24, [r5, 44]
  st r25, [r5, 48]
  st fp,  [r5, 52]
  st r11, [r5, 56]
.endm

.macro restore_registers
  ld r13, [r5, 0]
  ld r14, [r5, 4]
  ld r15, [r5, 8]
  ld r16, [r5, 12]
  ld r17, [r5, 16]
  ld r18, [r5, 20]
  ld r19, [r5, 24]
  ld r20, [r5, 28]
  ld r21, [r5, 32]
  ld r22, [r5, 36]
  ld r23, [r5, 40]
  ld r24, [r5, 44]
  ld r25, [r5, 48]
  ld fp,  [r5, 52]
  ld r11, [r5, 56]
.endm

/* N/Z/C/V registers -> CPSR (result left in r3) */
.macro collapse_flags
  ld r3, [r5, REG_CPSR]
  bmsk r3, r3, 7
  asl r4, r7, 31
  or r3, r3, r4
  asl r4, r8, 30
  or r3, r3, r4
  asl r4, r9, 29
  or r3, r3, r4
  asl r4, r10, 28
  or r3, r3, r4
  st r3, [r5, REG_CPSR]
.endm

/* CPSR in \src -> N/Z/C/V registers */
.macro extract_flags_from src
  lsr r7, \src, 31
  lsr r8, \src, 30
  bmsk r8, r8, 0
  lsr r9, \src, 29
  bmsk r9, r9, 0
  lsr r10, \src, 28
  bmsk r10, r10, 0
.endm

.macro extract_flags
  ld r4, [r5, REG_CPSR]
  extract_flags_from r4
.endm

/* Call C with the host state that C may clobber (r2, r6-r11) kept */
.macro call_c fn
  push r2
  push r6
  push r7
  push r8
  push r9
  push r10
  push r11
  push blink
  bl \fn
  pop blink
  pop r11
  pop r10
  pop r9
  pop r8
  pop r7
  pop r6
  pop r2
  mov r5, reg
.endm

/* Called (BL) every time the cycle counter runs out. r0: next pc */
/* from a branch exit: blink -> its b, the new pc is the word after it */
defsymbl arc_update_gba_w
  ld r0, [blink, 4]
defsymbl arc_update_gba
  st r0, [r5, REG_PC]
  st blink, [r5, REG_SAVE2]
  collapse_flags
  save_registers
  mov r0, r6
  bl update_gba
  mov r5, reg
  tst r0, r0
  bmi return_to_main         /* frame complete (bit 31) */
  and r6, r0, 0x7FFF
  btst r0, 30                        /* pc changed? */
  bne lookup_pc
  restore_registers
  extract_flags
  ld blink, [r5, REG_SAVE2]
  j [blink]

defsymbl arc_cheat_hook
  st blink, [r5, REG_SAVE2]
  collapse_flags
  save_registers
  st r6, [r5, REG_SAVE]
  bl process_cheats
  mov r5, reg
  ld r6, [r5, REG_SAVE]
  restore_registers
  extract_flags
  ld blink, [r5, REG_SAVE2]
  j [blink]

/* Back to execute_arm_translate's caller; r0 = update_gba result */
return_to_main:
  ld blink, [sp, 56]
  ld fp, [sp, 52]
  ld r25, [sp, 48]
  ld r24, [sp, 44]
  ld r23, [sp, 40]
  ld r22, [sp, 36]
  ld r21, [sp, 32]
  ld r20, [sp, 28]
  ld r19, [sp, 24]
  ld r18, [sp, 20]
  ld r17, [sp, 16]
  ld r16, [sp, 12]
  ld r15, [sp, 8]
  ld r14, [sp, 4]
  ld r13, [sp, 0]
  add sp, sp, 64
  j [blink]

/* Indirect branches (jumped to); r0: GBA address.
   arc_ibc: 512 direct-mapped {key, code} pairs, key = pc | thumb, cleared
   with the translation caches (cpu_threaded.c). VRAM code is not cached. */
.macro ibc_entry
  lsr r12, r4, 1
  bmsk r12, r12, 8
  mov r1, arc_ibc
  add3 r12, r1, r12
.endm

.macro ibc_lookup miss
  ibc_entry
  ld r1, [r12, 0]
  brne r1, r4, \miss
  ld r1, [r12, 4]
  j [r1]
.endm

.macro ibc_miss fn
  push r4
  call_c \fn
  pop r4
  breq r0, 0, 2f
  lsr r12, r4, 24
  breq r12, 6, 2f
  ibc_entry
  st r4, [r12, 0]
  st r0, [r12, 4]
2:
  j [r0]
.endm

defsymbl arc_indirect_branch_arm
  and r4, r0, ~3
  ibc_lookup 1f
1:
  mov r0, r4
  ibc_miss block_lookup_address_arm

defsymbl arc_indirect_branch_thumb
  bset r4, r0, 0
  ibc_lookup 1f
1:
  bclr r0, r0, 0
  ibc_miss block_lookup_address_thumb

/* As block_lookup_address_dual: bit 0 picks the mode (CPSR T) */
defsymbl arc_indirect_branch_dual
  ld r12, [r5, REG_CPSR]
  btst r0, 0
  beq 1f
  bset r12, r12, 5
  st r12, [r5, REG_CPSR]
  b arc_indirect_branch_thumb
1:
  bclr r12, r12, 5
  st r12, [r5, REG_CPSR]
  add r0, r0, 2
  b arc_indirect_branch_arm

/* Enter VRAM code through the interpreter; r0: pc */
defsymbl execute_vram_arm
  st r0, [r5, REG_PC]
  collapse_flags
  bclr r3, r3, 5
  b execute_vram_common
defsymbl execute_vram_thumb
  st r0, [r5, REG_PC]
  collapse_flags
  bset r3, r3, 5
execute_vram_common:
  st r3, [r5, REG_CPSR]
  save_registers
  mov r0, r6
  bl execute_arm_vram
  mov r5, reg
  tst r0, r0
  bmi return_to_main
  and r6, r0, 0x7FFF
  b lookup_pc

/* Resume at reg[REG_PC] (GBA registers and CPSR in reg[], cycles in r6) */
lookup_pc:
  st r6, [r5, REG_SAVE]
  ld r0, [r5, REG_PC]
  ld r1, [r5, REG_CPSR]
  btst r1, 5
  bne 1f
  bl block_lookup_address_arm
  b 2f
1:
  bl block_lookup_address_thumb
2:
  mov r5, reg
  ld r6, [r5, REG_SAVE]
  restore_registers
  extract_flags
  j [r0]

/* A store reported alerts (r0); reg[REG_PC] holds the next pc */
write_epilogue:
  st r0, [r5, REG_SAVE3]
  collapse_flags
  save_registers
  st r6, [r5, REG_SAVE]
  ld r0, [r5, REG_SAVE3]
  btst r0, 1                         /* CPU_ALERT_SMC */
  beq 1f
  bl flush_translation_cache_ram
  mov r5, reg
1:
  ld r0, [r5, REG_SAVE3]
  btst r0, 2                         /* CPU_ALERT_IRQ */
  beq 2f
  bl check_and_raise_interrupts
  mov r5, reg
2:
  ld r6, [r5, REG_SAVE]
  ld r0, [r5, REG_SAVE3]
  btst r0, 0                         /* CPU_ALERT_HALT */
  beq lookup_pc
cpu_sleep_loop:
  mov r0, r6
  bl update_gba
  mov r5, reg
  tst r0, r0
  bmi return_to_main
  and r6, r0, 0x7FFF
  b lookup_pc

/* Code was written: flush the RAM translations and resume at r2 (next pc) */
smc_write:
  ld r2, [blink, -4]
  st r2, [r5, REG_PC]
  mov r0, CPU_ALERT_SMC
  b write_epilogue

defsymbl execute_read_cpsr
  collapse_flags
  j [blink]

defsymbl execute_read_spsr
  ld r3, [r5, CPU_MODE]
  bmsk r3, r3, 3
  mov r4, spsr
  add2 r4, r4, r3
  ld r3, [r4, 0]
  j [blink]

/* SWI: r0 = return pc */
defsymbl execute_swi
  st blink, [r5, REG_SAVE3]
  st r0, [reg_mode + SUPERVISOR_LR]
  collapse_flags
  st r3, [spsr + SUPERVISOR_SPSR]
  lsr r3, r3, 6
  asl r3, r3, 6
  or r3, r3, 0x93                    /* supervisor, IRQs off */
  st r3, [r5, REG_CPSR]
  save_registers
  st r6, [r5, REG_SAVE]
  mov r0, 0x13
  bl set_cpu_mode
  mov r5, reg
  ld r6, [r5, REG_SAVE]
  restore_registers
  extract_flags
  mov r4, 0xe3a02004                 /* open bus value */
  st r4, [r5, REG_BUS_VALUE]
  ld blink, [r5, REG_SAVE3]
  j [blink]

/* r0: pc to restore to; returns the new pc in r0 */
defsymbl execute_spsr_restore
  ld r4, [r5, CPU_MODE]
  bmsk.f r4, r4, 3
  jeq [blink]                        /* user/system: no spsr */
  mov r12, spsr
  add2 r12, r12, r4
  ld r4, [r12, 0]
  st r4, [r5, REG_CPSR]
  extract_flags_from r4
  st blink, [r5, REG_SAVE3]
  save_registers
  st r6, [r5, REG_SAVE]
  bl execute_spsr_restore_body
  mov r5, reg
  ld r6, [r5, REG_SAVE]
  restore_registers
  extract_flags
  ld blink, [r5, REG_SAVE3]
  j [blink]

/* r0: new cpsr, r1: pc, r2: store mask (user), r3: store mask (privileged) */
defsymbl execute_store_cpsr
  ld r4, [r5, CPU_MODE]
  mov r12, r2
  btst r4, 4
  mov.ne r12, r3
  and r0, r0, r12
  ld r4, [r5, REG_CPSR]
  bic r4, r4, r12
  or r4, r4, r0
  st r4, [r5, REG_CPSR]
  extract_flags_from r4
  bmsk.f r12, r12, 7                 /* mode bits written? */
  jeq [blink]
  st blink, [r5, REG_SAVE3]
  save_registers
  st r6, [r5, REG_SAVE]
  mov r0, r4
  bl execute_store_cpsr_body         /* (cpsr, pc) */
  mov r5, reg
  ld r6, [r5, REG_SAVE]
  brne r0, 0, 1f
  restore_registers
  extract_flags
  ld blink, [r5, REG_SAVE3]
  j [blink]
1:                                   /* an IRQ was taken: r0 = new pc */
  st r0, [r5, REG_PC]
  b lookup_pc

/* r0: new spsr, r1: store mask */
defsymbl execute_store_spsr
  ld r4, [r5, CPU_MODE]
  bmsk r4, r4, 3
  mov r12, spsr
  add2 r12, r12, r4
  ld r4, [r12, 0]
  and r0, r0, r1
  bic r4, r4, r1
  or r4, r4, r0
  st r4, [r12, 0]
  j [blink]

/* BIOS Div: r0 = r0 / r1, r1 = r0 % r1, r3 = abs(r0 / r1) */
defsymbl arc_hle_div
  mov r2, r13
  mov r3, r14
  b hle_div_common
defsymbl arc_hle_div_arm
  mov r2, r14
  mov r3, r13
hle_div_common:
  breq r3, 0, 1f                     /* division by zero: leave as is */
  push r6
  push r7
  push r8
  push r9
  push r10
  push r11
  push blink
  push r2
  push r3
  mov r0, r2
  mov r1, r3
  bl __modsi3
  mov r14, r0
  ld r0, [sp, 4]
  ld r1, [sp, 0]
  bl __divsi3
  mov r13, r0
  abs r16, r0
  pop r3
  pop r2
  pop blink
  pop r11
  pop r10
  pop r9
  pop r8
  pop r7
  pop r6
  mov r5, reg
1:
  j [blink]

/* r0: cycles, r1: &reg[0] */
defsymbl execute_arm_translate_internal
  sub sp, sp, 64
  st r13, [sp, 0]
  st r14, [sp, 4]
  st r15, [sp, 8]
  st r16, [sp, 12]
  st r17, [sp, 16]
  st r18, [sp, 20]
  st r19, [sp, 24]
  st r20, [sp, 28]
  st r21, [sp, 32]
  st r22, [sp, 36]
  st r23, [sp, 40]
  st r24, [sp, 44]
  st r25, [sp, 48]
  st fp, [sp, 52]
  st blink, [sp, 56]
  mov r5, r1
  mov r6, r0
  ld r4, [r5, CPU_HALT_STATE]
  tst r4, r4
  bne cpu_sleep_loop
  b lookup_pc

/* ------------------------------------------------------------------------
 * Memory loads: r0 address; result in r3. The pc is a data word after the
 * call (bl stub; .word pc): the stubs return past it.
 * Fast path: aligned access to IWRAM, EWRAM or a page mapped in
 * memory_map_read, except the BIOS (0x0, read protection) and I/O (0x4).
 * Keeps r2 and r5-r11 (and the GBA registers).
 */

/* r3 = \ld_op of r0, then return; or branch to \slow. IWRAM (0x3: data at
   iwram + 0x8000 + (a & 0x7FFF)) and EWRAM (0x2: ewram + (a & 0x3FFFF)) are read
   directly, other pages through memory_map_read. */
.macro map_read slow, alignmask, ld_op
  .if \alignmask
  btst r0, 0
  bne \slow
  .endif
  .if \alignmask == 3
  btst r0, 1
  bne \slow
  .endif
  lsr r4, r0, 24
  breq.d r4, 3, 2f
  bmsk r12, r0, 14
  breq.d r4, 2, 3f
  bmsk r12, r0, 17
  breq r4, 0, \slow
  breq r4, 4, \slow
  brhs r4, 0x10, \slow
  lsr r4, r0, 15
  mov r12, memory_map_read
  add2 r4, r12, r4
  ld r4, [r4, 0]
  breq.d r4, 0, \slow
  bmsk r12, r0, 14
  \ld_op r3, [r4, r12]
  j [blink]
2:
  \ld_op r3, [r12, iwram + 0x8000]
  j [blink]
3:
  \ld_op r3, [r12, ewram]
  j [blink]
.endm

.macro load_slow fn
  ld r1, [blink, -4]                 /* the pc, a data word after the call */
  st r1, [r5, REG_PC]
  call_c \fn
  mov r3, r0
  j [blink]
.endm

defsymbl arc_load_u8
  add blink, blink, 4
  map_read 1f, 0, ldb
1:
  load_slow read_memory8

defsymbl arc_load_s8
  add blink, blink, 4
  map_read 1f, 0, ldb.x
1:
  load_slow read_memory8s

defsymbl arc_load_u16
  add blink, blink, 4
  map_read 1f, 1, ldw
1:
  load_slow read_memory16

defsymbl arc_load_s16
  add blink, blink, 4
  map_read 1f, 1, ldw.x
1:
  load_slow read_memory16s

defsymbl arc_load_u32
  add blink, blink, 4
  map_read 1f, 3, ld
1:
  load_slow read_memory32

/* Block transfers: address already aligned, keeps r2 */
defsymbl arc_load_aligned32
  map_read 1f, 0, ld
1:
  call_c read_memory32
  mov r3, r0
  j [blink]

/* ------------------------------------------------------------------------
 * Memory stores: r0 address, r1 value; the data word after the call is the
 * pc of the next instruction (r2 only on the slow paths).
 * EWRAM/IWRAM are written here, with the code-written (SMC) check against
 * the translation tags; everything else goes to write_memoryN, whose
 * alerts end the block through write_epilogue.
 */

/* EWRAM (0x2): data at ewram + (a & 0x3FFFF), tags 0x40000 above.
   IWRAM (0x3): data at iwram + 0x8000 + (a & 0x7FFF), tags at iwram.
   (s9 offsets cannot reach 0x8000, hence the adds.) */
.macro iwram_store st_op, ld_op, smc
  \ld_op r12, [r4, 0]
  add r4, r4, 0x8000
  \st_op r1, [r4, 0]
  tst r12, r12
  bne \smc
  j [blink]
.endm

.macro store_slow fn
  ld r2, [blink, -4]
  st r2, [r5, REG_PC]
  call_c \fn
  tst r0, r0
  bne write_epilogue
  j [blink]
.endm

.macro store_fast st_op, ld_op, alignbits, smc, size
  lsr r4, r0, 24
  brne r4, 2, 3f
  bmsk r4, r0, 17
  .if \alignbits
  lsr r4, r4, \alignbits
  asl r4, r4, \alignbits
  .endif
  mov r12, ewram
  add r4, r4, r12
  \st_op r1, [r4, 0]
  add r4, r4, 0x40000
  \ld_op r12, [r4, 0]
  tst r12, r12
  bne \smc
  j [blink]
3:
  brne r4, 3, 4f
  bmsk r4, r0, 14
  .if \alignbits
  lsr r4, r4, \alignbits
  asl r4, r4, \alignbits
  .endif
  mov r12, iwram
  add r4, r4, r12
  iwram_store \st_op, \ld_op, \smc
4:
  .if \size > 8
  video_store \st_op, \size
  .endif
.endm

/* r12 = BGR555 \src as RGB565 (convert_palette); uses r4 */
.macro conv_palette src
  and r12, \src, 0x1F
  asl r12, r12, 11
  and r4, \src, 0x3E0
  add r4, r4, r4
  or r12, r12, r4
  lsr r4, \src, 10
  and r4, r4, 0x1F
  or r12, r12, r4
.endm

/* 16/32-bit stores to palette (0x5), VRAM (0x6) and OAM (0x7), as
   write_memoryN does them; r4 = region. 8-bit ones stay in C (they are special). */
.macro video_store st_op, size
  breq r4, 6, 6f
  breq r4, 7, 7f
  brne r4, 5, 8f
  .if \size == 16
  and r0, r0, 0x3FE
  .else
  and r0, r0, 0x3FC
  .endif
  lsr r4, r0, 6                      /* LL: ll_pal_dirty[entry / 32] = ~0 */
  mov r12, ll_pal_dirty
  add2 r4, r12, r4
  mov r12, -1
  st r12, [r4, 0]
  mov r12, palette_ram
  add r4, r12, r0
  \st_op r1, [r4, 0]
  conv_palette r1
  mov r4, palette_ram_converted
  add r4, r4, r0
  stw r12, [r4, 0]
  .if \size == 32
  lsr r1, r1, 16
  conv_palette r1
  mov r4, palette_ram_converted + 2
  add r4, r4, r0
  stw r12, [r4, 0]
  .endif
  j [blink]
6:
  bmsk r4, r0, 16                    /* 0x18000-0x1FFFF mirrors 0x10000 */
  mov r12, 0x18000
  brlo r4, r12, 9f
  sub r4, r4, 0x8000
9:
  bclr r4, r4, 0
  .if \size == 32
  bclr r4, r4, 1
  .endif
  mov r12, vram
  add r4, r4, r12
  \st_op r1, [r4, 0]
  j [blink]
7:
  .if \size == 16
  and r4, r0, 0x3FE
  .else
  and r4, r0, 0x3FC
  .endif
  mov r12, oam_ram
  add r4, r4, r12
  \st_op r1, [r4, 0]
  mov r12, 1
  st r12, [r5, OAM_UPDATED_OFS]
  j [blink]
8:
.endm

defsymbl arc_store_u8
  add blink, blink, 4
  store_fast stb, ldb, 0, smc_write, 8
  store_slow write_memory8

defsymbl arc_store_u16
  add blink, blink, 4
  store_fast stw, ldw, 1, smc_write, 16
  store_slow write_memory16

defsymbl arc_store_u32
  add blink, blink, 4
  ld r4, [r5, REG_SAVE4]             /* alerts left by block stores */
  tst r4, r4
  bne 5f
  store_fast st, ld, 2, smc_write, 32
  store_slow write_memory32
5:
  mov r12, 0
  st r12, [r5, REG_SAVE4]
  ld r2, [blink, -4]
  st r2, [r5, REG_PC]
  push r4
  call_c write_memory32
  pop r4
  or r0, r0, r4
  tst r0, r0
  bne write_epilogue
  j [blink]

/* Block transfer stores: no pc here, alerts are kept for the final store
   (arc_store_u32); keeps r2 */
smc_pending:
  ld r12, [r5, REG_SAVE4]
  or r12, r12, CPU_ALERT_SMC
  st r12, [r5, REG_SAVE4]
  j [blink]

defsymbl arc_store_aligned32
  store_fast st, ld, 0, smc_pending, 32
  call_c write_memory32
  ld r12, [r5, REG_SAVE4]
  or r12, r12, r0
  st r12, [r5, REG_SAVE4]
  j [blink]

/* ------------------------------------------------------------------------ */

  .bss
  .align 64
defsymbl rom_translation_cache
  .space ROM_TRANSLATION_CACHE_SIZE
defsymbl ram_translation_cache
  .space RAM_TRANSLATION_CACHE_SIZE
