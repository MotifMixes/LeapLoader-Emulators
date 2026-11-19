# mkvp.py: writes vp/videoport.c = PicoDrive's pico/videoport.c with VRAM write
# stamps for -DLREUSE (drawx.c): lr_gseq[g] = lr_clock when a write changes
# 2 KB group g (same-value writes leave it alone; DMA copy/fill and 128K mode
# stamp their whole range). Run from port/.
src = open('../picodrive/pico/videoport.c').read()
def rep(a, b, n=1):
    global src
    assert src.count(a) == n, (a, src.count(a))
    src = src.replace(a, b)
helpers = r'''
extern unsigned lr_clock, lr_gseq[32];
static void lr_range(u32 a, int len, int inc)
{
  u32 n = (u32)len * (inc ? inc : 1) + 2, g;
  if (n >= 0x10000) { for (g = 0; g < 32; g++) lr_gseq[g] = lr_clock; return; }
  a = (u16)a;
  for (g = a >> 11; g <= ((a + n) >> 11); g++) lr_gseq[g & 31] = lr_clock;
}
#ifdef PSPR2   /* -DPSPR2 (draw_patched.c ParseSprites): bumped by every SAT write and SAT register change */
unsigned spr_epoch;
#define UpdateSAT(a, d) (spr_epoch++, UpdateSAT(a, d))
#define SPR_EP spr_epoch++;
#else
#define SPR_EP
#endif
static inline void lr_w(u32 a, u16 d)
{
#ifdef PSPR2
  if (((a ^ SATaddr) & SATmask) == 0) spr_epoch++;
#endif
  if (PicoMem.vram[(u16)a >> 1] != d) lr_gseq[((u16)a >> 11) & 31] = lr_clock;
  VideoWriteVRAM(a, d);
}
static void lr_copy(char *d, const char *s, int n, u32 a)
{
  while (n > 0) {
    int c = 0x800 - (a & 0x7ff);
    if (c > n) c = n;
    { /* compare up to the first difference, then copy the rest; words when both are
         4-byte aligned, else halfwords (newlib's memcmp is not safe on 2-byte alignment) */
      int k = 0;
      if (!(((u32)d | (u32)s) & 3)) {
        const u32 *dd = (const u32 *)d, *ss = (const u32 *)s;
        while (k < (c >> 2) && dd[k] == ss[k]) k++;
        k <<= 2;
      } else {
        const u16 *dd = (const u16 *)d, *ss = (const u16 *)s;
        while (k < (c >> 1) && dd[k] == ss[k]) k++;
        k <<= 1;
      }
      if (k < c) { lr_gseq[(a >> 11) & 31] = lr_clock; memcpy(d + k, s + k, c - k); } }
    d += c; s += c; a += c; n -= c;
  }
}
'''
rep('#include "memory.h"\n', '#include "memory.h"\n' + helpers)
rep('            VideoWriteVRAM(a, d);', '            lr_w(a, d);')
rep('        VideoWriteVRAM(a, d);\n        // AutoIncrement', '        lr_w(a, d);\n        // AutoIncrement')
rep('        memcpy((char *)r + a, base + (source & mask), len * 2);', '        lr_copy((char *)r + a, (const char *)(base + (source & mask)), len * 2, a);')
rep('  // nasty\n', '  // nasty\n  lr_range(0, 0x10000, 1);\n')
rep('  source|=pvid->reg[0x16]<<8;\n\n  for (; len; len--)', '  source|=pvid->reg[0x16]<<8;\n  lr_range(a, len, inc);\n\n  for (; len; len--)')
rep('  len = GetDmaLength();\n  elprintf(EL_VDPDMA, "DmaFill', '  len = GetDmaLength();\n  if (pvid->type == 1 || pvid->type == 0x81) lr_range(a, len, inc);\n  elprintf(EL_VDPDMA, "DmaFill')
rep('Pico.est.rendstatus |= PDRAW_DIRTY_SPRITES;', '{ Pico.est.rendstatus |= PDRAW_DIRTY_SPRITES; SPR_EP }', 4)

