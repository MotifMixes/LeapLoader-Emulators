/*
 * Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
 *
 * (c) Copyright 1996 - 2001 Gary Henderson (gary.henderson@ntlworld.com) and
 *                           Jerremy Koot (jkoot@snes9x.com)
 *
 * Super FX C emulator code
 * (c) Copyright 1997 - 1999 Ivar (ivar@snes9x.com) and
 *                           Gary Henderson.
 * Super FX    assembler emulator code (c) Copyright 1998 zsKnight and _Demo_.
 *
 * DSP1 emulator code (c) Copyright 1998 Ivar, _Demo_ and Gary Henderson.
 * C4 asm and some C emulation code (c) Copyright 2000 zsKnight and _Demo_.
 * C4 C code (c) Copyright 2001 Gary Henderson (gary.henderson@ntlworld.com).
 *
 * DOS port code contains the works of other authors. See headers in
 * individual files.
 *
 * (c) Copyright 2014 - 2016 Daniel De Matteis. (UNDER NO CIRCUMSTANCE 
 * WILL COMMERCIAL RIGHTS EVER BE APPROPRIATED TO ANY PARTY)
 *
 * Snes9x homepage: http://www.snes9x.com
 *
 * Permission to use, copy, modify and distribute Snes9x in both binary and
 * source form, for non-commercial purposes, is hereby granted without fee,
 * providing that this license information and copyright notice appear with
 * all copies and any derived work.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event shall the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Snes9x is freeware for PERSONAL USE only. Commercial users should
 * seek permission of the copyright holders first. Commercial use includes
 * charging money for Snes9x or software derived from Snes9x.
 *
 * The copyright holders request that bug fixes and improvements to the code
 * should be forwarded to them so everyone can benefit from the modifications
 * in future versions.
 *
 * Super NES and Super Nintendo Entertainment System are trademarks of
 * Nintendo Co., Limited and its subsidiary companies.
 */



#include "snes9x.h"

#include "memmap.h"
#include "cpuops.h"
#include "ppu.h"
#include "cpuexec.h"
#include "snapshot.h"
#include "gfx.h"
#include "missing.h"
#include "apu.h"
#include "dma.h"
#include "fxemu.h"
#ifdef USE_SA1
#include "sa1.h"
#endif

#include "os9x_asm_cpu.h"


void (*S9x_Current_HBlank_Event)();


#ifndef ASMCPU
#ifdef USE_SA1
void S9xMainLoop_SA1_APU(void)
{
   for (;;)
   {
      asm_APU_EXECUTE(1);
      if (CPU.Flags)
      {
         if (CPU.Flags & NMI_FLAG)
         {
            if (--CPU.NMICycleCount == 0)
            {
               CPU.Flags &= ~NMI_FLAG;
               if (CPU.WaitingForInterrupt)
               {
                  CPU.WaitingForInterrupt = FALSE;
                  ++CPU.PC;
               }
               S9xOpcode_NMI();
            }
         }

         if (CPU.Flags & IRQ_PENDING_FLAG)
         {
            if (CPU.IRQCycleCount == 0)
            {
               if (CPU.WaitingForInterrupt)
               {
                  CPU.WaitingForInterrupt = FALSE;
                  CPU.PC++;
               }
               if (CPU.IRQActive && !Settings.DisableIRQ)
               {
                  if (!CheckFlag(IRQ))
                     S9xOpcode_IRQ();
               }
               else
                  CPU.Flags &= ~IRQ_PENDING_FLAG;
            }
            else
               CPU.IRQCycleCount--;
         }
         if (CPU.Flags & SCAN_KEYS_FLAG)
            break;
      }

#ifdef CPU_SHUTDOWN
      CPU.PCAtOpcodeStart = CPU.PC;
#endif
#ifdef VAR_CYCLES
      CPU.Cycles += CPU.MemSpeed;
#else
      CPU.Cycles += ICPU.Speed [*CPU.PC];
#endif
      (*ICPU.S9xOpcodes[*CPU.PC++].S9xOpcode)();


      //S9xUpdateAPUTimer ();


      if (SA1.Executing)
         S9xSA1MainLoop();
      DO_HBLANK_CHECK();

#ifdef LAGFIX
      if (finishedFrame)
         break;
#endif
   }
}

