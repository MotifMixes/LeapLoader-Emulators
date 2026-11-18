/* llr.c (snes-hw, -DLL_NEWR): LeapLoader's renderer for the Leapster 2's 160x160 12 bpp LCD, included at
   the end of gfx.c (it uses gfx.c's LLShow, palette log and old renderer). Replaces the z-buffered
   256-wide drawing + LLScale for transparency off:
   - only the 160 shown lines (LLShow) and the 160 shown columns (x mod 8 in 0,1,3,4,6) are drawn,
     into 8-bit CG-index line buffers (LLRLb, column c at offset LBO + c, scratch margins on both sides);
   - painter's order per line (the old renderer's depths, all distinct: same picture as its z-buffer);
   - BG layers by tile strips: a chunk of up to LLR_CH shown lines, a run = the chunk's lines on one
     map row with one H scroll; per tile the map entry is read once for all of a run's rows
     (asm kernels llrk.S for 2/4 bpp 8x8 tiles without windows, C for the rest);
   - sprites from per-line lists (rebuilt when S9xSetupOBJ runs); lines whose sprites have mixed
     priorities resolve sprite-vs-sprite first (lowest list index wins, as the old z-buffer did);
   - the CG-index line goes straight to 12 bpp (output stage below: st.di into the LCD's VRAM).
   Modes 0, 1, 3 (no direct colour), no mosaic, no hi-res/interlace; anything else falls back to the
   old renderer for that part (LLOldUpdateScreen; its lines are copied to the LCD with st.di too).
   -DLLR_CHECK: both renderers run, the new one into a RAM shadow; every frame is compared (one line
   per frame that differs). -DLLR_CKERN: C kernels only. */

#define LLR_CH 16                  /* shown lines per chunk */
#define LBW 192                    /* line buffer: 16 margin, 160 columns, 16 margin */
#define LBO 16
#define LLR_NOPIX 0x7f
#ifdef LLR_NOINL
#define LLR_NI __attribute__((noinline))
#else
#define LLR_NI
#endif

#if !(defined LLR_DRIP && defined LLR_K2 && !defined LLR_CHECK && !defined LLR_CKERN)
static uint8 LLRLb[LLR_CH][LBW] __attribute__((aligned(32)));
#endif
static uint8 LLRObj[LLR_CH][LBW] __attribute__((aligned(32)));   /* mixed-priority sprite lines */
static uint8 LLRObjLo[LLR_CH], LLRObjHi[LLR_CH];
static uint8 LLRObjP[LLR_CH][LBW] __attribute__((aligned(32)));
static uint8 LLRMask[5][LBW] __attribute__((aligned(32)));        /* window masks per layer (1 = draw) */
static signed char LLRPix[8][8];   /* [fine][tile pixel] -> line buffer offset from 5k, or LLR_NOPIX */
unsigned LLRStat[8];               /* parts new / fallback, ... */
#ifdef LLR_FBSTAT
unsigned LLRFbL[32];
#endif
#if defined LLR_DRIP && defined LLR_K2 && !defined LLR_CHECK && !defined LLR_CKERN
#define LLR_DRIPON
#endif

/* ---- output stage: 160 CG indices -> one LCD row (swappable: st.di into VRAM now) ---- */
extern void llr_out12di(uint8* dst, const uint8* lb, const uint16* pal, const uint32* palb);
extern void llr_out12(uint8* dst, const uint8* lb, const uint16* pal, const uint32* palb);
extern void llr_copy240di(uint8* dst, const uint8* src);
#ifdef LLR_CKERN
static void llr_out12_c(uint8* q, const uint8* s, const uint16* pal, const uint32* pb)
{
   uint32 x;
   for (x = 0; x < 160; x += 2, q += 3)
   {
      uint32 t = pal[s[x]] | pb[s[x + 1]];
      q[0] = t, q[1] = t >> 8, q[2] = t >> 16;
   }
}
#endif
#ifdef LLR_CHECK
static uint8 LLRShadow[240 * 160] __attribute__((aligned(32)));
static uint8 LLRChecked[160];
uint16 LLROldIdx[160][160];
unsigned LLRFrame;
static uint8 LLRNewIdx[160][160];
#define LLR_DST(oy) (LLRShadow + (oy) * 240)
#ifdef LLR_CKERN
#define LLR_OUT(oy, lb) (llr_out12_c(LLR_DST(oy), lb, LLRPalA, LLRPalBv), LLRChecked[oy] = 1)
#else
#define LLR_OUT(oy, lb) (llr_out12(LLR_DST(oy), lb, LLRPalA, LLRPalBv), LLRChecked[oy] = 1, memcpy(LLRNewIdx[oy], lb, 160))
#endif
#else
#define LLR_DST(oy) ((uint8*) 0x03000000 + (oy) * 240)
#if defined LLR_OUT2 && defined LLR_OUTZ && !defined LLR_DRIP
/* -DLLR_OUTZ: the output clears the line buffer behind it; the chunk's memset is skipped */
#define LLR_ZEROED
extern void llr_out12di2z(uint8* dst, const uint8* lb, const uint16* pal, const uint32* palb);
#define LLR_OUT(oy, lb) llr_out12di2z(LLR_DST(oy), lb, LLRPalA, LLRPalBv)
#elif defined LLR_OUT2
extern void llr_out12di2(uint8* dst, const uint8* lb, const uint16* pal, const uint32* palb);
#define LLR_OUT(oy, lb) llr_out12di2(LLR_DST(oy), lb, LLRPalA, LLRPalBv)
#else
#define LLR_OUT(oy, lb) llr_out12di(LLR_DST(oy), lb, LLRPalA, LLRPalBv)
#endif
#endif
/* the old renderer's rows (fallback parts): LLScale writes them to LLRTmp, this copies one */
uint8 LLRTmp[240] __attribute__((aligned(32)));
static void LLRRaw(uint32 oy)
{
#ifndef LLR_CHECK
   llr_copy240di(LLR_DST(oy), LLRTmp);
#endif
}

/* ---- tiles ---- */
uint8 ConvertTile(uint8* pCache, uint32 TileAddr);
extern uint32 LLNib4[], LLNib2[];
#include <stdio.h>
static uint8 llr_convert(uint32 depth, uint32 t)
{
   static const uint8 bs[3] = { 2, 4, 8 };
#ifdef LLR_NIBONLY
   extern uint8 LLConvertNib(uint32 depth, uint32 t);
   if (depth != TILE_8BIT)
      return LLConvertNib(depth, t);
#endif
   BG.BitShift = bs[depth];
   return ConvertTile(IPPU.TileCache[depth] + (t << 6), t << (depth + 4));
}

struct llr_layer
{
   uint32 bpp, depth, tile16, nbase, tmask, palstart, palsh, palmask;
   const uint16* sc[4];
   const uint8* mask;              /* 0: no window */
   uint8* cached;
   const uint32* nib;
   const uint8* cache8;
   uint32 opt;                     /* LLR_MODE2: offset-per-tile on for this BG in this part */
};
struct llr_run
{
   const uint16* b1;
   const uint16* b2;
   const uint16* ent;              /* LLR_MODE2 OPT: the line's 33 map entries (8x8), one per tile column */
   const uint8* optr;              /* their tile rows; 0: all the same (r[0]) */
   uint32 hofs, txi, key, n;
   uint32 nhi;                     /* LLR_K2: high-priority tiles the low-priority pass skipped */
   uint8 j[LLR_CH], r[LLR_CH];
};

#ifdef LLR_MODE2
static int llr_opt_on(uint32 en, uint32 y0, uint32 y1);
#endif
/* asm kernel args (llrk.S, generated by kern/genllr.py; offsets fixed there) */
struct llr_k
{
   const uint16* ma; uint32 na; const uint16* mb; uint32 nbc;          /* 0 4 8 12 */
   const uint32* nib; const uint8* cached; uint32 nbase, tmask;        /* 16 20 24 28 */
   uint32 palstart, palsh, palmask, prio;                               /* 32 36 40 44 */
   uint32 nrows, k;                                                     /* 48 52 */
   uint8* lb[LLR_CH];                                                   /* 56 */
   uint32 ro[LLR_CH], rv[LLR_CH];                                       /* 120: row*4, 184: (7-row)*4 */
   uint32 pad[2];                                                       /* LLR_K2 kernels read one past */
   /* -DLLR_DRIP (256 260 264): the previous chunk's 12 bpp words still to store into VRAM: source in
      LLRStage, VRAM destination, source end (the drip kernels store one per non-empty tile row) */
   uint32* ds; uint32* dd; uint32* de;
   const uint8* trow;                                                   /* 268: LLR_MODE2 llr_bgo: row per tile */
};
typedef uint32 (*llr_kfn)(struct llr_k*);
#ifdef LLR_MODE2
extern uint32 llr_bgo_f0(struct llr_k*), llr_bgo_f1(struct llr_k*), llr_bgo_f2(struct llr_k*), llr_bgo_f3(struct llr_k*),
   llr_bgo_f4(struct llr_k*), llr_bgo_f5(struct llr_k*), llr_bgo_f6(struct llr_k*), llr_bgo_f7(struct llr_k*);
static const llr_kfn llr_bgo[8] = { llr_bgo_f0, llr_bgo_f1, llr_bgo_f2, llr_bgo_f3, llr_bgo_f4, llr_bgo_f5, llr_bgo_f6, llr_bgo_f7 };
#endif
extern uint32 llr_bg_f0(struct llr_k*), llr_bg_f1(struct llr_k*), llr_bg_f2(struct llr_k*), llr_bg_f3(struct llr_k*),
   llr_bg_f4(struct llr_k*), llr_bg_f5(struct llr_k*), llr_bg_f6(struct llr_k*), llr_bg_f7(struct llr_k*);
static const llr_kfn llr_bg[8] = { llr_bg_f0, llr_bg_f1, llr_bg_f2, llr_bg_f3, llr_bg_f4, llr_bg_f5, llr_bg_f6, llr_bg_f7 };
extern uint32 llr_bg8_f0(struct llr_k*), llr_bg8_f1(struct llr_k*), llr_bg8_f2(struct llr_k*), llr_bg8_f3(struct llr_k*),
   llr_bg8_f4(struct llr_k*), llr_bg8_f5(struct llr_k*), llr_bg8_f6(struct llr_k*), llr_bg8_f7(struct llr_k*);
static const llr_kfn llr_bg8[8] = { llr_bg8_f0, llr_bg8_f1, llr_bg8_f2, llr_bg8_f3, llr_bg8_f4, llr_bg8_f5, llr_bg8_f6, llr_bg8_f7 };
#ifdef LLR_K2
/* milestone 3 kernels (genllr.py): pipelined row loop; 8 bpp ones for palette 0 only */
extern uint32 llr_bg2_f0(struct llr_k*), llr_bg2_f1(struct llr_k*), llr_bg2_f2(struct llr_k*), llr_bg2_f3(struct llr_k*),
   llr_bg2_f4(struct llr_k*), llr_bg2_f5(struct llr_k*), llr_bg2_f6(struct llr_k*), llr_bg2_f7(struct llr_k*);
static const llr_kfn llr_bg2[8] = { llr_bg2_f0, llr_bg2_f1, llr_bg2_f2, llr_bg2_f3, llr_bg2_f4, llr_bg2_f5, llr_bg2_f6, llr_bg2_f7 };
extern uint32 llr_bg82_f0(struct llr_k*), llr_bg82_f1(struct llr_k*), llr_bg82_f2(struct llr_k*), llr_bg82_f3(struct llr_k*),
   llr_bg82_f4(struct llr_k*), llr_bg82_f5(struct llr_k*), llr_bg82_f6(struct llr_k*), llr_bg82_f7(struct llr_k*);
extern uint32 llr_bg3_f0(struct llr_k*), llr_bg3_f1(struct llr_k*), llr_bg3_f2(struct llr_k*), llr_bg3_f3(struct llr_k*),
   llr_bg3_f4(struct llr_k*), llr_bg3_f5(struct llr_k*), llr_bg3_f6(struct llr_k*), llr_bg3_f7(struct llr_k*);
/* -DLLR_K3: llr_bg2 + a path for rows without transparent pixels */
static const llr_kfn llr_bg3[8] = { llr_bg3_f0, llr_bg3_f1, llr_bg3_f2, llr_bg3_f3, llr_bg3_f4, llr_bg3_f5, llr_bg3_f6, llr_bg3_f7 };
#ifdef LLR_DRIPON
extern uint32 llr_bg2d_f0(struct llr_k*), llr_bg2d_f1(struct llr_k*), llr_bg2d_f2(struct llr_k*), llr_bg2d_f3(struct llr_k*),
   llr_bg2d_f4(struct llr_k*), llr_bg2d_f5(struct llr_k*), llr_bg2d_f6(struct llr_k*), llr_bg2d_f7(struct llr_k*);
extern uint32 llr_bg3d_f0(struct llr_k*), llr_bg3d_f1(struct llr_k*), llr_bg3d_f2(struct llr_k*), llr_bg3d_f3(struct llr_k*),
   llr_bg3d_f4(struct llr_k*), llr_bg3d_f5(struct llr_k*), llr_bg3d_f6(struct llr_k*), llr_bg3d_f7(struct llr_k*);
static const llr_kfn llr_bg2d[8] = { llr_bg2d_f0, llr_bg2d_f1, llr_bg2d_f2, llr_bg2d_f3, llr_bg2d_f4, llr_bg2d_f5, llr_bg2d_f6, llr_bg2d_f7 };
static const llr_kfn llr_bg3d[8] = { llr_bg3d_f0, llr_bg3d_f1, llr_bg3d_f2, llr_bg3d_f3, llr_bg3d_f4, llr_bg3d_f5, llr_bg3d_f6, llr_bg3d_f7 };
static struct llr_k LLRK;
/* -DLLR_DRIP: a chunk's 12 bpp output is stored into VRAM while the next chunk is drawn, one word per
   non-empty BG tile row by the drip kernels (the posted st.di then overlaps the row's work). The chunk's
   CG-index lines stay in the other line-buffer set; llr_bg_run converts them a few lines at a time into a
   small (cache-hot) stage; the rest is stored by llr_drip_flush before the next chunk's output. Only for
   chunks with one palette for all lines (no log entry inside) and contiguous LCD rows. */
