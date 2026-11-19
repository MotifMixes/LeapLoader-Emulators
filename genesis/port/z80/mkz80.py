# mkz80.py: writes z80/ = copies of PicoDrive's cpu/cz80 files, cz80_op.c with
# -DZ80IDLE idle-loop skipping (run from port/; the build compiles
# z80/cz80.c). Within one Cz80_Exec call nothing else runs, so a loop that only
# reads Z80 RAM and tests it repeats the same state every iteration; whole
# iterations are skipped by charging their cycles, as long as the cycle count
# stays > 0 (the interpreter then runs on from the same state and count it
# would have reached). Loops (seen in the test games):
#   T: ld a,(hl) / or a / jp p,T            21 cycles (Sonic, Gunstar)
#   T: ld a,(hl) / cp n / jr c,T            26 cycles (Streets of Rage 2)
#   T: djnz T                               13 cycles (Sonic, Gunstar delays)
# R (emulated exactly: +1 per opcode) is advanced too.
# Only with HL in Z80 RAM (< 0x2000), A = (HL) (the 68k may have changed it
# between two Exec calls) and no pending CPU->Status event.
import os, shutil
S = '../picodrive/cpu/cz80'
os.makedirs('z80', exist_ok=True)
for f in os.listdir(S):
    if f.endswith(('.c', '.h')) and f != 'cz80_op.c':
        shutil.copy(os.path.join(S, f), os.path.join('z80', f))
src = open(os.path.join(S, 'cz80_op.c'), newline='').read().replace('\r\n', '\n')
def sub(a, b):
    global src
    assert src.count(a) == 1, a
    src = src.replace(a, b)
