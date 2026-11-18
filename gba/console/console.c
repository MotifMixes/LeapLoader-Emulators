/* GBA on the Leapster 2: gpSP (libretro's gpsp, GPL-2, with LeapLoader's ARC
   dynarec and changes, #ifdef LL) as the native code of an SD-card image
   (mkimage.py). The image's movie calls the native __gba() once (entry.S);
   it never returns. The console side (BIOS calls, LCD, buttons, clock,
   sound) is the NES port's (nes/console/console.cpp), from the SNES port.

   * ROMs: the .gba files in b:\GBA\ (a folder on the card), picked from a
     list. gpSP keeps as much of the ROM in memory as fits (1 MB blocks) and
     reads the rest from the card in 32 KB pages when it is used. Saves go
     next to the ROM (NAME.sav), written a few seconds after the game changes
     them and when leaving the game.
   * Picture: each drawn GBA line goes to the LCD at once (ll_scanline_out):
     240 -> 160 pixels, each 3 blended to 2; all 160 lines are shown.
   * Speed: frames are timed on the 16 MHz clock; behind the GBA's 59.73 Hz,
     frames are run without drawing (at most MAXSKIP in a row).
   * Sound: gpSP mixes stereo at 32768 Hz; it is played mono at 32 kHz.

   ROM list: Up/Down choose, Left/Right page, A starts, PAUSE searches, B clears it.
   Buttons: A = B, B = A, PAUSE = Start, HINT = Select, HOME + B = L, HOME + A = R;
   HOME + PAUSE held 2 s: back to the ROM list; HINT + A + B held 2.5 s: back to the list
   as well; HOME in the list: back to the LeapLoader menu. */
#include <string.h>
#include "common.h"
#include "libretro.h"
#include <streams/file_stream.h>
#include "font.h"

void *malloc(unsigned);
void free(void *);
unsigned ll_heap_free(void);
char *ll_heap_mark(void);
void ll_heap_reset(char *p);
extern char *ll_heap_cap;
extern char __bss_start[], __bss_end[];
extern u8 open_gba_bios_rom[];
void retro_set_input_state(retro_input_state_t cb);
void init_gamepak_buffer(void);
void llmain(void);

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
static int b_fseek(void *f, long off, int whence)
{ return ((int (*)(void *, long, int))svc(SVC_FILE, 0x38))(f, off, whence); }
static int b_findfirst(const char *pattern, void *info)
{ return ((int (*)(const char *, void *))svc(SVC_FILE, 0x74))(pattern, info); }
static int b_findnext(int h, void *info) { return ((int (*)(int, void *))svc(SVC_FILE, 0x78))(h, info); }
static void b_findclose(void *info) { ((void (*)(void *))svc(SVC_FILE, 0x7C))(info); }
static void b_awake(void) { ((void (*)(void))svc(SVC_SYS, 0x14))(); }

/* ---- buttons */
static inline u32 io_rd(u32 a)
{
    u32 v;
    __asm__ volatile("ld.di %0, [%1]" : "=r"(v) : "r"(a) : "memory");
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

/* ---- the clock: 16 MHz counts */
#define TIMER0 0x0180D084
#define TICK_START (0x1000000 - 0x27100)
#define KERNEL 0x3C7FACB0
unsigned long long now;  /* (global: read by tests) */
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
   The game's lines go straight to the LCD's VRAM with st.di (ll_scanline_out). The list,
   messages, volume panel and prompt are drawn in RAM (frame) and copied with st.di (show,
   show_rows), as the GB port: drawn into VRAM through the data cache and written back with
   dc_fldl, the real console showed black blocks and leftover text (2026-11-01). Under an
   overlay, the game's rows are first read back from VRAM past the cache (grab_rows; VRAM
   is only ever written with st.di, so it holds the newest picture). */