static uint8 LLRLbS[2][LLR_CH][LBW] __attribute__((aligned(32)));
static uint8 (*LLRLb)[LBW] = LLRLbS[0];
static uint8 (*LLRDLb)[LBW];       /* the dripping chunk's lines */
static uint32 LLRDl, LLRDn, LLRDlit, LLRDa, LLRDb;   /* next line to convert, lines, backdrop swap + saved */
static uint32 LLRStage[4 * 60] __attribute__((aligned(32)));
extern void llr_cpdi(uint32* dst, const uint32* src, uint32 nwords);
#ifdef LLR_OUT2
#define LLR_OUTP(q, lb) llr_out12di2(q, lb, LLRPalA, LLRPalBv)
#else
#define LLR_OUTP(q, lb) llr_out12di(q, lb, LLRPalA, LLRPalBv)
#endif
/* top the stage up to >= 60 words when lines are left */
static void llr_drip_refill(void)
{
   uint32 r = LLRK.ds < LLRK.de ? LLRK.de - LLRK.ds : 0;
   uint32* q;
   if (r >= 60 || LLRDl >= LLRDn)
      return;
   memmove(LLRStage, LLRK.ds, r * 4);
   q = LLRStage + r;
   do
   {
      llr_out12((uint8*) q, LLRDLb[LLRDl++] + LBO, LLRPalA, LLRPalBv);
#ifdef LLR_DSTAT
      LLRStat[4]++;
#endif
      q += 60;
   } while (LLRDl < LLRDn && q + 60 <= LLRStage + 4 * 60);
   LLRK.ds = LLRStage, LLRK.de = q;
}
/* the drip kernels at a tile boundary with the stage used up; nothing left: ds != de, the kernels stop asking */
void llr_drip_krefill(void)
{
#ifdef LLR_DSTAT
   LLRStat[7]++;
#endif
   llr_drip_refill();
   if (LLRK.ds >= LLRK.de)
      LLRK.ds = LLRStage + 1, LLRK.de = LLRStage;
}
/* store what is left of the dripping chunk (st.di), put the backdrop entry back */
static void llr_drip_flush(void)
{
   uint32 r = LLRK.ds < LLRK.de ? LLRK.de - LLRK.ds : 0;
   uint32* d = LLRK.dd;
#ifdef LLR_DSTAT
   LLRStat[5] += r, LLRStat[6] += LLRDn - LLRDl;
#endif
   if (r)
      llr_cpdi(d, LLRK.ds, r), d += r;
   for (; LLRDl < LLRDn; LLRDl++, d += 60)
      LLR_OUTP((uint8*) d, LLRDLb[LLRDl] + LBO);
   LLRK.ds = LLRK.de = 0;
   LLRDn = LLRDl = 0;
   if (LLRDlit)
      LLRPalA[0] = LLRDa, LLRPalBv[0] = LLRDb, LLRDlit = 0;
}
#endif
static const llr_kfn llr_bg82[8] = { llr_bg82_f0, llr_bg82_f1, llr_bg82_f2, llr_bg82_f3, llr_bg82_f4, llr_bg82_f5, llr_bg82_f6, llr_bg82_f7 };
#endif

static void llr_init(void)
{
   static const signed char col[8] = { 0, 1, -1, 2, 3, -1, 4, -1 };
   int f, i;
   for (f = 0; f < 8; f++)
      for (i = 0; i < 8; i++)
      {
         int x = i - f, g = x < 0 ? -1 : 0, c = x - 8 * g;
         LLRPix[f][i] = col[c] < 0 ? LLR_NOPIX : g * 5 + col[c];
      }
}

/* C: one run of one BG layer, tiles of priority prio (0 or 0x2000) */
LLR_NI static void llr_bg_c(const struct llr_layer* L, const struct llr_run* R, uint32 prio)
{
   uint32 hp = R->hofs & (L->tile16 ? 0x3ff : 0x1ff), q = hp >> 3, f = hp & 7, n = f ? 33 : 32, k;
   const signed char* px = LLRPix[f];
   const uint8* m = L->mask;
   for (k = 0; k < n; k++, q++)
   {
      uint32 e, t, pal, j, hf;
      if (!L->tile16)
      {
         q &= 63;
         e = q > 31 ? R->b2[q & 31] : R->b1[q];
      }
      else
      {
         q &= 127;
         e = q > 63 ? R->b2[(q >> 1) & 31] : R->b1[q >> 1];
         e += tx_table[R->txi + ((e & (H_FLIP | V_FLIP)) >> 13) + (q & 1)];
      }
      if ((e ^ prio) & 0x2000)
         continue;
      t = ((e & 0x3ff) + L->nbase) & L->tmask;
      if (!L->cached[t])
         L->cached[t] = llr_convert(L->depth, t);
      if (L->cached[t] == BLANK_TILE)
         continue;
      pal = L->palstart + ((e >> L->palsh) & L->palmask);
      hf = (e & H_FLIP) ? 7 : 0;
      for (j = 0; j < R->n; j++)
      {
         uint32 r = R->r[j], i;
         uint8* d = LLRLb[R->j[j]] + LBO + 5 * k;
         if (e & V_FLIP)
            r = 7 - r;
         if (L->bpp == 8)
         {
            const uint8* p = L->cache8 + (t << 6) + r * 8;
            for (i = 0; i < 8; i++)
            {
               int o = px[i];
               uint32 v = p[i ^ hf];
               if (o == LLR_NOPIX || !v || (m && !m[LBO + 5 * k + o]))
                  continue;
               d[o] = pal + v;
            }
         }
         else
         {
            uint32 w = L->nib[t * 8 + r];
            if (!w)
               continue;
            for (i = 0; i < 8; i++)
            {
               int o = px[i];
               uint32 v = (w >> (4 * (i ^ hf))) & 15;
               if (o == LLR_NOPIX || !v || (m && !m[LBO + 5 * k + o]))
                  continue;
               d[o] = pal + v;
            }
         }
      }
   }
}

#ifdef LLR_MODE2
/* offset-per-tile lines: column k's entry and row from R->ent / R->optr (4-bit BGs; no window, see
   llr_fallback): only the tile pixels that reach the LCD line (LLRPix != NOPIX) */
__attribute__((noinline)) static void llr_bg_optc(const struct llr_layer* L, const struct llr_run* R, uint32 prio)
{
   uint32 f = R->hofs & 7, n = f ? 33 : 32, k, jj, np = 0, i;
   uint8 pi[8], po[8];
   for (i = 0; i < 8; i++)
      if (LLRPix[f][i] != LLR_NOPIX)
         pi[np] = i, po[np++] = LLRPix[f][i];
   for (jj = 0; jj < R->n; jj++)
   {
      uint8* d0 = LLRLb[R->j[jj]] + LBO;
      for (k = 0; k < n; k++, d0 += 5)
      {
         uint32 e = R->ent[k], t, pal, sh, r = R->optr ? R->optr[k] : R->r[jj], w;
         if ((e ^ prio) & 0x2000)
            continue;
         t = ((e & 0x3ff) + L->nbase) & L->tmask;
         if (!L->cached[t])
            L->cached[t] = llr_convert(L->depth, t);
         if (L->cached[t] == BLANK_TILE)
            continue;
         if (e & V_FLIP)
            r = 7 - r;
         w = L->nib[t * 8 + r];
         if (!w)
            continue;
         pal = L->palstart + ((e >> L->palsh) & L->palmask);
         sh = (e & H_FLIP) ? 7 : 0;
         for (i = 0; i < np; i++)
         {
            uint32 v = (w >> (4 * (pi[i] ^ sh))) & 15;
            if (v)
               d0[(int8) po[i]] = pal + v;
         }
      }
   }
}

/* entries / rows of screen line Y of BG l as gfx.c DrawBackgroundOffset reads them (no window: the line
   starts at Left 0); returns 1 when the rows differ between columns */
static uint16 LLROptE[2][LLR_CH][34];
static uint8 LLROptR[2][LLR_CH][34];
static struct { uint32 ok, a, b, c, d, gen; uint32 vo[34], ho[34]; } LLROptC[2];   /* reset per part */
/* the last line computed per BG: on a later line Y + d (d < 8, same column offsets) a column keeps its map
   entry with row + d unless that crosses its tile row (then its entry is read again) */
static struct { uint32 ok, y, gen, n; uint16 e[34]; uint8 r[34]; } LLROptL[2];
#ifdef LLR_OPTSTAT
unsigned LLROptStat[8];
#endif
__attribute__((noinline)) static int llr_opt_line(const struct llr_layer* L, uint32 l, uint32 j, uint32 Y, uint32 src)
{
   uint32 VOff = LineData[src].BG[2].VOffset - 1, HOff = LineData[src].BG[2].HOffset;
   uint32 LineH = LineData[src].BG[l].HOffset, LineV = LineData[src].BG[l].VOffset;
   uint32 en = 1u << (l + 13), om = L->tile16 ? 0x3ff : 0x1ff, sh = L->tile16 ? 4 : 3;
   uint32 sl = VOff >> 3, f = LineH & 7, n = f ? 33 : 32, k, vary = 0, inc, d;
   uint16 *BPS0, *BPS1, *BPS2, *BPS3, *s1, *s2;
   int vo;
   uint16* E = LLROptE[l][j];
   uint8* Rw = LLROptR[l][j];
   BPS0 = (uint16*) &Memory.VRAM[PPU.BG[2].SCBase << 1];
   BPS1 = (PPU.BG[2].SCSize & 1) ? BPS0 + 1024 : BPS0;
   BPS2 = (PPU.BG[2].SCSize & 2) ? BPS1 + 1024 : BPS0;
   BPS3 = (PPU.BG[2].SCSize & 1) ? BPS2 + 1024 : BPS2;
   if (sl & 0x20)
      s1 = BPS2, s2 = BPS3;
   else
      s1 = BPS0, s2 = BPS1;
   s1 += (sl & 0x1f) << 5;
   s2 += (sl & 0x1f) << 5;
   if ((sl & 0x1f) == 0x1f)
      vo = (sl & 0x20) ? BPS0 - BPS2 - 0x1f * 32 : BPS2 - BPS0 - 0x1f * 32;
   else
      vo = 32;
   if (!(LLROptC[l].ok && LLROptC[l].a == LineData[src].BG[2].VOffset && LLROptC[l].b == HOff &&
         LLROptC[l].c == LineH && LLROptC[l].d == LineV))
   {
      for (k = 0; k < n; k++)
      {
         uint32 Left = k ? 8 * k - f : 0;
         if (!k)
            LLROptC[l].vo[k] = LineV, LLROptC[l].ho[k] = LineH;
         else
         {
            uint32 Quot2 = ((HOff + Left - 1) & om) >> 3, HCell, VCell;
            const uint16* s0 = Quot2 > 31 ? s2 + (Quot2 & 0x1f) : s1 + Quot2;
            HCell = READ_2BYTES(s0);
            VCell = READ_2BYTES(s0 + vo);
            LLROptC[l].vo[k] = (VCell & en) ? VCell + 1 : LineV;
            LLROptC[l].ho[k] = (HCell & en) ? (HCell & ~7) | (LineH & 7) : LineH;
         }
      }
      LLROptC[l].ok = 1, LLROptC[l].a = LineData[src].BG[2].VOffset, LLROptC[l].b = HOff;
      LLROptC[l].c = LineH, LLROptC[l].d = LineV;
      LLROptC[l].gen++;
   }
   d = Y - LLROptL[l].y;
   inc = LLROptL[l].ok && LLROptL[l].gen == LLROptC[l].gen && d - 1 < 7 && LLROptL[l].n == n;
#ifdef LLR_OPTSTAT
   { extern unsigned LLROptStat[8]; LLROptStat[0]++; LLROptStat[1] += inc; LLROptStat[2] += LLROptL[l].gen == LLROptC[l].gen; LLROptStat[3] += d - 1 < 7; }
#endif
   for (k = 0; k < n; k++)
   {
      uint32 Left = k ? 8 * k - f : 0, VOffset = LLROptC[l].vo[k], HOffset = LLROptC[l].ho[k], HPos, Quot, e, ScreenLine;
      const uint16 *b1, *b2;
      const uint16* const* sc;
      if (inc && LLROptL[l].r[k] + d < 8)
      {
         E[k] = LLROptL[l].e[k];
         Rw[k] = LLROptL[l].r[k] + d;
         if (Rw[k] != Rw[0])
            vary = 1;
         continue;
      }
      ScreenLine = (VOffset + Y) >> sh;
      sc = L->sc + ((ScreenLine & 0x20) ? 2 : 0);
      b1 = sc[0] + ((ScreenLine & 0x1f) << 5);
      b2 = sc[1] + ((ScreenLine & 0x1f) << 5);
      HPos = (HOffset + Left) & om;
      Quot = HPos >> 3;
      if (!L->tile16)
         e = Quot > 31 ? b2[Quot & 0x1f] : b1[Quot];
      else
      {
         uint32 txi = (((VOffset + Y) & 15) <= 7) << 3;
         e = Quot > 63 ? b2[(Quot >> 1) & 0x1f] : b1[Quot >> 1];
         e = (uint16) (e + tx_table[txi + ((e & (H_FLIP | V_FLIP)) >> 13) + (Quot & 1)]);
      }
      E[k] = e;
      Rw[k] = (Y + VOffset) & 7;
      if (Rw[k] != Rw[0])
         vary = 1;
   }
   LLROptL[l].ok = 1, LLROptL[l].y = Y, LLROptL[l].gen = LLROptC[l].gen, LLROptL[l].n = n;
   memcpy(LLROptL[l].e, E, sizeof LLROptL[l].e);
   memcpy(LLROptL[l].r, Rw, sizeof LLROptL[l].r);
   return vary;
}
#endif

LLR_NI static void llr_bg_run(const struct llr_layer* L, const struct llr_run* R, uint32 prio)
{
#ifndef LLR_CKERN
#ifdef LLR_T16
   /* A4: 16x16 tiles through the 8x8 kernels: the run's map row as 8x8 entries (each 16x16 entry + llr_bg_c's
      tx_table offset for its half), the same entries llr_bg_c draws */
   if (!L->mask
#ifdef LLR_MODE2
       && !(R->ent && R->optr && L->bpp == 8)
#endif
      )
#else
   if (!L->tile16 && !L->mask)
#endif
   {
#ifdef LLR_DRIPON
#define K LLRK
#else
      static struct llr_k K;
#endif
#ifdef LLR_K2
      if (prio && !R->nhi)
         return;                   /* the low-priority pass found no high-priority tile */
      K.pad[1] = 0;
#endif
      uint32 hp = R->hofs & 0x1ff, q = hp >> 3, n = (hp & 7) ? 33 : 32, j, rs = L->bpp == 8 ? 8 : 4;
#ifdef LLR_K2
#ifdef LLR_K3
      llr_kfn kf = (L->bpp != 8 ? llr_bg3 : L->palstart || L->palmask ? llr_bg8 : llr_bg82)[hp & 7];
#else
      llr_kfn kf = (L->bpp != 8 ? llr_bg2 : L->palstart || L->palmask ? llr_bg8 : llr_bg82)[hp & 7];
#endif
#ifdef LLR_DRIPON
      llr_drip_refill();
      if (K.ds < K.de && L->bpp != 8)
#ifdef LLR_K3
         kf = llr_bg3d[hp & 7];
#else
         kf = llr_bg2d[hp & 7];
#endif
#endif
#else
      llr_kfn kf = (L->bpp == 8 ? llr_bg8 : llr_bg)[hp & 7];
#endif
#ifdef LLR_T16
      static uint16 V16[36];
#ifdef LLR_MODE2
      if (R->ent && R->optr)
         kf = llr_bgo[hp & 7], K.trow = R->optr;   /* rows per tile (one line) */
      if (R->ent)
      {
         K.ma = K.mb = R->ent;
         K.na = n;
         K.nbc = 0;
      }
      else
#endif
      if (L->tile16)
      {
         /* one map entry gives two 8x8 entries: from the 16x16 column of the first one, n + 1 entries */
         uint32 h = R->hofs & 0x3ff, c = h >> 4, k;
         const int* tx = tx_table + R->txi;
         for (k = 0; k < n + 1; k += 2, c++)
         {
            uint32 e;
            const int* t;
            c &= 63;
            e = c > 31 ? R->b2[c & 31] : R->b1[c];
            t = tx + ((e & (H_FLIP | V_FLIP)) >> 13);
            V16[k] = e + t[0];
            V16[k + 1] = e + t[1];
         }
         K.ma = K.mb = V16 + ((h >> 3) & 1);
         K.na = n;
         K.nbc = 0;
      }
      else
#endif
      {
      K.ma = q > 31 ? R->b2 + (q & 31) : R->b1 + q;
      K.na = 32 - (q & 31);
      if (K.na > n)
         K.na = n;
      K.mb = q > 31 ? R->b1 : R->b2;
      K.nbc = n - K.na;
      }
      K.nib = L->bpp == 8 ? (const uint32*) L->cache8 : L->nib;
      K.cached = L->cached;
      K.nbase = L->nbase;
      K.tmask = L->tmask;
      K.palstart = L->palstart;
      K.palsh = L->palsh;
      K.palmask = L->palmask;
      K.prio = prio;
      K.nrows = R->n;
      K.k = 0;
      for (j = 0; j < R->n; j++)
      {
         K.lb[j] = LLRLb[R->j[j]] + LBO;
         K.ro[j] = R->r[j] * rs;
         K.rv[j] = (7 - R->r[j]) * rs;
      }
      /* the kernel returns 1 at a tile not yet converted (K.k), converts it and goes on */
#ifdef LLR_KDBG
      unsigned dbgn = 0;
      extern unsigned LLRDbgF;
      printf("K f%u bpp %u hp %x n %u prio %x na %u nbc %u\n", LLRDbgF, L->bpp, hp, R->n, prio, K.na, K.nbc);
#endif
      while (kf(&K))
      {
         uint32 e = K.k < K.na ? K.ma[K.k] : K.mb[K.k - K.na], t = ((e & 0x3ff) + L->nbase) & L->tmask;
#ifdef LLR_KDBG
         if (++dbgn > 100) { printf("KDBG k %u na %u nbc %u e %x t %x c %u bpp %u\n", K.k, K.na, K.nbc, e, t, L->cached[t], L->bpp); for (;;) ; }
#endif
         L->cached[t] = llr_convert(L->depth, t);
      }
#ifdef LLR_K2
      if (!prio)
         ((struct llr_run*) R)->nhi = K.pad[1];
#endif
      return;
#undef K
   }
#endif
#ifdef LLR_MODE2
   if (R->ent)
   {
      llr_bg_optc(L, R, prio);
      return;
   }
#endif
   llr_bg_c(L, R, prio);
}

