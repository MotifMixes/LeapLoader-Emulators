/* llmenu.h: the emulators' ROM list, drawn like LeapLoader's menu (research/menugen.py):
   starry sky, glossy pills (4 a page) with a glow on the choice, a frog, "My Games: N",
   a footer with the page, and a search keyboard. Each emulator gives its own colours.

   Included by a console after it defines: LLM_LCD (the LCD's frame: 160 lines of 240 bytes,
   two 0xRGB pixels in three bytes), show(), keys(), tick() + now (16 MHz), b_awake(), to_menu(),
   the K_ buttons and font[95][9] (font.h). Each frame is drawn in RAM (malloc) and then copied
   to the LCD at once: drawn straight into it, half-drawn frames showed.

   List: Up/Down choose, Left/Right page, A starts, PAUSE searches, B clears the search,
   HOME goes back to the LeapLoader menu. Touch: a game starts it, the magnifying glass
   searches, the arrows page.
   Keyboard: the D-pad moves, A types, B deletes, PAUSE (or OK) searches, HOME cancels;
   touch types a key. The BIOS plays its menu sounds as in LeapLoader's menu (llm_sound:
   it needs the console's svc()). */

#include <stdlib.h>

struct llm_theme {
    const char *title, *foot;           /* header text; footer right (the emulator core) */
    u32 sky0, sky1;                     /* sky from top to bottom, 0xRGB */
    u32 rim, fill, shine, shadow, ink;  /* pills: rim, face, shine, drop shadow, text shadow */
    u32 head, head_shadow;              /* title */
    u32 foot_bg, foot_text;
    u32 frog, frog_dark;
};

#define LLM_TOP 44     /* list area top (menugen TOP) */
#define LLM_PITCH 24
#define LLM_X 10       /* pill */
#define LLM_W 122
#define LLM_H 20
#define LLM_ROWS 4
#define LLM_TEXT 7     /* title inset */
#define LLM_TOOL_X 138
#define LLM_TOOL_W 19
#define LLM_FOOTER 150
#define LLM_GLOW 0xFD2
#define LLM_QMAX 12
#define LLM_MAX 2048
#define LLM_FRAME (16000000 / 20)  /* 20 fps, as the menu */

static const char *const llm_frog[] = {
    "..GGG......GGG..", ".GGGGG....GGGGG.", ".GdwGGGGGGGGdwG.", ".GddGGGGGGGGddG.",
    "GGGGGGGGGGGGGGGG", "GGGGGdGGGGdGGGGG", "GGGGGGGGGGGGGGGG", "GGdGGGGGGGGGGdGG",
    "GGGddddddddddGGG", "GGGGddddddddGGGG", "GGGGGGddddGGGGGG", ".GGGGGGGGGGGGGG.",
    "..GGGGGGGGGGGG..", 0};
static const char *const llm_search_icon[] = {
    "...####.....", "..##..##....", ".##....##...", ".#......#...", ".#......#...", ".##....##...",
    "..##..##....", "...#####....", ".......###..", "........###.", ".........###", "..........##", 0};
static const char *const llm_up_icon[] = {
    ".....##.....", "....####....", "...######...", "..########..", ".##########.", "############",
    "....####....", "....####....", "....####....", 0};
static const char *const llm_down_icon[] = {
    "....####....", "....####....", "....####....", "############", ".##########.", "..########..",
    "...######...", "....####....", ".....##.....", 0};
static const unsigned char llm_stars[][2] = {
    {12, 6}, {58, 3}, {97, 9}, {148, 4}, {30, 52}, {152, 60}, {4, 88}, {156, 110}, {6, 128}, {80, 50},
    {118, 2}, {40, 12}};

