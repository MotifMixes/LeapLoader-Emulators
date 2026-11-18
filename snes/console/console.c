/* This port runs the fast core (ll65.S, the SPC700 in asm, the new renderer llr.c, the Super FX
   translator fxjit.c). The new renderer writes each finished row straight into the LCD's VRAM
   with st.di (0x0300_0000, stride 240, 12 bpp), as the bench does; menus, messages and overlays (volume
   panel, back prompt) are drawn in LLFrame (RAM) and copied with st.di (show). Where the notes below say
   the picture is drawn in RAM, that was the old renderer. */
/* SNES on the Leapster 2: snes9x2002 (with LeapLoader's speed work) as the
   native code of an SD-card image (mkimage.py). The image's movie calls the
   native __snes() once (entry.S); it never returns.

   * ROMs: the files in b:\SNES\ (the card's SNES folder), picked from a list.
     Battery saves go next to the ROM (NAME.srm), written a few seconds after
     the game changes them and when leaving the game.
   * Picture: 160 x 160, 12 bpp (gfx.c LL_LCD12), drawn straight into the
     LCD's frame in the on-chip VRAM (0x0300_0000: base 0, stride 240, format
     4, as the BIOS sets it up), whose lines are then flushed from the data
     cache. (LcdMPI CopyToScreen, the BIOS's DMA from RAM, took ~17-19 ms per
     frame in Ribbit even without waiting for the copy: our task waited for
     the driver's lock while other tasks ran.)
   * Buttons: read from the GPIO input register (0x0180_004C, active low), so
     they work while no BIOS task runs.
   * Speed: frames are timed on the 16 MHz hardware timer 0; behind the 60 Hz
     (50 Hz PAL) clock, drawing is skipped (libretro's auto frameskip).
   * Sound: mixed at 16 kHz mono, played through PCM DMA channel 1 (32 kHz,
     signed 16-bit) from our own interrupt handler (pcm_isr).

   ROM list: Up/Down choose, Left/Right page, A starts, PAUSE searches, B clears it.
   Buttons (2026-11-17, user): A = SNES Y, B = SNES B, HINT = SNES A, PAUSE = Start;
   HOME held: + A = X, + B = Select, + Left/Right = L/R;
   HOME + PAUSE held 2 s, or HINT + A + B held 2.5 s: back to the ROM list (saved);
   HOME in the ROM list: back to the LeapLoader menu. */
#include <string.h>
#include "libretro.h"
#include "snes9x.h"
#include "memmap.h"
#include "font.h"
#ifdef LL_SNDDBG
#include "apu.h"
#include "soundux.h"
unsigned ll_snd[64], ll_lvl[96];
#endif

typedef unsigned u32;
void *malloc(unsigned);
int snprintf(char *, unsigned, const char *, ...);
unsigned ll_heap_free(void);
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
/* Writing the frame's lines back from the data cache (16-byte lines): DC_FLDL
   (aux 0x4C) per line, then wait while DC_CTRL (aux 0x48) bit 8 shows a flush in
   progress. The BIOS's own range flush ([[gp + 4] + 0x10] + 0x1A8, BIOS 0.6
   0x4004_BE48: interrupts off, a tag check and a status poll per line) took
   17 ms per frame in Ribbit, and CopyToScreen's whole-cache flush as long. */
static void flush_lines(const void *p, int n)
{
    u32 a = (u32)p & ~15u, e = (u32)p + n, s;
    for (; a < e; a += 16) __asm__ volatile("sr %0, [0x4c]" : : "r"(a) : "memory");
    do __asm__ volatile("lr %0, [0x48]" : "=r"(s)); while (s & 0x100);
}
/* the idle timer's reset (otherwise the console powers off after 5 minutes) */
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

/* The clock: the RTOS's 10 ms ticks and timer 0 within the tick. Timer 0
   counts at 16 MHz from TICK_START to its limit 0x100_0000 and goes on from 0;
   the tick interrupt (BIOS 0.6 0x4004_C928, B5 the same code) adds a tick to
   the 64-bit count at [KERNEL] + 0x34 for each 0x2_7100 counts and writes the
   rest back above TICK_START, so ticks x 0x2_7100 + the position in the
   period is continuous, also over long gaps between reads (CopyToScreen's
   wait, other tasks). [KERNEL] is 0x3C7F_ACB0 in BIOS 0.6 and B5. LLTick runs
   every SNES line (cpuexec.c) and in waits. */
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