/* ---- sprites ---- */
static uint16 LLROOff[241];
static uint8 LLROPool[128 * 240], LLROPri[240];
#ifdef LLR_OBJCH
static uint32 LLRONum;
#ifdef LLR_CHMASK
/* C4 (-DLLR_CHMASK): a chunk's sprite list from the per-line lists (LLROPool): bit k of a 128-bit mask = OBJList[k]
   (LLRObjIdx[S] = k), read from the top bit down (= reverse OBJList order). It leaves out only sprites on none of the
   chunk's lines, which llr_obj_chunk skips anyway (nl == 0): the same pixels. */
static uint8 LLRObjIdx[128];
#endif
#ifdef LLR_CHSEL
static int16 LLRChV[128], LLRChE[128];   /* -DLLR_CHSEL (M10): OBJList[k]'s first line / end, packed for the chunk scan */
#endif
#endif
uint32 LLROBJDirty = 1;
LLR_NI static void llr_objlists(void)
{
   uint32 h = PPU.ScreenHeight, y, i, n;
   static uint16 cnt[240];
   int S;
   memset(cnt, 0, sizeof cnt);
   memset(LLROPri, 0, sizeof LLROPri);
   for (i = 0; (S = GFX.OBJList[i]) >= 0; i++)
   {
      int v = GFX.VPositions[S], e = v + (int) GFX.Sizes[S], yy;
      if (v < 0) v = 0;
      if (e > (int) h) e = h;
      for (yy = v; yy < e; yy++)
         if (LLRRow[yy] != 255)
         {
            cnt[yy]++;
            LLROPri[yy] |= 1 << PPU.OBJ[S].Priority;
         }
   }
   for (n = 0, y = 0; y < h; y++)
   {
      LLROOff[y] = n;
      n += cnt[y];
      cnt[y] = LLROOff[y];
   }
   LLROOff[h] = n;
   for (i = 0; (S = GFX.OBJList[i]) >= 0; i++)
   {
      int v = GFX.VPositions[S], e = v + (int) GFX.Sizes[S], yy;
      if (v < 0) v = 0;
      if (e > (int) h) e = h;
      for (yy = v; yy < e; yy++)
         if (LLRRow[yy] != 255)
            LLROPool[cnt[yy]++] = S;
   }
#ifdef LLR_OBJCH
   LLRONum = i;
#ifdef LLR_CHMASK
   for (i = 0; i < LLRONum; i++)
      LLRObjIdx[GFX.OBJList[i] & 127] = i;
#endif
#ifdef LLR_CHSEL
   for (i = 0; i < LLRONum; i++)
      LLRChV[i] = GFX.VPositions[GFX.OBJList[i]], LLRChE[i] = GFX.VPositions[GFX.OBJList[i]] + (int) GFX.Sizes[GFX.OBJList[i]];
#endif
#endif
   LLROBJDirty = 0;
#ifdef LLR_SKIP
   llr_skip_obj();
#endif
}

extern void llr_objpx(uint8* b, uint32 w, uint32 pal, uint32 sel);
/* one sprite's row on line y into d (line buffer + LBO); pri: also its priority there (mixed lines) */
LLR_NI static void llr_obj_row(uint8* d, uint8* pri, uint32 S, uint32 y, const uint8* m)
{
   const SOBJ* o = &PPU.OBJ[S];
   int sz = GFX.Sizes[S], l = (int) y - GFX.VPositions[S], x0 = o->HPos;
   uint32 rowoff, r, ncol = sz >> 3, X, pal = 128 + (o->Palette << 4), hf = o->HFlip ? 7 : 0, pr = o->Priority;
   uint8* cached = IPPU.TileCached[TILE_4BIT];
   if (o->VFlip)
      l = sz - 1 - l;
   rowoff = (l >> 3) << 4;
   r = l & 7;
#ifdef LLR_OBJROW
   if (!m && !pri)
   {
      /* llrk.S: the whole tile loop; it stops at a tile not converted yet */
      extern uint32 llr_objrow(uint32* k);
      uint32 k[11];
      k[0] = (uint32) d, k[1] = x0, k[2] = ncol, k[3] = 0, k[4] = PPU.OBJNameBase >> 5, k[5] = o->Name + rowoff;
      k[6] = hf ? 1 : 0, k[7] = pal, k[8] = (uint32) cached, k[9] = (uint32) (LLNib4 + r);
      while (llr_objrow(k))
         cached[k[10]] = llr_convert(TILE_4BIT, k[10]);
      return;
   }
#endif
   for (X = 0; X < ncol; X++, x0 += 8)
   {
      uint32 col, tn, w, f, i;
      uint8* b;
      if (x0 <= -8)
         continue;
      if (x0 >= 256)
         break;
      col = hf ? ncol - 1 - X : X;
      tn = ((PPU.OBJNameBase >> 5) + ((o->Name + rowoff + col) & 0x3ff)) & 0x7ff;
      if (!cached[tn])
         cached[tn] = llr_convert(TILE_4BIT, tn);
      if (cached[tn] == BLANK_TILE || !(w = LLNib4[tn * 8 + r]))
         continue;
      f = (-x0) & 7;
      b = d + 5 * ((x0 + (int) f) >> 3);
#ifdef LLR_OBJASM
      if (!m && !pri)
      {
         llr_objpx(b, w, pal, f | (hf ? 8 : 0));
         continue;
      }
#endif
      for (i = 0; i < 8; i++)
      {
         int oo = LLRPix[f][i];
         uint32 v = (w >> (4 * (i ^ hf))) & 15;
         if (oo == LLR_NOPIX || !v)
            continue;
         if (m && !m[(b - d) + LBO + oo])
            continue;
         b[oo] = pal + v;
         if (pri)
            pri[(b - d) + oo] = pr;
      }
   }
}

#if defined LLR_OBJCH && defined LLR_OBJASM
/* M7 -DLLR_OBJCH: the sprites of one priority on the chunk lines that hold only that priority, sprite by
   sprite (reverse OBJList order: on every line the same order as llr_obj_row's loop), tile column by tile
   column, line by line: the per-sprite and per-column work is done once per chunk, and a sprite's nibble rows
   are read while its cache lines are still there. Same pixels as the per-line llr_obj_row calls. */
static uint8 LLRChS[128];
static uint32 LLRChN;
__attribute__((noinline)) static void llr_obj_chunk(const uint8* cy, uint32 cn, uint32 lm, uint32 pr)
{
   uint32 i, nb = PPU.OBJNameBase >> 5;
   uint8* cached = IPPU.TileCached[TILE_4BIT];
   for (i = 0; i < LLRChN; i++)
   {
      uint32 S = LLRChS[i], j, nl = 0, X, Xs, Xe, ncol, hf, pal;
      const SOBJ* o = &PPU.OBJ[S];
      int v, sz, x0;
      uint8* ld[LLR_CH];
      const uint32* ln[LLR_CH];
      uint32 lb[LLR_CH];
      if (o->Priority != pr)
         continue;
      v = GFX.VPositions[S], sz = GFX.Sizes[S], x0 = o->HPos;
      for (j = 0; j < cn; j++)
      {
         int l = (int) cy[j] - v;
         if (!(lm & (1u << j)) || l < 0 || l >= sz)
            continue;
         if (o->VFlip)
            l = sz - 1 - l;
         ld[nl] = LLRLb[j], ln[nl] = LLNib4 + (l & 7), lb[nl++] = o->Name + ((l >> 3) << 4);
      }
      if (!nl)
         continue;
      ncol = sz >> 3;
      Xs = x0 > -8 ? 0 : (uint32) (-x0) >> 3;
      Xe = (uint32) (256 - x0 + 7) >> 3;
      if (Xe > ncol)
         Xe = ncol;
      hf = o->HFlip ? 1 : 0;
      pal = 128 + (o->Palette << 4);
      for (X = Xs; X < Xe; X++)
      {
         int x = x0 + 8 * (int) X;
         uint32 col = hf ? ncol - 1 - X : X, f = (-x) & 7, sel = f | (hf << 3), boff = LBO + 5 * ((x + (int) f) >> 3), k;
         for (k = 0; k < nl; k++)
         {
            uint32 tn = (nb + ((lb[k] + col) & 0x3ff)) & 0x7ff, w, c = cached[tn];
            if (!c)
               c = cached[tn] = llr_convert(TILE_4BIT, tn);
            if (c == BLANK_TILE || !(w = ln[k][tn * 8]))
               continue;
            llr_objpx(ld[k] + boff, w, pal, sel);
         }
      }
   }
}
#endif

/* mixed-priority sprite line: the sprite pixels of priority pr over the line buffer */
static LLR_NI void llr_mixc(uint8* d, const uint8* o, const uint8* op, uint32 c, uint32 e, uint32 pr)
{
   for (; c < e; c++)
      if (o[c] && op[c] == pr)
         d[c] = o[c];
}

/* ---- one part (screen lines y0..y1, PPU state as it is now) ---- */
static const uint16* llr_scaddr(uint32 a)
{
   return (const uint16*) &Memory.VRAM[a];
}
LLR_NI static void llr_masks(uint32 l, const ClipData* c)
{
   uint8* m = LLRMask[l];
   uint32 i, k;
   memset(m, 0, LBW);
   for (k = 0; k < c->Count[l]; k++)
   {
      uint32 L = c->Left[k][l], R = c->Right[k][l];
      for (i = 0; i < 160; i++)
      {
         uint32 x = (i / 5) * 8 + "\0\1\3\4\6"[i % 5];
         if (x >= L && x < R)
            m[LBO + i] = 1;
      }
   }
}

#ifdef LLR_M7
/* -DLLR_M7: Mode 7 without transparency, as gfx.c's DrawBGMode7Background16New (+ LL_M7SHOWN) draws it
   into the old renderer's screen: BG1 always (it doesn't look at 212C/D), window spans of IPPU.Clip[0]
   (with H flip each span starts from its own right edge), no mosaic, no EXTBG; sprites on top. Only the
   160 shown columns; the layer is drawn first, so a transparent pixel leaves the backdrop (0). */
static const uint8 LLRM7Col[8] = { 0, 1, 0xff, 2, 3, 0xff, 4, 0xff };
#if defined LLR_M7C
/* B6: llr_m7w3/n3 (genllr.py m7c): the char address from LLM7TP[tile] = VRAM + 1 + tile * 128 (filled by
   llr_m7tp when Memory.VRAM moves, i.e. once), same pixels */
#define llr_m7w llr_m7w3
#define llr_m7n llr_m7n3
const uint8* LLM7TP[256];
static void llr_m7tp(void)
{
   uint32 t;
   if (LLM7TP[0] != Memory.VRAM + 1)
      for (t = 0; t < 256; t++)
         LLM7TP[t] = Memory.VRAM + 1 + t * 128;
}
#elif defined LLR_M7B
/* M7: leaner Mode 7 kernels (genllr.py m7b), same pixels */
#define llr_m7w llr_m7w2
#define llr_m7n llr_m7n2
#endif
#ifdef LLR_M7P
/* B1 (-DLLR_M7P): a de-tiled copy of the Mode 7 plane, texel (X, Y) at LLM7Plane[Y * 1024 + X], so the kernels
   (genllr.py m7p) need no tile-map load (8-9 instructions a pixel instead of 17). Built lazily in blocks of 8x8
   tiles (64x64 texels) when a line needs them; a block is dropped when VRAM 0-7fff changes under it: its map
   entries (each 16-byte VRAM block holds 8 map entries of one block) or a char it uses (LLM7BChr). Every VRAM
   writer marks LLV_DIRTY (ppu.h); llr_m7p_ok syncs before a line is drawn. Lines with a large step (far away:
   little reuse, the tiled VRAM's shared chars cache better) or touching more than LLR_M7PB blocks keep the VRAM
   kernels. Same pixels either way. */
