// Doom on the FH1: PrBoom's platform (video, sound, input, time) on the SDK. GPL-2.0-or-later, as PrBoom.
//
// The game runs inside the full screen's frame(): first PrBoom's set-up (D_DoomInit), then one turn of its loop a
// frame (D_DoomStep), which draws a 320 x 240 picture in 256 colours that I_FinishUpdate turns into the screen's.
// The WAD: a file or http://host:port/name (wadsrc.c), named in the app's data folder (wad.txt) or by the dev command
// "wad <file or URL>"; until there is one, the screen says how to give it. Exits (the menu's Quit) and fatal errors
// leave through a long jump back to frame().
#include "fh1_sdk.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include "doomtype.h"
#include "doomstat.h"
#include "doomdef.h"
#include "d_main.h"
#include "d_event.h"
#include "g_game.h"
#include "i_system.h"
#include "i_video.h"
#include "i_sound.h"
#include "i_main.h"
#include "m_argv.h"
#include "m_misc.h"
#include "r_draw.h"
#include "r_main.h"
#include "r_fps.h"
#include "s_sound.h"
#include "sounds.h"
#include "st_stuff.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "lprintf.h"
#include "wadsrc.h"
#include "fh1_arena.h"
#include "doom_prof.h"
size_t Z_TagBytes(int tag, int *blocks);
void Z_TagSizes(int tag, int count[7], size_t bytes[7]);
int Z_TagBig(int tag, size_t min, unsigned *sizes, int n);

// What PrBoom expects of the platform
int snd_card = 1, mus_card = 0;          // sound effects; no music yet (OPL synthesis: later)
int snd_samplerate = 11025;
int current_palette = 0;

enum { ST_WAIT, ST_LOAD, ST_RUN, ST_FAIL, ST_QUIT };
static int s_state = ST_WAIT;
static jmp_buf s_jmp;                    // back to frame() from an exit or a fatal error
static char s_wad[200], s_msg[200];

static uint8_t *s_screen;                // PrBoom's picture, 256 colours: the firmware's (fh1_screen_open8)
static bool s_text_pal;                  // the palette is text_screen's, not the game's

// ── Log, errors, exit ──

void I_LogV(const char *fmt, va_list ap)
{
    char b[200];
    vsnprintf(b, sizeof(b), fmt, ap);
    size_t n = strlen(b);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    if (n) fh1_log("%s", b);
}

void I_Fatal(const char *msg)
{
    snprintf(s_msg, sizeof(s_msg), "%s", msg);
    fh1_log("doom: error: %s", msg);                         // (a timedemo's own end comes this way too: "Timed ...")
    s_state = ST_FAIL;
    longjmp(s_jmp, 1);
}

void I_SafeExit(int rc)
{
    s_state = ST_QUIT;
    longjmp(s_jmp, 1);
}

const char *I_DoomExeDir(void) { return fh1_app_data_dir(); }   // its config (prboom.cfg) and saves
const char *I_SigString(char *buf, size_t sz, int signum) { snprintf(buf, sz, "signal %d", signum); return buf; }

// ── Time ──

int I_GetTimeMS(void) { return (int)fh1_millis(); }
int I_GetTime(void) { return (int)((int64_t)fh1_millis() * TICRATE * realtic_clock_rate / 100000); }
void I_uSleep(unsigned long usecs) { usleep(usecs); }

// ── Video ──

void I_StartFrame(void) {}
void I_UpdateNoBlit(void) {}
bool I_StartDisplay(void) { return true; }
void I_EndDisplay(void) {}
void I_UpdateVideoMode(void) {}
void I_ShutdownGraphics(void) {}

void I_SetPalette(int pal)
{
    uint16_t *p = V_BuildPalette(pal, 16);
    fh1_screen_palette(p);                                   // the firmware shows the picture in these colours
    Z_Free(p);
    current_palette = pal;
    s_text_pal = false;
}

void I_InitGraphics(void)
{
    // 0: the picture (the firmware's); 1: the backdrop of a smaller view's border. (2 and 3 are the wipe's, made
    // and freed by f_wipe.c: sized here, 2 was allocated at the start too, and lost when a wipe made its own.)
    for (int i = 0; i < 2; i++) {
        screens[i].width = SCREENWIDTH;
        screens[i].height = SCREENHEIGHT;
        screens[i].byte_pitch = SCREENWIDTH;
    }
    screens[0].data = s_screen;
    screens[0].not_on_heap = true;
    screens[4].width = SCREENWIDTH;
    screens[4].height = ST_SCALED_HEIGHT + 1;
    screens[4].byte_pitch = SCREENWIDTH;
}

