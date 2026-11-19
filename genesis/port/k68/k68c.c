/* k68c.c: the C side of the hand-written ARC600 68000 core (k68i.S, -DK68).
 * PicoDrive keeps using FAME's context (M68K_CONTEXT PicoCpuFM68k) and its
 * entry points; only the instruction loop is replaced: fm68k_emulate() below
 * is FAME's own, with "NEXT" (the opcode loop) replaced by k68_exec().
 * FAME (famec.c) is compiled in for fm68k_init/reset/get_pc, the idle-loop
 * detector and as the reference of the K68CHECK lockstep checker. */
#include <stddef.h>
#include "../../picodrive/cpu/fame/fame.h"
#define OFS(f) (offsetof(M68K_CONTEXT, f) - 0x20)
_Static_assert(offsetof(M68K_CONTEXT, dreg) == 0x20, "dreg");
_Static_assert(OFS(areg) == 0x20 && OFS(asp) == 0x40 && OFS(interrupts) == 0x48, "a");
_Static_assert(OFS(sr) == 0x50 && OFS(execinfo) == 0x52 && OFS(io_cycle_counter) == 0x54, "b");
_Static_assert(OFS(cycles_needed) == 0x5c && OFS(PC) == 0x60 && OFS(BasePC) == 0x64, "c");
_Static_assert(OFS(flag_C) == 0x68 && OFS(flag_I) == 0x84 && OFS(Fetch) == 0x8c, "d");

#define fm68k_emulate     fame_emulate
#define fm68k_idle_install fame_idle_install
#define fm68k_idle_remove fame_idle_remove
#include "../../picodrive/cpu/fame/famec.c"
#undef fm68k_emulate
#undef fm68k_idle_install
#undef fm68k_idle_remove
#include <stdio.h>
extern M68K_CONTEXT PicoCpuFM68k;
#include "../../picodrive/pico/memory.h"

extern void k68_exec(M68K_CONTEXT *ctx);   /* k68i.S: runs until io_cycle_counter <= 0 */
extern void *k68_table[8192];              /* k68i.S: handler per opcode >> 3 */
opcode_func *k68_fametab = JumpTable;      /* for k68_fallback (FAME's handler for an opcode) */
int k68_idle_on;
unsigned k68_fallbacks;
#ifdef K68STATS
/* -DK68STATS: FAME fallbacks per opcode, the most frequent printed at exit */
unsigned k68_fbcount[65536];
void k68_stats(void)
{
	int i, j, best;
	printf("K68STATS: %u fallbacks\n", k68_fallbacks);
#ifdef K68JIT
	{ extern unsigned k68_blocks, k68_flushes, k68_stales, k68_collisions;
	  printf("K68STATS: %u blocks made, %u cache flushes, %u stale RAM blocks, %u hash replacements\n",
		k68_blocks, k68_flushes, k68_stales, k68_collisions); }
#endif
	for (j = 0; j < 40; j++) {
		for (best = 0, i = 1; i < 65536; i++)
			if (k68_fbcount[i] > k68_fbcount[best]) best = i;
		if (!k68_fbcount[best]) break;
		printf("K68STATS %04x %s %u\n", best, "", k68_fbcount[best]);
		k68_fbcount[best] = 0;
	}
}
#endif

#ifdef K68CHECK
static void k68_check_run(M68K_CONTEXT *ctx);
#define K68_RUN(ctx) k68_check_run(ctx)
#else
#define K68_RUN(ctx) k68_exec(ctx)
#endif

#ifdef K68POLL
/* -DK68POLL: FAME's "not idle" bne/beq -10 (0x73f6/0x77f6, idle detection's patch) closing
   "L: btst #3,(VDP control).l; bxx L" (k68 sends it here only then). The status byte the btst reads
   cannot change inside a 68k slice while no command is pending and the FIFO is empty (VB is set by
   the frame loop; VideoSr's FIFO sync is then a no-op and the read has no side effect), so the
   remaining iterations are charged arithmetically and the slice ends where the loop would have
   left it (after the btst or after the branch), with the flags it would have. */