#ifndef LLR_M7PT
#define LLR_M7PT 384
#endif
#ifndef LLR_M7PB
#define LLR_M7PB 24
#endif
static uint8 LLM7Plane[1024 * 1024] __attribute__((aligned(32)));
static uint8 LLM7BOk[256];
static uint32 LLM7BChr[256][8];
unsigned int LLVDirty[128], LLM7Any;
void LLM7Reset(void)
{
   memset(LLM7BOk, 0, sizeof LLM7BOk);
   memset(LLVDirty, 0, sizeof LLVDirty);
   LLM7Any = 0;
}
static void llr_m7p_build(uint32 b)
{
   const uint8* V = Memory.VRAM;
   uint32 bx = b & 15, by = b >> 4, tx, ty, r;
   uint32* cs = LLM7BChr[b];
   memset(cs, 0, 32);
   for (ty = by * 8; ty < by * 8 + 8; ty++)
      for (tx = bx * 8; tx < bx * 8 + 8; tx++)
      {
         uint32 t = V[(ty * 128 + tx) * 2];
         const uint8* q = V + t * 128 + 1;
         uint32* dp = (uint32*) (LLM7Plane + ty * 8 * 1024 + tx * 8);
         cs[t >> 5] |= 1u << (t & 31);
         for (r = 0; r < 8; r++, q += 16, dp += 256)
         {
            dp[0] = q[0] | q[2] << 8 | q[4] << 16 | (uint32) q[6] << 24;
            dp[1] = q[8] | q[10] << 8 | q[12] << 16 | (uint32) q[14] << 24;
         }
      }
   LLM7BOk[b] = 1;
}
static void llr_m7p_sync(void)
{
   uint32 i, k, chr[8], anyc = 0;
   memset(chr, 0, sizeof chr);
   LLM7Any = 0;
   for (i = 0; i < 64; i++)
   {
      uint32 m = LLVDirty[i];
      if (!m)
         continue;
      LLVDirty[i] = 0;
      for (k = 0; k < 32; k++)
         if (m & (1u << k))
         {
            uint32 w = (i * 32 + k) * 8, ch = w >> 6;   /* the 16-byte block's first VRAM word */
            LLM7BOk[((w >> 10) << 4) | ((w & 127) >> 3)] = 0;
            chr[ch >> 5] |= 1u << (ch & 31);
            anyc = 1;
         }
   }
   if (anyc)
      for (i = 0; i < 256; i++)
         if (LLM7BOk[i])
         {
            const uint32* cs = LLM7BChr[i];
            if ((cs[0] & chr[0]) | (cs[1] & chr[1]) | (cs[2] & chr[2]) | (cs[3] & chr[3]) |
                (cs[4] & chr[4]) | (cs[5] & chr[5]) | (cs[6] & chr[6]) | (cs[7] & chr[7]))
               LLM7BOk[i] = 0;
         }
}
/* the block range of coordinates p0..p1 (8 fraction bits) on one axis: first block, count (0: none) */
static uint32 llr_m7p_axis(int32 p0, int32 p1, uint32 wrap, uint32* first)
{
   int32 lo = (p0 < p1 ? p0 : p1) >> 8, hi = (p0 < p1 ? p1 : p0) >> 8;
   if (wrap)
   {
      if (hi - lo >= 1023)
         return *first = 0, 16;
      *first = (uint32) (lo >> 6) & 15;
      return (uint32) ((hi >> 6) - (lo >> 6)) + 1;
   }
   if (lo < 0)
      lo = 0;
   if (hi > 1023)
      hi = 1023;
   if (lo > hi)
      return 0;
   *first = lo >> 6;
   return (uint32) ((hi >> 6) - (lo >> 6)) + 1;
}
/* 1: the plane copy holds every texel the line (columns 0-255 at A + x a, C + x c) reads */
static int llr_m7p_ok(uint32 A, uint32 C, int32 a, int32 c, uint32 wrap)
{
   uint32 x0, y0, nx, ny, i, j;
   if (a > LLR_M7PT || a < -LLR_M7PT || c > LLR_M7PT || c < -LLR_M7PT)
      return 0;
   if (LLM7Any)
      llr_m7p_sync();
   nx = llr_m7p_axis((int32) A, (int32) (A + 255 * a), wrap, &x0);
   ny = llr_m7p_axis((int32) C, (int32) (C + 255 * c), wrap, &y0);
   if (nx * ny > LLR_M7PB)
      return 0;
   for (j = 0; j < ny; j++)
      for (i = 0; i < nx; i++)
      {
         uint32 b = (((y0 + j) & 15) << 4) | ((x0 + i) & 15);
         if (!LLM7BOk[b])
            llr_m7p_build(b);
      }
   return 1;
}
extern void llr_m7pw(uint8* d, const uint8* P, uint32 A, uint32 C, uint32 a, uint32 c);
extern void llr_m7pn(uint8* d, const uint8* P, uint32 A, uint32 C, uint32 a, uint32 c, const uint8* out);
#endif
extern void LLLineMat(uint32 end);
/* lcd != 0 (-DLLR_M7F): draw straight to that LCD row if a fused kernel covers the line (returns 1), else
   draw nothing and return 0 */
static int llr_m7_line(uint32 Line, uint8* d, uint8* lcd)
{
   const SLineMatrixData* l = &LineMatrixData[Line];
   const uint8* VRAM1 = Memory.VRAM + 1;
   const ClipData* cp = &IPPU.Clip[0];
   int32 HOffset = ((int32) LineData[Line].BG[0].HOffset << M7) >> M7;
   int32 VOffset = ((int32) LineData[Line].BG[0].VOffset << M7) >> M7;
   int32 CentreX = ((int32) l->CentreX << M7) >> M7;
   int32 CentreY = ((int32) l->CentreY << M7) >> M7;
   int yy, BB, DD, aa, cc, startx, dir;
   uint32 clip, cnt = cp->Count[0], ncl = cnt ? cnt : 1, Left = 0, Right = 256;
   yy = PPU.Mode7VFlip ? 255 - (int) Line : (int) Line;
#ifdef LLR_M7C
   llr_m7tp();
#endif
#ifdef LLR_M7STAT
   { extern int printf(const char*, ...); extern unsigned frame; static unsigned n[6], f;
     n[!cnt ? 0 : cnt == 1 && cp->Left[0][0] == 0 && cp->Right[0][0] == 256 ? 1 : cnt == 1 ? 2 : 3]++; n[4 + (PPU.Mode7Repeat != 0)]++;
     if (frame != f) { if (frame % 100 == 0) printf("M7 f%u none %u full %u part %u multi %u rep0 %u rep %u L%d R%d\n", frame, n[0], n[1], n[2], n[3], n[4], n[5], cnt ? cp->Left[0][0] : -1, cnt ? cp->Right[0][0] : -1); f = frame; memset(n, 0, sizeof n); } }
#endif
   if (PPU.Mode7Repeat == 0)
      yy += (VOffset - CentreY) % 1023;
   else
      yy += VOffset - CentreY;
   BB = l->MatrixB * yy + (CentreX << 8);
   DD = l->MatrixD * yy + (CentreY << 8);
   if (!cnt && !PPU.Mode7Repeat)
   {
      /* the common case: one span 0..255, wrapping; column x at (A + x * a, C + x * c) */
      const uint8* V = Memory.VRAM;
      uint32 a, c, A, C, g;
      int xx;
      if (PPU.Mode7HFlip)
         xx = 255 + (HOffset - CentreX) % 1023, a = -l->MatrixA, c = -l->MatrixC;
      else
         xx = (HOffset - CentreX) % 1023, a = l->MatrixA, c = l->MatrixC;
      A = (uint32) (l->MatrixA * xx) + BB, C = (uint32) (l->MatrixC * xx) + DD;
#ifdef LLR_M7ASM
      {
         extern void llr_m7w(uint8* d, const uint8* V, uint32 A, uint32 C, uint32 a, uint32 c);
#ifdef LLR_M7F
         if (lcd)
         {
            extern void llr_m7wf(uint8* d, const uint8* V, uint32 A, uint32 C, uint32 a, uint32 c);
            llr_m7wf(lcd, V, A, C, a, c);
            return 1;
         }
#endif
#ifdef LLR_M7P
         if (llr_m7p_ok(A, C, (int32) a, (int32) c, 1))
            llr_m7pw(d, LLM7Plane, A, C, a, c);
         else
#endif
         llr_m7w(d, V, A, C, a, c);
         return 0;
      }
#endif
#define LLR_M7PX(o, k)       {          uint32 X = (A + (o) * a) >> 8, Y = (C + (o) * c) >> 8;          uint32 t = V[((Y & 0x3f8) << 5) + ((X >> 2) & 0xfe)];          d[k] = V[(t << 7) + ((Y & 7) << 4) + ((X & 7) << 1) + 1];       }
      for (g = 0; g < 32; g++, d += 5, A += 8 * a, C += 8 * c)
      {
         LLR_M7PX(0, 0) LLR_M7PX(1, 1) LLR_M7PX(3, 2) LLR_M7PX(4, 3) LLR_M7PX(6, 4)
      }
#undef LLR_M7PX
      return 0;
   }
   if (!cnt && !(Settings.Dezaemon && PPU.Mode7Repeat == 2))
   {
      /* one span 0..255 without wrapping: outside the 1024x1024 plane transparent, or (repeat 3) tile 0's
         row (yy + CentreY) & 7 at column (x' + HOffset) & 7, x' = the old loop's x */
      const uint8* V = Memory.VRAM;
      uint32 a, c, A, C, g, fl = PPU.Mode7HFlip;
      uint8 out[8];
      int xx;
      if (fl)
         xx = 255 + HOffset - CentreX, a = -l->MatrixA, c = -l->MatrixC;
      else
         xx = HOffset - CentreX, a = l->MatrixA, c = l->MatrixC;
      A = (uint32) (l->MatrixA * xx) + BB, C = (uint32) (l->MatrixC * xx) + DD;
#ifdef LLR_M7OUT
      /* M9: repeat 1/2 (outside transparent): no per-pixel tile-0 row to build */
      if (PPU.Mode7Repeat != 3)
         memset(out, 0, sizeof out);
      else
#endif
#if defined LLR_M7OUT2 && defined LLR_M7OUT
      {
         /* B2 (-DLLR_M7OUT2): tile 0's row once, then the 8 columns (same bytes as below) */
         const uint8* t0 = V + 1 + (((yy + CentreY) & 7) << 4);
         uint32 h = (uint32) HOffset + (fl ? 7 : 0);
         if (fl)
            for (g = 0; g < 8; g++)
               out[g] = t0[((h - g) & 7) << 1];
         else
            for (g = 0; g < 8; g++)
               out[g] = t0[((h + g) & 7) << 1];
      }
#else
      for (g = 0; g < 8; g++)
         out[g] = PPU.Mode7Repeat != 3 ? 0 : V[1 + (((yy + CentreY) & 7) << 4) + ((((fl ? 255 - g : g) + HOffset) & 7) << 1)];
#endif
#ifdef LLR_M7ASM
      {
         extern void llr_m7w(uint8* d, const uint8* V, uint32 A, uint32 C, uint32 a, uint32 c);
         extern void llr_m7n(uint8* d, const uint8* V, uint32 A, uint32 C, uint32 a, uint32 c, const uint8* out);
         /* the coordinates are linear in x and never overflow (|A|, |C| < 2^30), so the columns inside the
            plane are one interval: both ends inside = all inside, both ends past one edge = none */
         int32 a0 = (int32) A, a1 = (int32) (A + 255 * a), c0 = (int32) C, c1 = (int32) (C + 255 * c);
         const int32 M = 1024 * 256 - 1;
         if ((a0 < 0 && a1 < 0) || (a0 > M && a1 > M) || (c0 < 0 && c1 < 0) || (c0 > M && c1 > M))
         {
            if (lcd)
               return 0;
            if (out[0] | out[1] | out[3] | out[4] | out[6])
#ifdef LLR_M7FILL
            {
               /* A6: the 5-byte column pattern as 5 words (20 bytes), 8 times (d is word aligned) */
               uint32 w[5];
#ifdef LLR_M7OUT2
               uint32 o0 = out[0], o1 = out[1], o3 = out[3], o4 = out[4], o6 = out[6];
               w[0] = o0 | o1 << 8 | o3 << 16 | o4 << 24;   /* columns 0 1 3 4 6 repeating, little endian */
               w[1] = o6 | o0 << 8 | o1 << 16 | o3 << 24;
               w[2] = o4 | o6 << 8 | o0 << 16 | o1 << 24;
               w[3] = o3 | o4 << 8 | o6 << 16 | o0 << 24;
               w[4] = o1 | o3 << 8 | o4 << 16 | o6 << 24;
#else
               uint32 k;
               uint8* pb = (uint8*) w;
               for (k = 0; k < 20; k++)
                  pb[k] = out[k % 5 == 0 ? 0 : k % 5 == 1 ? 1 : k % 5 == 2 ? 3 : k % 5 == 3 ? 4 : 6];
#endif
               for (g = 0; g < 8; g++, d += 20)
                  ((uint32*) d)[0] = w[0], ((uint32*) d)[1] = w[1], ((uint32*) d)[2] = w[2], ((uint32*) d)[3] = w[3], ((uint32*) d)[4] = w[4];
            }
#else
               for (g = 0; g < 32; g++, d += 5)
                  d[0] = out[0], d[1] = out[1], d[2] = out[3], d[3] = out[4], d[4] = out[6];
#endif
         }
#ifdef LLR_M7F
         else if (lcd)
         {
            extern void llr_m7wf(uint8* d, const uint8* V, uint32 A, uint32 C, uint32 a, uint32 c);
            extern void llr_m7nf(uint8* d, const uint8* V, uint32 A, uint32 C, uint32 a, uint32 c, const uint8* out);
            if (a0 >= 0 && a1 >= 0 && a0 <= M && a1 <= M && c0 >= 0 && c1 >= 0 && c0 <= M && c1 <= M)
               llr_m7wf(lcd, V, A, C, a, c);
            else
               llr_m7nf(lcd, V, A, C, a, c, out);
            return 1;
         }
#endif
#ifdef LLR_M7P
         else if (llr_m7p_ok(A, C, (int32) a, (int32) c, 0))
         {
            if (a0 >= 0 && a1 >= 0 && a0 <= M && a1 <= M && c0 >= 0 && c1 >= 0 && c0 <= M && c1 <= M)
               llr_m7pw(d, LLM7Plane, A, C, a, c);
            else
               llr_m7pn(d, LLM7Plane, A, C, a, c, out);
         }
#endif
         else if (a0 >= 0 && a1 >= 0 && a0 <= M && a1 <= M && c0 >= 0 && c1 >= 0 && c0 <= M && c1 <= M)
            llr_m7w(d, V, A, C, a, c);
         else
            llr_m7n(d, V, A, C, a, c, out);
         return 0;
      }
#endif
#define LLR_M7PX(o, k)       {          int32 X = (int32) (A + (o) * a) >> 8, Y = (int32) (C + (o) * c) >> 8;          if ((X | Y) & ~0x3ff)             d[k] = out[o];          else          {             uint32 t = V[((Y & 0x3f8) << 5) + ((X >> 2) & 0xfe)];             d[k] = V[(t << 7) + ((Y & 7) << 4) + ((X & 7) << 1) + 1];          }       }
      for (g = 0; g < 32; g++, d += 5, A += 8 * a, C += 8 * c)
      {
         LLR_M7PX(0, 0) LLR_M7PX(1, 1) LLR_M7PX(3, 2) LLR_M7PX(4, 3) LLR_M7PX(6, 4)
      }
#undef LLR_M7PX
      return 0;
   }
   if (lcd)
      return 0;
   for (clip = 0; clip < ncl; clip++)
   {
      int xx, AA, CC;
      uint32 x;
      if (cnt)
      {
         Left = cp->Left[clip][0];
         Right = cp->Right[clip][0];
         if (Right <= Left)
            continue;
      }
      if (PPU.Mode7HFlip)
         startx = Right - 1, dir = -1, aa = -l->MatrixA, cc = -l->MatrixC;
      else
         startx = Left, dir = 1, aa = l->MatrixA, cc = l->MatrixC;
      if (PPU.Mode7Repeat == 0)
         xx = startx + (HOffset - CentreX) % 1023;
      else
         xx = startx + HOffset - CentreX;
      AA = l->MatrixA * xx;
      CC = l->MatrixC * xx;
      /* x: the screen column (the old loop's p) */
      for (x = Left; x < Right; x++, AA += aa, CC += cc)
      {
         uint32 c = LLRM7Col[x & 7], b;
         int X, Y;
         if (c == 0xff)
            continue;
         X = (AA + BB) >> 8, Y = (CC + DD) >> 8;
         if (!PPU.Mode7Repeat)
            X &= 0x3ff, Y &= 0x3ff;
         else
         {
            if (Settings.Dezaemon && PPU.Mode7Repeat == 2)
               X &= 0x7ff, Y &= 0x7ff;
            if ((X | Y) & ~0x3ff)
            {
               if (PPU.Mode7Repeat == 3)
               {
                  /* the old loop's x counts from startx in the drawing direction */
                  int ox = startx + dir * (int) (x - Left);
                  b = *(VRAM1 + ((((yy + CentreY) & 7)) << 4) + (((ox + HOffset) & 7) << 1));
                  if (b)
                     d[(x >> 3) * 5 + c] = b;
               }
               continue;
            }
         }
         b = *(VRAM1 + (Memory.VRAM[((Y & ~7) << 5) + ((X >> 2) & ~1)] << 7) + ((Y & 7) << 4) + ((X & 7) << 1));
         if (b)
            d[(x >> 3) * 5 + c] = b;
      }
   }
   return 0;
}
#endif

