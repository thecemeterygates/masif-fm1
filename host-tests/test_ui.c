/* MASIF's screens on the host: the real screen code (firmware/src/masif_ui.c) on a framebuffer, worked through the
 * controls as a player would, with the real song model, player and adapter behind it. Checks what the controls do
 * to the song and what the pages show, and writes the screens as PPM images to look at.
 *   cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,signed-integer-overflow -fno-sanitize-recover=all \
 *      -Ifirmware/src -Ifirmware/gen -Ibuild/gen -o build/host/test_ui host-tests/test_ui.c && build/host/test_ui build/host/ui */
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
static void press(uint32_t b) { masif_ui_button(b, 1); masif_ui_button(b, 0); audio(1); }
static void shift_turn(uint32_t enc, int32_t d) { masif_ui_button(MK_GLO, 1); turn(enc, d); masif_ui_button(MK_GLO, 0); }
static void key(uint32_t k) { masif_ui_key(k, 1); audio(1); masif_ui_key(k, 0); audio(1); }
/* copy and paste: shift (GLO) + option (REC) selects, option copies, option + edit (SAVE) cuts,
 * shift + edit pastes */
static void sel(void) { masif_ui_button(MK_GLO, 1); press(MK_REC); masif_ui_button(MK_GLO, 0); }
static void copy(void) { press(MK_REC); }
static void cut(void) { masif_ui_button(MK_REC, 1); press(MK_SAVE); masif_ui_button(MK_REC, 0); audio(1); }
static void paste(void) { masif_ui_button(MK_GLO, 1); press(MK_SAVE); masif_ui_button(MK_GLO, 0); }
#define AT_SEL (MU_BG | MU_ON(MU_MID))
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

