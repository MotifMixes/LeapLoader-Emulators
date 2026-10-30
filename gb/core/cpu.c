



#include "defs.h"
#include "regs.h"
#include "hw.h"
#include "cpu.h"
#include "mem.h"
#include "fastmem.h"
#include "cpuregs.h"
#include "cpucore.h"
#include "lcdc.h"
#include "debug.h"

#ifdef USE_ASM
#include "asm.h"
#endif


struct cpu cpu;




#define ZFLAG(n) ( (n) ? 0 : FZ )
#define HFLAG(n) ( (n) ? 0 : FH )
#define CFLAG(n) ( (n) ? 0 : FC )


#define PUSH(w) ( (SP -= 2), (writew(xSP, (w))) )
#define POP(w) ( ((w) = readw(xSP)), (SP += 2) )


#define FETCH_OLD ( mbc.rmap[PC>>12] \
? mbc.rmap[PC>>12][PC++] \
: mem_read(PC++) )

#define FETCH (readb(PC++))


#define INC(r) { ((r)++); \
F = (F & (FL|FC)) | incflag_table[(r)]; }

#define DEC(r) { ((r)--); \
F = (F & (FL|FC)) | decflag_table[(r)]; }

#define INCW(r) ( (r)++ )

#define DECW(r) ( (r)-- )

#define ADD(n) { \
W(acc) = (un16)A + (un16)(n); \
F = (ZFLAG(LB(acc))) \
| (FH & ((A ^ (n) ^ LB(acc)) << 1)) \
| (HB(acc) << 4); \
A = LB(acc); }

#define ADC(n) { \
W(acc) = (un16)A + (un16)(n) + (un16)((F&FC)>>4); \
F = (ZFLAG(LB(acc))) \
| (FH & ((A ^ (n) ^ LB(acc)) << 1)) \
| (HB(acc) << 4); \
A = LB(acc); }

#define ADDW(n) { \
DW(acc) = (un32)HL + (un32)(n); \
F = (F & (FZ)) \
| (FH & ((H ^ ((n)>>8) ^ HB(acc)) << 1)) \
| (acc.b[HI][LO] << 4); \
HL = W(acc); }

#define ADDSP(n) { \
DW(acc) = (un32)SP + (un32)(n8)(n); \
F = (FH & (((SP>>8) ^ ((n)>>8) ^ HB(acc)) << 1)) \
| (acc.b[HI][LO] << 4); \
SP = W(acc); }

#define LDHLSP(n) { \
DW(acc) = (un32)SP + (un32)(n8)(n); \
F = (FH & (((SP>>8) ^ ((n)>>8) ^ HB(acc)) << 1)) \
| (acc.b[HI][LO] << 4); \
HL = W(acc); }

#define CP(n) { \
W(acc) = (un16)A - (un16)(n); \
F = FN \
| (ZFLAG(LB(acc))) \
| (FH & ((A ^ (n) ^ LB(acc)) << 1)) \
| ((un8)(-(n8)HB(acc)) << 4); }

#define SUB(n) { CP((n)); A = LB(acc); }

#define SBC(n) { \
W(acc) = (un16)A - (un16)(n) - (un16)((F&FC)>>4); \
F = FN \
| (ZFLAG((n8)LB(acc))) \
| (FH & ((A ^ (n) ^ LB(acc)) << 1)) \
| ((un8)(-(n8)HB(acc)) << 4); \
A = LB(acc); }

#define AND(n) { A &= (n); \
F = ZFLAG(A) | FH; }

#define XOR(n) { A ^= (n); \
F = ZFLAG(A); }

#define OR(n) { A |= (n); \
F = ZFLAG(A); }

#define RLCA(r) { (r) = ((r)>>7) | ((r)<<1); \
F = (((r)&0x01)<<4); }

#define RRCA(r) { (r) = ((r)<<7) | ((r)>>1); \
F = (((r)&0x80)>>3); }

#define RLA(r) { \
LB(acc) = (((r)&0x80)>>3); \
(r) = ((r)<<1) | ((F&FC)>>4); \
F = LB(acc); }

#define RRA(r) { \
LB(acc) = (((r)&0x01)<<4); \
(r) = ((r)>>1) | ((F&FC)<<3); \
F = LB(acc); }

#define RLC(r) { RLCA(r); F |= ZFLAG(r); }
#define RRC(r) { RRCA(r); F |= ZFLAG(r); }
#define RL(r) { RLA(r); F |= ZFLAG(r); }
#define RR(r) { RRA(r); F |= ZFLAG(r); }

#define SLA(r) { \
LB(acc) = (((r)&0x80)>>3); \
(r) <<= 1; \
F = ZFLAG((r)) | LB(acc); }

#define SRA(r) { \
LB(acc) = (((r)&0x01)<<4); \
(r) = (un8)(((n8)(r))>>1); \
F = ZFLAG((r)) | LB(acc); }

#define SRL(r) { \
LB(acc) = (((r)&0x01)<<4); \
(r) >>= 1; \
F = ZFLAG((r)) | LB(acc); }

#define CPL(r) { \
(r) = ~(r); \
F |= (FH|FN); }

#define SCF { F = (F & (FZ)) | FC; }

#define CCF { F = (F & (FZ|FC)) ^ FC; }

