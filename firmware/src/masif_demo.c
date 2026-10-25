/* MASIF demo song, built in with MASIF_DEMO_SONG=1 (masif_fm1_adapter.c loads it at power-on): the tracker made
 * audible while there is no tracker UI and no song storage; a test fixture, not part of a release (it needs the
 * factory kit's waves, tools/gen_kit.py). Two song rows of chains of four phrases (4 bars of 16ths a chain), round
 * and round: Am / F / C / G. A track's part p (0, 1) is chain p * 4 + track, its phrases (p * 4 + track) * 4 + 0..3.
 * Portable: tracker data only, written before the audio runs. Notes are MIDI numbers (60 = C4).
 *   track 1  BASS    roots and octaves, the octave sliding back down (SLD); a PBN dive and a NOTE CUT at the
 *                    end of bar 4
 *   track 2  VIBES   the bar's triad as an ARPEGGIO; in step 2 as a table (TBL: its rows transpose the note)
 *   track 3  PIANO   a line with MICRO TIMING, the volume column and VIBRATO on each bar's last note (step 2: each
 *                    note and its echo)
 *   track 4  OHAT    cut short by the KILL effect; CLAVE, left and right bar by bar (PAN: the hat follows it),
 *                    and a RET roll fading out in bar 4 */
#include "masif_tracker.h"

/* the instruments, and the factory kit's waves they play (KIT_WAVE's order) */
enum { MASIF_DEMO_BASS, MASIF_DEMO_VIBES, MASIF_DEMO_PIANO, MASIF_DEMO_CLAVE, MASIF_DEMO_OHAT, MASIF_DEMO_INSTRUMENTS };
#define MASIF_DEMO_TABLE 0x40u                      /* the minor triad; + 1: the major */
#define MASIF_DEMO_STEPS 2u
#define MASIF_DEMO_KEEP 0xFFu                       /* instrument / volume unchanged */

typedef struct { uint8_t row, note, volume, delay; } masif_demo_note_t;
static const masif_demo_note_t masif_demo_line[] = {
    {4, 76, 127, 0}, {7, 74, 100, 12}, {8, 72, 127, 0}, {12, 69, 110, 0},
    {20, 72, 127, 0}, {23, 69, 100, 12}, {24, 65, 127, 0}, {28, 69, 110, 0},
    {36, 67, 127, 0}, {39, 64, 100, 12}, {40, 67, 127, 0}, {44, 72, 110, 0},
    {52, 74, 127, 0}, {54, 71, 110, 0}, {56, 67, 127, 0}, {60, 74, 90, 0},
};
static const masif_demo_note_t masif_demo_echo[] = {
    {0, 69, 127, 0}, {2, 69, 50, 0}, {6, 72, 127, 0}, {8, 72, 50, 0}, {12, 76, 127, 0}, {14, 76, 50, 0},
    {16, 77, 127, 0}, {18, 77, 50, 0}, {22, 72, 127, 0}, {24, 72, 50, 0}, {28, 69, 127, 0},
    {32, 76, 127, 0}, {34, 76, 50, 0}, {38, 79, 127, 0}, {40, 79, 50, 0}, {44, 76, 127, 0},
    {48, 74, 127, 0}, {50, 74, 50, 0}, {54, 71, 127, 0}, {56, 71, 50, 0}, {60, 67, 127, 0},
};

static int masif_demo_cell(masif_song_t *song, uint32_t track, uint32_t pattern, uint32_t row, uint32_t note,
                           uint32_t inst, uint32_t volume, uint32_t fx, uint32_t param)
{
    masif_cell_t c;
    c.note = (uint8_t)note;
    c.instrument = (uint8_t)inst;
    c.volume = (uint8_t)volume;
    c.fx = (uint8_t)fx;
    c.param = (uint8_t)param;
    c.fx2 = c.param2 = 0;
    return masif_set_cell(song, (uint8_t)((pattern * 4u + track) * 4u + row / MASIF_ROWS), (uint8_t)(row % MASIF_ROWS), &c);
}

/* sample, root note (the note at which it sounds as recorded), fine, volume, pan, ... decay, sends, table tic;
 * then attack, sustain, release, filter, cutoff, resonance, the EQ, the modulation slots */
