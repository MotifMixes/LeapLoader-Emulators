/* NES on the Leapster 2: QuickNES (libretro's QuickNES_Core, LGPL-2.1+, with
   LeapLoader's changes, #ifdef LL) as the native code of an SD-card image
   (mkimage.py). The image's movie calls the native __nes() once (entry.S);
   it never returns. The console side (BIOS calls, LCD, buttons, clock,
   sound) is the SNES port's (snes/console/console.c), where it is explained.

   * ROMs: the .nes files in b:\NES\ (a folder on the card), picked from a
     list. Battery saves go next to the ROM (NAME.sav), written a few seconds
     after the game changes them and when leaving the game.
   * Picture: QuickNES draws in runs of up to 8 lines into a buffer that
     stays in the data cache (Nes_Ppu_Rendering.cpp ll_draw); lines() turns
     each line into the LCD's 12 bpp at once: NES pixels LEFT..LEFT+239 ->
     160, each 3 blended to 2; NES lines 8-231 -> 160 LCD lines (7 -> 5),
     each LCD line the average of two neighbouring NES lines (none dropped).
   * Speed: frames are timed on the 16 MHz clock; behind the NES's 60.1 Hz,
     frames are run without drawing (at most MAXSKIP in a row).
   * Sound: QuickNES mixes mono at 16 kHz; pcm_isr plays it at 32 kHz.

   ROM list: Up/Down choose, Left/Right page, A starts, PAUSE searches, B clears it.
   Buttons: A = B, B = A, PAUSE = Start, HINT = Select;
   HOME + PAUSE held 2 s: back to the ROM list; HINT + A + B held 2.5 s: back to the list
   as well; HOME in the list: back to the LeapLoader menu. */
#include <string.h>
#include "Nes_Emu.h"
#include "Data_Reader.h"
#include "font.h"

typedef unsigned u32;
extern "C" {
void *malloc(unsigned);
void free(void *);
int snprintf(char *, unsigned, const char *, ...);
unsigned ll_heap_free(void);
char *ll_heap_mark(void);
void ll_heap_reset(char *p);
void ll_shrink(void *q, unsigned n);
extern char __bss_start[], __bss_end[], __init_array_start[], __init_array_end[];
void llmain(void);
}

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
static int b_findfirst(const char *pattern, void *info)
{ return ((int (*)(const char *, void *))svc(SVC_FILE, 0x74))(pattern, info); }
static int b_findnext(int h, void *info) { return ((int (*)(int, void *))svc(SVC_FILE, 0x78))(h, info); }
static void b_findclose(void *info) { ((void (*)(void *))svc(SVC_FILE, 0x7C))(info); }
static void b_awake(void) { ((void (*)(void))svc(SVC_SYS, 0x14))(); }

/* ---- hardware */
static inline u32 io_rd(u32 a)
{
    u32 v;
    __asm__ volatile("ld.di %0, [%1]" : "=r"(v) : "r"(a));
    return v;
}
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
static u32 keys(void) { return ~io_rd(GPIO_IN) & 0xB703E380; }

#define TIMER0 0x0180D084
#define TICK_START (0x1000000 - 0x27100)
#define KERNEL 0x3C7FACB0
unsigned long long now;  /* 16 MHz counts (global: read by tests) */
static void tick(void)
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

/* ---- the picture: 160 x 160 at 12 bpp, two pixels in three bytes (g0b0, r0r1, g1b1).
   Drawn in RAM (frame) and copied to the LCD's VRAM at 0x0300_0000 with uncached stores
   (show), as the SDK's ll_show. Drawn straight into VRAM and written back a line at a time
   with sr [0x4C] (dc_fldl), the real console showed black blocks, leftover text and
   streaks (2026-11-01): that write-back doesn't happen there, so lines reached the LCD
   only when the cache evicted them. Ribbit doesn't model the caches. */
#include "../../common/llcache.h"
#define VRAM 0x03000000u
static u32 frame_words[160 * 240 / 4];
static unsigned char *const frame = (unsigned char *)frame_words;
static void show(void)
{
    const u32 *w = frame_words, *e = w + sizeof frame_words / 4;
    u32 a = VRAM;
    for (; w < e; w += 4, a += 16) {
        u32 w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3];
        __asm__ volatile("st.di %0, [%1]\n\tst.di %2, [%1, 4]\n\tst.di %3, [%1, 8]\n\tst.di %4, [%1, 12]"
                         : : "r"(w0), "r"(a), "r"(w1), "r"(w2), "r"(w3) : "memory");
    }
}
static void b_lcd_base(u32 vram) { ((void (*)(u32))svc(SVC_LCD, 0x28))(vram); }