void S9xMainLoop_SA1_NoAPU(void)
{
   for (;;)
   {
      if (CPU.Flags)
      {
         if (CPU.Flags & NMI_FLAG)
         {
            if (--CPU.NMICycleCount == 0)
            {
               CPU.Flags &= ~NMI_FLAG;
               if (CPU.WaitingForInterrupt)
               {
                  CPU.WaitingForInterrupt = FALSE;
                  ++CPU.PC;
               }
               S9xOpcode_NMI();
            }
         }

         if (CPU.Flags & IRQ_PENDING_FLAG)
         {
            if (CPU.IRQCycleCount == 0)
            {
               if (CPU.WaitingForInterrupt)
               {
                  CPU.WaitingForInterrupt = FALSE;
                  CPU.PC++;
               }
               if (CPU.IRQActive && !Settings.DisableIRQ)
               {
                  if (!CheckFlag(IRQ))
                     S9xOpcode_IRQ();
               }
               else
                  CPU.Flags &= ~IRQ_PENDING_FLAG;
            }
            else
               CPU.IRQCycleCount--;
         }
         if (CPU.Flags & SCAN_KEYS_FLAG)
            break;
      }

#ifdef CPU_SHUTDOWN
      CPU.PCAtOpcodeStart = CPU.PC;
#endif
#ifdef VAR_CYCLES
      CPU.Cycles += CPU.MemSpeed;
#else
      CPU.Cycles += ICPU.Speed [*CPU.PC];
#endif
      (*ICPU.S9xOpcodes[*CPU.PC++].S9xOpcode)();


      //S9xUpdateAPUTimer ();


      if (SA1.Executing)
         S9xSA1MainLoop();
      DO_HBLANK_CHECK();

#ifdef LAGFIX
      if (finishedFrame)
         break;
#endif
   }
}
// USE_SA1
#endif

#ifdef LL_APUBATCH
int32 LLOpCycles;
#ifdef LL_APUASM
uint8 LLApuPortHit;
#endif
#endif
#ifdef LL_SLEEP
extern int LLSleep;
extern uint32 LLSleepWrites, LLSleepIo;
extern uint8 LLSleepPorts [4];
/* LeapLoader: a wait found by the idle check (LLSleep 1) or the APU port poll
   (2) would only go round again on every line, so skip whole lines here until
   an interrupt, the end of the frame, a write/I/O read during the line
   processing (1) or a changed APU port (2; checked after each SPC700
   instruction, so the main CPU resumes exactly when the port changes). */
