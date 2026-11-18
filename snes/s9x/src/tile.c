/*
 * Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
 *
 * (c) Copyright 1996 - 2001 Gary Henderson (gary.henderson@ntlworld.com) and
 *                           Jerremy Koot (jkoot@snes9x.com)
 *
 * Super FX C emulator code
 * (c) Copyright 1997 - 1999 Ivar (ivar@snes9x.com) and
 *                           Gary Henderson.
 * Super FX assembler emulator code (c) Copyright 1998 zsKnight and _Demo_.
 *
 * DSP1 emulator code (c) Copyright 1998 Ivar, _Demo_ and Gary Henderson.
 * C4 asm and some C emulation code (c) Copyright 2000 zsKnight and _Demo_.
 * C4 C code (c) Copyright 2001 Gary Henderson (gary.henderson@ntlworld.com).
 *
 * (c) Copyright 2014 - 2016 Daniel De Matteis. (UNDER NO CIRCUMSTANCE 
 * WILL COMMERCIAL RIGHTS EVER BE APPROPRIATED TO ANY PARTY)
 *
 * DOS port code contains the works of other authors. See headers in
 * individual files.
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
#include "ppu.h"
#include "display.h"
#include "gfx.h"
#include "tile.h"

extern uint32 HeadMask [4];
extern uint32 TailMask [5];

#ifdef LL_NIB
/* LeapLoader: a second tile cache for 2 and 4 bpp tiles, one word per row with
   pixel i in bits 4i..4i+3 (32 bytes per tile, half of the 8 bpp cache's 64):
   DrawTile16's shown-pixel path reads it, so a tile row costs half the data
   cache lines. Written by ConvertTile together with the 8 bpp one, so both are
   valid whenever BG.Buffered says so. */
uint32 LLNib4[2048 * 8] __attribute__((aligned(32)));
uint32 LLNib2[4096 * 8] __attribute__((aligned(32)));
static uint32 LLNibT[256];
#ifdef LLR_NIBONLY
/* -DLLR_NIBONLY: the new renderer converts 2/4 bpp tiles into LLNib4/2 only (LLConvertNib) and marks them
   here; the old renderer converts a marked tile again (ConvertTile clears the mark) before using it */
