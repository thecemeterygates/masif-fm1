/* MASIF's screens with the arrows' controls (MASIF_CONTROLS 0, the user's of 2026-10-21): the arrows, SHIFT, OPTION,
 * EDIT and PLAY, the lists while EDIT is held, the knobs' banks, the playhead, and where one left
 * off kept and back at power-on. The real screen code, song model, player and adapter, as host-tests/test_ui.c
 * (which works the knob layout of 2026-10-12, MASIF_CONTROLS 1).
 *   cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,signed-integer-overflow -fno-sanitize-recover=all \
 *      -Ifirmware/src -Ifirmware/gen -Ibuild/gen -o build/host/test_ui_msf host-tests/test_ui_msf.c && build/host/test_ui_msf build/host/ui */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static uint16_t fb[240 * 240];
static uint32_t blits;
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i, j;
    blits++;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) if (x + i < 240u && y + j < 240u) fb[(y + j) * 240u + x + i] = p[j * w + i];
}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t s = (uint16_t)((c >> 8) | (c << 8));
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) if (x + i < 240u && y + j < 240u) fb[(y + j) * 240u + x + i] = s;
}
#define __attribute__(x)
#define MASIF_ENABLE_TRACKER 1
#define MASIF_SONG_STORE 1
#define MASIF_CONTROLS 0
#define SQ_NSEG 1u                              /* (as the MASIF build has the drum sequencer: only the clock) */
#define SQ_MAXEV 8u
static uint32_t fm1_ms = 1000;
#include "gfx.c"
#include "sp_core.c"
#include "sp_seq.c"
#include "masif_tracker.c"
#include "masif_runtime.c"
#include "masif_songfile.c"
#include "masif_store.c"
#include "masif_demo.c"
#include "masif_fm1_adapter.c"
#define MU_WAVE_NAME(w) ((w) == 5u ? "BREAK1" : "------")
#include "fake_pool.h"                         /* (the pool kept by the FM-1: sp_samples.c, masif_rec.c) */
#include "masif_ui.c"

/* the song store's flash: RAM */
static uint8_t flash[0x100000];
int masif_fl_read(uint32_t off, uint8_t *dst, uint32_t n) { memcpy(dst, flash + off, n); return 0; }
int masif_fl_erase(uint32_t off) { memset(flash + off, 0xFF, 4096); return 0; }
int masif_fl_write(uint32_t off, const uint8_t *src, uint32_t n) { uint32_t k; for (k = 0; k < n; k++) flash[off + k] &= src[k]; return 0; }
static const masif_store_region_t pool[1] = {{0xDA000, 0xE0000}};

