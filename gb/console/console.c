/* Game Boy / Game Boy Color on the Leapster 2: gnuboy (with LeapLoader's
   speed work, #ifdef LL) as the native code of an SD-card image (mkimage.py).
   The image's movie calls the native __gb() once (entry.S); it never returns.
   The console side (BIOS calls, LCD, buttons, clock, sound) is the SNES
   port's (snes/console/console.c), where it is explained.

   * ROMs: the files in b:\GB\ and b:\GBC\ (folders on the card), picked from
     a list. Battery saves go next to the ROM (NAME.sav), written a few
     seconds after the game changes them and when leaving the game.
   * Picture: 160 x 144 at 12 bpp (lcd.c LL), drawn straight into the LCD's
     frame (lines 8-151 of 160).
   * Speed: frames are timed on the 16 MHz clock; behind the GB's 59.73 Hz,
     drawing is skipped (at most MAXSKIP frames in a row).
   * Sound: gnuboy mixes 8-bit mono at 16 kHz; pcm_isr plays it at 32 kHz.

   ROM list: Up/Down choose, Left/Right page, A starts, PAUSE searches, B clears it.
   Buttons: A = B, B = A, PAUSE = Start, HINT = Select;
   HOME + PAUSE held 2 s, or HINT + A + B held 2.5 s: back to the ROM list (saved);
   HOME in the ROM list: back to the LeapLoader menu. */
#include <string.h>
#include "defs.h"
#include "regs.h"
#include "mem.h"
#include "hw.h"
#include "cpu.h"
#include "lcd.h"
#include "fb.h"
#include "pcm.h"
#include "rtc.h"
#include "sound.h"
#include "font.h"

typedef unsigned u32;
void *malloc(unsigned);
int snprintf(char *, unsigned, const char *, ...);
unsigned ll_heap_free(void);
char *ll_heap_mark(void);
void ll_heap_reset(char *p);
void ll_shrink(void *q, unsigned n);
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
static int b_findfirst(const char *pattern, void *info)
{ return ((int (*)(const char *, void *))svc(SVC_FILE, 0x74))(pattern, info); }
static int b_findnext(int h, void *info) { return ((int (*)(int, void *))svc(SVC_FILE, 0x78))(h, info); }
static void b_findclose(void *info) { ((void (*)(void *))svc(SVC_FILE, 0x7C))(info); }
static void flush_lines(const void *p, int n)
{
    u32 a = (u32)p & ~15u, e = (u32)p + n, s;
    for (; a < e; a += 16) __asm__ volatile("sr %0, [0x4c]" : : "r"(a) : "memory");
    do __asm__ volatile("lr %0, [0x48]" : "=r"(s)); while (s & 0x100);
}
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
static void to_menu(void);
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