uint8 LLNibOnly[3][4096];
uint8* LLBufNib = LLNibOnly[TILE_4BIT];
#endif
#endif
uint8 ConvertTile(uint8* pCache, uint32 TileAddr)
{
   uint8* tp = &Memory.VRAM[TileAddr];
   uint32* p = (uint32*) pCache;
   uint32 non_zero = 0;
   uint8 line;
   uint32 p1;
   uint32 p2;
   uint8 pix;

   switch (BG.BitShift)
   {
   case 8:
      for (line = 8; line != 0; line--, tp += 2)
      {
         p1 = p2 = 0;
         if ((pix = *(tp + 0)))
         {
            p1 |= odd_high[0][pix >> 4];
            p2 |= odd_low[0][pix & 0xf];
         }
         if ((pix = *(tp + 1)))
         {
            p1 |= even_high[0][pix >> 4];
            p2 |= even_low[0][pix & 0xf];
         }
         if ((pix = *(tp + 16)))
         {
            p1 |= odd_high[1][pix >> 4];
            p2 |= odd_low[1][pix & 0xf];
         }
         if ((pix = *(tp + 17)))
         {
            p1 |= even_high[1][pix >> 4];
            p2 |= even_low[1][pix & 0xf];
         }
         if ((pix = *(tp + 32)))
         {
            p1 |= odd_high[2][pix >> 4];
            p2 |= odd_low[2][pix & 0xf];
         }
         if ((pix = *(tp + 33)))
         {
            p1 |= even_high[2][pix >> 4];
            p2 |= even_low[2][pix & 0xf];
         }
         if ((pix = *(tp + 48)))
         {
            p1 |= odd_high[3][pix >> 4];
            p2 |= odd_low[3][pix & 0xf];
         }
         if ((pix = *(tp + 49)))
         {
            p1 |= even_high[3][pix >> 4];
            p2 |= even_low[3][pix & 0xf];
         }
         *p++ = p1;
         *p++ = p2;
         non_zero |= p1 | p2;
      }
      break;

   case 4:
      for (line = 8; line != 0; line--, tp += 2)
      {
         p1 = p2 = 0;
         if ((pix = *(tp + 0)))
         {
            p1 |= odd_high[0][pix >> 4];
            p2 |= odd_low[0][pix & 0xf];
         }
         if ((pix = *(tp + 1)))
         {
            p1 |= even_high[0][pix >> 4];
            p2 |= even_low[0][pix & 0xf];
         }
         if ((pix = *(tp + 16)))
         {
            p1 |= odd_high[1][pix >> 4];
            p2 |= odd_low[1][pix & 0xf];
         }
         if ((pix = *(tp + 17)))
         {
            p1 |= even_high[1][pix >> 4];
            p2 |= even_low[1][pix & 0xf];
         }
         *p++ = p1;
         *p++ = p2;
         non_zero |= p1 | p2;
      }
      break;

   case 2:
      for (line = 8; line != 0; line--, tp += 2)
      {
         p1 = p2 = 0;
         if ((pix = *(tp + 0)))
         {
            p1 |= odd_high[0][pix >> 4];
            p2 |= odd_low[0][pix & 0xf];
         }
         if ((pix = *(tp + 1)))
         {
            p1 |= even_high[0][pix >> 4];
            p2 |= even_low[0][pix & 0xf];
         }
         *p++ = p1;
         *p++ = p2;
         non_zero |= p1 | p2;
      }
      break;
   }
#ifdef LL_NIB
   if (BG.BitShift != 8)
   {
      uint32* n;
      if (!LLNibT[1])
      {
         uint32 v, i;
         for (v = 0; v < 256; v++)
            for (LLNibT[v] = 0, i = 0; i < 8; i++)
               LLNibT[v] |= ((v >> (7 - i)) & 1) << (4 * i);
      }
      tp = &Memory.VRAM[TileAddr];
#ifdef LLR_NIBONLY
      LLNibOnly[BG.BitShift == 4 ? TILE_4BIT : TILE_2BIT][TileAddr >> (BG.BitShift == 4 ? 5 : 4)] = 0;
#endif
      if (BG.BitShift == 4)
         for (n = LLNib4 + (TileAddr >> 5) * 8, line = 8; line != 0; line--, tp += 2)
            *n++ = LLNibT[tp[0]] | LLNibT[tp[1]] << 1 | LLNibT[tp[16]] << 2 | LLNibT[tp[17]] << 3;
      else
         for (n = LLNib2 + (TileAddr >> 4) * 8, line = 8; line != 0; line--, tp += 2)
            *n++ = LLNibT[tp[0]] | LLNibT[tp[1]] << 1;
   }
#endif
   return (non_zero ? TRUE : BLANK_TILE);
}
#if defined LL_NIB && defined LLR_NIBONLY
uint8 LLConvertNib(uint32 depth, uint32 t)
{
   const uint8* tp;
   uint32 *n, line, nz = 0;
   if (!LLNibT[1])
   {
      uint32 v, i;
      for (v = 0; v < 256; v++)
         for (LLNibT[v] = 0, i = 0; i < 8; i++)
            LLNibT[v] |= ((v >> (7 - i)) & 1) << (4 * i);
   }
   if (depth == TILE_4BIT)
      for (tp = &Memory.VRAM[t << 5], n = LLNib4 + t * 8, line = 8; line != 0; line--, tp += 2)
         nz |= *n++ = LLNibT[tp[0]] | LLNibT[tp[1]] << 1 | LLNibT[tp[16]] << 2 | LLNibT[tp[17]] << 3;
   else
      for (tp = &Memory.VRAM[t << 4], n = LLNib2 + t * 8, line = 8; line != 0; line--, tp += 2)
         nz |= *n++ = LLNibT[tp[0]] | LLNibT[tp[1]] << 1;
   LLNibOnly[depth][t] = 1;
   return nz ? TRUE : BLANK_TILE;
}
#endif

void SelectPalette()
{
   // GFX.ScreenColors = &GFX.ScreenColorsPre[(Tile & GFX.PaletteMask) >> GFX.PaletteShift];
   if (BG.DirectColourMode)
   {
      // GFX.ScreenColors = DirectColourMaps [(Tile >> 10) & BG.PaletteMask];

      GFX.ScreenColorsPre = DirectColourMaps[0];
      LL_LIT();
      GFX.PaletteMask = BG.PaletteMask << 10;
      GFX.PaletteShift = 10;
   }
   else
   {
      // GFX.ScreenColors = &IPPU.ScreenColors [(((Tile >> 10) & BG.PaletteMask) << BG.PaletteShift) + BG.StartPalette];

      GFX.ScreenColorsPre = &IPPU.ScreenColors[BG.StartPalette];
      GFX.PaletteMask = BG.PaletteMask << 10;
      GFX.PaletteShift = 10 - BG.PaletteShift;
   }

}