#define DAA { \
A += (LB(acc) = daa_table[((((int)F)&0x70)<<4) | A]); \
F = (F & (FN)) | ZFLAG(A) | daa_carry_table[LB(acc)>>2]; }

#define SWAP(r) { \
(r) = swap_table[(r)]; \
F = ZFLAG((r)); }

#define BIT(n,r) { F = (F & FC) | ZFLAG(((r) & (1 << (n)))) | FH; }
#define RES(n,r) { (r) &= ~(1 << (n)); }
#define SET(n,r) { (r) |= (1 << (n)); }

#define CB_REG_CASES(r, n) \
case 0x00|(n): RLC(r); break; \
case 0x08|(n): RRC(r); break; \
case 0x10|(n): RL(r); break; \
case 0x18|(n): RR(r); break; \
case 0x20|(n): SLA(r); break; \
case 0x28|(n): SRA(r); break; \
case 0x30|(n): SWAP(r); break; \
case 0x38|(n): SRL(r); break; \
case 0x40|(n): BIT(0, r); break; \
case 0x48|(n): BIT(1, r); break; \
case 0x50|(n): BIT(2, r); break; \
case 0x58|(n): BIT(3, r); break; \
case 0x60|(n): BIT(4, r); break; \
case 0x68|(n): BIT(5, r); break; \
case 0x70|(n): BIT(6, r); break; \
case 0x78|(n): BIT(7, r); break; \
case 0x80|(n): RES(0, r); break; \
case 0x88|(n): RES(1, r); break; \
case 0x90|(n): RES(2, r); break; \
case 0x98|(n): RES(3, r); break; \
case 0xA0|(n): RES(4, r); break; \
case 0xA8|(n): RES(5, r); break; \
case 0xB0|(n): RES(6, r); break; \
case 0xB8|(n): RES(7, r); break; \
case 0xC0|(n): SET(0, r); break; \
case 0xC8|(n): SET(1, r); break; \
case 0xD0|(n): SET(2, r); break; \
case 0xD8|(n): SET(3, r); break; \
case 0xE0|(n): SET(4, r); break; \
case 0xE8|(n): SET(5, r); break; \
case 0xF0|(n): SET(6, r); break; \
case 0xF8|(n): SET(7, r); break;


#define ALU_CASES(base, imm, op, label) \
case (imm): b = FETCH; goto label; \
case (base): b = B; goto label; \
case (base)+1: b = C; goto label; \
case (base)+2: b = D; goto label; \
case (base)+3: b = E; goto label; \
case (base)+4: b = H; goto label; \
case (base)+5: b = L; goto label; \
case (base)+6: b = readb(HL); goto label; \
case (base)+7: b = A; \
label: op(b); break;








#define JR ( PC += 1+(n8)readb(PC) )
#define JP ( PC = readw(PC) )

#define CALL ( PUSH(PC+2), JP )

#define NOJR ( clen--, PC++ )
#define NOJP ( clen--, PC+=2 )
#define NOCALL ( clen-=3, PC+=2 )
#define NORET ( clen-=3 )

#define RST(n) { PUSH(PC); PC = (n); }

#define RET ( POP(PC) )

#define EI ( IMA = 1 )
#define DI ( cpu.halt = IMA = IME = 0 )



#define PRE_INT ( DI, PUSH(PC) )
#define THROW_INT(n) ( (IF &= ~(1<<(n))), (PC = 0x40+((n)<<3)) )




void cpu_reset()
{
	cpu.speed = 0;
	cpu.halt = 0;
	cpu.div = 0;
	cpu.tim = 0;
	cpu.lcdc = 40;

	IME = 0;
	IMA = 0;

	PC = bootrom.bank ? 0 : 0x0100;
	SP = 0xFFFE;
	AF = 0x01B0;
	BC = 0x0013;
	DE = 0x00D8;
	HL = 0x014D;

	if (hw.cgb) A = 0x11;
	if (hw.gba) B = 0x01;
}


void div_advance(int cnt)
{
	cpu.div += (cnt<<1);
	if (cpu.div >= 256)
	{
		R_DIV += (cpu.div >> 8);
		cpu.div &= 0xff;
	}
}

void timer_advance(int cnt)
{
	int unit, tima;
	
	if (!(R_TAC & 0x04)) return;

	unit = ((-R_TAC) & 3) << 1;
	cpu.tim += (cnt<<unit);

	if (cpu.tim >= 512)
	{
		tima = R_TIMA + (cpu.tim >> 9);
		cpu.tim &= 0x1ff;
		if (tima >= 256)
		{
			hw_interrupt(IF_TIMER, IF_TIMER);
			hw_interrupt(0, IF_TIMER);
		}
		while (tima >= 256)
			tima = tima - 256 + R_TMA;
		R_TIMA = tima;
	}
}

void lcdc_advance(int cnt)
{
	cpu.lcdc -= cnt;
	if (cpu.lcdc <= 0) lcdc_trans();
}

void sound_advance(int cnt)
{
	cpu.snd += cnt;
}

void cpu_timers(int cnt)
{
	div_advance(cnt << cpu.speed);
	timer_advance(cnt << cpu.speed);
	lcdc_advance(cnt);
	sound_advance(cnt);
}

