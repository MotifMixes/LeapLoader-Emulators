/* Genesis / Mega Drive on the Leapster 2: PicoDrive 2.04 with LeapLoader's speed work (genesis/port:
   a 68000 interpreter and a Z80 core in assembly, an assembly line renderer that writes the LCD, the
   YM2612 and PSG loops in assembly, wait-loop skipping) as the native code of an SD-card image. The
   image's movie calls the native __llapp() once (entry.S); it never returns.

   * ROMs: the files in b:\GENESIS\ (.md, .bin, .gen, .smd; up to 4 MB, SMD dumps decoded on loading),
     picked from a list. Battery saves (SRAM / EEPROM) go next to the ROM (NAME.srm), written a few
     seconds after the game changes them and when leaving the game.
   * Picture: PicoDrive draws only the 160 of 224 lines the LCD shows (line y * 7 / 5), each one
     straight into the LCD's frame in the on-chip VRAM (0x0300_0000, 240 bytes a line, 12 bpp) with
     st.di, at 160 of 320 columns. Unchanged LCD rows are not drawn again (LREUSE).
   * Speed: frames are timed on the 16 MHz clock. Behind the 60 Hz (50 Hz PAL) clock, drawing is
     skipped (PicoDrive still runs the frame: game, Z80, sound are always exact): at most 2 frames in a
     row; while more than 2 frames behind, the limit rises a frame at a time up to 8 and goes back to
     2 once less than a frame behind.
   * Sound: YM2612 + PSG at 22050 Hz mono, played through PCM DMA channel 1 (32 kHz, signed 16-bit)
     from our own interrupt handler (pcm_isr).

   ROM list: Up/Down choose, Left/Right page, A starts, PAUSE searches, B clears it.
   Buttons: B = Genesis B, A = Genesis C, HINT = Genesis A, PAUSE = Start; HOME + A = Genesis A too;
   HOME + PAUSE held 2 s, or HINT + A + B held 2.5 s: back to the ROM list (saved);
   HOME in the ROM list: back to the LeapLoader menu. VOL +/-: the volume panel. */
#include <string.h>
#include "pico_int.h"
#include "font.h"

/* (u32: PicoDrive's, pico_types.h) */
void *malloc(unsigned);
void *calloc(unsigned, unsigned);
char *ll_heap_mark(void);
void ll_heap_reset(char *);
extern char __bss_start[], __bss_end[];

/* ---- BIOS services: [[gp + 4] + service] is a function table (bioscall.py) */
#define SVC_FILE 0x280  /* 0x1035: C library files */
#define SVC_SYS  0xAC   /* SysMgrMPI */
#define SVC_LCD  0xB8   /* LcdMPI */
static void *svc(u32 service, u32 slot)
{
    u32 gp;
    __asm__ volatile("mov %0, gp" : "=r"(gp));
    return *(void **)(*(u32 *)(*(u32 *)(gp + 4) + service) + slot);
}
static void *b_fopen(const char *path, const char *mode)
{ return ((void *(*)(const char *, const char *))svc(SVC_FILE, 0x28))(path, mode); }
static int b_fclose(void *f) { return ((int (*)(void *))svc(SVC_FILE, 0x2C))(f); }
static int b_fread(void *buf, int size, int n, void *f)
{ return ((int (*)(void *, int, int, void *))svc(SVC_FILE, 0x30))(buf, size, n, f); }
static int b_fwrite(const void *buf, int size, int n, void *f)
{ return ((int (*)(const void *, int, int, void *))svc(SVC_FILE, 0x34))(buf, size, n, f); }
/* the directory walk of BIOS 0.6 0x4002_BDC0 (behind SD_Dir): findfirst(pattern,
   info) -> handle (< 0: none), findnext(handle, info) (< 0: no more),
   findclose(info); info: +0x38 name, +0x13C attributes (bit 4: folder) */
static int b_findfirst(const char *pattern, void *info)
{ return ((int (*)(const char *, void *))svc(SVC_FILE, 0x74))(pattern, info); }
static int b_findnext(int h, void *info) { return ((int (*)(int, void *))svc(SVC_FILE, 0x78))(h, info); }
static void b_findclose(void *info) { ((void (*)(void *))svc(SVC_FILE, 0x7C))(info); }
/* the idle timer's reset (otherwise the console powers off after 5 minutes) */
static void b_awake(void) { ((void (*)(void))svc(SVC_SYS, 0x14))(); }