#ifdef LL_LINELOG
extern uint32 LLLineSame[8];
#endif
#if defined LL_LINELOG && defined LLR_SRCW
/* -DLLR_SRCW (M10): the last line <= y whose LLLineSame bit is clear (0 if none), a word at a time */
static inline uint32 llr_linesrc(uint32 y)
{
   for (;;)
   {
      uint32 b = y & 31, z = ~LLLineSame[y >> 5] & (b == 31 ? 0xffffffffu : (2u << b) - 1);
      if (z)
      {
         /* highest set bit of z (ARC norm; libgcc's __clzsi2 was ~5-10% of a frame) */
         int nb;
         if ((int32) z < 0)
            return (y & ~31u) + 31;
         __asm__("norm %0,%1" : "=r"(nb) : "r"(z));
         return (y & ~31u) + 30 - nb;
      }
      if (y < 32)
         return 0;
      y = (y & ~31u) - 1;
   }
}
#endif
#ifdef LLR_SKIP
/* ---- -DLLR_SKIP (M8): lines whose inputs equal the previous frame's keep their LCD row ----
   A line is skipped when its LCD row holds this renderer's drawing of the previous frame (LLRSeen = last
   frame's stamp), the part's PPU state (llr_pkey) and the line's BG offsets / Mode 7 line are the same,
   the palette replay up to the line is the same (start palette + log prefix), no sprite covering the line
   now or then changed (llr_skip_obj), and no VRAM 16-byte block it reads (map rows, BG tiles, sprite
   tiles; Mode 7: all of 0-7fff) was written this frame or the previous one (LLVDirty / LLVDirtyP).
   -DLLR_SKIPCHK draws them anyway and compares with the LCD at the frame's end. */
#ifdef LL_LINELOG
extern uint32 LLLineSame[8];
#endif
unsigned int LLVDirty[128];
static unsigned int LLVDirtyP[128], LLVD[128];
static uint32 LLRSkipAll = 1, LLRSkipNext = 1, LLRStamp = 1, LLRDrawn, LLRScanOk, LLRScanBase, LLRRowGen;
static uint32 LLRCause;
#ifdef LLR_SKIPDORM
static uint32 LLRDormNow, LLRDormProbe, LLRDormLeft, LLRDormK = 16, LLRDormS, LLRDormN;
#endif
static uint32 LLRSeen[240];          /* stamp of the frame the line's LCD row was drawn by this renderer */
static uint8 LLRPPart[240];
static uint32 LLRPLine[240][4];
struct llr_pkey
{
   uint8 fr[0x34], mode, fs, fb, b3, osz, bright, m7h, m7v, m7r, mos, bgm[4];
   uint16 fc[3], obase, osel, bg[4][4];
};
static struct llr_pkey LLRPK[2][16];
static uint32 LLRPKn, LLRPar;
static uint32 LLRLDirty[8];
/* the palette, line by line: LLRVc/LLRVp = this / last frame's colours at line LLRSwY, LLRGm[y] = which
   colours differ at line y: bit 16 = entry 0 (backdrop), bit g = a differing entry among g*16..g*16+15
   (entry 0 not counted); LLRPalBad: the log was flushed mid-frame */
static uint32 LLRPPalN, LLRCA, LLRCB, LLRSwY, LLRGmask, LLRPalBad;
static uint16 LLRVc[256], LLRVp[256];
static uint8 LLRDn[17];
static uint32 LLRGm[240];
#define LLR_GBIT(i) ((i) ? (i) >> 4 : 16)
static void llr_skip_pv(uint16* v, uint32 i, uint32 c)
{
   uint32 was = LLRVc[i] != LLRVp[i], now, b = LLR_GBIT(i);
   v[i] = c;
   now = LLRVc[i] != LLRVp[i];
   if (was != now)
   {
      if (now)
         LLRDn[b]++, LLRGmask |= 1u << b;
      else if (!--LLRDn[b])
         LLRGmask &= ~(1u << b);
   }
}
static const LLPalEnt* LLRPPal = LLPalLogS[1];
static uint16 LLRPPalStart[256];
#ifdef LLR_SKIPLAZY
static struct llr_osnap { int16 h, v; uint16 name; uint8 vf, hf, pr, pal, sz, in; } __attribute__((aligned(4))) LLROSnap[128];
#else
static struct llr_osnap { int16 h, v; uint16 name; uint8 vf, hf, pr, pal, sz, in; } LLROSnap[128];
#endif
#if defined LLR_SKIPSTAT || defined LLR_SKIPCHK
static uint32 LLRSkipped, LLRSkipBad, LLRSkipF;
#endif
#ifdef LLR_SKIPCHK
static uint32 LLRChkRow[160][60];
static uint8 LLRChkF[160];
static uint32 LLRWhyN[10];
#endif
void LLRSkipReset(void)
{
   memset(LLVDirty, 0xff, sizeof LLVDirty);
   LLRSkipAll = LLRSkipNext = 1; LLRCause |= 1;
}
/* S9xStartScreenRefresh, before LLPalFlushLog: keep the frame's palette log */
static void llr_skip_log(void)
{
#ifdef LLR_SKIPOFF2
   return;
#endif
   LLRPPalN = LLPalN;
}
/* after LLPalFlushLog */
static void llr_skip_start(uint32 render)
{
#ifdef LLR_SKIPOFF2
   return;
#endif
   if (!render)
   {
      LLRSkipNext = 1; LLRCause |= 2;
      return;
   }
#ifdef LLR_SKIPDORM
   /* -DLLR_SKIPDORM (M10): a frame (other than the one recording after a sleep) that skipped under 1/8 of its lines puts the
      skip to sleep for LLRDormK frames (16, doubling to 64 while it keeps failing): no records, no checks,
      no log swap. A sleeping frame still steps the stamp (no stale LLRSeen match) and sets LLRSkipNext, so the
      first frame after it only records (as after a reset) and the next one decides again. llr_skip_obj keeps
      running while asleep (sprite snapshots must follow every change). */
   if (LLRDormNow)
   {
      if (--LLRDormLeft)
      {
         LLRStamp++;
         LLRSkipNext = 1;
         return;
      }
      LLRDormNow = 0;
      LLRDormProbe = 1;
   }
   else if (LLRDormProbe)
      LLRDormProbe = 0;      /* the frame after a sleep only recorded: no verdict */
   else
   {
      if (LLRDormS * 8 < LLRDormN)
      {
         LLRDormLeft = LLRDormK;
         LLRDormK = LLRDormK < 64 ? LLRDormK * 2 : 64;
         LLRDormNow = 1;
         LLRDormS = LLRDormN = 0;
         LLRStamp++;
         LLRSkipNext = 1;
         return;
      }
      LLRDormK = 16;
   }
   LLRDormS = LLRDormN = 0;
#endif
   LLRSkipAll = LLRSkipNext;
   LLRSkipNext = 0;
   LLRPPal = LLPalLog;
   LLPalLog = LLPalLog == LLPalLogS[0] ? LLPalLogS[1] : LLPalLogS[0];
   LLRStamp++;
   memcpy(LLVDirtyP, LLVDirty, sizeof LLVDirty);
   memset(LLVDirty, 0, sizeof LLVDirty);
   LLRPar ^= 1;
   LLRPKn = 0;
   LLRScanOk = 0;
   LLRDrawn = 0;
   memset(LLRLDirty, 0, sizeof LLRLDirty);
   {
      uint32 i;
      memcpy(LLRVc, LLRPalA, sizeof LLRVc);
      memcpy(LLRVp, LLRPPalStart, sizeof LLRVp);
      memset(LLRDn, 0, sizeof LLRDn);
      LLRGmask = 0;
      for (i = 0; i < 256; i++)
         if (LLRVc[i] != LLRVp[i])
            LLRDn[LLR_GBIT(i)]++, LLRGmask |= 1u << LLR_GBIT(i);
   }
   memcpy(LLRPPalStart, LLRPalA, sizeof LLRPPalStart);
   LLRCA = LLRCB = LLRSwY = LLRPalBad = 0;
}
/* the palette log overflowed (flushed mid-frame) or was reset */
static void llr_skip_palreset(void)
{
   LLRPalBad = 1;
   LLRSkipNext = 1; LLRCause |= 4;
}
static void llr_skip_mark(int v, int e)
{
   if (v < 0) v = 0;
   if (e > 240) e = 240;
#ifdef LLR_SKIPLAZY
   while (v < e)
   {
      uint32 b = v & 31, k = e - v < 32 - b ? e - v : 32 - b;
      LLRLDirty[v >> 5] |= (k == 32 ? 0xffffffffu : ((1u << k) - 1)) << b;
      v += k;
   }
#else
   for (; v < e; v++)
      LLRLDirty[v >> 5] |= 1u << (v & 31);
#endif
}
/* llr_objlists: sprites whose attributes changed since the last lists dirty their old and new lines */
static void llr_skip_obj(void)
{
   uint32 S;
   static uint8 in[128];
   int i, s;
   memset(in, 0, sizeof in);
   for (i = 0; (s = GFX.OBJList[i]) >= 0; i++)
      in[s] = 1;
   LLRScanOk = 0;
   if (LLRDrawn)
      LLRSkipNext = 1; LLRCause |= 8;      /* lines of this frame were drawn with the old ones */
   for (S = 0; S < 128; S++)
   {
      struct llr_osnap n, *o = &LLROSnap[S];
      const SOBJ* p = &PPU.OBJ[S];
#ifdef LLR_SKIPLAZY
      if (!in[S] && !o->in)
         continue;     /* in no list, then or now: nothing to mark (entering a list always differs in .in) */
#endif
      memset(&n, 0, sizeof n);
      n.h = p->HPos, n.v = GFX.VPositions[S], n.name = p->Name, n.vf = p->VFlip, n.hf = p->HFlip;
      n.pr = p->Priority, n.pal = p->Palette, n.sz = GFX.Sizes[S], n.in = in[S];
#ifdef LLR_SKIPLAZY
      if ((((const uint32*) &n)[0] ^ ((const uint32*) o)[0]) | (((const uint32*) &n)[1] ^ ((const uint32*) o)[1]) |
          (((const uint32*) &n)[2] ^ ((const uint32*) o)[2]))
#else
      if (memcmp(&n, o, sizeof n))
#endif
      {
         if (o->in)
            llr_skip_mark(o->v, o->v + o->sz);
         if (n.in)
            llr_skip_mark(n.v, n.v + n.sz);
         *o = n;
      }
   }
}
#define LLV_T(b) ((LLVD[((b) >> 5) & 127] >> ((b) & 31)) & 1)
/* a BG map row (both screens) and every tile it names: 1 if none was written */
/* the 33 (8x8) / 17 (16x16) map entries a line with H offset hofs reads from map row sl, as llr_bg_c, and
   the tiles they name (16x16: all four): 0x20000 | colour groups if no block was written, else the groups
   (any = 0: no VRAM block was written at all) */