extern int vdp_poll_quiet(void), vdp_poll_status(void);
unsigned k68_pollskips;
static void k68_pollbcc(M68K_CONTEXT *ctx)
{
	int beq = Opcode & 0x400;
	u16 *t;
	u32 adr, nz;
	s32 c, k;
	if (beq ? flag_NotZ : !flag_NotZ) RET(8)
	t = PC - 5;
	PC = t;
	ctx->io_cycle_counter -= 10;
	c = ctx->io_cycle_counter;
	if (c <= 0 || t[0] != 0x0839 || (t[1] & 7) != 3) return;
	adr = ((u32)t[2] << 16 | t[3]) & 0xffffff;
	if ((adr & 0xe700fd) != 0xc00005 || !vdp_poll_quiet()) return;
	nz = vdp_poll_status() & 8;
	if (beq ? nz : !nz) return;          /* the next btst ends the loop: run it */
	k = (c - 1) / 30;                    /* whole iterations (btst 20 + taken branch 10) */
	c -= k * 30 + 20;
	flag_NotZ = nz;
	if (c <= 0) PC = t + 4;              /* the slice ends after a btst */
	else c -= 10;                        /* ... or after the branch, at L */
	ctx->io_cycle_counter = c;
	k68_pollskips++;
}
#endif
void *k68_detfn;   /* -DK68IDLE2: FAME's idle_detector_bcc8 (k68 falls back only while an op's entry is it) */
#ifdef K68IDLEJ
extern void k68_ij_stub(void);
extern void *k68_ij_orig;
#endif
#ifdef K68DBLOOP
extern void k68_dl_stub(void);
extern void *k68_dl_orig;
#endif
int fm68k_idle_install(void) { int r; k68_idle_on = 1; r = fame_idle_install(); k68_detfn = (void *)JumpTable[0x66fa];
#ifdef K68IDLEJ
	if (k68_table[0x46f8 >> 3] != (void *)k68_ij_stub) { k68_ij_orig = k68_table[0x46f8 >> 3]; k68_table[0x46f8 >> 3] = (void *)k68_ij_stub; }
#endif
#ifdef K68IDLEG
	{ extern void k68_ig_stub(void); extern void *k68_ig_orig;
	  if (k68_table[0x4e70 >> 3] != (void *)k68_ig_stub) { k68_ig_orig = k68_table[0x4e70 >> 3]; k68_table[0x4e70 >> 3] = (void *)k68_ig_stub; } }
#endif
#ifdef K68DBLOOP
	if (k68_table[0x51c8 >> 3] != (void *)k68_dl_stub) { k68_dl_orig = k68_table[0x51c8 >> 3]; k68_table[0x51c8 >> 3] = (void *)k68_dl_stub; }
#endif
#ifdef K68POLL
	JumpTable[0x73f6] = JumpTable[0x77f6] = k68_pollbcc;
#endif
	return r; }
int fm68k_idle_remove(void) { k68_idle_on = 0; return fame_idle_remove(); }

/* exception entry for the asm core (FAME's execute_exception + SET_PC);
 * flags must be in ctx (FAME form). Returns nothing: ctx->PC/BasePC set. */
void k68_exception(M68K_CONTEXT *ctx, int vect, u32 oldpc)
{
	u32 newpc = execute_exception(ctx, vect, oldpc, GET_SR);
	SET_PC(newpc);
}

/* group 0 (address error on a branch to an odd address): CHECK_BRANCH_EXCEPTION */
void k68_addr_error(M68K_CONTEXT *ctx)
{
	u32 new_PC, pr_PC = GET_PC;
	ctx->execinfo |= FM68K_EMULATE_GROUP_0;
	new_PC = execute_exception_group_0(ctx, M68K_ADDRESS_ERROR_EX, 0, pr_PC, 0x12);
	SET_PC(new_PC);
}

