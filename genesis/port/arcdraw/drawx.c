/* drawx.c (-DARCDRAW): PicoDrive's draw.c with each drawn line handed to
   arcdraw_line(), which draws the 160 LCD columns (every other one of 320,
   as -DDIRECT samples them) straight into a 160-byte line and writes it to
   the LCD at 12 bpp. Same pixels as PicoDrive (its quirks included); it reuses
   ParseSprites' per-line sprite lists. Lines with modes the test games don't
   use (interlace, 2-cell vscroll, window split inside the line, debug layer
   switches) go through PicoDrive's DrawDisplay and are sampled from HighCol.
   The hot parts are ARC assembly (arcdraw.S) unless -DARCDRAW_C. */
#include "draw_patched.c"

#define AW 160
unsigned char *arcdraw_dst;        /* LCD line (240 bytes) for the next drawn line; the driver sets it */
unsigned int arcdraw_stat[8];
#ifdef PSPR2
unsigned pspr2_stat[2];
#endif      /* lines: drawn here, fallback, blanked; sh lines; AS lines */

/* line buffer: lb[i] = column 2i, PicoDrive's HighCol byte (bits 0-5 colour,
   0x40 highlight/operator mark, 0x80 shadow). 8 spare bytes each side. */
static u8 lbuf[8 + AW + 8] ALIGNED(4);
#define lb (lbuf + 8)
/* HighPal as the LCD's 12 bpp: two columns are three bytes g0b0, r0r1, g1b1, so even columns go
   through arcdraw_pal[i] = gb | r << 12 and odd ones through arcdraw_pal[256 + i] = r | gb << 8
   (arcdraw.S QGRP); 2 KB aligned, the odd half is arcdraw_pal | 1024 */
u32 arcdraw_pal[512] __attribute__((aligned(2048)));
#define PAL_E(c) (((c) & 0xff) | ((c) >> 8) << 12)    /* 0xRGB -> even-column entry */
#define PAL_O(e) (((e) >> 12) | ((e) & 0xff) << 8)    /* even-column entry -> odd-column entry */
#ifdef OUTQ
/* -DOUTQ: the LCD's st.di stores are posted (one word per ~24 cycles on the
   VRAM bus, the CPU runs on). The line buffer is doubled: while line N is
   drawn into one, line N-1 (the other) is converted to 12 bpp and stored,
   8 pixels (3 words) every few tiles of arcdraw_plane's loop (arcdraw.S);
   what is left is done by arcdraw_qfin before line N becomes the pending
   one, before a palette update, and at the end of the frame (arcdraw_flush).
   arcdraw_q = {line bytes, LCD word, 8-pixel groups left}. */
struct { const u8 *src; u32 *dst; int n; } arcdraw_q;
void arcdraw_qfin(void);
static u8 lbuf2[8 + AW + 8] ALIGNED(4);
static u8 *lbp = lbuf + 8;
#undef lb
#define lb lbp
#endif

struct hit { int x0; u32 code; u32 pack; int part; };  /* cached high tile */
static struct hit hcA[48], hcB[48];

static inline u32 vram32(u32 a) { return *(u32 *)(PicoMem.vram + a); }

/* pixel j (0..7, left to right) of a tile row, h-flipped or not */
static inline unsigned tpix(u32 pack, int flip, int j)
{
  int k = flip ? 7 - j : j;
  return k < 4 ? (pack >> (12 - 4 * k)) & 15 : (pack >> (44 - 4 * k)) & 15;
}

/* plane/window tile pixels: lb = pal|t for t != 0, columns [0, lim) */
static void tile_put(int x0, u32 pack, int flip, unsigned pal, int lim)
{
  int j;
  for (j = x0 & 1; j < 8; j += 2) {
    int c = x0 + j; unsigned t;
    if (c < 0 || c >= lim) continue;
    t = tpix(pack, flip, j);
    if (t) lb[c >> 1] = pal | t;
  }
}

static void clear_sh(int c0, int c1)   /* columns [c0, c1) &= 0x7f */
{
  if (c0 < 0) c0 = 0;
  if (c1 > 320) c1 = 320;
  for (c0 = (c0 + 1) & ~1; c0 < c1; c0 += 2) lb[c0 >> 1] &= 0x7f;
}

#ifdef LREUSE
static u32 lr_palid = 1, lr_palchg;
#endif
#ifdef PLBW
/* -DPLBW: plane B stores its transparent pixels as bytes with low nibble 0
   (palette << 4, bits 6-7 as usual); no other layer writes such a byte, and
   the background fill's byte is bgc. So the output palette's 16 entries
   k << 4 hold the colour of bgc | (k << 4 & 0xc0); plbw_orig keeps their own. */
static u32 plbw_orig[16];
static int plbw_bgc = -1;
static int plbw_apply(void)
{
  int j, k, chg = 0; u32 c[4];
  for (j = 0; j < 4; j++) { int i = plbw_bgc | j << 6; c[j] = (i & 15) ? arcdraw_pal[i] : plbw_orig[i >> 4]; }
  for (k = 0; k < 16; k++) { if (arcdraw_pal[k << 4] != c[k >> 2]) chg = 1; arcdraw_pal[k << 4] = c[k >> 2]; arcdraw_pal[256 + (k << 4)] = PAL_O(c[k >> 2]); }
  return chg;
}
#endif
static NOINLINE void pal_update(void)
{
  int i;
  PicoDrawUpdateHighPal();
  for (i = 0; i < 256; i++) {
    unsigned c = Pico.est.HighPal[i];
    c = (c >> 12) << 8 | ((c >> 7) & 15) << 4 | ((c >> 1) & 15);
    c = PAL_E(c);
#ifdef PLBW
    if (!(i & 15)) { plbw_orig[i >> 4] = c; continue; }
#endif
#ifdef LREUSE
    if (arcdraw_pal[i] != c) lr_palchg = 1;
#endif
    arcdraw_pal[i] = c;
    arcdraw_pal[256 + i] = PAL_O(c);
  }
#ifdef PLBW
  if (plbw_bgc >= 0 && plbw_apply()) {
#ifdef LREUSE
    lr_palchg = 1;
#endif
  }
#endif
#ifdef LREUSE
  if (lr_palchg) { lr_palchg = 0; lr_palid++; }
#endif
#ifdef PLB
  { extern int plb_dirty; plb_dirty = 1; }
#endif
}

#ifdef ARCDRAW_C
/* 160 line bytes -> 12 bpp LCD line through arcdraw_pal */
void arcdraw_out(unsigned char *dst, const unsigned char *s, const u32 *pal)
{
  unsigned *d = (unsigned *)dst; int x;
  for (x = 0; x < AW; x += 8, s += 8, d += 3) {
    unsigned p[8]; int k;
    for (k = 0; k < 8; k++) p[k] = pal[s[k] + (k & 1) * 256];
    d[0] = p[0] | p[1] << 8 | p[2] << 24;
    d[1] = p[2] >> 8 | p[3] | p[4] << 16 | p[5] << 24;
    d[2] = p[5] >> 8 | p[6] << 8 | p[7] << 16;
  }
}
#else
void arcdraw_out(unsigned char *dst, const unsigned char *s, const u32 *pal);
#endif