#include "../../common/llcache.h"  /* ll_cache_clean around the file service (NES/GB ports) */
#define VRAM 0x03000000u
static u32 frame_words[160 * 240 / 4];
static unsigned char *const frame = (unsigned char *)frame_words;
static void show_rows(int y0, int y1)
{
    const u32 *w = frame_words + y0 * 60, *e = frame_words + y1 * 60;
    u32 a = VRAM + y0 * 240;
    for (; w < e; w += 4, a += 16) {
        u32 w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3];
        __asm__ volatile("st.di %0, [%1]\n\tst.di %2, [%1, 4]\n\tst.di %3, [%1, 8]\n\tst.di %4, [%1, 12]"
                         : : "r"(w0), "r"(a), "r"(w1), "r"(w2), "r"(w3) : "memory");
    }
}
static void grab_rows(int y0, int y1)
{
    u32 *w = frame_words + y0 * 60, *e = frame_words + y1 * 60, a = VRAM + y0 * 240, x;
    for (; w < e; w++, a += 4) {
        __asm__ volatile("ld.di %0, [%1]" : "=r"(x) : "r"(a) : "memory");
        *w = x;
    }
}
static void show(void) { show_rows(0, 160); }
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
/* Playback rate (input samples per output sample, 1/65536): it follows the rate the game
   makes sound at, slowly (no wobble from the frames' bursts), so a game below full speed
   plays a little lower instead of running the ring dry (crackle). 81% .. 103%. */
static u32 step = 0x10000;  /* 32 kHz in, 32 kHz out */
static volatile int adapt;  /* a game is running (the menu's sounds play at the normal rate) */
static void pcm_isr(void *data)
{
    u32 f = io_rd(IRQ_FLAGS), i, vsrc, n = ring_rd, *dst;
    u32 avail = ring_wr - n;
    if (f & 1u << 20) io_wr(IRQ_FLAGS, 1u << 20);
    if (!(f & 1u << 21)) return;
    io_wr(IRQ_FLAGS, 1u << 21);
    dst = (u32 *)(PCM1_BUF + ((io_rd(PCM1_BASE) - PCM1_BUF) & 0x100 ? 0 : 0x100));
    /* The BIOS's voices (its menu sounds, as LeapLoader's menu plays them) reach the speaker
       only through us: while channel 1 plays, channel 0 writes their mix to memory (the
       half it has just finished), and it is added to ours. */
    vsrc = PCM0_BUF + ((io_rd(PCM0_BASE) - PCM0_BUF) & 0x100 ? 0 : 0x100);
    if (!adapt) step = 0x10000;
    else if (avail && avail + 256 < FILL && step > 0xD000) step -= 32;
    else if (avail > FILL + 256 && step < 0x10800) step += 32;
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
static const char *const dirs[] = {"b:\\GBA\\"};
#define NDIRS 1
#define MAX_ROMS 300
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
    return e && (!nocase_cmp(e + 1, "gba") || !nocase_cmp(e + 1, "agb"));
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
    while (n < max && (k = b_fread(buf + n, 1, max - n < 0x8000 ? max - n : 0x8000, f)) > 0)
        n += k;
    b_fclose(f);
    return n;
}

/* ---- gpSP's files (libretro filestream): the ROM, read through the BIOS.
   The BIOS has no ftell here, so the position is kept, and the size is found
   with fseek + 1-byte reads. */