static const masif_instrument_t masif_demo_instrument[MASIF_DEMO_INSTRUMENTS] = {
    {20, 45, 0, 100, 0, MASIF_PLAY_FWD, 0, 0, 0, 0, 110, {0, 0, 0}, 1, 0, 0, 0, 0, MASIF_FILTER_LP, 80, 40, {24, 0, 0}, {{0}}, 0, 0, 16, 0, 0, {0}},   /* BASS: A2, low-passed, +4.5 dB of low */
    {19, 60, 0, 100, -10, MASIF_PLAY_FWD, 0, 0, 0, 0, 110, {30, 20, 40}, 2, 0, 0, 0, 0, 0, 127, 0, {0, 0, 0},
     {{MASIF_MOD_LFO, MASIF_DEST_VOLUME, 20, {MASIF_LFO_TRI, MASIF_TRIG_RETRIG, 40, 0}}}, 0, 0, 16, 0, 0, {0}},   /* VIBES: C4; its tables 2 ticks a row; a tremolo */
    {26, 65, -37, 100, 0, MASIF_PLAY_FWD, 0, 0, 0, 0, 110, {10, 0, 30}, 1, 0, 20, 0, 0, 0, 127, 0, {0, 0, 0}, {{0}}, 0, 0, 16, 0, 0, {0}},    /* PIANO: stored for 45 -> 33, so
                                                                                     * as it is 5.37 semitones up */
    {14, 60, 0, 90, 12, MASIF_PLAY_FWD, 0, 0, 0, 0, 100, {0, 0, 0}, 1, 0, 0, 0, 0, 0, 127, 0, {0, 0, 0}, {{0}}, 0, 0, 16, 0, 0, {0}},        /* CLAVE (unpitched: at its own) */
    {5, 60, 0, 80, 18, MASIF_PLAY_FWD, 0, 0, 0, 0, 100, {0, 0, 15}, 1, 0, 0, 0, 0, MASIF_FILTER_HP, 90, 0, {0, 0, 16}, {{0}}, 0, 0, 16, 0, 0, {0}},        /* OHAT */
};

/* an ARPEGGIO of `rows` rows from `row`: the note on the first, the effect alone on the others */
static int masif_demo_arp(masif_song_t *song, uint32_t pattern, uint32_t row, uint32_t rows, uint32_t note,
                          uint32_t chord)
{
    uint32_t i;
    int ok = 1;
    for (i = 0; i < rows; i++)
        ok &= masif_demo_cell(song, 1, pattern, row + i, i ? MASIF_NOTE_NONE : note,
                              i ? MASIF_DEMO_KEEP : MASIF_DEMO_VIBES, i ? MASIF_DEMO_KEEP : 110u, MASIF_FX_ARPEGGIO, chord);
    return ok;
}