macros = r'''

#ifdef Z80PARK   /* no multi-instruction skips in Z80PARK single steps */
#define Z80PK_SKIPOK (!z80pk_step)
#else
#define Z80PK_SKIPOK 1
#endif
#ifdef Z80LOOP
#ifdef Z80LOOPSTAT
extern unsigned z80lstat[4];
#define Z80LSTAT if (z80loop.pc == h_) { z80lstat[0]++; if (z80loop_mod) z80lstat[1]++; else if (!z80loop_same()) z80lstat[2]++; else if (memcmp(z80loop.regs, CPU, 0x1c)) z80lstat[3]++; }
#else
#define Z80LSTAT
#endif
#define Z80LOOP_PRE UINT32 o_ = zRealPC;
#define Z80LOOP_JP { UINT32 h_ = zRealPC; \
	if (h_ < o_ && h_ < 0x2000 && !CPU->Status) { \
		Z80LSTAT 		if (z80loop.pc == h_ && !z80loop_mod && z80loop_same() && z80loop.ic > CPU->ICount && !memcmp(z80loop.regs, CPU, 0x1c) \
		    && !memcmp(z80loop.misc, &CPU->IFF, 6) && z80loop.irq[0] == CPU->IRQLine && z80loop.irq[1] == CPU->IRQState) { \
			INT32 c_ = z80loop.ic - CPU->ICount; UINT32 r_ = zR - z80loop.r; \
			INT32 m_ = (CPU->ICount - 1) / c_; \
			CPU->ICount -= m_ * c_; zR += m_ * r_; z80loop_skips += m_; \
		} \
		z80loop.pc = h_; z80loop.ic = CPU->ICount; z80loop.r = zR; memcpy(z80loop.regs, CPU, 0x1c); \
		memcpy(z80loop.misc, &CPU->IFF, 6); z80loop.irq[0] = CPU->IRQLine; z80loop.irq[1] = CPU->IRQState; z80loop_new(); \
	} }
#else
#ifdef Z80PARK
#define Z80LOOP_PRE UINT32 o_ = zRealPC;
#else
#define Z80LOOP_PRE
#endif
#define Z80LOOP_JP
#endif
#ifdef Z80IDLECHK  /* no skipping: check that each loop iteration costs and keeps what is assumed */
#include <stdio.h>
static struct { UINT32 pc; INT32 c; int a, f, hl, b, k, n; } zi_;
#define Z80CHK(per, cond, isdj) { UINT32 r_ = zRealPC; \
	if (cond) { \
		if (zi_.pc == r_ && CPU->ICount < zi_.c && !CPU->Status && zi_.c - (per) * (zi_.k + 1) > 0) { zi_.k++; \
			if (CPU->ICount != zi_.c - (per) * zi_.k || zA != zi_.a || zF != zi_.f || zHL != zi_.hl \
			    || (isdj ? zB != zi_.b - zi_.k : zB != zi_.b)) { \
				if (zi_.n++ < 20) printf("ZCHK pc %x k %d icount %d exp %d a %x/%x f %x/%x hl %x/%x b %x/%x\n", r_, zi_.k, \
				  CPU->ICount, zi_.c - (per) * zi_.k, zA, zi_.a, zF, zi_.f, zHL, zi_.hl, zB, zi_.b); \
				zi_.pc = 0; } \
		} else { zi_.pc = r_; zi_.c = CPU->ICount; zi_.a = zA; zi_.f = zF; zi_.hl = zHL; zi_.b = zB; zi_.k = 0; } \
	} }
#define Z80IDLE_JP Z80CHK(21, r_ >= 3 && r_ < 0x1ffe && zHL < 0x2000 && Z80I_B(-3) == 0x7e && Z80I_B(-2) == 0xb7 \
	    && (Z80I_B(0) | (Z80I_B(1) << 8)) == r_ - 3, 0)
#define Z80IDLE_JRC Z80CHK(26, r_ >= 4 && r_ < 0x2000 && zHL < 0x2000 && Z80I_B(0) == 0xfb && Z80I_B(-4) == 0x7e \
	    && Z80I_B(-3) == 0xfe, 0)
#define Z80IDLE_DJNZ Z80CHK(13, r_ >= 1 && r_ < 0x2000 && Z80I_B(0) == 0xfe, 1)
#define Z80I_B(o)  (*(UINT8 *)(PC + (o)))
#elif defined(Z80IDLE)
#define Z80I_B(o)  (*(UINT8 *)(PC + (o)))
/* at a taken JP P,nn (PC at nn) */
#define Z80IDLE_JP { UINT32 r_ = zRealPC; \
	if (!CPU->Status && r_ >= 3 && r_ < 0x1ffe && zHL < 0x2000 && zA == picodrive_read(zHL) && Z80I_B(-3) == 0x7e && Z80I_B(-2) == 0xb7 \
	    && (Z80I_B(0) | (Z80I_B(1) << 8)) == r_ - 3 && CPU->ICount > 21 && Z80PK_SKIPOK) \
		{ INT32 m_ = (CPU->ICount - 1) / 21; CPU->ICount -= m_ * 21; zR += 3 * m_; } }
/* at a taken JR C,e (PC at e) */
#define Z80IDLE_JRC { UINT32 r_ = zRealPC; \
	if (!CPU->Status && r_ >= 4 && r_ < 0x2000 && zHL < 0x2000 && zA == picodrive_read(zHL) && Z80I_B(0) == 0xfb && Z80I_B(-4) == 0x7e \
	    && Z80I_B(-3) == 0xfe && CPU->ICount > 26 && Z80PK_SKIPOK) \
		{ INT32 m_ = (CPU->ICount - 1) / 26; CPU->ICount -= m_ * 26; zR += 3 * m_; } }
/* at a taken DJNZ e, B already decremented, 1 cycle charged, 12 to come */
#define Z80IDLE_DJNZ { UINT32 r_ = zRealPC; \
	if (!CPU->Status && r_ >= 1 && r_ < 0x2000 && Z80I_B(0) == 0xfe && zB > 1 && CPU->ICount > 25 && Z80PK_SKIPOK) { \
		INT32 m_ = (CPU->ICount - 13) / 13; if (m_ > zB - 1) m_ = zB - 1; \
		zB -= m_; CPU->ICount -= m_ * 13; zR += m_; } }
#else
#define Z80IDLE_JP
#define Z80IDLE_JRC
#define Z80IDLE_DJNZ
#endif
#ifdef Z80POLL
/* -DZ80POLL, at DI (R and PC already past it): Vectorman's GEMS poll
   "di / push af / push hl / ld hl,nn / ld a,(hl) / or a / jr z,T ... T: pop hl / pop af"
   with (nn) == 0 is done at once (same writes, R, cycles; registers and SP end
   unchanged); the interpreter goes on at T+2 (ei / ret). Only when all 9 would
   run in this Exec call (count > 69 before the pop af), no Status event, nn and
   the stack in Z80 RAM. */
#define Z80I_P(o)  (*(UINT8 *)(PC + (o)))
#define Z80POLL_DI { UINT32 n_, t_; 	if (!CPU->Status && CPU->ICount > 69 && Z80PK_SKIPOK && Z80I_P(0) == 0xf5 && Z80I_P(1) == 0xe5 && Z80I_P(2) == 0x21 	    && Z80I_P(5) == 0x7e && Z80I_P(6) == 0xb7 && Z80I_P(7) == 0x28 	    && (n_ = Z80I_P(3) | (Z80I_P(4) << 8)) < 0x2000 && zSP >= 8 && zSP <= 0x2000) { 		t_ = 9 + (INT8)Z80I_P(8); 		if (Z80I_P(t_) == 0xe1 && Z80I_P(t_ + 1) == 0xf1 && zRealPC + t_ + 2 < 0x2000 && picodrive_read(n_) == 0) { 			UINT32 sp_ = zSP - 2; WRITE_MEM16(sp_, (zA << 8) | zF); sp_ -= 2; WRITE_MEM16(sp_, zHL); 			zIFF = 0; zR += 8; USE_CYCLES(79) PC += t_ + 2; goto Cz80_Exec; } } }
#else
#define Z80POLL_DI
#endif
'''
sub('OP_JP_P:\n\t\tif (!(zF & SF)) goto OP_JP;', 'OP_JP_P:\n\t\tif (!(zF & SF)) { Z80IDLE_JP goto OP_JP; }')
sub('OP_JR_C:\n\t\tif (zF & CF) goto OP_JR;', 'OP_JR_C:\n\t\tif (zF & CF) { Z80IDLE_JRC goto OP_JR; }')
sub('OP_DJNZ:\n\t\tUSE_CYCLES(1)\n\t\tif (--zB) goto OP_JR;', 'OP_DJNZ:\n\t\tUSE_CYCLES(1)\n\t\tif (--zB) { Z80IDLE_DJNZ goto OP_JR; }')
sub('OP_DI:\n\t\tzIFF = 0;', 'OP_DI:\n\t\tZ80POLL_DI\n\t\tzIFF = 0;')