int cpu_idle(int max)
{
	int cnt, unit;

	if (!(cpu.halt && IME)) return 0;
	if (R_IF & R_IE)
	{
		cpu.halt = 0;
		return 0;
	}

	/* Make sure we don't miss lcdc status events! */
	if ((R_IE & (IF_VBLANK | IF_STAT)) && (max > cpu.lcdc))
		max = cpu.lcdc;
	
	/* If timer interrupt cannot happen, this is very simple! */
	if (!((R_IE & IF_TIMER) && (R_TAC & 0x04)))
	{
		cpu_timers(max);
		return max;
	}

	/* Figure out when the next timer interrupt will happen */
	unit = ((-R_TAC) & 3) << 1;
	cnt = (511 - cpu.tim + (1<<unit)) >> unit;
	cnt += (255 - R_TIMA) << (9 - unit);

	if (max < cnt)
		cnt = max;
	
	cpu_timers(cnt);
	return cnt;
}

#ifdef LL
/* Idle-loop skipping. A loop that only reads memory which cannot change
   between LCDC/timer events, with idempotent ALU ops, ends the same way
   each time round; such a loop is fast-forwarded to the next event, the
   way cpu_idle does for HALT. */
static int spin_safe(int a)
{
	if (a < 0xA000) return 1;			/* ROM, VRAM */
	if (a >= 0xC000 && a < 0xFE00) return 1;	/* WRAM, echo */
	if (a >= 0xFF80) return 1;			/* HRAM, IE */
	switch (a & 0xFF)
	{
	case RI_P1: case RI_IF: case RI_LCDC: case RI_STAT: case RI_LY: case RI_LYC:
		return a >= 0xFF00;
	}
	return 0;
}

/* A routine called from an idle loop: straight-line code up to RET that
   writes only WRAM/HRAM (none of the loop's polled addresses) with
   balanced PUSH/POP. Running it fewer times only means fewer steps of
   whatever it updates (typically a random number generator). */
static int spin_call(int a, int lo, int hi)
{
	byte op, b;
	int n, depth = 0, w;
	for (n = 0; n < 128; n++)
	{
		op = readb(a);
		switch (op)
		{
		case 0xC9: return depth == 0;
		case 0xC5: case 0xD5: case 0xE5: case 0xF5: depth++; a++; break;
		case 0xC1: case 0xD1: case 0xE1: case 0xF1: if (--depth < 0) return 0; a++; break;
		case 0x01: case 0x11: case 0x21: a += 3; break;
		case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E:
		case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
			a += 2; break;
		case 0x03: case 0x13: case 0x23: case 0x0B: case 0x1B: case 0x2B:
		case 0x09: case 0x19: case 0x29: case 0x39:
		case 0x0A: case 0x1A: case 0x2A: case 0x3A:
		case 0x07: case 0x0F: case 0x17: case 0x1F: case 0x2F: case 0x37: case 0x3F:
			a++; break;
		case 0xF0:
			if (!spin_safe(0xFF00 | readb(a+1))) return 0;
			a += 2; break;
		case 0xFA:
			if (!spin_safe(readw(a+1))) return 0;
			a += 3; break;
		case 0xEA: /* LD (nn),A */
			w = readw(a+1);
			if (!((w >= 0xC000 && w < 0xE000) || (w >= 0xFF80 && w < 0xFFFF))) return 0;
			if (w >= lo && w <= hi) return 0;
			a += 3; break;
		case 0xCB:
			b = readb(a+1);
			if ((b & 7) == 6 && (b & 0xC0) != 0x40) return 0; /* (HL) only for BIT */
			a += 2; break;
		default:
			if ((op & 0xC7) == 0x04 || (op & 0xC7) == 0x05) /* INC/DEC r */
			{
				if (op == 0x34 || op == 0x35) return 0;
				a++; break;
			}
			if (op >= 0x40 && op < 0x80 && (op & 0xF8) != 0x70) { a++; break; }
			if (op >= 0x80 && op < 0xC0) { a++; break; }
			return 0;
		}
	}
	return 0;
}

static int spin_scan(int a, int end)
{
	byte op, b;
	int start = a, c, lo = 0x10000, hi = -1, t, io = 0;
	while (a < end)
	{
		op = readb(a);
		switch (op)
		{
		case 0xF0: /* LDH A,(n) */
			t = 0xFF00 | readb(a+1);
			if (!spin_safe(t)) return 0;
			if (t < lo) lo = t;
			if (t > hi) hi = t;
			if (t >= 0xFF00 && (t < 0xFF80 || t == 0xFFFF)) io = 1;
			a += 2; break;
		case 0xFA: /* LD A,(nn) */
			t = readw(a+1);
			if (!spin_safe(t)) return 0;
			if (t < lo) lo = t;
			if (t > hi) hi = t;
			if (t >= 0xFF00 && (t < 0xFF80 || t == 0xFFFF)) io = 1;
			a += 3; break;
		case 0xCD: /* CALL nn: checked once the polled addresses are known */
			c = readw(a+1);
			if (c >= 0x8000 || (c >= 0x4000 && (start < 0x4000 || start >= 0x8000))) return 0;
			a += 3; break;
		case 0xE6: case 0xF6: case 0xFE: /* AND/OR/CP n */
		case 0x20: case 0x28: case 0x30: case 0x38: /* JR cc */
			a += 2; break;
		case 0xCB:
			b = readb(a+1);
			if (b < 0x40 || b >= 0x80 || (b & 7) == 6) return 0; /* BIT n,r only */
			a += 2; break;
		default:
			/* AND/OR/CP r (not (HL)) */
			if (op >= 0xA0 && op < 0xA8 && op != 0xA6) { a++; break; }
			if (op >= 0xB0 && op < 0xC0 && op != 0xB6 && op != 0xBE) { a++; break; }
			return 0;
		}
	}
	if (a != end) return 0;
	for (a = start; a < end; )
	{
		op = readb(a);
		if (op == 0xCD && !spin_call(readw(a+1), lo, hi)) return 0;
		a += op == 0xCD || op == 0xFA ? 3 : op == 0xCB || op == 0xF0 || op == 0xE6 || op == 0xF6
			|| op == 0xFE || (op & 0xE7) == 0x20 ? 2 : 1;
	}
	return io ? 1 : 2;
}

