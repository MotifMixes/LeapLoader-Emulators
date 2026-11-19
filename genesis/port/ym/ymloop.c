/* ymloop.c (-DYMLOOP, included by ym_patched.c in place of PicoDrive's
   chan_render_loop): the same per-sample computation with the state in
   locals (registers) instead of the context, the envelope step out of line,
   vol_out read only after an envelope step (nothing else changes it inside
   the loop) and the small sin/tl tables (see mkym.py). Same results.
   -DYMASM: mono channels with output enabled and no DAC bit drop go to
   ymx_loop (ym/ymx.S); its C helpers are here (E values, EG masks, LFO and
   envelope steps). -DYMCHECK: each ymx_loop call is re-run by the C loop on
   a copy and compared (ymchk.sh prints the first differences). */

static __attribute__((noinline)) void ymx_eg_step(chan_rend_context *ct)
{
	/* the body of PicoDrive's "while (eg_timer >= 1<<EG_SH)" loop, once */
	if (ct->pack & 2)
		update_ssg_eg_channel(ct);
	if (ct->algo & 0x30)
		ct->algo -= 0x10;
	if (!(ct->algo & 0x30)) {
		ct->algo |= 0x30;
		ct->eg_cnt++;
		if (ct->eg_cnt >= 4096) ct->eg_cnt = 1;
		update_eg_phase_channel(ct);
	}
}

static INLINE int ymx_op(UINT32 sin, unsigned int env)
{
	/* op_calc after the phase sum: sin = 10-bit index (more bits above) */
	unsigned p;
	int ret;
	if (sin & 0x100) sin ^= 0xff;
	p = ((env & ~1) << 2) + ym_sin_tab[sin & 0xff];
	ret = p >= 13*TL_RES_LEN ? 0 : ym_tl_tab2[p];
	return (sin & 0x200) ? -ret : ret;
}
#define OPC(ph, env, pm)  ymx_op(((ph) >> 16) + ((pm) >> 1), env)

#ifdef YMASM
#include <stddef.h>
/* ymx.S tables: ymx_sin[i] = ym_sin_tab[] after the quarter-wave fold of
   i (0..511); ymx_tl = ym_tl_tab2 + one 0 at 3328 */
UINT16 ymx_sin[512], ymx_tl[13*TL_RES_LEN + 2];
/* per slot of the channel in ymx_loop: update_eg_phase acts only when
   (eg_cnt & mask) == 0; 0xfff for EG_OFF (eg_cnt is 1..4095) */
UINT32 ymx_mask[4];
void ymx_loop(chan_rend_context *ct, s32 *buffer, int length);
typedef char ymx_chk[(offsetof(chan_rend_context, pad) == 0x0c && offsetof(chan_rend_context, incr1) == 0x20
	&& offsetof(chan_rend_context, eg_timer) == 0x44 && offsetof(chan_rend_context, op1_out) == 0x54) ? 1 : -1];

static void ymx_setE(chan_rend_context *ct)
{
	/* E1..E4 = slots SLOT1..SLOT4 (SLOT[0], [2], [1], [3]) with the LFO's AM */
	static const unsigned char sl[4] = { SLOT1, SLOT2, SLOT3, SLOT4 };
	FM_SLOT *S = ct->CH->SLOT;
	UINT16 *E = &ct->vol_out1;
	UINT32 pack = ct->pack;
	unsigned add = (pack & 8) ? pack >> (((pack&0xc0)>>6)+24) : 0;
	int k;
	for (k = 0; k < 4; k++) {
		FM_SLOT *s = &S[sl[k]];
		unsigned e = s->vol_out;
		if (pack & (1 << (sl[k] + 8))) e += add;
		e = (e & ~1) << 2;
		E[k] = e > 13*TL_RES_LEN ? 13*TL_RES_LEN : e;
		ymx_mask[k] = s->state == EG_OFF ? 0xfff : (1u << (s->eg_pack[s->state - 1] >> 24)) - 1;
	}
}

#ifdef YMSETE
/* -DYMSETE: an EG step (no SSG-EG here) updates and recomputes only the slots
   whose update_eg_phase acts ((eg_cnt & mask) == 0; never EG_OFF: its mask
   is 0xfff and eg_cnt 1..4095); an LFO step recomputes E only when the AM
   offset changed (nothing else in E or the masks depends on the LFO). */