/* a plane line (B or A over the whole line): low tiles drawn, high tiles
   cached in hc (all in s/h mode, else only non-blank). Returns the cache
   length; *allhi = 1 if every tile visited was high priority. */
#ifdef RSTAT
unsigned rstat_on; unsigned long rstat[16];
static u32 rs_lfr[256], rs_h, rs_prev[2], rs_fr[2][256], rs_lprev, rs_tprev[2][48], rs_tcur[2][48];
static inline u32 rs_mix(u32 h, u32 v) { return (h ^ v) * 16777619u; }
#endif
static int plane_lo(int pb, int maxcells, int maxw, int sh, struct hit *hc, int *allhi)
{
#ifdef RSTAT
  int rs_k = 0;
#endif
  struct PicoVideo *pv = &Pico.video;
  static const char shift[4] = { 5, 6, 5, 7 };
  int L = Pico.est.DrawScanline;
  int width = pv->reg[16], height = (width >> 4) & 3, xmask, ymask, nametab, htab;
  int hs, line, ty, tilex, dx, cells, n = 0, hi = 1, part;
  unsigned shb = sh ? 0x80 : 0;
  width &= 3;
  xmask = (1 << shift[width]) - 1;
  ymask = (height << 8) | 0xff;
  switch (width) { case 1: ymask &= 0x1ff; break; case 2: ymask = 7; break; case 3: ymask = 0xff; break; }
  nametab = pb ? (pv->reg[4] & 7) << 12 : (pv->reg[2] & 0x38) << 9;
  htab = pv->reg[13] << 9;
  switch (pv->reg[11] & 3) {
    case 1: htab += (L << 1) & 0x0f; break;
    case 2: htab += (L << 1) & ~0x0f; break;
    case 3: htab += L << 1; break;
  }
  htab += pb;
  hs = PicoMem.vram[htab & 0x7fff];
  line = (PicoMem.vsram[pb] + L) & ymask;
  nametab += (line >> 3) << shift[width];
  ty = (line & 7) << 1;
  tilex = (-hs) >> 3; dx = ((hs - 1) & 7) + 1; cells = maxcells;
  part = (dx & 7) ? 1 : 0;
#ifdef RSTAT
  rs_h = rs_mix(2166136261u, dx | sh << 8);
#endif
  for (;;) {
    u32 code, addr, pack;
    if (cells == 0) { if (!(dx & 7)) break; part = 2; }
    else if (cells < 0) break;
    code = PicoMem.vram[nametab + (tilex & xmask)];
    addr = ((code << 4) & 0x7ff0) + ty;
    if (code & 0x1000) addr ^= 14;
    pack = vram32(addr);
#ifdef RSTAT
    { u32 t = rs_mix(rs_mix(2166136261u, code & 0xe800), pack);
      rs_h = rs_mix(rs_mix(rs_h, code & 0xe800), pack);
      if (rs_k < 48) { if (rstat_on) { rstat[6]++; if (rs_tprev[pb][rs_k] == t) rstat[7]++; if (!pack) rstat[8]++; } rs_tcur[pb][rs_k++] = t; } }
#endif
    if (code & 0x8000) {
      if (sh || pack) { hc[n].x0 = dx - 8; hc[n].code = code; hc[n].pack = pack; hc[n].part = part; n++; }
    } else {
      hi = 0;
      if (pack) tile_put(dx - 8, pack, code & 0x800, ((code >> 9) & 0x30) | shb, maxw);
    }
    if (part == 2) break;
    if (part == 1) part = 0;   /* the first partial tile takes a cell too */
    dx += 8; tilex++; cells--;
  }
  *allhi = hi;
#ifdef RSTAT
  if (rstat_on) { rstat[0]++; if (rs_prev[pb] == rs_h) rstat[1]++; if (rs_fr[pb][L & 255] == rs_h) rstat[2]++;
    if (rs_prev[pb] == rs_h || rs_fr[pb][L & 255] == rs_h) rstat[3]++; }
  rs_prev[pb] = rs_h; rs_fr[pb][L & 255] = rs_h;
  memcpy(rs_tprev[pb], rs_tcur[pb], sizeof rs_tcur[pb]);
#endif
  return n;
}

/* high tiles from the cache; s/h: clear shadow under each (PicoDrive's
   partial-tile handling: a first partial tile that is also the last cached
   one clears nothing visible) unless the whole line was cleared */
static void plane_hi(struct hit *hc, int n, int shclr, int maxw)
{
  int i;
  for (i = 0; i < n; i++) {
    struct hit *t = &hc[i];
    if (shclr && !(t->part == 1 && i == n - 1)) {
      int c1 = t->x0 + 8;
      if (c1 > maxw) c1 = maxw;
      clear_sh(t->x0, c1);
    }
    if (t->pack) tile_put(t->x0, t->pack, t->code & 0x800, (t->code >> 9) & 0x30, maxw);
  }
}

static int window_pass(int tstart, int tend, int prio, int sh, int diff)
{
  struct PicoVideo *pv = &Pico.video;
  int L = Pico.est.DrawScanline, ty = (L & 7) << 1, nametab, tilex;
  unsigned pal_sh = (sh && !prio) ? 0x80 : 0;
  if (pv->reg[12] & 1) nametab = ((pv->reg[3] & 0x3c) << 9) + ((L >> 3) << 6);
  else                 nametab = ((pv->reg[3] & 0x3e) << 9) + ((L >> 3) << 5);
  if (prio && !diff) return 0;
  for (tilex = tstart << 1; tilex < (tend << 1); tilex++) {
    u32 code = PicoMem.vram[nametab + tilex], addr, pack;
    if ((code >> 15) != prio) { diff = 1; continue; }
    if (sh && prio) clear_sh(tilex << 3, (tilex << 3) + 8);
    addr = ((code << 4) & 0x7ff0) + ty;
    if (code & 0x1000) addr ^= 14;
    pack = vram32(addr);
    if (pack) tile_put(tilex << 3, pack, code & 0x800, ((code >> 9) & 0x30) | pal_sh, 1 << 30);
  }
  return diff;
}

/* sprite pixel operations */
enum { M_NORM, M_NONSH, M_MARKOP, M_SH, M_ONLYOP, AS_MARK, AS_NORM, AS_SH, AS_ONLYOP };
static u8 claim[AW];