static void LLSleepLines(void)
{
#ifdef LL_SLEEPDBG
   extern int fprintf(); extern void* stderr;
   static int dbg; int apuops = 0; int lines = 0, kind = LLSleep; uint32 f0 = CPU.Flags;
#endif
   while (!(CPU.Flags & (NMI_FLAG | IRQ_PENDING_FLAG | SCAN_KEYS_FLAG)))
   {
      if (LLSleep == 1)
      {
         if (LLWriteCount != LLSleepWrites || LLIoReads != LLSleepIo)
            break;
      }
      else
      {
#ifdef SPC700_SHUTDOWN
         /* the loop would read the port again: that wakes the SPC700 */
         LL_SPCFLUSH();
         CPU.APU_APUExecuting = Settings.APUEnabled;
         IAPU.WaitCounter++;
#endif
         if (CPU.APU_APUExecuting == 1)
            while (!memcmp(APU.OutPorts, LLSleepPorts, 4) && CPU.APU_Cycles < CPU.NextEvent)
            {
#ifdef LL_APUASM
               LLApuPortHit = 0;
               LLApuRunP((const int32*) &CPU.NextEvent);
#else
               CPU.APU_Cycles += S9xAPUCycles [*IAPU.PC];
               SPCH (*S9xApuOpcodes[*IAPU.PC]) ();
#endif
#ifdef LL_SLEEPDBG
               apuops++;
#endif
            }
         if (memcmp(APU.OutPorts, LLSleepPorts, 4))
         {
            if (CPU.Cycles < CPU.APU_Cycles)
               CPU.Cycles = CPU.APU_Cycles;
            break;
         }
      }
      if (CPU.Cycles < CPU.NextEvent)
         CPU.Cycles = CPU.NextEvent;
      LLOpCycles = CPU.Cycles;
      S9xDoHBlankProcessing();
#ifdef LL_SLEEPDBG
      lines++;
#endif
#ifdef LL_SLEEP3
      {
         extern uint32 LLSleep4212;
         if (LLSleep == 3 && (REGISTER_4212() & ~0x40) != LLSleep4212)
            break;
      }
#endif
   }
#ifdef LL_SLEEPDBG
   if (dbg < 400 && (dbg++ % 20 == 0 || lines > 300))
      fprintf(stderr, "SLEEP k%d lines %d pc %02x:%04x flags %x->%x wc %u/%u io %u/%u V %d ports %08x apuops %d apc %04x acyc %d next %d cyc %d ex %d\n", kind, lines,
         Registers.PB, (unsigned)(CPU.PC - CPU.PCBase), f0, CPU.Flags, LLWriteCount, LLSleepWrites, LLIoReads, LLSleepIo,
         CPU.V_Counter, *(uint32*)APU.OutPorts, apuops, (unsigned)(IAPU.PC - IAPU.RAM), CPU.APU_Cycles, CPU.NextEvent, CPU.Cycles, CPU.APU_APUExecuting);
#endif
   LLSleep = 0;
}
#endif

#ifdef LL_ASMCORE
void LLAsmInit(void);
void LLAsmRun(void);
/* the line processing after an instruction, called by llcore.S */
void LLAsmEvent(void)
{
   S9xDoHBlankProcessing();
   if (LLSleep)
      LLSleepLines();
}
#endif

#ifdef LL_CORE2
/* LeapLoader milestone 2: the register-resident asm 65816 core (ll65.S, kern/gen65.py).
   ll65.S calls this between two instructions when CPU.Cycles >= CPU.NextEvent or
   CPU.Flags was set: first what the C loop does after an instruction (ev = 1: the line
   processing), then what it does before the next one (the flag checks). Returns 1 when
   the C loop would leave (SCAN_KEYS_FLAG). ll65.S has spilled its state before. */