static INLINE void WRITE_4PIXELS(int32 Offset, uint8* Pixels)
{
   uint8 Pixel;
   uint8* Screen = GFX.S + Offset;
   uint8* Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N])) \
    { \
   Screen [N] = (uint8) GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELS_FLIPPED(int32 Offset, uint8* Pixels)
{
   uint8 Pixel;
   uint8* Screen = GFX.S + Offset;
   uint8* Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N])) \
    { \
   Screen [N] = (uint8) GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELSHI16(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[2*N])) \
    { \
   Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELSHI16_FLIPPED(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[6 - 2*N])) \
    { \
   Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELSx2(int32 Offset, uint8* Pixels)
{
   uint8 Pixel;
   uint8* Screen = GFX.S + Offset;
   uint8* Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [0] && (Pixel = Pixels[N])) \
    { \
   Screen [N * 2] = Screen [N * 2 + 1] = (uint8) GFX.ScreenColors [Pixel]; \
   Depth [N * 2] = Depth [N * 2 + 1] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELS_FLIPPEDx2(int32 Offset, uint8* Pixels)
{
   uint8 Pixel;
   uint8* Screen = GFX.S + Offset;
   uint8* Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[3 - N])) \
    { \
   Screen [N * 2] = Screen [N * 2 + 1] = (uint8) GFX.ScreenColors [Pixel]; \
   Depth [N * 2] = Depth [N * 2 + 1] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELSx2x2(int32 Offset, uint8* Pixels)
{
   uint8 Pixel;
   uint8* Screen = GFX.S + Offset;
   uint8* Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[N])) \
    { \
   Screen [N * 2] = Screen [N * 2 + 1] = Screen [GFX_PITCH + N * 2] =  \
       Screen [GFX_PITCH + N * 2 + 1] = (uint8) GFX.ScreenColors [Pixel]; \
   Depth [N * 2] = Depth [N * 2 + 1] = Depth [GFX_PITCH + N * 2] = \
       Depth [GFX_PITCH + N * 2 + 1] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELS_FLIPPEDx2x2(int32 Offset, uint8* Pixels)
{
   uint8 Pixel;
   uint8* Screen = GFX.S + Offset;
   uint8* Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[3 - N])) \
    { \
   Screen [N * 2] = Screen [N * 2 + 1] = Screen [GFX_PITCH + N * 2] =  \
       Screen [GFX_PITCH + N * 2 + 1] = (uint8) GFX.ScreenColors [Pixel]; \
   Depth [N * 2] = Depth [N * 2 + 1] = Depth [GFX_PITCH + N * 2] = \
       Depth [GFX_PITCH + N * 2 + 1] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

void DrawTile(uint32 Tile, int32 Offset, uint32 StartLine,
              uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE

   RENDER_TILE(WRITE_4PIXELS, WRITE_4PIXELS_FLIPPED, 4)
}

void DrawClippedTile(uint32 Tile, int32 Offset,
                     uint32 StartPixel, uint32 Width,
                     uint32 StartLine, uint32 LineCount)
{
   uint32 dd, d1, d2;
   uint8* bp;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELS, WRITE_4PIXELS_FLIPPED, 4)
}

void DrawTilex2(uint32 Tile, int32 Offset, uint32 StartLine,
                uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE
   RENDER_TILE(WRITE_4PIXELSx2, WRITE_4PIXELS_FLIPPEDx2, 8)
}

void DrawClippedTilex2(uint32 Tile, int32 Offset,
                       uint32 StartPixel, uint32 Width,
                       uint32 StartLine, uint32 LineCount)
{
   uint32 dd, d1, d2;
   uint8* bp;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELSx2, WRITE_4PIXELS_FLIPPEDx2, 8)
}

void DrawTilex2x2(uint32 Tile, int32 Offset, uint32 StartLine,
                  uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE
   RENDER_TILE(WRITE_4PIXELSx2x2, WRITE_4PIXELS_FLIPPEDx2x2, 8)
}

void DrawClippedTilex2x2(uint32 Tile, int32 Offset,
                         uint32 StartPixel, uint32 Width,
                         uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
   uint32 dd, d1, d2;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELSx2x2, WRITE_4PIXELS_FLIPPEDx2x2, 8)
}

void DrawLargePixel(uint32 Tile, int32 Offset,
                    uint32 StartPixel, uint32 Pixels,
                    uint32 StartLine, uint32 LineCount)
{
   uint8 *sp, *Depth;
   uint8 pixel;
   TILE_PREAMBLE
   sp = GFX.S + Offset;
   Depth = GFX.DB + Offset;
#define PLOT_PIXEL(screen, pixel) (pixel)

   RENDER_TILE_LARGE(((uint8) GFX.ScreenColors [pixel]), PLOT_PIXEL)
}

static INLINE void WRITE_4PIXELS16(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N])) \
    { \
   Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELS16_FLIPPED(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N])) \
    { \
   Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELS16x2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[N])) \
    { \
   Screen [N * 2] = Screen [N * 2 + 1] = GFX.ScreenColors [Pixel]; \
   Depth [N * 2] = Depth [N * 2 + 1] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELS16_FLIPPEDx2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[3 - N])) \
    { \
   Screen [N * 2] = Screen [N * 2 + 1] = GFX.ScreenColors [Pixel]; \
   Depth [N * 2] = Depth [N * 2 + 1] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELS16x2x2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[N])) \
    { \
   Screen [N * 2] = Screen [N * 2 + 1] = Screen [(GFX_PITCH >> 1) + N * 2] = \
       Screen [(GFX_PITCH >> 1) + N * 2 + 1] = GFX.ScreenColors [Pixel]; \
   Depth [N * 2] = Depth [N * 2 + 1] = Depth [(GFX_PITCH >> 1) + N * 2] = \
       Depth [(GFX_PITCH >> 1) + N * 2 + 1] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

static INLINE void WRITE_4PIXELS16_FLIPPEDx2x2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.DB + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[3 - N])) \
    { \
   Screen [N * 2] = Screen [N * 2 + 1] = Screen [(GFX_PITCH >> 1) + N * 2] = \
       Screen [(GFX_PITCH >> 1) + N * 2 + 1] = GFX.ScreenColors [Pixel]; \
   Depth [N * 2] = Depth [N * 2 + 1] = Depth [(GFX_PITCH >> 1) + N * 2] = \
       Depth [(GFX_PITCH >> 1) + N * 2 + 1] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)