/* ---- sound (as the SNES port) */
#define SVC_KERNEL 0x10
#define IRQ_FLAGS  0x01800080
#define IRQ_ENABLE 0x01800084
#define PCM0_CTRL  0x01803000
#define PCM0_BASE  0x01803004
#define PCM1_CTRL  0x01803008
#define PCM1_BASE  0x0180300C
#define PCM_ON     0x7F01
#define PCM1_BUF   0x0300EE00
#define PCM0_BUF   0x0300F200
#define RING 4096
#define FILL 800
static inline void io_wr(u32 a, u32 v) { __asm__ volatile("st.di %0, [%1]" : : "r"(v), "r"(a) : "memory"); }
static short ring[RING];
static volatile u32 ring_wr;
static volatile u32 ring_rd;
static u32 ring_frac;
static int last;
u32 ll_under;  /* (read by tests: output samples played without input) */
static void pcm_isr(void *data)
{
    u32 f = io_rd(IRQ_FLAGS), i, vsrc, n = ring_rd, step, *dst;
    u32 avail = ring_wr - n;
    if (f & 1u << 20) io_wr(IRQ_FLAGS, 1u << 20);
    if (!(f & 1u << 21)) return;
    io_wr(IRQ_FLAGS, 1u << 21);
    dst = (u32 *)(PCM1_BUF + ((io_rd(PCM1_BASE) - PCM1_BUF) & 0x100 ? 0 : 0x100));
    /* The BIOS's voices (its menu sounds, as LeapLoader's menu plays them) reach the speaker
       only through us: while channel 1 plays, channel 0 writes their mix to memory (the
       half it has just finished), and it is added to ours. */
    vsrc = PCM0_BUF + ((io_rd(PCM0_BASE) - PCM0_BUF) & 0x100 ? 0 : 0x100);
    step = avail > FILL + 256 ? 0x8100 : avail + 256 < FILL ? 0x7F00 : 0x8000;
    for (i = 0; i < 64; i++) {
        int s[2], k;
        for (k = 0; k < 2; k++) {
            if (ring_wr - n >= 2) {
                int a = ring[n & (RING - 1)], b = ring[(n + 1) & (RING - 1)], d = b - a;
                switch (ring_frac >> 14) {
                case 0: last = a; break;
                case 1: last = a + (d >> 2); break;
                case 2: last = a + (d >> 1); break;
                default: last = b - (d >> 2); break;
                }
                ring_frac += step;
                n += ring_frac >> 16;
                ring_frac &= 0xFFFF;
            } else {
                last -= last >> 5;
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
    unsigned char *q = frame + y * 240 + (x >> 1) * 3;
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
static const char *const dirs[] = {"b:\\NES\\"};
#define NDIRS 1
#define MAX_ROMS 700
static char names[MAX_ROMS][64];
static unsigned char dir_of[MAX_ROMS];
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
    static const char *const ext[] = {"nes", 0};
    int i;
    if (!e) return 0;
    for (i = 0; ext[i]; i++)
        if (!nocase_cmp(e + 1, ext[i])) return 1;
    return 0;
}

static void list_roms(void)
{
    char pat[16];
    int d, h, i, j;
    nroms = 0;
    for (d = 0; d < NDIRS; d++) {
        strcpy(pat, dirs[d]);
        strcat(pat, "*");
        if ((h = b_findfirst(pat, info)) < 0) continue;
        do {
            const char *n = info + 0x38;
            if (!(info[0x13C] & 0x10) && is_rom(n) && strlen(n) < sizeof names[0] && nroms < MAX_ROMS) {
                dir_of[nroms] = d;
                strcpy(names[nroms++], n);
            }
        } while ((h = b_findnext(h, info)) >= 0);
        b_findclose(info);
    }
    for (i = 1; i < nroms; i++)
        for (j = i; j > 0 && nocase_cmp(names[j - 1], names[j]) > 0; j--) {
            char t[64];
            unsigned char td = dir_of[j];
            strcpy(t, names[j]);
            strcpy(names[j], names[j - 1]);
            strcpy(names[j - 1], t);
            dir_of[j] = dir_of[j - 1];
            dir_of[j - 1] = td;
        }
}

static void path_of(char *out, int i, const char *ext)
{
    strcpy(out, dirs[dir_of[i]]);
    strcat(out, names[i]);
    if (ext) {
        char *e = strrchr(out, '.');
        strcpy(e ? e : out + strlen(out), ext);
    }
}

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

/* ---- ROM list */
static void message(const char *a, const char *b, const char *c)
{
    memset(frame, 0, 160 * 240);
    text(4, 40, a, 0xFFF);
    if (b) text(4, 56, b, 0xAAA);
    if (c) text(4, 68, c, 0xAAA);
    show();
}
static u32 wait_press(void)  /* the buttons first pressed */
{
    u32 k;
    while (!(k = keys())) {}
    while (keys()) {}
    return k;
}

static u32 wait_keys(void)
{
    static u32 held;
    static unsigned long long again;
    for (;;) {
        u32 k = keys();
        tick();
        if (k & ~held) {
            again = now + 16000000 / 3;
            held = k;
            break;
        }
        held = k;
        if (k && now >= again) {
            again = now + 16000000 / 12;
            break;
        }
    }
    b_awake();
    return held;
}

/* ---- back to the LeapLoader menu: PlayGame.lnk = the menu, then LC_Boot("ExtStorage") restarts
   into it (as LeapLoader's in-game return, docs/how-it-works.md section 3) */
#define SVC_BOOT 0x28  /* 0x1003 */
static void to_menu(void)
{
    static const char menu[] = "b:\\Leapster\\System\\SDMenu\\SDMenu.bin";
    void *f = b_fopen("b:\\Leapster\\PlayGame.lnk", "w+");
    if (f) {
        b_fwrite(menu, 1, sizeof menu - 1, f);
        b_fclose(f);
    }
    ((void (*)(const char *))svc(SVC_BOOT, 0x34))("ExtStorage");
    for (;;) {}
}

/* ---- the ROM list, drawn like LeapLoader's menu (../../common/llmenu.h) */
#define LLM_LCD frame
#include "../../common/llmenu.h"
#include "../../common/llvol.h"
static const struct llm_theme theme = {"NES", "QuickNES", 0x100, 0x733, 0x333, 0x888, 0xCCC, 0x100, 0x222, 0xF66, 0x100, 0x100, 0xDAA, 0xD33, 0x400};

static int pick(int sel)
{
    for (;;) {
        list_roms();
        if (!nroms) {
            llm_message(&theme, "No NES games yet.", "Put .nes files in the", "NES folder on the card.", "HOME: back to LeapLoader");
            if (wait_press() & K_HOME) to_menu();
            continue;
        }
        return llm_pick(&theme, names[0], sizeof names[0], nroms, sel);
    }
}


/* ---- the C++ runtime (no exceptions, no RTTI; the heap is libc.c's) */
void *operator new(unsigned n) { return malloc(n); }
void *operator new[](unsigned n) { return malloc(n); }
void operator delete(void *p) { free(p); }
void operator delete[](void *p) { free(p); }
void operator delete(void *p, unsigned) { free(p); }
void operator delete[](void *p, unsigned) { free(p); }
extern "C" {
void *__dso_handle;
int __cxa_atexit(void (*f)(void *), void *a, void *d) { return 0; }
void __cxa_pure_virtual(void) { for (;;) {} }
}

/* ---- the picture, from the core as it draws (Nes_Ppu_Rendering.cpp ll_draw) */
extern unsigned char ll_line_shift[240], ll_show[240];
extern void (*ll_lines)(const unsigned char *row, long pitch, int start, int count,
                        const short *palette, int begin, int size);
static unsigned char lcd_line[240];  /* LCD line of each NES line that ends one */
/* Each LCD line is the average of two neighbouring NES lines (7 -> 5: NES lines
   0+1, 1+2, 2+3, 4+5, 5+6 of each 7): bit 0 = this line is the first of a pair,
   bit 1 = the second (it completes LCD line lcd_line[y]). */
static unsigned char line_role[240];
static unsigned line_prev[60], line_cur[60];  /* lines scaled across, LCD format */
/* the LCD's pixel pairs: lut_a[v] | lut_b[w] is the 3 bytes of pixels v, w */
static unsigned lut_a[256], lut_b[256];
/* the same at half brightness: lut_c[v] + lut_c[w] = their average as pixel 0, lut_d as pixel 1 */
static unsigned lut_c[256], lut_d[256];
static int lut_n;  /* palette entries set this frame */
#ifndef LEFT
#define LEFT 8  /* NES pixels cut on the left (games often blank the first 8); 240 shown */
#endif
static void lines(const unsigned char *row, long pitch, int start, int count,
                  const short *palette, int begin, int size)
{
    int y;
    for (; lut_n < size; lut_n++) {
        const Nes_Emu::rgb_t &c = Nes_Emu::nes_colors[palette[lut_n]];
        unsigned r = c.red >> 4, gb = (c.green >> 4) << 4 | c.blue >> 4;
        unsigned rh = r >> 1, gbh = gb >> 1 & 0x77;
        lut_a[(begin + lut_n) & 255] = gb | r << 12;  /* g0b0, r0 */
        lut_b[(begin + lut_n) & 255] = r << 8 | gb << 16;  /* r1, g1b1 */
        lut_c[(begin + lut_n) & 255] = gbh | rh << 12;
        lut_d[(begin + lut_n) & 255] = rh << 8 | gbh << 16;
    }
    for (y = start; y < start + count; y++, row += pitch) {
        if (!line_role[y]) continue;
        const unsigned char *s = row + ll_line_shift[y] + LEFT;
        unsigned *w = line_cur;
        int x;
        /* 240 NES pixels -> 160 LCD pixels: each 3 (a, b, c) -> (a+b)/2, (b+c)/2;
           12 NES pixels = 4 pairs = 3 words */
#define P(j) ((lut_c[s[j]] + lut_c[s[j + 1]]) | (lut_d[s[j + 1]] + lut_d[s[j + 2]]))
        for (x = 0; x < 20; x++, s += 12, w += 3) {
            unsigned p0 = P(0), p1 = P(3), p2 = P(6), p3 = P(9);
            w[0] = p0 | p1 << 24; w[1] = p1 >> 8 | p2 << 16; w[2] = p2 >> 16 | p3 << 8;
        }
#undef P
        if (line_role[y] & 2) {
            /* the average of the two lines, every 4-bit channel at once (rounded down) */
            unsigned *o = (unsigned *)(frame + lcd_line[y] * 240);
            for (x = 0; x < 60; x++) {
                unsigned a = line_prev[x], b = line_cur[x];
                o[x] = (a & b) + (((a ^ b) & 0xEEEEEEEEu) >> 1);
            }
        }
        if (line_role[y] & 1) memcpy(line_prev, line_cur, sizeof line_prev);
    }
}

/* ---- buttons: QuickNES's joypad bits */
static int pad_of(u32 k)
{
    return (k & K_B ? 0x01 : 0) | (k & K_A ? 0x02 : 0) | (k & K_HINT ? 0x04 : 0) | (k & K_PAUSE ? 0x08 : 0)
        | (k & K_UP ? 0x10 : 0) | (k & K_DOWN ? 0x20 : 0) | (k & K_LEFT ? 0x40 : 0) | (k & K_RIGHT ? 0x80 : 0);
}

/* ---- battery saves: the cartridge's 8 KB at 0x6000 */
static char sav[128];
static u32 sav_sum;
static u32 sum(const unsigned char *p, u32 n)
{
    u32 h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}
static void save_sav(Nes_Emu *emu, int force)
{
    u32 s;
    void *f;
    if (!emu->has_battery_ram()) return;
    s = sum(emu->high_mem(), Nes_Emu::high_mem_size);
    if (s == sav_sum && !force) return;
    sav_sum = s;
    if ((f = b_fopen(sav, "wb"))) {
        ll_cache_clean();  /* (the file service reads the buffer past the cache) */
        b_fwrite(emu->high_mem(), 1, Nes_Emu::high_mem_size, f);
        b_fclose(f);
    }
}

#ifndef MAXSKIP
#define MAXSKIP 3
#endif
u32 ll_frames, ll_drawn;  /* (read by tests) */
static char *heap0;  /* the heap's start for each game */
#define BUF_W (Nes_Emu::image_width + 16)
/* the frame buffer QuickNES is given (ll_draw draws elsewhere; kept for the core's own use) */
static unsigned char pixels[BUF_W * (Nes_Emu::image_height + 2)] __attribute__((aligned(16)));
static short samples[2048];

static void play(int i)
{
    char path[128];
    unsigned long long per, due;
    u32 quit = 0, skipped = 0, menu_hold = 0;
    int size, max, f;
    unsigned char *rom;
    const char *err;
    Nes_Emu *emu;

    ll_heap_reset(heap0);
    message("Loading", names[i], 0);
    path_of(path, i, 0);
    max = (int)ll_heap_free() - 0x80000;
    rom = (unsigned char *)malloc(max);
    size = rom ? read_file(path, rom, max) : -1;
    if (size < 16 || size > max) {
        message(size < 0 ? "Can't read this file." : size > max ? "Too big." : "Not a NES game?",
                names[i], "Press a button.");
        wait_press();
        return;
    }
    ll_shrink(rom, size);
    emu = new Nes_Emu;
    err = emu ? 0 : "Out of memory.";
    if (!err) {
        emu->set_pixels(pixels, BUF_W);
        err = emu->set_sample_rate(16000);
    }
    if (!err) {
        Mem_File_Reader reader(rom, size);
        err = emu->load_ines(reader);
    }
    if (err) {
        message("Can't play this game:", err, names[i]);
        wait_press();
        return;
    }
    path_of(sav, i, ".sav");
    if (emu->has_battery_ram())
        read_file(sav, emu->high_mem(), Nes_Emu::high_mem_size - 1);  /* (max + 1 bytes at most) */
    sav_sum = sum(emu->high_mem(), Nes_Emu::high_mem_size);

    memset(frame, 0, 160 * 240);
    show();
    per = 16000000ull * 1000 / 60099;  /* 60.099 Hz */
    tick();
    due = now;
    for (f = 0;; f++) {
        u32 k = keys();
        long n, j;
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
        if (k && !(f & 63)) b_awake();
        if (!(f % 300)) save_sav(emu, 0);
        tick();
        if (now > due + 60 * per) due = now;  /* a second behind: start again from here */
        lut_n = 0;
        if ((now <= due + per / 4 || skipped >= MAXSKIP) && !menu_hold) {  /* (still under the prompt) */
            skipped = 0;
            emu->emulate_frame(pad_of(k));
            { int dy = llv_offset(); if (dy < LLV_HIDE) llv_draw(dy, fill); }  /* the volume panel (llvol.h) */
            show();
            ll_drawn++;
        } else {
            skipped++;
            emu->emulate_skip_frame(pad_of(k));
        }
        if (menu_hold) llm_back_prompt(menu_hold, 150), show();
        /* the sound into the ring, at the volume */
        n = emu->read_samples(samples, sizeof samples / sizeof *samples);
        {
            u32 w = ring_wr;
            int g = llv_gain[llv_level];
            for (j = 0; j < n && w - ring_rd < RING - 1; j++)
                ring[w++ & (RING - 1)] = samples[j] * g >> 8;
            ring_wr = w;
        }
        ll_frames++;
        due += per;
        do tick(); while (now < due);  /* ahead: wait for the frame's time */
    }
    save_sav(emu, 1);
    while (keys()) {}
}

void llmain(void)
{
    int sel = 0, oy;
    memset(__bss_start, 0, __bss_end - __bss_start);
    for (char *p = __init_array_start; p < __init_array_end; p += 4) (*(void (**)(void))p)();
    tick();
    b_lcd_base(VRAM);
    for (oy = 0; oy < 160; oy++) {
        int sy = 8 + oy * 7 / 5;
        line_role[sy] |= 1, line_role[sy + 1] |= 2, lcd_line[sy + 1] = oy;
        ll_show[sy] = ll_show[sy + 1] = 1;
    }
    ll_lines = lines;
    heap0 = ll_heap_mark();
    pcm_start();
    llv_load();  /* the level LeapLoader's menu left (llvol.h) */
    for (;;) {
        sel = pick(sel);
        play(sel);
    }
}