/* ---- hardware */
static inline u32 io_rd(u32 a)
{
    u32 v;
    __asm__ volatile("ld.di %0, [%1]" : "=r"(v) : "r"(a));
    return v;
}
static inline void io_wr(u32 a, u32 v) { __asm__ volatile("st.di %0, [%1]" : : "r"(v), "r"(a) : "memory"); }
#define GPIO_IN 0x0180004C
#define K_RIGHT 0x00000080
#define K_DOWN  0x00000100
#define K_LEFT  0x00000200
#define K_B     0x00002000
#define K_A     0x00004000
#define K_PAUSE 0x04000000
#define K_HINT  0x10000000
#define K_HOME  0x20000000
#define K_UP    0x80000000
#define K_VOLDN 0x00008000
#define K_VOLUP 0x00010000
static void to_menu(void);
static u32 keys(void) { return ~io_rd(GPIO_IN) & 0xB703E380; }

/* The clock: the RTOS's 10 ms ticks and timer 0 within the tick (see the SNES console):
   ticks x 0x2_7100 + the position in the period, 16 MHz counts. */
#define TIMER0 0x0180D084
#define TICK_START (0x1000000 - 0x27100)
#define KERNEL 0x3C7FACB0
unsigned long long now;  /* 16 MHz counts (global: read by tests) */
void LLTick(void)
{
    volatile u32 *ticks = (volatile u32 *)(*(volatile u32 *)KERNEL + 0x34);
    u32 lo, hi, c;
    unsigned long long t;
    do {
        lo = ticks[0], hi = ticks[1];
        c = io_rd(TIMER0);
    } while (lo != ticks[0]);
    t = ((unsigned long long)hi << 32 | lo) * 0x27100 + (c >= TICK_START ? c - TICK_START : c + 0x27100);
    if (t > now) now = t;
}

/* ---- the picture: 160 x 160 at 12 bpp, two pixels in three bytes (g0b0, r0r1, g1b1). The game's lines
   go straight into the LCD's VRAM (st.di); menus, messages and overlays are drawn in LLFrame (RAM) and
   copied with st.di (show): lines written into VRAM through the data cache reached the LCD late or not
   at all on a real Leapster 2 (NES/GB ports, 2026-11-01). */
#define VRAM 0x03000000u
static u32 frame_words[160 * 240 / 4];
unsigned char *LLFrame = (unsigned char *)frame_words;
static void show(void)
{
    const u32 *w = frame_words, *e = frame_words + 160 * 240 / 4;
    u32 a = VRAM;
    for (; w < e; w += 4, a += 16)
        __asm__ volatile("st.di %0, [%1]\n\tst.di %2, [%1, 4]\n\tst.di %3, [%1, 8]\n\tst.di %4, [%1, 12]"
                         : : "r"(w[0]), "r"(a), "r"(w[1]), "r"(w[2]), "r"(w[3]) : "memory");
}
/* the LCD's current picture back into LLFrame (ld.di), so an overlay can be drawn over the game's frame */
static void pull(void)
{
    u32 *w = frame_words, *e = frame_words + 160 * 240 / 4;
    u32 a = VRAM;
    for (; w < e; w++, a += 4) __asm__ volatile("ld.di %0, [%1]" : "=r"(*w) : "r"(a));
}
#include "../../common/llcache.h"  /* ll_cache_clean around the file service (NES/GB ports) */
/* LcdMPI SetFrameBuffer (+0x28, BIOS 0.6 0x400B_1480): the LCD shows VRAM from this address */
static void b_lcd_base(u32 vram) { ((void (*)(u32))svc(SVC_LCD, 0x28))(vram); }
/* the renderer's unchanged-row reuse (arcdraw/drawx.c LREUSE): rows written by anything else must be
   drawn again by the game */
void arcdraw_lr_invalidate(int row0, int row1);

/* ---- sound: PCM DMA channel 1 plays signed 16-bit samples at 32 kHz from a ring of two 128-sample
   halves and raises interrupt 0x13 after each half; set up and handled as in the SNES console (the
   BIOS's voices on channel 0 are mixed in). The handler fills the half just played from a 22050 Hz
   ring (write_sound) with linear steps; the step follows the ring's level (+-0.6%) so the frame clock
   and the sound clock don't drift into an underrun. No multiplies in the handler (MUL64 registers). */