static unsigned char *llm_fb;  /* where the frame is drawn */
static void llm_px(int x, int y, u32 c)
{
    unsigned char *q;
    if ((unsigned)x >= 160 || (unsigned)y >= 160) return;
    q = llm_fb + y * 240 + (x >> 1) * 3;
    if (x & 1) {
        q[1] = (q[1] & 0xF0) | c >> 8;
        q[2] = c;
    } else {
        q[0] = c;
        q[1] = (q[1] & 0x0F) | (c >> 4 & 0xF0);
    }
}
static void llm_present(void)
{
    if (llm_fb != LLM_LCD) memcpy(LLM_LCD, llm_fb, 160 * 240);
    show();
}
static void llm_rect(int x, int y, int w, int h, u32 c)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) llm_px(i, j, c);
}
/* menugen's rounded_rects: row insets r, 1, 1 ... at the top and bottom */
static void llm_round(int x, int y, int w, int h, int r, u32 c)
{
    int i;
    for (i = 0; i < r; i++) {
        int in = i ? 1 : r;
        llm_rect(x + in, y + i, w - 2 * in, 1, c);
        llm_rect(x + in, y + h - 1 - i, w - 2 * in, 1, c);
    }
    llm_rect(x, y + r, w, h - 2 * r, c);
}
static void llm_art(const char *const *art, int x, int y, int sc, char ch, u32 c)
{
    int r, i;
    for (r = 0; art[r]; r++)
        for (i = 0; art[r][i]; i++)
            if (art[r][i] == ch) llm_rect(x + i * sc, y + r * sc, sc, sc, c);
}
/* text from its top (the font's row 0; the baseline is 7 rows down), clipped to x0..x1 */
static void llm_text(int x, int y, const char *s, u32 c, int sc, int x0, int x1)
{
    for (; *s && x < x1; s++, x += 6 * sc) {
        int ch = (unsigned char)*s, r, i, d, e;
        if (ch < 32 || ch > 126) ch = '?';
        if (x + 5 * sc <= x0) continue;
        for (r = 0; r < 9; r++)
            for (i = 0; i < 5; i++)
                if (font[ch - 32][r] >> (4 - i) & 1)
                    for (d = 0; d < sc; d++)
                        for (e = 0; e < sc; e++) {
                            int px = x + i * sc + e;
                            if (px >= x0 && px < x1) llm_px(px, y + r * sc + d, c);
                        }
    }
}
static void llm_shadowed(int x, int y, const char *s, u32 c, u32 sh, int sc, int x0, int x1)
{
    llm_text(x + sc, y + sc, s, sh, sc, x0, x1);
    llm_text(x, y, s, c, sc, x0, x1);
}
static u32 llm_mix(u32 a, u32 b, int i, int n)
{
    u32 out = 0;
    int s;
    for (s = 0; s < 12; s += 4) {
        int p = a >> s & 15, q = b >> s & 15;
        out |= (u32)(p + (q - p) * i / n) << s;
    }
    return out;
}
static void llm_sky(const struct llm_theme *t)
{
    int i;
    for (i = 0; i < 10; i++) {  /* bands of 16 lines: one line built, then copied */
        u32 c = llm_mix(t->sky0, t->sky1, i, 9), j;
        unsigned char *q = llm_fb + i * 16 * 240;
        for (j = 0; j < 240; j += 3) q[j] = c, q[j + 1] = (c >> 8) * 0x11, q[j + 2] = c;
        for (j = 1; j < 16; j++) memcpy(q + j * 240, q, 240);
    }
    for (i = 0; i < 12; i++) {
        int x = llm_stars[i][0], y = llm_stars[i][1];
        llm_px(x, y, 0xFEF);
        if (i < 4) {
            llm_rect(x - 1, y + 1, 3, 1, 0xFEF);
            llm_px(x, y + 2, 0xFEF);
        }
    }
}
static void llm_header(const struct llm_theme *t, const char *title, const char *line)
{
    llm_shadowed(4, 8, title, t->head, t->head_shadow, 2, 0, 122);
    llm_art(llm_frog, 124, 4, 2, 'G', t->frog);
    llm_art(llm_frog, 124, 4, 2, 'd', t->frog_dark);
    llm_art(llm_frog, 124, 4, 2, 'w', 0xFFF);
    if (line) llm_shadowed(6, 29, line, 0xFFF, t->head_shadow, 1, 0, 160);
}
static void llm_footer(const struct llm_theme *t, const char *left, const char *right)
{
    llm_rect(0, LLM_FOOTER, 160, 160 - LLM_FOOTER, t->foot_bg);
    if (left) llm_text(4, LLM_FOOTER + 1, left, t->foot_text, 1, 0, 160);
    if (right) llm_text(156 - 6 * (int)strlen(right), LLM_FOOTER + 1, right, t->foot_text, 1, 0, 160);
}
static void llm_pill(const struct llm_theme *t, int x, int y, int w, int h)
{
    llm_round(x + 1, y + 2, w, h, 3, t->shadow);
    llm_round(x, y, w, h, 3, t->rim);
    llm_round(x + 1, y + 1, w - 2, h - 2, 2, t->fill);
    llm_round(x + 4, y + 2, w - 8, 5, 2, t->shine);
}
static void llm_tool(const struct llm_theme *t, int y, const char *const *icon)
{
    int h = 0;
    while (icon[h]) h++;
    llm_round(LLM_TOOL_X + 1, y + 2, LLM_TOOL_W, LLM_TOOL_W, 3, t->shadow);
    llm_round(LLM_TOOL_X, y, LLM_TOOL_W, LLM_TOOL_W, 3, t->rim);
    llm_round(LLM_TOOL_X + 1, y + 1, LLM_TOOL_W - 2, LLM_TOOL_W - 2, 2, t->fill);
    llm_art(icon, LLM_TOOL_X + 4, y + (LLM_TOOL_W - h) / 2 + 1, 1, '#', t->ink);
    llm_art(icon, LLM_TOOL_X + 3, y + (LLM_TOOL_W - h) / 2, 1, '#', 0xFFF);
}