#undef FN
}

#ifdef LL_FASTTILE
/* LeapLoader: DrawTile16 with GFX's fields in locals and the pixels taken from
   the two words already loaded: the stores through Screen and Depth made GCC
   reload GFX.Z1/Z2/ScreenColors for every pixel, each a load-use stall. A
   line's depth bytes are loaded first (ARC600: a load's result is ready 3
   cycles later). LL_LCD: lines that LLOut doesn't show are skipped. */
#define LLPIX(i, w, sh) \
   if (z1 > d##i && (p = (w >> sh) & 0xff)) \
   { \
      s[i] = colors[p]; \
      d[i] = z2; \
   }
#define LLDEPTH \
   d0 = d[0], d1 = d[1], d2 = d[2], d3 = d[3]; \
   d4 = d[4], d5 = d[5], d6 = d[6], d7 = d[7];
#ifdef LL_LCD
/* LL_LCD without transparency: only screen columns 0, 1, 3, 4, 6 (mod 8) are
   in LLOut, so a tile has exactly 5 shown pixels; one loop per tile column
   (Offset & 7) and flip draws only those (generated, see LLROW) */
#define LLP(N, W, SH)    if (z1 > d[N] && (p = (W >> SH) & 0xff))    {       s[N] = colors[p];       d[N] = z2;    }
#define LLROW(PIXELS)    for (l = LineCount; l != 0; l--, bp += inc, s += GFX_PPL, d += GFX_PPL)    {       if (!*show++)          continue;       a = ((uint32*) bp)[0];       b = ((uint32*) bp)[1];       if (a | b)       {          PIXELS       }    }
#ifdef LL_NIB
#include "llnib.h"
#ifdef LL_PALIDX
/* the colours are CG-RAM indices: no table lookup (cb = 0x100 | first index) */
#define LLPN(N, SH, E) if (z1 > E && (p = (a >> SH) & 15)) { s[N] = cb + p; d[N] = z2; }
#else
#define LLPN(N, SH, E) if (z1 > E && (p = (a >> SH) & 15)) { s[N] = colors[p]; d[N] = z2; }
#endif
#define LLROWN(PRE, PIXELS) for (l = LineCount; l != 0; l--, nb += inc, s += GFX_PPL, d += GFX_PPL) { if (!*show++) continue; a = *nb; if (a) { PRE PIXELS } }
#endif
#define LLSHOWN       if (!Settings.Transparency && !IPPU.DoubleWidthPixels)       {          switch ((Offset & 7) | (Tile & H_FLIP ? 8 : 0))          { \
         case 0: LLROW(LLP(0, a, 0) LLP(1, a, 8) LLP(3, a, 24) LLP(4, b, 0) LLP(6, b, 16)) return; \
         case 1: LLROW(LLP(0, a, 0) LLP(2, a, 16) LLP(3, a, 24) LLP(5, b, 8) LLP(7, b, 24)) return; \
         case 2: LLROW(LLP(1, a, 8) LLP(2, a, 16) LLP(4, b, 0) LLP(6, b, 16) LLP(7, b, 24)) return; \
         case 3: LLROW(LLP(0, a, 0) LLP(1, a, 8) LLP(3, a, 24) LLP(5, b, 8) LLP(6, b, 16)) return; \
         case 4: LLROW(LLP(0, a, 0) LLP(2, a, 16) LLP(4, b, 0) LLP(5, b, 8) LLP(7, b, 24)) return; \
         case 5: LLROW(LLP(1, a, 8) LLP(3, a, 24) LLP(4, b, 0) LLP(6, b, 16) LLP(7, b, 24)) return; \
         case 6: LLROW(LLP(0, a, 0) LLP(2, a, 16) LLP(3, a, 24) LLP(5, b, 8) LLP(6, b, 16)) return; \
         case 7: LLROW(LLP(1, a, 8) LLP(2, a, 16) LLP(4, b, 0) LLP(5, b, 8) LLP(7, b, 24)) return; \
         case 8: LLROW(LLP(0, b, 24) LLP(1, b, 16) LLP(3, b, 0) LLP(4, a, 24) LLP(6, a, 8)) return; \
         case 9: LLROW(LLP(0, b, 24) LLP(2, b, 8) LLP(3, b, 0) LLP(5, a, 16) LLP(7, a, 0)) return; \
         case 10: LLROW(LLP(1, b, 16) LLP(2, b, 8) LLP(4, a, 24) LLP(6, a, 8) LLP(7, a, 0)) return; \
         case 11: LLROW(LLP(0, b, 24) LLP(1, b, 16) LLP(3, b, 0) LLP(5, a, 16) LLP(6, a, 8)) return; \
         case 12: LLROW(LLP(0, b, 24) LLP(2, b, 8) LLP(4, a, 24) LLP(5, a, 16) LLP(7, a, 0)) return; \
         case 13: LLROW(LLP(1, b, 16) LLP(3, b, 0) LLP(4, a, 24) LLP(6, a, 8) LLP(7, a, 0)) return; \
         case 14: LLROW(LLP(0, b, 24) LLP(2, b, 8) LLP(3, b, 0) LLP(5, a, 16) LLP(6, a, 8)) return; \
         case 15: LLROW(LLP(1, b, 16) LLP(2, b, 8) LLP(4, a, 24) LLP(5, a, 16) LLP(7, a, 0)) return; \
         }       }