struct RFILE { void *f; int64_t pos, size; };
static struct RFILE rfiles[2];
RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
    struct RFILE *r = !rfiles[0].f ? &rfiles[0] : !rfiles[1].f ? &rfiles[1] : 0;
    if (!r || !(r->f = b_fopen(path, mode & RETRO_VFS_FILE_ACCESS_WRITE ? "wb" : "rb"))) return 0;
    r->pos = 0;
    r->size = -1;
    return r;
}
int filestream_close(RFILE *r)
{
    b_fclose(r->f);
    r->f = 0;
    return 0;
}
int64_t filestream_get_size(RFILE *r)
{
    if (r->size < 0) {
        unsigned char c;
        u32 lo = 0, bit;  /* the largest offset with a byte there, + 1 */
        ll_cache_clean();
        for (bit = 1u << 26; bit; bit >>= 1)
            if (!b_fseek(r->f, lo + bit - 1, SEEK_SET) && b_fread(&c, 1, 1, r->f) == 1) lo += bit;
        r->size = lo;
        b_fseek(r->f, r->pos, SEEK_SET);
    }
    return r->size;
}
int64_t filestream_seek(RFILE *r, int64_t off, int whence)
{
    int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? r->pos + off : filestream_get_size(r) + off;
    if (b_fseek(r->f, p, SEEK_SET)) return -1;
    r->pos = p;
    return 0;
}
int64_t filestream_tell(RFILE *r) { return r->pos; }
int64_t filestream_read(RFILE *r, void *d, int64_t len)
{
    int64_t n = 0;
    int k;
    ll_cache_clean();  /* (the file service writes the buffer past the cache) */
    while (n < len && (k = b_fread((char *)d + n, 1, len - n < 0x8000 ? len - n : 0x8000, r->f)) > 0) n += k;
    r->pos += n;
    return n;
}

/* ---- messages */
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
        ll_cache_clean();
        b_fwrite(menu, 1, sizeof menu - 1, f);
        b_fclose(f);
    }
    ((void (*)(const char *))svc(SVC_BOOT, 0x34))("ExtStorage");
    for (;;) {}
}

/* ---- the ROM list, drawn like LeapLoader's menu (../../../common/llmenu.h) */
#define LLM_LCD frame
#include "../../common/llmenu.h"
#include "../../common/llvol.h"  /* the volume panel, shared with LeapLoader's menu */
static const struct llm_theme theme = {"GBA", "gpSP", 0x013, 0x426, 0x335, 0x99B, 0xDDF, 0x012, 0x223, 0x9AF, 0x012, 0x012, 0xBBE, 0x33A, 0x113};

static int pick(int sel)
{
    for (;;) {
        list_roms();
        if (!nroms) {
            llm_message(&theme, "No GBA games yet.", "Put .gba files in the", "GBA folder on the card.", "HOME: back to LeapLoader");
            if (wait_press() & K_HOME) to_menu();
            continue;
        }
        return llm_pick(&theme, names[0], sizeof names[0], nroms, sel);
    }
}

/* ---- the picture, from the core as it draws: a GBA line (RGB565) -> an LCD line,
   each 3 GBA pixels a, b, c -> (a+b)/2, (b+c)/2 (the bench driver's lcd_pair). A line
   whose source is unchanged since it was last sent is not packed again. */
static int drawing;  /* this frame is shown (the volume panel goes over it) */
static inline u32 lcd_pair(u32 a, u32 b, u32 c)
{
    u32 x = ((a | c << 16) >> 1 & 0x7BEF7BEF) + ((b | b << 16) >> 1 & 0x7BEF7BEF);
    return (x & 0xF000) | ((x >> 20) & 0xF00) | ((x >> 3) & 0xF000F0) | ((x >> 1) & 0xF000F);
}
static u32 lcd_hash[160];
void ll_scanline_out(u32 vcount, const u16 *line)
{
    u32 *w = (u32 *)VRAM + vcount * 60;
    const u32 *q = (const u32 *)line;
    const u16 *s = line;
    u32 h = 0x811C9DC5, x;
    if (vcount >= 160) return;
    for (x = 0; x < 120; x += 4)
        h = (((((h ^ q[x]) * 0x01000193) ^ q[x + 1]) * 0x01000193 ^ q[x + 2]) * 0x01000193 ^ q[x + 3]) * 0x01000193;
    if (h == lcd_hash[vcount]) return;
    lcd_hash[vcount] = h;
    for (x = 0; x < 20; x++, s += 12, w += 3) {
        u32 p0 = lcd_pair(s[0], s[1], s[2]), p1 = lcd_pair(s[3], s[4], s[5]);
        u32 p2 = lcd_pair(s[6], s[7], s[8]), p3 = lcd_pair(s[9], s[10], s[11]);
        /* st.di: the LCD reads VRAM past the data cache (cached stores reach it late or never) */
        __asm__ volatile("st.di %0, [%3]\n\tst.di %1, [%3, 4]\n\tst.di %2, [%3, 8]"
                         : : "r"(p0 | p1 << 24), "r"(p1 >> 8 | p2 << 16), "r"(p2 >> 16 | p3 << 8), "r"(w) : "memory");
    }
}