/* ---- the picture: 160 x 160 at 12 bpp, two pixels in three bytes (g0b0, r0r1, g1b1) */
static unsigned char *const frame = (unsigned char *)0x03000000;
static void show(void) { flush_lines(frame, 160 * 240); }
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
static const char *const dirs[] = {"b:\\GB\\", "b:\\GBC\\"};
#define MAX_ROMS 2000
static char names[MAX_ROMS][128];  /* (long GBC names: up to 127 characters) */
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
    static const char *ext[] = {"gb", "gbc", "cgb", "sgb", 0};
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
    for (d = 0; d < 2; d++) {
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
    /* shell sort (thousands of games: insertion sort's n^2 copies took seconds) */
    {
        int gap;
        for (gap = 1; gap < nroms / 3; gap = gap * 3 + 1) {}
        for (; gap > 0; gap /= 3)
            for (i = gap; i < nroms; i++)
                for (j = i; j >= gap && nocase_cmp(names[j - gap], names[j]) > 0; j -= gap) {
                    char t[sizeof names[0]];
                    unsigned char td = dir_of[j];
                    strcpy(t, names[j]);
                    strcpy(names[j], names[j - gap]);
                    strcpy(names[j - gap], t);
                    dir_of[j] = dir_of[j - gap];
                    dir_of[j - gap] = td;
                }
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

/* ---- the ROM list, drawn like LeapLoader's menu (../../common/llmenu.h) */
#define LLM_LCD frame
#include "../../common/llmenu.h"
static const struct llm_theme theme = {"Game Boy", "gnuboy", 0x020, 0x6A4, 0x713, 0xB25, 0xE7A, 0x020, 0x401, 0xCE8, 0x020, 0x020, 0xAD8, 0x8C3, 0x130};

static int pick(int sel)
{
    for (;;) {
        list_roms();
        if (!nroms) {
            llm_message(&theme, "No Game Boy games yet.", "Put .gb and .gbc files in", "the GB or GBC folder.", "HOME: back to LeapLoader");
            if (wait_press() & K_HOME) to_menu();
            continue;
        }
        return llm_pick(&theme, names[0], sizeof names[0], nroms, sel);
    }
}

/* ---- volume (as the SNES port) */
static const unsigned short vol_gain[] = {0, 11, 16, 23, 32, 45, 64, 91, 128, 181, 256};  /* /256 */
#define VOL_MAX 10
static int vol = VOL_MAX;
static int vol_show;
static void vol_keys(u32 k)
{
    static u32 held, t;
    u32 v = k & (K_VOLUP | K_VOLDN);
    if (v != held) held = v, t = 0;
    else if (v) t++;
    if (!v || (t && (t < 20 || t % 6))) return;
    if (v == K_VOLUP && vol < VOL_MAX) vol++;
    if (v == K_VOLDN && vol > 0) vol--;
    vol_show = 90;
}
static void vol_bar(void)
{
    int i;
    fill(4, 4, 4 + VOL_MAX * 6, 10, 0x000);
    for (i = 0; i < VOL_MAX; i++)
        fill(7 + i * 6, 7 + (VOL_MAX - 1 - i) / 3, 4, 4 - (VOL_MAX - 1 - i) / 3, i < vol ? 0x0F0 : 0x444);
}

/* ---- gnuboy's frontend */
struct fb fb;
struct pcm pcm;
static byte sndbuf[1024];
extern byte *ll_rom; extern int ll_romlen;
int rom_load_simple(char *fn);
void emu_reset(void), emu_step(void), lcd_begin(void), sound_mix(void);

/* the samples gnuboy mixed (8-bit unsigned) into the ring, at the volume */
int pcm_submit(void)
{
    u32 w = ring_wr, i;
    int g = vol_gain[vol];
    for (i = 0; i < (u32)pcm.pos && w - ring_rd < RING - 1; i++)
        ring[w++ & (RING - 1)] = (pcm.buf[i] - 128) * g;
    ring_wr = w;
    pcm.pos = 0;
    return 1;
}
void pcm_init(void) {}
void pcm_close(void) {}
void pcm_pause(int p) {}
void vid_begin(void) {}
void vid_end(void) {}
void vid_setpal(int i, int r, int g, int b) {}
void vid_settitle(char *t) {}
void *sys_timer(void) { return 0; }
int sys_elapsed(void *t) { return 0; }
void sys_sleep(int us) {}
void sys_checkdir(char *p, int wr) {}
void sys_sanitize(char *s) {}
void doevents(void) {}
void ev_poll(int w) {}
void die(char *fmt, ...) { for (;;) {} }
int debug_trace;
void debug_disassemble(int a, int c) {}
void xz_crc32_init(void) {}
void xz_crc64_init(void) {}
void *xz_dec_init(int m, unsigned d) { return 0; }
int xz_dec_run(void *s, void *b) { return 1; }
void xz_dec_end(void *s) {}

static void map(u32 k)
{
    pad_set(PAD_UP, !!(k & K_UP));
    pad_set(PAD_DOWN, !!(k & K_DOWN));
    pad_set(PAD_LEFT, !!(k & K_LEFT));
    pad_set(PAD_RIGHT, !!(k & K_RIGHT));
    pad_set(PAD_A, !!(k & K_B));
    pad_set(PAD_B, !!(k & K_A));
    pad_set(PAD_START, !!(k & K_PAUSE));
    pad_set(PAD_SELECT, !!(k & K_HINT));
}

/* ---- battery saves */
static char sav[160];
static u32 sav_sum;
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
        b_fwrite(menu, 1, sizeof menu - 1, f);
        b_fclose(f);
    }
    ((void (*)(const char *))svc(SVC_BOOT, 0x34))("ExtStorage");
    for (;;) {}
}