#else
#define LLSHOWN
#endif
void DrawTile16(uint32 Tile, int32 Offset, uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
#ifdef LL_HIDESKIP
   /* 1 or 2 lines, none of them shown: nothing to do (LLShow is all 1s with
      transparency; never 3 hidden lines in a row) */
   if (LineCount <= 2 && !IPPU.DoubleWidthPixels && !IPPU.LatchedInterlace)
   {
      extern uint8 LLShow[];
      uint32 y = (uint32) Offset / GFX_PPL;
      if (!LLShow[y] && !LLShow[y + LineCount - 1])
         return;
   }
#endif
   TILE_PREAMBLE
   {
      const uint32* colors = GFX.ScreenColors;
      uint16* s = (uint16*) GFX.S + Offset;
      uint8* d = GFX.DB + Offset;
      uint32 z1 = GFX.Z1, z2 = GFX.Z2, p, a, b, d0, d1, d2, d3, d4, d5, d6, d7;
      int inc = 8;
#ifdef LL_LCD
      extern uint8 LLShow[];
      const uint8* show = LLShow + Offset / GFX_PPL;
#endif
#ifdef LL_UPDCOUNT
      { extern unsigned LLT16Calls, LLT16Lines; LLT16Calls++; LLT16Lines += LineCount; }
#endif
#if defined(LL_NIB) && defined(LL_LCD)
#ifdef LL_PALIDX
      uint32 cb = 0x100 + (uint32)(colors - IPPU.ScreenColors);
      if (BG.BitShift != 8 && !Settings.Transparency && !IPPU.DoubleWidthPixels && cb - 0x100 < 256)
#else
      if (BG.BitShift != 8 && !Settings.Transparency && !IPPU.DoubleWidthPixels)
#endif
      {
         const uint32* nb = (BG.BitShift == 4 ? LLNib4 : LLNib2) + TileNumber * 8 + (StartLine >> 3);
         if (Tile & V_FLIP)
         {
            nb = (BG.BitShift == 4 ? LLNib4 : LLNib2) + TileNumber * 8 + 7 - (StartLine >> 3);
            inc = -1;
         }
         else
            inc = 1;
         LLNIBSHOWN
      }
#endif
      bp = pCache + StartLine;
      if (Tile & V_FLIP)
      {
         bp = pCache + 56 - StartLine;
         inc = -8;
      }
      LLSHOWN
      if (!(Tile & H_FLIP))
         for (l = LineCount; l != 0; l--, bp += inc, s += GFX_PPL, d += GFX_PPL)
         {
#ifdef LL_LCD
            if (!*show++)
               continue;
#endif
            a = ((uint32*) bp)[0];
            b = ((uint32*) bp)[1];
            LLDEPTH
            if (a)
            {
               LLPIX(0, a, 0) LLPIX(1, a, 8) LLPIX(2, a, 16) LLPIX(3, a, 24)
            }
            if (b)
            {
               LLPIX(4, b, 0) LLPIX(5, b, 8) LLPIX(6, b, 16) LLPIX(7, b, 24)
            }
         }
      else
         for (l = LineCount; l != 0; l--, bp += inc, s += GFX_PPL, d += GFX_PPL)
         {
#ifdef LL_LCD
            if (!*show++)
               continue;
#endif
            a = ((uint32*) bp)[0];
            b = ((uint32*) bp)[1];
            LLDEPTH
            if (b)
            {
               LLPIX(0, b, 24) LLPIX(1, b, 16) LLPIX(2, b, 8) LLPIX(3, b, 0)
            }
            if (a)
            {
               LLPIX(4, a, 24) LLPIX(5, a, 16) LLPIX(6, a, 8) LLPIX(7, a, 0)
            }
         }
   }
}
#else
void DrawTile16(uint32 Tile, int32 Offset, uint32 StartLine, uint32 LineCount)
{
   uint8*  bp;
#if 1
   TILE_PREAMBLE
#else

   uint8* pCache;
   uint32 TileAddr = BG.TileAddress + ((Tile & 0x3ff) << BG.TileShift);

   TileAddr &= 0xffff;

   uint32 TileNumber;
   pCache = &BG.Buffer[(TileNumber = (TileAddr >> BG.TileShift)) << 6];

#ifdef LLR_NIBONLY
   if (!BG.Buffered [TileNumber] || LLBufNib[TileNumber])
#else
   if (!BG.Buffered [TileNumber])
#endif
      BG.Buffered[TileNumber] = ConvertTile(pCache, TileAddr);

   if (BG.Buffered [TileNumber] == BLANK_TILE)
   {
      TileBlank = Tile & 0xFFFFFFFF;
      return;
   }

   if (BG.DirectColourMode)
   {
      if (IPPU.DirectColourMapsNeedRebuild)
         S9xBuildDirectColourMaps();
      GFX.ScreenColors = DirectColourMaps [(Tile >> 10) & BG.PaletteMask];
   }
   else
      GFX.ScreenColors = &IPPU.ScreenColors [(((Tile >> 10) & BG.PaletteMask) << BG.PaletteShift) + BG.StartPalette];

//      GFX.ScreenColors = &GFX.ScreenColorsPre[(Tile & GFX.PaletteMask) >> GFX.PaletteShift];

   uint32 l;
#endif
   RENDER_TILE(WRITE_4PIXELS16, WRITE_4PIXELS16_FLIPPED, 4)
}
#endif