// The picture is already the firmware's: it shows it through the palette, centred (the Guition's screen is
// 320 x 320), when frame() returns.
void I_FinishUpdate(void) {}

// ── Sound: the effects on the SDK's mixer ──
// Played as the WAD has them (8-bit, at their own rate: fh1_sfx_play8), from the lump in the zone, which stays
// locked while it may still be playing and is then left to the zone's cache like any other: no copy of its own (as
// 16-bit copies they took 2.7 MB in Freedoom, kept to a budget they still went over).

typedef struct {
    uint16_t format, rate;
    uint32_t length;
    uint8_t samples[];
} doom_sfx_t;

static const doom_sfx_t *s_sfx[NUMSFX];                     // locked, or NULL
static uint32_t s_until[NUMSFX];                             // when it may have stopped playing (ms)
static int s_voice[NUMSFX];                                  // its last voice

void I_InitSound(void) {}

void I_ShutdownSound(void) { fh1_sfx_stop_all(); }

// A level starting: every sound stopped and its lump back to the cache (p_setup.c: the heap left in large runs)
void I_ReleaseSounds(void)
{
    fh1_sfx_stop_all();
    for (int i = 1; i < NUMSFX; i++)
        if (s_sfx[i]) {
            W_UnlockLumpNum(S_sfx[i].lumpnum);
            s_sfx[i] = NULL;
        }
}

// Every frame: the lumps of sounds done playing back to the zone's cache.
static void sfx_release(void)
{
    const uint32_t now = fh1_millis();
    for (int i = 1; i < NUMSFX; i++)
        if (s_sfx[i] && (int32_t)(now - s_until[i]) > 0 && !fh1_sfx_playing(s_voice[i])) {
            W_UnlockLumpNum(S_sfx[i].lumpnum);
            s_sfx[i] = NULL;
        }
}

int I_StartSound(int id, int channel, int vol, int sep, int pitch, int priority)
{
    if (id <= 0 || id >= NUMSFX || S_sfx[id].lumpnum < 0) return -1;
    if (!s_sfx[id]) {
        const doom_sfx_t *d = W_CacheLumpNum(S_sfx[id].lumpnum);
        if (!d || d->format != 3 || d->length <= 32 || W_LumpLength(S_sfx[id].lumpnum) < 8 + (int)d->length) {
            W_UnlockLumpNum(S_sfx[id].lumpnum);
            return -1;
        }
        s_sfx[id] = d;
    }
    const doom_sfx_t *d = s_sfx[id];
    const uint32_t frames = d->length - 32, rate = d->rate ? d->rate : 11025;   // 16 bytes of padding at either end
    int v = vol * 2 * snd_SfxVolume / 15;
    if (v > 255) v = 255;
    const uint32_t until = fh1_millis() + (uint32_t)((uint64_t)frames * 1000 / rate) + 200;
    if ((int32_t)(until - s_until[id]) > 0) s_until[id] = until;
    s_voice[id] = fh1_sfx_play8(d->samples + 16, frames, rate, (uint8_t)v);
    return s_voice[id];
}

void I_StopSound(int handle) { if (handle >= 0) fh1_sfx_stop(handle); }
bool I_SoundIsPlaying(int handle) { return handle >= 0 && fh1_sfx_playing(handle); }
bool I_AnySoundStillPlaying(void) { return false; }
void I_UpdateSoundParams(int handle, int volume, int seperation, int pitch) {}

// Music: none yet.
void I_PlaySong(int handle, int looping) {}
void I_PauseSong(int handle) {}
void I_ResumeSong(int handle) {}
void I_StopSong(int handle) {}
void I_UnRegisterSong(int handle) {}
int I_RegisterSong(const void *data, size_t len) { return 0; }
void I_SetMusicVolume(int volume) {}

// ── Input: the FH1's keys and player 1's controller, as Doom's keys ──
// D-pad: walk and turn; A: fire (and pick in menus); B: use, open (and back in menus); X: run; Y: next weapon;
// L1 / R1: strafe; Start: Doom's menu; Select: the map. The left stick walks and turns as the D-pad does.