int LL65Init(void);
void LL65Run(void);
int LL65Between(int ev)
{
   int r = 0;
   S9xFixCycles();
#ifdef LL65DBG
   { extern int printf(const char*, ...); if (ICPU.Frame >= LL65DBG && ICPU.Frame <= LL65DBG + 1)
      printf("B f%u v%d ev%d cyc %d next %d we %d flg %x pc %04x wfi %d irqa %d irqc %d pl %02x a %04x x %04x\n", ICPU.Frame, CPU.V_Counter, ev,
         CPU.Cycles, CPU.NextEvent, CPU.WhichEvent, CPU.Flags, (unsigned)(CPU.PC - CPU.PCBase), CPU.WaitingForInterrupt, CPU.IRQActive,
         CPU.IRQCycleCount, Registers.PL, Registers.A.W, Registers.X.W); }
#endif
   if (ev && CPU.Cycles >= CPU.NextEvent)
   {
      S9xDoHBlankProcessing();
      if (LLSleep)
         LLSleepLines();
   }
   if (CPU.Flags)
   {
      if (CPU.Flags & NMI_FLAG)
      {
         if (--CPU.NMICycleCount == 0)
         {
            CPU.Flags &= ~NMI_FLAG;
            if (CPU.WaitingForInterrupt)
            {
               CPU.WaitingForInterrupt = FALSE;
               ++CPU.PC;
            }
            S9xOpcode_NMI();
            r = 2;
         }
      }

      if (CPU.Flags & IRQ_PENDING_FLAG)
      {
         if (CPU.IRQCycleCount == 0)
         {
            if (CPU.WaitingForInterrupt)
            {
               CPU.WaitingForInterrupt = FALSE;
               CPU.PC++;
            }
            if (CPU.IRQActive && !Settings.DisableIRQ)
            {
               if (!CheckFlag(IRQ))
               {
                  S9xOpcode_IRQ();
                  r = 2;
               }
            }
            else
               CPU.Flags &= ~IRQ_PENDING_FLAG;
         }
         else
            CPU.IRQCycleCount--;
      }
      if (CPU.Flags & SCAN_KEYS_FLAG)
         return 1;
   }
   return r;
}
#endif
void S9xMainLoop_NoSA1_APU(void)
{
#ifdef LL_CORE2
   if (LL65Init())
   {
      LL65Run();
      return;
   }
#endif
#ifdef LL_ASMCORE
   LLAsmInit();
#endif
   for (;;)
   {
#ifndef LL_APUBATCH
      asm_APU_EXECUTE(1);
#endif
      if (CPU.Flags)
      {
         if (CPU.Flags & NMI_FLAG)
         {
            if (--CPU.NMICycleCount == 0)
            {
               CPU.Flags &= ~NMI_FLAG;
               if (CPU.WaitingForInterrupt)
               {
                  CPU.WaitingForInterrupt = FALSE;
                  ++CPU.PC;
               }
               S9xOpcode_NMI();
            }
         }

         if (CPU.Flags & IRQ_PENDING_FLAG)
         {
            if (CPU.IRQCycleCount == 0)
            {
               if (CPU.WaitingForInterrupt)
               {
                  CPU.WaitingForInterrupt = FALSE;
                  CPU.PC++;
               }
               if (CPU.IRQActive && !Settings.DisableIRQ)
               {
                  if (!CheckFlag(IRQ))
                     S9xOpcode_IRQ();
               }
               else
                  CPU.Flags &= ~IRQ_PENDING_FLAG;
            }
            else
               CPU.IRQCycleCount--;
         }
         if (CPU.Flags & SCAN_KEYS_FLAG)
            break;
      }

#ifdef LL_ASMCORE
      /* runs instructions until the checks below or above have work */
      LLAsmRun();
#else
#ifdef LL_APUBATCH
      LLOpCycles = CPU.Cycles;
#endif
#ifdef TRACEL
      { extern unsigned frame; extern int printf(const char*, ...); if (frame == TRACEL && CPU.V_Counter == 256 && CPU.Cycles > 1200) printf("I c%d op%02x pc%04x ne%d\n", (int) CPU.Cycles, *CPU.PC, (unsigned) (CPU.PC - CPU.PCBase), (int) CPU.NextEvent); }
#endif
#ifdef CPU_SHUTDOWN
      CPU.PCAtOpcodeStart = CPU.PC;
#endif
#ifdef VAR_CYCLES
      CPU.Cycles += CPU.MemSpeed;
#else
      CPU.Cycles += ICPU.Speed [*CPU.PC];
#endif
#ifdef OPHIST
      { extern unsigned ophist[]; ophist[*CPU.PC | ((Registers.PL & 0x30) << 4)]++; }
#endif
#ifdef PCHIST
      { extern unsigned pchist[]; pchist[((unsigned)CPU.Regs.PB << 16 | (unsigned)(CPU.PC - CPU.PCBase)) & 0xFFFFFF]++; }
#endif
      (*ICPU.S9xOpcodes[*CPU.PC++].S9xOpcode)();
#endif


      //S9xUpdateAPUTimer ();

#ifdef LL_SLEEP
      /* a sleep always starts with CPU.Cycles at the next event, so it is
         checked only here, not after every instruction */
      if (CPU.Cycles >= CPU.NextEvent)
      {
         S9xDoHBlankProcessing();
         if (LLSleep)
            LLSleepLines();
      }
#else
      DO_HBLANK_CHECK();
#endif

#ifdef LAGFIX
      if (finishedFrame)
         break;
#endif
   }
}