void DrawClippedTile16(uint32 Tile, int32 Offset,
                       uint32 StartPixel, uint32 Width,
                       uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
   uint32 dd, d1, d2;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELS16, WRITE_4PIXELS16_FLIPPED, 4)
}

void DrawTile16x2(uint32 Tile, int32 Offset, uint32 StartLine,
                  uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE
   RENDER_TILE(WRITE_4PIXELS16x2, WRITE_4PIXELS16_FLIPPEDx2, 8)
}

void DrawClippedTile16x2(uint32 Tile, int32 Offset,
                         uint32 StartPixel, uint32 Width,
                         uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
   uint32 dd, d1, d2;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELS16x2, WRITE_4PIXELS16_FLIPPEDx2, 8)
}

void DrawTile16x2x2(uint32 Tile, int32 Offset, uint32 StartLine,
                    uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE
   RENDER_TILE(WRITE_4PIXELS16x2x2, WRITE_4PIXELS16_FLIPPEDx2x2, 8)
}

void DrawClippedTile16x2x2(uint32 Tile, int32 Offset,
                           uint32 StartPixel, uint32 Width,
                           uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
   uint32 dd, d1, d2;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELS16x2x2, WRITE_4PIXELS16_FLIPPEDx2x2, 8)
}

void DrawLargePixel16(uint32 Tile, int32 Offset,
                      uint32 StartPixel, uint32 Pixels,
                      uint32 StartLine, uint32 LineCount)
{
   uint16 pixel;
   uint16 *sp;
   uint8 *Depth;
   TILE_PREAMBLE
   sp = (uint16*) GFX.S + Offset;
   Depth = GFX.DB + Offset;
   RENDER_TILE_LARGE(GFX.ScreenColors [pixel], PLOT_PIXEL)
}

static INLINE void WRITE_4PIXELS16_ADD(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N])) \
    { \
   if (SubDepth [N]) \
   { \
       if (SubDepth [N] != 1) \
      Screen [N] = COLOR_ADD (GFX.ScreenColors [Pixel], \
               Screen [GFX.Delta + N]); \
       else \
      Screen [N] = COLOR_ADD (GFX.ScreenColors [Pixel], \
               GFX.FixedColour); \
   } \
   else \
       Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}

static INLINE void WRITE_4PIXELS16_FLIPPED_ADD(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N])) \
    { \
   if (SubDepth [N]) \
   { \
       if (SubDepth [N] != 1) \
      Screen [N] = COLOR_ADD (GFX.ScreenColors [Pixel], \
               Screen [GFX.Delta + N]); \
       else \
      Screen [N] = COLOR_ADD (GFX.ScreenColors [Pixel], \
               GFX.FixedColour); \
   } \
   else \
       Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}

static INLINE void WRITE_4PIXELS16_ADD1_2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N])) \
    { \
   if (SubDepth [N]) \
   { \
       if (SubDepth [N] != 1) \
      Screen [N] = (uint16) (COLOR_ADD1_2 (GFX.ScreenColors [Pixel], \
                       Screen [GFX.Delta + N])); \
       else \
      Screen [N] = COLOR_ADD (GFX.ScreenColors [Pixel], \
               GFX.FixedColour); \
   } \
   else \
       Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}

static INLINE void WRITE_4PIXELS16_FLIPPED_ADD1_2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N])) \
    { \
   if (SubDepth [N]) \
   { \
       if (SubDepth [N] != 1) \
      Screen [N] = (uint16) (COLOR_ADD1_2 (GFX.ScreenColors [Pixel], \
                       Screen [GFX.Delta + N])); \
       else \
      Screen [N] = COLOR_ADD (GFX.ScreenColors [Pixel], \
               GFX.FixedColour); \
   } \
   else \
       Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}

static INLINE void WRITE_4PIXELS16_SUB(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N])) \
    { \
   if (SubDepth [N]) \
   { \
       if (SubDepth [N] != 1) \
      Screen [N] = (uint16) COLOR_SUB (GFX.ScreenColors [Pixel], \
               Screen [GFX.Delta + N]); \
       else \
      Screen [N] = (uint16) COLOR_SUB (GFX.ScreenColors [Pixel], \
               GFX.FixedColour); \
   } \
   else \
       Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}

static INLINE void WRITE_4PIXELS16_FLIPPED_SUB(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N])) \
    { \
   if (SubDepth [N]) \
   { \
       if (SubDepth [N] != 1) \
      Screen [N] = (uint16) COLOR_SUB (GFX.ScreenColors [Pixel], \
               Screen [GFX.Delta + N]); \
       else \
      Screen [N] = (uint16) COLOR_SUB (GFX.ScreenColors [Pixel], \
               GFX.FixedColour); \
   } \
   else \
       Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}

