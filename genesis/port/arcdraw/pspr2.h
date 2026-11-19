/* pspr2.h (-DPSPR2, inside draw_patched.c's ParseSprites after the line lists are
   cleared; needs -DPSPR and vp/videoport.c's spr_epoch, i.e. -DLREUSE): the
   sprite chain (link order) is walked once per SAT state (spr_epoch: every SAT
   write, SAT cache reload and register 5/6/12 change) into per-position
   SAT words, HighPreSpr words and 8-line band masks; a call then visits only
   the positions whose y range meets [first_line, max_lines] (in position order,
   as PicoDrive) and writes their HighPreSpr entries (PSPR's rule: entries no new
   list names are left; the DrawScanline list's ones are written). Not used when
   limit is set (that path reads entries of an older list). */
  if (!limit) {
    extern unsigned spr_epoch;
    static unsigned ps_ep = ~0u;
    static int ps_n, ps_ms, ps_tab = -1;
    static u32 ps_c[80], ps_c2[80], ps_pd0[80], ps_pd1[80];
    static u32 ps_band[32][3];
    s32 *pb = pd;
    u32 m0 = 0, m1 = 0, m2 = 0;
    int b;
    if (ps_ep != spr_epoch || ps_ms != max_sprites || ps_tab != table) {
      int k, lk = 0;
      memset(ps_band, 0, sizeof(ps_band));
      for (k = 0; k < max_sprites && lk < max_sprites; k++) {
        u32 *sp_ = (u32 *)(PicoMem.vram+((table+(lk<<2))&0x7ffc));
        int c_ = CPU_LE2(VdpSATCache[2*lk]), sy_ = (c_&0x1ff)-0x80, hv_ = (c_>>24)&0xf;
        int h_ = (hv_&3)+1, w_ = (hv_>>2)+1, c2_ = CPU_LE2(sp_[1]), sx_ = ((c2_>>16)&0x1ff) - 0x78, y0, y1;
        ps_c[k] = c_; ps_c2[k] = c2_;
        ps_pd0[k] = (w_<<28)|(h_<<24)|(lk<<16)|((u16)sy_);
        ps_pd1[k] = (sx_<<16)|((u16)c2_);
        y0 = sy_ < 0 ? 0 : sy_; y1 = sy_ + (h_<<3); if (y1 > 255) y1 = 255;
        for (; y0 <= y1; y0 = (y0 | 7) + 1) ps_band[y0 >> 3][k >> 5] |= 1u << (k & 31);
        lk = (c_>>16)&0x7f;
        if (!lk) { k++; break; }
      }
      ps_n = k; ps_ep = spr_epoch; ps_ms = max_sprites; ps_tab = table;
      { extern unsigned pspr2_stat[2]; pspr2_stat[0]++; }
    }
    { extern unsigned pspr2_stat[2]; pspr2_stat[1]++; }
    for (b = first_line >> 3; b <= (max_lines >> 3); b++) { m0 |= ps_band[b][0]; m1 |= ps_band[b][1]; m2 |= ps_band[b][2]; }
    for (b = 0; b < 3; b++) {                 /* the DrawScanline list's entries */
      u32 m = psneed[b];
      while (m) { int k = b * 32 + __builtin_ctz(m); m &= m - 1;
        if (k < ps_n) { pb[2*k] = ps_pd0[k]; pb[2*k+1] = ps_pd1[k]; } }
    }
    for (b = 0; b < 3; b++) {
      u32 m = b == 0 ? m0 : b == 1 ? m1 : m2;
      while (m) {
        int code, code2, sx, sy, hv, height, width;
        u = b * 32 + __builtin_ctz(m); m &= m - 1;
        code = ps_c[u]; sy = (code&0x1ff)-0x80; hv = (code>>24)&0xf;
        height = (hv&3)+1; width = (hv>>2)+1;
        if (!(sy <= max_lines && sy + (height<<3) >= first_line)) continue;
        code2 = ps_c2[u]; sx = ((code2>>16)&0x1ff) - 0x78;
        {
          int entry, y, w, sx_min, onscr_x, maybe_op = 0;
          int last_line = max_lines;
          sx_min = 8-(width<<3);
          onscr_x = sx_min < sx && sx < max_width;
          if (sh && (code2 & 0x6000) == 0x6000)
            maybe_op = SPRL_MAY_HAVE_OP;
          entry = (u & 0x7f) | ((code2>>8)&0x80);
          y = (sy >= first_line) ? sy : first_line;
          for (; y < sy + (height<<3) && y <= last_line; y++)
          {
            unsigned char *p = &HighLnSpr[y][0];
            int cnt = p[0] & 0x7f;
            if (p[1] & SPRL_MASKED) continue;
            if (p[3] >= max_line_sprites) continue;
            p[3] ++;
            w = width;
            if (p[2] + width > max_line_sprites*2) {
              if (y+1 < 240) HighLnSpr[y+1][1] |= SPRL_TILE_OVFL;
              if (p[2] >= max_line_sprites*2) continue;
              w = max_line_sprites*2 - p[2];
            }
            p[2] += w;
            if (sx == -0x78) {
              if (p[1] & (SPRL_HAVE_X|SPRL_TILE_OVFL))
                p[1] |= SPRL_MASKED;
              if (!(p[1] & SPRL_HAVE_X) && cnt == 0)
                p[1] |= SPRL_HAVE_MASK0;
            } else
              p[1] |= SPRL_HAVE_X;
            if (!onscr_x) continue;
            p[1] |= (entry & 0x80) ? SPRL_HAVE_HI : SPRL_HAVE_LO;
            p[1] |= maybe_op;
            if (cnt > 0 && (code2 & 0x8000) && !(p[4+cnt-1]&0x80))
              p[1] |= SPRL_LO_ABOVE_HI;
            p[4+cnt] = entry;
            p[5+cnt] = w;
            p[0] = (cnt + 1) | HighPreSprBank;
          }
        }
        pb[2*u] = ps_pd0[u]; pb[2*u+1] = ps_pd1[u];
      }
    }
    pb[2*ps_n] = 0;
    HighPreSprBank ^= 0x80;
    return;
  }