void ymx_eg_upd(chan_rend_context *ct)
{
	static const unsigned char sl[4] = { SLOT1, SLOT2, SLOT3, SLOT4 };
	FM_SLOT *S = ct->CH->SLOT;
	UINT16 *E = &ct->vol_out1;
	UINT32 pack = ct->pack, eg_cnt = ct->eg_cnt;
	unsigned add = (pack & 8) ? pack >> (((pack&0xc0)>>6)+24) : 0;
	int k;
	for (k = 0; k < 4; k++) {
		FM_SLOT *s;
		unsigned e;
		if (eg_cnt & ymx_mask[k]) continue;
		s = &S[sl[k]];
		update_eg_phase(s, eg_cnt, 0);
		e = s->vol_out;
		if (pack & (1 << (sl[k] + 8))) e += add;
		e = (e & ~1) << 2;
		E[k] = e > 13*TL_RES_LEN ? 13*TL_RES_LEN : e;
		ymx_mask[k] = s->state == EG_OFF ? 0xfff : (1u << (s->eg_pack[s->state - 1] >> 24)) - 1;
	}
}

void ymx_lfo(chan_rend_context *ct)
{
	UINT32 n = ct->lfo_cnt, p0 = ct->pack, p1;
	p1 = (p0&0xffff) | (advance_lfo(p0 >> 16, n - ct->lfo_inc, n) << 16);
	ct->pack = p1;
	if ((p1 & 8) && (p1 & 0xf00) && ((p0 ^ p1) >> 24))
		ymx_setE(ct);
}
#else
void ymx_eg_upd(chan_rend_context *ct)
{
	update_eg_phase_channel(ct);
	ymx_setE(ct);
}

void ymx_lfo(chan_rend_context *ct)
{
	UINT32 n = ct->lfo_cnt;
	ct->pack = (ct->pack&0xffff) | (advance_lfo(ct->pack >> 16, n - ct->lfo_inc, n) << 16);
	ymx_setE(ct);
}
#endif

void ymx_eg_run(chan_rend_context *ct)
{
	do {
		ct->eg_timer -= 1<<EG_SH;
		ymx_eg_step(ct);
	} while (ct->eg_timer >= 1<<EG_SH);
	ymx_setE(ct);
}
#endif

static void ymx_cloop(chan_rend_context *ct, s32 *buffer, int length);

static void chan_render_loop(chan_rend_context *ct, s32 *buffer, int length)
{
#ifdef YMSTAT  /* calls and samples per channel render, printed every 1000 calls */
	{ static unsigned n, s, h[4]; n++; s += length; h[length < 2 ? 0 : length < 8 ? 1 : length < 64 ? 2 : 3]++;
	  if (n % 1000 == 0) printf("YMSTAT calls %u samples %u | len 1: %u, 2-7: %u, 8-63: %u, 64+: %u\n", n, s, h[0], h[1], h[2], h[3]); }
#endif
#ifdef YMASM
	if (!(ct->pack & 0x5) && !(ct->algo & 0x80)) { /* mono, output enabled */
#ifdef YMCHECK  /* run the C loop on a copy too and compare (first difference printed) */
		static s32 b0[2048], ba[2048]; static int bad;
		chan_rend_context c0 = *ct, ca; FM_CH ch0 = *ct->CH, cha; int i;
		memcpy(b0, buffer, length * 4);
#endif
		if (!ymx_tl[1]) {
			int i;
			for (i = 0; i < 512; i++) ymx_sin[i] = ym_sin_tab[((i & 0x100) ? i ^ 0xff : i) & 0xff];
			for (i = 0; i < 13*TL_RES_LEN; i++) ymx_tl[i] = ym_tl_tab2[i];
		}
		ymx_setE(ct);
		ymx_loop(ct, buffer, length);
#ifdef YMCHECK
		ca = *ct; cha = *ct->CH; memcpy(ba, buffer, length * 4);
		*ct = c0; *ct->CH = ch0; memcpy(buffer, b0, length * 4);
		ymx_cloop(ct, buffer, length);
		if (bad < 5) {
			for (i = 0; i < length && ba[i] == buffer[i]; i++) ;
			if (i < length || memcmp(&cha, ct->CH, sizeof cha) || ca.phase1 != ct->phase1 || ca.phase2 != ct->phase2
			    || ca.phase3 != ct->phase3 || ca.phase4 != ct->phase4 || ca.mem != ct->mem || ca.op1_out != ct->op1_out
			    || ca.eg_timer != ct->eg_timer || ca.eg_cnt != ct->eg_cnt || ca.algo != ct->algo || ca.pack != ct->pack
			    || ca.lfo_cnt != ct->lfo_cnt) {
				bad++;
				printf("YMCHECK len %d algo %x/%x pack %x/%x smp %d: %d vs %d | ph %x/%x %x/%x mem %d/%d op1 %x/%x egt %x/%x egc %d/%d ch %d\n",
				  length, ca.algo, ct->algo, ca.pack, ct->pack, i, i < length ? ba[i] : 0, i < length ? buffer[i] : 0,
				  ca.phase1, ct->phase1, ca.phase2, ct->phase2, ca.mem, ct->mem, ca.op1_out, ct->op1_out,
				  ca.eg_timer, ct->eg_timer, ca.eg_cnt, ct->eg_cnt, memcmp(&cha, ct->CH, sizeof cha) != 0);
			}
		}
#endif
		return;
	}
#endif
	ymx_cloop(ct, buffer, length);
}