static void spr_tile(int x0, u32 pack, int flip, unsigned pal, int mode)
{
  int j;
  for (j = x0 & 1; j < 8; j += 2) {
    int c = x0 + j, i; unsigned t;
    if (c < 0 || c >= 320) continue;
    t = tpix(pack, flip, j); i = c >> 1;
    if (!t) continue;
    switch (mode) {
      case M_NORM: lb[i] = pal | t; break;
      case M_NONSH: lb[i] = pal | t; if (t == 14) lb[i] &= ~0x80; break;
      case M_MARKOP: lb[i] = t < 14 ? pal | t : lb[i] | 0x40; break;
      case M_SH: lb[i] = t < 14 ? pal | t : (lb[i] | ((t - 1) << 6)) & 0xff; break;
      case M_ONLYOP: if (t >= 14 && (lb[i] & 0x40)) lb[i] = ((lb[i] & ~0x40) | ((t - 1) << 6)) & 0xff; break;
      default:
        if (claim[i]) break;
        claim[i] = 1;
        if (mode == AS_NORM) lb[i] = pal | t;
        else if (mode == AS_SH) lb[i] = t < 14 ? pal | t : ((lb[i] & ~0x40) | ((t - 1) << 6)) & 0xff;
        else if (mode == AS_ONLYOP && t >= 14 && (lb[i] & 0x40)) lb[i] = ((lb[i] & ~0x40) | ((t - 1) << 6)) & 0xff;
        break;
    }
  }
}

/* one sprite on this line (w: tile limit, 0 = none) */
static void spr_one(s32 *sprite, int w, unsigned pal, int mode)
{
  int L = Pico.est.DrawScanline;
  s32 sy = sprite[0], code = sprite[1];
  int sx = code >> 16, width = sy >> 28, height = (sy >> 24) & 7, row, tile, delta;
  sy = (s16)sy;
  row = L - sy;
  if (code & 0x1000) row = (height << 3) - 1 - row;
  tile = code + (row >> 3);
  delta = height;
  if (code & 0x0800) { tile += delta * (width - 1); delta = -delta; }
  tile &= 0x7ff; tile <<= 4; tile += (row & 7) << 1;
  delta <<= 4;
  if (w) width = w;
  for (; width; width--, sx += 8, tile += delta) {
    if (sx <= 0) continue;
    if (sx >= 328) break;
    spr_tile(sx - 8, vram32(tile & 0x7fff), code & 0x800, pal, mode);
  }
}

static int spr_masked(const u8 *sprited)
{
  return (sprited[1] & (SPRL_TILE_OVFL | SPRL_HAVE_MASK0)) == (SPRL_TILE_OVFL | SPRL_HAVE_MASK0);
}

/* DrawAllSprites */
static void spr_all(const u8 *sprited, s32 *pre, int prio, int sh)
{
  const u8 *p = sprited + 4;
  int cnt = sprited[0] & 0x7f, w;
  if (!cnt || spr_masked(sprited)) return;
  for (w = p[cnt], cnt--; cnt >= 0; cnt--, w = 0) {
    s32 *sp = pre + (p[cnt] & 0x7f) * 2, code = sp[1];
    unsigned pal = (code >> 9) & 0x30;
    if ((p[cnt] >> 7) != prio) continue;
    if (sh && (code & 0x6000) == 0x6000) spr_one(sp, w, pal | 0x80, M_MARKOP);
    else if (sh) spr_one(sp, w, pal | 0x80, M_NONSH);
    else spr_one(sp, w, pal, M_NORM);
  }
}

/* DrawSpritesSHi */
#ifdef SHFAST
__attribute__((noinline))   /* (keeps v3's code layout: spr_shi has one caller now) */
#endif
static void spr_shi(const u8 *sprited, s32 *pre)
{
  const u8 *p = sprited + 4;
  int cnt = sprited[0] & 0x7f, w;
  if (!cnt || spr_masked(sprited)) return;
  for (w = p[cnt], cnt--; cnt >= 0; cnt--, w = 0) {
    s32 *sp = pre + (p[cnt] & 0x7f) * 2, code = sp[1];
    unsigned pal = (code >> 9) & 0x30;
    if (pal == 0x30) spr_one(sp, w, pal, (code & 0x8000) ? M_SH : M_ONLYOP);
    else if (code & 0x8000) spr_one(sp, w, pal, M_NORM);
  }
}

/* DrawSpritesHiAS: front to back, a pixel goes to the first sprite with a
   non-transparent pixel there */
static void spr_as(const u8 *sprited, s32 *pre, int sh)
{
  const u8 *p = sprited + 4;
  int cnt = sprited[0] & 0x7f, e;
  if (!cnt || spr_masked(sprited)) return;
  memset(claim, 0, sizeof claim);
  for (e = 0; e < cnt; e++) {
    s32 *sp = pre + (p[e] & 0x7f) * 2, code = sp[1];
    unsigned pal = (code >> 9) & 0x30;
    int op = sh && pal == 0x30, hi = !!(code & 0x8000);
    int mode = op ? (hi ? AS_SH : AS_ONLYOP) : (hi ? AS_NORM : AS_MARK);
    int w = (e + 1 == cnt) ? p[e + 1] : 0;
    if (e + 1 == cnt && !w) continue;   /* PicoDrive: width = p[cnt] even if 0 */
    spr_one(sp, w, pal, mode);
  }
}

#ifndef ARCDRAW_C
/* the assembly plane path (H40 only; H32 lines use the C functions above) */
struct pl_args { u8 *lbpos; const u16 *ntrow; int tilex, xmask; const u8 *vram; int ty, count;
  u32 pal_or; int rotN, rotF; u32 *hc; int cache_blank; int allhi; u32 *hcend; u32 mask;
#ifdef PLBW
  u32 pw[4];
#endif
};
#ifdef LREUSE
/* -DLREUSE: a drawn LCD row is left alone when nothing it was drawn from has
   changed since: key (line, regs, h/v scroll, palette id, sprite list header),
   the sprite list and the sprite entries it names, and no VRAM write that
   changed a 2 KB group the row read (plane tiles, nametable rows, sprite tiles;
   vp/videoport.c stamps lr_gseq). Rows that missed 3+ times in a row are
   checked/saved only 2 frames in 8 (cost only: a row not checked is drawn). */
#define LR_NS 12
struct lrkey { u32 k[7]; u32 mask; };
struct lrspr { u8 hdr[16]; s32 pre[2 * LR_NS]; };
static struct lrkey lr_tab[160] ALIGNED(32);
static struct lrspr lr_spr[160];
static u32 lr_seq[160];
static u8 lr_st[160];              /* bit 7: entry valid; bits 0-6: misses in a row */
static unsigned lr_fc;
#ifdef LRADAPT
static unsigned lr_off, lr_nhit, lr_nchk;
#define LR_ON (!lr_off)
#else
#define LR_ON 1
#endif
#ifdef LRCHECK  /* hits are drawn anyway and the line compared with the saved one */
static u8 lr_lb[160][AW];
static int lr_chk, lr_bad;
#endif
unsigned lr_clock, lr_gseq[32];
int arcdraw_row;
static u32 lr_k[7], lr_mask;
/* for the console port: anything else that writes LCD rows (an overlay such as
   LeapLoader's volume panel or the menu combo prompt, a screen clear, a page
   flip) must call this for the rows it touched, or LREUSE leaves them as they are */