static uint32 llr_skip_bgrow(const struct llr_layer* L, uint32 sl, uint32 hofs, uint32 any)
{
   const uint16** sc = (const uint16**) L->sc + ((sl & 0x20) ? 2 : 0);
   const uint16 *b1 = sc[0] + ((sl & 0x1f) << 5), *b2 = sc[1] + ((sl & 0x1f) << 5);
   uint32 sh = L->bpp == 2 ? 0 : L->bpp == 4 ? 1 : 2, g = 0, d = 0, k, n, q;
   uint32 nb = L->nbase, tm = L->tmask, ps = L->palstart, psh = L->palsh, pm = L->palmask;
   if (any)
   {
      uint32 a1 = ((const uint8*) b1 - Memory.VRAM) >> 4, a2 = ((const uint8*) b2 - Memory.VRAM) >> 4;
      d = LLV_T(a1) | LLV_T(a1 + 1) | LLV_T(a1 + 2) | LLV_T(a1 + 3) | LLV_T(a2) | LLV_T(a2 + 1) | LLV_T(a2 + 2) | LLV_T(a2 + 3);
   }
#ifdef LLR_SKIPLAZY
   if (d)
      return 0;
#endif
   if (!L->tile16)
   {
      q = (hofs & 0x1ff) >> 3, n = (hofs & 7) ? 33 : 32;
      for (k = 0; k < n; k++, q++)
      {
         uint32 e, t, j;
         q &= 63;
         e = q > 31 ? b2[q & 31] : b1[q];
         g |= 1u << ((ps + ((e >> psh) & pm)) >> 4);
         if (any && !d)
         {
            t = (((e & 0x3ff) + nb) & tm) << sh;
#ifdef LLR_SKIPLAZY
            /* the tile's 1 << sh blocks are aligned bits of one word; a written block ends the row's test
               (the caller only uses the colour groups of a clean row) */
            if ((LLVD[(t >> 5) & 127] >> (t & 31)) & ((1u << (1u << sh)) - 1))
               return 0;
#else
            for (j = 0; j < (1u << sh); j++)
               d |= LLV_T(t + j);
#endif
         }
      }
   }
   else
   {
      q = ((hofs & 0x3ff) >> 4), n = 17;
      for (k = 0; k < n; k++, q++)
      {
         uint32 e, o, j;
         q &= 63;
         e = q > 31 ? b2[q & 31] : b1[q];
         g |= 1u << ((ps + ((e >> psh) & pm)) >> 4);
         if (any && !d)
            for (o = 0; o < 4; o++)
            {
               uint32 t = ((((e + (o & 1) + (o & 2) * 8) & 0x3ff) + nb) & tm) << sh;
               for (j = 0; j < (1u << sh); j++)
                  d |= LLV_T(t + j);
            }
#ifdef LLR_SKIPLAZY
         if (d)
            return 0;
#endif
      }
   }
   if (L->bpp == 8)
      g = 0xffff;
   return d ? g : g | 0x20000;
}
/* ys[0..n): the part's shown lines; returns how many still have to be drawn (in ys) */
static uint32 llr_skip_lines(uint8* ys, uint32 n, uint32 y0, uint32 y1, const struct llr_layer* Ls, uint32 lay, uint32 objon)
{
   struct llr_pkey k;
   uint32 pi, i, l, m = 0, any = 0, m7d = 0, src, sy, prev = LLRStamp - 1;
   uint8 peq[16];
#ifdef LLR_SKIPOFF
   if (n) return n;
#endif
   static uint32 rows[4][64], rowh[4][64], rowg[4][64];
   memset(&k, 0, sizeof k);
   memcpy(k.fr, Memory.FillRAM + 0x2100, 0x34);
   memset(k.fr + 0x02, 0, 3);
   memset(k.fr + 0x0d, 0, 0x0d);
   memset(k.fr + 0x1b, 0, 8);
   k.mode = PPU.BGMode, k.fs = PPU.FirstSprite, k.fb = PPU.ForcedBlanking, k.b3 = PPU.BG3Priority;
   k.osz = PPU.OBJSizeSelect, k.bright = PPU.Brightness, k.m7h = PPU.Mode7HFlip, k.m7v = PPU.Mode7VFlip;
   k.m7r = PPU.Mode7Repeat, k.mos = PPU.Mosaic;
   k.fc[0] = PPU.FixedColourRed, k.fc[1] = PPU.FixedColourGreen, k.fc[2] = PPU.FixedColourBlue;
   k.obase = PPU.OBJNameBase, k.osel = PPU.OBJNameSelect;
   for (l = 0; l < 4; l++)
   {
      k.bgm[l] = PPU.BGMosaic[l];
      k.bg[l][0] = PPU.BG[l].SCBase, k.bg[l][1] = PPU.BG[l].SCSize, k.bg[l][2] = PPU.BG[l].NameBase;
      k.bg[l][3] = PPU.BG[l].BGSize;
   }
   if (LLRPKn && !memcmp(&LLRPK[LLRPar][LLRPKn - 1], &k, sizeof k))
      pi = LLRPKn - 1;
   else if (LLRPKn < 16)
      LLRPK[LLRPar][pi = LLRPKn++] = k;
   else
      pi = 255;
   LLRDrawn = 1;
#ifdef LLR_SKIPLAZY
   /* -DLLR_SKIPLAZY (M10): the per-line tests that need no VRAM / sprite / palette work first; when no line
      passes them (scrolling games: every frame), only the next frame's line records are kept and the costly
      part (VRAM dirty merge, sprite tile scan, palette replay, map rows) is left for a later part that needs
      it (all of it catches up: LLVD / LLRSwY / LLRScanOk are compared or replayed from where they stopped) */
   {
      static uint32 lks[240][4];
      uint32 cand = 0;
      prev = LLRStamp - 1;
#ifdef LL_LINELOG
#ifdef LLR_SRCW
      src = llr_linesrc(y0);
#else
      for (src = y0; src && (LLLineSame[src >> 5] & (1u << (src & 31))); src-- )
         ;
#endif
#endif
      sy = y0;
      for (i = 0; i < n; i++)
      {
         uint32 y = ys[i], *lk = lks[i];
#ifdef LL_LINELOG
         for (; sy <= y; sy++)
            if (!(LLLineSame[sy >> 5] & (1u << (sy & 31))))
               src = sy;
#else
         src = y;
#endif
         if (PPU.BGMode == 7)
         {
            memcpy(lk, &LineMatrixData[y], 12);
            memcpy(lk + 3, &LineData[y].BG[0], 4);
         }
         else
         {
            const uint32* w = (const uint32*) &LineData[src].BG[0];   /* aligned(16) in globals.c */
            lk[0] = w[0], lk[1] = w[1], lk[2] = w[2], lk[3] = w[3];
         }
         if (!cand && !LLRSkipAll && LLRSeen[y] == prev && pi != 255 && LLRPPart[y] != 255 &&
             !(LLRLDirty[y >> 5] & (1u << (y & 31))) &&
             !((lk[0] ^ LLRPLine[y][0]) | (lk[1] ^ LLRPLine[y][1]) | (lk[2] ^ LLRPLine[y][2]) | (lk[3] ^ LLRPLine[y][3])))
            cand = 1;
      }
      if (!cand)
      {
         for (i = 0; i < n; i++)
         {
            uint32 y = ys[i];
            LLRSeen[y] = LLRStamp;
            LLRPPart[y] = pi;
            LLRPLine[y][0] = lks[i][0], LLRPLine[y][1] = lks[i][1], LLRPLine[y][2] = lks[i][2], LLRPLine[y][3] = lks[i][3];
         }
         return n;
      }
   }
#endif
   /* the palette: replay both frames' logs up to y1 */
   for (; LLRSwY <= y1; LLRSwY++)
   {
      for (; LLRCA < LLPalN && LLPalLog[LLRCA].line <= LLRSwY; LLRCA++)
         llr_skip_pv(LLRVc, LLPalLog[LLRCA].i, LLPalLog[LLRCA].c);
      for (; LLRCB < LLRPPalN && LLRPPal[LLRCB].line <= LLRSwY; LLRCB++)
         llr_skip_pv(LLRVp, LLRPPal[LLRCB].i, LLRPPal[LLRCB].c);
      LLRGm[LLRSwY] = LLRPalBad ? 0x1ffff : LLRGmask;
   }
   for (i = 0; i < 128; i++)
   {
      uint32 v = LLVDirty[i] | LLVDirtyP[i];
      if (v != LLVD[i])
         LLRScanOk = 0, LLVD[i] = v;
      any |= v;
   }
   if (any)
   {
      /* sprites whose tiles were written: once per frame, again when the lists, VRAM or the name base change */
      if (objon && (!LLRScanOk || LLRScanBase != PPU.OBJNameBase))
      {
         LLRScanOk = 1, LLRScanBase = PPU.OBJNameBase;
         int j, S;
         for (j = 0; (S = GFX.OBJList[j]) >= 0; j++)
         {
            const SOBJ* o = &PPU.OBJ[S];
            uint32 nc = GFX.Sizes[S] >> 3, r, c, d = 0;
            for (r = 0; r < nc && !d; r++)
               for (c = 0; c < nc; c++)
               {
                  uint32 tn = ((PPU.OBJNameBase >> 5) + ((o->Name + (r << 4) + c) & 0x3ff)) & 0x7ff;
                  if (LLV_T(tn * 2) | LLV_T(tn * 2 + 1))
                  {
                     d = 1;
                     break;
                  }
               }
            if (d)
               llr_skip_mark(GFX.VPositions[S], GFX.VPositions[S] + GFX.Sizes[S]);
         }
      }
      if (PPU.BGMode == 7)
         for (i = 0; i < 64; i++)
            m7d |= LLVD[i];
   }
   memset(peq, 0, sizeof peq);
   LLRRowGen++;
#ifdef LL_LINELOG
#ifdef LLR_SRCW
   src = llr_linesrc(y0);
#else
   for (src = y0; src && (LLLineSame[src >> 5] & (1u << (src & 31))); src-- )
      ;
#endif
#else
   src = y0;
#endif
   sy = y0;
   for (i = 0; i < n; i++)
   {
      uint32 y = ys[i], lk[4], ok;
#ifdef LL_LINELOG
      for (; sy <= y; sy++)
         if (!(LLLineSame[sy >> 5] & (1u << (sy & 31))))
            src = sy;
#else
      src = y;
#endif
      if (PPU.BGMode == 7)
      {
         memcpy(lk, &LineMatrixData[y], 12);
         memcpy(lk + 3, &LineData[y].BG[0], 4);
      }
      else
#ifdef LLR_SKIPLAZY
      {
         const uint32* w = (const uint32*) &LineData[src].BG[0];   /* aligned(16) in globals.c */
         lk[0] = w[0], lk[1] = w[1], lk[2] = w[2], lk[3] = w[3];
      }
#else
         memcpy(lk, &LineData[src].BG[0], 16);
#endif
      ok = !LLRSkipAll && LLRSeen[y] == prev && pi != 255 && LLRPPart[y] != 255 && !m7d &&
           !(LLRLDirty[y >> 5] & (1u << (y & 31))) && !memcmp(lk, LLRPLine[y], 16);
#ifdef LLR_SKIPCHK
      {
         uint32 w = LLRSkipAll ? 0 : LLRSeen[y] != prev ? 1 : (pi == 255 || LLRPPart[y] == 255) ? 2 : 3 > 3 ? 3 :
                    m7d ? 4 : (LLRLDirty[y >> 5] & (1u << (y & 31))) ? 5 : memcmp(lk, LLRPLine[y], 16) ? 6 : 7;
         LLRWhyN[w]++;
         if (w == 7 && memcmp(&LLRPK[LLRPar ^ 1][LLRPPart[y]], &k, sizeof k)) LLRWhyN[8]++;
      }
#endif
      if (ok)
      {
         uint32 q = LLRPPart[y];
         if (!peq[q])
            peq[q] = memcmp(&LLRPK[LLRPar ^ 1][q], &k, sizeof k) ? 2 : 1;
         ok = peq[q] == 1;
      }
      if (ok && (any || LLRGm[y]))
      {
         uint32 used = 0x10000;
         if (PPU.BGMode == 7)
            used = 0x1ffff;
         else
            for (l = 0; l < 4 && ok; l++)
               if (lay & (1 << l))
               {
                  const struct llr_layer* L = &Ls[l];
                  uint32 vy = ((const uint16*) lk)[l * 2] + y, sl = (L->tile16 ? vy >> 4 : vy >> 3) & 63;
                  uint32 h = ((const uint16*) lk)[l * 2 + 1];
                  if (rowg[l][sl] != LLRRowGen || rowh[l][sl] != h)
                     rows[l][sl] = llr_skip_bgrow(L, sl, h, any) | 0x40000, rowh[l][sl] = h, rowg[l][sl] = LLRRowGen;
                  ok = (rows[l][sl] >> 17) == 3;
                  used |= rows[l][sl] & 0xffff;
               }
         if (ok && LLRGm[y])
         {
            if (objon)
            {
               int q;
               for (q = LLROOff[y]; q < (int) LLROOff[y + 1]; q++)
                  used |= 0x100u << PPU.OBJ[LLROPool[q]].Palette;
            }
            ok = !(used & LLRGm[y]);
#ifdef LLR_SKIPCHK
            if (!ok) LLRWhyN[3]++;
#endif
         }
      }
      LLRSeen[y] = LLRStamp;
      LLRPPart[y] = pi;
      memcpy(LLRPLine[y], lk, 16);
      if (!ok)
      {
         ys[m++] = y;
         continue;
      }
#if defined LLR_SKIPSTAT || defined LLR_SKIPCHK
      LLRSkipped++;
#endif
#ifdef LLR_SKIPCHK
      {
         uint32 oy = LLRRow[y], w;
         const uint8* p = LLR_DST(oy);
         for (w = 0; w < 60; w++)
            __asm__ volatile("ld.di %0,[%1]" : "=r"(LLRChkRow[oy][w]) : "r"(p + w * 4));
         LLRChkF[oy] = 1;
      }
      ys[m++] = y;
#endif
   }
   return m;
}
#if defined LLR_SKIPSTAT || defined LLR_SKIPCHK
/* S9xEndScreenRefresh */
static void llr_skip_end(void)
{
#ifdef LLR_SKIPCHK
   uint32 oy, w, v, bad = 0;
   for (oy = 0; oy < 160; oy++)
      if (LLRChkF[oy])
      {
         const uint8* p = LLR_DST(oy);
         LLRChkF[oy] = 0;
         for (w = 0; w < 60; w++)
         {
            __asm__ volatile("ld.di %0,[%1]" : "=r"(v) : "r"(p + w * 4));
            if (v != LLRChkRow[oy][w])
            {
               if (bad++ < 4)
                  printf("SKIPBAD f %u row %u\n", LLRSkipF, oy);
               break;
            }
         }
      }
   LLRSkipBad += bad;
#endif
#ifdef LLR_SKIPCHK
   printf("C %x\n", LLRCause); LLRCause = 0;
   printf("W %u %u %u %u %u %u %u %u %u\n", LLRWhyN[0], LLRWhyN[1], LLRWhyN[2], LLRWhyN[3], LLRWhyN[4], LLRWhyN[5], LLRWhyN[6], LLRWhyN[7], LLRWhyN[8]);
   memset(LLRWhyN, 0, sizeof LLRWhyN);
#endif
   printf("K %u %u %u\n", LLRSkipF++, LLRSkipped, LLRSkipBad);
   LLRSkipped = 0;
}
#endif
#endif

#ifdef LL_WINLINE
/* -DLL_WINLINE (A2): a window position write ($2126-$2129) doesn't end the drawn part when only sprites can use
   that window there (no BG has it enabled in W12SEL/W34SEL; the colour window isn't drawn without transparency)
   and the new renderer draws the part: RenderLine keeps every line's four positions (LLWinPos), and llr_part
   builds the OBJ window mask of each line from its own positions (ComputeClipWindows with them), i.e. what the
   flush at each write gave (a flush draws the lines whose RenderLine ran before the write with the old values).
   Mario Kart: the split-screen OBJ window edges are H-DMA'd (18 of its 27 parts per frame). */