/* A screen with a few lines of text (no games, a load error). */
static void llm_message(const struct llm_theme *t, const char *a, const char *b, const char *c, const char *d)
{
    const char *l[4] = {a, b, c, d};
    int i;
    llm_fb = LLM_LCD;  /* drawn once */
    llm_sky(t);
    llm_header(t, t->title, 0);
    for (i = 0; i < 4; i++)
        if (l[i]) llm_shadowed(6, 50 + i * 14, l[i], 0xFFF, t->head_shadow, 1, 0, 160);
    llm_footer(t, 0, t->foot);
    llm_present();
}

static void llm_wait(unsigned long long *due)
{
    do tick(); while (now < *due);
    *due = now > *due + LLM_FRAME ? now : *due + LLM_FRAME;
}
/* new presses, with repeats while held (as wait_keys) */
static u32 llm_keys(u32 *held, unsigned long long *again)
{
    u32 k = keys(), e = k & ~*held;
    if (e) *again = now + 16000000 / 3;
    else if (k && k == *held && now >= *again) {
        e = k;
        *again = now + 16000000 / 12;
    }
    *held = k;
    if (e) b_awake();
    return e;
}

/* The touch screen, from the BIOS touch driver's data (BIOS 0.6: 0x3C00_D24C), found by
   its thresholds (+0x98: pen-up level 0x7F0 twice, jitter limits 0x40 and 0x29): the
   pressure reading at +0xC8 (0x7FF with the pen up), the position in screen pixels at
   +0xD0/+0xD4. VERIFIED-EMU (Ribbit, BIOS 0.6); other BIOSes: found the same way or no touch. */
static volatile u32 *llm_tp;  /* the thresholds (+0x98), 0: not found */
static int llm_tp_looked, llm_pen_down, llm_pen_x, llm_pen_y;
/* A tap (the pen lifted; where it last was) since the last call. */
static int llm_tap(int *x, int *y)
{
    int down;
    if (!llm_tp_looked) {
        u32 *p;
        llm_tp_looked = 1;
        for (p = (u32 *)0x3C000000; p < (u32 *)0x3C080000; p++)
            if (p[0] == 0x07F007F0 && p[1] == 0x00400040 && p[2] == 0x00290029) {
                llm_tp = p;
                break;
            }
    }
    if (!llm_tp) return 0;
    down = llm_tp[12] < 0x7F0 && llm_tp[14] < 160 && llm_tp[15] < 160;
    if (down) {
        llm_pen_x = llm_tp[14], llm_pen_y = llm_tp[15];
        if (!llm_pen_down) b_awake();
    }
    if (llm_pen_down && !down) {
        llm_pen_down = 0;
        *x = llm_pen_x, *y = llm_pen_y;
        return 1;
    }
    llm_pen_down = down;
    return 0;
}
static int llm_in(int x, int y, int x0, int y0, int w, int h)
{
    return x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
}

/* The BIOS's own menu sounds, played by the BIOS exactly as LeapLoader's menu plays them
   (LF_PlayAudioRaw(lfh(n)), research/menugen.py): 29 = MenuUp (a move back, a key),
   27 = MenuDown (a move on), 28 = MenuSelect (choose, OK). LF_PlayAudioRaw (BIOS 0.6 native
   0x400A_32A4, helper 0x400A_23D8) calls the audio service 0x1005, slot 0x78, with (sound,
   volume, 0, 0); the volume is the raw volume (100 unless LF_SetRawVolume changed it), and
   lfh(n) is the number n. VERIFIED-FW (BIOS 0.6); the service table makes it any BIOS's. */