static INLINE void WRITE_4PIXELS16_SUB1_2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N])) \
    { \
   if (SubDepth [N]) \
   { \
       if (SubDepth [N] != 1) \
      Screen [N] = (uint16) COLOR_SUB1_2 (GFX.ScreenColors [Pixel], \
                  Screen [GFX.Delta + N]); \
       else \
      Screen [N] = (uint16) COLOR_SUB (GFX.ScreenColors [Pixel], \
               GFX.FixedColour); \
   } \
   else \
       Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}

static INLINE void WRITE_4PIXELS16_FLIPPED_SUB1_2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N])) \
    { \
   if (SubDepth [N]) \
   { \
       if (SubDepth [N] != 1) \
      Screen [N] = (uint16) COLOR_SUB1_2 (GFX.ScreenColors [Pixel], \
                  Screen [GFX.Delta + N]); \
       else \
      Screen [N] = (uint16) COLOR_SUB (GFX.ScreenColors [Pixel], \
               GFX.FixedColour); \
   } \
   else \
       Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}


void DrawTile16Add(uint32 Tile, int32 Offset, uint32 StartLine,
                   uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE
   RENDER_TILE(WRITE_4PIXELS16_ADD, WRITE_4PIXELS16_FLIPPED_ADD, 4)
}

void DrawClippedTile16Add(uint32 Tile, int32 Offset,
                          uint32 StartPixel, uint32 Width,
                          uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
   uint32 dd, d1, d2;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELS16_ADD, WRITE_4PIXELS16_FLIPPED_ADD, 4)
}

void DrawTile16Add1_2(uint32 Tile, int32 Offset, uint32 StartLine,
                      uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE
   RENDER_TILE(WRITE_4PIXELS16_ADD1_2, WRITE_4PIXELS16_FLIPPED_ADD1_2, 4)
}

void DrawClippedTile16Add1_2(uint32 Tile, int32 Offset,
                             uint32 StartPixel, uint32 Width,
                             uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
   uint32 dd, d1, d2;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELS16_ADD1_2, WRITE_4PIXELS16_FLIPPED_ADD1_2, 4)
}

void DrawTile16Sub(uint32 Tile, int32 Offset, uint32 StartLine,
                   uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE
   RENDER_TILE(WRITE_4PIXELS16_SUB, WRITE_4PIXELS16_FLIPPED_SUB, 4)
}

void DrawClippedTile16Sub(uint32 Tile, int32 Offset,
                          uint32 StartPixel, uint32 Width,
                          uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
   uint32 dd, d1, d2;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELS16_SUB, WRITE_4PIXELS16_FLIPPED_SUB, 4)
}

void DrawTile16Sub1_2(uint32 Tile, int32 Offset, uint32 StartLine,
                      uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE
   RENDER_TILE(WRITE_4PIXELS16_SUB1_2, WRITE_4PIXELS16_FLIPPED_SUB1_2, 4)
}

void DrawClippedTile16Sub1_2(uint32 Tile, int32 Offset,
                             uint32 StartPixel, uint32 Width,
                             uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
   uint32 dd, d1, d2;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELS16_SUB1_2, WRITE_4PIXELS16_FLIPPED_SUB1_2, 4)
}

static INLINE void WRITE_4PIXELS16_ADDF1_2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N])) \
    { \
   if (SubDepth [N] == 1) \
       Screen [N] = (uint16) (COLOR_ADD1_2 (GFX.ScreenColors [Pixel], \
                   GFX.FixedColour)); \
   else \
       Screen [N] = GFX.ScreenColors [Pixel];\
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}

static INLINE void WRITE_4PIXELS16_FLIPPED_ADDF1_2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N])) \
    { \
   if (SubDepth [N] == 1) \
       Screen [N] = (uint16) (COLOR_ADD1_2 (GFX.ScreenColors [Pixel], \
                   GFX.FixedColour)); \
   else \
       Screen [N] = GFX.ScreenColors [Pixel];\
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}

static INLINE void WRITE_4PIXELS16_SUBF1_2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N])) \
    { \
   if (SubDepth [N] == 1) \
       Screen [N] = (uint16) COLOR_SUB1_2 (GFX.ScreenColors [Pixel], \
                  GFX.FixedColour); \
   else \
       Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}

static INLINE void WRITE_4PIXELS16_FLIPPED_SUBF1_2(int32 Offset, uint8* Pixels)
{
   uint32 Pixel;
   uint16* Screen = (uint16*) GFX.S + Offset;
   uint8*  Depth = GFX.ZBuffer + Offset;
   uint8*  SubDepth = GFX.SubZBuffer + Offset;

#define FN(N) \
    if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N])) \
    { \
   if (SubDepth [N] == 1) \
       Screen [N] = (uint16) COLOR_SUB1_2 (GFX.ScreenColors [Pixel], \
                  GFX.FixedColour); \
   else \
       Screen [N] = GFX.ScreenColors [Pixel]; \
   Depth [N] = GFX.Z2; \
    }

   FN(0)
   FN(1)
   FN(2)
   FN(3)