fast = r"""
#ifdef K68POLL
int vdp_poll_quiet(void) { return !Pico.video.pending && !VdpFIFO.fifo_ql; }
int vdp_poll_status(void) { return Pico.video.status; }
#endif
#ifdef VDPFAST   /* -DVDPFAST: a 68k data-port write to VRAM (64K mode, no pending command, no DMA fill,
                    FIFO on) without the generic dispatch: the same steps, sync and FIFO bookkeeping inlined */
static inline void vdpf_sync(int skip)
{
  if (!(PicoIn.opt & POPT_ALT_RENDERER) && !PicoIn.skipFrame) {
    int lines = Pico.video.reg[1]&0x08 ? 240 : 224;
    int last = Pico.m.scanline - (skip > 0);
    if (last >= lines) last = lines-1;
    else Pico.est.rendstatus |= PDRAW_SYNC_NEXT;
    if ((linedisabled | lineenabled) >= 0 || Pico.est.DrawScanline <= last) { PicoVideoSync(skip); return; }
  }
  if (skip >= 0) Pico.est.rendstatus |= PDRAW_SYNC_NEEDED;
}
static inline int vdpf_fifo(void)   /* PicoVideoFIFOWrite(1, FQ_BYTE, 0, PVS_CPUWR) */
{
  struct VdpFIFO *vf = &VdpFIFO;
  struct PicoVideo *pv = &Pico.video;
  int lc = SekCyclesDone()-Pico.t.m68c_line_start;
  int burn = 0, x;
  if ((vf->fifo_ql >= 6) | (pv->status & PVS_DMABG))
    PicoVideoFIFOSync(lc);
  x = (vf->fifo_qx + vf->fifo_ql - 1 - !!(pv->status & PVS_DMABG)) & 7;
  pv->status |= PVS_CPUWR;
  vf->fifo_total += 1;
  if (!vf->fifo_ql)
    vf->fifo_slot = Cyc2Sl(vf, lc+7);
  if (vf->fifo_queue[x] && (vf->fifo_queue[x] & 7) == FQ_BYTE) {
    vf->fifo_queue[x] += 1 << 4;
  } else {
    vf->fifo_ql ++;
    x = (x+1) & 7;
    vf->fifo_queue[(x+1)&7] = vf->fifo_queue[x];
    vf->fifo_queue[x] = (1 << 4) | FQ_BYTE;
  }
  x = (Cyc2Sl(vf,lc) - vf->fifo_slot) / 2;
  if (vf->fifo_total > 4 + x)
    burn = PicoVideoFIFODrain(4, lc, 0);
  return burn;
}
static inline void vdpf_vram(struct PicoVideo *pvid, u16 d)
{
  u32 a = pvid->addr;
  if ((pvid->reg[1]&0x40) && !(!(a&1) && ((a^SATaddr)&SATmask) && PicoMem.vram[a>>1] == d))
    vdpf_sync(InHblank(30));
  VdpFIFO.fifo_data[++VdpFIFO.fifo_dx&3] = d;
  SekCyclesBurnRun(vdpf_fifo());
  if (pvid->type == 1) {
    a = pvid->addr;
    if (a & 1)
      d = (u16)((d << 8) | (d >> 8));
    a |= pvid->addr_u << 16;
    lr_w(a, d);
    AutoIncrement();
  } else
    VideoWrite(d);
  if (pvid->status & PVS_DMAFILL)
    DmaFill(VdpFIFO.fifo_data[(VdpFIFO.fifo_dx + !!(pvid->type&~0x81))&3]);
}
static NOINLINE void PicoVideoWrite0(u32 a,unsigned short d);
PICO_INTERNAL_ASM void PicoVideoWrite(u32 a,unsigned short d)
{
  struct PicoVideo *pvid=&Pico.video;
  if (!(a & 0x1c) && pvid->type == 1 && !pvid->pending && !(pvid->status & PVS_DMAFILL) && !(PicoIn.opt&POPT_DIS_VDP_FIFO))
    vdpf_vram(pvid, d);
  else
    PicoVideoWrite0(a, d);
}
#define PicoVideoWrite PicoVideoWrite0
static
#endif
"""
rep('PICO_INTERNAL_ASM void PicoVideoWrite(u32 a,unsigned short d)\n{', fast + 'PICO_INTERNAL_ASM void PicoVideoWrite(u32 a,unsigned short d)\n{')
src += r"""
#ifdef FSKIP
/* -DFSKIP (main.c, after a frame run with PicoIn.skipFrame): a display on/off event pending at or before the
   last visible line would have been consumed by the frame's closing PicoVideoSync(-1) had the frame been drawn
   (later ones stay pending, as then); without this a skipped frame's event lands in the next drawn frame */
__attribute__((section(".xtext.vpskip"))) void PicoVideoSkipped(void)
{
  int lines = Pico.video.reg[1] & 0x08 ? 240 : 224;
  if (linedisabled >= 0 && linedisabled <= lines - 1) linedisabled = -1;
  if (lineenabled >= 0 && lineenabled <= lines - 1) lineenabled = -1;
}
#endif
"""
open('vp/videoport.c', 'w', newline=chr(10)).write('/* generated by vp/mkvp.py from ../picodrive/pico/videoport.c */\n' + src)