#define SVC_KERNEL 0x10
#define IRQ_FLAGS  0x01800080
#define IRQ_ENABLE 0x01800084
#define PCM0_CTRL  0x01803000
#define PCM0_BASE  0x01803004
#define PCM1_CTRL  0x01803008
#define PCM1_BASE  0x0180300C
#define PCM_ON     0x7F01  /* 128 samples per half, on (the BIOS's value) */
#define PCM1_BUF   0x0300EE00
#define PCM0_BUF   0x0300F200
#define RING 4096  /* 22050 Hz samples (186 ms) */
#define FILL 1100  /* the ring's level aimed at (50 ms) */
#define STEP 0xB066  /* 22050 / 32000 in 1/65536 */
static short ring[RING];
static volatile u32 ring_wr;  /* samples written (write_sound) */
static volatile u32 ring_rd;  /* samples read (pcm_isr) */
static u32 ring_frac;  /* and the position between two, 0x10000 = 1 */
static int last;  /* the last sample played */
u32 ll_under;  /* (read by tests: output samples played without input) */
static void pcm_isr(void *data)
{
    u32 f = io_rd(IRQ_FLAGS), i, vsrc, n = ring_rd, avail = ring_wr - n, step, *dst;
    if (f & 1u << 20) io_wr(IRQ_FLAGS, 1u << 20);
    if (!(f & 1u << 21)) return;
    io_wr(IRQ_FLAGS, 1u << 21);
    dst = (u32 *)(PCM1_BUF + ((io_rd(PCM1_BASE) - PCM1_BUF) & 0x100 ? 0 : 0x100));
    vsrc = PCM0_BUF + ((io_rd(PCM0_BASE) - PCM0_BUF) & 0x100 ? 0 : 0x100);
    step = avail > FILL + 256 ? STEP + 0x100 : avail + 256 < FILL ? STEP - 0x100 : STEP;
    for (i = 0; i < 64; i++) {
        int s[2], k;
        for (k = 0; k < 2; k++) {
            if (ring_wr - n >= 2) {
                int a = ring[n & (RING - 1)], b = ring[(n + 1) & (RING - 1)], d = b - a;
                switch (ring_frac >> 14) {  /* a + (b - a) x frac, in quarters */
                case 0: last = a; break;
                case 1: last = a + (d >> 2); break;
                case 2: last = a + (d >> 1); break;
                default: last = b - (d >> 2); break;
                }
                ring_frac += step;
                n += ring_frac >> 16;
                ring_frac &= 0xFFFF;
            } else {
                last -= last >> 5;  /* nothing to play: fade out */
                ll_under++;
            }
            s[k] = last;
        }
        {
            u32 v = io_rd(vsrc + 4 * i);
            int a = s[0] + (short)v, b = s[1] + (short)(v >> 16);
            s[0] = a > 32767 ? 32767 : a < -32768 ? -32768 : a;
            s[1] = b > 32767 ? 32767 : b < -32768 ? -32768 : b;
        }
        __asm__ volatile("st.di %0, [%1]" : : "r"((s[0] & 0xFFFF) | (u32)s[1] << 16), "r"(dst + i) : "memory");
    }
    ring_rd = n;
}
static void pcm_start(void)
{
    u32 i;
    ((void (*)(void))svc(SVC_KERNEL, 0x19C))();
    ((void *(*)(u32, void (*)(void *), void *))svc(SVC_KERNEL, 0x180))(0x13, pcm_isr, 0);
    for (i = 0; i < 0x200; i += 4) io_wr(PCM1_BUF + i, 0);
    io_wr(PCM1_CTRL, 0);
    io_wr(PCM1_BASE, PCM1_BUF);
    io_wr(PCM0_CTRL, 0);
    io_wr(PCM0_BASE, PCM0_BUF);
    io_wr(IRQ_FLAGS, 1u << 20);
    io_wr(IRQ_ENABLE, io_rd(IRQ_ENABLE) | 1u << 20);
    io_wr(PCM0_CTRL, PCM_ON);
    io_wr(0x01800010, 0xFF);
    while (io_rd(0x01800010) & 0x80) {}
    io_wr(IRQ_FLAGS, 1u << 21);
    io_wr(IRQ_ENABLE, io_rd(IRQ_ENABLE) | 1u << 21);
    io_wr(PCM1_CTRL, PCM_ON);
    io_wr(0x018020F4, io_rd(0x018020F4) | 0x400);
    ((void (*)(void))svc(SVC_KERNEL, 0x198))();
}