#ifdef K68IDLEJ
#define XTEXT __attribute__((noinline, section(".xtext.k68")))
/* -DK68IDLEJ: the idle loop "L: move.w #imm,sr; jsr S.l; bra.b L" with "S: move.w a.w,d0;
   beq.w R ... R: rts" (Gunstar Heroes' main loop waiting for its VBlank flag, 42% of its
   68k instructions). While the RAM word a is 0, no interrupt above imm's mask is pending and
   SR's upper byte is imm's, nothing the loop reads can change inside a 68k slice (interrupts
   and the other CPUs only act between slices), so the slice is finished arithmetically:
   FAME's cycles per instruction (16 20 12 10 16 10), the slice ends after the first
   instruction that takes the counter to <= 0, with the registers, flags, PC and the stacked
   return address that instruction sequence leaves. */
static const u8 ij_cyc[6] = { 16, 20, 12, 10, 16, 10 };
static u32 ij_L, ij_S, ij_R, ij_adr, ij_pos[6];
static u16 ij_imm;
unsigned k68_ijskips;
static XTEXT int ij_find(M68K_CONTEXT *ctx, u32 pc)
{
	u16 *t;
	u32 S, R, sPC = (u32)(uptr)PC, sBase = BasePC;
	int i;
	if (ij_L) for (i = 0; i < 6; i++) if (pc == ij_pos[i]) return i;
	if (PC[0] != 0x46fc || PC[2] != 0x4eb9 || PC[5] != 0x60f4) return -1;
	S = (u32)PC[3] << 16 | PC[4];
	if (S >= 0x400000 || (S & 1)) return -1;
	SET_PC(S) t = PC;
	R = S + 6 + (s16)t[3];
	if (t[0] != 0x3038 || t[2] != 0x6700 || (R & 1) || R >= 0x400000) { PC = (u16 *)(uptr)sPC; BasePC = sBase; return -1; }
	SET_PC(R)
	i = PC[0];
	PC = (u16 *)(uptr)sPC; BasePC = sBase;
	if (i != 0x4e75) return -1;
	ij_adr = (u32)(s32)(s16)t[1] & 0xffffff;
	if (ij_adr < 0xe00000) { ij_L = 0; return -1; }   /* only a RAM word has no read side effects */
	ij_L = pc; ij_S = S; ij_R = R; ij_imm = PC[1];
	ij_pos[0] = pc; ij_pos[1] = pc + 4; ij_pos[2] = S; ij_pos[3] = S + 4; ij_pos[4] = R; ij_pos[5] = pc + 10;
	return 0;
}
static XTEXT int ij_skip(M68K_CONTEXT *ctx)
{
	s32 c = ctx->io_cycle_counter;
	u32 pc = GET_PC, osp;
	int i, last = -1, did_sr = 0, did_jsr = 0, did_mv = 0, dsp = 0;
	u32 slot;
	if (c <= 0 || pc >= 0x400000) return 0;
	i = ij_find(ctx, pc);
	if (i < 0) return 0;
	if (((GET_SR ^ ij_imm) & 0xff00) || interrupt_chk__(ctx) > 0) return 0;
	if (i == 3 && flag_NotZ) return 0;   /* the beq tests the previous slice's read */
	osp = AREG(7) + (i >= 2 && i <= 4 ? 4 : 0);   /* SP outside the subroutine */
	slot = (osp - 4) & 0xffffff;          /* the stacked return address must not overlap the word */
	if (((slot - ij_adr) & 0xffffff) < 2 || ((ij_adr - slot) & 0xffffff) < 4) return 0;
	if ((u16)ctx->read_word(ij_adr) != 0) return 0;
	if (i >= 2 && i <= 4 && ctx->read_long(osp - 4) != ij_L + 10) return 0;   /* S called from elsewhere */
	for (;;) {
		if (i == 0 && c > 84) {   /* whole iterations (84 cycles; no break inside one) */
			did_sr = did_jsr = did_mv = 1; last = 1;
			while (c > 84 * 256) c -= 84 * 256;
			while (c > 84 * 16) c -= 84 * 16;
			while (c > 84) c -= 84;
		}
		c -= ij_cyc[i];
		switch (i) {
		case 0: last = 0; did_sr = 1; break;
		case 1: dsp -= 4; did_jsr = 1; break;
		case 2: last = 1; did_mv = 1; break;
		case 4: dsp += 4; break;
		}
		i = i == 5 ? 0 : i + 1;
		if (c <= 0) break;
	}
	if (did_jsr) ctx->write_long(osp - 4, ij_L + 10);
	AREG(7) += dsp;
	if (did_sr) {   /* SR as k68's move to SR leaves it (FAME's SET_SR keeps stray high bits) */
		SET_SR(ij_imm)
		flag_C = (ij_imm & 1) << 8; flag_V = (ij_imm & 2) << 6; flag_NotZ = !(ij_imm & 4);
		flag_N = (ij_imm & 8) << 4; flag_X = (ij_imm & 0x10) << 4;
	}
	if (last == 1) { flag_C = 0; flag_V = 0; flag_NotZ = 0; flag_N = 0; }
	if (did_mv) DREGu16(0) = 0;
	SET_PC(ij_pos[i])
	ctx->io_cycle_counter = c;
	k68_ijskips++;
	return 1;
}
/* k68ij.S's handler for the 0x46f8 group (move to SR) calls this at a move.w #imm,sr with
   ctx->PC at the opcode: 1 = the slice was finished in the idle loop (ctx updated), 0 = run it */