void arcdraw_lr_invalidate(int row0, int row1)
{
  for (; row0 <= row1 && row0 < 160; row0++) if (row0 >= 0) lr_st[row0] &= 0x7f;
}
static void lr_ntmask(const u16 *ntrow, int xmask)
{
  u32 a = (const u8 *)ntrow - (const u8 *)PicoMem.vram, e = a + 2 * xmask + 1;
  lr_mask |= 1u << ((a >> 11) & 31) | 1u << ((e >> 11) & 31);
}
static void lr_key(int L, int sh, int bgc, int hvwind, const u8 *sp)
{
  struct PicoVideo *pv = &Pico.video;
  int htab = pv->reg[13] << 9;
  switch (pv->reg[11] & 3) {
    case 1: htab += (L << 1) & 0x0f; break;
    case 2: htab += (L << 1) & ~0x0f; break;
    case 3: htab += L << 1; break;
  }
  lr_k[0] = 0x80000000u | L | hvwind << 9 | sh << 11 | bgc << 12 | pv->reg[18] << 18;
  lr_k[1] = pv->reg[1] | pv->reg[2] << 8 | pv->reg[3] << 16 | pv->reg[4] << 24;
  lr_k[2] = pv->reg[7] | pv->reg[11] << 8 | pv->reg[12] << 16 | pv->reg[13] << 24;
  lr_k[3] = pv->reg[16] | pv->reg[17] << 8 | (lr_palid & 0xffff) << 16;
  lr_k[4] = PicoMem.vram[(htab + 1) & 0x7fff] | PicoMem.vram[htab & 0x7fff] << 16;
  lr_k[5] = PicoMem.vsram[0] | PicoMem.vsram[1] << 16;
  lr_k[6] = (sp[0] & 0x7f) | sp[1] << 8 | sp[2] << 16 | sp[3] << 24;
}
#ifdef LRK2
/* -DLRK2: the row key in one 16 B line {global id << 8 | L, hscroll words,
   sprite header, seq} + the group mask in a dense array; the global part of
   the key (k[0] without L, k[1-3], k[5]) is compared once a line with the
   last one seen and numbered (lr_gid). */
struct lrk2 { u32 gl, hs, sp, seq; };
static struct lrk2 lr_t2[160] ALIGNED(16);
static u32 lr_m2[160];
/* the last 8 global states seen, each with its id (states that alternate within
   a frame keep their ids); ids are never reused (wrap: all rows invalid) */
#define LR_NG 8
static u32 lr_G[LR_NG][6], lr_gid, lr_gcur, lr_grr;
static u32 lr_gl, lr_gid_cur;
static void lr_k2(void)
{
  u32 g0 = lr_k[0] & ~0x1ffu, *G = lr_G[lr_gcur];
  if (g0 != G[0] || lr_k[1] != G[1] || lr_k[2] != G[2] || lr_k[3] != G[3] || lr_k[5] != G[4] || !G[5]) {
    int i;
    for (i = 0; i < LR_NG; i++) { G = lr_G[i];
      if (g0 == G[0] && lr_k[1] == G[1] && lr_k[2] == G[2] && lr_k[3] == G[3] && lr_k[5] == G[4] && G[5]) break; }
    if (i == LR_NG) {
      i = lr_grr; lr_grr = (lr_grr + 1) & (LR_NG - 1); G = lr_G[i];
      G[0] = g0; G[1] = lr_k[1]; G[2] = lr_k[2]; G[3] = lr_k[3]; G[4] = lr_k[5];
      if (++lr_gid >= 0xffffff) { int j; lr_gid = 1; for (j = 0; j < 160; j++) lr_st[j] &= 0x7f;
        for (j = 0; j < LR_NG; j++) lr_G[j][5] = 0; }
      G[5] = lr_gid;
    }
    lr_gcur = i;
  }
  lr_gid_cur = G[5];
  lr_gl = lr_gid_cur << 8 | (lr_k[0] & 0xff);
}
#endif
static int lr_hit(int row, const u8 *sp, const s32 *pre)
{
#ifdef LRK2
  const struct lrk2 *e = &lr_t2[row];
  int i, n;
  u32 m, sq = e->seq;
  if (e->gl != lr_gl || e->hs != lr_k[4] || e->sp != lr_k[6]) return 0;
  for (m = lr_m2[row]; m; m &= m - 1)
#else
  const struct lrkey *e = &lr_tab[row];
  int i, n;
  u32 m, sq = lr_seq[row];
  for (i = 0; i < 7; i++) if (e->k[i] != lr_k[i]) return 0;
  for (m = e->mask; m; m &= m - 1)
#endif
    if (lr_gseq[31 - __builtin_clz(m & -m)] >= sq) return 0;
  n = sp[0] & 0x7f;
  if (n) {
    const struct lrspr *q = &lr_spr[row];
    for (i = 0; i <= n; i++) if (q->hdr[i] != sp[4 + i]) return 0;
    for (i = 0; i < n; i++) {
      const s32 *p = pre + (sp[4 + i] & 0x7f) * 2;
      if (p[0] != q->pre[2 * i] || p[1] != q->pre[2 * i + 1]) return 0;
    }
  }
  return 1;
}
static void lr_save(int row, u32 clk, const u8 *sp, const s32 *pre)
{
  struct lrkey *e = &lr_tab[row];
  int i, n = sp[0] & 0x7f;
  u32 m = lr_mask;
  if (n > LR_NS) return;
  if (n) {
    struct lrspr *q = &lr_spr[row];
    for (i = 0; i <= n; i++) q->hdr[i] = sp[4 + i];
    for (i = 0; i < n; i++) {
      const s32 *p = pre + (sp[4 + i] & 0x7f) * 2;
      u32 t = p[1] & 0x7ff;      /* tiles t .. t + 15 at most */
      q->pre[2 * i] = p[0]; q->pre[2 * i + 1] = p[1];
      m |= 1u << (t >> 6) | 1u << (((t + 15) >> 6) & 31);
    }
  }
#ifdef LRK2
  { struct lrk2 *f = &lr_t2[row]; f->gl = lr_gl; f->hs = lr_k[4]; f->sp = lr_k[6]; f->seq = clk; lr_m2[row] = m; (void)e; }
#else
  for (i = 0; i < 7; i++) e->k[i] = lr_k[i];
  e->mask = m; lr_seq[row] = clk;
#endif
  lr_st[row] |= 0x80;
}
#endif
void arcdraw_plane(struct pl_args *a);
#ifdef PLBW
void arcdraw_planeb(struct pl_args *a);
#endif
void arcdraw_hi(const u32 *hc, int n, int shclr, u8 *line);
#ifdef SHFAST
int arcdraw_ntscan(const u16 *ntrow, int tilex, int xmask, int count);
#endif
void arcdraw_spr(const u8 *sprited, const s32 *pre, int prio, int sh, u8 *line, const u8 *vram, int L);
static void spr_all_asm(const u8 *sprited, s32 *pre, int prio, int sh)
{
  if (!(sprited[0] & 0x7f) || spr_masked(sprited)) return;
  arcdraw_spr(sprited, pre, prio, sh, lb, (const u8 *)PicoMem.vram, Pico.est.DrawScanline);
}
static u32 hcAa[3 * 48], hcBa[3 * 48];