/* ---- the picture: 160 x 160 at 12 bpp, two pixels in three bytes (g0b0, r0r1, g1b1) */
/* The picture is drawn in RAM (gfx.c, the list, messages) and copied to the LCD's VRAM with
   st.di: on a real Leapster 2 lines drawn into VRAM through the data cache reached the LCD late
   or not at all, and DC_FLDL didn't help (NES/GB ports, 2026-11-01). */
#define VRAM 0x03000000u
static u32 frame_words[160 * 240 / 4];
unsigned char *LLFrame = (unsigned char *)frame_words;  /* the frame (gfx.c draws into it) */
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

/* ---- sound: PCM DMA channel 1 plays signed 16-bit samples at 32 kHz from a
   ring of two 128-sample halves and raises interrupt 0x13 after each half
   (flag bits 20/21 of 0x0180_0080, write 1 to clear; enable bits in
   0x0180_0084). Set up as BIOS 0.6 0x4003_D712 does (the buffers in on-chip
   VRAM, as the BIOS's 0x0300_EC00 + 0x200 / + 0x600; channel 0 captures the
   voice mix, as the BIOS needs it to start channel 1 in step; 0x0180_20F4 bit
   10 sends PCM to the DAC); our handler replaces the BIOS's (0x4003_D994) in
   the kernel's table (kernel services [[gp + 4] + 0x10]: +0x180 install ISR,
   +0x198 / +0x19C interrupts on / off). The handler fills the half just played
   from a 16 kHz ring (audio_batch), 2x with linear steps; the step follows the
   ring's level (+-0.8%) so the 60.1 Hz SNES frame and our 60 Hz clock don't
   drift into an underrun. No multiplies in the handler (MUL64 registers). */
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
#define RING 4096  /* 16 kHz samples (256 ms) */
#define FILL 800   /* the ring's level aimed at (50 ms) */
static inline void io_wr(u32 a, u32 v) { __asm__ volatile("st.di %0, [%1]" : : "r"(v), "r"(a) : "memory"); }
static short ring[RING];
static volatile u32 ring_wr;  /* samples written (audio_batch) */
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
    /* the position (base + bytes played) says which half is playing now */
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
#define DIR "b:\\SNES\\"
#define MAX_ROMS 200
static char names[MAX_ROMS][64];
static int nroms;
static char info[0x200] __attribute__((aligned(4)));

/* (newlib's strcasecmp reads its locale through gp; port.h maps it to strcmp) */
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
    static const char *ext[] = {"sfc", "smc", "swc", "fig", "bin", 0};
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
    /* alphabetical, ignoring case */
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

/* ---- ROM list */
static void message(const char *a, const char *b, const char *c)
{
    memset(LLFrame, 0, 160 * 240);
    text(4, 40, a, 0xFFF);
    if (b) text(4, 56, b, 0xAAA);
    if (c) text(4, 68, c, 0xAAA);
    show();
}