/* 1: the whole song is in. The song is cleared first. */
/* (minsize: it runs once at power-on, and the factory-kit build that carries it has no bytes to spare) */
MASIF_SMALL int masif_demo_load(masif_song_t *song)
{
    static const uint8_t root[4] = {45, 41, 48, 43};            /* A2, F2, C3, G2 */
    uint32_t bar, i;
    int ok = 1;
    if (!song) return 0;
    masif_song_clear(song);
    for (i = 0; i < MASIF_DEMO_INSTRUMENTS; i++)
        ok &= masif_set_instrument(song, (uint8_t)i, &masif_demo_instrument[i]);
    for (i = 0; i < 2u; i++) {                                  /* the triads as tables: root, third, fifth, octave */
        static const int8_t up[2][4] = {{0, 3, 7, 12}, {0, 4, 7, 12}};
        masif_table_cell_t tc = {0, 0xFF, 0, 0, 0, 0};
        for (bar = 0; bar < 4u; bar++) {
            tc.transpose = up[i][bar];
            tc.fx = bar == 3u ? MASIF_FX_TABLE_HOP : 0;         /* (and round again from row 0) */
            ok &= masif_set_table_cell(song, (uint8_t)(MASIF_DEMO_TABLE + i), (uint8_t)bar, tc);
        }
    }
    for (bar = 0; bar < 4u; bar++) {
        uint32_t r = bar * 16u, n = root[bar];
        uint32_t chord = bar ? 0x47u : 0x37u;                   /* a minor triad on the A, major on the others */
        ok &= masif_demo_cell(song, 0, 0, r, n, MASIF_DEMO_BASS, 127, 0, 0);
        ok &= masif_demo_cell(song, 0, 0, r + 6u, n + 12u, MASIF_DEMO_BASS, 100, 0, 0);
        ok &= masif_demo_cell(song, 0, 0, r + 10u, n, MASIF_DEMO_BASS, MASIF_DEMO_KEEP, MASIF_FX_SLIDE, 24);
        ok &= masif_demo_arp(song, 0, r, 4, n + 24u, chord);
        ok &= masif_demo_arp(song, 0, r + 8u, 2, n + 24u, chord);
        for (i = 0; i < 16u; i += 6u)                           /* step 2: the table arpeggiates, two tracker ticks a note */
            ok &= masif_demo_cell(song, 1, 1, r + i, n + 24u, MASIF_DEMO_VIBES, 110, MASIF_FX_SET_TABLE,
                                  MASIF_DEMO_TABLE + (bar ? 1u : 0u));
        ok &= masif_demo_cell(song, 3, 0, r + 2u, 60, MASIF_DEMO_OHAT, 90, MASIF_FX_KILL, 3);
        ok &= masif_demo_cell(song, 3, 0, r + 7u, 60, MASIF_DEMO_CLAVE, 90, MASIF_FX_PAN, bar & 1u ? 208 : 48);
        for (i = 13; i < 15u && bar < 3u; i++)                  /* (bar 4's last note is cut by the next bar's) */
            ok &= masif_demo_cell(song, 2, 0, r + i, MASIF_NOTE_NONE, MASIF_DEMO_KEEP, MASIF_DEMO_KEEP, MASIF_FX_VIBRATO, 0x65);
        ok &= masif_demo_cell(song, 3, 0, r + 10u, 60, MASIF_DEMO_OHAT, 90, MASIF_FX_KILL, 3);
    }
    ok &= masif_demo_cell(song, 0, 0, 60, 55, MASIF_DEMO_BASS, 127, MASIF_FX_PITCH_BEND, 0x0F);
    ok &= masif_demo_cell(song, 0, 0, 61, MASIF_NOTE_CUT, MASIF_DEMO_KEEP, MASIF_DEMO_KEEP, 0, 0);
    ok &= masif_demo_cell(song, 3, 0, 60, 60, MASIF_DEMO_CLAVE, 127, MASIF_FX_RETRIGGER, 0x12);   /* the roll: a hit every */
    for (i = 1; i < 4u; i++)                                    /* two tracker ticks (3 a row), each softer */
        ok &= masif_demo_cell(song, 3, 0, 60u + i, MASIF_NOTE_NONE, MASIF_DEMO_KEEP, MASIF_DEMO_KEEP, MASIF_FX_RETRIGGER, 0x12);
    for (i = 0; i < sizeof masif_demo_line / sizeof masif_demo_line[0]; i++) {
        const masif_demo_note_t *m = &masif_demo_line[i];
        ok &= masif_demo_cell(song, 2, 0, m->row, m->note, MASIF_DEMO_PIANO, m->volume,
                              m->delay ? MASIF_FX_MICROTIME : MASIF_FX_NONE, m->delay);
    }
    for (i = 0; i < sizeof masif_demo_echo / sizeof masif_demo_echo[0]; i++) {
        const masif_demo_note_t *m = &masif_demo_echo[i];
        ok &= masif_demo_cell(song, 2, 1, m->row, m->note, MASIF_DEMO_PIANO, m->volume, 0, 0);
    }
    for (i = 0; i < 8u; i++)                                    /* the eight chains: four phrases each */
        for (bar = 0; bar < 4u; bar++)
            ok &= masif_set_chain_step(song, (uint8_t)i, (uint8_t)bar, (uint8_t)(i * 4u + bar), 0);
    for (i = 0; i < MASIF_DEMO_STEPS; i++) {                    /* song row 2: tracks 2 and 3 play their second part */
        ok &= masif_set_song_cell(song, (uint8_t)i, 0, 0);
        ok &= masif_set_song_cell(song, (uint8_t)i, 1, (uint8_t)(i * 4u + 1u));
        ok &= masif_set_song_cell(song, (uint8_t)i, 2, (uint8_t)(i * 4u + 2u));
        ok &= masif_set_song_cell(song, (uint8_t)i, 3, 3);
    }
    return ok;
}