/* ---- buttons for gpSP (libretro joypad ids) */
static u32 pad;
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    u32 home = pad & K_HOME;
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_A: return !home && (pad & K_B);
    case RETRO_DEVICE_ID_JOYPAD_B: return !home && (pad & K_A);
    case RETRO_DEVICE_ID_JOYPAD_L: return home && (pad & K_B);
    case RETRO_DEVICE_ID_JOYPAD_R: return home && (pad & K_A);
    case RETRO_DEVICE_ID_JOYPAD_START: return !home && (pad & K_PAUSE);
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return (pad & K_HINT) != 0;
    case RETRO_DEVICE_ID_JOYPAD_UP: return (pad & K_UP) != 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN: return (pad & K_DOWN) != 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT: return (pad & K_LEFT) != 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT: return (pad & K_RIGHT) != 0;
    }
    return 0;
}

/* ---- saves: gpSP's backup memory, as big as the game's type */
static char sav[128];
static u32 sav_sum;
static u32 sav_size(void)
{
    switch (backup_type) {
    case BACKUP_SRAM: return 0x8000;
    case BACKUP_FLASH: return flash_bank_cnt * 0x10000;
    case BACKUP_EEPROM: return eeprom_size * 512;
    }
    return 0;
}
static u32 sum(const u8 *p, u32 n)
{
    u32 h = 2166136261u;
    for (n /= 4; n--; p += 4) h = (h ^ *(const u32 *)p) * 16777619u;
    return h;
}
static void save_sav(int force)
{
    u32 n = sav_size(), s;
    void *f;
    if (!n) return;
    s = sum(gamepak_backup, n);
    if (s == sav_sum && !force) return;
    sav_sum = s;
    if ((f = b_fopen(sav, "wb"))) {
        ll_cache_clean();  /* (the file service reads the buffer past the cache) */
        b_fwrite(gamepak_backup, 1, n, f);
        b_fclose(f);
    }
}

/* what libretro.c would define */
u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;
u32 netplay_num_clients = 0, netplay_client_id = 0;
void netpacket_poll_receive(void) {}
void netpacket_send(uint16_t client_id, const void *buf, size_t len) {}
boot_mode selected_boot_mode = boot_game;
u32 skip_next_frame = 0;
int sprite_limit = 1;
void set_fastforward_override(bool fastforward) {}
int dynarec_enable = 1;

#ifndef MAXSKIP
#define MAXSKIP 7  /* SMA2 in Ribbit: 3 -> 88.6% speed, 7 -> 99.2% (2026-10-29) */
#endif
#define RESERVE 0x40000  /* heap kept after gpSP's ROM blocks */
u32 ll_frames, ll_drawn;  /* (read by tests) */
static int started;       /* gpSP's sound and BIOS are set up */
static char *heap0;       /* the heap's start for each game */
extern RFILE *gamepak_file_large;
static s16 audio[2 * 1024];