#undef FN
}

void DrawTile16FixedAdd1_2(uint32 Tile, int32 Offset, uint32 StartLine,
                           uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE
   RENDER_TILE(WRITE_4PIXELS16_ADDF1_2, WRITE_4PIXELS16_FLIPPED_ADDF1_2, 4)
}

void DrawClippedTile16FixedAdd1_2(uint32 Tile, int32 Offset,
                                  uint32 StartPixel, uint32 Width,
                                  uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
   uint32 dd, d1, d2;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELS16_ADDF1_2,
                       WRITE_4PIXELS16_FLIPPED_ADDF1_2, 4)
}

void DrawTile16FixedSub1_2(uint32 Tile, int32 Offset, uint32 StartLine,
                           uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE
   RENDER_TILE(WRITE_4PIXELS16_SUBF1_2, WRITE_4PIXELS16_FLIPPED_SUBF1_2, 4)
}

void DrawClippedTile16FixedSub1_2(uint32 Tile, int32 Offset,
                                  uint32 StartPixel, uint32 Width,
                                  uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
   uint32 dd, d1, d2;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILE(WRITE_4PIXELS16_SUBF1_2,
                       WRITE_4PIXELS16_FLIPPED_SUBF1_2, 4)
}

void DrawLargePixel16Add(uint32 Tile, int32 Offset,
                         uint32 StartPixel, uint32 Pixels,
                         uint32 StartLine, uint32 LineCount)
{
   uint16 *sp;
   uint8 *Depth;
   uint16 pixel;
   TILE_PREAMBLE

   sp    = (uint16*) GFX.S + Offset;
   Depth = GFX.ZBuffer + Offset;
#define LARGE_ADD_PIXEL(s, p) \
(Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_ADD (p, *(s + GFX.Delta))    : \
                COLOR_ADD (p, GFX.FixedColour)) \
             : p)

   RENDER_TILE_LARGE(GFX.ScreenColors [pixel], LARGE_ADD_PIXEL)
}

void DrawLargePixel16Add1_2(uint32 Tile, int32 Offset,
                            uint32 StartPixel, uint32 Pixels,
                            uint32 StartLine, uint32 LineCount)
{
   uint16 *sp;
   uint8 *Depth;
   uint16 pixel;
   TILE_PREAMBLE

   sp    = (uint16*) GFX.S + Offset;
   Depth = GFX.ZBuffer + Offset;

#define LARGE_ADD_PIXEL1_2(s, p) \
((uint16) (Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_ADD1_2 (p, *(s + GFX.Delta))    : \
                COLOR_ADD (p, GFX.FixedColour)) \
             : p))

   RENDER_TILE_LARGE(GFX.ScreenColors [pixel], LARGE_ADD_PIXEL1_2)
}

void DrawLargePixel16Sub(uint32 Tile, int32 Offset,
                         uint32 StartPixel, uint32 Pixels,
                         uint32 StartLine, uint32 LineCount)
{
   uint16 *sp;
   uint8 *Depth;
   uint16 pixel;
   TILE_PREAMBLE

   sp    = (uint16*) GFX.S + Offset;
   Depth = GFX.ZBuffer + Offset;

#define LARGE_SUB_PIXEL(s, p) \
(Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_SUB (p, *(s + GFX.Delta))    : \
                COLOR_SUB (p, GFX.FixedColour)) \
             : p)

   RENDER_TILE_LARGE(GFX.ScreenColors [pixel], LARGE_SUB_PIXEL)
}

void DrawLargePixel16Sub1_2(uint32 Tile, int32 Offset,
                            uint32 StartPixel, uint32 Pixels,
                            uint32 StartLine, uint32 LineCount)
{
   uint16 *sp;
   uint8 *Depth;
   uint16 pixel;
   TILE_PREAMBLE

   sp    = (uint16*) GFX.S + Offset;
   Depth = GFX.ZBuffer + Offset;

#define LARGE_SUB_PIXEL1_2(s, p) \
(Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_SUB1_2 (p, *(s + GFX.Delta))    : \
                COLOR_SUB (p, GFX.FixedColour)) \
             : p)

   RENDER_TILE_LARGE(GFX.ScreenColors [pixel], LARGE_SUB_PIXEL1_2)
}

void DrawHiResTile16(uint32 Tile, int32 Offset, uint32 StartLine,
                     uint32 LineCount)
{
   uint8* bp;
   TILE_PREAMBLE
   RENDER_TILEHI(WRITE_4PIXELSHI16, WRITE_4PIXELSHI16_FLIPPED, 4)
}

void DrawHiResClippedTile16(uint32 Tile, int32 Offset,
                            uint32 StartPixel, uint32 Width,
                            uint32 StartLine, uint32 LineCount)
{
   uint8* bp;
   uint32 dd, d1, d2;
   TILE_PREAMBLE
   TILE_CLIP_PREAMBLE
   RENDER_CLIPPED_TILEHI(WRITE_4PIXELSHI16, WRITE_4PIXELSHI16_FLIPPED, 4)
}