XTEXT int k68_ijtry(M68K_CONTEXT *ctx) { return ij_skip(ctx); }
extern void *k68_table[8192];
extern void k68_ij_stub(void);
void *k68_ij_orig;
#endif
#ifdef K68DBLOOP
/* -DK68DBLOOP: two RAM loops closed by dbra, run here instead of one instruction at a time
   (k68dl.S's handler for the dbra group checks the exact opcodes first):
   A "L: move.b (a1,d2.w),d0; move.b d0,(a1)+; dbra d3,L" (Sonic's scroll table copy),
   B "L: move.w d0,(a1); addq.w #4,a1; dbra d1,L" (Vectorman's scroll table fill).
   Called at the dbra with ctx->PC at it. FAME's cycles (dbra 10 taken / 14 out, move.b
   (d8,An,Xn) 14, move.b Dn,(An)+ 8, move.w Dn,(An) 8, addq.w #,An 8); the slice ends after
   the first instruction that takes the counter to <= 0, or the loop ends, or an access would
   leave 68k RAM (then k68 runs that instruction itself). Same registers, flags, RAM. */
#ifndef XTEXT
#define XTEXT __attribute__((noinline, section(".xtext.k68")))
#endif
extern struct { unsigned char ram[0x10000]; } PicoMem;   /* pico_int.h's struct PicoMem starts with ram */
XTEXT int k68_dbtry(M68K_CONTEXT *ctx)
{
	u16 *pc = PC;
	u32 op = pc[0], a;
	s32 c = ctx->io_cycle_counter;
	u8 *ram = PicoMem.ram;
	int pos;
	if (op == 0x51cb) {                 /* A */
		a = (AREG(1) + (s32)(s16)DREGu16(2)) & 0xffffff;
		if (a < 0xe00000 || (AREG(1) & 0xffffff) < 0xe00000) return 0;
		for (;;) {
			u32 r = (DREGu16(3) - 1) & 0xffff, b;
			DREGu16(3) = r;
			if (r != 0xffff) { c -= 10; pos = 0; } else { c -= 14; pos = 3; break; }
			if (c <= 0) break;
			a = (AREG(1) + (s32)(s16)DREGu16(2)) & 0xffffff;
			if (a < 0xe00000) break;
			b = ram[(a & 0xffff) ^ 1];
			DREGu8(0) = b;
			flag_C = 0; flag_V = 0; flag_NotZ = b; flag_N = b;
			c -= 14;
			pos = 1;
			if (c <= 0) break;
			a = AREG(1) & 0xffffff;
			if (a < 0xe00000) break;
			ram[(a & 0xffff) ^ 1] = b;
			AREG(1) += 1;
			c -= 8;
			pos = 2;
			if (c <= 0) break;
		}
		PC = pos == 0 ? pc - 3 : pos == 1 ? pc - 1 : pos == 2 ? pc : pc + 2;
	} else if (op == 0x51c9) {          /* B */
		a = AREG(1) & 0xffffff;
		if (a < 0xe00000 || (a & 1)) return 0;
		for (;;) {
			u32 r = (DREGu16(1) - 1) & 0xffff, w;
			DREGu16(1) = r;
			if (r != 0xffff) { c -= 10; pos = 0; } else { c -= 14; pos = 3; break; }
			if (c <= 0) break;
			a = AREG(1) & 0xffffff;
			if (a < 0xe00000 || (a & 1)) break;
			w = DREGu16(0);
			*(u16 *)(ram + (a & 0xfffe)) = w;
			flag_C = 0; flag_V = 0; flag_NotZ = w; flag_N = w >> 8;
			c -= 8;
			pos = 1;
			if (c <= 0) break;
			AREG(1) += 4;
			c -= 8;
			pos = 2;
			if (c <= 0) break;
		}
		PC = pos == 0 ? pc - 2 : pos == 1 ? pc - 1 : pos == 2 ? pc : pc + 2;
	} else
		return 0;
	ctx->not_polling = 1;
	ctx->io_cycle_counter = c;
	return 1;
}
extern void k68_dl_stub(void);
void *k68_dl_orig;
#endif
#ifdef K68IDLEG
void *k68_ig_orig;
#ifndef XTEXT
#define XTEXT __attribute__((noinline, section(".xtext.k68")))
#endif
#include "idleg.c"
#endif
/* FAME's fm68k_emulate (NO_GOTOS form) with NEXT -> K68_RUN */
int fm68k_emulate(M68K_CONTEXT *ctx, int cycles, fm68k_call_reason reason)
{
	flag_I = M68K_PPL;
	if (ctx->execinfo & FM68K_HALTED) {
		if (interrupt_chk__(ctx) <= 0)
			return cycles;
		ctx->execinfo &= ~FM68K_HALTED;
	}
	ctx->execinfo |= M68K_RUNNING;
	SET_SR(ctx->sr)
	SET_PC(ctx->pc)
	ctx->io_cycle_counter = cycles;
	cycles_needed = 0;
	if (!(ctx->execinfo & FM68K_EMULATE_TRACE)) {
		s32 line = interrupt_chk__(ctx);
		if (line > 0) {
			if (ctx->iack_handler != NULL)
				ctx->iack_handler(line);
			else
				ctx->interrupts[0] = 0;
			SET_PC(execute_exception(ctx, line + 0x18, GET_PC, GET_SR));
			flag_I = (u32)line;
			if (ctx->io_cycle_counter <= 0) goto famec_End;
		} else if (flag_T) {
			ctx->execinfo |= FM68K_EMULATE_TRACE;
			cycles_needed = ctx->io_cycle_counter;
			ctx->io_cycle_counter = 0;
		}
	}
famec_Exec:
#ifdef K68IDLEG
	k68_run_ig(ctx);   /* (a call of the same size as k68_exec's: the code layout stays) */
#else
	K68_RUN(ctx);
#endif
	if (ctx->execinfo & FM68K_EMULATE_TRACE) {
		ctx->io_cycle_counter += cycles_needed;
		cycles_needed = 0;
		ctx->execinfo &= ~FM68K_EMULATE_TRACE;
		ctx->execinfo |= FM68K_DO_TRACE;
		SET_PC(execute_exception(ctx, M68K_TRACE_EX, GET_PC, GET_SR));
		if (ctx->io_cycle_counter > 0)
			goto famec_Exec;
	} else if (cycles_needed != 0) {
		u32 line;
		ctx->io_cycle_counter += cycles_needed;
		cycles_needed = 0;
		line = interrupt_chk__(ctx);
		if (line > 0) {
			if (ctx->iack_handler != NULL)
				ctx->iack_handler(line);
			else
				ctx->interrupts[0] = 0;
			SET_PC(execute_exception(ctx, line + 0x18, GET_PC, GET_SR));
			flag_I = (u32)line;
		}
		if (!(flag_T))
		if (ctx->io_cycle_counter > 0)
			goto famec_Exec;
	}
famec_End:
	ctx->sr = GET_SR;
	ctx->pc = GET_PC;
	ctx->execinfo &= ~M68K_RUNNING;
	return cycles - ctx->io_cycle_counter;
}