static uint32_t s_prev;

void I_StartTic(void)
{
    uint32_t down = 0;
    for (int k = 0; k <= FH1_KEY_SELECT; k++)
        if (fh1_input_is_down(k)) down |= 1u << k;
    fh1_pad_t pad;
    if (fh1_pad(0, &pad)) {
        if (pad.buttons & FH1_PAD_BTN_L1) down |= 1u << 16;
        if (pad.buttons & FH1_PAD_BTN_R1) down |= 1u << 17;
        if (pad.ly < -200) down |= 1u << FH1_KEY_UP;
        if (pad.ly > 200) down |= 1u << FH1_KEY_DOWN;
        if (pad.lx < -200) down |= 1u << FH1_KEY_LEFT;
        if (pad.lx > 200) down |= 1u << FH1_KEY_RIGHT;
    }
    const struct { uint32_t bit; int *key; } map[] = {
        { 1u << FH1_KEY_UP, &key_up }, { 1u << FH1_KEY_DOWN, &key_down }, { 1u << FH1_KEY_LEFT, &key_left },
        { 1u << FH1_KEY_RIGHT, &key_right }, { 1u << FH1_KEY_A, &key_fire }, { 1u << FH1_KEY_A, &key_enter },
        { 1u << FH1_KEY_B, &key_use }, { 1u << FH1_KEY_B, &key_backspace }, { 1u << FH1_KEY_X, &key_speed },
        { 1u << FH1_KEY_Y, &key_weapontoggle }, { 1u << FH1_KEY_START, &key_escape },
        { 1u << FH1_KEY_SELECT, &key_map }, { 1u << 16, &key_strafeleft }, { 1u << 17, &key_straferight },
    };
    const uint32_t changed = down ^ s_prev;
    s_prev = down;
    if (!changed) return;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (changed & map[i].bit) {
            event_t ev = { .type = (down & map[i].bit) ? ev_keydown : ev_keyup, .data1 = *map[i].key };
            D_PostEvent(&ev);
        }
}

void I_Init(void)
{
    snd_channels = 8;
    snd_samplerate = 11025;
    snd_SfxVolume = 15;
    snd_MusicVolume = 0;
}

// ── The app ──

static const char *s_argv[8];

static void save_wad(const char *w)
{
    char p[160];
    snprintf(p, sizeof(p), "%s/wad.txt", fh1_app_data_dir());
    FILE *f = fopen(p, "w");
    if (f) {
        fprintf(f, "%s\n", w);
        fclose(f);
    }
}

// The WAD: the one named in wad.txt (or by the dev command), else the first IWAD found in the wads folder of the
// SD card or of internal storage (the web console's Upload puts .wad files there).
static void load_wad(void)
{
    char p[160];
    snprintf(p, sizeof(p), "%s/wad.txt", fh1_app_data_dir());
    FILE *f = fopen(p, "r");
    if (f) {
        if (fgets(s_wad, sizeof(s_wad), f)) s_wad[strcspn(s_wad, "\r\n")] = 0;
        fclose(f);
        if (s_wad[0]) return;
    }
    static const char *const dirs[] = { "/sd/wads", "/int/wads" };
    static const char *const names[] = { "doom2.wad", "plutonia.wad", "tnt.wad", "doom.wad", "freedoom2.wad",
                                         "freedoom1.wad", "doom1.wad" };
    for (int d = 0; d < 2; d++)
        for (int n = 0; n < (int)(sizeof(names) / sizeof(names[0])); n++) {
            snprintf(p, sizeof(p), "%s/%s", dirs[d], names[n]);
            if (access(p, R_OK) == 0) {
                snprintf(s_wad, sizeof(s_wad), "%s", p);
                return;
            }
        }
}

// A line of text in colour c of text_screen's palette: drawn by the SDK into a line of RGB565, then copied as c.
static void text_line(uint8_t *px, int w, int h, int y, const char *text, uint8_t c, int scale)
{
    static uint16_t line[320 * 16];
    const int lh = 8 * scale;
    if (w > 320 || y < 0 || y + lh > h) return;
    memset(line, 0, sizeof(line));
    fh1_px_text(line, w, lh, 16, 0, text, 0xffffff, scale);
    for (int i = 0; i < w * lh; i++)
        if (line[i]) px[y * w + i] = c;
}