/* the keys when one is newly pressed, or again every 1/12 s after 1/3 s held */
static u32 wait_keys(void)
{
    static u32 held;
    static unsigned long long again;
    for (;;) {
        u32 k = keys();
        LLTick();
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
#define tick LLTick
#define LLM_LCD LLFrame
#include "../../common/llmenu.h"
#include "../../common/llvol.h"  /* the volume panel, shared with LeapLoader's menu */
static const struct llm_theme theme = {"SNES", "snes9x2002", 0x112, 0x889, 0x316, 0x74C, 0xB9F, 0x112, 0x214, 0xDCF, 0x112, 0x112, 0xCCD, 0x74C, 0x214};

static int pick(int sel)
{
    for (;;) {
        list_roms();
        if (!nroms) {
            llm_message(&theme, "No SNES games yet.", "Put .sfc / .smc files in", "the SNES folder.", "HOME: back to LeapLoader");
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

/* ---- libretro frontend */
static retro_audio_buffer_status_callback_t buffcb;
u32 ll_frames, ll_drawn;  /* (read by tests: emulated frames, drawn frames, */
u32 ll_tskip, ll_tdraw;  /* 16 MHz counts spent in skipped / drawn frames, */
u32 ll_tshow;  /* and in CopyToScreen) */
static bool env(unsigned cmd, void *data)
{
    if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE) {
        struct retro_variable *v = data;
#ifdef LL_FLOOR
        /* test build: nothing drawn (the bench's "floor") */
        if (!strcmp(v->key, "snes9x2002_frameskip")) { v->value = "fixed_interval"; return true; }
        if (!strcmp(v->key, "snes9x2002_frameskip_interval")) { v->value = "99"; return true; }
#elif !defined HWTIME  /* (HWTIME: no frameskip, every frame drawn, as snesbench) */
        if (!strcmp(v->key, "snes9x2002_frameskip")) { v->value = "auto"; return true; }
#endif
        if (!strcmp(v->key, "snes9x2002_transparency")) { v->value = "disabled"; return true; }
        return false;
    }
    if (cmd == RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK) {
        buffcb = data ? ((struct retro_audio_buffer_status_callback *)data)->callback : 0;
        return true;
    }
    return cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT;
}
static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    /* the new renderer has already put the frame on the LCD (st.di); overlays go on top in play() */
    if (data) ll_drawn++;
}
static size_t audio_batch(const int16_t *d, size_t n)
{
    u32 w = ring_wr, i;
#ifdef LL_SNDDBG
    {
        extern unsigned ll_frames, ll_lvl[96];
        u32 k = ll_frames / 60;
        if (k < 32)
            for (i = 0; i < n; i++) {
                int l = d[2 * i], r = d[2 * i + 1], m = (l + r) >> 1;
                ll_lvl[3 * k] += (l < 0 ? -l : l) >> 4;
                ll_lvl[3 * k + 1] += (r < 0 ? -r : r) >> 4;
                ll_lvl[3 * k + 2] += (m < 0 ? -m : m) >> 4;
            }
    }
#endif
    {
        int g = llv_gain[llv_level];
        for (i = 0; i < n && w - ring_rd < RING - 1; i++, d += 2)
            ring[w++ & (RING - 1)] = (d[0] + d[1]) * g >> 9;
    }
    ring_wr = w;
    return n;
}
static void audio(int16_t l, int16_t r) {}

static void poll(void) {}

static u32 pad;  /* libretro joypad bits */
static int16_t input(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return pad;
    return pad >> id & 1;
}
static u32 map(u32 k)
{
    u32 m = 0;
#define B_(x) (1u << RETRO_DEVICE_ID_JOYPAD_##x)
    if (k & K_UP) m |= B_(UP);
    if (k & K_DOWN) m |= B_(DOWN);
    if (k & K_PAUSE) m |= B_(START);
    if (k & K_HINT) m |= B_(A);
    if (k & K_HOME) {
        if (k & K_A) m |= B_(X);
        if (k & K_B) m |= B_(SELECT);
        if (k & K_LEFT) m |= B_(L);
        if (k & K_RIGHT) m |= B_(R);
    } else {
        if (k & K_LEFT) m |= B_(LEFT);
        if (k & K_RIGHT) m |= B_(RIGHT);
        if (k & K_A) m |= B_(Y);
        if (k & K_B) m |= B_(B);
    }
#undef B_
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
    u32 n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM), s;
    unsigned char *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
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

static void play(const char *name)
{
    char path[128];
    struct retro_game_info game;
    unsigned long long per, due;
    u32 n, quit = 0, menu_hold = 0;
    int size, frame;

    message("Loading", name, 0);
    path_of(path, name, 0);
    size = read_file(path, Memory.ROM, MAX_ROM_SIZE + 0x200);
    if (size < 0 || size > MAX_ROM_SIZE + 0x200) {
        message(size < 0 ? "Can't read this file." : "Too big (4 MB at most).", name, "Press a button.");
        while (!keys()) {}
        while (keys()) {}
        return;
    }
    memset(&game, 0, sizeof game);
    game.path = path;
    game.data = Memory.ROM;  /* LoadROM copies it onto itself */
    game.size = size;
    if (!retro_load_game(&game)) {
        message("Not a SNES game?", name, "Press a button.");
        while (!keys()) {}
        while (keys()) {}
        return;
    }
    path_of(srm, name, ".srm");
    n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (n && retro_get_memory_data(RETRO_MEMORY_SAVE_RAM))
        read_file(srm, retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), n);
    srm_sum = n ? sum(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), n) : 0;

    per = 16000000ull / (Settings.PAL ? 50 : 60);
    LLTick();
    due = now;
    for (frame = 0;; frame++) {
        u32 k = keys();
        pad = map(k);
        llv_keys(k);
#ifdef LL_BENCHINPUT
        /* test build: the bench's input (port/main.c) for its first 900 frames, counters frozen after */
        pad = (frame % 90 >= 60 && frame % 90 < 66) << RETRO_DEVICE_ID_JOYPAD_START
            | (frame > 400) << RETRO_DEVICE_ID_JOYPAD_RIGHT | (frame > 400 && frame % 40 < 20) << RETRO_DEVICE_ID_JOYPAD_B;
        if (frame == 900) { ll_frames |= 0x80000000; }
        if (ll_frames >> 31) { retro_run(); due = now; continue; }
#endif
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
        if (buffcb) buffcb(true, 0, now > due + per / 4 || menu_hold);  /* (still under the prompt) */
        {
            unsigned long long t0 = now;
            u32 d0 = ll_drawn;
            retro_run();
            LLTick();
            if (ll_drawn != d0) ll_tdraw += now - t0;
            else ll_tskip += now - t0;
            /* overlays: over a newly drawn game frame, or the prompt's bar as it grows */
            int dy = llv_offset();  /* the volume panel (llvol.h), LLV_HIDE when it is not up */
            if ((dy < LLV_HIDE && ll_drawn != d0) || menu_hold) {
                t0 = now;
                pull();
                if (menu_hold) llm_back_prompt(1, 150), llm_back_prompt(menu_hold, 150);
                if (dy < LLV_HIDE) llv_draw(dy, fill);
                show();
                LLTick();
                ll_tshow += now - t0;
            }
        }
        ll_frames++;
#ifdef LL_SNDDBG
        /* test build: the sound CPU's RAM hash and DSP FLG / main volume every 60 frames */
        if (!(frame % 60) && frame / 60 < 32) {
            extern unsigned ll_snd[64];
            ll_snd[frame / 30] = sum(IAPU.RAM, 0x10000);
            ll_snd[frame / 30 + 1] = APU.DSP[0x6C] | APU.DSP[0x0C] << 8 | APU.DSP[0x4C] << 16 | so.mute_sound << 24;
        }
#endif
        due += per;
        do LLTick(); while (now < due);  /* ahead: wait for the frame's time */
    }
    save_srm(1);
    retro_unload_game();
    while (keys()) {}
}

#ifdef HWTIME
/* ---- -DHWTIME (package SNESTIME): fixed scripted runs, every frame drawn, no pacing; per-frame 16 MHz
   ticks of retro_run (emulation + drawing + LCD output) over the same windows as snes-hw's hwbench.sh
   (its first marked frame, which includes start-up, is left out: dkc/smw 2..899, mk 1001..3199);
   results in b:\Leapster\Apps\SNESTIME	iming.txt; then a green screen (red: something failed). */
#define HT_OUT "b:\\Leapster\\Apps\\SNESTIME\\timing.txt"
static const struct { const char *name, *rom; int frames, from, script; } ht_games[] = {
    {"dkc", "Donkey Kong Country.smc", 900, 2, 0},
    {"mk", "Super Mario Kart.smc", 3200, 1001, 1},
    {"smw", "Super Mario World.smc", 900, 2, 0},
};
#define HT_N 3
#define HT_MAX 2200
static u32 ht_ticks[HT_MAX], ht_sorted[HT_MAX];
static char ht_buf[96 * 1024];
static int ht_len;
static void ht_put(const char *f, u32 a, u32 b, u32 c)
{
    if (ht_len < (int)sizeof ht_buf - 128) ht_len += snprintf(ht_buf + ht_len, sizeof ht_buf - ht_len, f, a, b, c);
}
/* the bench's input (snes-hw port/main.c): script 0 = its default (Start 6 of every 90 frames, Right and
   B pulses from 401: dkc, and smw's "Start then right"); 1 = games.txt mk (-DINSCRIPT -DIN_START=100
   -DIN_UNTIL=1000 -DIN_HOLD=1000 -DIN_HOLDM=1 -DIN_PULSEM=64) */
static u32 ht_mask(int script, u32 f)
{
    u32 m = 0;
    if (script == 1) {
        if (f >= 100 && f < 1000 && f % 60 >= 54) m |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
        if (f >= 1000) { m |= 1; if (f % 40 < 6) m |= 64; }
        return m;
    }
    if (f % 90 >= 60 && f % 90 < 66) m |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
    if (f > 400) m |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (f > 400 && f % 40 < 20) m |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
    return m;
}
/* % of a 60 Hz frame, in tenths: ticks x 60 x 1000 / 16 MHz = ticks x 3 / 800 */
#define HT_PM(t) ((t) * 3 / 800)
static int ht_run(int g)
{
    char path[128];
    struct retro_game_info game;
    int size, n = 0, i, j;
    u32 f, sum = 0, worst = 0, wf = 0, over = 0;
    message("SNES Timing", ht_games[g].name, ht_games[g].rom);
    path_of(path, ht_games[g].rom, 0);
    size = read_file(path, Memory.ROM, MAX_ROM_SIZE + 0x200);
    if (size < 0 || size > MAX_ROM_SIZE + 0x200) { ht_put("game %u: can't read the ROM\n", g, 0, 0); return 0; }
    memset(&game, 0, sizeof game);
    game.path = path;
    game.data = Memory.ROM;
    game.size = size;
    if (!retro_load_game(&game)) { ht_put("game %u: load failed\n", g, 0, 0); return 0; }
    for (f = 0; f < (u32)ht_games[g].frames; f++) {
        unsigned long long t0;
        pad = ht_mask(ht_games[g].script, f);
        LLTick();
        t0 = now;
        retro_run();
        LLTick();
        if (f >= (u32)ht_games[g].from && n < HT_MAX) ht_ticks[n++] = (u32)(now - t0);
        if (!(f & 63)) b_awake();
    }
    retro_unload_game();
    for (i = 0; i < n; i++) {
        u32 t = ht_ticks[i];
        sum += t >> 4;
        if (t > worst) worst = t, wf = ht_games[g].from + i;
        if (t > 266667) over++;
        for (j = i; j > 0 && ht_sorted[j - 1] > t; j--) ht_sorted[j] = ht_sorted[j - 1];
        ht_sorted[j] = t;
    }
    {
        u32 avg = sum / n << 4, p95 = ht_sorted[n * 95 / 100 - 1];  /* (sum: ticks / 16, no 64-bit division) */
        ht_put("game %s\n", (u32)ht_games[g].name, 0, 0);
        ht_put("frames %u..%u (%u measured)\n", ht_games[g].from, ht_games[g].frames - 1, n);
        ht_put("avg %u.%u%%  ", HT_PM(avg) / 10, HT_PM(avg) % 10, 0);
        ht_put("worst %u.%u%% (f%u)  ", HT_PM(worst) / 10, HT_PM(worst) % 10, wf);
        ht_put("p95 %u.%u%%  ", HT_PM(p95) / 10, HT_PM(p95) % 10, 0);
        ht_put(">100%%: %u/%u  avg ticks %u\n", over, n, avg);
    }
    ht_put("raw ticks (16 MHz), frame %u on:\n", ht_games[g].from, 0, 0);
    for (i = 0; i < n; i++) ht_put(i % 10 == 9 || i == n - 1 ? "%u\n" : "%u ", ht_ticks[i], 0, 0);
    ht_put("\n", 0, 0, 0);
    return 1;
}
static void ht_main(void)
{
    int g, ok = 1;
    void *fl;
    ht_put("SNES Timing (snes-console, obj-m9f core): retro_run ticks at 16 MHz, %% of a 60 Hz frame = ticks / 266667\n\n", 0, 0, 0);
    for (g = 0; g < HT_N; g++) ok &= ht_run(g);
    message("SNES Timing", "writing timing.txt", 0);
    fl = b_fopen(HT_OUT, "wb");
    if (fl) {
        ll_cache_clean();
        if (b_fwrite(ht_buf, 1, ht_len, fl) != ht_len) ok = 0;
        b_fclose(fl);
    } else ok = 0;
    memset(LLFrame, 0, 160 * 240);
    fill(0, 0, 160, 160, ok ? 0x0C0 : 0xC00);
    text(4, 70, ok ? "Done: timing.txt" : "Failed", 0xFFF);
    text(4, 84, "Any button: menu", 0xFFF);
    show();
    while (keys()) {}
    while (!keys()) LLTick();
    to_menu();
}
#endif

void llmain(void)
{
    int sel = 0;
    memset(__bss_start, 0, __bss_end - __bss_start);
    LLTick();
    b_lcd_base(VRAM);
    message("SNES", "starting...", 0);
    retro_set_environment(env);
    retro_set_video_refresh(video);
    retro_set_audio_sample(audio);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(poll);
    retro_set_input_state(input);
    retro_init();
    Settings.SoundPlaybackRate = 16000;  /* cheaper mixing (pcm_isr plays it at 32 kHz) */
    Settings.InterpolatedSound = FALSE;
    if (!Memory.ROM) {
        message("Out of memory.", 0, 0);
        for (;;) {}
    }
    pcm_start();
    llv_load();  /* the level LeapLoader's menu left (llvol.h) */
#ifdef HWTIME
    ht_main();
#endif
    for (;;) {
        sel = pick(sel);
        play(names[sel]);
    }
}