static void play(int i)
{
    char path[128];
    struct retro_game_info gi = {0, 0, 0, 0};
    unsigned long long per, due;
    u32 quit = 0, skipped = 0, menu_hold = 0, f;

    message("Loading", names[i], 0);
    path_of(path, i, 0);
    /* each game: the heap from its start, gpSP's ROM blocks as big as it allows */
    if (gamepak_file_large) {
        filestream_close(gamepak_file_large);
        gamepak_file_large = NULL;
    }
    ll_heap_reset(heap0);
    gba_screen_pixels = malloc(GBA_SCREEN_BUFFER_SIZE);
    ll_heap_cap = ll_heap_mark() + ll_heap_free() - RESERVE;
    init_gamepak_buffer();
    ll_heap_cap = 0;
    if (!started) {
        retro_set_input_state(input);
        init_sound();
        /* 32768 Hz: at 16 kHz the square-wave channels alias audibly (harsh, out-of-tune tones) */
        sound_freq_bits = 15; sound_frequency = 1u << 15; sound_frequency_changed();
        memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
        started = 1;
    }
    gi.path = path;
    if (load_gamepak(&gi, path, 0, 0, 0) != 0) {
        message("Can't play this game:", names[i], "Press a button.");
        wait_press();
        return;
    }
    reset_gba();
    path_of(sav, i, ".sav");
    read_file(sav, gamepak_backup, sizeof gamepak_backup);
    sav_sum = sum(gamepak_backup, sav_size());
    memset(lcd_hash, 0, sizeof lcd_hash);
    adapt = 1;

    memset(frame, 0, 160 * 240);
    show();
    per = 16000000ull * 10000 / 597275;  /* 59.7275 Hz */
    tick();
    due = now;
    for (f = 0;; f++) {
        u32 k = keys();
        int n, j;
        pad = k;
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
        if (!(f % 300)) save_sav(0);
        tick();
        if (now > due + 60 * per) due = now;  /* a second behind: start again from here */
        drawing = (now <= due + per / 4 || skipped >= MAXSKIP) && !menu_hold;  /* (still under the prompt) */
        skipped = drawing ? 0 : skipped + 1;
        skip_next_frame = !drawing;
        update_input();
        execute_arm_translate(execute_cycles);
        {
            int dy = llv_offset(), y0 = LLV_Y + dy;  /* the volume panel (llvol.h) over the game's rows */
            if (drawing && dy < LLV_HIDE)
                grab_rows(y0, 160), llv_draw(dy, fill), show_rows(y0, 160), memset(lcd_hash + y0, 0, (160 - y0) * sizeof *lcd_hash);
        }
        if (drawing) ll_drawn++;
        if (menu_hold) {  /* (every line is drawn again after it: none is "unchanged") */
            grab_rows(34, 122), llm_back_prompt(menu_hold, 150), show_rows(34, 122);
            memset(lcd_hash, 0, sizeof lcd_hash);
        }
        /* the sound into the ring: mono, without its constant offset (the GBA's speaker
           is AC-coupled; games leave large offsets), at the volume */
        n = sound_read_samples(audio, sizeof audio / sizeof *audio / 2);
        {
            static int dc_x, dc_y;  /* DC blocker y = x - x' + 0.996 y' (y in 1/256) */
            u32 w = ring_wr;
            int g = llv_gain[llv_level];
            for (j = 0; j < n && w - ring_rd < RING - 1; j++) {
                int x = (audio[2 * j] + audio[2 * j + 1]) >> 1, y;
                dc_y += (x - dc_x) * 256 - (dc_y >> 8);
                dc_x = x;
                y = dc_y >> 8;
                y = y > 32767 ? 32767 : y < -32768 ? -32768 : y;
                ring[w++ & (RING - 1)] = y * g >> 10;  /* -12 dB: GBA square waves at full level were harsh and loud (peaks ~17000) */
            }
            ring_wr = w;
        }
        ll_frames++;
        due += per;
        do tick(); while (now < due);  /* ahead: wait for the frame's time */
    }
    save_sav(1);
    adapt = 0;
    while (keys()) {}
}

void llmain(void)
{
    int sel = 0;
    memset(__bss_start, 0, __bss_end - __bss_start);
    tick();
    b_lcd_base(VRAM);
    heap0 = ll_heap_mark();
    pcm_start();
    llv_load();  /* the level LeapLoader's menu left (llvol.h) */
    for (;;) {
        sel = pick(sel);
        play(sel);
    }
}