#ifdef K68CHECK
#undef PC
#undef BasePC
#undef flag_C
#undef flag_V
#undef flag_NotZ
#undef flag_N
#undef flag_X
#undef flag_T
#undef flag_S
#undef flag_I
#undef cycles_needed
#undef Opcode
/* Lockstep checker: k68 runs one instruction at a time (k68i.S built with
 * -DK68CHECK exits after each), logging every memory access (value and what
 * the handler did to the cycle counter / irq level); FAME then runs the same
 * instruction from a copy of the context with the log replayed, and the two
 * results are compared. */
enum { R8, R16, R32, W8, W16, W32 };
static const char *kname[] = { "r8", "r16", "r32", "w8", "w16", "w32" };
struct ent { int k; u32 a, v; int dcnt; int irq; };
static struct ent elog[64];
static int nlog, plog, chk_bad;
static M68K_CONTEXT *refctx;
static unsigned chk_count;

static u32 chk_do(int k, u32 a, u32 v)
{
	M68K_CONTEXT *c = &PicoCpuFM68k;
	int before = c->io_cycle_counter;
	switch (k) {
	case R8: v = m68k_read8(a) & 0xff; break;
	case R16: v = m68k_read16(a) & 0xffff; break;
	case R32: v = m68k_read32(a); break;
	case W8: m68k_write8(a, v); break;
	case W16: m68k_write16(a, v); break;
	case W32: m68k_write32(a, v); break;
	}
	if (nlog < 64) {
		struct ent *e = &elog[nlog++];
		e->k = k; e->a = a; e->v = v; e->dcnt = c->io_cycle_counter - before; e->irq = c->interrupts[0];
	}
	return v;
}
u32 k68chk_r8(u32 a) { return chk_do(R8, a, 0); }
u32 k68chk_r16(u32 a) { return chk_do(R16, a, 0); }
u32 k68chk_r32(u32 a) { return chk_do(R32, a, 0); }
void k68chk_w8(u32 a, u32 d) { chk_do(W8, a, d & 0xff); }
void k68chk_w16(u32 a, u32 d) { chk_do(W16, a, d & 0xffff); }
void k68chk_w32(u32 a, u32 d) { chk_do(W32, a, d); }