int main(int argc, char **argv)
{
    static uint8_t wave[3000];
    static int16_t pcm[2000];
    uint32_t n, i;
    dir = argc > 1 ? argv[1] : "build/host";
    memset(flash, 0xFF, sizeof flash);
    for (i = 0; i < 2000u; i++)                     /* a sample to look at: four hits that die away */
        pcm[i] = (int16_t)((((i * 37u) % 200u) < 100u ? 1 : -1) * (int32_t)(1800u - (i % 500u) * 3u) * 16);
    sp_pack(wave, pcm, 2000);
    sp_wave[5].d = wave; sp_wave[5].n = 2000; sp_wave[5].rate = 26040;
    sq_init();
    sq.bpm10 = 1600;
    masif_fm1_init();
    masif_song.instrument[5].sample = 5;            /* (a new song's instruments are empty: this one has the sample to look at) */
    masif_store_init(pool, 1);
    masif_ui_init();
    audio(1);

    ppm("1-song-empty");
    check(!strncmp(line(0), "SONG", 4) && !strncmp(line(0) + 15, "SCPIT", 5) && mu_at[0][15] == MU_CURSOR && mu_at[0][16] == MU_MID &&
          !strncmp(line(1), "    1  2  3  4", 14) && !strncmp(line(2), "00 -- -- -- --", 14),
          "the song page of an empty song: its name, S C P I T at the top right with S marked, the tracks' heads, 16 rows");

    /* a song by the controls alone: chains into the song, phrases into a chain, notes into a phrase */
    shift_turn(ME_SELECT, 1);                       /* an empty cell: the chain entered last (00) */
    turn(ME_SELECT, 1);
    shift_turn(ME_SELECT, 1); shift_turn(ME_SELECT, 1);          /* track 2: 00, then 01 */
    turn(ME_ALGORITHM, 1); turn(ME_SELECT, -1);
    shift_turn(ME_SELECT, 1); shift_turn(ME_ALGORITHM, 1);       /* row 1, track 1: 01, then + 0x10 = 11 */
    check(masif_song.song[0][0] == 0 && masif_song.song[0][1] == 1 && masif_song.song[1][0] == 0x11 && masif_song.song[1][1] == MASIF_NONE,
          "GLO + SELECT / ALGORITHM on the song page: a chain into the cell (the last one entered), then +-1 / +-0x10");
    masif_ui_button(MK_REC, 1); press(MK_SAVE); masif_ui_button(MK_REC, 0);
    check(masif_song.song[1][0] == MASIF_NONE, "REC (option) + SAVE: the cell under the cursor is empty again");
    turn(ME_ALGORITHM, -1); turn(ME_SELECT, 1);     /* back to row 0, track 2: chain 01 */
    ppm("2-song");

    press(MK_OCTUP);
    check(mu.page == MU_CHAIN && mu.chain == 1 && !strncmp(line(0), "CHAIN 01", 8) && !strncmp(line(1) + 3, "PH TSP", 6) && mu_at[0][16] == MU_CURSOR,
          "OCT+ from the song: the chain page, on the chain under the cursor; C marked; the heads PH and TSP");
    shift_turn(ME_SELECT, 1); shift_turn(ME_SELECT, 4);          /* step 0: phrase 00 -> 04 */
    turn(ME_ALGORITHM, 1);
    shift_turn(ME_SELECT, 1);                       /* step 1: phrase 04 (the last) */
    turn(ME_SELECT, 1);
    shift_turn(ME_ALGORITHM, -1);                   /* its transpose: an octave down */
    check(masif_song.chain[1][0].phrase == 4 && masif_song.chain[1][1].phrase == 4 && masif_song.chain[1][1].transpose == -12 &&
          masif_song.chain[1][2].phrase == MASIF_NONE, "the chain page: phrases into its steps, a step's transpose by octaves");
    ppm("3-chain");
    turn(ME_SELECT, -1); turn(ME_ALGORITHM, -1);

    press(MK_OCTUP);
    check(mu.page == MU_PHRASE && mu.phrase == 4 && !strncmp(line(0), "PHRASE 04", 9) && !strncmp(line(1), "  N  I V  FX1  FX2", 18),
          "OCT+ from the chain: the phrase page, on the phrase under the cursor; the heads N I V FX1 FX2");
    key(7);                                         /* the keys: C4 on row 0 */
    turn(ME_ALGORITHM, 4); key(10);                 /* D#4 on row 4 */
    turn(ME_ALGORITHM, 4); shift_turn(ME_SELECT, 1); shift_turn(ME_ALGORITHM, -1);   /* row 8: the last note, an octave down */
    turn(ME_SELECT, 2); shift_turn(ME_SELECT, 1); shift_turn(ME_ALGORITHM, -2);      /* its volume: 64 -> 44 */
    turn(ME_SELECT, 1); shift_turn(ME_SELECT, 1);   /* effect 1: the last one entered (RET) */
    turn(ME_SELECT, 1); shift_turn(ME_SELECT, 3); shift_turn(ME_ALGORITHM, 1);       /* its parameter: 13 */
    turn(ME_SELECT, 1); shift_turn(ME_SELECT, -5);  /* effect 2: 4 back from RET by the letters: PAN */
    {
        const masif_cell_t *c = masif_song.phrase[4];
        check(c[0].note == 60 && c[0].instrument == 0 && c[4].note == 63 && c[8].note == 51 && c[8].volume == 0x44 &&
              c[8].fx == MASIF_FX_RETRIGGER && c[8].param == 0x13 && c[8].fx2 == MASIF_FX_PAN && c[1].note == MASIF_NOTE_NONE,
              "the phrase page: notes from the keys and from GLO + the knobs, a volume, two effects with a parameter");
        check(!strncmp(line(2), "0 C-400--", 9) && !strncmp(line(10), "8 D#30044 RET13PAN", 18),
              "... and its rows read note, instrument, volume, two effects, without gaps");
    }
    ppm("4-phrase");
    n = blits;
    turn(ME_ALGORITHM, 1);
    masif_ui_draw();
    check(blits - n == 2u, "moving the cursor a row down sends two rows of the screen to the LCD, not all of it");

    /* PLAY plays what the page is on: here the phrase alone, on the track the song's cursor was on */
    press(MK_PLAY);
    audio(60);
    check(sq.playing && masif_runtime.tracker.what == MASIF_PLAY_PHRASE && masif_runtime.tracker.on[1] && masif_runtime.tracker.phrase[1] == 4 &&
          !masif_runtime.tracker.on[0], "PLAY on the phrase page: that phrase alone, on its track");
    ppm("5-phrase-playing");
    press(MK_PLAY);
    audio(10);
    check(!sq.playing, "PLAY again: stop");

    /* the instrument page: the sampler. Its waveform, its settings on tabs */
    turn(ME_ALGORITHM, -9);                         /* (the phrase's row 0: instrument 00) */
    press(MK_OCTUP);
    check(mu.page == MU_INST && mu.inst == 0 && !strncmp(line(6), "INST 00", 7) && mu_at[0][18] == MU_CURSOR && !strncmp(line(7), "SAMPLE", 6) &&
          !strncmp(line(7) + 15, "SMPL", 4), "OCT+ from the phrase: the instrument page, on the row's instrument (its number under the picture); its SMPL tab");
    shift_turn(ME_SELECT, 6);                       /* the sample: from none (--) to 5 */
    turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 2);          /* START 20 */
    turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 12);         /* LENGTH C0 */
    turn(ME_ALGORITHM, 1); shift_turn(ME_SELECT, 2);             /* PLAY: LOOP */
    turn(ME_SELECT, 1);                                          /* the LOOP tab */
    shift_turn(ME_ALGORITHM, 6);                                 /* LOOP 60 */
    turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 10);         /* LP END A0 */
    turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 2);          /* TPR IN 20 */
    turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 1);          /* TPR OUT 10 */
    {
        const masif_instrument_t *in = &masif_song.instrument[0];
        check(in->sample == 5 && in->start == 0x20 && in->length == 0xC0 && in->play == MASIF_PLAY_LOOP && in->loop_start == 0x60 && in->loop_end == 0xA0 &&
              in->taper_in == 0x20 && in->taper_out == 0x10, "the WAVE and LOOP tabs: the sample, its part, how it plays, its loop and the loop's tapers");
    }
    n = blits;
    ppm("6-instrument-loop");
    check(!strncmp(line(7), "LOOP    60", 10) && !strncmp(line(8), "LP END  A0", 10) && !strncmp(line(9), "TPR IN  20", 10) && !strncmp(line(10), "TPR OUT 10", 10) &&
          !strncmp(line(8) + 15, "LOOP", 4), "the LOOP tab: a loop's two ends and its two tapers");
    masif_fm1_preview(1, 0, masif_song.instrument[0].root_note, 127); audio(2);
    check(sp_ch[SP_NCH + 1].looped == 1 && sp_ch[SP_NCH + 1].loop == 750 && sp_ch[SP_NCH + 1].lend == 1250 && sp_ch[SP_NCH + 1].tin == 62 &&
          sp_ch[SP_NCH + 1].tout == 31, "a note: the loop 750..1250, the level rising over its first 62 samples (20/100) and falling over its last 31");
    {   /* the voice's output over the loop: silent where the loop begins, full past the taper, falling to its end */
        sp_ch_t *v = &sp_ch[SP_NCH + 1];
        v->pos = 750; v->frac = 0;
        check(v->tin && v->rtin == 65536u / 62u, "(its reciprocal, so the audio does not divide)");
    }
    masif_fm1_preview(1, 0, 0, 0); audio(2);
    mu.zoom = 0; press(MK_SEL);
    check(mu.zoom == 2u && !strncmp(line(MU_ROWS - 1u), "ZOOM 2X", 7), "SEL on the instrument page: ZOOM 2X (round the setting under the cursor)");
    ppm("6a-instrument-loop-zoom");
    press(MK_SEL); press(MK_SEL);
    check(mu.zoom == 1u, "SEL again: 4X, then 1X");
    turn(ME_SELECT, -1); turn(ME_ALGORITHM, 5); shift_turn(ME_SELECT, 4);   /* the WAVE tab: SLICES 4 */
    check(masif_song.instrument[0].slices == 4 && !strncmp(line(10), "PLAY    FWDLOOP", 15), "the WAVE tab again: its slices");
    ppm("6-instrument-wave");
    turn(ME_ALGORITHM, -2); shift_turn(ME_SELECT, 6);            /* PLAY: OSC PP, the last of the first nine */
    masif_ui_draw();
    check(masif_song.instrument[0].play == MASIF_PLAY_OSC_PING && !strncmp(line(10), "PLAY    OSC PP", 14),
          "PLAY: the first nine, FWD .. OSC PP");
    shift_turn(ME_SELECT, 1);
    masif_ui_draw();
    check(masif_song.instrument[0].play == MASIF_PLAY_REPITCH && !strncmp(line(10), "PLAY    REPITCH", 15) && !strncmp(line(11), "STEPS   10", 10),
          "REPITCH: the part lasts STEPS rows of a phrase (10 = 16: a bar)");
    shift_turn(ME_SELECT, 2); turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 9); shift_turn(ME_SELECT, 4);
    masif_ui_draw();
    check(masif_song.instrument[0].play == MASIF_PLAY_BPM && masif_song.instrument[0].steps == 164 && !strncmp(line(10), "PLAY    BPM", 11) &&
          !strncmp(line(11), "BPM     164", 11), "BPM: the same setting is the sample's own tempo, in BPM");
    shift_turn(ME_ALGORITHM, -9); shift_turn(ME_SELECT, -4); turn(ME_ALGORITHM, -1);
    shift_turn(ME_SELECT, -9); turn(ME_ALGORITHM, 2);            /* (FWDLOOP again, back on SLICES) */
    masif_ui_draw();
    check(!strncmp(line(0), "BREAK1      ", 12) && !strncmp(line(6), "INST 00", 7) && blits > n,
          "the sample's name is the page's title, the instrument's number is under the waveform; the waveform is drawn");
    n = blits;
    masif_ui_draw();
    check(blits == n, "nothing changed: nothing is sent to the LCD, the waveform is not drawn again");
    turn(ME_SELECT, 3);                             /* the FILT tab (WAVE, LOOP, AMP, FILT) */
    shift_turn(ME_SELECT, 3);                       /* TYPE: HIGH */
    turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, -3);         /* CUTOFF 7F -> 4F */
    turn(ME_SELECT, 1);                             /* the EQ tab */
    turn(ME_ALGORITHM, 2); shift_turn(ME_SELECT, -20);           /* HIGH -20 */
    check(masif_song.instrument[0].filter == MASIF_FILTER_HP && masif_song.instrument[0].cutoff == 0x4F && masif_song.instrument[0].eq[2] == -20 &&
          !strncmp(line(9), "HIGH    -20", 11) && (mu_at[11][15] & 3u) == MU_BG,
          "SELECT goes through the tabs (FILT: the filter's type and cutoff; EQ: a band), each with its own settings");
    ppm("7-instrument-eq");
    /* the AMP tab: the level's envelope as a picture, which follows its settings */
    turn(ME_SELECT, -2);
    ppm("7a-instrument-amp-open");
    check(!strncmp(line(9), "ATTACK  00", 10) && px(4, 46, MU_INK) && px(100, 46, MU_MID) && px(100, 21, MU_DIM),
          "the AMP tab: the envelope in place of the waveform; no attack, no decay: up at once and it stays up");
    turn(ME_ALGORITHM, 2); shift_turn(ME_ALGORITHM, 4);          /* ATTACK 40 */
    masif_ui_draw();
    check(masif_song.instrument[0].attack == 0x40 && px(4, 46, MU_DIM) && px(40, 46, MU_MID), "ATTACK turned: the picture's slope follows");
    turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, -5);         /* DECAY 2F */
    turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 3);          /* SUSTAIN 30 */
    turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 3);          /* RELEASE 30 */
    ppm("7b-instrument-amp");
    check(masif_song.instrument[0].decay == 0x2F && masif_song.instrument[0].sustain == 0x30 && px(120, 30, MU_DIM) && px(120, 62, MU_MID) &&
          px(230, 62, MU_DIM), "DECAY, SUSTAIN and RELEASE: down to the sustain level, held, and away after the note is let go");
    turn(ME_ALGORITHM, -3); shift_turn(ME_ALGORITHM, -4);        /* (as it was: at once, and it holds) */
    turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 5);
    turn(ME_ALGORITHM, 2); shift_turn(ME_ALGORITHM, -3);
    turn(ME_SELECT, 2);                             /* (the EQ tab again) */
    press(MK_PLAY);
    audio(2);
    check(sp_ch[SP_NCH + 1].on && sp_ch[SP_NCH + 1].w == &sp_wave[5] && sp_ch[SP_NCH + 1].flt == MASIF_FILTER_HP && !sq.playing,
          "PLAY on the instrument page: the instrument is heard (its sample, through its filter), the song does not start");
    press(MK_PLAY);
    audio(2);
    check(!sp_ch[SP_NCH + 1].on, "PLAY again: it stops");

    /* the table of that instrument: N, V, FX1, FX2 */
    press(MK_OCTUP);
    check(mu.page == MU_TABLE && !strncmp(line(0), "TABLE 00", 8) && !strncmp(line(1), "  N  V  FX1  FX2", 16) && mu_at[0][19] == MU_CURSOR,
          "OCT+ from the instrument: its table; the heads N V FX1 FX2; T marked");
    turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 1);          /* row 1: N + 12 */
    turn(ME_SELECT, 1); shift_turn(ME_SELECT, 1); shift_turn(ME_ALGORITHM, -2);      /* V: the last volume (44), - 0x20 */
    turn(ME_SELECT, 3); shift_turn(ME_SELECT, 1);   /* FX2: the last effect (PAN) */
    turn(ME_SELECT, 1); shift_turn(ME_ALGORITHM, 8);             /* its parameter 80 */
    check(masif_song.table[0][1].transpose == 12 && masif_song.table[0][1].volume == 0x24 && masif_song.table[0][1].fx2 == MASIF_FX_PAN &&
          masif_song.table[0][1].param2 == 0x80 && masif_song.table[0][1].fx == 0 && !strncmp(line(3), "1 0C 24 ---  PAN80", 18),
          "the table page: a row's transpose, volume and second effect, through the edit queue");
    ppm("8-table");

    /* the pages off the row */
    press(MK_HOME);
    check(mu.page == MU_INST, "HOME: back to the page before");
    press(MK_OCTUP); press(MK_OCTUP);
    check(mu.page == MU_SONG, "OCT+ on along S C P I T and round to S");
    press(MK_OCTDN);
    check(mu.page == MU_MIXER && !strncmp(line(0), "MIXER", 5) && !strncmp(line(0) + 15, "MCPIT", 5) && mu_at[0][15] == MU_CURSOR,
          "OCT- from S: down to the mixer, which takes S's place in the page row");
    turn(ME_SELECT, 2); shift_turn(ME_ALGORITHM, -3);            /* track 3: 64 -> 34 */
    turn(ME_SELECT, -1); press(MK_SAVE);            /* track 2: muted */
    turn(ME_SELECT, 4); shift_turn(ME_SELECT, 6);   /* the mix's MID: + 6 */
    check(masif_song.track_level[2] == 0x34 && masif_mute == 2 && sp_trk_mix[1] == 0 && sp_trk_mix[2] == 0x34 && masif_song.master_eq[1] == 6,
          "the mixer: a track's level, a track muted with SAVE, a band of the mix's EQ");
    ppm("9-mixer");
    press(MK_OCTDN);
    check(mu.page == MU_PROJECT && !strncmp(line(0), "PROJECT", 7) && !strncmp(line(0) + 15, "PCPIT", 5),
          "... round through the top: the project, in S's place too");
    turn(ME_ALGORITHM, 2); shift_turn(ME_SELECT, 9);            /* the key: A */
    turn(ME_ALGORITHM, 1); shift_turn(ME_SELECT, 2);            /* the scale: minor */
    turn(ME_ALGORITHM, -2); shift_turn(ME_ALGORITHM, 1);        /* the tempo: + 10 */
    check(masif_song.key == 9 && masif_song.scale == 2 && masif_song.tempo == 170 && sq.bpm10 == 1700,
          "the project page: the key, the scale and the tempo, through the edit queue");
    turn(ME_ALGORITHM, -1); shift_turn(ME_SELECT, 2);           /* the song slot: 3 */
    turn(ME_ALGORITHM, 6); press(MK_SAVE);                      /* SAVE */
    {
        masif_store_info_t info;
        static masif_song_t kept;
        kept = masif_song;
        check(masif_store_info(2, &info) && info.used && !strcmp(info.name, "SONG 03") && !strncmp(line(17), "SAVED", 5),
              "SAVE on the project page: the song into the chosen slot, and the screen says so");
        ppm("10-project-saved");
        turn(ME_ALGORITHM, 1); press(MK_SAVE);                  /* NEW */
        check(masif_song.song[0][0] == MASIF_NONE && masif_song.key == 0, "NEW: an empty song");
        turn(ME_ALGORITHM, -2); press(MK_SAVE);                 /* LOAD */
        check(!memcmp(&kept, &masif_song, sizeof kept), "LOAD: the saved song back, all of it");
        press(MK_PLAY); audio(20);
        press(MK_SAVE);
        check(!strncmp(line(17), "STOP FIRST", 10), "while it plays, LOAD / SAVE / NEW only say STOP FIRST");
        press(MK_PLAY); audio(20);
        fm1_ms += 2000;
        check(line(17)[0] == ' ', "the word at the bottom goes away after a moment");
    }
    press(MK_OCTDN);
    check(mu.page == MU_SONG, "OCT- again: the song");
    ppm("11-song-with-key");

    /* ---- the PRESETS knob: the pages along S C P I T, round in either direction; with shift off the row */
    turn(ME_PRESETS, -1);
    check(mu.page == MU_TABLE, "PRESETS to the left from S: round to T");
    turn(ME_PRESETS, -2);
    check(mu.page == MU_PHRASE, "... and on to the left: I, P");
    turn(ME_PRESETS, 4);
    check(mu.page == MU_CHAIN, "PRESETS to the right: I, T, round to S, C");
    turn(ME_PRESETS, -1);
    shift_turn(ME_PRESETS, 1);
    check(mu.page == MU_MIXER, "shift + PRESETS at S: down to the mixer");
    shift_turn(ME_PRESETS, 1); i = mu.page; shift_turn(ME_PRESETS, 1);
    check(i == MU_PROJECT && mu.page == MU_SONG, "... round through the project to the song");
    shift_turn(ME_PRESETS, -1);
    check(mu.page == MU_PROJECT, "... and the other way: up to the project");
    turn(ME_ALGORITHM, -9); turn(ME_ALGORITHM, 3);  /* the scale's row: EDIT held shows the scales */
    masif_ui_button(MK_EDIT, 1); turn(ME_ALGORITHM, 3);
    shot("14-scale-chooser");
    check(mu.pick && !strncmp(line(0), " SCALE", 6) && !strncmp(line(6), " LYDIAN", 7) && mu_at[6][1] == MU_CURSOR && masif_song.scale == 2,
          "EDIT held on the scale: the list of scales, the knobs go through it, nothing is entered yet");
    masif_ui_button(MK_EDIT, 0); audio(1);
    check(!mu.pick && masif_song.scale == 5, "EDIT let go: the marked scale is the project's");
    shift_turn(ME_SELECT, -5);                       /* (chromatic for what follows) */
    shift_turn(ME_PRESETS, 1);

    /* ---- copy and paste on the song page */
    turn(ME_ALGORITHM, -20); turn(ME_SELECT, -4);
    sel(); turn(ME_SELECT, 1);
    masif_ui_draw();
    check(mu.sel && mu_at[2][3] == AT_SEL && mu_at[2][4] == AT_SEL && mu_at[2][6] == MU_CURSOR && mu_at[3][3] != AT_SEL,
          "shift + option: a selection from where the cursor was to where it is, shown on the page");
    shot("15-song-selection");
    copy();
    check(!mu.sel && !strncmp(line(17), "COPIED", 6), "option: copied, the selection is gone");
    turn(ME_ALGORITHM, 2); turn(ME_SELECT, -1);
    paste();
    check(masif_song.song[2][0] == 0 && masif_song.song[2][1] == 1 && masif_song.song[2][2] == MASIF_NONE && mu.song_row == 3,
          "shift + enter: pasted at the cursor; the cursor is under it");
    turn(ME_SELECT, 2);
    paste();
    check(masif_song.song[3][2] == 0 && masif_song.song[3][3] == 1 && masif_song.song[3][0] == MASIF_NONE,
          "on the song page a paste goes in from the cursor's track on");
    turn(ME_ALGORITHM, -1);                          /* rows 2-3: a selection, grown to all four tracks, cut */
    sel(); turn(ME_ALGORITHM, -1); sel();
    masif_ui_draw();
    check(mu_at[4][3] == AT_SEL && mu_at[5][12] == AT_SEL && mu_at[6][3] != AT_SEL, "shift + option again: the selection takes in every track");
    cut();
    check(masif_song.song[2][0] == MASIF_NONE && masif_song.song[2][1] == MASIF_NONE && masif_song.song[3][3] == MASIF_NONE &&
          masif_song.song[0][1] == 1 && !mu.sel, "option + enter: the selection is cut");
    turn(ME_ALGORITHM, 2); turn(ME_SELECT, -2);
    paste();
    check(masif_song.song[4][0] == 0 && masif_song.song[4][1] == 1 && masif_song.song[5][2] == 0 && masif_song.song[5][3] == 1,
          "... and is in the clipboard: pasted two rows further down");
    turn(ME_ALGORITHM, -1); sel(); turn(ME_ALGORITHM, -1); sel(); cut(); /* (away again) */
    check(masif_song.song[4][0] == MASIF_NONE && masif_song.song[5][3] == MASIF_NONE && masif_song.song[6][0] == MASIF_NONE,
          "a selection of 2 rows x 4 tracks cut again");

    /* ---- the phrase page: a selection's values, a whole phrase, an effect from one column into the other */
    turn(ME_ALGORITHM, -20); turn(ME_SELECT, -4); turn(ME_SELECT, 1);
    press(MK_OCTUP); press(MK_OCTUP);
    turn(ME_ALGORITHM, -20); turn(ME_SELECT, -10);
    sel(); turn(ME_ALGORITHM, 4);
    shift_turn(ME_ALGORITHM, 1);
    check(mu.page == MU_PHRASE && masif_song.phrase[4][0].note == 72 && masif_song.phrase[4][4].note == 75 &&
          masif_song.phrase[4][1].note == MASIF_NOTE_NONE && masif_song.phrase[4][8].note == 51,
          "shift + a knob with a selection: every note in it moves (an octave up), the empty cells stay empty");
    shift_turn(ME_ALGORITHM, -1);
    press(MK_HOME);
    check(!mu.sel && mu.page == MU_PHRASE && masif_song.phrase[4][0].note == 60, "HOME: out of the selection, nothing copied, the page stays");
    sel(); sel();
    masif_ui_draw();
    check(mu_at[2][2] == AT_SEL && mu_at[17][4] == AT_SEL && mu_at[2][5] != AT_SEL, "shift + option twice: the whole column");
    sel();
    masif_ui_draw();
    check(mu_at[2][5] == AT_SEL && mu_at[17][19] == AT_SEL, "... three times: the whole phrase");
    shot("16-phrase-selected");
    copy();
    turn(ME_ALGORITHM, 8);                           /* the cursor was on row 4: row 12 */
    paste();
    check(masif_song.phrase[4][12].note == 60 && masif_song.phrase[4][12].instrument == 0 && masif_song.phrase[4][13].note == MASIF_NOTE_NONE &&
          masif_song.phrase[4][8].note == 51, "a phrase pasted at row C: its rows from there down, as far as the page goes");
    turn(ME_ALGORITHM, -7); turn(ME_SELECT, 3);      /* row 8, effect 1 (RET 13): into effect 2 of row 9 */
    sel(); copy();
    turn(ME_ALGORITHM, 1); turn(ME_SELECT, 3);
    paste();
    check(masif_song.phrase[4][9].fx2 == MASIF_FX_RETRIGGER && masif_song.phrase[4][9].param2 == 0x13 && masif_song.phrase[4][9].fx == 0 &&
          mu.row == 10, "an effect with its parameter copied from the first effect column and pasted into the second");

    /* ---- EDIT held on an effect cell: the effects as a list */
    masif_ui_button(MK_EDIT, 1);
    turn(ME_SELECT, -40); turn(ME_ALGORITHM, 4); turn(ME_SELECT, 2);    /* the first, 4 rows down, 2 on: VIB, the last */
    shot("17-effect-chooser");
    check(mu.pick && !strncmp(line(0), "VIB VIBRATO", 11) && !strncmp(line(2), "--- ARP DED GRV HOP", 19) && !strncmp(line(6) + 8, "VIB", 3) &&
          mu_at[6][8] == MU_CURSOR && !strncmp(line(3), "MTT OFS PAN PBN PRB", 19) && !strncmp(line(5), "SLC SLD SND STP TBL", 19) && !strncmp(line(14), "PITCH VIBRATO", 13) && !strncmp(line(16), "X SPEED  Y DEPTH", 16) &&
          masif_song.phrase[4][10].fx2 == 0,
          "EDIT held on an effect: the whole screen: the effects by their letters, the marked one's name, what it does and its digits");
    turn(ME_PRESETS, 1);
    check(mu.page == MU_PHRASE, "... while it is open the page stays");
    masif_ui_button(MK_EDIT, 0); audio(1);
    check(!mu.pick && masif_song.phrase[4][10].fx2 == MASIF_FX_VIBRATO && masif_song.phrase[4][10].fx == 0, "EDIT let go: the effect is in the cell");
    cut();
    check(masif_song.phrase[4][10].fx2 == 0, "option + enter without a selection: the cursor's cell is empty");
    masif_ui_button(MK_FX, 1);                       /* FX held does the same on an effect column */
    turn(ME_SELECT, -40); turn(ME_SELECT, 18);
    check(mu.pick == PK_FX && !strncmp(line(0), "STP STOP", 8) && !strncmp(line(16), "XX TICKS  00 AT ONCE", 20), "FX held on an effect: the same list (here on STP)");
    masif_ui_button(MK_FX, 0); audio(1);
    check(!mu.pick && masif_song.phrase[4][10].fx2 == MASIF_FX_STOP, "FX let go: the effect is in the cell");
    cut();
    turn(ME_SELECT, -4);                             /* (left of the effect columns: no list there) */
    masif_ui_button(MK_FX, 1);
    check(!mu.pick, "FX held off the effect columns: nothing");
    masif_ui_button(MK_FX, 0);
    turn(ME_SELECT, 4);

    /* ---- the modulation page, above the instrument's */
    turn(ME_ALGORITHM, -20);
    turn(ME_PRESETS, 1);
    shift_turn(ME_PRESETS, -1);
    check(mu.page == MU_MOD && !strncmp(line(0), "MOD", 3) && !strncmp(line(0) + 15, "SCPMT", 5) && mu_at[0][18] == MU_CURSOR &&
          !strncmp(line(6), "INST 00", 7) && !strncmp(line(7), "TYPE    OFF", 11) && !strncmp(line(7) + 15, "1 OFF", 5) && !strncmp(line(13) + 15, "4 OFF", 5),
          "shift + PRESETS at I: the modulation page (M in I's place): the instrument's four slots");
    shift_turn(ME_SELECT, 3);                        /* slot 1: an LFO ... */
    {
        const masif_mod_t *m = &masif_song.instrument[0].mod[0];
        check(m->type == MASIF_MOD_LFO && m->p[0] == MASIF_LFO_TRI && m->p[1] == MASIF_TRIG_FREE && m->p[2] == 7 && m->p[3] == 1 && !strncmp(line(10), "SHAPE   TRI", 11) &&
              !strncmp(line(12), "RATE    1/4", 11), "a slot's type set: its settings appear, as that type usually has them");
        turn(ME_ALGORITHM, 1); shift_turn(ME_SELECT, 4);         /* ... on the cutoff, */
        turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 2);      /* by + 32 */
        turn(ME_ALGORITHM, 1);
        masif_ui_button(MK_EDIT, 1);
        turn(ME_SELECT, 6);
        shot("18-mod-shape-chooser");
        check(mu.pick && !strncmp(line(6), " SHAPE", 6) && !strncmp(line(7), " TRI     SINE", 13) && !strncmp(line(10), " SQU DN", 7) && mu_at[10][1] == MU_CURSOR,
              "EDIT held on the LFO's shape: the shapes as a list, under the picture");
        masif_ui_button(MK_EDIT, 0); audio(1);
        turn(ME_ALGORITHM, 1); shift_turn(ME_SELECT, 1);         /* TRIG: RETRIG */
        check(m->dest == MASIF_DEST_CUTOFF && m->amount == 32 && m->p[0] == MASIF_LFO_SQU_DOWN && m->p[1] == MASIF_TRIG_RETRIG &&
              !strncmp(line(8), "DEST    CUTOFF", 14) && !strncmp(line(9), "AMOUNT  +32", 11) && !strncmp(line(8) + 15, "CUT", 3),
              "the LFO's destination, amount, shape and trigger; the slot's column says where it goes");
        shot("19-mod-lfo");
        check(px(30, 23, MU_INK) && px(30, 70, MU_DIM) && px(90, 77, MU_INK) && px(90, 23, MU_DIM),
              "the picture is the LFO's shape: a square, up first");
        press(MK_PLAY);
        {
            int32_t lo = 999, hi = -1;
            for (i = 0; i < 600u; i++) {
                audio(1);
                if (sp_ch[SP_NCH + 1].cut < lo) lo = sp_ch[SP_NCH + 1].cut;
                if (sp_ch[SP_NCH + 1].cut > hi) hi = sp_ch[SP_NCH + 1].cut;
            }
            check(sp_ch[SP_NCH + 1].on && lo == 0x4F - 63 && hi == 127, "PLAY: the instrument sounds and the LFO moves its filter's cutoff, both ways from its own");
        }
        press(MK_PLAY);
        turn(ME_SELECT, 1); turn(ME_ALGORITHM, -9); turn(ME_ALGORITHM, 1);   /* slot 2, its first row: an envelope on the pitch */
        shift_turn(ME_SELECT, 1);
        turn(ME_ALGORITHM, 1); shift_turn(ME_SELECT, 2);
        turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, -2);
        turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 2);      /* ATTACK 20 */
        turn(ME_ALGORITHM, 1); shift_turn(ME_ALGORITHM, 1);      /* HOLD 10 */
        m = &masif_song.instrument[0].mod[1];
        shot("20-mod-envelope");
        check(m->type == MASIF_MOD_AHD && m->dest == MASIF_DEST_PITCH && m->amount == -32 && m->p[0] == 0x20 && m->p[1] == 0x10 && m->p[2] == 40 &&
              !strncmp(line(7), "TYPE    AHD", 11) && !strncmp(line(12), "DECAY   28", 10) && !strncmp(line(9) + 15, "2 AHD", 5) && !strncmp(line(10) + 15, "PITCH", 5) &&
              mu_at[9][15] == MU_CURSOR && px(4, 60, MU_DIM) && px(22, 30, MU_MID) && px(200, 30, MU_DIM),
              "slot 2: an attack / hold / decay envelope on the pitch, and its picture");
        {
            uint32_t s0, s1, s2;
            press(MK_PLAY); audio(1);
            s0 = sp_ch[SP_NCH + 1].step;
            audio(200); s1 = sp_ch[SP_NCH + 1].step;
            audio(2000); s2 = sp_ch[SP_NCH + 1].step;
            press(MK_PLAY);
            check(s1 < s0 * 3u / 4u && s2 >= s0 && s2 - s0 < s0 / 100u && s2 == 26040u * 65536u / SP_FS, "PLAY: the envelope bends the note's pitch down and lets it back");
        }
    }

    /* ---- an instrument copied to another: the cursor up onto the instrument's number, another instrument */
    sel();
    masif_ui_draw();
    check(mu.sel && mu_at[6][0] == AT_SEL && mu_at[6][6] == AT_SEL, "shift + option on these pages: the whole instrument is selected");
    copy();
    turn(ME_ALGORITHM, -9);
    shift_turn(ME_SELECT, 1);
    check(mu.inum && mu.inst == 1 && !strncmp(line(6), "INST 01", 7) && mu_at[6][5] == MU_CURSOR && !strncmp(line(7) + 15, "1 OFF", 5),
          "the cursor above the first setting is on the instrument's number: turned, the page is on another instrument");
    paste();
    check(!memcmp(&masif_song.instrument[0], &masif_song.instrument[1], sizeof(masif_instrument_t)) && masif_song.instrument[1].mod[0].type == MASIF_MOD_LFO &&
          !strncmp(line(7) + 15, "1 LFO", 5), "shift + enter: the copied instrument, all of it, is this one too");
    sel(); cut();
    {
        masif_instrument_t def;
        masif_instrument_default(&def, 1);
        check(!memcmp(&def, &masif_song.instrument[1], sizeof def) && masif_song.instrument[0].mod[0].type == MASIF_MOD_LFO,
              "option + enter with the instrument selected: it is as an empty song has it");
    }
    shift_turn(ME_SELECT, -1);
    turn(ME_ALGORITHM, 2); cut();
    check(masif_song.instrument[0].mod[1].dest == MASIF_DEST_OFF && masif_song.instrument[0].mod[1].type == MASIF_MOD_AHD,
          "... without a selection: the setting under the cursor is as an empty song has it");
    check(masif_edits.dropped == 0, "no edit was lost on the way to the audio side");

    /* ---- the modulation itself (masif_fm1_adapter.c), slot by slot */
    {
        masif_mod_t m = {MASIF_MOD_LFO, MASIF_DEST_PITCH, 64, {0, MASIF_TRIG_RETRIG, 64, 0}};
        masif_mod_state_t st;
        masif_instrument_t in;
        int32_t v, lo, hi, ok = 1, changes = 0, wraps = 0, last;
        uint32_t k, sh;
        for (sh = 0; sh < MASIF_LFO_SHAPES; sh++) {          /* every shape, a few cycles */
            memset(&st, 0, sizeof st);
            m.p[0] = (uint8_t)sh;
            lo = 999; hi = -999;
            for (k = 0; k < 3000u; k++) { v = masif_mod_run(&m, &st, 0, 1); if (v < lo) lo = v; if (v > hi) hi = v; }
            ok &= lo >= -256 && hi <= 256 && hi - lo > 300;
        }
        check(ok, "every LFO shape stays within its range and swings through most of it");
        memset(&st, 0, sizeof st);
        m.p[0] = MASIF_LFO_RANDOM; last = 9999;
        for (k = 0; k < 3000u; k++) {
            uint16_t was = st.phase;
            v = masif_mod_run(&m, &st, 0, 1);
            if (v != last) changes++;
            if (st.phase < was) wraps++;
            last = v;
        }
        check(wraps > 5 && changes <= wraps + 1 && changes >= wraps / 2, "RANDOM: a new value a cycle, held between");
        {   /* SYNC: the rate is a length in the tempo. A quarter at 120 BPM is 689.1 blocks: 100 cycles in 68910 */
            uint16_t bpm = sq.bpm10;
            uint32_t w120 = 0, w60 = 0, wbar = 0;
            m.p[0] = MASIF_LFO_RAMP_UP; m.p[2] = 7; m.p[3] = 1;
            memset(&st, 0, sizeof st);
            sq.bpm10 = 1200;
            for (k = 0; k < 68910u; k++) { uint16_t was = st.phase; (void)masif_mod_run(&m, &st, 0, 1); w120 += st.phase < was; }
            sq.bpm10 = 600;
            for (k = 0; k < 68910u; k++) { uint16_t was = st.phase; (void)masif_mod_run(&m, &st, 0, 1); w60 += st.phase < was; }
            m.p[2] = 3; sq.bpm10 = 1200;            /* a bar: 4 quarters */
            for (k = 0; k < 68910u; k++) { uint16_t was = st.phase; (void)masif_mod_run(&m, &st, 0, 1); wbar += st.phase < was; }
            sq.bpm10 = bpm;
            m.p[2] = 64; m.p[3] = 0;
            check(w120 >= 99u && w120 <= 100u && w60 >= 49u && w60 <= 50u && wbar >= 24u && wbar <= 25u,
                  "a synced LFO: a quarter's cycle is a quarter long at any tempo, a bar's a bar");
        }
        masif_instrument_default(&in, 2);
        in.sample = 5; in.play = MASIF_PLAY_LOOP;
        in.mod[0] = m; in.mod[0].p[0] = MASIF_LFO_RAMP_UP; in.mod[0].p[1] = MASIF_TRIG_ONCE;
        in.mod[1] = m; in.mod[1].p[0] = MASIF_LFO_RAMP_UP; in.mod[1].p[1] = MASIF_TRIG_HOLD;
        in.mod[2] = m; in.mod[2].p[0] = MASIF_LFO_RAMP_UP; in.mod[2].p[1] = MASIF_TRIG_FREE;
        in.mod[0].dest = in.mod[1].dest = in.mod[2].dest = MASIF_DEST_OFF;
        check(masif_set_instrument(&masif_song, 2, &in), "an instrument with three LFOs: once, held, free");
        masif_mod[0][1].phase = 0x4000; masif_mod[0][2].phase = 0x4000;
        masif_fm1_preview(0, 2, 60, 127); audio(1);
        {
            int32_t held = masif_mod[0][1].value;
            uint16_t free0 = masif_mod[0][2].phase;
            audio(400);
            check(masif_mod[0][0].stage == 1 && masif_mod[0][0].phase == 0xFFFF && masif_lfo_now(&in.mod[0], &masif_mod[0][0]) > 250,
                  "ONCE: one cycle from the note's start, then it stands at its end");
            check(held > -140 && held < -110 && masif_mod[0][1].value == held && masif_mod_run(&in.mod[1], &masif_mod[0][1], 0, 1) == held,
                  "HOLD: what the LFO was when the note started, for the whole note");
            check(free0 >= 0x4000 && free0 < 0x4000 + 2000 && masif_mod[0][2].stage == 0, "FREE: the note's start does not move it");
        }
        /* envelopes: an AHD on the level (as a VCA), an ADSR on the cutoff, and a slot on the next one's amount */
        masif_instrument_default(&in, 2);
        in.sample = 5; in.play = MASIF_PLAY_LOOP; in.filter = MASIF_FILTER_LP; in.cutoff = 20; in.release = 100;
        in.mod[0].type = MASIF_MOD_AHD; in.mod[0].dest = MASIF_DEST_VOLUME; in.mod[0].amount = 64;
        in.mod[0].p[0] = 30; in.mod[0].p[1] = 30; in.mod[0].p[2] = 30;
        in.mod[1].type = MASIF_MOD_ADSR; in.mod[1].dest = MASIF_DEST_CUTOFF; in.mod[1].amount = 64;
        in.mod[1].p[0] = 0; in.mod[1].p[1] = 20; in.mod[1].p[2] = 64; in.mod[1].p[3] = 20;
        in.mod[2].type = MASIF_MOD_AHD; in.mod[2].dest = MASIF_DEST_MOD_AMT; in.mod[2].amount = 64;
        in.mod[2].p[0] = 40; in.mod[2].p[1] = 127; in.mod[2].p[2] = 0;
        in.mod[3] = m; in.mod[3].p[0] = MASIF_LFO_SQU_DOWN; in.mod[3].dest = MASIF_DEST_PAN; in.mod[3].amount = 64;
        check(masif_set_instrument(&masif_song, 2, &in), "an instrument with two envelopes, and a third on the amount of an LFO on the pan");
        masif_fm1_preview(0, 2, 60, 127); audio(1);
        {
            const sp_ch_t *c = &sp_ch[SP_NCH];
            int32_t g0 = c->gl + c->gr, cut0 = c->cut, g1, cut1, g2, cut2, pan0 = c->gl - c->gr, pan1 = 0;
            audio(1 + 30 * 30 * 3445 / 16129 + 20);
            g1 = c->gl + c->gr; cut1 = c->cut;
            for (k = 0; k < 400u; k++) { audio(1); if (c->gl - c->gr > pan1) pan1 = c->gl - c->gr; if (c->gr - c->gl > pan1) pan1 = c->gr - c->gl; }
            g2 = c->gl + c->gr; cut2 = c->cut;
            check(g0 == 0 && g1 > 2000 && g2 == 0, "an envelope on the level: from silence up with its attack, held, and down again");
            check(cut0 == 127 && cut1 < 127 && cut2 == 20 + 129 * 64 * 127 / 16384 && c->on,   /* (sustain 64 of 127 is 129 of 256) */
                  "an ADSR on the cutoff: the filter opens at the note's start and closes to the sustain level");
            check(pan0 == 0 && pan1 > 2000, "an envelope on the next slot's amount: the LFO's pan swing fades in");
            masif_fm1_preview(0, 2, 0, 0); audio(120);
            check(c->on && c->stage == 2u && c->cut == 20, "NOTE OFF: while the note's own release fades it, the ADSR's takes the cutoff back to the instrument's own");
        }
        masif_fm1_preview(0, 2, 60, 0); audio(2);
        masif_instrument_default(&in, 2);
        (void)masif_set_instrument(&masif_song, 2, &in);
    }

    /* ---- the groove page, above the table page: a groove's steps, the sums, copy and paste to another groove */
    masif_song_clear(&masif_song);
    masif_song.instrument[5].sample = 5;
    masif_fm1_song_changed();
    masif_ui_init();
    press(MK_OCTUP); press(MK_OCTUP); press(MK_OCTUP); press(MK_OCTUP);
    shift_turn(ME_PRESETS, 1);
    check(mu.page == MU_GROOVE && !strncmp(line(0), "GROOVE 0", 8) && !strncmp(line(0) + 15, "SCPIG", 5) && mu_at[0][19] == MU_CURSOR &&
          !strncmp(line(2), "0  06", 5) && !strncmp(line(4), "2  --", 5) && !strncmp(line(3) + 8, "PHRASE  096", 11),
          "shift + PRESETS on the table page: the groove page, G in T's place; an empty song's groove 06 06");
    shift_turn(ME_SELECT, 2); turn(ME_ALGORITHM, 1); shift_turn(ME_SELECT, -2);
    turn(ME_ALGORITHM, 1); shift_turn(ME_SELECT, 1);                  /* an empty step begins with 06 */
    check(masif_song.groove[0][0] == 8 && masif_song.groove[0][1] == 4 && masif_song.groove[0][2] == 6 && !strncmp(line(2) + 8, "STEPS   018", 11) &&
          !strncmp(line(3) + 8, "PHRASE  098", 11), "the steps turned through the edit queue: 08 04 06, their sum and a phrase's 16 rows");
    shot("21-groove");
    cut();
    check(masif_song.groove[0][2] == 0xFF, "option + enter: the step is empty again");
    sel(); sel(); copy();                           /* the whole groove */
    turn(ME_ALGORITHM, -5);
    check(mu.gnum && !strncmp(line(0), "GROOVE 0", 8) && mu_at[0][7] == MU_CURSOR, "up from the first step: the cursor is on the groove's number");
    shift_turn(ME_SELECT, 3);
    turn(ME_ALGORITHM, 1);
    paste(); audio(2);
    check(mu.groove == 3 && masif_song.groove[3][0] == 8 && masif_song.groove[3][1] == 4 && masif_song.groove[3][2] == 0xFF && masif_song.groove[1][0] == 6,
          "shift + a knob there: another groove; the copied groove pasted into it");
    shift_turn(ME_PRESETS, 1);
    check(mu.page == MU_TABLE, "shift + PRESETS again: back on the table page");
    mu.page = MU_PROJECT; mu.prow = MJ_GROOVE;      /* the project's groove: the one every track starts on */
    shift_turn(ME_SELECT, 3); audio(1);
    check(masif_song.project_groove == 3 && !strncmp(line(6), "GROOVE  03", 10) && mu_at[6][8] == MU_CURSOR, "the project page: GROOVE, through the edit queue");
    shot("22-project-groove");
    press(MK_PLAY); audio(8);
    check(sq.playing && masif_runtime.groove[0] == 3 && masif_runtime.groove[3] == 3 && masif_runtime.row_len[0] == 32, "PLAY: every track is on the project's groove (here 08 04)");
    press(MK_PLAY); audio(4);
    {   /* an instrument's DRIVE (the AMP tab): into the voice's soft clip with its note; an LFO slot begins synced */
        const sp_ch_t *v = &sp_ch[SP_NCH];
        mu.page = MU_INST; mu.inst = 5; mu.itab = MI_AMP; mu.irow = 6; mu.inum = 0;
        check(!strncmp(line(13), "DRIVE   00", 10) && mu_at[13][8] == MU_CURSOR, "the AMP tab: DRIVE under the envelope's settings");
        shift_turn(ME_ALGORITHM, 4); audio(1);
        masif_fm1_preview(0, 5, 60, 127); audio(2);
        check(masif_song.instrument[5].drive == 0x40 && v->drv == 16 + 0x40, "DRIVE 40: the instrument's notes go through the clip, 5 times as loud into it");
        masif_fm1_preview(0, 0, 0, 0); audio(2);
        mu.page = MU_MOD; mu.mslot = 0; mu.mrow = 0;
        shift_turn(ME_SELECT, 3); audio(1);          /* TYPE: LFO */
        check(masif_song.instrument[5].mod[0].type == MASIF_MOD_LFO && masif_song.instrument[5].mod[0].p[3] == 1 &&
              !strncmp(line(12), "RATE    1/4", 11) && !strncmp(line(13), "SYNC    ON", 10), "a slot that becomes an LFO: synced, a quarter long");
        shot("23-lfo-synced");
        mu.mrow = 6; shift_turn(ME_SELECT, -1); audio(1);
        check(masif_song.instrument[5].mod[0].p[3] == 0 && masif_song.instrument[5].mod[0].p[2] == 40 && !strncmp(line(12), "RATE    28", 10),
              "SYNC off: the rate is the free one again (Hz)");
    }

    {   /* the sample pool (EDIT held on SAMPLE or the instrument's number): hear one, give it to the page's instrument */
        uint32_t t, n = 0, w;
        uint8_t inst5[sizeof(masif_instrument_t)];
        sp_wave[6].d = wave; sp_wave[6].n = 2000; sp_wave[6].rate = 26040;            /* a sample with its own loop */
        sp_wave[6].lp_a = 500; sp_wave[6].lp_b = 1500; sp_wave[6].lp_play = MASIF_PLAY_PING;
        for (w = 0; w < MASIF_SAMPLES; w++) n += sp_wave[w].n != 0;
        memcpy(inst5, &masif_song.instrument[5], sizeof inst5);
        mu.page = MU_INST; mu.inst = 5; mu.itab = MI_WAVE; mu.irow = 0; mu.inum = 0; mu.wave_drawn = 0;
        masif_ui_button(MK_EDIT, 1); audio(1);
        check(mu.pick == PK_POOL && mu.pick_n == n && mu_pool_wave(mu.pick_cur) == 5 && !strncmp(line(0), "SAMPLE POOL", 11),
              "EDIT held on SAMPLE: the pool, every sample there is, the instrument's own marked");
        turn(ME_SELECT, 1);
        press(MK_PLAY); audio(2);
        check(mu_pool_wave(mu.pick_cur) == 6 && sp_ch[SP_NCH + mu.track].on && sp_ch[SP_NCH + mu.track].w == &sp_wave[6] && !sp_ch[SP_NCH + mu.track].looped,
              "PLAY in the pool: the marked sample heard as it is, once");
        shot("24-sample-pool");
        masif_ui_button(MK_EDIT, 0); audio(2);
        check(mu.inst == 5 && masif_song.instrument[5].sample == 6 && masif_song.instrument[5].play == MASIF_PLAY_PING &&
              (masif_song.instrument[5].flags & MASIF_INST_SMPLOOP), "letting EDIT go: the page's instrument gets the sample (its own replaced), its loop and play mode");
        mu.itab = MI_LOOP; mu.irow = 0;
        check(!strncmp(line(7), "LOOP    SMP", 11) && !strncmp(line(8), "LP END  SMP", 11), "LOOP and LP END: the sample's own (SMP)");
        shot("25-pool-assigned");
        masif_fm1_preview(0, 5, masif_song.instrument[5].root_note, 127); audio(2);
        check(sp_ch[SP_NCH].looped == 2 && sp_ch[SP_NCH].loop == 500 && sp_ch[SP_NCH].lend == 1500, "its note loops at the sample's own points, to the sample");
        masif_fm1_preview(0, 0, 0, 0); audio(2);
        shift_turn(ME_SELECT, 1); audio(1);
        check(!(masif_song.instrument[5].flags & MASIF_INST_SMPLOOP) && masif_song.instrument[5].loop_start == 65 && masif_song.instrument[5].loop_end == 192,
              "LOOP turned: the instrument's own loop from the sample's, as near as 1/256 (40 + 1, C0)");
        mu.itab = MI_WAVE; mu.irow = 0;
        masif_ui_button(MK_EDIT, 1); turn(ME_SELECT, -1); press(MK_HOME); masif_ui_button(MK_EDIT, 0); audio(2);
        check(mu.inst == 5 && masif_song.instrument[5].sample == 6, "HOME in the pool: nothing changes");
        /* another instrument number on the page: an empty one is a new INIT, waiting for the pool */
        mu.inum = 1; shift_turn(ME_ALGORITHM, 2); audio(1);          /* (to 0x25) */
        t = mu.inst;
        check(t == 0x25 && mu_inst_empty(t) && !strncmp(line(0), "INIT EDIT:POOL", 14), "INST 25: empty, the page says INIT and asks for the pool");
        mu.inum = 0;
        check(!strncmp(line(7), "SAMPLE  --", 10) && !strncmp(line(8), "START   00", 10), "... SAMPLE --, the rest as a new instrument has it");
        shot("26-init");
        masif_ui_button(MK_EDIT, 1); turn(ME_SELECT, -5); masif_ui_button(MK_EDIT, 0); audio(2);
        check(mu.inst == t && masif_song.instrument[t].sample == 5 && masif_song.instrument[t].play == MASIF_PLAY_FWD &&
              !(masif_song.instrument[t].flags & MASIF_INST_SMPLOOP) && masif_song.instrument[5].sample == 6, "the pool on it: the sample goes to it, 05 keeps its own");
        /* from a phrase row without an instrument: the page opens on the first empty one */
        mu.page = MU_PHRASE; mu.phrase = 0x30; mu.row = 0; mu.pcol = MP_NOTE;
        for (w = 0; masif_song.instrument[w].sample != MASIF_NO_SAMPLE; w++) {}
        press(MK_SAVE);
        check(mu.page == MU_INST && mu.inst == w && w != 5 && w != t, "into the sampler from a row without an instrument: the first empty instrument");
        mu.inum = 0; mu.itab = MI_SEND; mu.irow = 4; mu.inst = 5;
        masif_song.instrument[5].flags = MASIF_INST_SMPLOOP;
        shift_turn(ME_SELECT, 1); audio(1);
        check(masif_song.instrument[5].flags == (MASIF_INST_SMPLOOP | MASIF_INST_FREE), "SCALE turns bit 0 alone (the sample-loop bit stays)");
        memcpy(&masif_song.instrument[5], inst5, sizeof inst5);
        mu.itab = MI_WAVE; mu.irow = 0;
    }

    /* the demo song, playing: the song page marks where each track is */
    (void)masif_demo_load(&masif_song);
    for (i = 0; i < MASIF_DEMO_INSTRUMENTS; i++) masif_song.instrument[i].sample = 63;   /* (no waves here: silent) */
    masif_fm1_song_changed();
    masif_ui_init();
    press(MK_PLAY);
    audio(64 * 24 * 7 + 100);                       /* into the second song row */
    ppm("12-demo-playing");
    check(sq.playing && masif_runtime.tracker.song_row[0] == 1 && (mu_at[3][3] >> 2) == MU_DIM && (mu_at[3][6] >> 2) == MU_DIM && (mu_at[2][6] >> 2) == MU_BG,
          "the song page while the demo plays: the cells the tracks are on are marked");
    for (i = 1; i < MU_NPALETTE; i++) {             /* the other palettes */
        char nm[32];
        mu.palette = (uint8_t)i; mu.all = 1;
        snprintf(nm, sizeof nm, "13-palette-%u", (unsigned)i + 1u);
        ppm(nm);
    }
    puts(bad ? "MASIF screen tests: FAIL" : "MASIF screen tests: PASS");
    return bad ? 1 : 0;
}