void S9xMainLoop_NoSA1_NoAPU(void)
{
   for (;;)
   {
      if (CPU.Flags)
      {
         if (CPU.Flags & NMI_FLAG)
         {
            if (--CPU.NMICycleCount == 0)
            {
               CPU.Flags &= ~NMI_FLAG;
               if (CPU.WaitingForInterrupt)
               {
                  CPU.WaitingForInterrupt = FALSE;
                  ++CPU.PC;
               }
               S9xOpcode_NMI();
            }
         }

         if (CPU.Flags & IRQ_PENDING_FLAG)
         {
            if (CPU.IRQCycleCount == 0)
            {
               if (CPU.WaitingForInterrupt)
               {
                  CPU.WaitingForInterrupt = FALSE;
                  CPU.PC++;
               }
               if (CPU.IRQActive && !Settings.DisableIRQ)
               {
                  if (!CheckFlag(IRQ))
                     S9xOpcode_IRQ();
               }
               else
                  CPU.Flags &= ~IRQ_PENDING_FLAG;
            }
            else
               CPU.IRQCycleCount--;
         }
         if (CPU.Flags & SCAN_KEYS_FLAG)
            break;
      }

#ifdef CPU_SHUTDOWN
      CPU.PCAtOpcodeStart = CPU.PC;
#endif
#ifdef VAR_CYCLES
      CPU.Cycles += CPU.MemSpeed;
#else
      CPU.Cycles += ICPU.Speed [*CPU.PC];
#endif
      (*ICPU.S9xOpcodes[*CPU.PC++].S9xOpcode)();


      //S9xUpdateAPUTimer ();


      DO_HBLANK_CHECK();

#ifdef LAGFIX
      if (finishedFrame)
         break;
#endif
   }
}
#endif


void
S9xMainLoop(void)
{
#ifdef LAGFIX
   do
   {
#endif
#ifndef ASMCPU
      if (Settings.APUEnabled == 1)
      {
#ifdef USE_SA1
         if (Settings.SA1)
            S9xMainLoop_SA1_APU();
         else
#endif
            S9xMainLoop_NoSA1_APU();
      }
      else
      {
#ifdef USE_SA1
         if (Settings.SA1)
            S9xMainLoop_SA1_NoAPU();
         else
#endif
            S9xMainLoop_NoSA1_NoAPU();
      }
#else
#ifdef ASM_SPC700
      if (Settings.asmspc700)
         asmMainLoop_spcAsm(&CPU);
      else
#endif
         asmMainLoop_spcC(&CPU);
#endif

#ifdef LAGFIX
      if (!finishedFrame)
      {
#endif
#ifndef LAGFIX
         LL_SPCFLUSH(); /* A1: catch-ups after the last line end (a DMA or sleep crossing it) */
         Registers.PC = CPU.PC - CPU.PCBase;

#ifndef ASMCPU
         S9xPackStatus();
#endif

         S9xAPUPackStatus();
#endif

         //if (CPU.Flags & SCAN_KEYS_FLAG)
         // {
         CPU.Flags &= ~SCAN_KEYS_FLAG;
         //}

         if (CPU.BRKTriggered && Settings.SuperFX && !CPU.TriedInterleavedMode2)
         {
            CPU.TriedInterleavedMode2 = TRUE;
            CPU.BRKTriggered = FALSE;
            S9xDeinterleaveMode2();
         }
#ifdef LAGFIX
      }
      else
      {
         finishedFrame = false;

         Registers.PC = CPU.PC - CPU.PCBase;

#ifndef ASMCPU
         S9xPackStatus();
#endif

         S9xAPUPackStatus();

         break;
      }
   } while (!finishedFrame);
#endif
}

void S9xSetIRQ(uint32 source)
{
   CPU.IRQActive |= source;
   CPU.Flags |= IRQ_PENDING_FLAG;
   CPU.IRQCycleCount = 3;
   if (CPU.WaitingForInterrupt)
   {
      // Force IRQ to trigger immediately after WAI -
      // Final Fantasy Mystic Quest crashes without this.
      CPU.IRQCycleCount = 0;
      CPU.WaitingForInterrupt = FALSE;
      CPU.PC++;
   }
}

void S9xClearIRQ(uint32 source)
{
   CLEAR_IRQ_SOURCE(source);
}