static void ymx_cloop(chan_rend_context *ct, s32 *buffer, int length)
{
	UINT32 phase1 = ct->phase1, phase2 = ct->phase2, phase3 = ct->phase3, phase4 = ct->phase4;
	const UINT32 incr1 = ct->incr1, incr2 = ct->incr2, incr3 = ct->incr3, incr4 = ct->incr4;
	UINT32 eg_timer = ct->eg_timer;
	const UINT32 eg_timer_add = ct->eg_timer_add;
	UINT32 pack = ct->pack;
	const int algo = ct->algo & 7, dac = ct->algo & 0x80;
	INT32 mem = ct->mem, op1_out = ct->op1_out;
	UINT32 lfo_cnt = ct->lfo_cnt;
	const UINT32 lfo_inc = ct->lfo_inc;
	FM_SLOT *S = ct->CH->SLOT;
	unsigned int v1 = S[SLOT1].vol_out, v2 = S[SLOT2].vol_out, v3 = S[SLOT3].vol_out, v4 = S[SLOT4].vol_out;
	int had = 0;
	s32 *end = buffer + ((pack & 1) ? 2 * length : length);

	while (buffer < end)
	{
		int smp = 0;
		unsigned int eg_out, eg_out2, eg_out4;
		int m2, c1, c2 = 0;

		eg_timer += eg_timer_add;
		if (eg_timer >= 1<<EG_SH) {
			ct->phase1 = phase1; ct->phase2 = phase2; ct->phase3 = phase3; ct->phase4 = phase4;
			do {
				eg_timer -= 1<<EG_SH;
				ymx_eg_step(ct);
			} while (eg_timer >= 1<<EG_SH);
			phase1 = ct->phase1; phase2 = ct->phase2; phase3 = ct->phase3; phase4 = ct->phase4;
			v1 = S[SLOT1].vol_out; v2 = S[SLOT2].vol_out; v3 = S[SLOT3].vol_out; v4 = S[SLOT4].vol_out;
		}

		if (pack & 4) goto disabled; /* output disabled */

		if (pack & 8) {
			pack = (pack&0xffff) | (advance_lfo(pack >> 16, lfo_cnt, lfo_cnt + lfo_inc) << 16);
			lfo_cnt += lfo_inc;
		}

		eg_out = v1 & 0xffff;
		if ((pack & 8) && (pack&(1<<(SLOT1+8))))
			eg_out += pack >> (((pack&0xc0)>>6)+24);

		if (eg_out < ENV_QUIET) {
			int out = 0;
			if (pack&0xf000) out = ((op1_out + (op1_out<<16))>>16) << ((pack&0xf000)>>12);
			op1_out <<= 16;
			op1_out |= (unsigned short)ymx_op((phase1+out)>>16, eg_out);
		} else
			op1_out <<= 16;

		eg_out = v3; eg_out2 = v2; eg_out4 = v4;
		if (pack & 8) {
			unsigned int add = pack >> (((pack&0xc0)>>6)+24);
			if (pack & (1<<(SLOT3+8))) eg_out  += add;
			if (pack & (1<<(SLOT2+8))) eg_out2 += add;
			if (pack & (1<<(SLOT4+8))) eg_out4 += add;
		}

		switch (algo) {
		case 0:
			m2 = mem; c1 = op1_out>>16;
			mem = eg_out2 < ENV_QUIET ? OPC(phase2, eg_out2, c1) : 0;
			if (eg_out  < ENV_QUIET) c2  = OPC(phase3, eg_out,  m2);
			if (eg_out4 < ENV_QUIET) smp = OPC(phase4, eg_out4, c2);
			break;
		case 1:
			m2 = mem; mem = op1_out>>16;
			if (eg_out2 < ENV_QUIET) mem += OPC(phase2, eg_out2, 0);
			if (eg_out  < ENV_QUIET) c2  = OPC(phase3, eg_out,  m2);
			if (eg_out4 < ENV_QUIET) smp = OPC(phase4, eg_out4, c2);
			break;
		case 2:
			m2 = mem; c2 = op1_out>>16;
			mem = eg_out2 < ENV_QUIET ? OPC(phase2, eg_out2, 0) : 0;
			if (eg_out  < ENV_QUIET) c2 += OPC(phase3, eg_out,  m2);
			if (eg_out4 < ENV_QUIET) smp = OPC(phase4, eg_out4, c2);
			break;
		case 3:
			c2 = mem; c1 = op1_out>>16;
			mem = eg_out2 < ENV_QUIET ? OPC(phase2, eg_out2, c1) : 0;
			if (eg_out  < ENV_QUIET) c2 += OPC(phase3, eg_out,  0);
			if (eg_out4 < ENV_QUIET) smp = OPC(phase4, eg_out4, c2);
			break;
		case 4:
			c1 = op1_out>>16;
			if (eg_out  < ENV_QUIET) c2   = OPC(phase3, eg_out,  0);
			if (eg_out2 < ENV_QUIET) smp  = OPC(phase2, eg_out2, c1);
			if (eg_out4 < ENV_QUIET) smp += OPC(phase4, eg_out4, c2);
			break;
		case 5:
			m2 = mem; mem = c1 = c2 = op1_out>>16;
			if (eg_out  < ENV_QUIET) smp  = OPC(phase3, eg_out,  m2);
			if (eg_out2 < ENV_QUIET) smp += OPC(phase2, eg_out2, c1);
			if (eg_out4 < ENV_QUIET) smp += OPC(phase4, eg_out4, c2);
			break;
		case 6:
			c1 = op1_out>>16;
			if (eg_out  < ENV_QUIET) smp  = OPC(phase3, eg_out,  0);
			if (eg_out2 < ENV_QUIET) smp += OPC(phase2, eg_out2, c1);
			if (eg_out4 < ENV_QUIET) smp += OPC(phase4, eg_out4, 0);
			break;
		default:
			smp = op1_out>>16;
			if (eg_out  < ENV_QUIET) smp += OPC(phase3, eg_out,  0);
			if (eg_out2 < ENV_QUIET) smp += OPC(phase2, eg_out2, 0);
			if (eg_out4 < ENV_QUIET) smp += OPC(phase4, eg_out4, 0);
			break;
		}
disabled:
		phase1 += incr1; phase2 += incr2; phase3 += incr3; phase4 += incr4;

		if (smp) {
			smp = clip(smp);
			if (dac) {
				smp &= ~0x1f;
				smp -= (smp < 0 ? 7:0) << 5;
			}
			if (pack & 1) {
				if (pack & 0x20) buffer[0] += smp;
				if (pack & 0x10) buffer[1] += smp;
			} else
				buffer[0] += smp;
			had = 8;
		}
		buffer += (pack & 1) ? 2 : 1;
	}
	ct->phase1 = phase1; ct->phase2 = phase2; ct->phase3 = phase3; ct->phase4 = phase4;
	ct->eg_timer = eg_timer;
	ct->pack = pack;
	ct->algo |= had;
	ct->mem = mem; ct->op1_out = op1_out;
	ct->lfo_cnt = lfo_cnt;
	ct->vol_out1 = v1; ct->vol_out2 = v2; ct->vol_out3 = v3; ct->vol_out4 = v4;
}
