#include "../firmware/src/masif_tracker.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static masif_song_t song;
static void note(unsigned phrase, unsigned row, unsigned n, unsigned fx, unsigned param)
{
    masif_cell_t c = {(uint8_t)n, 3, 0xFF, (uint8_t)fx, (uint8_t)param, 0, 0};
    assert(masif_set_cell(&song, (uint8_t)phrase, (uint8_t)row, &c));
}
static masif_tracker_t tr;
static masif_row_event_t out[MASIF_TRACKS];
static void rows(unsigned n) { while (n--) masif_tick_row(&tr, out); }

int main(void)
{
    masif_table_cell_t tc;
    masif_cell_t c;
    unsigned i;

    /* an empty song: nothing plays, nothing moves */
    masif_song_clear(&song);
    masif_tracker_init(&tr, &song);
    rows(40);
    for (i = 0; i < MASIF_TRACKS; i++) assert(!tr.on[i] && out[i].note == MASIF_NOTE_NONE && out[i].instrument == 0xFF);
    memset(&c, 0, sizeof c);
    assert(!masif_set_cell(&song, MASIF_PHRASES, 0, &c) && !masif_set_cell(&song, 0, MASIF_ROWS, &c));
    assert(!masif_set_chain_step(&song, MASIF_CHAINS, 0, 0, 0) && !masif_set_chain_step(&song, 0, MASIF_CHAIN_STEPS, 0, 0) &&
           !masif_set_chain_step(&song, 0, 0, 0, 49));     /* (every phrase number is one: 00-FE, FF none) */
    assert(!masif_set_song_cell(&song, 0, MASIF_TRACKS, 0) && !masif_set_song_cell(&song, 0, 0, MASIF_CHAINS));

    /* a phrase is 16 rows; a chain plays its phrases in turn, each with its transpose; then round again */
    note(10, 0, 60, MASIF_FX_ARPEGGIO, 0x37);
    note(11, 0, 62, 0, 0);
    note(11, 15, 64, 0, 0);
    assert(masif_set_chain_step(&song, 5, 0, 10, 0) && masif_set_chain_step(&song, 5, 1, 11, 12) && masif_set_chain_step(&song, 5, 2, 10, -5));
    assert(masif_set_song_cell(&song, 0, 2, 5));                /* track 3 plays chain 5 */
    masif_tracker_start_song(&tr, 0);
    assert(tr.on[2] && !tr.on[0] && tr.chain[2] == 5 && tr.phrase[2] == 10);
    rows(1);
    assert(out[2].note == 60 && out[2].instrument == 3 && out[2].fx == MASIF_FX_ARPEGGIO && out[2].param == 0x37 && out[0].note == 0);
    rows(15); assert(tr.step[2] == 1 && tr.phrase[2] == 11 && tr.row[2] == 0);
    rows(1);  assert(out[2].note == 62 + 12);                   /* (the chain step's transpose) */
    rows(15); assert(out[2].note == 64 + 12 && tr.step[2] == 2);
    rows(1);  assert(out[2].note == 60 - 5);
    rows(15); assert(tr.step[2] == 0 && tr.song_row[2] == 0);   /* three phrases: the chain is over, the same one again */
    rows(1);  assert(out[2].note == 60);

    /* every track goes down its own column of the song; at the end of its run of chains back to the run's top */
    masif_song_clear(&song);
    note(0, 0, 50, 0, 0); note(1, 0, 51, 0, 0); note(2, 0, 52, 0, 0); note(3, 0, 53, 0, 0);
    for (i = 0; i < 4; i++) assert(masif_set_chain_step(&song, (uint8_t)i, 0, (uint8_t)i, 0));     /* chain i = phrase i */
    assert(masif_set_chain_step(&song, 3, 1, 3, 0));            /* (chain 3: two phrases long) */
    assert(masif_set_song_cell(&song, 4, 0, 0) && masif_set_song_cell(&song, 5, 0, 1) && masif_set_song_cell(&song, 6, 0, 2));
    assert(masif_set_song_cell(&song, 4, 1, 3));                /* track 2: one chain of 32 rows */
    assert(masif_set_song_cell(&song, 8, 0, 3));                /* (another run further down: not reached) */
    masif_tracker_start_song(&tr, 4);
    rows(1);  assert(out[0].note == 50 && out[1].note == 53 && tr.on[0] && tr.on[1] && !tr.on[2]);
    rows(15); assert(tr.song_row[0] == 5 && tr.song_row[1] == 4 && tr.step[1] == 1);
    rows(1);  assert(out[0].note == 51 && out[1].note == 53);
    rows(15); assert(tr.song_row[0] == 6 && tr.song_row[1] == 4 && tr.step[1] == 0);   /* track 2 round again already */
    rows(16); assert(tr.song_row[0] == 4);                      /* row 7 is empty: back to the run's top, row 4 */
    rows(1);  assert(out[0].note == 50);
    masif_tracker_start_song(&tr, 5);                           /* from a row in the middle: the run's top is still 4 */
    assert(!tr.on[1]);                                          /* (track 2 has no chain in row 5: it rests) */
    rows(32); assert(tr.song_row[0] == 4);

    /* BREAK x: the track's next phrase at row x; JMP (taken out) is nothing */
    masif_song_clear(&song);
    note(0, 3, 60, MASIF_FX_BREAK, 5);                          /* phrase 0 ends at row 3: on at row 5 of the next */
    note(1, 5, 61, 0, 0);
    note(1, 6, 0, MASIF_FX_X_JUMP, 2);                          /* (was: to song row 2) */
    note(2, 0, 70, 0, 0);
    assert(masif_set_chain_step(&song, 0, 0, 0, 0) && masif_set_chain_step(&song, 0, 1, 1, 0) && masif_set_chain_step(&song, 1, 0, 2, 0));
    assert(masif_set_song_cell(&song, 0, 0, 0) && masif_set_song_cell(&song, 2, 0, 1));
    masif_tracker_start_song(&tr, 0);
    rows(4);  assert(out[0].note == 60 && tr.phrase[0] == 1 && tr.row[0] == 5 && tr.step[0] == 1);
    rows(1);  assert(out[0].note == 61);
    rows(1);  assert(tr.song_row[0] == 0 && tr.phrase[0] == 1 && tr.row[0] == 7);

    /* for the screens: one chain, or one phrase, alone on a track, round and round */
    masif_tracker_start_chain(&tr, 3, 0);
    assert(tr.on[3] && !tr.on[0] && tr.what == MASIF_PLAY_CHAIN);
    rows(4);  assert(out[3].note == 60 && tr.step[3] == 1 && tr.row[3] == 5);
    rows(2);  assert(tr.step[3] == 1 && tr.row[3] == 7);        /* (a JMP is nothing) */
    rows(9);  assert(tr.step[3] == 0 && tr.row[3] == 0);
    masif_tracker_start_phrase(&tr, 1, 1);
    rows(6);  assert(out[1].note == 61 && tr.on[1] && !tr.on[3]);
    rows(16); assert(out[1].note == 61 && tr.phrase[1] == 1);   /* the same phrase again */

    /* a chain whose first step is empty is 16 rows of nothing (no endless loop), then the next */
    masif_song_clear(&song);
    assert(masif_set_song_cell(&song, 0, 0, 9) && masif_set_song_cell(&song, 1, 0, 0) && masif_set_chain_step(&song, 0, 0, 0, 0));
    note(0, 0, 44, 0, 0);
    masif_tracker_start_song(&tr, 0);
    rows(16); assert(out[0].note == 0 && tr.song_row[0] == 1);
    rows(1);  assert(out[0].note == 44);

    /* a row's two effect columns: BREAK counts in either */
    masif_song_clear(&song);
    {
        masif_cell_t c2 = {60, 3, 0xFF, MASIF_FX_PAN, 0x40, MASIF_FX_BREAK, 9};
        assert(masif_set_cell(&song, 0, 2, &c2));
        assert(masif_set_chain_step(&song, 0, 0, 0, 0) && masif_set_chain_step(&song, 0, 1, 1, 0) && masif_set_song_cell(&song, 0, 0, 0));
        masif_tracker_start_song(&tr, 0);
        rows(3);
        assert(out[0].fx == MASIF_FX_PAN && out[0].param == 0x40 && out[0].fx2 == MASIF_FX_BREAK && tr.phrase[0] == 1 && tr.row[0] == 9);
    }

    /* tables are song data here; the runtime plays them (host-tests/test_runtime.c) */
    tc.transpose = -12; tc.volume = 70; tc.fx = MASIF_FX_TABLE_HOP; tc.param = 0; tc.fx2 = 0; tc.param2 = 0;
    assert(masif_set_table_cell(&song, 1, 15, tc) && song.table[1][15].transpose == -12);
    assert(!masif_set_table_cell(&song, MASIF_TABLES, 0, tc) && !masif_set_table_cell(&song, 1, MASIF_TABLE_ROWS, tc));
    assert(song.table[0][0].volume == 0xFF && song.table[0][0].fx == 0);

    /* an instrument is a sample and how a note plays it; what cannot be is refused */
    {
        masif_instrument_t in = song.instrument[70];
        assert(in.sample == MASIF_NO_SAMPLE && in.root_note == MASIF_DEFAULT_ROOT_NOTE && in.volume == 127 && in.table_tic == 1);
        in.sample = 9; in.play = MASIF_PLAY_LOOP; in.start = 64; in.loop_start = 96; in.length = 128; in.slices = 8;
        assert(masif_set_instrument(&song, 127, &in) && song.instrument[127].slices == 8);
        in.sample = MASIF_SAMPLES; assert(!masif_set_instrument(&song, 0, &in)); in.sample = 9;
        in.play = MASIF_PLAY_MODES; assert(!masif_set_instrument(&song, 0, &in)); in.play = 0;
        in.pan = 65; assert(!masif_set_instrument(&song, 0, &in)); in.pan = 0;
        in.eq[1] = 65; assert(!masif_set_instrument(&song, 0, &in)); in.eq[1] = 0;
        in.root_note = 0; assert(!masif_set_instrument(&song, 0, &in)); in.root_note = 60;
        assert(!masif_set_instrument(&song, MASIF_INSTRUMENTS, &in));
    }
    /* the project's key and scale: a note that is not in them is the next one below that is */
    masif_song_clear(&song);
    for (i = 1; i <= 120; i++) assert(masif_scale_note(&song, (uint8_t)i) == i);       /* chromatic: as written */
    song.scale = 1; song.key = 0;                               /* C major */
    {
        static const uint8_t want[12] = {60, 60, 62, 62, 64, 65, 65, 67, 67, 69, 69, 71};
        for (i = 0; i < 12; i++) assert(masif_scale_note(&song, (uint8_t)(60 + i)) == want[i]);
        assert(masif_scale_note(&song, 73) == 72 && masif_scale_note(&song, 1) == 1 && masif_scale_note(&song, 120) == 120);
    }
    song.scale = 2; song.key = 9;                               /* A minor: the same notes */
    for (i = 21; i <= 108; i++) { song.scale = 1; song.key = 0; c.note = masif_scale_note(&song, (uint8_t)i); song.scale = 2; song.key = 9; assert(masif_scale_note(&song, (uint8_t)i) == c.note); }
    song.scale = 11; song.key = 4;                              /* E minor pentatonic: E G A B D */
    assert(masif_scale_note(&song, 64) == 64 && masif_scale_note(&song, 65) == 64 && masif_scale_note(&song, 66) == 64 &&
           masif_scale_note(&song, 67) == 67 && masif_scale_note(&song, 70) == 69 && masif_scale_note(&song, 73) == 71 && masif_scale_note(&song, 74) == 74);
    for (song.scale = 0; song.scale < MASIF_SCALES; song.scale++)       /* every scale, every key: a note of the scale, */
        for (song.key = 0; song.key < 12; song.key++)                   /* never above the one written, at the most 3 below */
            for (i = 4; i <= 120; i++) {
                unsigned n = masif_scale_note(&song, (uint8_t)i);
                assert(n <= i && i - n <= 3 && masif_scale_note(&song, (uint8_t)n) == n && masif_scale_name[song.scale][0]);
            }
    song.key = 0; song.scale = 99; assert(masif_song_sanitize(&song) == 1 && song.scale == 0);
    song.key = 12; assert(masif_song_sanitize(&song) == 1 && song.key == 0);
    {   /* the effects taken out (2026-10-23) are cleared where a song is loaded, in phrases and tables */
        masif_cell_t c3 = {60, 0, 0xFF, MASIF_FX_X_NOTE_DELAY, 3, MASIF_FX_X_GROOVE_ALL, 2};
        masif_table_cell_t t3 = {0, 0xFF, MASIF_FX_X_TABLE_SPEED, 1, MASIF_FX_SLICE, 4};
        assert(masif_set_cell(&song, 9, 1, &c3) && masif_set_table_cell(&song, 9, 1, t3));
        assert(masif_song_sanitize(&song) == 3u);
        assert(song.phrase[9][1].fx == 0 && song.phrase[9][1].param == 0 && song.phrase[9][1].fx2 == 0 && song.phrase[9][1].note == 60);
        assert(song.table[9][1].fx == 0 && song.table[9][1].fx2 == MASIF_FX_SLICE && song.table[9][1].param2 == 4);
    }

    puts("MASIF tracker core tests: PASS");
    return 0;
}
