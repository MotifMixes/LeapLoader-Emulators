/* snx.c (-DSNX, included by sn_patched.c in place of PicoDrive's
   SN76496Update): the same computation with the generator state in locals
   (registers) during the loop; mono only (stereo goes to the original). */
static void SN76496Update_orig(short *buffer, int length, int stereo);

#define SNX_TONE(i, C, O, V) do { \
	if (O) V += C; \
	C -= STEP; \
	if (C < -2*per##i || vol##i == 0) { \
		V += STEP/2; \
		C = O = 0; \
	} \
	while (C < 0) { \
		C += per##i; \
		if (C >= 0) { \
			O ^= 1; \
			if (O) V += per##i; \
			break; \
		} \
		C += per##i; \
		V += per##i; \
	} \
	if (O) V -= C; \
} while (0)

void SN76496Update(short *buffer, int length, int stereo)
{
	struct SN76496 *R = &ono_sn;
	int c0, c1, c2, c3, o0, o1, o2, o3;
	int per0, per1, per2, per3, vol0, vol1, vol2, vol3;
	unsigned int rng;
	int fb;

	if (stereo && R->Panning != 0xff) {
		SN76496Update_orig(buffer, length, stereo);
		return;
	}
	c0 = R->Count[0]; c1 = R->Count[1]; c2 = R->Count[2]; c3 = R->Count[3];
	o0 = R->Output[0]; o1 = R->Output[1]; o2 = R->Output[2]; o3 = R->Output[3];
	per0 = R->Period[0]; per1 = R->Period[1]; per2 = R->Period[2]; per3 = R->Period[3];
	vol0 = R->Volume[0]; vol1 = R->Volume[1]; vol2 = R->Volume[2]; vol3 = R->Volume[3];
	rng = R->RNG; fb = R->NoiseFB;
#ifdef SNQ
	/* -DSNQ: a tone channel with volume 0 ends every sample with C = O = 0 and
	   adds 0: done once here, the loop skips it (same state, same samples). */
	if (length > 0) {
		if (!vol0) c0 = o0 = 0;
		if (!vol1) c1 = o1 = 0;
		if (!vol2) c2 = o2 = 0;
	}
#define SNQ_IF(x) if (x)
#else
#define SNQ_IF(x)
#endif
#if defined SNASM && defined SNQ
	/* -DSNASM: the mono loop in ARC asm (sn.S), same arithmetic */
	if (!stereo) {
		extern void snx_loop(int *st, short *buffer, int length);
		int st[18] = { c0, c1, c2, c3, o0, o1, o2, o3, per0, per1, per2, per3, vol0, vol1, vol2, vol3, (int)rng, fb };
		snx_loop(st, buffer, length);
		c0 = st[0]; c1 = st[1]; c2 = st[2]; c3 = st[3];
		o0 = st[4]; o1 = st[5]; o2 = st[6]; o3 = st[7]; rng = st[16];
		length = 0;
	}
#endif

	while (length > 0)
	{
		int v0 = 0, v1 = 0, v2 = 0, v3 = 0;
		int left;
		unsigned int out;

		SNQ_IF(vol0) SNX_TONE(0, c0, o0, v0);
		SNQ_IF(vol1) SNX_TONE(1, c1, o1, v1);
		SNQ_IF(vol2) SNX_TONE(2, c2, o2, v2);

		left = STEP;
		if (o3) v3 += c3;
		do
		{
			int nextevent;
			if (c3 < left) nextevent = c3;
			else nextevent = left;
			c3 -= nextevent;
			if (c3 <= 0)
			{
				o3 = rng & 1;
				rng >>= 1;
				if (o3)
				{
					rng ^= fb;
					v3 += per3;
				}
				c3 += per3;
			}
			left -= nextevent;
		} while (left > 0 && vol3);
		if (o3) v3 -= c3;

		length--;
#ifdef SNQ
		out = 0;
		if (vol0) out += v0 * vol0;
		if (vol1) out += v1 * vol1;
		if (vol2) out += v2 * vol2;
		if (vol3) out += v3 * vol3;
#else
		out = v0 * vol0 + v1 * vol1 + v2 * vol2 + v3 * vol3;
#endif
		if (out > MAX_OUTPUT * STEP) out = MAX_OUTPUT * STEP;
		out /= STEP;
		*buffer++ += out;
		if (stereo) *buffer++ += out;
	}
	R->Count[0] = c0; R->Count[1] = c1; R->Count[2] = c2; R->Count[3] = c3;
	R->Output[0] = o0; R->Output[1] = o1; R->Output[2] = o2; R->Output[3] = o3;
	R->RNG = rng;
}
#define SN76496Update SN76496Update_orig