/* LL: a loop polling no I/O register (spin_scan 2) can only end through an
   interrupt; its start PC | rombank << 16 is kept here when a slice ended
   inside it, and the next slices are skipped while no interrupt is due */
static int ll_spin_pc = -1;

/* is [a,end) an idle loop (1 = polls I/O, 2 = memory only)? ROM results
   are cached per bank */
static int spin_check(int a, int end)
{
	static unsigned key[256];
	static byte bank[256], res[256];
	int k, r;
	if (end - a > 24) return 0;
	if (a >= 0x8000) return spin_scan(a, end);
	k = (a ^ (a >> 8)) & 255;
	if (key[k] == (unsigned)(a | end << 16) && (a < 0x4000 || bank[k] == (byte)mbc.rombank))
		return res[k];
	r = spin_scan(a, end);
	key[k] = a | end << 16; bank[k] = mbc.rombank; res[k] = r;
	return r;
}

/* cycles to skip: up to the next LCDC event or timer overflow */
static int spin_skip(int max)
{
	int cnt, unit;
	if (IME != IMA || (IME && (IF & IE))) return 0;
	if (max > cpu.lcdc) max = cpu.lcdc;
	if (R_TAC & 0x04)
	{
		unit = ((-R_TAC) & 3) << 1;
		cnt = (511 - cpu.tim + (1<<unit)) >> unit;
		cnt += (255 - R_TIMA) << (9 - unit);
		if (max > cnt) max = cnt;
	}
	if (max <= 0) return 0;
	cpu_timers(max);
	return max;
}
#endif

#ifndef ASM_CPU_EMULATE

extern int debug_trace;

/* LL: PC and SP live in locals while cpu_emulate runs (nothing it calls
   reads them); written back on return */
#ifdef LL
#undef PC
#undef SP
#undef xPC
#undef xSP
#define PC lpc
#define SP lsp
#define xPC lpc
#define xSP lsp
#define SYNC_REGS ( W(cpu.pc) = lpc, W(cpu.sp) = lsp )

/* LL: div/timer/lcdc/sound are advanced lazily. pend counts cycles (clen<<1
   units) not yet given to them; budget is how many may pass before the next
   event (LCDC, timer overflow, end of the slice). Every slow-path memory
   access catches up first, so the result is the same as advancing after
   each instruction. */
static int ll_flush(int c)
{
	cpu.div += c << 1;
	if (cpu.div >= 256)
	{
		R_DIV += cpu.div >> 8;
		cpu.div &= 0xff;
	}
	if (R_TAC & 0x04) timer_advance(c);
	c >>= cpu.speed;
	cpu.snd += c;
	if ((cpu.lcdc -= c) <= 0) lcdc_trans();
	return c;
}

static int ll_budget(int i)
{
	int b, n, unit;
	b = (i < cpu.lcdc ? i : cpu.lcdc) << cpu.speed;
	if (R_TAC & 0x04)
	{
		unit = ((-R_TAC) & 3) << 1;
		n = ((256 - R_TIMA) << 9) - cpu.tim;
		n = (n + (1 << unit) - 1) >> unit;
		if (n < b) b = n;
	}
	return b;
}

#define SYNC ( pend ? (i -= ll_flush(pend), pend = 0) : 0 )
#undef readhi
#undef writehi
#define readb(x) ({ int a_ = (x); byte *p_ = mbc.rmap[a_>>12]; 	p_ ? p_[a_] : a_ >= 0xFF80 ? ram.hi[a_ & 0xFF] : (SYNC, mem_read(a_)); })
#define readw(x) ({ int a_ = (x); byte *p_ = mbc.rmap[a_>>12]; 	(p_ && ((a_+1) & 0xfff)) ? p_[a_] | (p_[a_+1] << 8) : 	(a_ >= 0xFF80 && a_ < 0xFFFF) ? ram.hi[a_ & 0xFF] | (ram.hi[(a_+1) & 0xFF] << 8) : 	(SYNC, mem_read(a_) | (mem_read(a_+1) << 8)); })
#define writeb(x, v) ({ int a_ = (x); byte v_ = (v); byte *p_ = mbc.wmap[a_>>12]; 	if (p_) p_[a_] = v_; 	else if (a_ >= 0xFF80 && a_ != 0xFFFF) ram.hi[a_ & 0xFF] = v_; 	else { SYNC; mem_write(a_, v_); budget = 0; } })
#define writew(x, v) ({ int a_ = (x); int v_ = (v); byte *p_ = mbc.wmap[a_>>12]; 	if (p_ && ((a_+1) & 0xfff)) { p_[a_] = v_; p_[a_+1] = v_ >> 8; } 	else if (a_ >= 0xFF80 && a_ < 0xFFFE) { ram.hi[a_ & 0xFF] = v_; ram.hi[(a_+1) & 0xFF] = v_ >> 8; } 	else { SYNC; mem_write(a_, v_); mem_write(a_+1, v_ >> 8); budget = 0; } })
#define readhi(x) readb((x) | 0xff00)
#define writehi(x, v) writeb((x) | 0xff00, (v))
#else
#define SYNC_REGS 0
#endif