static void pixel(int x, int y, u32 rgb)  /* rgb: 0xRGB */
{
    unsigned char *q = LLFrame + y * 240 + (x >> 1) * 3;
    if (x & 1) {
        q[1] = (q[1] & 0xF0) | rgb >> 8;
        q[2] = rgb;
    } else {
        q[0] = rgb;
        q[1] = (q[1] & 0x0F) | (rgb >> 4 & 0xF0);
    }
}
static void fill(int x0, int y0, int w, int h, u32 rgb)
{
    int x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++) pixel(x, y, rgb);
}
static int text(int x, int y, const char *s, u32 rgb)
{
    for (; *s && x <= 160 - 5; s++, x += 6) {
        int c = (unsigned char)*s, r, i;
        if (c < 32 || c > 126) c = '?';
        for (r = 0; r < 9; r++)
            for (i = 0; i < 5; i++)
                if (font[c - 32][r] >> (4 - i) & 1) pixel(x + i, y + r, rgb);
    }
    return x;
}

/* ---- files */
#define DIR "b:\\GENESIS\\"
#define MAX_ROMS 200
static char names[MAX_ROMS][64];
static int nroms;
static char info[0x200] __attribute__((aligned(4)));

static int nocase_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y || !x) return x - y;
    }
}

static int is_rom(const char *n)
{
    const char *e = strrchr(n, '.');
    static const char *ext[] = {"md", "bin", "gen", "smd", 0};
    int i;
    if (!e) return 0;
    for (i = 0; ext[i]; i++)
        if (!nocase_cmp(e + 1, ext[i])) return 1;
    return 0;
}

static void list_roms(void)
{
    int h = b_findfirst(DIR "*", info), i, j;
    nroms = 0;
    if (h < 0) return;
    do {
        const char *n = info + 0x38;
        if (!(info[0x13C] & 0x10) && is_rom(n) && strlen(n) < sizeof names[0] && nroms < MAX_ROMS)
            strcpy(names[nroms++], n);
    } while ((h = b_findnext(h, info)) >= 0);  /* (each result is the next call's handle) */
    b_findclose(info);
    for (i = 1; i < nroms; i++)
        for (j = i; j > 0 && nocase_cmp(names[j - 1], names[j]) > 0; j--) {
            char t[64];
            strcpy(t, names[j]);
            strcpy(names[j], names[j - 1]);
            strcpy(names[j - 1], t);
        }
}

static void path_of(char *out, const char *name, const char *ext)
{
    strcpy(out, DIR);
    strcat(out, name);
    if (ext) {
        char *e = strrchr(out, '.');
        strcpy(e ? e : out + strlen(out), ext);
    }
}

/* the whole file into buf (at most max bytes); -1 if it can't be opened, max + 1 if longer */
static int read_file(const char *path, unsigned char *buf, int max)
{
    void *f = b_fopen(path, "rb");
    int n = 0, k;
    if (!f) return -1;
    ll_cache_clean();
    while (n <= max && (k = b_fread(buf + n, 1, max + 1 - n < 0x8000 ? max + 1 - n : 0x8000, f)) > 0)
        n += k;
    b_fclose(f);
    return n;
}

static void message(const char *a, const char *b, const char *c)
{
    memset(LLFrame, 0, 160 * 240);
    text(4, 40, a, 0xFFF);
    if (b) text(4, 56, b, 0xAAA);
    if (c) text(4, 68, c, 0xAAA);
    show();
}

/* ---- the ROM list, drawn like LeapLoader's menu (../../common/llmenu.h) */
#define tick LLTick
#define LLM_LCD LLFrame
#include "../../common/llmenu.h"
#include "../../common/llvol.h"  /* the volume panel, shared with LeapLoader's menu */
static const struct llm_theme theme = {"Genesis", "PicoDrive", 0x012, 0x248, 0x139, 0x25B, 0x9CF, 0x012, 0x024, 0xCDF, 0x012, 0x012, 0xCCD, 0x25B, 0x024};