#ifdef SHFAST
static inline __attribute__((always_inline)) void plane_setup(int pb, int sh, u32 *hc, struct pl_args *ap)
#else
static int plane_asm(int pb, int sh, u32 *hc, int *allhi)
#endif
{
  struct PicoVideo *pv = &Pico.video;
  static const char shift[4] = { 5, 6, 5, 7 };
#ifdef SHFAST
#define a (*ap)
#else
  struct pl_args a;
#endif
  int L = Pico.est.DrawScanline;
  int width = pv->reg[16], height = (width >> 4) & 3, ymask, nametab, htab, hs, line, x0;
  width &= 3;
  ymask = (height << 8) | 0xff;
  switch (width) { case 1: ymask &= 0x1ff; break; case 2: ymask = 7; break; case 3: ymask = 0xff; break; }
  nametab = pb ? (pv->reg[4] & 7) << 12 : (pv->reg[2] & 0x38) << 9;
  htab = pv->reg[13] << 9;
  switch (pv->reg[11] & 3) {
    case 1: htab += (L << 1) & 0x0f; break;
    case 2: htab += (L << 1) & ~0x0f; break;
    case 3: htab += L << 1; break;
  }
  hs = PicoMem.vram[(htab + pb) & 0x7fff];
  line = (PicoMem.vsram[pb] + L) & ymask;
  x0 = ((hs - 1) & 7) - 7;                 /* first tile's column: -7..0 */
  a.lbpos = lb + ((x0 + 1) >> 1);
  a.ntrow = PicoMem.vram + nametab + ((line >> 3) << shift[width]);
  a.tilex = (-hs) >> 3;
  a.xmask = (1 << shift[width]) - 1;
  a.vram = (const u8 *)PicoMem.vram;
  a.ty = (line & 7) << 2;
  a.count = x0 ? 41 : 40;
  a.pal_or = sh ? 0x80 : 0;
  a.rotN = (x0 & 1) ? 28 : 0;
  a.rotF = (x0 & 1) ? 4 : 0;
  a.hc = hc;
  a.cache_blank = sh;
#ifdef SHFAST
#undef a
}
static int plane_asm(int pb, int sh, u32 *hc, int *allhi)
{
  struct pl_args a;
  plane_setup(pb, sh, hc, &a);
#endif
#ifdef PLBW
  if (pb) { u32 o = (sh ? 0x80 : 0) * 0x01010101u;
    a.pw[0] = o; a.pw[1] = o | 0x10101010u; a.pw[2] = o | 0x20202020u; a.pw[3] = o | 0x30303030u;
    arcdraw_planeb(&a); } else
#endif
  arcdraw_plane(&a);
#ifdef LREUSE
  lr_mask |= a.mask; lr_ntmask(a.ntrow, a.xmask);
#endif
  *allhi = a.allhi;
  return (a.hcend - hc) / 3;
}

static int window_asm(int sh, u32 *hc)
{
  struct PicoVideo *pv = &Pico.video;
  struct pl_args a;
  int L = Pico.est.DrawScanline;
  a.lbpos = lb;
  a.ntrow = PicoMem.vram + ((pv->reg[3] & 0x3c) << 9) + ((L >> 3) << 6);
  a.tilex = 0; a.xmask = 63;
  a.vram = (const u8 *)PicoMem.vram;
  a.ty = (L & 7) << 2;
  a.count = 40;
  a.pal_or = sh ? 0x80 : 0;
  a.rotN = a.rotF = 0;
  a.hc = hc;
  a.cache_blank = sh;
  arcdraw_plane(&a);
#ifdef LREUSE
  lr_mask |= a.mask; lr_ntmask(a.ntrow, a.xmask);
#endif
  return (a.hcend - hc) / 3;
}

#ifdef SHFAST
void arcdraw_plane_x(struct pl_args *a);
void arcdraw_spr_x(const u8 *sprited, const s32 *pre, int prio, int sh, u8 *line, const u8 *vram, int L);
#define XTEXT __attribute__((noinline, section(".xtext.drawx")))
static XTEXT void spr_x(const u8 *sprited, s32 *pre, int prio, int sh)
{
  if (!(sprited[0] & 0x7f) || spr_masked(sprited)) return;
  arcdraw_spr_x(sprited, pre, prio, sh, lb, (const u8 *)PicoMem.vram, Pico.est.DrawScanline);
}
#if defined(NTMEMO) && defined(LREUSE)
/* -DNTMEMO: arcdraw_ntscan's result kept per (nametable row, first tile, mask, count) until a
   VRAM write changes a 2 KB group the row spans (lr_gseq stamps, as LREUSE) */
static XTEXT int ntscan_m(const u16 *ntrow, int tilex, int xmask, int count)
{
  static struct { const u16 *nt; int tx, xm, cnt; u32 seq; int r; } m[8];
  u32 a = (const u8 *)ntrow - (const u8 *)PicoMem.vram, e = a + 2 * xmask + 1;
  int i = ((a >> 6) ^ tilex) & 7, r;
  if (m[i].nt == ntrow && m[i].tx == tilex && m[i].xm == xmask && m[i].cnt == count
      && lr_gseq[(a >> 11) & 31] < m[i].seq && lr_gseq[(e >> 11) & 31] < m[i].seq)
    return m[i].r;
  r = arcdraw_ntscan(ntrow, tilex, xmask, count);
  m[i].nt = ntrow; m[i].tx = tilex; m[i].xm = xmask; m[i].cnt = count; m[i].seq = lr_clock; m[i].r = r;
  return r;
}
#define NTSCAN ntscan_m
#else
#define NTSCAN arcdraw_ntscan
#endif
/* -DSHFAST: s/h lines (not a full-line window) drawn without the high tile cache where the
   result is the same; returns 0 when the line needs the usual path */