static u32 rp(int k, u32 a, u32 v)
{
	struct ent *e;
	if (plog >= nlog) { if (!chk_bad) printf("K68CHECK: FAME does an extra %s [%06x]\n", kname[k], a); chk_bad = 1; return 0; }
	e = &elog[plog++];
	if (e->k != k || (e->a & 0xffffff) != (a & 0xffffff) || (k >= W8 && e->v != v)) {
		if (!chk_bad) printf("K68CHECK: access %d: k68 %s [%06x] %x, FAME %s [%06x] %x\n",
			plog - 1, kname[e->k], e->a, e->v, kname[k], a, v);
		chk_bad = 1;
	}
	refctx->io_cycle_counter += e->dcnt;
	refctx->interrupts[0] = e->irq;
	return e->v;
}
static u32 rp_r8(u32 a) { return rp(R8, a, 0); }
static u32 rp_r16(u32 a) { return rp(R16, a, 0); }
static u32 rp_r32(u32 a) { return rp(R32, a, 0); }
static void rp_w8(u32 a, u8 d) { rp(W8, a, d); }
static void rp_w16(u32 a, u16 d) { rp(W16, a, d); }
static void rp_w32(u32 a, u32 d) { rp(W32, a, d); }

extern int k68_is_fallback(unsigned op);   /* k68i.S table entry is k68_fallback */