static int pick(int sel)
{
    for (;;) {
        list_roms();
        if (!nroms) {
            llm_message(&theme, "No Genesis games yet.", "Put .md / .bin / .gen files", "in the GENESIS folder.", "HOME: back to LeapLoader");
            {
                u32 k;
                while (!(k = keys())) {}
                while (keys()) {}
                if (k & K_HOME) to_menu();
            }
            continue;
        }
        return llm_pick(&theme, names[0], sizeof names[0], nroms, sel);
    }
}

/* ---- PicoDrive's platform side */
void lprintf(const char *fmt, ...) {}
void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed) { return calloc(1, size); }
void *plat_mremap(void *ptr, size_t oldsize, size_t newsize) { return realloc(ptr, newsize); }
void plat_munmap(void *ptr, size_t size) { free(ptr); }
void *plat_mem_get_for_drc(size_t size) { return NULL; }
int plat_mem_set_exec(void *ptr, size_t size) { return 0; }
void cache_flush_d_inval_i(void *s, void *e) {}
static int vstart;
void emu_video_mode_change(int start_line, int line_count, int start_col, int col_count) { vstart = start_line; }

/* the LCD rows: PicoDrive draws only the 160 lines shown (224 -> 160, line y * 7 / 5) */
static short lcdrow[256];  /* source line -> LCD line, or -1 */
static unsigned char skiprun[256];
extern unsigned char *arcdraw_dst;  /* arcdraw/drawx.c draws the line straight to the LCD */
extern int arcdraw_row;
static int scan_begin(unsigned int num)
{
    unsigned l = num - vstart;
    if (l < 256 && !skiprun[l]) {
        arcdraw_dst = (unsigned char *)VRAM + lcdrow[l] * 240;
        arcdraw_row = lcdrow[l];
    }
    return l < 256 ? skiprun[l] : 1;
}
/* a line the assembly renderer left to PicoDrive's C one: its 8-bit line, through the palette, to the LCD */
static int scan_end(unsigned int num)
{
    unsigned l = num - vstart;
    int y = l < 256 ? lcdrow[l] : -1, x, k;
    const unsigned char *s;
    const unsigned short *pal;
    u32 a;
    if (y < 0) return 0;
    PicoDrawUpdateHighPal();
    s = Pico.est.HighCol + 8;
    pal = Pico.est.HighPal;
    a = VRAM + y * 240;
    for (x = 0; x < 160; x += 8, s += 16, a += 12) {
        u32 p[8];
        for (k = 0; k < 8; k++) {
            u32 c = pal[s[2 * k]];  /* RGB565 -> 4:4:4 */
            p[k] = (c >> 12) << 8 | ((c >> 7) & 15) << 4 | ((c >> 1) & 15);
        }
        io_wr(a, p[0] | p[1] << 12 | p[2] << 24);
        io_wr(a + 4, p[2] >> 8 | p[3] << 4 | p[4] << 16 | p[5] << 28);
        io_wr(a + 8, p[5] >> 4 | p[6] << 8 | p[7] << 20);
    }
    return 0;
}

/* the frame's sound (PicoDrive calls this once a frame; len in bytes, mono) into the ring, at the volume */
static short snd[2 * 1024];
static void write_sound(int len)
{
    u32 w = ring_wr, i, n = len >> 1;
    int g = llv_gain[llv_level];
    for (i = 0; i < n && w - ring_rd < RING - 1; i++)
        ring[w++ & (RING - 1)] = snd[i] * g >> 8;
    ring_wr = w;
}

static u32 map(u32 k)
{
    u32 m = 0;  /* PicoDrive's pad: MXYZ SACB RLDU */
    if (k & K_UP) m |= 0x01;
    if (k & K_DOWN) m |= 0x02;
    if (k & K_LEFT) m |= 0x04;
    if (k & K_RIGHT) m |= 0x08;
    if (k & K_B) m |= 0x10;               /* B */
    if (k & K_HINT) m |= 0x40;            /* A */
    if (k & K_PAUSE) m |= 0x80;           /* Start */
    if (k & K_HOME) {
        if (k & K_A) m |= 0x40;           /* HOME + A: A */
    } else if (k & K_A) m |= 0x20;        /* C */
    return m;
}

