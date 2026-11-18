/* offs65.c: struct offsets for ll65.S (kern/gen65.py). Compiled with -S only; core.sh keeps
   the .equ lines it prints (s9x/src/lloffs65.inc). */
#include <stddef.h>
#include "snes9x.h"
#include "memmap.h"
#include "cpuexec.h"
#include "apu.h"
#define O(n, v) __asm__ volatile (".equ " #n ", %0" :: "i" (v))
void LL65Offsets(void)
{
   O(C_FLAGS, offsetof(SCPUState, Flags));
   O(C_BRANCHSKIP, offsetof(SCPUState, BranchSkip));
   O(C_IRQACTIVE, offsetof(SCPUState, IRQActive));
   O(C_WAITINT, offsetof(SCPUState, WaitingForInterrupt));
   O(C_PB, offsetof(SCPUState, Regs.PB));
   O(C_DB, offsetof(SCPUState, Regs.DB));
   O(C_PL, offsetof(SCPUState, Regs.P.W));
   O(C_A, offsetof(SCPUState, Regs.A.W));
   O(C_D, offsetof(SCPUState, Regs.D.W));
   O(C_X, offsetof(SCPUState, Regs.X.W));
   O(C_S, offsetof(SCPUState, Regs.S.W));
   O(C_Y, offsetof(SCPUState, Regs.Y.W));
   O(C_PC, offsetof(SCPUState, PC));
   O(C_CYCLES, offsetof(SCPUState, Cycles));
   O(C_PCBASE, offsetof(SCPUState, PCBase));
   O(C_PCOP, offsetof(SCPUState, PCAtOpcodeStart));
   O(C_WAITADDR, offsetof(SCPUState, WaitAddress));
   O(C_NEXTEVENT, offsetof(SCPUState, NextEvent));
   O(C_MEMSPEED, offsetof(SCPUState, MemSpeed));
   O(C_MEMSPEED2, offsetof(SCPUState, MemSpeedx2));
   O(C_APUEXEC, offsetof(SCPUState, APU_APUExecuting));
   O(C_IRQCOUNT, offsetof(SCPUState, IRQCycleCount));
   O(S_DISABLEIRQ, offsetof(SSettings, DisableIRQ));
   O(I_CARRY, offsetof(SICPU, _Carry));
   O(I_ZERO, offsetof(SICPU, _Zero));
   O(I_NEG, offsetof(SICPU, _Negative));
   O(I_OVF, offsetof(SICPU, _Overflow));
   O(I_SPB, offsetof(SICPU, ShiftedPB));
   O(I_SDB, offsetof(SICPU, ShiftedDB));
   O(MAPLAST, MAP_LAST);
   O(C_APUCYC, offsetof(SCPUState, APU_Cycles));
   O(IA_PC, offsetof(SIAPU, PC));
   /* -DLL_SPCASM (kern/genspc.py) */
   O(IA_DP, offsetof(SIAPU, DirectPage));
   O(IA_WA1, offsetof(SIAPU, WaitAddress1));
   O(IA_WA2, offsetof(SIAPU, WaitAddress2));
   O(IA_WC, offsetof(SIAPU, WaitCounter));
   O(IA_CARRY, offsetof(SIAPU, _Carry));
   O(IA_OVF, offsetof(SIAPU, _Overflow));
   O(IA_TWO, offsetof(SIAPU, TwoCycles));
   O(IA_A, offsetof(SIAPU, YA.B.A));
   O(IA_Y, offsetof(SIAPU, YA.B.Y));
   O(IA_P, offsetof(SIAPU, P));
   O(IA_ZERO, offsetof(SIAPU, _Zero));
   O(IA_X, offsetof(SIAPU, X));
   O(IA_S, offsetof(SIAPU, S));
   O(IA_RAM, offsetof(SIAPU, RAM));
   O(A_SHOWROM, offsetof(SAPU, ShowROM));
   O(A_OUTPORTS, offsetof(SAPU, OutPorts));
   O(A_EXTRARAM, offsetof(SAPU, ExtraRAM));
   O(A_TTARGET, offsetof(SAPU, TimerTarget));
   O(S_SHUTDOWN, offsetof(SSettings, Shutdown));
   O(I_CPUEXEC, offsetof(SICPU, CPUExecuting));
}