uint32 LLWinPos[240];
static uint32 LLWinVar, LLWinVarCur;
uint32 LLWinBad;
static uint8 LLRWM[240][LBW] __attribute__((aligned(32)));
static const uint8* LLRLM[240];
/* the OBJ window masks of the part's lines; main = OBJ on the main screen (Clip[0]) */
static void llr_winmasks(const uint8* ys, uint32 n, uint32 main)
{
   uint32 keys[240], nk = 0, k, j, last = 0, cur;
   const uint8* lm = 0;
   ClipData* c = main ? &IPPU.Clip[0] : &IPPU.Clip[1];
   uint8 a = PPU.Window1Left, b = PPU.Window1Right, d = PPU.Window2Left, e = PPU.Window2Right;
   for (j = 0; j < n; j++)
   {
      cur = LLWinPos[ys[j]];
      if (!lm || cur != last)
      {
         for (k = 0; k < nk && keys[k] != cur; k++)
            ;
         if (k == nk)
         {
            uint8* m = LLRWM[nk];
            keys[nk++] = cur;
            PPU.Window1Left = cur, PPU.Window1Right = cur >> 8, PPU.Window2Left = cur >> 16, PPU.Window2Right = cur >> 24;
            ComputeClipWindows();
            if (c->Count[4])
            {
               llr_masks(4, c);
               memcpy(m, LLRMask[4], LBW);
            }
            else
            {
               memset(m, 0, LBW);
               memset(m + LBO, 1, 160);
            }
         }
         lm = LLRWM[k], last = cur;
      }
      LLRLM[ys[j]] = lm;
   }
   if (nk)
   {
      PPU.Window1Left = a, PPU.Window1Right = b, PPU.Window2Left = d, PPU.Window2Right = e;
      ComputeClipWindows();
   }
}
#define LLR_LM(yy) (LLWinVarCur ? LLRLM[yy] : objmask)
#else
#define LLR_LM(yy) objmask
#endif
LLR_NI static void llr_part(uint32 y0, uint32 y1)
{
   static struct llr_layer Ls[4];
   static struct llr_run runs[4][LLR_CH];
   static uint8 ys[240];
   uint32 nruns[4];
   uint8 steps[20][2];
#ifdef LL_LINELOG
   uint8 lsrc[LLR_CH];
   uint32 sy, src;
#endif
   uint32 nsteps = 0, n = 0, y, l, i, mode = PPU.BGMode, back = 0, lit = 0, objon, objmix = 0;
   uint8 r212c = Memory.FillRAM[0x212c], r212d = Memory.FillRAM[0x212d], subadd = Memory.FillRAM[0x2131] & 0x3f;
   const uint8* objmask = 0;
   uint8 code[18];

   for (y = y0; y <= y1; y++)
      if (LLRRow[y] != 255)
         ys[n++] = y;
   if (!n)
      return;
   if (!LLRPix[0][1])
      llr_init();
   LLRStat[0]++;

   /* backdrop: CG entry 0 (index 0 in the line buffers) or a literal colour */
   if (PPU.ForcedBlanking)
      lit = 1, back = 0;
   else if (!(Memory.FillRAM[0x2131] & 0x80) && (Memory.FillRAM[0x2131] & 0x20) &&
            (PPU.FixedColourRed || PPU.FixedColourGreen || PPU.FixedColourBlue))
      lit = 1, back = BUILD_PIXEL(IPPU.XB[PPU.FixedColourRed], IPPU.XB[PPU.FixedColourGreen], IPPU.XB[PPU.FixedColourBlue]);

   /* the layers and their order (the old renderer's depths, back to front) */
   memset(code, 0xff, sizeof code);
   objon = 0;
   if (!PPU.ForcedBlanking)
   {
      static const uint8 z01[4][2] = { {10, 14}, {9, 13}, {3, 6}, {2, 5} }, z3[2][2] = { {5, 13}, {2, 9} };
      uint32 nbg = mode == 0 ? 4 : mode == 1 ? 3 : mode == 7 ? 0 : 2;
#define LLR_DISP(b) ((r212c & (b)) || ((r212d & (b)) && subadd))
      for (l = 0; l < nbg; l++)
      {
         struct llr_layer* L = &Ls[l];
         const ClipData* c = (r212c & (1 << l)) ? &IPPU.Clip[0] : &IPPU.Clip[1];
         uint32 sc, z0, z1;
         if (!LLR_DISP(1 << l))
            continue;
         if (c->Count[l])
         {
            llr_masks(l, c);
            L->mask = LLRMask[l];
         }
         else
            L->mask = 0;
         L->bpp = BitShifts[mode][l];
         L->depth = L->bpp == 2 ? TILE_2BIT : L->bpp == 4 ? TILE_4BIT : TILE_8BIT;
         L->tile16 = PPU.BG[l].BGSize;
         L->tmask = 0xffff >> TileShifts[mode][l];
         L->nbase = ((PPU.BG[l].NameBase << 1) & 0xffff) >> TileShifts[mode][l];
         L->palstart = mode == 0 ? l << 5 : 0;
         L->palsh = 10 - PaletteShifts[mode][l];
         L->palmask = PaletteMasks[mode][l] << PaletteShifts[mode][l];
         L->cached = IPPU.TileCached[L->depth];
         L->nib = L->bpp == 4 ? LLNib4 : LLNib2;
         L->cache8 = IPPU.TileCache[TILE_8BIT];
#ifdef LLR_MODE2
         L->opt = mode == 2 && llr_opt_on(1u << (13 + l), y0, y1 + 1);
         if (l < 2)
            LLROptC[l].ok = 0;
#endif
         sc = PPU.BG[l].SCBase << 1;
         {
            const uint16 *s0 = llr_scaddr(sc & 0xffff), *s1, *s2, *s3;
            /* as DrawBackground_8/16 (SC1 wraps, SC2/SC3 come back 64 KB) */
            s1 = (PPU.BG[l].SCSize & 1) ? s0 + 1024 : s0;
            if (s1 >= (const uint16*)(Memory.VRAM + 0x10000))
               s1 = (const uint16*) &Memory.VRAM[((const uint8*) s1 - Memory.VRAM) % 0x10000];
            s2 = (PPU.BG[l].SCSize & 2) ? s1 + 1024 : s0;
            if (((const uint8*) s2 - Memory.VRAM) >= 0x10000)
               s2 -= 0x8000;
            s3 = (PPU.BG[l].SCSize & 1) ? s2 + 1024 : s2;
            if (((const uint8*) s3 - Memory.VRAM) >= 0x10000)
               s3 -= 0x8000;
            L->sc[0] = s0, L->sc[1] = s1, L->sc[2] = s2, L->sc[3] = s3;
         }
         if (mode <= 1)
         {
            z0 = z01[l][0];
            z1 = (l == 2 && PPU.BG3Priority) ? 17 : z01[l][1];
         }
         else
            z0 = z3[l][0], z1 = z3[l][1];
         if (!L->mask || memchr(L->mask + LBO, 1, 160))
            code[z0] = l * 2, code[z1] = l * 2 + 1;
      }
#ifdef LLR_M7
      if (mode == 7)
      {
         code[0] = 12;             /* the Mode 7 layer, under every sprite */
#ifdef LL_LINELOG
         LLLineMat(y1 + 1);
#endif
      }
#endif
      if (LLR_DISP(16))
      {
         const ClipData* c = (r212c & 16) ? &IPPU.Clip[0] : &IPPU.Clip[1];
         objon = 1;
#ifdef LL_WINLINE
         if (LLWinVarCur)
         {
            llr_winmasks(ys, n, r212c & 16);
            objmask = LLRMask[4];   /* non-zero: the masked sprite paths; each line uses LLRLM[y] */
         }
         else
#endif
         if (c->Count[4])
         {
            llr_masks(4, c);
            objmask = LLRMask[4];
            if (!memchr(objmask + LBO, 1, 160))
               objon = 0;
         }
         if (objon)
            code[4] = 8, code[8] = 9, code[12] = 10, code[16] = 11;
      }
   }
   for (i = 0; i < 18; i++)
      if (code[i] != 0xff)
         steps[nsteps][0] = code[i] >> 1, steps[nsteps++][1] = code[i] & 1;
   if (objon && LLROBJDirty)
      llr_objlists();
#ifdef LLR_SKIP
   {
      uint32 lay = 0;
      for (i = 0; i < nsteps; i++)
         if (steps[i][0] < 4)
            lay |= 1u << steps[i][0];
#ifdef LLR_SKIPDORM
      if (!LLRDormNow)
      {
         uint32 n0 = n;
         n = llr_skip_lines(ys, n, y0, y1, Ls, lay, objon);
         LLRDormN += n0, LLRDormS += n0 - n;
      }
#else
      n = llr_skip_lines(ys, n, y0, y1, Ls, lay, objon);
#endif
      if (!n)
         return;
   }
#endif

#ifdef LL_LINELOG
   /* the line LineData holds y0's offsets in (lines with their LLLineSame bit set weren't written) */
#ifdef LLR_SRCW
   src = llr_linesrc(y0);
#else
   for (src = y0; src && (LLLineSame[src >> 5] & (1u << (src & 31))); src-- )
      ;
#endif
   sy = y0;
#endif
   for (i = 0; i < n; i += LLR_CH)
   {
      uint32 cn = n - i < LLR_CH ? n - i : LLR_CH, j, s;
      const uint8* cy = ys + i;
#ifdef LLR_M7F
      /* A3: Mode 7 lines without sprites go straight to the LCD when the palette is the same on all the chunk's
         lines (log replayed to its first line now, as the output loop below would; no entry after that) */
      uint32 fused = 0, m7f = 0;
      if (mode == 7 && !PPU.ForcedBlanking)
      {
         while (LLRPalPos < LLPalN && LLPalLog[LLRPalPos].line <= cy[0])
         {
            LLRPalSet(LLPalLog[LLRPalPos].i, LLPalLog[LLRPalPos].c);
            LLRPalPos++;
         }
         m7f = !(LLRPalPos < LLPalN && LLPalLog[LLRPalPos].line <= cy[cn - 1]);
      }
#endif
#ifdef LLR_M7FDBG
      { extern unsigned frame; extern int printf(const char*, ...); if (frame == LLR_M7FDBG) { uint32 q, ns = 0; for (q = 0; q < cn; q++) ns += objon && LLROPri[cy[q]]; printf("M7F y%u cn%u mode%u lit%u m7f%u spr%u paln%u pos%u%c", cy[0], cn, mode, lit, m7f, ns, LLPalN, LLRPalPos, 10); } }
#endif
#ifdef LL_LINELOG
      for (j = 0; j < cn; j++)
      {
         for (; sy <= cy[j]; sy++)
            if (!(LLLineSame[sy >> 5] & (1u << (sy & 31))))
               src = sy;
         lsrc[j] = src;
      }
#endif
#ifndef LLR_ZEROED
      for (j = 0; j < cn; j++)
         memset(LLRLb[j] + LBO, 0, 160);
#endif
      memset(nruns, 0, sizeof nruns);
      /* sprites: lines with mixed priorities resolved first */
      objmix = 0;
#if defined LLR_OBJCH && defined LLR_OBJASM
      if (objon && !objmask)
      {
         /* the sprites that touch the chunk's lines, reverse OBJList order */
         int k, lo = cy[0], hi = cy[cn - 1];
         LLRChN = 0;
#ifdef LLR_CHMASK
         {
            uint32 m[4] = {0, 0, 0, 0}, w;
            for (k = 0; k < (int) cn; k++)
            {
               uint32 q, qe = LLROOff[cy[k] + 1];
               for (q = LLROOff[cy[k]]; q < qe; q++)
               {
                  uint32 x = LLRObjIdx[LLROPool[q]];
                  m[x >> 5] |= 1u << (x & 31);
               }
            }
            for (w = 4; w-- > 0;)
               while (m[w])
               {
                  int b = 31 - __builtin_clz(m[w]);
                  LLRChS[LLRChN++] = GFX.OBJList[w * 32 + b];
                  m[w] &= ~(1u << b);
               }
         }
         if (0)
#endif
         for (k = (int) LLRONum - 1; k >= 0; k--)
         {
#ifdef LLR_CHSEL
            if (LLRChV[k] <= hi && LLRChE[k] > lo)
               LLRChS[LLRChN++] = GFX.OBJList[k];
#else
            uint32 S = GFX.OBJList[k];
            int v = GFX.VPositions[S];
            if (v <= hi && v + (int) GFX.Sizes[S] > lo)
               LLRChS[LLRChN++] = S;
#endif
         }
      }
#endif
      if (objon)
         for (j = 0; j < cn; j++)
         {
            uint32 yy = cy[j], p = LLROPri[yy];
            if (p & (p - 1))
            {
               int k;
               uint32 lo = 160, hi = 0;
               objmix |= 1 << j;
               /* the columns the line's sprites cover */
               for (k = LLROOff[yy]; k < (int) LLROOff[yy + 1]; k++)
               {
                  uint32 S = LLROPool[k], cl, ch;
                  int x = PPU.OBJ[S].HPos, e = x + (int) GFX.Sizes[S];
                  if (x < 0) x = 0;
                  if (e > 256) e = 256;
                  cl = (x >> 3) * 5, ch = ((e + 7) >> 3) * 5;
                  if (cl < lo) lo = cl;
                  if (ch > hi) hi = ch;
               }
               if (hi <= lo)
                  lo = hi = 0;
               LLRObjLo[j] = lo, LLRObjHi[j] = hi;
               memset(LLRObj[j] + LBO + lo, 0, hi - lo);
               for (k = LLROOff[yy + 1] - 1; k >= (int) LLROOff[yy]; k--)
                  llr_obj_row(LLRObj[j] + LBO, LLRObjP[j] + LBO, LLROPool[k], yy, LLR_LM(yy));
            }
         }
      for (s = 0; s < nsteps; s++)
      {
         uint32 ly = steps[s][0], p = steps[s][1];
         if (ly < 4)
         {
            struct llr_layer* L = &Ls[ly];
            struct llr_run* R = runs[ly];
            uint32 nr = nruns[ly], k;
#ifdef LLR_MODE2
            if (!nr && L->opt)
            {
               /* offset-per-tile: one run per line with its own column entries */
               for (j = 0; j < cn; j++)
               {
                  struct llr_run* r;
#ifdef LL_LINELOG
                  uint32 src = lsrc[j];
#else
                  uint32 src = cy[j];
#endif
                  int vary = llr_opt_line(L, ly, j, cy[j], src);
                  uint32 h = LineData[src].BG[ly].HOffset;
                  if (nr && !vary && !R[nr - 1].optr && ((R[nr - 1].hofs ^ h) & 7) == 0 &&
                      !memcmp(R[nr - 1].ent, LLROptE[ly][j], 33 * sizeof(uint16)))
                  {
                     /* same column entries, one row each: the previous line's run */
                     r = &R[nr - 1];
                     r->j[r->n] = j;
                     r->r[r->n++] = LLROptR[ly][j][0];
                     continue;
                  }
                  r = &R[nr++];
                  r->ent = LLROptE[ly][j];
                  r->optr = vary ? LLROptR[ly][j] : 0;
                  r->hofs = LineData[src].BG[ly].HOffset;
                  r->key = ~0u;
                  r->txi = 0;
                  r->n = 1;
                  r->nhi = 1;
                  r->j[0] = j;
                  r->r[0] = LLROptR[ly][j][0];
               }
               nruns[ly] = nr;
            }
#endif
            if (!nr)
            {
               /* runs: consecutive lines on the same map row with the same H scroll */
               for (j = 0; j < cn; j++)
               {
#ifdef LL_LINELOG
                  uint32 yy = cy[j], vy = LineData[lsrc[j]].BG[ly].VOffset + yy, h = LineData[lsrc[j]].BG[ly].HOffset;
#else
                  uint32 yy = cy[j], vy = LineData[yy].BG[ly].VOffset + yy, h = LineData[yy].BG[ly].HOffset;
#endif
                  uint32 key = (vy >> 3) & 127;
                  if (!nr || R[nr - 1].key != key || R[nr - 1].hofs != h)
                  {
                     struct llr_run* r = &R[nr++];
                     uint32 sl = L->tile16 ? vy >> 4 : vy >> 3;
                     const uint16** sc = (const uint16**) L->sc + ((sl & 0x20) ? 2 : 0);
                     r->b1 = sc[0] + ((sl & 0x1f) << 5);
                     r->b2 = sc[1] + ((sl & 0x1f) << 5);
                     r->hofs = h;
                     r->key = key;
                     r->txi = L->tile16 ? (((vy & 15) <= 7) << 3) : 0;
                     r->n = 0;
                     r->nhi = 1;
#ifdef LLR_MODE2
                     r->ent = 0;
                     r->optr = 0;
#endif
                  }
                  R[nr - 1].j[R[nr - 1].n] = j;
                  R[nr - 1].r[R[nr - 1].n++] = vy & 7;
               }
               nruns[ly] = nr;
            }
            for (k = 0; k < nr; k++)
               llr_bg_run(L, &R[k], p ? 0x2000 : 0);
         }
#ifdef LLR_M7
         else if (ly == 6)
         {
#ifdef LLR_M7F
            uint16 pa0 = LLRPalA[0];
            uint32 pb0 = LLRPalBv[0];
            if (m7f && lit)
               LLRPalSet(0, back);   /* the literal backdrop, as the output loop sets it per line */
#endif
            for (j = 0; j < cn; j++)
            {
#ifdef LLR_M7F
               if (m7f && !(objon && LLROPri[cy[j]]) && llr_m7_line(cy[j], 0, LLR_DST(LLRRow[cy[j]])))
               {
                  fused |= 1u << j;
                  continue;
               }
#endif
               llr_m7_line(cy[j], LLRLb[j] + LBO, 0);
            }
#ifdef LLR_M7F
            LLRPalA[0] = pa0, LLRPalBv[0] = pb0;
#endif
         }
#endif
         else
         {
            /* sprites of priority (ly - 4) * 2 + p */
            uint32 pr = (ly - 4) * 2 + p;
#if defined LLR_OBJCH && defined LLR_OBJASM
            if (!objmask)
            {
               uint32 lm = 0;
               for (j = 0; j < cn; j++)
               {
                  uint32 pm = LLROPri[cy[j]];
                  if (objmix & (1 << j))
                  {
                     if (pm & (1u << pr))
                        llr_mixc(LLRLb[j] + LBO, LLRObj[j] + LBO, LLRObjP[j] + LBO, LLRObjLo[j], LLRObjHi[j], pr);
                  }
                  else if (pm == 1u << pr)
                     lm |= 1u << j;
               }
               if (lm)
                  llr_obj_chunk(cy, cn, lm, pr);
               continue;
            }
#endif
            for (j = 0; j < cn; j++)
            {
               uint32 yy = cy[j], pm = LLROPri[yy];
               if (!pm)
                  continue;
               if (objmix & (1 << j))
               {
                  uint8 *d = LLRLb[j] + LBO, *o = LLRObj[j] + LBO, *op = LLRObjP[j] + LBO;
                  uint32 c;
                  if (!(pm & (1u << pr)))
                     continue;
                  llr_mixc(d, o, op, LLRObjLo[j], LLRObjHi[j], pr);
               }
               else if (pm == 1u << pr)
               {
                  int k;
                  for (k = LLROOff[yy + 1] - 1; k >= (int) LLROOff[yy]; k--)
                     llr_obj_row(LLRLb[j] + LBO, 0, LLROPool[k], yy, LLR_LM(yy));
               }
            }
         }
      }
      /* output: the palette as it was on each line (gfx.c's log), the backdrop */
#ifdef LLR_DRIPON
      llr_drip_flush();
      while (LLRPalPos < LLPalN && LLPalLog[LLRPalPos].line <= cy[0])
      {
         LLRPalSet(LLPalLog[LLRPalPos].i, LLPalLog[LLRPalPos].c);
         LLRPalPos++;
      }
      if (i + cn < n && LLRRow[cy[cn - 1]] == LLRRow[cy[0]] + cn - 1 &&
          !(LLRPalPos < LLPalN && LLPalLog[LLRPalPos].line <= cy[cn - 1]))
      {
         /* drip this chunk during the next one's drawing; the next one draws into the other set */
         if (lit)
         {
            LLRDa = LLRPalA[0], LLRDb = LLRPalBv[0], LLRDlit = 1;
            LLRPalSet(0, back);
         }
         LLRDLb = LLRLb, LLRDl = 0, LLRDn = cn;
#ifdef LLR_DSTAT
         LLRStat[2]++;
#endif
         LLRK.ds = LLRK.de = LLRStage, LLRK.dd = (uint32*) LLR_DST(LLRRow[cy[0]]);
         LLRLb = LLRLb == LLRLbS[0] ? LLRLbS[1] : LLRLbS[0];
         continue;
      }
#endif
      for (j = 0; j < cn; j++)
      {
         uint32 yy = cy[j], oy = LLRRow[yy];
         while (LLRPalPos < LLPalN && LLPalLog[LLRPalPos].line <= yy)
         {
            LLRPalSet(LLPalLog[LLRPalPos].i, LLPalLog[LLRPalPos].c);
            LLRPalPos++;
         }
#ifdef LLR_M7F
         if (fused & (1u << j))
            continue;
#endif
         if (lit)
         {
            uint16 a = LLRPalA[0];
            uint32 b = LLRPalBv[0];
            LLRPalSet(0, back);
            LLR_OUT(oy, LLRLb[j] + LBO);
            LLRPalA[0] = a, LLRPalBv[0] = b;
         }
         else
            LLR_OUT(oy, LLRLb[j] + LBO);
      }
   }
}