int cpu_emulate(int cycles)
{
#ifdef LL
	word lpc = W(cpu.pc), lsp = W(cpu.sp);
	int spin = 0, pend = 0, budget = ll_budget(cycles);
#endif
	int i;
	byte op, cbop;
	int clen;
	union reg acc;
	byte b;
	word w;

	i = cycles;
#ifdef LL
	if (ll_spin_pc >= 0)
	{
		clen = ll_spin_pc == (lpc | mbc.rombank << 16) ? spin_skip(i) : 0;
		if (clen == i) return cycles;
		ll_spin_pc = -1;
		i -= clen;
		budget = ll_budget(i);
	}
#endif
next:
#ifdef LL
	if (cpu.halt)
	{
		SYNC;
		if ((clen = cpu_idle(i)))
		{
			i -= clen;
			budget = ll_budget(i);
			if (i > 0) goto next;
			SYNC_REGS;
			return cycles-i;
		}
	}
#else
	if (cpu.halt && (clen = cpu_idle(i)))
	{
		i -= clen;
		if (i > 0) goto next;
		return cycles-i;
	}
#endif

	if (IME && (IF & IE))
	{
		PRE_INT;
		switch ((byte)(IF & IE))
		{
		case 0x01: case 0x03: case 0x05: case 0x07:
		case 0x09: case 0x0B: case 0x0D: case 0x0F:
		case 0x11: case 0x13: case 0x15: case 0x17:
		case 0x19: case 0x1B: case 0x1D: case 0x1F:
			THROW_INT(0); break;
		case 0x02: case 0x06: case 0x0A: case 0x0E:
		case 0x12: case 0x16: case 0x1A: case 0x1E:
			THROW_INT(1); break;
		case 0x04: case 0x0C: case 0x14: case 0x1C:
			THROW_INT(2); break;
		case 0x08: case 0x18:
			THROW_INT(3); break;
		case 0x10:
			THROW_INT(4); break;
		}
	}
	IME = IMA;
	
#ifndef LL
	if (debug_trace) debug_disassemble(PC, 1);
#endif
#ifdef LL_PCHIST
	{ extern unsigned ll_pchist[65536]; ll_pchist[PC]++; }
#endif
	op = FETCH;
	clen = cycles_table[op];

	switch(op)
	{
	case 0x00: /* NOP */
	case 0x40: /* LD B,B */
	case 0x49: /* LD C,C */
	case 0x52: /* LD D,D */
	case 0x5B: /* LD E,E */
	case 0x64: /* LD H,H */
	case 0x6D: /* LD L,L */
	case 0x7F: /* LD A,A */
		break;
			
	case 0x41: /* LD B,C */
		B = C; break;
	case 0x42: /* LD B,D */
		B = D; break;
	case 0x43: /* LD B,E */
		B = E; break;
	case 0x44: /* LD B,H */
		B = H; break;
	case 0x45: /* LD B,L */
		B = L; break;
	case 0x46: /* LD B,(HL) */
		B = readb(xHL); break;
	case 0x47: /* LD B,A */
		B = A; break;

	case 0x48: /* LD C,B */
		C = B; break;
	case 0x4A: /* LD C,D */
		C = D; break;
	case 0x4B: /* LD C,E */
		C = E; break;
	case 0x4C: /* LD C,H */
		C = H; break;
	case 0x4D: /* LD C,L */
		C = L; break;
	case 0x4E: /* LD C,(HL) */
		C = readb(xHL); break;
	case 0x4F: /* LD C,A */
		C = A; break;

	case 0x50: /* LD D,B */
		D = B; break;
	case 0x51: /* LD D,C */
		D = C; break;
	case 0x53: /* LD D,E */
		D = E; break;
	case 0x54: /* LD D,H */
		D = H; break;
	case 0x55: /* LD D,L */
		D = L; break;
	case 0x56: /* LD D,(HL) */
		D = readb(xHL); break;
	case 0x57: /* LD D,A */
		D = A; break;

	case 0x58: /* LD E,B */
		E = B; break;
	case 0x59: /* LD E,C */
		E = C; break;
	case 0x5A: /* LD E,D */
		E = D; break;
	case 0x5C: /* LD E,H */
		E = H; break;
	case 0x5D: /* LD E,L */
		E = L; break;
	case 0x5E: /* LD E,(HL) */
		E = readb(xHL); break;
	case 0x5F: /* LD E,A */
		E = A; break;

	case 0x60: /* LD H,B */
		H = B; break;
	case 0x61: /* LD H,C */
		H = C; break;
	case 0x62: /* LD H,D */
		H = D; break;
	case 0x63: /* LD H,E */
		H = E; break;
	case 0x65: /* LD H,L */
		H = L; break;
	case 0x66: /* LD H,(HL) */
		H = readb(xHL); break;
	case 0x67: /* LD H,A */
		H = A; break;
			
	case 0x68: /* LD L,B */
		L = B; break;
	case 0x69: /* LD L,C */
		L = C; break;
	case 0x6A: /* LD L,D */
		L = D; break;
	case 0x6B: /* LD L,E */
		L = E; break;
	case 0x6C: /* LD L,H */
		L = H; break;
	case 0x6E: /* LD L,(HL) */
		L = readb(xHL); break;
	case 0x6F: /* LD L,A */
		L = A; break;
			
	case 0x70: /* LD (HL),B */
		b = B; goto __LD_HL;
	case 0x71: /* LD (HL),C */
		b = C; goto __LD_HL;
	case 0x72: /* LD (HL),D */
		b = D; goto __LD_HL;
	case 0x73: /* LD (HL),E */
		b = E; goto __LD_HL;
	case 0x74: /* LD (HL),H */
		b = H; goto __LD_HL;
	case 0x75: /* LD (HL),L */
		b = L; goto __LD_HL;
	case 0x77: /* LD (HL),A */
		b = A;
	__LD_HL:
		writeb(xHL,b);
		break;
			
	case 0x78: /* LD A,B */
		A = B; break;
	case 0x79: /* LD A,C */
		A = C; break;
	case 0x7A: /* LD A,D */
		A = D; break;
	case 0x7B: /* LD A,E */
		A = E; break;
	case 0x7C: /* LD A,H */
		A = H; break;
	case 0x7D: /* LD A,L */
		A = L; break;
	case 0x7E: /* LD A,(HL) */
		A = readb(xHL); break;

	case 0x01: /* LD BC,imm */
		BC = readw(xPC); PC += 2; break;
	case 0x11: /* LD DE,imm */
		DE = readw(xPC); PC += 2; break;
	case 0x21: /* LD HL,imm */
		HL = readw(xPC); PC += 2; break;
	case 0x31: /* LD SP,imm */
		SP = readw(xPC); PC += 2; break;

	case 0x02: /* LD (BC),A */
		writeb(xBC, A); break;
	case 0x0A: /* LD A,(BC) */
		A = readb(xBC); break;
	case 0x12: /* LD (DE),A */
		writeb(xDE, A); break;
	case 0x1A: /* LD A,(DE) */
		A = readb(xDE); break;

	case 0x22: /* LDI (HL),A */
		writeb(xHL, A); HL++; break;
	case 0x2A: /* LDI A,(HL) */
		A = readb(xHL); HL++; break;
	case 0x32: /* LDD (HL),A */
		writeb(xHL, A); HL--; break;
	case 0x3A: /* LDD A,(HL) */
		A = readb(xHL); HL--; break;

	case 0x06: /* LD B,imm */
		B = FETCH; break;
	case 0x0E: /* LD C,imm */
		C = FETCH; break;
	case 0x16: /* LD D,imm */
		D = FETCH; break;
	case 0x1E: /* LD E,imm */
		E = FETCH; break;
	case 0x26: /* LD H,imm */
		H = FETCH; break;
	case 0x2E: /* LD L,imm */
		L = FETCH; break;
	case 0x36: /* LD (HL),imm */
		b = FETCH; writeb(xHL, b); break;
	case 0x3E: /* LD A,imm */
		A = FETCH; break;

	case 0x08: /* LD (imm),SP */
		writew(readw(xPC), SP); PC += 2; break;
	case 0xEA: /* LD (imm),A */
		writeb(readw(xPC), A); PC += 2; break;

	case 0xE0: /* LDH (imm),A */
		writehi(FETCH, A); break;
	case 0xE2: /* LDH (C),A */
		writehi(C, A); break;
	case 0xF0: /* LDH A,(imm) */
		A = readhi(FETCH); break;
	case 0xF2: /* LDH A,(C) (undocumented) */
		A = readhi(C); break;
			

	case 0xF8: /* LD HL,SP+imm */
		{
			/* https://gammpei.github.io/blog/posts/2018-03-04/how-to-write-a-game-boy-emulator-part-8-blarggs-cpu-test-roms-1-3-4-5-7-8-9-10-11.html */
			signed char v = (signed char) FETCH;
			int temp = (int)(SP) + (int)v;

			byte half_carry = ((SP & 0xff) ^ v ^ temp) & 0x10;

			F &= ~(FZ | FN | FH | FC);

			if (half_carry) F |= FH;
			if ((SP & 0xff) + (byte)v > 0xff) F |= FC;

			HL = temp & 0xffff;
		}
		break;
	case 0xF9: /* LD SP,HL */
		SP = HL; break;
	case 0xFA: /* LD A,(imm) */
		A = readb(readw(xPC)); PC += 2; break;

		ALU_CASES(0x80, 0xC6, ADD, __ADD)
		ALU_CASES(0x88, 0xCE, ADC, __ADC)
		ALU_CASES(0x90, 0xD6, SUB, __SUB)
		ALU_CASES(0x98, 0xDE, SBC, __SBC)
		ALU_CASES(0xA0, 0xE6, AND, __AND)
		ALU_CASES(0xA8, 0xEE, XOR, __XOR)
		ALU_CASES(0xB0, 0xF6, OR, __OR)
		ALU_CASES(0xB8, 0xFE, CP, __CP)

	case 0x09: /* ADD HL,BC */
		w = BC; goto __ADDW;
	case 0x19: /* ADD HL,DE */
		w = DE; goto __ADDW;
	case 0x39: /* ADD HL,SP */
		w = SP; goto __ADDW;
	case 0x29: /* ADD HL,HL */
		w = HL;
	__ADDW:
		ADDW(w);
		break;

	case 0x04: /* INC B */
		INC(B); break;
	case 0x0C: /* INC C */
		INC(C); break;
	case 0x14: /* INC D */
		INC(D); break;
	case 0x1C: /* INC E */
		INC(E); break;
	case 0x24: /* INC H */
		INC(H); break;
	case 0x2C: /* INC L */
		INC(L); break;
	case 0x34: /* INC (HL) */
		b = readb(xHL);
		INC(b);
		writeb(xHL, b);
		break;
	case 0x3C: /* INC A */
		INC(A); break;
			
	case 0x03: /* INC BC */
		INCW(BC); break;
	case 0x13: /* INC DE */
		INCW(DE); break;
	case 0x23: /* INC HL */
		INCW(HL); break;
	case 0x33: /* INC SP */
		INCW(SP); break;
			
	case 0x05: /* DEC B */
		DEC(B); break;
	case 0x0D: /* DEC C */
		DEC(C); break;
	case 0x15: /* DEC D */
		DEC(D); break;
	case 0x1D: /* DEC E */
		DEC(E); break;
	case 0x25: /* DEC H */
		DEC(H); break;
	case 0x2D: /* DEC L */
		DEC(L); break;
	case 0x35: /* DEC (HL) */
		b = readb(xHL);
		DEC(b);
		writeb(xHL, b);
		break;
	case 0x3D: /* DEC A */
		DEC(A); break;

	case 0x0B: /* DEC BC */
		DECW(BC); break;
	case 0x1B: /* DEC DE */
		DECW(DE); break;
	case 0x2B: /* DEC HL */
		DECW(HL); break;
	case 0x3B: /* DEC SP */
		DECW(SP); break;

	case 0x07: /* RLCA */
		RLCA(A); break;
	case 0x0F: /* RRCA */
		RRCA(A); break;
	case 0x17: /* RLA */
		RLA(A); break;
	case 0x1F: /* RRA */
		RRA(A); break;

	case 0x27: /* DAA */
		{
			int a = A;
			if (!(F & FN))
			{
				if ((F & FH) || ((a & 0x0f) > 9)) a += 0x06;

				if ((F & FC) || (a > 0x9f)) a += 0x60;
			}
			else
			{
				if (F & FH) a = (a - 6) & 0xff;
				if (F & FC) a -= 0x60;
			}

			F &= ~(FH | FZ);

			if (a & 0x100) F |= FC;

			a &= 0xff;

			if (!a) F |= FZ;

			A = (byte)a;
		}
		break;
	case 0x2F: /* CPL */
		CPL(A); break;

	case 0x18: /* JR */
	__JR:
#ifdef LL
		if ((n8)readb(PC) < 0) spin = PC + 1, budget = 0;	/* loop end */
#endif
		JR; break;
	case 0x20: /* JR NZ */
		if (!(F&FZ)) goto __JR; NOJR; break;
	case 0x28: /* JR Z */
		if (F&FZ) goto __JR; NOJR; break;
	case 0x30: /* JR NC */
		if (!(F&FC)) goto __JR; NOJR; break;
	case 0x38: /* JR C */
		if (F&FC) goto __JR; NOJR; break;

	case 0xC3: /* JP */
	__JP:
		JP; break;
	case 0xC2: /* JP NZ */
		if (!(F&FZ)) goto __JP; NOJP; break;
	case 0xCA: /* JP Z */
		if (F&FZ) goto __JP; NOJP; break;
	case 0xD2: /* JP NC */
		if (!(F&FC)) goto __JP; NOJP; break;
	case 0xDA: /* JP C */
		if (F&FC) goto __JP; NOJP; break;
	case 0xE9: /* JP HL */
		PC = HL; break;

	case 0xC9: /* RET */
	__RET:
		RET; break;
	case 0xC0: /* RET NZ */
		if (!(F&FZ)) goto __RET; NORET; break;
	case 0xC8: /* RET Z */
		if (F&FZ) goto __RET; NORET; break;
	case 0xD0: /* RET NC */
		if (!(F&FC)) goto __RET; NORET; break;
	case 0xD8: /* RET C */
		if (F&FC) goto __RET; NORET; break;
	case 0xD9: /* RETI */
		IME = IMA = 1; goto __RET;

	case 0xCD: /* CALL */
	__CALL:
		CALL; break;
	case 0xC4: /* CALL NZ */
		if (!(F&FZ)) goto __CALL; NOCALL; break;
	case 0xCC: /* CALL Z */
		if (F&FZ) goto __CALL; NOCALL; break;
	case 0xD4: /* CALL NC */
		if (!(F&FC)) goto __CALL; NOCALL; break;
	case 0xDC: /* CALL C */
		if (F&FC) goto __CALL; NOCALL; break;

	case 0xC7: /* RST 0 */
		b = 0x00; goto __RST;
	case 0xCF: /* RST 8 */
		b = 0x08; goto __RST;
	case 0xD7: /* RST 10 */
		b = 0x10; goto __RST;
	case 0xDF: /* RST 18 */
		b = 0x18; goto __RST;
	case 0xE7: /* RST 20 */
		b = 0x20; goto __RST;
	case 0xEF: /* RST 28 */
		b = 0x28; goto __RST;
	case 0xF7: /* RST 30 */
		b = 0x30; goto __RST;
	case 0xFF: /* RST 38 */
		b = 0x38;
	__RST:
		RST(b); break;
			
	case 0xC1: /* POP BC */
		POP(BC); break;
	case 0xC5: /* PUSH BC */
		PUSH(BC); break;
	case 0xD1: /* POP DE */
		POP(DE); break;
	case 0xD5: /* PUSH DE */
		PUSH(DE); break;
	case 0xE1: /* POP HL */
		POP(HL); break;
	case 0xE5: /* PUSH HL */
		PUSH(HL); break;
	case 0xF1: /* POP AF */
		POP(AF); AF &= 0xfff0; break;
	case 0xF5: /* PUSH AF */
		PUSH(AF); break;

	case 0xE8: /* ADD SP,imm */
		{
			/* https://gammpei.github.io/blog/posts/2018-03-04/how-to-write-a-game-boy-emulator-part-8-blarggs-cpu-test-roms-1-3-4-5-7-8-9-10-11.html */
			signed char v = (signed char) FETCH;
			int temp = (int)(SP) + (int)v;

			byte half_carry = ((SP & 0xff) ^ v ^ temp) & 0x10;

			F &= ~(FZ | FN | FH | FC);

			if (half_carry) F |= FH;
			if ((SP & 0xff) + (byte)v > 0xff) F |= FC;

			SP = temp & 0xffff;
		}
		break;


	case 0xF3: /* DI */
		DI; break;
	case 0xFB: /* EI */
		EI; break;

	case 0x37: /* SCF */
		SCF; break;
	case 0x3F: /* CCF */
		CCF; break;

	case 0x10: /* STOP */
		PC++;
		if (R_KEY1 & 1)
		{
#ifdef LL
			SYNC; budget = 0;
#endif
			cpu.speed = cpu.speed ^ 1;
			R_KEY1 = (R_KEY1 & 0x7E) | (cpu.speed << 7);
			break;
		}
		/* NOTE - we do not implement dmg STOP whatsoever */
		break;
			
	case 0x76: /* HALT */
		cpu.halt = 1;
		break;

	case 0xCB: /* CB prefix */
		cbop = FETCH;
		clen = cb_cycles_table[cbop];
		switch (cbop)
		{
			CB_REG_CASES(B, 0);
			CB_REG_CASES(C, 1);
			CB_REG_CASES(D, 2);
			CB_REG_CASES(E, 3);
			CB_REG_CASES(H, 4);
			CB_REG_CASES(L, 5);
			CB_REG_CASES(A, 7);
		default:
			b = readb(xHL);
			switch(cbop)
			{
				CB_REG_CASES(b, 6);
			}
			if ((cbop & 0xC0) != 0x40) /* exclude BIT */
				writeb(xHL, b);
			break;
		}
		break;
			
	default:
		die(
			"invalid opcode 0x%02X at address 0x%04X, rombank = %d\n",
			op, (PC-1) & 0xffff, mbc.rombank);
		break;
	}

#ifdef LL
	clen <<= 1;
	pend += clen;
	if ((budget -= clen) > 0) goto next;
	SYNC;
	if (spin)
	{
		if ((clen = spin_check(PC, spin)) && i > 0)
		{
			i -= spin_skip(i);
			if (clen == 2 && i <= 0) ll_spin_pc = PC | mbc.rombank << 16;
		}
		spin = 0;
	}
	if (i > 0)
	{
		budget = ll_budget(i);
		goto next;
	}
	SYNC_REGS;
	return cycles-i;
#else
	/* div/timer/lcdc/sound_advance, inlined */
	clen <<= 1;
	cpu.div += clen << 1;
	if (cpu.div >= 256)
	{
		R_DIV += cpu.div >> 8;
		cpu.div &= 0xff;
	}
	if (R_TAC & 0x04)
	{
		cpu.tim += clen << (((-R_TAC) & 3) << 1);
		if (cpu.tim >= 512) timer_advance(0);
	}
	clen >>= cpu.speed;
	cpu.snd += clen;
	if ((cpu.lcdc -= clen) <= 0) lcdc_trans();

	i -= clen;
	if (i > 0) goto next;
	return cycles-i;
#endif
}

#ifdef LL
#undef readb
#undef readw
#undef writeb
#undef writew
#undef readhi
#undef writehi
#undef SYNC
#undef PC
#undef SP
#undef xPC
#undef xSP
#define PC W(cpu.pc)
#define SP W(cpu.sp)
#define xPC DW(cpu.pc)
#define xSP DW(cpu.sp)
#endif

#endif /* ASM_CPU_EMULATE */


#ifndef ASM_CPU_STEP

int cpu_step(int max)
{
	int cnt;
	if ((cnt = cpu_idle(max))) return cnt;
	return cpu_emulate(1);
}

#endif /* ASM_CPU_STEP */












