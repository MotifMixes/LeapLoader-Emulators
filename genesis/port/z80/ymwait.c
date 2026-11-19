/* ymwait.c (-DZ80IDLE3, z80x.S): a Z80 loop "BIT b,(HL); JR/JP Z,loop" with HL on the YM2612
 * status (4000-5fff) and b = 0/1 waits for a timer flag. Each read is ym2612_read_local_z80:
 * status = busy bit (xcycles < ym2612_busy) | sticky timer flags set once xcycles >= the timer's
 * next overflow (if the mode enables the flag). The read of iteration j happens at a known
 * cycle, so the iterations whose read still gives 0 are skipped; the status afterwards is that
 * of the last skipped read (sticky bits only grow, the busy bit is the last read's). Called at
 * the taken jump with c = ICount (before the jump's own cycles), per = cycles an iteration,
 * first = the jump's cycles (the read is at ICount = c - first - j * per). Returns the iterations
 * skipped (each one leaves ICount > 0, as running them would) and sets CZ80.ICount. No lp, no
 * division. In .xtext (after all hot code). */
#include "pico_int.h"
#include "sound/ym2612.h"
#include "../../picodrive/cpu/cz80/cz80.h"

__attribute__((section(".xtext.ymwait"))) int z80_ymwait(int c, int per, int first, int b)
{
	int m = 0, last = 0, k = c - first, aim = Pico.t.z80c_aim, ex = CZ80.ExtraCycles;
	int en = (ym2612.OPN.ST.mode >> (2 + b)) & 1;
	int ofl = b ? Pico.t.timer_b_next_oflow : Pico.t.timer_a_next_oflow;
	if (b > 1 || (ym2612.OPN.ST.status & (1 << b)))
		return 0;
	while (c > per) {
		int t = (aim - (k - ex)) << 8;
		if (en && t >= ofl)
			break;
		c -= per; k -= per; m++; last = t;
	}
	if (m) {
		ym2612.OPN.ST.status &= ~0x80;
		ym2612.OPN.ST.status |= (last < Pico.t.ym2612_busy) * 0x80;
		if (last >= Pico.t.timer_a_next_oflow) ym2612.OPN.ST.status |= (ym2612.OPN.ST.mode >> 2) & 1;
		if (last >= Pico.t.timer_b_next_oflow) ym2612.OPN.ST.status |= (ym2612.OPN.ST.mode >> 2) & 2;
	}
	CZ80.ICount = c;
	return m;
}

/* -DZ80IDLE4 (z80x.S, alternative JR e handler): a taken "JR $-9" (18 f5) closing the loop
 *   L: LD A,(nn); OR A; JR NZ,x; CALL P; JR L     P: LD A,(4000-5fff); RRCA; CALL C,a; RRCA; CALL C,b; RET
 * (Technosoft's driver, Lightening Force) waits for (nn) != 0 or a YM2612 timer flag. With A == 0 now
 * (the status read of the iteration just run was 0) and (nn) == 0 (Z80 RAM can't change inside one Exec
 * call), every later iteration whose status read is still 0 (not busy, no enabled timer overflowed) ends
 * in the same state: A, F, the stacked return address, PC. Those are skipped: 104 cycles and R += 11
 * each; the status read is 53 cycles after the jump's start. c = ICount before the jump's cycles,
 * off = offset of the jump's operand in Z80 RAM. Returns the iterations skipped, sets CZ80.ICount. */
#ifdef TFDBG
unsigned tfdbg[8];
#define TF(n) tfdbg[n]++
#else
#define TF(n)
#endif
__attribute__((section(".xtext.ymwait"))) int z80_tfwait(int c, int off, int af, int sp)
{
	int a = af & 0xff;
	TF(0);
	if ((af >> 8) != 0x44) { TF(1); return 0; }   /* F as a clean iteration leaves it (OR A on 0, RRCA x2) */
	const u8 *z = PicoMem.zram;
	int t0 = off - 10, nn, p, m = 0, k, aim, ex, enA, enB;
	if (a != 0 || sp < 2 || sp > 0x2000 || t0 < 0 || off > 0x1ff0 || z[t0] != 0x3a || z[t0 + 3] != 0xb7 || z[t0 + 4] != 0x20 ||
	    z[t0 + 6] != 0xcd || z[t0 + 9] != 0x18)
		{ TF(2); return 0; }
	nn = z[t0 + 1] | z[t0 + 2] << 8;
	p = z[t0 + 7] | z[t0 + 8] << 8;
	if (nn >= 0x2000 || z[nn] != 0 || p > 0x1ff0 || z[p] != 0x3a || z[p + 2] < 0x40 || z[p + 2] > 0x5f ||
	    z[p + 3] != 0x0f || z[p + 4] != 0xdc || z[p + 7] != 0x0f || z[p + 8] != 0xdc || z[p + 11] != 0xc9)
		{ TF(3); return 0; }
	if (ym2612.OPN.ST.status & 0x83)
		{ TF(4); return 0; }
	aim = Pico.t.z80c_aim; ex = CZ80.ExtraCycles; k = c - 53;
	enA = (ym2612.OPN.ST.mode >> 2) & 1; enB = (ym2612.OPN.ST.mode >> 3) & 1;
	while (c > 104) {
		int t = (aim - (k - ex)) << 8;
		if ((enA && t >= Pico.t.timer_a_next_oflow) || (enB && t >= Pico.t.timer_b_next_oflow) || t < Pico.t.ym2612_busy)
			break;
		c -= 104; k -= 104; m++;
	}
	TF(m ? 5 : 6); if (c > 104 && !m) TF(7);
	TF(m ? 5 : 6); if (c > 104 && !m) TF(7);
	CZ80.ICount = c;
	return m;
}