/* ---- battery saves */
static char srm[128];
static u32 srm_sum;
static u32 sum(const unsigned char *p, u32 n)
{
    u32 h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}
#define SVC_BOOT 0x28  /* 0x1003 */
/* Back to the LeapLoader menu: PlayGame.lnk names it, then the BIOS restarts from the card (as the NES port). */
static void to_menu(void)
{
    static const char menu[] = "b:\\Leapster\\System\\SDMenu\\SDMenu.bin";
    void *f = b_fopen("b:\\Leapster\\PlayGame.lnk", "w+");
    if (f) {
        ll_cache_clean();
        b_fwrite(menu, 1, sizeof menu - 1, f);
        b_fclose(f);
    }
    ((void (*)(const char *))svc(SVC_BOOT, 0x34))("ExtStorage");
    for (;;) {}
}

static void save_srm(int force)
{
    u32 n = Pico.sv.size, s;
    unsigned char *p = Pico.sv.data;
    void *f;
    if (!n || !p) return;
    s = sum(p, n);
    if (s == srm_sum && !force) return;
    srm_sum = s;
    if ((f = b_fopen(srm, "wb"))) {
        ll_cache_clean();  /* (the file service reads the buffer past the cache) */
        b_fwrite(p, 1, n, f);
        b_fclose(f);
    }
}

/* ---- the ROM: one buffer for the biggest (4 MB, + 0x200 for an SMD header), allocated once. Loaded
   as PicoCartLoad does (size rounded up to 4 bytes, SMD blocks deinterleaved, else byteswapped), the
   rest zero as its calloc'd buffer; PicoCartInsert adds its 4 bytes after the ROM. */
#define ROM_MAX 0x400000
static unsigned char *rom_buf;
static int load_rom(const char *path, u32 *psize)
{
    int n = read_file(path, rom_buf, ROM_MAX + 0x200), size, i;
    if (n < 0 || n > ROM_MAX + 0x200) return n;
    size = (n + 3) & ~3;
    memset(rom_buf + n, 0, ROM_MAX + 0x200 + 16 - n);
    if (size >= 0x4200 && (size & 0x3fff) == 0x200 &&
        ((rom_buf[0x2280] == 'S' && rom_buf[0x280] == 'E') || (rom_buf[0x280] == 'S' && rom_buf[0x2281] == 'E'))) {
        static unsigned char t[0x4000];
        for (i = 0; i + 0x4200 <= size; i += 0x4000) {  /* (PicoDrive's DecodeSmd/InterleveBlock) */
            int j;
            for (j = 0; j < 0x2000; j++)  /* (MEM_BE2 on little-endian: odd bytes at even offsets) */
                t[j << 1] = rom_buf[0x200 + i + j], t[(j << 1) + 1] = rom_buf[0x200 + i + 0x2000 + j];
            memcpy(rom_buf + i, t, 0x4000);
        }
        size -= 0x200;
        memset(rom_buf + size, 0, 0x200);
    } else if (size > ROM_MAX) {
        return ROM_MAX + 0x201;
    } else
        Byteswap(rom_buf, rom_buf, size);
    *psize = size;
    return n;
}

/* ---- frame pacing and skipping (port/main.c fs_decide, M16): draw when no later than a quarter frame
   behind, or after `cap` skipped frames in a row; cap 2, rising a step a frame (to 8) while more than
   2 frames behind, back to 2 at a frame behind. */
#define CAP 2
#define CAP_MAX 8
void PicoVideoSkipped(void);
void arcdraw_flush(void);