#ifdef LL_LINELOG
extern uint32 LLLineSame[8];
void LLLineMat(uint32 end);
#define LLR_MAT() LLLineMat(IPPU.CurrentLine < 240 ? IPPU.CurrentLine : 240)
#else
#define LLR_MAT()
#endif

#ifdef LLR_MODE2
/* -DLLR_MODE2: Mode 2 parts whose offset-per-tile entries are all off are drawn like Mode 3 with 4-bit BGs
   (gfx.c DrawBackgroundOffset then reads every tile with the line's own offsets). Checked per pending line:
   every BG3 map entry DrawBackgroundOffset can read for the line (both 32-entry rows of the H row and of
   the V row) must have the enable bit of each shown BG clear. */
__attribute__((noinline)) static int llr_opt_on(uint32 en, uint32 y0, uint32 y1)
{
   uint32 y, last = ~0u;
   uint16 *BPS0, *BPS1, *BPS2, *BPS3;
   BPS0 = (uint16*) &Memory.VRAM[PPU.BG[2].SCBase << 1];
   BPS1 = (PPU.BG[2].SCSize & 1) ? BPS0 + 1024 : BPS0;
   BPS2 = (PPU.BG[2].SCSize & 2) ? BPS1 + 1024 : BPS0;
   BPS3 = (PPU.BG[2].SCSize & 1) ? BPS2 + 1024 : BPS2;
   if (y1 > (uint32) PPU.ScreenHeight)
      y1 = PPU.ScreenHeight;
   for (y = y0; y < y1; y++)
   {
#if defined LL_LINELOG && defined LLR_SRCW
      uint32 ls = llr_linesrc(y);
#else
      uint32 ls = y;
#endif
      uint32 sl = ((uint32) LineData[ls].BG[2].VOffset - 1) >> 3, i;
      uint16 *s1, *s2;
      int vo;
      if ((sl & 0x3f) == last)
         continue;
      last = sl & 0x3f;
      if (sl & 0x20)
         s1 = BPS2, s2 = BPS3;
      else
         s1 = BPS0, s2 = BPS1;
      s1 += (sl & 0x1f) << 5;
      s2 += (sl & 0x1f) << 5;
      if ((sl & 0x1f) == 0x1f)
         vo = (sl & 0x20) ? BPS0 - BPS2 - 0x1f * 32 : BPS2 - BPS0 - 0x1f * 32;
      else
         vo = 32;
      for (i = 0; i < 32; i++)
         if ((READ_2BYTES(s1 + i) | READ_2BYTES(s2 + i) | READ_2BYTES(s1 + vo + i) | READ_2BYTES(s2 + vo + i)) & en)
            return 1;
   }
   return 0;
}
#endif

/* parts the new renderer can't draw go to the old one */
static uint32 LLRWhy;   /* fallback reasons this frame: 1 transparency/interlace/hi-res, 2 mode, 4 direct colour, 8 mosaic */
static int llr_fallback(void)
{
   uint32 mode = PPU.BGMode, l;
   uint8 r212c = Memory.FillRAM[0x212c], r212d = Memory.FillRAM[0x212d], subadd = Memory.FillRAM[0x2131] & 0x3f;
   if (Settings.Transparency || IPPU.LatchedInterlace || IPPU.DoubleWidthPixels)
      return LLRWhy |= 1;
   if (PPU.ForcedBlanking)
      return 0;
#ifdef LLR_M7
   if (mode == 7)
      return (Memory.FillRAM[0x2130] & 1) ? (LLRWhy |= 4) : 0;
#endif
#ifdef LLR_MODE2
   if (mode == 2)
   {
      /* offset-per-tile with a window on that BG: the old renderer (a clipped tile reads other entries) */
      for (l = 0; l < 2; l++)
         if (LLR_DISP(1 << l) && (IPPU.Clip[0].Count[l] || IPPU.Clip[1].Count[l]) &&
             llr_opt_on(1u << (13 + l), IPPU.PreviousLine, IPPU.CurrentLine))
            return LLRWhy |= 2;
   }
   else
#endif
   if (mode != 0 && mode != 1 && mode != 3)
      return LLRWhy |= 2;
   if (mode == 3 && (Memory.FillRAM[0x2130] & 1) && LLR_DISP(1))
      return LLRWhy |= 4;
   for (l = 0; l < (mode == 0 ? 4u : mode == 1 ? 3u : 2u); l++)
      if (LLR_DISP(1 << l) && PPU.BGMosaic[l] && PPU.Mosaic > 1)
         return LLRWhy |= 8;
   return 0;
}

#ifdef LL_WINLAZY
/* -DLL_WINLAZY (M10): a window position write ($2126-$2129, window w = 1/2) ends the drawn part only when the
   window can change a pending line (PreviousLine..CurrentLine-1) of the new renderer: no BG has the window
   enabled (W12SEL/W34SEL; the colour window is not drawn without transparency), and if OBJ has it, no sprite is
   on a pending shown line (the sprite lists are current). Mega Man 7 / Mario Kart: HDMA window edges every line. */
int LLWinFlush(uint32 w)
{
   uint32 m = w == 1 ? 0x22 : 0x88, y, e = IPPU.CurrentLine, why = LLRWhy;
   int fb;
   if (IPPU.PreviousLine >= e)
      return 0;
   if ((Memory.FillRAM[0x2123] | Memory.FillRAM[0x2124]) & m)
      return 1;
   fb = llr_fallback();
   LLRWhy = why;
   if (fb || LLShowH != (PPU.ScreenHeight | Settings.Transparency << 16))
      return 1;
   if (!(Memory.FillRAM[0x2125] & m & 0x0f) || PPU.ForcedBlanking)
      return 0;
   if (IPPU.OBJChanged || LLROBJDirty)
      return 1;
   if (e > 240)
      e = 240;
   for (y = IPPU.PreviousLine; y < e; y++)
      if (LLRRow[y] != 255 && LLROPri[y])
         return 1;
   return 0;
}
#endif
#ifdef LL_WINLINE
int LLWinLine(uint32 w)
{
#ifdef LL_WINDBG
   { extern unsigned frame; extern int printf(const char*, ...); static int wn; if (frame >= LL_WINDBG && wn++ < 40) printf("WL f%u w%u 2123 %02x 2124 %02x 2125 %02x 212e %02x cl %u pl %u%c", frame, w, Memory.FillRAM[0x2123], Memory.FillRAM[0x2124], Memory.FillRAM[0x2125], Memory.FillRAM[0x212e], IPPU.CurrentLine, IPPU.PreviousLine, 10); }
#endif
   uint32 m = w == 1 ? 0x22 : 0x88, why = LLRWhy;
   int fb;
   if (IPPU.PreviousLine >= IPPU.CurrentLine)
      return 0;
   /* BGs with the window, or the colour window using it (it clips every layer when $2130 clips the main screen) */
   if (((Memory.FillRAM[0x2123] | Memory.FillRAM[0x2124]) & m) || (Memory.FillRAM[0x2125] & m & 0xf0))
      return 1;
   fb = llr_fallback();
   LLRWhy = why;
   if (fb || LLShowH != (PPU.ScreenHeight | Settings.Transparency << 16))
      return 1;
   LLWinVar = 1;
   return 0;
}
#endif
#ifdef LLR_KDBG
unsigned LLRDbgF;
#endif
#ifdef LLR_PTIME
/* -DLLR_PTIME: S9xUpdateScreen cycles per part size (bench clock register), printed per frame from LLR_PTIME */
static void llr_upd(void);
void S9xUpdateScreen(void)
{
   extern unsigned frame; extern int printf(const char*, ...);
   static unsigned f, n[4], c[4];
   uint32 y0 = IPPU.PreviousLine, end = IPPU.CurrentLine, t0, k;
   if (end > (uint32) PPU.ScreenHeight)
      end = PPU.ScreenHeight;
   k = end <= y0 ? 0 : end - y0 == 1 ? 1 : end - y0 < 16 ? 2 : 3;
   if (frame != f)
   {
      if (frame >= LLR_PTIME && frame < LLR_PTIME + 8)
         printf("PT f%u empty %u/%u one %u/%u few %u/%u many %u/%u\n", f, n[0], c[0], n[1], c[1], n[2], c[2], n[3], c[3]);
      f = frame; memset(n, 0, sizeof n); memset(c, 0, sizeof c);
   }
   t0 = *(volatile uint32*) 0x0180FF20;
   llr_upd();
   n[k]++, c[k] += *(volatile uint32*) 0x0180FF20 - t0;
}
static void llr_upd(void)
#else
void S9xUpdateScreen(void)
#endif
{
   uint32 y0 = IPPU.PreviousLine, end = IPPU.CurrentLine;
#ifdef LL_BR_ON
   if (LLBrPend && !(PPU.ForcedBlanking && !Settings.Transparency && !IPPU.LatchedInterlace && !IPPU.DoubleWidthPixels))
      S9xFixColourBrightness();
#endif
   if (end > (uint32) PPU.ScreenHeight)
      end = PPU.ScreenHeight;
#ifdef LL_UPDCOUNT
   LLUpdCalls++; LLUpdLines += end > y0 ? end - y0 : 0; LLUpdHist[LLUpdReg & 0xff]++; LLUpdReg = 0x21ff;
#endif
#ifdef LLR_CHECK
   LLR_MAT();
   LLOldUpdateScreen();
   if (llr_fallback())
   {
      LLRStat[1]++;
      return;
   }
#else
#ifdef LLR_PDBG
   {
      extern int printf(const char*, ...); extern unsigned frame; static int n;
      if (frame >= LLR_PDBG && frame <= LLR_PDBG + 1 && n++ < 400)
         printf("P f%d %u-%u mode %d dw %d bg %02x/%02x fb %d\n", (int) frame, y0, end, PPU.BGMode, IPPU.DoubleWidthPixels, Memory.FillRAM[0x212c], Memory.FillRAM[0x212d], PPU.ForcedBlanking);
   }
#endif
   if (LLShowH != (PPU.ScreenHeight | Settings.Transparency << 16))
      LLSetShow();
#ifdef LLR_FBSTAT
   /* -DLLR_FBSTAT: lines per renderer path (main.c prints them): [mode] new renderer, [15] forced blank, [16 + reason bits] old renderer */
   {
      extern unsigned LLRFbL[32];
      uint32 w = LLRWhy, r;
      LLRWhy = 0; llr_fallback(); r = LLRWhy; LLRWhy = w;
      if (end > y0) LLRFbL[r ? 16 + (r & 15) : PPU.ForcedBlanking ? 15 : PPU.BGMode] += end - y0;
   }
#endif
   if (llr_fallback())
   {
      LLRStat[1]++;
#ifdef LL_WINLINE
      if (LLWinVar)
      {
         extern int printf(const char*, ...);
         if (LLWinBad++ < 4)
            printf("LLWINBAD: old renderer part with per-line windows\n");
         LLWinVar = 0;
      }
#endif
#ifdef LLR_SKIP
      {
         uint32 y;
         for (y = y0; y < end && y < 240; y++)
            LLRSeen[y] = 0;
      }
#endif
      LLR_MAT();
      LLOldUpdateScreen();
      return;
   }
   if (IPPU.OBJChanged)
      S9xSetupOBJ();
   if (PPU.RecomputeClipWindows)
   {
      ComputeClipWindows();
      PPU.RecomputeClipWindows = FALSE;
   }
   IPPU.PreviousLine = IPPU.CurrentLine;
#endif
#ifdef LLR_KDBG
   {
      static uint32 last;
      LLRDbgF++;
      last = y0;
      printf("U f%u %u-%u\n", LLRDbgF, y0, end);
   }
#endif
#ifdef LL_WINLINE
   LLWinVarCur = LLWinVar;
   LLWinVar = 0;
#endif
   if (y0 < end)
      llr_part(y0, end - 1);
#ifdef LL_WINLINE
   LLWinVarCur = 0;
#endif
#ifdef LLR_KDBG
   printf("U done\n");
#endif
}


#ifdef LLR_CHECK
/* end of frame: the new renderer's rows against the old one's in the LCD */
static void llr_check_frame(void)
{
   static unsigned f;
   extern unsigned LLRFrame;
   const uint8* lcd = (const uint8*) 0x03000000;
   uint32 oy, x, rows = 0, px = 0, fy = 0, fx = 0, fo = 0, fn = 0, unchecked = 0;
   for (oy = 0; oy < 160; oy++)
   {
      uint32 bad = 0;
      if (!LLRChecked[oy])
      {
         unchecked++;
         continue;
      }
      LLRChecked[oy] = 0;
      for (x = 0; x < 160; x++)
      {
         const uint8 *a = lcd + oy * 240 + (x >> 1) * 3, *b = LLRShadow + oy * 240 + (x >> 1) * 3;
         uint32 va = (x & 1) ? (a[1] & 15) << 8 | a[2] : (a[1] >> 4) << 8 | a[0], vb = (x & 1) ? (b[1] & 15) << 8 | b[2] : (b[1] >> 4) << 8 | b[0];
         if (va != vb)
         {
            if (!px)
               fy = oy, fx = x, fo = va, fn = vb;
            if (px < 8)
               printf("LLRD f %u y %u x %u old %03x new %03x idx old %03x new %02x\n", f, oy, x, va, vb,
                      LLROldIdx[oy][x], LLRNewIdx[oy][x]);
            px++, bad = 1;
         }
      }
      rows += bad;
   }
   if (rows || unchecked)
      printf("LLR f %u rows %u px %u unchecked %u first y %u x %u old %03x new %03x mode %u why %x\n", f, rows, px,
             unchecked, fy, fx, fo, fn, PPU.BGMode, LLRWhy);
   LLRWhy = 0;
   LLRFrame = ++f;
}
#endif