static void text_screen(uint8_t *px, int w, int h, const char *title, const char *a, const char *b)
{
    if (!s_text_pal) {
        static uint16_t pal[256];                            // black, red, light grey, grey
        pal[1] = 0xd186, pal[2] = 0xe71c, pal[3] = 0xa514;
        fh1_screen_palette(pal);
        s_text_pal = true;
    }
    memset(px, 0, (size_t)w * h);
    text_line(px, w, h, h / 2 - 30, title, 1, 2);
    if (a) text_line(px, w, h, h / 2 + 4, a, 2, 1);
    if (b) text_line(px, w, h, h / 2 + 18, b, 3, 1);
}

// The dev command "fps": every 5 s, the pictures drawn per second and the time each one took (the game's tics
// and its drawing).
static bool s_fps_log;
static uint32_t s_fps_t0, s_fps_n, s_fps_ms, s_fps_max;
uint32_t doom_prof[PROF_N];
static uint32_t s_gap, s_frame_end;                          // cycles between frames: the firmware's

static unsigned held_kb(void);

static void fps_count(uint32_t ms)
{
    s_fps_n++, s_fps_ms += ms;
    if (ms > s_fps_max) s_fps_max = ms;
    const uint32_t now = fh1_millis();
    if (!s_fps_t0) s_fps_t0 = now;
    if (now - s_fps_t0 < 5000) return;
    if (s_fps_log) {
        fh1_log("doom: %u.%u fps, %u ms a frame (at most %u), %u KB of memory", (unsigned)(s_fps_n * 10000 / (now - s_fps_t0) / 10),
                (unsigned)(s_fps_n * 10000 / (now - s_fps_t0) % 10), (unsigned)(s_fps_ms / s_fps_n), (unsigned)s_fps_max,
                held_kb());
        // Each phase in tenths of a ms per frame drawn (240 cycles a microsecond)
#define T(c) (unsigned)((c) / s_fps_n / 24000)
        fh1_log("doom: ms/10 a frame: step %u = tics %u + display %u (view %u = walls %u, planes %u, sprites %u; hud %u); "
                "between frames %u", T(doom_prof[PROF_STEP]), T(doom_prof[PROF_TICS]), T(doom_prof[PROF_DISPLAY]),
                T(doom_prof[PROF_VIEW]), T(doom_prof[PROF_WALLS]), T(doom_prof[PROF_PLANES]), T(doom_prof[PROF_SPRITES]),
                T(doom_prof[PROF_HUD]), T(s_gap));
        fh1_log("doom: ms/10 a frame: segs %u (columns %u), sprites projected %u, plane spans %u", T(doom_prof[PROF_SEGS]),
                T(doom_prof[PROF_COLS]), T(doom_prof[PROF_ADDSPR]), T(doom_prof[PROF_SPANS]));
        fh1_log("doom: ms/10 a frame: sprites: drawn %u, masked walls %u, weapon %u (the rest: sorting, clipping)",
                T(doom_prof[PROF_SPRDRAW]), T(doom_prof[PROF_MASKSEG]), T(doom_prof[PROF_PSPR]));
        fh1_log("doom: ms/10 a frame: textures built %u, patches built %u, WAD reads %u", T(doom_prof[PROF_COMPOSITE]),
                T(doom_prof[PROF_PATCH]), T(doom_prof[PROF_READ]));
#undef T
    }
    for (int i = 0; i < PROF_N; i++) doom_prof[i] = 0;
    s_gap = 0;
    s_fps_t0 = now, s_fps_n = s_fps_ms = s_fps_max = 0;
}

// The firmware's two pictures take turns (fh1_screen_open8), and frame() says each one is drawn whole
// (FH1_FRAME_SHOW_REDRAW): the 3D view is, but in a level the status bar is only touched where it changed, and a
// smaller view's border not at all. So those rows come from the picture drawn before: 10 KB, not the 77 KB the
// firmware copies otherwise (3 ms with the other core reading it too).
static uint8_t *s_last_drawn;