static void play(const char *name)
{
    char path[128];
    unsigned long long per, due;
    u32 size, quit = 0, menu_hold = 0, run = 0, cap = CAP;
    int n, frame;
    char *mark = ll_heap_mark();

    message("Loading", name, 0);
    path_of(path, name, 0);
    n = load_rom(path, &size);
    if (n < 0 || n > ROM_MAX + 0x200) {
        message(n < 0 ? "Can't read this file." : "Too big (4 MB at most).", name, "Press a button.");
        while (!keys()) {}
        while (keys()) {}
        return;
    }
    if (PicoCartInsert(rom_buf, size, NULL)) {
        message("Not a Genesis game?", name, "Press a button.");
        while (!keys()) {}
        while (keys()) {}
        ll_heap_reset(mark);
        return;
    }
    PicoLoopPrepare();
    PicoIn.writeSound = write_sound;
    PicoIn.sndOut = snd;
    PsndRerate(0);
    PicoDrawSetOutFormat(PDF_NONE, 0);
    PicoDrawSetCallbacks(scan_begin, scan_end);
    path_of(srm, name, ".srm");
    if (Pico.sv.size && Pico.sv.data) read_file(srm, Pico.sv.data, Pico.sv.size);
    srm_sum = Pico.sv.size && Pico.sv.data ? sum(Pico.sv.data, Pico.sv.size) : 0;
    memset(LLFrame, 0, 160 * 240);
    show();
    arcdraw_lr_invalidate(0, 159);

    per = 16000000ull / (Pico.m.pal ? 50 : 60);
    LLTick();
    due = now;
    for (frame = 0;; frame++) {
        u32 k = keys(), draw;
        PicoIn.pad[0] = map(k);
        llv_keys(k);
        if ((k & (K_HOME | K_PAUSE)) == (K_HOME | K_PAUSE)) {
            if (++quit >= 120) break;
        } else
            quit = 0;
        if ((k & (K_HINT | K_A | K_B)) == (K_HINT | K_A | K_B)) {
            if (++menu_hold >= 150)  /* 2.5 s: HINT + A + B, back to the game list (saved below) */
                break;
        } else
            menu_hold = 0;
        if (k && !(frame & 63)) b_awake();
        if (!(frame % 300)) save_srm(0);
        LLTick();
        if (now > due + 60 * per) due = now;  /* a second behind (a slow scene, a pause): start again from here */
        {
            unsigned long long late = now > due ? now - due : 0;
            if (late > 2 * per) { if (cap < CAP_MAX) cap++; }
            else if (late <= per) cap = CAP;
            draw = (late <= per / 4 || run >= cap) && !menu_hold;  /* (still under the prompt) */
        }
        run = draw ? 0 : run + 1;
        PicoIn.skipFrame = !draw;
        PicoFrame();
        arcdraw_flush();  /* the last line's queued LCD words */
        if (!draw) PicoVideoSkipped();
        {
            /* overlays: over a newly drawn game frame, or the prompt's bar as it grows */
            int dy = llv_offset();  /* the volume panel (llvol.h), LLV_HIDE when it is not up */
            if ((dy < LLV_HIDE && draw) || menu_hold) {
                pull();
                if (menu_hold) llm_back_prompt(1, 150), llm_back_prompt(menu_hold, 150);
                if (dy < LLV_HIDE) llv_draw(dy, fill);
                show();
                arcdraw_lr_invalidate(0, 159);  /* the game draws every row again next time */
            }
        }
        due += per;
        do LLTick(); while (now < due);  /* ahead: wait for the frame's time */
    }
    save_srm(1);
    PicoCartUnload();
    Pico.sv.data = NULL;
    ll_heap_reset(mark);  /* everything the game allocated */
    while (keys()) {}
}

void llmain(void)
{
    int sel = 0, l, y;
    memset(__bss_start, 0, __bss_end - __bss_start);
    LLTick();
    b_lcd_base(VRAM);
    message("Genesis", "starting...", 0);
    rom_buf = calloc(1, ROM_MAX + 0x200 + 16);
    PicoIn.opt = POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80 | POPT_ACC_SPRITES | POPT_DIS_32C_BORDER;
    PicoIn.sndRate = 22050;
    PicoIn.autoRgnOrder = 0x184;  /* US, EU, JP */
    PicoInit();
    if (!rom_buf) {
        message("Out of memory.", 0, 0);
        for (;;) {}
    }
    for (l = 0; l < 256; l++) lcdrow[l] = -1;
    for (y = 0; y < 160; y++) lcdrow[y * 7 / 5] = y;
    for (l = 255; l >= 0; l--) skiprun[l] = lcdrow[l] >= 0 ? 0 : (l < 255 ? skiprun[l + 1] : 0) + 1;
    pcm_start();
    llv_load();  /* the level LeapLoader's menu left (llvol.h) */
    for (;;) {
        sel = pick(sel);
        play(names[sel]);
    }
}