# -DZ80LOOP: whole-iteration skip of any backward JP loop. At a taken backward JP (hook: PC at
# the target, the JP's 10 cycles not yet charged) the state is recorded; when the next arrival at
# the same target finds the same registers (all but R), IFF/IM/I/Status/IRQ fields, and the
# iteration made no value-changing Z80 RAM write and no access through a map handler (YM, bank,
# VDP, 68k bus), the next iteration is the same function of the same state (inside one
# Cz80_Exec nothing else runs), so m = (ICount - 1) / c more iterations are charged at once
# (c cycles, R += m * r). The record is dropped at each Cz80_Exec entry (the 68k may have
# written Z80 RAM in between).
sub('OP_JP:\n\t\tres = READ_ARG16();\n\t\tSET_PC(res);',
    'OP_JP:\n\t\tres = READ_ARG16();\n\t\tZ80LOOP_PRE\n\t\tSET_PC(res);\n\t\tZ80LOOP_JP\n#ifdef Z80PARK\n\t\tZ80PARK_JP\n#endif')

open('z80/cz80_op.c', 'w', newline='\n').write('/* generated by z80/mkz80.py from ../picodrive/cpu/cz80/cz80_op.c */' + macros + src)

# Z80LOOP: map-handler accesses and value-changing RAM writes mark the iteration
def patchf(f, a, b):
    t = open(f, newline='', encoding='latin-1').read().replace('\r\n', '\n')
    assert t.count(a) == 1, (f, a)
    open(f, 'w', newline='\n', encoding='latin-1').write(t.replace(a, b))
patchf('z80/cz80macro.h', r"""	if (map_flag_set(v)) \
		((z80_write_f *)(v << 1))(a, d); \
	else \
		*(unsigned char *)((v << 1) + a) = d; \
""", r"""	if (map_flag_set(v)) { \
		Z80LOOP_IO \
		((z80_write_f *)(v << 1))(a, d); \
	} else Z80LOOP_W((unsigned char *)((v << 1) + a), d) \
""")
patchf('z80/cz80.c', """	if (map_flag_set(v))
		return ((z80_read_f *)(v << 1))(a);""", """	if (map_flag_set(v)) {
		Z80LOOP_IO
		return ((z80_read_f *)(v << 1))(a);
	}""")