static void keep_level_rows(uint8_t *px)
{
    if (!s_last_drawn || s_last_drawn == px || gamestate != GS_LEVEL) return;
    if (viewwidth != SCREENWIDTH) {
        memcpy(px, s_last_drawn, SCREENWIDTH * SCREENHEIGHT);
        return;
    }
    const int top = viewwindowy * SCREENWIDTH, bottom = (viewwindowy + viewheight) * SCREENWIDTH;
    memcpy(px, s_last_drawn, top);
    memcpy(px + bottom, s_last_drawn + bottom, SCREENWIDTH * SCREENHEIGHT - bottom);
}

// What Doom holds: the zone's blocks in use, and its blocks outside the zone (KB)
static unsigned held_kb(void)
{
    return (unsigned)((arena_used() + doom_mem_in_use()) / 1024);
}

// "timedemo DEMO TICS": the first TICS of the demo timed (from its first tic in the level)
static unsigned s_td_limit, s_td_g0;
static uint32_t s_td_t0;

static void timedemo_check(void)
{
    extern boolean singletics, timingdemo;
    if (!s_td_limit || !timingdemo || gamestate != GS_LEVEL) return;
    if (!s_td_t0) {
        s_td_t0 = fh1_millis();
        s_td_g0 = gametic;
        return;
    }
    const unsigned tics = gametic - s_td_g0;
    if (tics < s_td_limit) return;
    const uint32_t ms = fh1_millis() - s_td_t0;
    fh1_log("doom: timedemo: %u tics in %u ms = %u.%u fps", tics, (unsigned)ms, (unsigned)(tics * 10000ull / ms / 10),
            (unsigned)(tics * 10000ull / ms % 10));
    singletics = timingdemo = false;                         // the rest at the normal pace
    s_td_limit = 0;
}

