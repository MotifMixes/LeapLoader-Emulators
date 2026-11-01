/* Volume, as LeapLoader's menu does it (research/menugen.py, from LeapFrog's own menus): 13 levels,
   a pink panel with 12 bars sliding up from the bottom, the BIOS's sound 39 per step (25 when already
   at the top). The apps play their sound themselves, so the level is a gain on their mix.

   The level is shared with LeapLoader's menu through LLV_FILE (its index 0-12 as text): read at start,
   written when the panel has gone. The console restarts between the menu and an app, and the BIOS
   comes back at its own level (user, 2026-11-01: the home menu on 4 bars, LeapLoader then on 6).
   The menu writes it when it starts a game and spoils it ("-") once it has read it back, so a level
   set in the home menu in between is not overruled by an old one.

   Included by a console after llmenu.h (svc, SVC_AUDIO), with u32, now/tick(), K_VOLUP/K_VOLDN,
   b_fopen/b_fread/b_fwrite/b_fclose and ll_cache_clean (llcache.h) defined. llmenu's list calls
   llv_menu() once a frame; a game calls llv_game(fill) where it draws. */
#define LLV_FILE "b:\\Leapster\\System\\SDMenu\\LeapLoaderVolume.txt"
#define LLV_LEVELS 13
#define LLV_START 6                       /* without a file: the level the BIOS comes back at (80) */
#define LLV_SOUND 39
#define LLV_TOP_SOUND 25
#define LLV_Y 102                         /* the panel's top when up */
#define LLV_HIDE 60                       /* how far it slides down to go */
#define LLV_SLIDE (16000000 / 7)          /* 150 ms in, 150 ms out (16 MHz counts) */
#define LLV_HOLD 16000000u                /* 1 s up after the last press */
#define LLV_BAR_X 15
#define LLV_BAR_Y 125
#define LLV_BAR_PITCH 11
#define LLV_BAR_W 8
#define LLV_BAR_H 25

/* gain /256 per level: (volume / 127)^2 of LeapLoader's 0, 16, 32 ... 127 */
static const unsigned short llv_gain[LLV_LEVELS] = {0, 4, 16, 36, 65, 82, 101, 122, 146, 171, 199, 228, 256};
static int llv_level = LLV_START, llv_dirty;
static unsigned long long llv_at;         /* the last press (0: the panel has never been up) */

static void llv_load(void)
{
    char s[8] = {0};
    void *f = b_fopen(LLV_FILE, "rb");
    int v = 0, i, n;
    if (!f) return;
    ll_cache_clean();
    n = b_fread(s, 1, sizeof s - 1, f);
    b_fclose(f);
    for (i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + s[i] - '0';
    if (i && v < LLV_LEVELS) llv_level = v;
}

static void llv_save(void)
{
    static char s[4];
    void *f;
    s[0] = llv_level >= 10 ? '1' : '0' + llv_level;
    s[1] = llv_level >= 10 ? '0' + llv_level - 10 : 0;
    s[2] = 0;
    ll_cache_clean();  /* (the file service reads s past the cache) */
    if ((f = b_fopen(LLV_FILE, "w"))) {
        b_fwrite(s, 1, s[1] ? 3 : 2, f);  /* with the 0: LeapLoader's SD_ReadText read past a bare "2" */
        b_fclose(f);
    }
}

/* the volume keys: one level per press, as the BIOS's menus */
static void llv_keys(u32 k)
{
    static u32 held;
    u32 v = k & (K_VOLUP | K_VOLDN), e = v & ~held;
    held = v;
    if (!e) return;
    if (e & K_VOLUP) {
        if (llv_level < LLV_LEVELS - 1) llv_level++, ((u32 (*)(u32, u32, u32, u32))svc(SVC_AUDIO, 0x78))(LLV_SOUND, 100, 0, 0);
        else ((u32 (*)(u32, u32, u32, u32))svc(SVC_AUDIO, 0x78))(LLV_TOP_SOUND, 100, 0, 0);
    } else {
        if (llv_level > 0) llv_level--;
        ((u32 (*)(u32, u32, u32, u32))svc(SVC_AUDIO, 0x78))(LLV_SOUND, 100, 0, 0);
    }
    tick();
    llv_at = now ? now : 1;
    llv_dirty = 1;
}

/* how far down the panel is now (0: all the way up), or LLV_HIDE once it has gone: then the
   level is written if it changed */
static int llv_offset(void)
{
    unsigned long long t;
    if (!llv_at) return LLV_HIDE;
    tick();
    t = now - llv_at;
    if (t < LLV_SLIDE) return (int)(LLV_HIDE - LLV_HIDE * t / LLV_SLIDE);
    if (t < LLV_SLIDE + LLV_HOLD) return 0;
    if (t < 2 * LLV_SLIDE + LLV_HOLD) return (int)(LLV_HIDE * (t - LLV_SLIDE - LLV_HOLD) / LLV_SLIDE);
    llv_at = 0;
    if (llv_dirty) llv_dirty = 0, llv_save();
    return LLV_HIDE;
}

/* the panel, LLV_HIDE pixels at most below its place, cut at the screen's bottom; rect(x, y, w, h, 0xRGB) */
static void llv_draw(int dy, void (*rect)(int, int, int, int, u32))
{
    int y = LLV_Y + dy, i, h = 160 - y;
    static const unsigned char inset[4] = {6, 3, 2, 1};  /* the rounded top corners */
    if (h <= 0) return;
    for (i = 0; i < 4 && i < h; i++) rect(6 + inset[i], y + i, 148 - 2 * inset[i], 1, 0xFFF);
    if (h > 4) rect(6, y + 4, 148, h - 4, 0xFFF);
    if (h > 1) rect(8, y + 1, 144, 1, 0xCCC);
    if (h > 4) rect(10, y + 4, 140, h - 4, 0xF7F);
    for (i = 0; i < 12; i++) {
        int x = LLV_BAR_X + i * LLV_BAR_PITCH, by = LLV_BAR_Y + dy;
        if (by >= 160) break;
        rect(x, by, LLV_BAR_W, by + LLV_BAR_H > 160 ? 160 - by : LLV_BAR_H, 0x436);
        if (by + 1 < 160)
            rect(x + 1, by + 1, LLV_BAR_W - 2, by + LLV_BAR_H - 1 > 160 ? 159 - by : LLV_BAR_H - 2, i < llv_level ? 0x0C5 : 0x000);
    }
}

/* once a frame in llmenu's list, before the frame is shown */
static void llv_menu(void)
{
    int dy;
    llv_keys(keys());
    if ((dy = llv_offset()) < LLV_HIDE) llv_draw(dy, llm_rect);
}

/* once a frame in a game, where it draws (the keys come with the game's own) */
static void llv_game(u32 k, int drawn, void (*rect)(int, int, int, int, u32))
{
    int dy;
    llv_keys(k);
    dy = llv_offset();
    if (drawn && dy < LLV_HIDE) llv_draw(dy, rect);
}