patchf('z80/cz80.c', """	CPU->ICount = cycles - CPU->ExtraCycles;
	CPU->ExtraCycles = 0;
""", """	CPU->ICount = cycles - CPU->ExtraCycles;
	CPU->ExtraCycles = 0;
	Z80LOOP_ENTRY
""")
patchf('z80/cz80.c', "#if PICODRIVE_HACKS\nstatic inline unsigned char picodrive_read", """#ifdef Z80LOOP
struct z80loop { UINT32 pc; INT32 ic; UINT32 r; UINT8 regs[0x24]; UINT8 misc[8]; INT32 irq[2]; } z80loop;
int z80loop_mod;
unsigned z80loop_skips, z80lstat[4];
#define Z80LOOP_IO z80loop_mod = 1;
/* a value-changing write: Z80 RAM addresses are logged once per iteration with their value at the
   iteration start (tag = epoch); the iteration is unchanged if all of them hold that value again */
static unsigned char z80loop_tag[0x2000], z80loop_ep = 1, z80loop_lo[32];
static unsigned short z80loop_la[32];
static int z80loop_n;
#define Z80LOOP_W(p, d) { unsigned char *p_ = p, d_ = d; if (*p_ != d_) { unsigned o_ = p_ - PicoMem.zram; 	if (o_ >= 0x2000) z80loop_mod = 1; 	else if (z80loop_tag[o_] != z80loop_ep) { z80loop_tag[o_] = z80loop_ep; 		if (z80loop_n < 32) { z80loop_la[z80loop_n] = o_; z80loop_lo[z80loop_n++] = *p_; } else z80loop_mod = 1; } 	*p_ = d_; } }
static inline int z80loop_same(void)
{
	int i;
	for (i = 0; i < z80loop_n; i++) if (PicoMem.zram[z80loop_la[i]] != z80loop_lo[i]) return 0;
	return 1;
}
static inline void z80loop_new(void)
{
	z80loop_mod = 0; z80loop_n = 0;
	if (++z80loop_ep == 0) { memset(z80loop_tag, 0, sizeof(z80loop_tag)); z80loop_ep = 1; }
}
#define Z80LOOP_ENTRY { z80loop.pc = 0xffffffff; z80loop_new(); }
#elif defined(Z80PARK)
#define Z80LOOP_IO z80pk_io = 1;
#define Z80LOOP_W(p, d) { unsigned char *p_ = p, d_ = d; if (*p_ != d_) { if (z80pk_wlog) pk_wlog(p_, d_); *p_ = d_; } }
#define Z80LOOP_ENTRY
#else
#define Z80LOOP_IO
#define Z80LOOP_W(p, d) *(p) = (d);
#define Z80LOOP_ENTRY
#endif
#if PICODRIVE_HACKS
static inline unsigned char picodrive_read""")

# -DZ80ASM: z80x.S (mkz80x.py) runs the common opcodes; Cz80 does the rest
patchf('z80/cz80.c', "\tif (CPU->ICount > 0)\n\t{\nCz80_Exec_nocheck:", """\tif (CPU->ICount > 0)
	{
#ifdef Z80ASM
#ifdef Z80PARK
		if (!z80pk_noasm)
#endif
		PC = z80x_run(CPU, PC, PicoMem.zram);
		if (CPU->ICount <= 0) goto Cz80_Exec_End;
#endif
Cz80_Exec_nocheck:""")
patchf('z80/cz80.c', "INT32 Cz80_Exec(cz80_struc *CPU, INT32 cycles)\n{", """#ifdef Z80PARK
static INT32 Cz80_Exec_real(cz80_struc *CPU, INT32 cycles)
#else
INT32 Cz80_Exec(cz80_struc *CPU, INT32 cycles)
#endif
{""")
# Z80PARK single steps (z80pk_step = 1): the call ends at the first stop point, with ICount
# counting the whole budget (map handlers see the same z80_cyclesDone as in one call)
patchf('z80/cz80.c', "Cz80_Exec:\n\tif (CPU->Status)", """Cz80_Exec:
#ifdef Z80PARK
	if (z80pk_step && z80pk_step++ > 1) goto Cz80_Exec_End;
#endif
	if (CPU->Status)""")
patchf('z80/cz80.c', "void Cz80_Reset(cz80_struc *CPU)\n{", """#ifdef Z80PARK
static INT32 Cz80_Exec_real(cz80_struc *CPU, INT32 cycles);
#include "z80park.h"
#endif
void Cz80_Reset(cz80_struc *CPU)
{
#ifdef Z80PARK
	pk_materialize(CPU);
#endif""")
patchf('z80/cz80.c', "void Cz80_Set_IRQ(cz80_struc *CPU, INT32 line, INT32 state)\n{", """void Cz80_Set_IRQ(cz80_struc *CPU, INT32 line, INT32 state)
{
#ifdef Z80PARK
	if (line == IRQ_LINE_NMI ? (!state != !(CPU->Status & CZ80_HAS_NMI)) : (line != CPU->IRQLine || state != CPU->IRQState))
		pk_materialize(CPU);
#endif""")
patchf('z80/cz80.c', "#if PICODRIVE_HACKS\nstatic inline unsigned char picodrive_read", """#ifdef Z80ASM
#include <stddef.h>
FPTR z80x_run(cz80_struc *CPU, FPTR PC, void *zram);
/* offsets z80x.S uses */
_Static_assert(offsetof(cz80_struc, FA) == 6 && offsetof(cz80_struc, IX) == 8 && offsetof(cz80_struc, IY) == 10
	&& offsetof(cz80_struc, SP) == 12 && offsetof(cz80_struc, BC2) == 0x14 && offsetof(cz80_struc, FA2) == 0x1a
	&& offsetof(cz80_struc, R) == 0x1c && offsetof(cz80_struc, ICount) == 0x2c && offsetof(cz80_struc, BasePC) == 0x34
	&& offsetof(cz80_struc, Fetch) == 0x3c, "z80x.S offsets");
#endif
#if PICODRIVE_HACKS
static inline unsigned char picodrive_read""")