static int frame(uint8_t *px, int w, int h, uint32_t dt, void *user)
{
    s_screen = px;
    if (s_state == ST_RUN) R_SetScreen(px);
    char cmd[200];
    while (fh1_dev_command(cmd, sizeof(cmd))) {
        if (!strncmp(cmd, "wad ", 4) && s_state == ST_WAIT) {
            snprintf(s_wad, sizeof(s_wad), "%s", cmd + 4);
            save_wad(s_wad);
        } else if (!strcmp(cmd, "fps")) {
            s_fps_log = !s_fps_log;
            fh1_log("doom: fps log %s", s_fps_log ? "on" : "off");
        } else if (!strcmp(cmd, "mem")) {
            static const char *const tags[] = { "free", "static", "sound", "music", "level", "levspec", "cache" };
            for (int t = 1; t < PU_MAX; t++) {
                int blocks;
                const size_t n = Z_TagBytes(t, &blocks);
                fh1_log("doom: zone %s: %u KB in %d blocks", tags[t], (unsigned)(n / 1024), blocks);
                if (n > 64 * 1024) {
                    int c[7];
                    size_t b[7];
                    Z_TagSizes(t, c, b);
                    fh1_log("doom:   by size (<64 <256 <1K <4K <16K <64K more): %d/%uK %d/%uK %d/%uK %d/%uK %d/%uK %d/%uK %d/%uK",
                            c[0], (unsigned)(b[0] / 1024), c[1], (unsigned)(b[1] / 1024), c[2], (unsigned)(b[2] / 1024),
                            c[3], (unsigned)(b[3] / 1024), c[4], (unsigned)(b[4] / 1024), c[5], (unsigned)(b[5] / 1024),
                            c[6], (unsigned)(b[6] / 1024));
                    unsigned sz[24];
                    const int k = Z_TagBig(t, 16384, sz, 24);
                    char line[200];
                    int o = 0;
                    for (int i = 0; i < k && i < 24 && o < (int)sizeof(line) - 8; i++)
                        o += snprintf(line + o, sizeof(line) - o, " %u", sz[i] / 1024);
                    fh1_log("doom:   blocks of 16 KB or more (KB):%s", line);
                }
            }
            fh1_log("doom: tables' arena %u of %u KB; WAD blocks %u KB; all %u KB", (unsigned)(arena_used() / 1024),
                    (unsigned)(arena_size() / 1024), (unsigned)(wadsrc_cache_bytes() / 1024), held_kb());
        } else if (!strncmp(cmd, "timedemo", 8) && s_state == ST_RUN) {
            // As -timedemo: the demo a tic a frame, as fast as it draws; at its end the log says how fast
            // ("Timed N gametics in M realtics"). "timedemo demo2" for another; "timedemo demo1 2000": its first
            // 2000 tics only, then on at the normal pace.
            extern boolean singletics, timingdemo, singledemo;
            static char demo[16];                            // kept: the demo starts later, from its name
            snprintf(demo, sizeof(demo), "demo1");
            unsigned limit = 0;
            sscanf(cmd + 8, "%15s %u", demo, &limit);
            singletics = timingdemo = singledemo = true;
            s_td_limit = limit, s_td_t0 = 0;
            G_DeferedPlayDemo(demo);
            fh1_log("doom: timedemo %s %u", demo, limit);
        } else if (!strcmp(cmd, "net")) {
            unsigned r, kb, ms;
            wadsrc_stats(&r, &kb, &ms);
            fh1_log("doom: %u requests, %u KB, %u ms waiting", r, kb, ms);
        }
    }
    if (setjmp(s_jmp)) {                                     // an exit or a fatal error from inside the game
        return s_state == ST_QUIT ? FH1_FRAME_END : FH1_FRAME_SHOW;
    }
    switch (s_state) {
    case ST_WAIT:
        if (!s_wad[0]) {
            text_screen(px, w, h, "DOOM", "Falta o WAD: ponha um .wad na", "pasta wads do cartão SD (Upload).");
            return FH1_FRAME_SHOW;
        }
        text_screen(px, w, h, "DOOM", "Carregando...", s_wad);
        s_state = ST_LOAD;                                   // shown first; the loading next frame
        return FH1_FRAME_SHOW;
    case ST_LOAD:
        s_argv[0] = "doom";
        s_argv[1] = "-iwad";
        s_argv[2] = s_wad;
        s_argv[3] = NULL;
        myargv = s_argv;
        myargc = 3;
        SCREENWIDTH = 320;
        SCREENHEIGHT = 240;
        s_state = ST_RUN;
        Z_Init();
        D_DoomInit();
        {
            extern void R_FH1PreallocPlanes(void), R_FH1PreallocSprites(void), R_FH1PreallocSegs(void);
            R_FH1PreallocPlanes();
            R_FH1PreallocSprites();
            R_FH1PreallocSegs();
        }
        I_SetPalette(0);
        {
            unsigned r, kb, ms;
            wadsrc_stats(&r, &kb, &ms);
            fh1_log("doom: ready; the WAD: %u requests, %u KB, %u ms waiting; %u KB of memory", r, kb, ms, held_kb());
        }
        return FH1_FRAME_SHOW;
    case ST_RUN: {
        const uint32_t t0 = fh1_millis(), c0 = prof_now();
        if (s_frame_end) s_gap += c0 - s_frame_end;
        const uint32_t c_step = prof_now();
        sfx_release();
        keep_level_rows(px);
        const int drawn = D_DoomStep();
        doom_prof[PROF_STEP] += prof_now() - c_step;
        if (drawn) s_last_drawn = px;
        timedemo_check();
        if (drawn) fps_count(fh1_millis() - t0);
        s_frame_end = prof_now();
        // All of it drawn: the firmware need not copy it into the next picture
        return drawn == 1 ? FH1_FRAME_SHOW_REDRAW : drawn ? FH1_FRAME_SHOW : FH1_FRAME_SAME;
    }
    case ST_FAIL:
        text_screen(px, w, h, "DOOM", "Erro:", s_msg);
        return fh1_input_is_down(FH1_KEY_B) ? FH1_FRAME_END : FH1_FRAME_SHOW;
    default:
        return FH1_FRAME_END;
    }
}

static void closed(void *user)
{
    fh1_sfx_stop_all();
    wadsrc_close_all();
    fh1_log("doom: closed; %u KB of memory given back", (unsigned)((arena_size() + doom_mem_in_use()) / 1024));
    doom_mem_free_all();                                     // (PrBoom frees little of its own)
    arena_close();                                           // the tables' arena (fh1_arena.c)
    fh1_app_exit();
}

int app_main(int argc, char *argv[])
{
    load_wad();
    if (!fh1_screen_open8(10, 320, 240, frame, closed, NULL)) {
        doom_mem_free_all();
        fh1_ui_message("Doom", "Não foi possível usar a tela.");
    }
    return 0;
}