#define LLM_SND_UP 29
#define LLM_SND_DOWN 27
#define LLM_SND_SELECT 28
#define SVC_AUDIO 0x40  /* 0x1005 */
static void llm_sound(int n)
{
    ((u32 (*)(u32, u32, u32, u32))svc(SVC_AUDIO, 0x78))((u32)n, 100, 0, 0);
}
#define LLM_SND_KEY() llm_sound(LLM_SND_UP)
#define LLM_SND_DEL() llm_sound(LLM_SND_UP)
#define LLM_SND_OK() llm_sound(LLM_SND_SELECT)

static int llm_has(const char *name, const char *q)
{
    int i, j;
    if (!*q) return 1;
    for (i = 0; name[i]; i++) {
        for (j = 0; q[j]; j++) {
            int a = name[i + j];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (a != q[j]) break;
        }
        if (!q[j]) return 1;
    }
    return 0;
}
static void llm_name(char *out, const char *in)  /* without the extension */
{
    char *e;
    strncpy(out, in, 63);
    out[63] = 0;
    if ((e = strrchr(out, '.'))) *e = 0;
}

/* HINT + A + B held in a game: LeapLoader's in-game prompt (research/ingame.py: panel,
   "Back to the" + the menu's name, "Keep holding HINT + A + B", a bar filling over 2.5 s),
   drawn over the game straight into the LCD; `held` of `need` frames so far. The game's
   picture stays still meanwhile (the consoles stop drawing it), so the panel is drawn once
   and then only the bar grows (redrawn whole each frame, it flickered as the LCD scanned). */
static void llm_back_prompt(int held, int need)
{
    int w = 108 * (held > need ? need : held) / need;
    llm_fb = LLM_LCD;
    if (held > 1) {
        if (w) llm_rect(26, 104, w, 8, 0x3B4);
        return;
    }
    llm_round(12, 34, 136, 88, 4, 0xFD1);   /* edge 0xFFD21F */
    llm_round(15, 37, 130, 82, 3, 0x316);   /* panel 0x3A0F6B */
    llm_text(22, 43, "Back to the", 0xFFF, 1, 0, 160);
    llm_text(20, 56, "game list", 0xFD1, 2, 0, 160);
    llm_text(22, 78, "Keep holding", 0xFFF, 1, 0, 160);
    llm_text(22, 89, "HINT + A + B", 0xFD1, 1, 0, 160);
    llm_round(24, 102, 112, 12, 2, 0x103);  /* track 0x1B0533 */
    if (w) llm_rect(26, 104, w, 8, 0x3B4);  /* bar 0x2FB548 */
}