static void save_sav(int force)
{
    u32 n = mbc.batt ? 8192 * mbc.ramsize : 0, s;
    void *f;
    if (!n || !ram.sbank) return;
    s = sum(ram.sbank[0], n);
    if (s == sav_sum && !force) return;
    sav_sum = s;
    if ((f = b_fopen(sav, "wb"))) {
        b_fwrite(ram.sbank, 1, n, f);
        b_fclose(f);
    }
}

#ifndef MAXSKIP
#define MAXSKIP 3
#endif
u32 ll_frames, ll_drawn;  /* (read by tests) */
static char *heap0;  /* the heap's start for each game */

static void play(int i)
{
    char path[160];
    unsigned long long per, due;
    u32 quit = 0, menu_hold = 0, skipped = 0, n;
    int size, max, f;

    ll_heap_reset(heap0);
    message("Loading", names[i], 0);
    path_of(path, i, 0);
    max = (int)ll_heap_free() - 0x40000;
    ll_rom = malloc(max);
    size = ll_rom ? read_file(path, ll_rom, max) : -1;
    if (size < 0x150 || size > max) {
        message(size < 0 ? "Can't read this file." : size > max ? "Too big." : "Not a Game Boy game?",
                names[i], "Press a button.");
        wait_press();
        return;
    }
    ll_shrink(ll_rom, size);
    ll_romlen = size;
    rom.bank = 0, ram.sbank = 0;
    if (rom_load_simple("-")) {
        message("Not a Game Boy game?", names[i], "Press a button.");
        wait_press();
        return;
    }
    path_of(sav, i, ".sav");
    n = mbc.batt ? 8192 * mbc.ramsize : 0;
    if (n) read_file(sav, ram.sbank[0], n - 1);  /* (max + 1 bytes at most) */
    sav_sum = n ? sum(ram.sbank[0], n) : 0;

    memset(frame, 0, 160 * 240);
    show();
    fb.w = 160; fb.h = 144; fb.pelsize = 2; fb.pitch = 240; fb.ptr = frame + 8 * 240;
    fb.cc[0].r = 4; fb.cc[0].l = 12; fb.cc[1].r = 4; fb.cc[1].l = 4; fb.cc[2].r = 4; fb.cc[2].l = 0;
    fb.enabled = 1;
    pcm.hz = 16000; pcm.buf = sndbuf; pcm.len = sizeof sndbuf; pcm.stereo = 0; pcm.pos = 0;
    sound_reset();
    emu_reset();
    lcd_begin();

    per = 16000000ull * 70224 / 4194304;  /* 59.73 Hz */
    tick();
    due = now;
    for (f = 0;; f++) {
        u32 k = keys();
        map(k);
        vol_keys(k);
        if (vol_show) vol_show--;
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
        if (!(f % 300)) save_sav(0);
        tick();
        if (now > due + 60 * per) due = now;  /* a second behind: start again from here */
        fb.enabled = (now <= due + per / 4 || skipped >= MAXSKIP) && !menu_hold;  /* (still under the prompt) */
        skipped = fb.enabled ? 0 : skipped + 1;

        cpu_emulate(2280);
        while (R_LY > 0 && R_LY < 144) emu_step();
        rtc_tick();
        sound_mix();
        pcm_submit();
        if (!(R_LCDC & 0x80)) cpu_emulate(32832);
        while (R_LY > 0) emu_step();
        if (fb.enabled) {
            if (vol_show) vol_bar();
            show();
            ll_drawn++;
        }
        if (menu_hold) llm_back_prompt(menu_hold, 150), show();
        lcd_begin();
        ll_frames++;

        due += per;
        do tick(); while (now < due);  /* ahead: wait for the frame's time */
    }
    save_sav(1);
    while (keys()) {}
}

void llmain(void)
{
    int sel = 0;
    memset(__bss_start, 0, __bss_end - __bss_start);
    tick();
    b_lcd_base((u32)frame);
    heap0 = ll_heap_mark();
    pcm_start();
    for (;;) {
        sel = pick(sel);
        play(sel);
    }
}