static void chk_dump(const char *w, M68K_CONTEXT *c)
{
	int i;
	printf(" %s pc %06x cnt %d sr %x C%d V%d Z%d N%d X%d S%x I%d T%x ex %x cn %d\n  ", w,
		(u32)((uptr)c->PC - c->BasePC), c->io_cycle_counter, c->sr,
		!!(c->flag_C & 0x100), !!(c->flag_V & 0x80), !c->flag_NotZ, !!(c->flag_N & 0x80),
		!!(c->flag_X & 0x100), c->flag_S, c->flag_I, c->flag_T, c->execinfo, c->cycles_needed);
	for (i = 0; i < 16; i++) printf("%08x%c", c->dreg[i].D, i == 7 ? '|' : ' ');
	printf("usp %08x\n", c->asp);
}

static void k68_check_run(M68K_CONTEXT *ctx)
{
	static M68K_CONTEXT ref, before;
	do {
		u16 op = *ctx->PC;
		unsigned fb0 = k68_fallbacks;
		before = *ctx;
		nlog = 0;
		k68_exec(ctx);
		chk_count++;
		if (!(chk_count & 0xfffff)) printf("K68CHECK: %u instructions, %u fallbacks\n", chk_count, k68_fallbacks);
		if (k68_fallbacks != fb0) continue;
		ref = before;
		ref.read_byte = (void *)rp_r8; ref.read_word = (void *)rp_r16; ref.read_long = (void *)rp_r32;
		ref.write_byte = (void *)rp_w8; ref.write_word = (void *)rp_w16; ref.write_long = (void *)rp_w32;
		refctx = &ref; plog = 0; chk_bad = 0;
		{
			M68K_CONTEXT *c = &ref;
			c->Opcode = *c->PC++;
			JumpTable[c->Opcode](c);
		}
		if (plog != nlog && !chk_bad) { printf("K68CHECK: k68 did %d accesses, FAME %d\n", nlog, plog); chk_bad = 1; }
#define NZ(c) (!(c)->flag_NotZ)
		if (memcmp(ref.dreg, ctx->dreg, 16 * 4) || ref.asp != ctx->asp
		    || (uptr)ref.PC - ref.BasePC != (uptr)ctx->PC - ctx->BasePC
		    || ref.io_cycle_counter != ctx->io_cycle_counter
		    || !(ref.flag_C & 0x100) != !(ctx->flag_C & 0x100) || !(ref.flag_V & 0x80) != !(ctx->flag_V & 0x80)
		    || NZ(&ref) != NZ(ctx) || !(ref.flag_N & 0x80) != !(ctx->flag_N & 0x80)
		    || !(ref.flag_X & 0x100) != !(ctx->flag_X & 0x100)
		    || ref.flag_S != ctx->flag_S || ref.flag_I != ctx->flag_I || ref.flag_T != ctx->flag_T
		    || ref.execinfo != ctx->execinfo || ref.cycles_needed != ctx->cycles_needed
		    || ref.interrupts[0] != ctx->interrupts[0])
			chk_bad = 1;
		if (chk_bad) {
			printf("K68CHECK: mismatch after op %04x (#%u)\n", op, chk_count);
			chk_dump("before", &before); chk_dump("k68   ", ctx); chk_dump("FAME  ", &ref);
			exit(1);
		}
	} while (ctx->io_cycle_counter > 0);
}
#endif