/* The search keyboard (menugen's: 7 x 4 letters, then delete, clear and OK). */
#define LLM_KEYS "ABCDEFGHIJKLMNOPQRSTUVWXYZ. "
static void llm_keyboard(const struct llm_theme *t, char *q)
{
    char old[LLM_QMAX + 1];
    int r = 0, c = 0;
    u32 held = keys(), e;
    unsigned long long again = now + 16000000 / 3, due = now;  /* a button still held is no new press */
    strcpy(old, q);
    for (;;) {
        int i, n = strlen(q);
        e = llm_keys(&held, &again);
        {
            int tx, ty, k;
            if (llm_tap(&tx, &ty))
                for (k = 0; k < 31; k++) {
                    int kr = k < 28 ? k / 7 : 4, kc = k < 28 ? k % 7 : k - 28;
                    int x = k < 28 ? 3 + kc * 22 : (kc == 0 ? 3 : kc == 1 ? 60 : 117), y = 30 + kr * 22;
                    if (llm_in(tx, ty, x - 1, y - 1, (k < 28 ? 20 : 40) + 2, 22)) {
                        r = kr, c = kc, e = K_A;
                        break;
                    }
                }
        }
        if (e & K_UP) {
            if (r == 4) c = c == 0 ? 0 : c == 1 ? 3 : 6;
            else if (r == 0) c = c < 2 ? 0 : c < 5 ? 1 : 2;
            r = r ? r - 1 : 4;
        }
        else if (e & K_DOWN) {
            if (r == 3) c = c < 2 ? 0 : c < 5 ? 1 : 2;
            else if (r == 4) c = c == 0 ? 0 : c == 1 ? 3 : 6;
            r = r < 4 ? r + 1 : 0;
        } else if (e & K_LEFT) c = c ? c - 1 : (r == 4 ? 2 : 6);
        else if (e & K_RIGHT) c = c < (r == 4 ? 2 : 6) ? c + 1 : 0;
        else if (e & K_B) {
            if (n) q[n - 1] = 0, LLM_SND_DEL();
        } else if (e & K_HOME) {
            strcpy(q, old);
            LLM_SND_DEL();
            break;
        } else if (e & K_PAUSE) {
            LLM_SND_OK();
            break;
        } else if (e & K_A) {
            if (r < 4) {
                if (n < LLM_QMAX && !(n == 0 && r * 7 + c == 27)) q[n] = LLM_KEYS[r * 7 + c], q[n + 1] = 0, LLM_SND_KEY();
            } else if (c == 0) {
                if (n) q[n - 1] = 0, LLM_SND_DEL();
            } else if (c == 1) {
                if (n) q[0] = 0, LLM_SND_DEL();
            } else {
                LLM_SND_OK();
                break;
            }
        }
        llm_sky(t);
        llm_round(3, 4, 154, 22, 3, t->head_shadow);
        llm_round(4, 5, 152, 20, 2, 0xFFF);
        if (*q) llm_text(7, 7, q, 0x306, 2, 0, 156);
        else llm_text(8, 11, "Type a name", 0x98B, 1, 0, 156);
        for (i = 0; i < 31; i++) {
            int kr = i < 28 ? i / 7 : 4, kc = i < 28 ? i % 7 : i - 28;
            int x = i < 28 ? 3 + kc * 22 : (kc == 0 ? 3 : kc == 1 ? 60 : 117), y = 30 + kr * 22;
            int w = i < 28 ? 20 : 40;
            static const char *const wide[] = {"DEL", "CLR", "OK"};
            char s[2] = {(char)(i < 28 ? LLM_KEYS[i] : 0), 0};
            if (s[0] == ' ') s[0] = '_';
            if (kr == r && kc == c) llm_round(x - 2, y - 2, w + 4, 24, 3, LLM_GLOW);
            llm_round(x, y, w, 20, 3, t->head_shadow);
            llm_round(x + 1, y + 1, w - 2, 18, 2, 0xFFD);
            if (i < 28) llm_text(x + 5, y + 3, s, 0x306, 2, 0, 160);
            else llm_text(x + (w - 12 * (int)strlen(wide[kc])) / 2 + 1, y + 3, wide[kc], 0x306, 2, 0, 160);
        }
        llm_footer(t, "B: delete  PAUSE: search", 0);
        llm_present();
        llm_wait(&due);
    }
    while (keys()) tick();
}