#ifdef LL_SPCDEFER
#define LLSPC_MAX 1024
int32 LLSpcMax = LLSPC_NONE;
int LLSpcN;
static int32 LLSpcQ[LLSPC_MAX];   /* catch-up targets; LLSPC_NONE | parity = line end */
static void LLSpcTimers(int odd)
{
      // Use TimerErrorCounter to skip update of SPC700 timers once
      // every 128 updates. Needed because this section of code is called
      // once every emulated 63.5 microseconds, which coresponds to
      // 15.750KHz, but the SPC700 timers need to be updated at multiples
      // of 8KHz, hence the error correction.
      // IAPU.TimerErrorCounter++;
      // if (IAPU.TimerErrorCounter >= )
      //     IAPU.TimerErrorCounter = 0;
      // else
      {
         if (APU.TimerEnabled [2])
         {
            APU.Timer [2] += 4;
            while (APU.Timer [2] >= APU.TimerTarget [2])
            {
               IAPU.RAM [0xff] = (IAPU.RAM [0xff] + 1) & 0xf;
               APU.Timer [2] -= APU.TimerTarget [2];
#ifdef SPC700_SHUTDOWN
               IAPU.WaitCounter++;
               /*IAPU.APUExecuting*/CPU.APU_APUExecuting = TRUE;
#endif
            }
         }
         if (odd)
         {
            if (APU.TimerEnabled [0])
            {
               APU.Timer [0]++;
               if (APU.Timer [0] >= APU.TimerTarget [0])
               {
                  IAPU.RAM [0xfd] = (IAPU.RAM [0xfd] + 1) & 0xf;
                  APU.Timer [0] = 0;
#ifdef SPC700_SHUTDOWN
                  IAPU.WaitCounter++;
                  /*IAPU.APUExecuting*/CPU.APU_APUExecuting = TRUE;
#endif
               }
            }
            if (APU.TimerEnabled [1])
            {
               APU.Timer [1]++;
               if (APU.Timer [1] >= APU.TimerTarget [1])
               {
                  IAPU.RAM [0xfe] = (IAPU.RAM [0xfe] + 1) & 0xf;
                  APU.Timer [1] = 0;
#ifdef SPC700_SHUTDOWN
                  IAPU.WaitCounter++;
                  /*IAPU.APUExecuting*/CPU.APU_APUExecuting = TRUE;
#endif
               }
            }
         }
      }
}
#if defined LL_OSC && defined LL_OSCHOT
#define LL_O2 __attribute__((optimize("O2")))   /* B4: hot loops stay -O2 in the -Os files */
#else
#define LL_O2
#endif
LL_O2 void LLSpcFlush(void)
{
   int i, n = LLSpcN;
   LLSpcN = 0;
   LLSpcMax = LLSPC_NONE;   /* a target after this flush must be queued even if lower (a flusher may wake the SPC) */
   for (i = 0; i < n; i++)
   {
      int32 q = LLSpcQ[i];
      if ((uint32) q - (uint32) LLSPC_NONE < 2u)
      {
         if (CPU.APU_APUExecuting)
            CPU.APU_Cycles -= Settings.H_Max;
         else
            CPU.APU_Cycles = 0;
         LLSpcTimers(q & 1);
      }
      else if (CPU.APU_APUExecuting == 1)
         LLApuRunLE(&LLSpcQ[i]);
   }
}
#endif
LL_O2 void S9xDoHBlankProcessing()
{
#ifdef TRACEL
   { extern unsigned frame; extern int printf(const char*, ...); if (frame == TRACEL && CPU.V_Counter >= 255 && CPU.V_Counter <= 256) printf("H v%d c%d we%d ne%d sl%d io%u wc%u op%d\n", (int) CPU.V_Counter, (int) CPU.Cycles, (int) CPU.WhichEvent, (int) CPU.NextEvent, LLSleep, LLIoReads, LLWriteCount, (int) LLOpCycles); }
#endif

#ifdef LL_CONSOLE
   /* the console driver's clock (port/console.c): read every line, so the
      10 ms hardware timer never wraps unseen */
   extern void LLTick(void);
   LLTick();
#endif
#ifdef LL_APUBATCH
   /* LeapLoader: the sound CPU runs in batches (to here, and before each
      APU port access) instead of after every main CPU instruction. */
#ifdef LL_APUHB
   /* only at the line's end (timers, APU_Cycles rebase): the other events don't touch the sound CPU
      (port accesses, HDMA's included, catch it up themselves), so its runs are just longer */
   if (CPU.WhichEvent == HBLANK_END_EVENT)
#endif
#ifdef LL_SPCDEFER
   /* with nothing queued and the SPC shut down this catch-up would run nothing: not queued (the line end then
      runs at once below, as the original did, so SPC-idle games keep an empty queue) */
   if (LLOpCycles > LLSpcMax && (LLSpcN || CPU.APU_APUExecuting == 1))
      LLSpcQ[LLSpcN++] = LLSpcMax = LLOpCycles;
#else
   LL_APU_CATCHUP();
#endif
#endif
#ifdef CPU_SHUTDOWN
   CPU.WaitCounter++;
#endif

   switch (CPU.WhichEvent)
   {
   case HBLANK_START_EVENT:
      if (IPPU.HDMA && CPU.V_Counter <= PPU.ScreenHeight)
         IPPU.HDMA = S9xDoHDMA(IPPU.HDMA);
      break;

   case HBLANK_END_EVENT:
#ifdef TRACEL
      { extern unsigned frame; extern int printf(const char*, ...); if (frame >= TRACEL && frame <= TRACEL + 1 || CPU.V_Counter == 100) printf("L %u %d c %d pc %06x apc %04x ac %d a %02x x %04x\n", frame, (int) CPU.V_Counter, (int) CPU.Cycles, (unsigned) (Registers.PB << 16 | Registers.PC), (unsigned) (IAPU.PC - IAPU.RAM), (int) CPU.APU_Cycles, Registers.A.W, Registers.X.W); }
#endif
#ifndef LL_SPCDEFER
      asm_APU_EXECUTE(3); // notaz: run spc700 in sound 'speed hack' mode
#endif
      if (Settings.SuperFX)
         S9xSuperFXExec();

      CPU.Cycles -= Settings.H_Max;
#ifndef LL_SPCDEFER
      if (/*IAPU.APUExecuting*/CPU.APU_APUExecuting)
         CPU.APU_Cycles -= Settings.H_Max;
      else
         CPU.APU_Cycles = 0;
#endif

      CPU.NextEvent = -1;
      ICPU.Scanline++;

      if (++CPU.V_Counter >= (Settings.PAL ? SNES_MAX_PAL_VCOUNTER : SNES_MAX_NTSC_VCOUNTER))
      {
         CPU.V_Counter = 0;
         CPU.NMIActive = FALSE;
         ICPU.Frame++;
         PPU.HVBeamCounterLatched = 0;
         CPU.Flags |= SCAN_KEYS_FLAG;
         S9xStartHDMA();
      }

      if (PPU.VTimerEnabled && !PPU.HTimerEnabled &&
            CPU.V_Counter == PPU.IRQVBeamPos)
         S9xSetIRQ(PPU_V_BEAM_IRQ_SOURCE);

      if (CPU.V_Counter == PPU.ScreenHeight + FIRST_VISIBLE_LINE)
      {
         // Start of V-blank
         S9xEndScreenRefresh();
         IPPU.HDMA = 0;
         // Bits 7 and 6 of $4212 are computed when read in S9xGetPPU.
         missing.dma_this_frame = 0;
         IPPU.MaxBrightness = PPU.Brightness;
         PPU.ForcedBlanking = (Memory.FillRAM [0x2100] >> 7) & 1;

         if (!PPU.ForcedBlanking)
         {
            PPU.OAMAddr = PPU.SavedOAMAddr;
            PPU.OAMFlip = 0;
            PPU.FirstSprite = 0;
            if (PPU.OAMPriorityRotation)
               PPU.FirstSprite = PPU.OAMAddr >> 1;
         }

         Memory.FillRAM[0x4210] = 0x80;
         if (Memory.FillRAM[0x4200] & 0x80)
         {
            CPU.NMIActive = TRUE;
            CPU.Flags |= NMI_FLAG;
            CPU.NMICycleCount = CPU.NMITriggerPoint;
         }

      }

      if (CPU.V_Counter == PPU.ScreenHeight + 3)
         S9xUpdateJoypads();

      if (CPU.V_Counter == FIRST_VISIBLE_LINE)
      {
         Memory.FillRAM[0x4210] = 0;
         CPU.Flags &= ~NMI_FLAG;
         S9xStartScreenRefresh();
      }
      if (CPU.V_Counter >= FIRST_VISIBLE_LINE &&
            CPU.V_Counter < PPU.ScreenHeight + FIRST_VISIBLE_LINE)
         RenderLine(CPU.V_Counter - FIRST_VISIBLE_LINE);
#ifdef LL_SPCDEFER
      LLSpcMax = LLSPC_NONE;
      if (!LLSpcN)
      {
         /* nothing pending: the line end's rebase and timer ticks now */
         if (CPU.APU_APUExecuting)
            CPU.APU_Cycles -= Settings.H_Max;
         else
            CPU.APU_Cycles = 0;
         LLSpcTimers(CPU.V_Counter & 1);
      }
      else
      {
         LLSpcQ[LLSpcN++] = LLSPC_NONE | (CPU.V_Counter & 1);
         if (LLSpcN >= LLSPC_MAX - 16 || CPU.V_Counter == 0)
            LLSpcFlush();
      }
#else
      // Use TimerErrorCounter to skip update of SPC700 timers once
      // every 128 updates. Needed because this section of code is called
      // once every emulated 63.5 microseconds, which coresponds to
      // 15.750KHz, but the SPC700 timers need to be updated at multiples
      // of 8KHz, hence the error correction.
      // IAPU.TimerErrorCounter++;
      // if (IAPU.TimerErrorCounter >= )
      //     IAPU.TimerErrorCounter = 0;
      // else
      {
         if (APU.TimerEnabled [2])
         {
            APU.Timer [2] += 4;
            while (APU.Timer [2] >= APU.TimerTarget [2])
            {
               IAPU.RAM [0xff] = (IAPU.RAM [0xff] + 1) & 0xf;
               APU.Timer [2] -= APU.TimerTarget [2];
#ifdef SPC700_SHUTDOWN
               IAPU.WaitCounter++;
               /*IAPU.APUExecuting*/CPU.APU_APUExecuting = TRUE;
#endif
            }
         }
         if (CPU.V_Counter & 1)
         {
            if (APU.TimerEnabled [0])
            {
               APU.Timer [0]++;
               if (APU.Timer [0] >= APU.TimerTarget [0])
               {
                  IAPU.RAM [0xfd] = (IAPU.RAM [0xfd] + 1) & 0xf;
                  APU.Timer [0] = 0;
#ifdef SPC700_SHUTDOWN
                  IAPU.WaitCounter++;
                  /*IAPU.APUExecuting*/CPU.APU_APUExecuting = TRUE;
#endif
               }
            }
            if (APU.TimerEnabled [1])
            {
               APU.Timer [1]++;
               if (APU.Timer [1] >= APU.TimerTarget [1])
               {
                  IAPU.RAM [0xfe] = (IAPU.RAM [0xfe] + 1) & 0xf;
                  APU.Timer [1] = 0;
#ifdef SPC700_SHUTDOWN
                  IAPU.WaitCounter++;
                  /*IAPU.APUExecuting*/CPU.APU_APUExecuting = TRUE;
#endif
               }
            }
         }
      }
#endif
      break;
   case HTIMER_BEFORE_EVENT:
   case HTIMER_AFTER_EVENT:
      if (PPU.HTimerEnabled &&
            (!PPU.VTimerEnabled || CPU.V_Counter == PPU.IRQVBeamPos))
         S9xSetIRQ(PPU_H_BEAM_IRQ_SOURCE);
      break;
   }
#ifdef LL_SPCDEFDBG
   LL_SPCFLUSH();
#endif
   S9xReschedule();
}