static XTEXT int line_shfast(const u8 *sprited, s32 *pre)
{
  int nB, hiB;
  /* s/h with plane A all high: PicoDrive clears the shadow bit of the whole line
     after the low sprites, then draws the high tiles without it. Same result:
     B without the shadow bit, low sprites without it (sh 3: operator colours
     still mark), B's high tiles, then all of A drawn directly (no cache). */
  {
    struct pl_args aA;
    int scA;
    plane_setup(0, 1, hcAa, &aA);
    scA = NTSCAN(aA.ntrow, aA.tilex, aA.xmask, aA.count);
    if ((scA & 0xffff) == aA.count) {
      arcdraw_stat[6]++;
      nB = plane_asm(1, 0, hcBa, &hiB);
      if (sprited[1] & SPRL_HAVE_LO) spr_x(sprited, pre, 0, 3);
      if (nB) arcdraw_hi(hcBa, nB, 0, lb);
      aA.pal_or = 0; aA.cache_blank = 2;
      arcdraw_plane_x(&aA);
#ifdef LREUSE
      lr_mask |= aA.mask; lr_ntmask(aA.ntrow, aA.xmask);
#endif
      return 1;
    }
    /* no low sprites and no high tile in B: A's tiles can be drawn in order, a high one
       clearing the shadow under it (not when A's only high tile is a first partial one:
       PicoDrive leaves the shadow under a first partial tile that is the last cached) */
    if (!(sprited[1] & SPRL_HAVE_LO) && !(scA == 0x10001 && aA.lbpos < lb)) {
      struct pl_args aB;
      plane_setup(1, 1, hcBa, &aB);
      if (!(NTSCAN(aB.ntrow, aB.tilex, aB.xmask, aB.count) & 0xffff)) {
        arcdraw_stat[7]++;
        nB = plane_asm(1, 1, hcBa, &hiB);
        aA.cache_blank = 3;
        arcdraw_plane_x(&aA);
#ifdef LREUSE
        lr_mask |= aA.mask; lr_ntmask(aA.ntrow, aA.xmask);
#endif
        return 1;
      }
    }
  }
  return 0;
}
#endif
static void line_asm(int line, int sh, int hvwind)
{
  u8 *sprited = &HighLnSpr[line][0];
  s32 *pre = HighPreSpr + (sprited[0] & 0x80) * 2;
  int nB, nA, hiB, hiA = 0;
#ifdef SHFAST
  if (sh && hvwind != 1 && line_shfast(sprited, pre)) return;
#endif
  nB = plane_asm(1, sh, hcBa, &hiB);
  if (hvwind == 1) nA = window_asm(sh, hcAa);
  else nA = plane_asm(0, sh, hcAa, &hiA);
  if (sprited[1] & SPRL_HAVE_LO) spr_all_asm(sprited, pre, 0, sh);
  if (sh && (hiB || hiA) && (nB || nA)) {   /* PDRAW_PLANE_HI_PRIO: clear the whole line once */
    u32 *w = (u32 *)lb; int i;
    for (i = 0; i < AW / 4; i++) w[i] &= 0x7f7f7f7f;
    sh = 0;
  }
  if (nB) arcdraw_hi(hcBa, nB, sh, lb);
  if (nA) arcdraw_hi(hcAa, nA, sh, lb);
}
#ifdef PLB
/* -DPLB: plane buffers + compositor (H40, no shadow/highlight, no window split,
   no partial blanking). Each plane (or the full-line window) is drawn into its
   own word buffer with plain word stores, no transparency tests: byte =
   prio << 7 | pal << 4 | pixel (pixel 0 = transparent), 4 shown columns per
   tile. Sprites go to two byte buffers (low and high priority passes, same
   drawing code and order as PicoDrive). The compositor resolves each group
   of 4 columns with masks (B, A over B unless B is a high pixel and A low,
   low sprites unless a plane pixel is high, high sprites) and converts to
   12 bpp through plb_pal (index = byte; pixel 0 = the background colour),
   3 st.di per 8 columns. Same pixels as the line buffer path. */
static u32 plb_bufB[44] ALIGNED(16), plb_bufA[44] ALIGNED(16);
static u8 plb_sl[8 + AW + 8] ALIGNED(4), plb_sh[8 + AW + 8] ALIGNED(4);
u32 plb_pal[256];
static int plb_bgc = -1;
int plb_dirty = 1;
static int plb_sdirty;
const u32 plb_rep[8] = { 0, 0x10101010, 0x20202020, 0x30303030,
  0x80808080, 0x90909090, 0xa0a0a0a0, 0xb0b0b0b0 };
unsigned plb_stat[2];

static void plb_mkpal(int bgc)
{
  int i;
  for (i = 0; i < 256; i++) plb_pal[i] = arcdraw_pal[(i & 15) ? (i & 0x3f) : bgc];
  plb_bgc = bgc;
}

#ifndef PLB_C
u32 plb_tiles(u32 *buf, const u16 *ntrow, int tilex, int xmask, int ty, int count, int par, const u8 *vram);
struct plb_ca { u32 *dst; const u32 *b, *a; int sb, sa; const u8 *sl, *sh; int mode; };
void plb_comp_asm(struct plb_ca *c);
static u32 plb_or;
#define plb_tiles(b, n, x, m, t, c, p) (plb_or |= plb_tiles(b, n, x, m, t, c, p, (const u8 *)PicoMem.vram))
#else
static void plb_tiles(u32 *buf, const u16 *ntrow, int tilex, int xmask, int ty, int count, int par)
{
  const u8 *vram = (const u8 *)PicoMem.vram;
  int i;
  for (i = 0; i < count; i++) {
    u32 code = ntrow[tilex & xmask], a, w, x;
    tilex++;
    a = ((code & 0x7ff) << 5) + ((code & 0x1000) ? (ty ^ 28) : ty);
    w = *(const u32 *)(vram + a);
    if (code & 0x800) { x = (par ? w >> 4 : w) & 0x0f0f0f0f; x = (x >> 16) | (x << 16); }
    else { x = (par ? w : w >> 4) & 0x0f0f0f0f; x = ((x & 0x00ff00ff) << 8) | ((x >> 8) & 0x00ff00ff); }
    buf[i] = x | plb_rep[code >> 13];
  }
}
#endif

static int plb_plane(int pb, u32 *buf)
{
  struct PicoVideo *pv = &Pico.video;
  static const char shift[4] = { 5, 6, 5, 7 };
  int L = Pico.est.DrawScanline;
  int width = pv->reg[16], height = (width >> 4) & 3, ymask, nametab, htab, hs, line, x0;
  width &= 3;
  ymask = (height << 8) | 0xff;
  switch (width) { case 1: ymask &= 0x1ff; break; case 2: ymask = 7; break; case 3: ymask = 0xff; break; }
  nametab = pb ? (pv->reg[4] & 7) << 12 : (pv->reg[2] & 0x38) << 9;
  htab = pv->reg[13] << 9;
  switch (pv->reg[11] & 3) {
    case 1: htab += (L << 1) & 0x0f; break;
    case 2: htab += (L << 1) & ~0x0f; break;
    case 3: htab += L << 1; break;
  }
  hs = PicoMem.vram[(htab + pb) & 0x7fff];
  line = (PicoMem.vsram[pb] + L) & ymask;
  x0 = ((hs - 1) & 7) - 7;
  plb_tiles(buf, PicoMem.vram + nametab + ((line >> 3) << shift[width]), (-hs) >> 3,
            (1 << shift[width]) - 1, (line & 7) << 2, x0 ? 41 : 40, x0 & 1);
  return -((x0 + 1) >> 1);
}