/* The list: returns the index (into names) of the ROM to start. */
static int llm_pick(const struct llm_theme *t, const char *names, int stride, int n, int sel)
{
    static short shown[LLM_MAX];
    static char q[LLM_QMAX + 1];
    char nm[64], line[32];
    int m = 0, at = 0, i, scroll = 0, rest = 30, launch = 0, rebuild = 1;
    u32 held = keys(), e;
    unsigned long long again = now + 16000000 / 3, due = now;  /* a button still held is no new press */
    if (n > LLM_MAX) n = LLM_MAX;
    if (!(llm_fb = (unsigned char *)malloc(160 * 240))) llm_fb = LLM_LCD;
    for (;;) {
        if (rebuild) {  /* the shown list; the choice stays if it is still in it */
            int want = m ? shown[at] : sel;
            m = at = rebuild = 0;
            for (i = 0; i < n; i++)
                if (llm_has(names + i * stride, q)) {
                    if (i == want) at = m;
                    shown[m++] = i;
                }
            scroll = 0, rest = 30;
        }
        e = llm_keys(&held, &again);
        {
            /* touch: a game starts it, the magnifying glass searches, the arrows page */
            int tx, ty;
            if (llm_tap(&tx, &ty) && !launch) {
                if (llm_in(tx, ty, LLM_TOOL_X - 1, LLM_TOP - 1, LLM_TOOL_W + 2, LLM_TOOL_W + 2)) e = K_PAUSE;
                else if (m > LLM_ROWS && llm_in(tx, ty, LLM_TOOL_X - 1, LLM_TOP + 2 * LLM_PITCH - 1, LLM_TOOL_W + 2, LLM_TOOL_W + 2))
                    e = K_LEFT;
                else if (m > LLM_ROWS && llm_in(tx, ty, LLM_TOOL_X - 1, LLM_TOP + 3 * LLM_PITCH - 1, LLM_TOOL_W + 2, LLM_TOOL_W + 2))
                    e = K_RIGHT;
                else
                    for (i = 0; i < LLM_ROWS; i++) {
                        int k = at / LLM_ROWS * LLM_ROWS + i;
                        if (k < m && llm_in(tx, ty, LLM_X - 2, LLM_TOP + i * LLM_PITCH - 2, LLM_W + 4, LLM_H + 4)) {
                            at = k, e = K_A;
                            break;
                        }
                    }
            }
        }
        if (launch) e = 0;
        if (e & (K_UP | K_DOWN | K_LEFT | K_RIGHT)) {
            int old = at;
            llm_sound(e & (K_UP | K_LEFT) ? LLM_SND_UP : LLM_SND_DOWN);
            if (m && (e & K_UP)) at = at ? at - 1 : m - 1;
            else if (m && (e & K_DOWN)) at = at + 1 < m ? at + 1 : 0;
            else if (m && (e & K_LEFT)) at = at >= LLM_ROWS ? (at / LLM_ROWS - 1) * LLM_ROWS : 0;
            else if (m && (e & K_RIGHT))
                at = (at / LLM_ROWS + 1) * LLM_ROWS < m ? (at / LLM_ROWS + 1) * LLM_ROWS : m - 1;
            if (at != old) scroll = 0, rest = 30;
        } else if (e & K_HOME) to_menu();
        else if (e & K_PAUSE) {
            llm_sound(LLM_SND_SELECT);
            llm_keyboard(t, q);
            held = keys();
            rebuild = 1;
            continue;
        } else if ((e & K_B) && *q) {
            q[0] = 0;
            rebuild = 1;
            continue;
        } else if ((e & K_A) && m) {
            launch = 1;
            llm_sound(LLM_SND_SELECT);
        }
        if (launch && ++launch > 9) {  /* the glow blinks, then the game starts */
            while (keys()) tick();
            if (llm_fb != LLM_LCD) free(llm_fb);
            return shown[at];
        }

        llm_sky(t);
        if (*q) snprintf(line, sizeof line, "Search: %s", q);
        else snprintf(line, sizeof line, "My Games: %d", n);
        llm_header(t, t->title, line);
        if (!m) {
            llm_shadowed(6, 62, "No games found.", 0xFFF, t->head_shadow, 1, 0, 160);
            llm_shadowed(6, 76, "PAUSE: search again", 0xFFF, t->head_shadow, 1, 0, 160);
            llm_shadowed(6, 90, "B: show them all", 0xFFF, t->head_shadow, 1, 0, 160);
        }
        for (i = 0; i < LLM_ROWS; i++) {
            int k = at / LLM_ROWS * LLM_ROWS + i, y = LLM_TOP + i * LLM_PITCH, w;
            if (k >= m) break;
            llm_name(nm, names + shown[k] * stride);
            w = 6 * (int)strlen(nm) - 1;
            if (k == at && !(launch & 2)) llm_round(LLM_X - 3, y - 3, LLM_W + 6, LLM_H + 7, 4, LLM_GLOW);
            llm_pill(t, LLM_X, y, LLM_W, LLM_H);
            llm_shadowed(LLM_X + LLM_TEXT - (k == at ? scroll : 0), y + 7, nm, 0xFFF, t->ink, 1,
                         LLM_X + LLM_TEXT, LLM_X + LLM_W - LLM_TEXT);
            if (k == at && w > LLM_W - 2 * LLM_TEXT) {  /* marquee, as the menu: rest, scroll, rest, back */
                int over = w - (LLM_W - 2 * LLM_TEXT);
                if (rest) rest--;
                else if (scroll >= over) scroll = 0, rest = 30;
                else if ((scroll += 2) >= over) scroll = over, rest = 30;
            }
        }
        llm_tool(t, LLM_TOP, llm_search_icon);
        if (m > LLM_ROWS) {
            llm_tool(t, LLM_TOP + 2 * LLM_PITCH, llm_up_icon);
            llm_tool(t, LLM_TOP + 3 * LLM_PITCH, llm_down_icon);
        }
        snprintf(line, sizeof line, "Page %d/%d", m ? at / LLM_ROWS + 1 : 0, (m + LLM_ROWS - 1) / LLM_ROWS);
        llm_footer(t, line, t->foot);
        llm_present();
        llm_wait(&due);
    }
}