static const char *dir;
static void ppm(const char *name)
{
    char p[300];
    FILE *f;
    uint32_t i;
    masif_ui_draw();
    snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    f = fopen(p, "wb");
    if (!f) { perror(p); exit(2); }
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t v = (uint16_t)((fb[i] >> 8) | (fb[i] << 8));
        uint8_t rgb[3] = {(uint8_t)((v >> 11) * 255 / 31), (uint8_t)(((v >> 5) & 63) * 255 / 63), (uint8_t)((v & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static void shot(const char *name) { fm1_ms += 2000; ppm(name); }      /* (without the word at the bottom) */
static int bad;
static void check(int ok, const char *what) { printf("ui: %-100s %s\n", what, ok ? "ok" : "FAIL"); bad += !ok; }

/* the audio side's turn: one block (the edits the screens posted get into the song) */
static void audio(uint32_t blocks)
{
    int32_t o[2 * SP_BLK];
    while (blocks--) { memset(o, 0, sizeof o); sq_block(); masif_fm1_audio_block(); sp_render(o); }
}
static void turn(uint32_t enc, int32_t d) { masif_ui_knob(enc, d); audio(1); }
static void key(uint32_t k) { masif_ui_key(k, 1); audio(1); masif_ui_key(k, 0); audio(1); }
/* a pixel of the screen as one of the palette's shades */
static int px(uint32_t x, uint32_t y, uint32_t shade)
{
    uint16_t c = MU_PALETTE[mu.palette % MU_NPALETTE][shade];
    return fb[y * 240u + x] == (uint16_t)((c >> 8) | (c << 8));
}
/* a row of the screen as text */
static const char *line(uint32_t r)
{
    static char s[MU_COLS + 1];
    masif_ui_draw();
    memcpy(s, mu_ch[r], MU_COLS);
    s[MU_COLS] = 0;
    return s;
}
/* the buttons (MASIF_CONTROLS 0) */
static void tick(uint32_t ms);
static void down(uint32_t b) { masif_ui_button(b, 1); audio(1); if (b == MB_EDIT) tick(MU_COMBO_MS); }   /* (EDIT alone: its wait) */
static void edit_down_now(void) { masif_ui_button(MB_EDIT, 1); audio(1); }   /* (and OPTION / SHIFT within it) */
static int heard(void) { return sp_ch[SP_NCH + mu.track].on; }
static void hush(void) { masif_fm1_preview(mu.track, 0, 0, 0); audio(400); }
static void up(uint32_t b) { masif_ui_button(b, 0); audio(1); }
static void press(uint32_t b) { down(b); up(b); }
static void with(uint32_t held, uint32_t b) { down(held); press(b); up(held); }      /* held + b */
static void edit_tap(void) { press(MB_EDIT); fm1_ms += 1000; }        /* (a single press: no double tap after it) */
static uint32_t leds_btn(void) { uint32_t b, k, g; masif_ui_leds(&b, &k, &g); return b; }
static uint32_t leds_keys(void) { uint32_t b, k, g; masif_ui_leds(&b, &k, &g); return k; }
static uint32_t leds_green(void) { uint32_t b, k, g; masif_ui_leds(&b, &k, &g); return g; }
static void tick(uint32_t ms) { fm1_ms += ms; masif_ui_tick(); audio(1); }

int main(int argc, char **argv)
{
    static uint8_t wave[3000];
    static int16_t pcm[2000];
    static masif_song_t was;
    uint32_t i, r, c;
    dir = argc > 1 ? argv[1] : "build/host";
    memset(flash, 0xFF, sizeof flash);
    for (i = 0; i < 2000u; i++)
        pcm[i] = (int16_t)((((i * 37u) % 200u) < 100u ? 1 : -1) * (int32_t)(1800u - (i % 500u) * 3u) * 16);
    sp_pack(wave, pcm, 2000);
    sp_wave[5].d = wave; sp_wave[5].n = 2000; sp_wave[5].rate = 26040;
    sp_wave[6].d = wave; sp_wave[6].n = 1500; sp_wave[6].rate = 26040;
    sq_init();
    sq.bpm10 = 1600;
    masif_fm1_init();
    masif_song.instrument[0].sample = 5;            /* (two instruments with a sample: the first empty one is 01) */
    masif_song.instrument[5].sample = 5;
    masif_store_init(pool, 1);
    masif_ui_init();
    masif_ui_resume();
    audio(1);

    check(leds_btn() == (0x3FFFu & ~(1u << MK_PLAY) &
                         ~(1u << MK_OCTDN | 1u << MK_OCTUP | 1u << MK_FX | 1u << MK_ENV | 1u << MK_LFO | 1u << MK_SEQ)) &&
          leds_green() == 1u && !leds_keys(),
          "stopped: the white buttons in use lit, the printed PLAY (SHIFT) green, REC (PLAY) red; no key");
    masif_ui_key(7, 1); audio(1);
    check(leds_keys() == 1u << 7, "a note key held: lit");
    masif_ui_key(7, 0); audio(1);
    check(!leds_keys(), "let go: dark");
    check(MB_UP == 3 && MB_LEFT == 8 && MB_RIGHT == 10 && MB_DOWN == 9 && MB_SHIFT == 12 &&
          MB_OPTION == 6 && MB_EDIT == 7 && MB_PLAY == 13,
          "SEL = UP, HOME = LEFT, ARP = RIGHT, SAVE = DOWN, PLAY = SHIFT, EDIT = OPTION, GLO = EDIT, REC = PLAY");

    /* ---- the song page: EDIT, EDIT + the arrows, OPTION + EDIT, the double tap */
    edit_tap();
    check(masif_song.song[0][0] == 0, "EDIT on an empty song cell: the chain entered last (00)");
    with(MB_EDIT, MB_RIGHT); with(MB_EDIT, MB_UP);
    check(masif_song.song[0][0] == 0x11, "EDIT + RIGHT: +1, EDIT + UP: +0x10");
    with(MB_EDIT, MB_DOWN); with(MB_EDIT, MB_LEFT);
    check(masif_song.song[0][0] == 0x00, "EDIT + DOWN: -0x10, EDIT + LEFT: -1");
    with(MB_OPTION, MB_EDIT);
    check(masif_song.song[0][0] == MASIF_NONE, "OPTION + EDIT: the cell empty again");
    edit_tap();                                     /* row 0: chain 00 */
    press(MB_DOWN);
    press(MB_EDIT); press(MB_EDIT); fm1_ms += 1000;
    check(masif_song.song[1][0] == 1, "EDIT twice quickly: a new chain (00 is in the song: 01)");
    press(MB_DOWN);
    press(MB_EDIT); fm1_ms += 1000; press(MB_EDIT); fm1_ms += 1000;
    check(masif_song.song[2][0] == 1, "EDIT twice slowly: twice the one entered last (01), no new one");
    with(MB_OPTION, MB_EDIT);
    with(MB_SHIFT, MB_LEFT);
    check(mu.page == MU_SONG, "SHIFT + LEFT on the song page: nothing (the pages do not go round with the arrows)");
    with(MB_SHIFT, MB_RIGHT);
    check(mu.page == MU_SONG && mu.msg && !strcmp(mu.msg, "NO CHAIN HERE"), "SHIFT + RIGHT on an empty song cell: no chain page");
    press(MB_UP);
    ppm("msf-1-song");
    with(MB_SHIFT, MB_RIGHT);
    check(mu.page == MU_CHAIN && mu.chain == 1, "SHIFT + RIGHT on a chain: its page");

    /* ---- the chain page: no phrase page before a phrase is there */
    with(MB_SHIFT, MB_RIGHT);
    check(mu.page == MU_CHAIN && !strcmp(mu.msg, "NO PHRASE HERE"), "SHIFT + RIGHT on an empty chain step: no phrase page");
    turn(ME_PRESETS, 1);
    check(mu.page == MU_CHAIN, "nor by the PRESETS knob");
    edit_tap();
    press(MB_DOWN);
    press(MB_EDIT); press(MB_EDIT); fm1_ms += 1000;
    check(masif_song.chain[1][0].phrase == 0 && masif_song.chain[1][1].phrase == 1, "EDIT: phrase 00; EDIT twice quickly on the next step: a new phrase (01)");
    press(MB_UP);
    with(MB_SHIFT, MB_RIGHT);
    check(mu.page == MU_PHRASE && mu.phrase == 0, "SHIFT + RIGHT on a phrase: its page");

    /* ---- the phrase page */
    edit_tap();
    check(masif_song.phrase[0][0].note == 60 && masif_song.phrase[0][0].instrument == 0, "EDIT on an empty note: the note and the instrument entered last (C-4, 00)");
    down(MB_EDIT); press(MB_UP); press(MB_LEFT);
    check(mu.previewing && sp_ch[SP_NCH + mu.track].on, "EDIT held on a note: it is heard as it changes");
    up(MB_EDIT); audio(2);
    check(masif_song.phrase[0][0].note == 71, "EDIT + UP: an octave up, EDIT + LEFT: a semitone down (B-4)");
    press(MB_RIGHT);
    press(MB_EDIT); press(MB_EDIT); fm1_ms += 1000;
    check(masif_song.phrase[0][0].instrument == 1, "EDIT twice quickly on the instrument: the first one without a sample (01)");
    with(MB_EDIT, MB_LEFT);                         /* (back to 00, the one with a sample) */
    press(MB_LEFT); press(MB_DOWN);
    key(7);
    check(masif_song.phrase[0][1].note == 60 && masif_song.phrase[0][1].instrument == 0, "a note key in the note column: that note, with the instrument entered last");
    press(MB_UP);
    for (i = 0; i < 3u; i++) press(MB_RIGHT);       /* FX1 */
    check(mu.pcol == MP_FX1, "RIGHT three times: the first effect column");
    down(MB_EDIT);
    check(mu.pick == PK_FX && mu_pick_fx(mu.pick_cur) == MASIF_FX_RETRIGGER && !strncmp(line(0), "RET", 3),
          "EDIT held on an effect: the effects' list over the screen, the one entered last marked");
    press(MB_RIGHT); press(MB_RIGHT); press(MB_DOWN);
    i = mu_pick_fx(mu.pick_cur);
    shot("msf-2-fx-list");
    up(MB_EDIT);
    check(mu.pick == PK_NONE && mu.page == MU_PHRASE && masif_song.phrase[0][0].fx == i && i == MU_FX_ORDER[mu_fx_index(MASIF_FX_RETRIGGER) + 2u + MU_FXP_COLS],
          "the arrows go through the list (RIGHT one, DOWN a row of 5), letting EDIT go enters it, back on the phrase");
    down(MB_EDIT); press(MB_RIGHT); press(MB_OPTION); up(MB_EDIT);
    check(masif_song.phrase[0][0].fx == i && !mu.pick, "OPTION while the list is open: letting EDIT go changes nothing");
    press(MB_RIGHT);
    down(MB_EDIT);
    check(!mu.pick, "EDIT on the effect's parameter: no list");
    press(MB_RIGHT); press(MB_UP); up(MB_EDIT);
    check(masif_song.phrase[0][0].param == 0x11, "EDIT + the arrows on the parameter: +1, +0x10");
    press(MB_LEFT);
    with(MB_OPTION, MB_EDIT);
    check(!masif_song.phrase[0][0].fx && !masif_song.phrase[0][0].param, "OPTION + EDIT on an effect: it and its parameter gone");
    for (i = 0; i < 3u; i++) press(MB_LEFT);
    press(MB_DOWN);
    with(MB_OPTION, MB_EDIT);
    check(masif_song.phrase[0][1].note == MASIF_NOTE_NONE && mu_clip.n == 1 && mu_clip.page == MU_PHRASE && mu_clip.d[0][0] == 60,
          "OPTION + EDIT on a note: cut, the note in the clipboard");
    press(MB_DOWN);
    with(MB_SHIFT, MB_EDIT);
    check(masif_song.phrase[0][2].note == 60 && masif_song.phrase[0][1].note == MASIF_NOTE_NONE, "SHIFT + EDIT: the cut note pasted elsewhere");
    press(MB_UP);                                   /* (a paste leaves the cursor under what it pasted) */
    with(MB_OPTION, MB_EDIT); press(MB_UP);
    with(MB_SHIFT, MB_EDIT);                        /* (back to row 1) */
    check(masif_song.phrase[0][1].note == 60 && masif_song.phrase[0][2].note == MASIF_NOTE_NONE, "and cut again, back where it was");
    mu.row = 2;
    with(MB_OPTION, MB_EDIT);
    check(masif_song.phrase[0][2].note == MASIF_NOTE_OFF && !strncmp(line(MU_DATA + 2u) + 2, "OFF", 3),
          "OPTION + EDIT on an empty note: OFF (the note let go, as STP 00)");
    with(MB_OPTION, MB_EDIT);
    check(masif_song.phrase[0][2].note == MASIF_NOTE_NONE && mu_clip.d[0][0] == MASIF_NOTE_OFF, "OPTION + EDIT on OFF: cut, as any note");
    {   /* a note's cut takes its instrument and volume too (the user, 2026-10-21); EDIT a few ms before OPTION is
         * still OPTION + EDIT */
        masif_cell_t c = masif_song.phrase[0][1];
        mu.row = 1;
        with(MB_OPTION, MB_EDIT);
        check(masif_song.phrase[0][1].note == MASIF_NOTE_NONE && masif_song.phrase[0][1].instrument == 0xFF &&
              masif_song.phrase[0][1].volume == 0xFF && mu_clip.c0 == MP_NOTE && mu_clip.c1 == MP_VOL &&
              mu_clip.d[0][1] == c.instrument && mu_clip.d[0][2] == c.volume, "OPTION + EDIT on a note: its instrument and volume go with it");
        mu.row = 3;
        with(MB_SHIFT, MB_EDIT);
        check(masif_song.phrase[0][3].note == c.note && masif_song.phrase[0][3].instrument == c.instrument &&
              masif_song.phrase[0][3].volume == c.volume, "SHIFT + EDIT: all three back");
        mu.row = 2; hush();
        edit_down_now(); down(MB_OPTION); up(MB_EDIT); up(MB_OPTION);
        check(masif_song.phrase[0][2].note == MASIF_NOTE_OFF && masif_song.phrase[0][2].instrument == 0xFF && !heard(),
              "EDIT, then OPTION, on an empty note: OFF (not EDIT's insert), nothing heard");
        with(MB_OPTION, MB_EDIT);
        mu.row = 3; mu.last_note = 72;
        down(MB_EDIT);
        check(mu.last_note == c.note, "EDIT on a note: that note is the one heard and the next entered");
        up(MB_EDIT); hush();
        edit_down_now(); down(MB_OPTION); up(MB_EDIT); up(MB_OPTION);
        check(masif_song.phrase[0][3].note == MASIF_NOTE_NONE && mu_clip.d[0][0] == c.note && !heard(),
              "EDIT, then OPTION, on a note: cut, nothing heard");
        mu.row = 1;
        with(MB_SHIFT, MB_EDIT);
        check(masif_song.phrase[0][1].note == c.note && masif_song.phrase[0][1].instrument == c.instrument && !heard(),
              "and pasted back, nothing heard");
        with(MB_OPTION, MB_EDIT); mu.row = 3; hush();
        edit_down_now(); down(MB_SHIFT); up(MB_EDIT); up(MB_SHIFT);
        check(masif_song.phrase[0][3].note == c.note && masif_song.phrase[0][3].volume == c.volume && !heard(),
              "EDIT, then SHIFT: the paste, nothing heard");
        mu.row = 3; with(MB_OPTION, MB_EDIT); mu.row = 2;   /* (as the tests below expect it) */
    }

    /* ---- select, copy, paste */
    press(MB_UP); press(MB_UP);
    down(MB_SHIFT); press(MB_OPTION); up(MB_SHIFT);
    press(MB_DOWN);
    check(mu.sel && mu.sel_row == 0 && mu.row == 1, "SHIFT + OPTION: a selection from the cursor, DOWN stretches it");
    shot("msf-3-selection");
    press(MB_OPTION);
    check(!mu.sel && mu_clip.n == 2 && mu.msg && !strcmp(mu.msg, "COPIED"), "OPTION: the selection copied");
    press(MB_DOWN); press(MB_DOWN); press(MB_DOWN);
    with(MB_SHIFT, MB_EDIT);
    check(masif_song.phrase[0][4].note == 71 && masif_song.phrase[0][5].note == 60 && masif_song.phrase[0][5].instrument == MASIF_NONE,
          "SHIFT + EDIT: pasted at the cursor (the column copied: the notes)");
    down(MB_SHIFT); press(MB_OPTION); press(MB_OPTION); up(MB_SHIFT);
    down(MB_EDIT); press(MB_RIGHT); up(MB_EDIT);
    check(masif_song.phrase[0][0].note == 72 && masif_song.phrase[0][5].note == 61 && masif_song.phrase[0][3].note == MASIF_NOTE_NONE,
          "SHIFT + OPTION twice: the whole column; EDIT + RIGHT: every note in it a semitone up, the empty ones stay empty");
    down(MB_OPTION); press(MB_EDIT); up(MB_OPTION);
    check(!mu.sel && masif_song.phrase[0][0].note == MASIF_NOTE_NONE && masif_song.phrase[0][5].note == MASIF_NOTE_NONE,
          "OPTION + EDIT with a selection: cut");
    mu.row = 0;
    with(MB_SHIFT, MB_EDIT);                        /* (and back) */

    /* ---- an arrow held goes again */
    mu.row = 0;
    down(MB_DOWN);
    tick(100);
    check(mu.row == 1, "an arrow: once at once");
    tick(MU_REPEAT_FIRST);
    for (i = 0; i < 3u; i++) tick(MU_REPEAT_NEXT);
    up(MB_DOWN);
    tick(500);
    check(mu.row == 5, "held: again after a while, then quickly; let go: no more");
    mu.row = 0;

    /* ---- PLAY */
    press(MB_PLAY); audio(4);
    check(sq.playing && masif_runtime.tracker.what == MASIF_PLAY_PHRASE, "PLAY on the phrase page: the phrase");
    press(MB_PLAY); audio(4);
    check(!sq.playing, "PLAY again: stop");
    with(MB_SHIFT, MB_PLAY); audio(4);
    check(sq.playing && masif_runtime.tracker.what == MASIF_PLAY_SONG, "SHIFT + PLAY: the song");
    press(MB_PLAY); audio(4);

    /* ---- the instrument page */
    with(MB_SHIFT, MB_RIGHT);
    check(mu.page == MU_INST && mu.inst == 0, "SHIFT + RIGHT on the phrase: the row's instrument");
    with(MB_OPTION, MB_RIGHT);
    check(mu.inst == 1, "OPTION + RIGHT: the next instrument");
    with(MB_OPTION, MB_LEFT);
    with(MB_OPTION, MB_UP);
    check(mu.zoom <= 1u, "OPTION + UP on the instrument page: nothing (no zoom)");
    press(MB_RIGHT); press(MB_RIGHT);
    check(mu.itab == MI_AMP && mu.irow == 0, "LEFT / RIGHT: the tabs (AMP, its VOLUME)");
    r = masif_song.instrument[0].volume;
    with(MB_EDIT, MB_DOWN); with(MB_EDIT, MB_RIGHT);
    check(masif_song.instrument[0].volume == r - 16u + 1u, "EDIT + DOWN / RIGHT on a setting: -0x10, +1");
    press(MB_UP);
    check(mu.inum, "UP from the first setting: onto the instrument's number");
    with(MB_EDIT, MB_RIGHT);
    check(mu.inst == 1 && !mu.pick, "EDIT + RIGHT there: another instrument (no pool)");
    with(MB_EDIT, MB_LEFT);
    press(MB_DOWN); press(MB_LEFT); press(MB_LEFT);
    check(mu.itab == MI_WAVE && mu.irow == 0 && !mu.inum, "back on WAVE's SAMPLE");
    down(MB_EDIT);
    check(mu.pick == PK_POOL && mu_pool_wave(mu.pick_cur) == 5, "EDIT held on SAMPLE: the pool, the instrument's own marked");
    press(MB_DOWN);
    press(MB_PLAY); audio(2);
    check(sp_ch[SP_NCH + mu.track].on && sp_ch[SP_NCH + mu.track].w == &sp_wave[6], "DOWN, PLAY: the next sample heard");
    shot("msf-4-pool");
    up(MB_EDIT); audio(2);
    check(mu.pick == PK_POOL && masif_song.instrument[0].sample == 5, "letting EDIT go: the pool stays open");
    press(MB_EDIT); audio(2);
    check(mu.pick == PK_POOL && masif_song.instrument[0].sample == 6, "EDIT again: the instrument has it, the pool still open");
    check(line(MU_POOL_ROW0 + mu.pick_cur)[2] == '*', "the instrument's sample marked *");
    press(MB_UP); press(MB_OPTION); audio(2);
    check(!mu.pick && masif_song.instrument[0].sample == 6, "UP, OPTION: closed, what EDIT took stays");
    masif_fm1_preview(mu.track, 0, 0, 0); audio(400);

    /* ---- the playhead */
    mu_edit1(MASIF_EDIT_INSTRUMENT, 0, MU_OFF(sample), 5); audio(2);
    press(MB_PLAY); audio(10);
    {
        const uint32_t y = MU_Y0 + MU_WAVE_R0 * MU_CH + 2u;
        int32_t x0, x1;
        masif_ui_draw();
        x0 = mu_head_x(mu.track, &sp_wave[5], 0, 2000);
        check(x0 > 0 && px((uint32_t)x0, y, MU_INK) && px((uint32_t)x0 - 1u, y, MU_BG) && px((uint32_t)x0 + 1u, y, MU_BG),
              "the instrument's sound playing: a playhead over the picture where its voice is");
        shot("msf-5-playhead");
        audio(20);
        masif_ui_draw();
        x1 = mu_head_x(mu.track, &sp_wave[5], 0, 2000);
        check(x1 > x0 && px((uint32_t)x1, y, MU_INK), "and it moves along with the sound");
    }
    press(MB_PLAY); audio(400);

    /* ---- the knobs: OPTION + a knob gives it the setting under the cursor */
    press(MB_RIGHT); press(MB_RIGHT); press(MB_RIGHT); press(MB_DOWN);   /* FILT, CUTOFF */
    check(mu.itab == MI_FILT && mu.irow == 1, "FILT tab, CUTOFF");
    down(MB_OPTION); turn(ME_KNOB1, 1); up(MB_OPTION);
    check(mu_knobs[0][0].on && mu_knobs[0][0].page == MU_INST && mu_knobs[0][0].inst == 0 && mu_knobs[0][0].row == 1 && !mu.sel,
          "OPTION + KNOB 1: it has the instrument's CUTOFF");
    r = masif_song.instrument[0].cutoff;
    press(MB_UP); with(MB_SHIFT, MB_LEFT);          /* elsewhere: the phrase page */
    turn(ME_KNOB1, -3); audio(2);
    check(mu.page == MU_PHRASE && masif_song.instrument[0].cutoff == r - 3u && !strncmp(line(MU_ROWS - 1u), "1 I00 CUTOFF", 12),
          "KNOB 1 turned on another page: that cutoff, and the bottom row says so");
    down(MB_SHIFT); turn(ME_KNOB1, -1); up(MB_SHIFT); audio(2);
    check(masif_song.instrument[0].cutoff == r - 3u - 16u, "SHIFT + KNOB 1: by 0x10");
    press(MB_BANK);
    turn(ME_KNOB1, 5); audio(2);
    check(mu.bank == 0 && masif_song.instrument[0].cutoff == r - 3u - 16u + 5u, "FX: nothing (the user, 2026-10-23): KNOB 1 still the cutoff");
    with(MB_SHIFT, MB_LEFT); with(MB_SHIFT, MB_LEFT); with(MB_SHIFT, MB_DOWN);
    check(mu.page == MU_MIXER, "SHIFT + LEFT twice, SHIFT + DOWN: the mixer");
    down(MB_OPTION); turn(ME_KNOB1 + 1u, 1); up(MB_OPTION);
    turn(ME_KNOB1 + 1u, -4); audio(2);
    check(mu_knobs[0][1].page == MU_MIXER && masif_song.track_level[0] == 100u - 4u, "OPTION + KNOB 2 on the mixer: track 1's level");
    down(MB_SHIFT); down(MB_OPTION); turn(ME_KNOB1 + 1u, 1); up(MB_OPTION); up(MB_SHIFT);
    check(!mu_knobs[0][1].on && !mu.sel, "SHIFT + OPTION + KNOB 2: taken away");
    press(MB_EDIT);
    check(masif_mute == 1u, "EDIT on a track's level: its mute");
    press(MB_EDIT);
    check(masif_mute == 0u, "and again: on");
    with(MB_EDIT, MB_UP); audio(2);
    check(masif_song.track_level[0] == 100u - 4u + 16u, "EDIT + UP: the level, +0x10");
    with(MB_EDIT, MB_DOWN); audio(2);
    check(masif_song.track_level[0] == 100u - 4u && masif_mute == 0u, "EDIT + DOWN: -0x10, and no mute");

    /* ---- the pages up and down; the project */
    with(MB_SHIFT, MB_UP); with(MB_SHIFT, MB_UP);
    check(mu.page == MU_PROJECT, "SHIFT + UP twice from the mixer: the project");
    with(MB_SHIFT, MB_UP);
    check(mu.page == MU_PROJECT, "and no further");
    mu.prow = MJ_SAVE;
    press(MB_EDIT);
    {
        masif_store_info_t info;
        check(masif_store_info(0, &info) && info.used, "EDIT on SAVE: the song in slot 1");
    }
    turn(ME_PRESETS, 1);
    check(mu.page == MU_PROJECT, "PRESETS on the project: nothing to the right of it (it is S's)");
    down(MB_SHIFT); turn(ME_PRESETS, 1); up(MB_SHIFT);
    check(mu.page == MU_SONG, "SHIFT + PRESETS: down to the song");
    turn(ME_PRESETS, -1);
    check(mu.page == MU_SONG, "PRESETS left on the song: S is the end, no wrap");
    mu.song_row = 1; mu.track = 0;
    turn(ME_PRESETS, 6);
    check(mu.page == MU_TABLE, "PRESETS right: C P I T (the cells have something), and T is the end, no wrap");
    turn(ME_PRESETS, -9);
    check(mu.page == MU_SONG, "and back to S");
    down(MB_SHIFT); turn(ME_PRESETS, 1); up(MB_SHIFT);
    check(mu.page == MU_MIXER, "SHIFT + PRESETS: down to the mixer");

    /* ---- the groove page above the table (the user, 2026-10-23), the SELECT and ALGORITHM knobs */
    mu_goto(MU_PHRASE);
    with(MB_SHIFT, MB_UP);
    check(mu.page == MU_PHRASE, "SHIFT + UP on the phrase: nothing above it now");
    mu_goto(MU_TABLE);
    with(MB_SHIFT, MB_UP);
    check(mu.page == MU_GROOVE && !strncmp(line(0) + 15, "SCPIG", 5), "SHIFT + UP on the table: the groove, G in T's place");
    with(MB_SHIFT, MB_RIGHT);
    check(mu.page == MU_GROOVE, "SHIFT + RIGHT on the groove: nothing (T is the end)");
    with(MB_SHIFT, MB_DOWN);
    check(mu.page == MU_TABLE, "SHIFT + DOWN: back on the table");
    mu_goto(MU_GROOVE);
    with(MB_SHIFT, MB_LEFT);
    check(mu.page == MU_INST, "SHIFT + LEFT on the groove: the instrument");
    mu_goto(MU_GROOVE); mu.gnum = 0; mu.grow = 0;
    turn(ME_ALGORITHM, 2);
    check(mu.grow == 2, "ALGORITHM: the cursor down");
    {
        uint8_t g = 6;
        masif_song.groove[mu.groove][2] = g;        /* (stopped: nothing reads it now) */
        down(MB_SHIFT); turn(ME_SELECT, -1); up(MB_SHIFT); audio(1);
        check(masif_song.groove[mu.groove][2] == (uint8_t)(g - 1u), "SHIFT + SELECT: the value");
        down(MB_SHIFT); turn(ME_SELECT, 1); up(MB_SHIFT); audio(1);
    }
    mu_goto(MU_PHRASE); mu.row = 0; mu.pcol = 0;
    turn(ME_SELECT, 2); turn(ME_ALGORITHM, 3);
    check(mu.pcol == 2 && mu.row == 3, "SELECT: the cursor along, ALGORITHM: up and down");
    turn(ME_SELECT, -2); turn(ME_ALGORITHM, -3);
    mu_goto(MU_INST); mu.itab = MI_WAVE; mu.inum = 0;
    {
        int smpl = 0, rr;
        masif_ui_draw();
        for (rr = 0; rr < (int)MU_ROWS; rr++) smpl |= strstr(line((uint32_t)rr), "SMPL") != 0;
        check(smpl, "the instrument page's first tab: SMPL");
    }
    check(!strcmp(MU_FREE_NAME[0], "PROJ") && !strcmp(MU_FREE_NAME[1], "CHRO"), "SCALE: PROJ or CHRO");
    mu_goto(MU_MIXER);

    /* ---- cloning: EDIT on a selection */
    with(MB_SHIFT, MB_UP);
    mu.song_row = 1; mu.track = 0; fm1_ms += 1000;
    down(MB_SHIFT); press(MB_OPTION); up(MB_SHIFT);
    press(MB_EDIT);
    check(masif_song.song[1][0] == 2 && !memcmp(masif_song.chain[2], masif_song.chain[1], sizeof masif_song.chain[1]) && !strcmp(mu.msg, "CLONED"),
          "EDIT on a selected song cell: its chain cloned (a new chain, 02, the same phrases)");
    press(MB_EDIT);
    check(masif_song.chain[2][0].phrase == 2 && masif_song.chain[2][1].phrase == 3 && masif_song.chain[1][0].phrase == 0 &&
          !memcmp(masif_song.phrase[2], masif_song.phrase[0], sizeof masif_song.phrase[0]) && !strcmp(mu.msg, "DEEP CLONED"),
          "EDIT again soon: deep, its phrases cloned too (02, 03); the first chain keeps its own");
    press(MB_OPTION); fm1_ms += 1000;
    with(MB_SHIFT, MB_RIGHT);
    down(MB_SHIFT); press(MB_OPTION); up(MB_SHIFT);
    press(MB_EDIT);
    check(mu.page == MU_CHAIN && mu.chain == 2 && masif_song.chain[2][0].phrase == 4 &&
          !memcmp(masif_song.phrase[4], masif_song.phrase[2], sizeof masif_song.phrase[0]), "EDIT on a selected chain step: its phrase cloned (04)");
    press(MB_OPTION); fm1_ms += 1000;
    with(MB_SHIFT, MB_LEFT);

    /* ---- mute and solo */
    with(MB_OPTION, MB_PLAY);
    check(masif_mute == 1u && line(1)[5] == 'M' && !sq.playing, "OPTION + PLAY: the cursor's track muted (M at its head), nothing starts");
    press(MB_RIGHT);
    with(MB_EDIT, MB_PLAY); audio(2);
    check(mu.solo == 2u && masif_mute == (1u | 4u | 8u) && line(1)[8] == 'S' && masif_song.song[1][1] == MASIF_NONE,
          "EDIT + PLAY: its solo (the others silent); what EDIT's press put in the empty cell is taken back");
    with(MB_EDIT, MB_PLAY); press(MB_LEFT); with(MB_OPTION, MB_PLAY);
    check(masif_mute == 0u && !mu.solo, "and again: all heard");

    /* ---- PLAY blinks on the beats */
    press(MB_PLAY);
    {
        int off = 0, on = 0, keyon = 0;
        for (i = 0; i < 2000u && !(off && on && keyon); i++) {
            audio(1);
            if (masif_runtime.active_note[0] >= 53u && masif_runtime.active_note[0] <= 79u && sp_ch[SP_NCH].on)
                keyon |= (int)((leds_keys() >> (masif_runtime.active_note[0] - 53u)) & 1u);
            if (sq.playing && sq_ticks % 96u >= 12u) off |= !((leds_btn() >> MK_REC) & 1u) && leds_green() == 1u;
            if (sq.playing && sq_ticks % 96u < 12u) on |= ((leds_btn() >> MK_REC) & 1u) && leds_green() == 1u;
        }
        check(keyon, "playing: the key of a note a track sounds is lit");
        check(off && on && (leds_btn() | (1u << MK_REC)) == (0x3FFFu & ~(1u << MK_PLAY) &
              ~(1u << MK_OCTDN | 1u << MK_OCTUP | 1u << MK_FX | 1u << MK_ENV | 1u << MK_LFO | 1u << MK_SEQ)),
              "playing: REC (PLAY) a red flash on each beat (half a row), dark the rest, the white ones in use lit (not OCT, FX, ENV, LFO, SEQ), SHIFT green");
    }
    press(MB_PLAY); audio(4);

    /* ---- a setting with names, an instrument's name */
    mu_goto(MU_INST); mu.inst = 0; mu.itab = MI_WAVE; mu.inum = 0;
    {
        mu_par_t p;
        for (mu.irow = 0; mu_par(mu.irow, &p) && p.names != MU_PLAY_NAME; mu.irow++) {}
        down(MB_EDIT);
        check(mu.pick == PK_PAR && mu.pick_cur == masif_song.instrument[0].play, "EDIT held on PLAY: its list, the one it is marked");
        press(MB_DOWN);
        r = mu.pick_cur;
        shot("msf-8-play-list");
        up(MB_EDIT);
        check(!mu.pick && masif_song.instrument[0].play == r && r != MASIF_PLAY_FWD, "DOWN, letting EDIT go: that one");
        for (i = 0, c = mu.irow; i <= c; i++) press(MB_UP);
    }
    press(MB_RIGHT);
    check(mu.inum && mu.ichar == 1, "on the instrument's number, RIGHT: its name's first character");
    down(MB_EDIT);
    check(mu.pick == PK_CHAR && MU_CHARSET[mu.pick_cur] == 'A', "EDIT held: the characters, A marked");
    press(MB_RIGHT);
    shot("msf-9-name");
    up(MB_EDIT);
    press(MB_EDIT);
    check(!memcmp(masif_song.instrument[0].name, "BA", 3) && mu.ichar == 3 && !strncmp(line(0), "BA", 2),
          "B, then A: the name BA, the page's title, the cursor on to the next");
    press(MB_LEFT); press(MB_LEFT);
    with(MB_OPTION, MB_EDIT); press(MB_RIGHT); with(MB_OPTION, MB_EDIT);
    check(masif_song.instrument[0].name[0] == ' ' && !masif_song.instrument[0].name[1], "OPTION + EDIT: a character gone (one in the middle: a space)");
    mu_edit1(MASIF_EDIT_INSTRUMENT, 0, MU_OFF(name), 'K'); audio(1);
    press(MB_DOWN);

    /* ---- the project page: the arrows' repeat (350 ms, then every 60: no setting), the song's name, THEME */
    mu_goto(MU_PROJECT); mu.prow = MJ_NEW;
    down(MB_UP); tick(MU_REPEAT_FIRST); tick(MU_REPEAT_NEXT); up(MB_UP);
    check(mu.prow == MJ_NEW - 3u, "an arrow held: again after 350 ms, then every 60");
    check(strstr(line(12), "THEME") && !strstr(line(13), "KEY") && !strstr(line(15), "LED") && !strstr(line(3), "FREE"),
          "THEME; no KEY RPT, no LED TEST, no FREE beside the tempo");
    {
        masif_store_info_t info;
        mu.prow = MJ_SLOT; mu.schar = 0;
        for (i = 0; i < 9u; i++) mu.sname[i] = 0;
        press(MB_RIGHT);
        check(mu.schar == 1 && line(2)[11] == '.', "RIGHT on SONG: onto the name's first character (none yet: '.')");
        with(MB_EDIT, MB_RIGHT);
        check(mu.sname[0] == 'A' && line(2)[11] == 'A', "EDIT + RIGHT: A");
        with(MB_EDIT, MB_LEFT);
        check(mu.sname[0] == ' ', "EDIT + LEFT: back round to the space");
        with(MB_EDIT, MB_DOWN);
        check(mu.sname[0] == '9', "EDIT + DOWN: on to 9 (A-Z, 0-9, space)");
        press(MB_RIGHT); press(MB_RIGHT);
        with(MB_EDIT, MB_UP); with(MB_EDIT, MB_UP);
        check(!strcmp(mu.sname, "9 B"), "a character further on: the ones before it spaces");
        with(MB_OPTION, MB_EDIT);
        check(!strcmp(mu.sname, "9 "), "OPTION + EDIT: the character gone");
        shot("msf-10-song-name");
        mu.prow = MJ_SAVE; edit_tap();
        check(masif_store_info(mu.slot, &info) && !strcmp(info.name, "9") && !strcmp(mu.sname, "9"),
              "SAVE: the song saved with its name (no spaces at the end)");
        mu.prow = MJ_NEW; edit_tap();
        check(!mu.sname[0], "NEW: no name");
        mu.prow = MJ_LOAD; edit_tap();
        check(!strcmp(mu.sname, "9"), "LOAD: the slot's name");
        press(MB_DOWN);
        check(!mu.schar, "UP / DOWN: off the name");
    }
    mu.prow = MJ_TEMPO;
    mu_goto(MU_MIXER);

    /* ---- where one left off: kept a while after the last touch, back at power-on */
    with(MB_SHIFT, MB_UP);
    mu.song_row = 1; mu.track = 0;
    with(MB_SHIFT, MB_RIGHT); press(MB_DOWN); press(MB_DOWN);   /* chain 02 (the clone), step 2 */
    tick(1000);
    {
        masif_store_info_t info;
        check(!masif_store_info(MASIF_STORE_WORK, &info) || !info.used, "nothing kept while the controls are being used");
        tick(MU_KEEP_QUIET);
        check(masif_store_info(MASIF_STORE_WORK, &info) && info.used && flash[MU_KEEP_AT] == 'M', "a few seconds after the last touch: the song and the screens kept");
    }
    memcpy(&was, &masif_song, sizeof was);
    for (i = 0; i < 40u; i++) { press(i & 1u ? MB_UP : MB_DOWN); tick(MU_KEEP_QUIET + 1000u); }   /* (more records than a sector has) */
    check(mu.keep_next >= 1u && mu.keep_next <= MU_KEEP_RECS, "the records go round in their sector");
    press(MB_DOWN); tick(MU_KEEP_QUIET + 1000u);
    masif_song_clear(&masif_song); masif_fm1_song_changed();          /* the power goes; it comes back */
    memset(&mu_knobs, 0, sizeof mu_knobs);
    masif_ui_init();
    masif_store_init(pool, 1);
    check(masif_store_load(&masif_song, MASIF_STORE_WORK) == MASIF_STORE_OK, "at power-on the working copy loads");
    masif_fm1_song_changed();
    masif_ui_resume();
    audio(2);
    check(!memcmp(&was, &masif_song, sizeof was), "the song as it was");
    check(mu.page == MU_CHAIN && mu.chain == 2 && mu.step == 3 && mu.song_row == 1 && mu.phrase == 0 && mu.inst == 0 &&
          mu.itab == MI_WAVE && masif_song.instrument[0].name[0] == 'K' && mu_knobs[0][0].on && mu_knobs[0][0].row == 1 && !mu_knobs[0][1].on,
          "the page, the cursors and the knobs as they were");
    check(!strcmp(mu.sname, "9"), "and the song's name");
    tick(MU_KEEP_QUIET + 1000u);
    c = mu.keep_next;
    tick(MU_KEEP_QUIET + 1000u);
    check(mu.keep_next == c, "nothing changed: nothing written");
    ppm("msf-7-resumed");
    {                                               /* a record of the firmware before FONT (version 3) still loads */
        static uint8_t rec[MU_KEEP_REC];
        uint32_t n = mu_keep_pack(rec), crc;
        uint8_t knob0 = mu_knobs[0][0].row;
        memmove(rec + 8u + MU_NKEPT - 1u, rec + 8u + MU_NKEPT, n - 8u - MU_NKEPT);
        rec[2] = 3; rec[3] = (uint8_t)(rec[3] - 1u);
        crc = ms_crc(0xFFFFFFFFu, rec + 8, rec[3]);
        rec[4] = (uint8_t)crc; rec[5] = (uint8_t)(crc >> 8); rec[6] = (uint8_t)(crc >> 16); rec[7] = (uint8_t)(crc >> 24);
        mu.font = 1; mu_knobs[0][0].row = 9;
        check(mu_keep_good(rec), "a version 3 record (no FONT) is good");
        mu_keep_unpack(rec);
        check(mu.font == 0 && mu_knobs[0][0].row == knob0 && mu_knobs[0][0].on, "and gives the knobs as they were, FONT SMOOTH");
    }

    puts(bad ? "MASIF controls tests: FAIL" : "MASIF controls tests: PASS");
    return bad ? 1 : 0;
}