static inline u32 plb_opq(u32 x) { u32 t = (((x & 0x0f0f0f0f) + 0x0f0f0f0f) >> 4) & 0x01010101; return (t << 8) - t; }
static inline u32 plb_hi(u32 x) { u32 t = (x >> 7) & 0x01010101; return (t << 8) - t; }
static inline u32 plb_get(const u32 *b, int k, int o)
{ return o ? (b[k] >> (8 * o)) | (b[k + 1] << (32 - 8 * o)) : b[k]; }
static inline void stdi(u32 *p, int o, u32 v) { __asm__ volatile("st.di %0,[%1,%2]" :: "r"(v), "r"(p), "i"(o) : "memory"); }

#ifdef PLB_C
static NOINLINE void plb_comp(u32 *dst, int oB, int oA, int spr)
{
  const u32 *sl = (const u32 *)(plb_sl + 8), *sh = (const u32 *)(plb_sh + 8);
  const u32 *P = plb_pal;
  int k;
  for (k = 0; k < AW / 4; k += 2) {
    u32 r[2]; int j;
    for (j = 0; j < 2; j++) {
      u32 b = plb_get(plb_bufB, k + j, oB), a = plb_get(plb_bufA, k + j, oA);
      u32 ma = plb_opq(a), hpl = 0;
      if ((a | b) & 0x80808080) {
        u32 hb = plb_opq(b) & plb_hi(b), ha = plb_hi(a);
        hpl = hb | (ma & ha);
        ma &= ha | ~hb;
      }
      b ^= (a ^ b) & ma;
      if (spr) {
        u32 s = sl[k + j], t = sh[k + j];
        if (s) b ^= (s ^ b) & plb_opq(s) & ~hpl;
        if (t) b ^= (t ^ b) & plb_opq(t);
      }
      r[j] = b;
    }
    {
      u32 p0 = P[r[0] & 0xff], p1 = P[(r[0] >> 8) & 0xff], p2 = P[(r[0] >> 16) & 0xff], p3 = P[r[0] >> 24];
      u32 p4 = P[r[1] & 0xff], p5 = P[(r[1] >> 8) & 0xff], p6 = P[(r[1] >> 16) & 0xff], p7 = P[r[1] >> 24];
      stdi(dst, 0, p0 | p1 << 12 | p2 << 24);
      stdi(dst, 4, p2 >> 8 | p3 << 4 | p4 << 16 | p5 << 28);
      stdi(dst, 8, p5 >> 4 | p6 << 8 | p7 << 20);
      dst += 3;
    }
  }
}
#endif

static void plb_line(int line, int bgc, int hvwind)
{
  struct PicoVideo *pv = &Pico.video;
  u8 *sprited = &HighLnSpr[line][0];
  s32 *pre = HighPreSpr + (sprited[0] & 0x80) * 2;
  int oB, oA = 0, spr = 0;
  if (bgc != plb_bgc || plb_dirty) { plb_mkpal(bgc); plb_dirty = 0; }
#ifndef PLB_C
  plb_or = 0;
#endif
  oB = plb_plane(1, plb_bufB);
  if (hvwind == 1) {
    int L = Pico.est.DrawScanline;
    plb_tiles(plb_bufA, PicoMem.vram + ((pv->reg[3] & 0x3c) << 9) + ((L >> 3) << 6), 0, 63, (L & 7) << 2, 40, 0);
  } else oA = plb_plane(0, plb_bufA);
  if ((sprited[0] & 0x7f) && !spr_masked(sprited) && (sprited[1] & (SPRL_HAVE_LO | SPRL_HAVE_HI))) {
    memset(plb_sl + 8, 0, AW); memset(plb_sh + 8, 0, AW); spr = plb_sdirty = 1;
    if (sprited[1] & SPRL_HAVE_LO)
      arcdraw_spr(sprited, pre, 0, 0, plb_sl + 8, (const u8 *)PicoMem.vram, Pico.est.DrawScanline);
    if (sprited[1] & SPRL_HAVE_HI)
      arcdraw_spr(sprited, pre, 1, 0, plb_sh + 8, (const u8 *)PicoMem.vram, Pico.est.DrawScanline);
  }
  else if (plb_sdirty) { memset(plb_sl + 8, 0, AW); memset(plb_sh + 8, 0, AW); plb_sdirty = 0; }
#ifdef OUTQ
  arcdraw_qfin();
#endif
#ifdef PLB_C
  plb_comp((u32 *)arcdraw_dst, oB, oA, spr);
#else
  { struct plb_ca c = { (u32 *)arcdraw_dst, plb_bufB, plb_bufA, oB, oA, plb_sl + 8, plb_sh + 8,
      spr | ((plb_or & 0x8000) ? 2 : 0) };
    plb_comp_asm(&c); }
#endif
  plb_stat[0]++;
}
#endif
#endif

static void sample_highcol(void)
{
  const u8 *s = Pico.est.HighCol + 8; int i;
  for (i = 0; i < AW; i++) lb[i] = s[2 * i];
}

static void fill(int c0, int c1, unsigned v)    /* columns [c0, c1) = v */
{
  for (c0 = (c0 + 1) & ~1; c0 < c1; c0 += 2) lb[c0 >> 1] = v;
}

static int arcdraw_line(int line, int sh, int bgc, int off, int on)
{
  struct PicoEState *est = &Pico.est;
  struct PicoVideo *pv = &Pico.video;
  unsigned char *dst = arcdraw_dst;
#ifdef LREUSE
  int lrrow = arcdraw_row;
  u32 lrclk = ++lr_clock;
  int lrsave = 0, lrst = lr_st[lrrow];
  lr_st[lrrow] = lrst & 0x7f;          /* invalid until saved again */
  if (!lrrow) { lr_fc++;
#ifdef LRADAPT
    /* -DLRADAPT: when under 1 in 4 of the rows checked over 16 frames hit (a check and
       save cost about a quarter of a drawn line), no row is checked for the next 112 */
    if (lr_off) lr_off--;
    else if (!(lr_fc & 15)) { if (lr_nchk >= 64 && lr_nhit * 4 < lr_nchk) lr_off = 112; lr_nhit = lr_nchk = 0; }
#endif
  }
#endif

#ifdef OUTQ
  if (Pico.m.dirtyPal) { arcdraw_qfin(); pal_update(); }
#ifdef PLBW
  if (bgc != plbw_bgc) { arcdraw_qfin(); plbw_bgc = bgc;
#ifdef LREUSE
    if (plbw_apply()) lr_palid++;
#else
    plbw_apply();
#endif
  }
#endif

#else
  if (Pico.m.dirtyPal) pal_update();
#endif
  if (off < 0) {                       /* DrawBlankedLine */
    memset(lb, bgc | (sh << 7), AW);
    arcdraw_stat[2]++;
  } else if (!(pv->reg[1] & 0x40)) {   /* display off */
    memset(lb, bgc | (sh << 7), AW);
    arcdraw_stat[2]++;
  } else {
    int maxcells = (pv->reg[12] & 1) ? 40 : 32, maxw = maxcells * 8;
    int win = pv->reg[0x12], edge = (win & 0x1f) << 3, hvwind = 0;
    if (win & 0x80) { if (line >= edge) hvwind = 1; }
    else            { if (line <  edge) hvwind = 1; }
    if (!hvwind) {
      win = pv->reg[0x11]; edge = win & 0x1f;
      if (win & 0x80) { if (!edge) hvwind = 1; else if (edge < (maxcells >> 1)) hvwind = 2; }
      else { if (!edge); else if (edge < (maxcells >> 1)) hvwind = 2; else hvwind = 1; }
    }
    if (hvwind == 2 || (pv->reg[12] & 6) == 6 || (pv->reg[11] & 4) || pv->debug_p
#ifdef ARCDRAW_FALLBACK
        || 1
#endif
        ) {
      BackFill(bgc, sh, est);
      DrawDisplay(sh);
      sample_highcol();
      arcdraw_stat[1]++;
    }
#if defined(PLB) && !defined(ARCDRAW_C)
    else if (maxcells == 40 && !sh && !(off | on) && !((HighLnSpr[line][1] & SPRL_LO_ABOVE_HI) && (PicoIn.opt & POPT_ACC_SPRITES))) {
      plb_line(line, bgc, hvwind);
      return 1;
    }
#endif
#ifndef ARCDRAW_C
    else if (maxcells == 40) {
      u8 *sprited = &HighLnSpr[line][0];
      s32 *pre = HighPreSpr + (sprited[0] & 0x80) * 2;
#ifdef LREUSE
      if (!(off | on) && line == Pico.est.DrawScanline && LR_ON && ((lrst & 0x7f) < 3 || (lr_fc & 7) < 2)) {
        lr_key(line, sh, bgc, hvwind, sprited);
#ifdef LRK2
        lr_k2();
#endif
#ifdef LRADAPT
        lr_nchk++;
#endif
        if ((lrst & 0x80) && lr_hit(lrrow, sprited, pre)) { arcdraw_stat[5]++;
#ifdef LRADAPT
          lr_nhit++;
#endif
          lr_st[lrrow] = 0x80;
#ifdef LRCHECK
          lr_chk = 1;
#else
          return 1;
#endif
        } else if ((lrst & 0x7f) < 0x7f) lr_st[lrrow] = (lrst & 0x7f) + 1;
        lrsave = 1; lr_mask = 0;
      }
#endif
#ifndef PLBW
      memset(lb, bgc | (sh << 7), AW);
#endif
      line_asm(line, sh, hvwind);
      if ((sprited[1] & SPRL_LO_ABOVE_HI) && (PicoIn.opt & POPT_ACC_SPRITES)) { spr_as(sprited, pre, sh); arcdraw_stat[4]++; }
      else if (sh && (sprited[1] & SPRL_MAY_HAVE_OP)) 
#ifdef SHFAST
        spr_x(sprited, pre, 0, 4);
#else
        spr_shi(sprited, pre);
#endif
      else if (sprited[1] & SPRL_HAVE_HI) spr_all_asm(sprited, pre, 1, 0);
      arcdraw_stat[0]++;
      if (sh) arcdraw_stat[3]++;
#ifdef LREUSE
      if (lrsave) lr_save(lrrow, lrclk, sprited, pre);
#endif
    }
#endif
    else {
      u8 *sprited = &HighLnSpr[line][0];
      s32 *pre = HighPreSpr + (sprited[0] & 0x80) * 2;
      int nB, nA = 0, hiB, hiA = 0, diff = 0;
      memset(lb, bgc | (sh << 7), AW);
      nB = plane_lo(1, maxcells, maxw, sh, hcB, &hiB);
      if (hvwind == 1) diff = window_pass(0, maxcells >> 1, 0, sh, 0);
      else nA = plane_lo(0, maxcells, maxw, sh, hcA, &hiA);
      if (sprited[1] & SPRL_HAVE_LO) spr_all(sprited, pre, 0, sh);
      if (sh && (hiB || hiA) && (nB || nA)) {   /* PDRAW_PLANE_HI_PRIO: clear the whole line once */
        int i; for (i = 0; i < AW; i++) lb[i] &= 0x7f;
        plane_hi(hcB, nB, 0, maxw); plane_hi(hcA, nA, 0, maxw);
      } else {
        plane_hi(hcB, nB, sh, maxw);
        plane_hi(hcA, nA, sh, maxw);
      }
      if (hvwind == 1) window_pass(0, maxcells >> 1, 1, sh, diff);
      if ((sprited[1] & SPRL_LO_ABOVE_HI) && (PicoIn.opt & POPT_ACC_SPRITES)) { spr_as(sprited, pre, sh); arcdraw_stat[4]++; }
      else if (sh && (sprited[1] & SPRL_MAY_HAVE_OP)) spr_shi(sprited, pre);
      else if (sprited[1] & SPRL_HAVE_HI) spr_all(sprited, pre, 1, 0);
      arcdraw_stat[0]++;
      if (sh) arcdraw_stat[3]++;
    }
    if (off | on) {                    /* partial line blanking */
      int width = (pv->reg[12] & 1) ? 320 : 256;
      if (off > 0) fill(off, width, bgc);
      if (on > 0) fill(0, on, bgc);
    }
  }
#ifdef LREUSE
#ifdef LRCHECK
  if (lr_chk) { int i; lr_chk = 0;
    for (i = 0; i < AW && lb[i] == lr_lb[lrrow][i]; i++) ;
    if (i < AW && lr_bad++ < 12) printf("LRBAD clk %u L %d row %d col %d %02x/%02x\n", lrclk, Pico.est.DrawScanline, lrrow, i, lb[i], lr_lb[lrrow][i]);
  }
  if (lrsave) memcpy(lr_lb[lrrow], lb, AW);
#endif
#endif
#ifdef RSTAT
  { u32 h = 2166136261u; int i; for (i = 0; i < AW; i++) h = rs_mix(h, lb[i]);
    h = rs_mix(h, Pico.m.dirtyPal);
    if (rstat_on) { rstat[4]++; if (h == rs_lprev) rstat[5]++; if (h == rs_lfr[line & 255]) rstat[9]++; } rs_lprev = h; rs_lfr[line & 255] = h; }
#endif
#ifdef OUTQ
  arcdraw_qfin();
  arcdraw_q.src = lb; arcdraw_q.dst = (u32 *)dst; arcdraw_q.n = AW / 8;
  lbp = lbp == lbuf + 8 ? lbuf2 + 8 : lbuf + 8;
#else
  arcdraw_out(dst, lb, arcdraw_pal);
#endif
  return 1;
}
