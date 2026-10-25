#include "../firmware/src/masif_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned count, effects, notes, pitches, cuts, retriggers; masif_audio_event_t last; } capture_t;
static void capture(void *context, const masif_audio_event_t *event)
{
    capture_t *c = (capture_t *)context;
    c->count++;
    if (event->type == MASIF_EVENT_EFFECT) c->effects++;
    if (event->type == MASIF_EVENT_NOTE_ON) c->notes++;
    if (event->type == MASIF_EVENT_PITCH) c->pitches++;
    if (event->type == MASIF_EVENT_CUT) c->cuts++;
    if (event->type == MASIF_EVENT_RETRIGGER) c->retriggers++;
    c->last = *event;
}

/* every event of one track with the tick it came on */
typedef struct { unsigned tick, n, effects; struct { unsigned tick; uint8_t type, vel, fx, note; int16_t pitch; } ev[512]; } log_t;
static void logger(void *context, const masif_audio_event_t *event)
{
    log_t *l = (log_t *)context;
    if (event->track != 0u || (event->type == MASIF_EVENT_EFFECT && !l->effects)) return;
    assert(l->n < 512u);
    l->ev[l->n].tick = l->tick; l->ev[l->n].type = event->type; l->ev[l->n].pitch = event->pitch_offset;
    l->ev[l->n].vel = event->velocity; l->ev[l->n].fx = event->fx; l->ev[l->n].note = event->note;
    l->n++;
}
/* The tests' song: track t plays chain t, round and round; chain t is the four phrases t * 4 + 0..3, so a track is 64
 * rows of its own (row r of track t is phrase t * 4 + r / 16, row r % 16). put() writes track 1's rows. */
static void clear(masif_song_t *song)
{
    unsigned t, k;
    masif_song_clear(song);
    for (t = 0; t < MASIF_TRACKS; t++) {
        for (k = 0; k < 4u; k++) assert(masif_set_chain_step(song, (uint8_t)t, (uint8_t)k, (uint8_t)(t * 4u + k), 0));
        assert(masif_set_song_cell(song, 0, (uint8_t)t, (uint8_t)t));
    }
}
static void put(masif_song_t *song, unsigned row, unsigned note, unsigned fx, unsigned param)
{
    masif_cell_t c;
    c.note = (uint8_t)note; c.instrument = note ? 7u : 0xFFu; c.volume = 0xFFu; c.fx = (uint8_t)fx; c.param = (uint8_t)param;
    c.fx2 = c.param2 = 0;
    assert(masif_set_cell(song, (uint8_t)(row / MASIF_ROWS), (uint8_t)(row % MASIF_ROWS), &c));
}
static void run(masif_runtime_t *rt, log_t *l, unsigned ticks)
{
    while (ticks--) { (void)masif_runtime_ticks(rt, 1u, 0, logger, l); l->tick++; }
}
static unsigned count(const log_t *l, uint8_t type, unsigned from, unsigned to)
{
    unsigned i, n = 0;
    for (i = 0; i < l->n; i++) n += l->ev[i].type == type && l->ev[i].tick >= from && l->ev[i].tick < to;
    return n;
}

static void test_effects(void)
{
    static masif_song_t song;
    static log_t l;
    masif_runtime_t rt;
    unsigned i, k;

    /* ARPEGGIO: the note, +3, +7, four ticks each (6 a row); a row that goes on with it starts at the note again;
     * the row after it is back at the note's pitch */
    clear(&song);
    put(&song, 0, 60, MASIF_FX_ARPEGGIO, 0x37);
    put(&song, 1, 0, MASIF_FX_ARPEGGIO, 0x37);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 72);
    {
        static const struct { unsigned tick; int8_t pitch; } want[] = {
            {4, 3}, {8, 7}, {12, 0}, {16, 3}, {20, 7}, {24, 0}, {28, 3}, {32, 7}, {36, 0}, {40, 3}, {44, 7}, {48, 0}};
        assert(l.n == 13u && l.ev[0].type == MASIF_EVENT_NOTE_ON && l.ev[0].tick == 0u);
        for (i = 0; i < 12u; i++)
            assert(l.ev[i + 1u].type == MASIF_EVENT_PITCH && l.ev[i + 1u].tick == want[i].tick && l.ev[i + 1u].pitch == want[i].pitch * MASIF_PITCH_SEMI);
    }

    /* PBN xy: up for x rows, then down for y, 1/16 semitone every clock tick (24 a row, the rows' first ticks too);
     * F: on until the next note; the pitch stays where a bend left it until the next note */
    clear(&song);
    put(&song, 0, 60, MASIF_FX_PITCH_BEND, 0x21);
    put(&song, 5, 62, MASIF_FX_PITCH_BEND, 0xF0);
    put(&song, 7, 64, 0, 0);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 24);
    assert(rt.pitch_offset[0] == 23);
    run(&rt, &l, 25);                               /* (the up's last tick is row 2's first) */
    assert(rt.pitch_offset[0] == 48 && !rt.bend_up[0]);
    run(&rt, &l, 24);                               /* then down a row */
    assert(rt.pitch_offset[0] == 24 && !rt.bend_up[0] && !rt.bend_down[0]);
    for (i = 2; i < l.n; i++) {                     /* a step at a time */
        int d = l.ev[i].pitch - l.ev[i - 1u].pitch;
        assert(l.ev[i].type == MASIF_EVENT_PITCH && (d == 1 || d == -1));
    }
    k = l.n;
    run(&rt, &l, 47);                               /* rows 3, 4: it stays */
    assert(l.n == k && rt.pitch_offset[0] == 24);
    run(&rt, &l, 1);                                /* row 5, a note: at its own pitch, then up, on and on */
    assert(l.ev[k].type == MASIF_EVENT_NOTE_ON && l.ev[k].pitch == 0);
    run(&rt, &l, 47);
    assert(rt.pitch_offset[0] == 47 && rt.bend_up[0] == MASIF_BEND_ON);
    run(&rt, &l, 1);                                /* row 7's note ends it */
    assert(rt.pitch_offset[0] == 0 && !rt.bend_up[0] && rt.active_note[0] == 64u);
    clear(&song);
    put(&song, 0, 60, MASIF_FX_PITCH_BEND, 0xF0);   /* (the most a bend goes: 48 semitones) */
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 380);
    assert(rt.pitch_offset[0] == 379);
    clear(&song);
    put(&song, 0, 0, MASIF_FX_PITCH_BEND, 0x10);    /* (no note: nothing) */
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 24);
    assert(l.n == 0u && !rt.bend_up[0]);

    /* SLD: a note with it is not hit, the sounding note slides there, up or down (param / 16 semitones a tick),
     * and stops on it; without a sounding note the note just plays */
    clear(&song);
    put(&song, 0, 60, 0, 0);
    put(&song, 1, 64, MASIF_FX_SLIDE, 4);      /* 4 semitones in 16 ticks */
    put(&song, 2, 0, MASIF_FX_SLIDE, 4);
    put(&song, 3, 57, MASIF_FX_SLIDE, 255);    /* down 3 from the note: in one tick */
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 48);
    assert(count(&l, MASIF_EVENT_NOTE_ON, 0, ~0u) == 1u && rt.active_note[0] == 60u);
    assert(rt.pitch_offset[0] == 4 * MASIF_PITCH_SEMI && count(&l, MASIF_EVENT_PITCH, 24, 48) == 16u);
    run(&rt, &l, 24);
    assert(count(&l, MASIF_EVENT_PITCH, 48, 72) == 0u);         /* there: nothing more to do */
    run(&rt, &l, 24);
    assert(rt.pitch_offset[0] == -3 * MASIF_PITCH_SEMI && count(&l, MASIF_EVENT_NOTE_ON, 0, ~0u) == 1u);
    clear(&song);
    put(&song, 0, 64, MASIF_FX_SLIDE, 4);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 24);
    assert(count(&l, MASIF_EVENT_NOTE_ON, 0, ~0u) == 1u && rt.pitch_offset[0] == 0);

    /* VIBRATO 0x48: around the note's pitch, +-1 semitone (depth 8 / 8), a cycle in 64 ticks (256 / 4); the row
     * after it is back on the pitch; on a slid note around where the slide left it */
    clear(&song);
    put(&song, 0, 60, MASIF_FX_VIBRATO, 0x48);
    put(&song, 1, 0, MASIF_FX_VIBRATO, 0x48);
    put(&song, 2, 0, MASIF_FX_VIBRATO, 0x48);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 73);
    {
        int lo = 0, hi = 0;
        for (i = 1; i < l.n - 1u; i++) {
            assert(l.ev[i].type == MASIF_EVENT_PITCH);
            if (l.ev[i].pitch < lo) lo = l.ev[i].pitch;
            if (l.ev[i].pitch > hi) hi = l.ev[i].pitch;
        }
        assert(hi == MASIF_PITCH_SEMI && lo == -MASIF_PITCH_SEMI && rt.pitch_offset[0] == 0);
        assert(l.ev[l.n - 1u].tick == 72u && l.ev[l.n - 1u].pitch == 0);
    }

    /* TABLES. An instrument's own table starts with its note: a row a tracker tick (4 clock ticks), its transpose
     * on top of the note while the row lasts, its volume from then on; after row F it is at row 0 again */
    clear(&song);
    {
        masif_table_cell_t tc = {0, 0xFF, 0, 0, 0, 0};
        tc.transpose = 0;  tc.volume = 127; assert(masif_set_table_cell(&song, 7, 0, tc));
        tc.transpose = 4;  tc.volume = 0xFF; assert(masif_set_table_cell(&song, 7, 1, tc));
        tc.transpose = 7;  tc.volume = 64; assert(masif_set_table_cell(&song, 7, 2, tc));
        tc.transpose = 0;  tc.volume = 0xFF; tc.fx = MASIF_FX_TABLE_HOP; tc.param = 1; assert(masif_set_table_cell(&song, 7, 3, tc));
    }
    put(&song, 0, 60, 0, 0);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 32);
    {   /* row 0 at tick 0 (volume), 1 at 4 (+4), 2 at 8 (+7, volume 64), 3 at 12 (0, hop to 1), 1 at 16 (+4), 2 at 20 ... */
        static const struct { unsigned tick; uint8_t type; int16_t semis; uint8_t vel; } want[] = {
            {0, MASIF_EVENT_NOTE_ON, 0, 100}, {0, MASIF_EVENT_VOLUME, 0, 100}, {4, MASIF_EVENT_PITCH, 4, 100},
            {8, MASIF_EVENT_PITCH, 7, 100}, {8, MASIF_EVENT_VOLUME, 7, 50}, {12, MASIF_EVENT_PITCH, 0, 50},
            {16, MASIF_EVENT_PITCH, 4, 50}, {20, MASIF_EVENT_PITCH, 7, 50}, {20, MASIF_EVENT_VOLUME, 7, 50},
            {24, MASIF_EVENT_PITCH, 0, 50}, {28, MASIF_EVENT_PITCH, 4, 50}};   /* (the cell's volume 100, x 64 / 127) */
        assert(l.n == sizeof want / sizeof want[0]);
        for (i = 0; i < l.n; i++)
            assert(l.ev[i].tick == want[i].tick && l.ev[i].type == want[i].type &&
                   l.ev[i].pitch == want[i].semis * MASIF_PITCH_SEMI && l.ev[i].vel == want[i].vel);
    }
    /* the instrument's TIC 03: 3 tracker ticks a table row; NOTE OFF ends it */
    song.instrument[7].table_tic = 3;
    put(&song, 2, MASIF_NOTE_OFF, 0, 0);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 48);
    assert(count(&l, MASIF_EVENT_PITCH, 0, 48) == 3u && l.ev[2].tick == 12u && l.ev[3].tick == 24u && l.ev[5].tick == 36u);
    k = l.n;
    run(&rt, &l, 48);                               /* row 2: NOTE OFF */
    assert(l.n == k + 1u && l.ev[k].type == MASIF_EVENT_NOTE_OFF);
    /* TBL xx runs another table in place of the instrument's; an effect in a table row is the track's from then
     * on (here a slide); an instrument with table tic 0 has no table of its own */
    clear(&song);
    {
        masif_table_cell_t tc = {12, 0xFF, 0, 0, 0, 0};
        assert(masif_set_table_cell(&song, 7, 0, tc));          /* (the instrument's own: must not run) */
        tc.transpose = 0; tc.fx = MASIF_FX_PITCH_BEND; tc.param = 0xF0;
        assert(masif_set_table_cell(&song, 0x55, 1, tc));
    }
    put(&song, 0, 60, MASIF_FX_SET_TABLE, 0x55);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 24);
    assert(rt.table_id[0] == 0x55 && rt.effect[0][0] == MASIF_FX_PITCH_BEND);
    assert(l.ev[1].tick == 5u && l.ev[1].pitch == 1 && rt.pitch_offset[0] == 19);
    song.instrument[7].table_tic = 0;
    put(&song, 0, 60, 0, 0);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 24);
    assert(l.n == 1u && !rt.table_on[0]);

    /* a table row's two effects both become the track's, each in its column */
    clear(&song);
    {
        masif_table_cell_t tc = {0, 0xFF, MASIF_FX_VIBRATO, 0x48, MASIF_FX_RETRIGGER, 0x02};
        assert(masif_set_table_cell(&song, 7, 0, tc));
    }
    put(&song, 0, 60, 0, 0);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 24);
    assert(rt.effect[0][0] == MASIF_FX_VIBRATO && rt.effect[0][1] == MASIF_FX_RETRIGGER && count(&l, MASIF_EVENT_RETRIGGER, 0, 24) == 2u &&
           count(&l, MASIF_EVENT_PITCH, 0, 24) > 10u);

    /* the volume column alone is the sounding note's volume */
    clear(&song);
    put(&song, 0, 60, 0, 0);
    {
        masif_cell_t c = {0, 0xFF, 40, 0, 0, 0, 0};
        assert(masif_set_cell(&song, 0, 1, &c));
    }
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 30);
    assert(l.n == 2u && l.ev[1].type == MASIF_EVENT_VOLUME && l.ev[1].tick == 24u && l.ev[1].vel == 40u && l.ev[0].vel == 100u);

    /* SET TEMPO reaches the host as an effect event with its BPM */
    clear(&song);
    put(&song, 0, 0, MASIF_FX_SET_TEMPO, 0xA0);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    l.effects = 1;
    run(&rt, &l, 4);
    assert(l.n == 1u && l.ev[0].type == MASIF_EVENT_EFFECT && l.ev[0].fx == MASIF_FX_SET_TEMPO);
    l.effects = 0;

    /* RET xy: a hit every y tracker ticks. 01 with the note: 5 more in its row; a row that goes on with it (no
     * note) 6, the first on the row's own tick: an even roll over the rows; nothing after it */
    clear(&song);
    song.instrument[7].table_tic = 0;
    put(&song, 0, 60, MASIF_FX_RETRIGGER, 0x01);
    put(&song, 1, 0, MASIF_FX_RETRIGGER, 0x01);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 72);
    assert(l.n == 12u && l.ev[0].type == MASIF_EVENT_NOTE_ON);
    for (i = 1; i < 12u; i++)
        assert(l.ev[i].type == MASIF_EVENT_RETRIGGER && l.ev[i].tick == 4u * i && l.ev[i].vel == 100u);
    /* y = 3: 32nds, 6: 16ths (the row's own tick only), C: 8ths (every other row), 0: once, where it begins */
    {
        static const struct { uint8_t y; unsigned hits; } rate[4] = {{3, 8}, {6, 4}, {0xC, 2}, {0, 1}};
        for (k = 0; k < 4u; k++) {
            clear(&song);
            song.instrument[7].table_tic = 0;
            put(&song, 0, 60, 0, 0);
            for (i = 1; i < 5u; i++) put(&song, i, 0, MASIF_FX_RETRIGGER, rate[k].y);
            masif_runtime_init(&rt, &song, 1200u);
            memset(&l, 0, sizeof l);
            run(&rt, &l, 5u * 24u);
            assert(count(&l, MASIF_EVENT_RETRIGGER, 24, 120) == rate[k].hits);
        }
    }
    /* x: each hit softer (1-7) or louder (9-F), from the note's own volume, to silence or the most; a new note
     * starts at its own again */
    clear(&song);
    song.instrument[7].table_tic = 0;
    put(&song, 0, 60, MASIF_FX_RETRIGGER, 0x31);    /* 18 softer a hit */
    put(&song, 1, 0, MASIF_FX_RETRIGGER, 0x31);
    put(&song, 2, 62, MASIF_FX_RETRIGGER, 0xF3);    /* 42 louder a hit */
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 72);
    assert(l.ev[0].vel == 100u && l.ev[1].vel == 82u && l.ev[2].vel == 64u && l.ev[5].vel == 10u && l.ev[6].vel == 0u && l.ev[11].vel == 0u);
    assert(l.ev[12].type == MASIF_EVENT_NOTE_ON && l.ev[12].tick == 48u && l.ev[12].vel == 100u && l.ev[13].vel == 127u);
    clear(&song);                        /* (no sounding note: nothing to hit again) */
    put(&song, 0, 0, MASIF_FX_RETRIGGER, 0x01);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 24);
    assert(l.n == 0u);

    /* PROBABILITY (percent): 0 never, 100 and more always, 50 about every other; a lost row changes nothing */
    {
        static const unsigned pct[4] = {0, 100, 255, 50};
        unsigned notes[4];
        for (k = 0; k < 4u; k++) {
            clear(&song);
            for (i = 0; i < 64u; i++) put(&song, i, 60, MASIF_FX_PROBABILITY, pct[k]);
            masif_runtime_init(&rt, &song, 1200u);
            memset(&l, 0, sizeof l);
            run(&rt, &l, 4u * 64u * 24u);
            notes[k] = count(&l, MASIF_EVENT_NOTE_ON, 0, ~0u);
            if (k == 0u) assert(rt.active_instrument[0] == 0u);     /* (the cells' instrument 7 never came) */
        }
        assert(notes[0] == 0u && notes[1] == 256u && notes[2] == 256u && notes[3] > 96u && notes[3] < 160u);
        masif_runtime_restart(&rt);                 /* a PLAY is not the same rolls again */
        k = rt.rng;
        memset(&l, 0, sizeof l);
        run(&rt, &l, 24);
        assert(rt.rng != k);
    }

    /* a cell's instrument that is not there (128-254) changes nothing: a retrigger after it is still the note's
     * own sound; a volume past 127 is 127 */
    clear(&song);
    put(&song, 0, 60, 0, 0);
    {
        masif_cell_t c;
        c.note = 0; c.instrument = 200; c.volume = 200; c.fx = MASIF_FX_RETRIGGER; c.param = 0x02;
        assert(masif_set_cell(&song, 0, 1, &c));
    }
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 48);
    assert(rt.active_instrument[0] == 7u && rt.active_volume[0] == 127u && count(&l, MASIF_EVENT_RETRIGGER, 24, 48) == 3u);

    /* BREAK x (HOP): the track's phrase ends, its next one goes on at row x: the track's own, the others go on as
     * they were */
    clear(&song);
    put(&song, 3, 60, MASIF_FX_BREAK, 5);           /* track 1: after row 3 on at row 5 of its 2nd phrase (row 21 of 64) */
    put(&song, 21, 62, 0, 0);
    put(&song, 22, 0, MASIF_FX_X_JUMP, 7);          /* (JMP: taken out, nothing) */
    {
        masif_cell_t c = {72, 7, 0xFF, 0, 0, 0, 0};
        assert(masif_set_cell(&song, 4, 5, &c));    /* (track 2, its row 5: must come at its own time) */
    }
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 7u * 24u);
    assert(l.n == 2u && l.ev[0].tick == 3u * 24u && l.ev[1].tick == 4u * 24u);
    assert(rt.tracker.song_row[0] == 0u && rt.tracker.row[0] == 8u && rt.tracker.song_row[1] == 0u && rt.tracker.row[1] == 7u);

    /* the chain's transpose is in the note that plays; for the screens one chain or one phrase plays alone */
    clear(&song);
    put(&song, 0, 60, 0, 0);
    put(&song, 16, 60, 0, 0);
    assert(masif_set_chain_step(&song, 0, 1, 1, 7));
    masif_runtime_init(&rt, &song, 1200u);
    {
        capture_t c2;
        memset(&c2, 0, sizeof c2);
        (void)masif_runtime_ticks(&rt, 1, 0, capture, &c2);
        assert(c2.last.note == 60u);
        for (i = 0; i < 16u * 24u; i++) (void)masif_runtime_ticks(&rt, 1, 0, capture, &c2);
        assert(c2.last.note == 67u && c2.notes == 2u);
        masif_runtime_start(&rt, MASIF_PLAY_PHRASE, 3, 1);      /* phrase 1 on track 4, round and round */
        memset(&c2, 0, sizeof c2);
        for (i = 0; i < 48u * 24u; i++) (void)masif_runtime_ticks(&rt, 1, 0, capture, &c2);
        assert(c2.notes == 3u && c2.last.track == 3u && c2.last.note == 60u);
        masif_runtime_start(&rt, MASIF_PLAY_CHAIN, 2, 0);       /* chain 0 on track 3 */
        memset(&c2, 0, sizeof c2);
        for (i = 0; i < 64u * 24u; i++) (void)masif_runtime_ticks(&rt, 1, 0, capture, &c2);
        assert(c2.notes == 2u && c2.last.track == 2u && c2.last.note == 67u);
    }

    /* a row's two effects both run: here a roll that bends (RET + PBN), a pan beside a send */
    clear(&song);
    {
        masif_cell_t c = {60, 7, 0xFF, MASIF_FX_RETRIGGER, 0x03, MASIF_FX_PITCH_BEND, 0x10};
        assert(masif_set_cell(&song, 0, 0, &c));
        c.note = 64; c.fx = MASIF_FX_PAN; c.param = 0x40; c.fx2 = MASIF_FX_SEND; c.param2 = 0x12;
        assert(masif_set_cell(&song, 0, 1, &c));
        c.note = 65; c.fx = MASIF_FX_X_NOTE_DELAY; c.param = 1; c.fx2 = MASIF_FX_X_NOTE_CUT; c.param2 = 4;
        assert(masif_set_cell(&song, 0, 2, &c));    /* (DLY, CUT: taken out, nothing: the note on time, not cut) */
    }
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    l.effects = 1;
    run(&rt, &l, 72);
    assert(count(&l, MASIF_EVENT_RETRIGGER, 0, 24) == 1u && count(&l, MASIF_EVENT_PITCH, 0, 24) == 23u && rt.pitch_offset[0] == 0);
    assert(count(&l, MASIF_EVENT_EFFECT, 0, 1) == 2u && count(&l, MASIF_EVENT_EFFECT, 24, 25) == 2u);
    assert(count(&l, MASIF_EVENT_NOTE_ON, 24, 25) == 1u && count(&l, MASIF_EVENT_NOTE_ON, 48, 49) == 1u);
    assert(count(&l, MASIF_EVENT_CUT, 0, 72) == 0u && rt.active_note[0] == 65u);
    l.effects = 0;

    /* STP lets the note go (a NOTE OFF: its release rings), after xx tracker ticks or at the row's start; DED cuts
     * the voice and ends what runs on the track, also where no note is held any more */
    clear(&song);
    put(&song, 0, 60, MASIF_FX_STOP, 2);
    put(&song, 1, 62, 0, 0);
    put(&song, 2, 0, MASIF_FX_STOP, 0);
    {
        masif_cell_t c = {64, 7, 0xFF, MASIF_FX_PITCH_BEND, 0xF0, MASIF_FX_KILL, 3};
        assert(masif_set_cell(&song, 0, 3, &c));
    }
    put(&song, 4, 0, MASIF_FX_KILL, 0);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 120);
    assert(count(&l, MASIF_EVENT_NOTE_OFF, 0, 8) == 0u && count(&l, MASIF_EVENT_NOTE_OFF, 8, 9) == 1u && count(&l, MASIF_EVENT_NOTE_OFF, 9, 48) == 0u);
    assert(count(&l, MASIF_EVENT_NOTE_OFF, 48, 49) == 1u && count(&l, MASIF_EVENT_CUT, 0, 84) == 0u);
    assert(count(&l, MASIF_EVENT_PITCH, 73, 84) == 11u && count(&l, MASIF_EVENT_CUT, 84, 85) == 1u && count(&l, MASIF_EVENT_PITCH, 85, 96) == 0u);
    assert(count(&l, MASIF_EVENT_CUT, 96, 97) == 1u && rt.pitch_offset[0] == 0 && rt.active_note[0] == MASIF_NOTE_NONE);

    /* REP and RTO: the column's last command again on every row, its value moved by REP's, until RTO's
     * value (it goes on with that), on rows that leave the column empty too; REP 00 ends it. 80-FF count down */
    clear(&song);
    put(&song, 0, 60, MASIF_FX_PAN, 0x10);
    put(&song, 1, 0, MASIF_FX_REPEAT, 0x10);
    put(&song, 3, 0, MASIF_FX_REPEAT_TO, 0x38);
    put(&song, 6, 0, MASIF_FX_REPEAT, 0);
    put(&song, 8, 60, MASIF_FX_PAN, 0x20);
    put(&song, 9, 0, MASIF_FX_REPEAT, 0xF0);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    l.effects = 1;
    run(&rt, &l, 72);
    assert(rt.effect[0][0] == MASIF_FX_PAN && rt.effect_param[0][0] == 0x30 && count(&l, MASIF_EVENT_EFFECT, 0, 72) == 3u);
    run(&rt, &l, 72);
    assert(rt.effect_param[0][0] == 0x38 && count(&l, MASIF_EVENT_EFFECT, 72, 144) == 3u);
    run(&rt, &l, 48);
    assert(rt.effect[0][0] == MASIF_FX_NONE && count(&l, MASIF_EVENT_EFFECT, 144, 192) == 0u);
    run(&rt, &l, 96);
    assert(rt.effect[0][0] == MASIF_FX_PAN && rt.effect_param[0][0] == 0x00 && count(&l, MASIF_EVENT_EFFECT, 192, 288) == 4u);
    l.effects = 0;

    /* RNL: in the second column the first column's value is random on every pass, digit by digit (here
     * the second digit, 0..3 on); in the first column the row's note (0..4 semitones up) and its instrument */
    clear(&song);
    {
        masif_cell_t c = {60, 7, 0xFF, MASIF_FX_PAN, 0x40, MASIF_FX_RANDOM_LEFT, 0x03};
        unsigned seen = 0, notes = 0, pass;
        assert(masif_set_cell(&song, 0, 0, &c));
        c.note = 60; c.fx = MASIF_FX_RANDOM_LEFT; c.param = 0x40; c.fx2 = 0; c.param2 = 0;
        assert(masif_set_cell(&song, 0, 1, &c));
        masif_runtime_init(&rt, &song, 1200u);
        for (pass = 0; pass < 64u; pass++) {
            memset(&l, 0, sizeof l);
            run(&rt, &l, 24);
            assert(rt.effect[0][0] == MASIF_FX_PAN && rt.effect_param[0][0] >= 0x40 && rt.effect_param[0][0] <= 0x43);
            seen |= 1u << (rt.effect_param[0][0] - 0x40);
            memset(&l, 0, sizeof l);
            run(&rt, &l, 24);
            assert(l.n >= 1u && l.ev[0].type == MASIF_EVENT_NOTE_ON && l.ev[0].note >= 60u && l.ev[0].note <= 64u);
            notes |= 1u << (l.ev[0].note - 60u);
            run(&rt, &l, 62u * 24u);
        }
        assert(seen == 15u && notes == 31u);
    }

    /* grooves: a track's rows last what its groove's steps say, round (here 08 04: swing), a step of
     * 00 passes a row of the phrase over; GRV puts the track on another groove, GRV 00 back to the 16ths */
    clear(&song);
    for (i = 0; i < 6u; i++) put(&song, i, 60 + i, 0, 0);
    assert(masif_set_groove(&song, 0, 0, 8) && masif_set_groove(&song, 0, 1, 4) && !masif_set_groove(&song, 0, 2, 0x40) && !masif_set_groove(&song, 16, 0, 6));
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 97);
    assert(l.n == 5u && l.ev[1].tick == 32u && l.ev[2].tick == 48u && l.ev[3].tick == 80u && l.ev[4].tick == 96u);
    assert(masif_set_groove(&song, 0, 0, 6) && masif_set_groove(&song, 0, 1, 0) && masif_set_groove(&song, 0, 2, 3));
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 61);                               /* row 0 (6), row 1 passed over, row 2 (3), row 3 (6), row 4 over, row 5 */
    assert(l.n == 4u && l.ev[0].note == 60u && l.ev[1].note == 62u && l.ev[1].tick == 24u && l.ev[2].note == 63u && l.ev[2].tick == 36u &&
           l.ev[3].note == 65u && l.ev[3].tick == 60u);
    clear(&song);
    for (i = 0; i < 6u; i++) put(&song, i, 60 + i, i == 1u || i == 3u || i == 4u ? MASIF_FX_GROOVE : 0, i == 1u ? 5 : i == 3u ? 2 : 0);
    assert(masif_set_groove(&song, 5, 0, 3) && masif_set_groove(&song, 5, 1, 0xFF) && masif_set_groove(&song, 2, 0, 12) && masif_set_groove(&song, 2, 1, 0xFF));
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 37);                               /* row 0: 24; row 1 GRV 05: 12; row 2: 12 */
    assert(l.n == 3u && l.ev[1].tick == 24u && l.ev[2].tick == 36u && rt.groove[0] == 5u && rt.groove[1] == 0u);
    run(&rt, &l, 12);                               /* row 3 GRV 02: 48 a row, this track's alone */
    assert(l.n == 4u && l.ev[3].tick == 48u && rt.groove[0] == 2u && rt.groove[3] == 0u);
    run(&rt, &l, 73);                               /* row 4 at 96 with GRV 00: 16ths again */
    assert(l.n == 6u && l.ev[4].tick == 96u && l.ev[5].tick == 120u && rt.groove[0] == 0u && rt.groove[3] == 0u);
    song.project_groove = 5;                        /* the project's groove: what every track is on after PLAY */
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 13);
    assert(l.n == 2u && l.ev[1].tick == 12u && rt.groove[2] == 5u);
    song.project_groove = 0;

    /* MTT: a row begins late or early by clock ticks; the rows after it are where they were */
    clear(&song);
    for (i = 0; i < 6u; i++) put(&song, i, 60 + i, i == 1u || i == 3u || i == 5u ? MASIF_FX_MICROTIME : 0, i == 1u ? 6 : i == 3u ? 0xFA : 0xC0);
    masif_runtime_init(&rt, &song, 1200u);
    memset(&l, 0, sizeof l);
    run(&rt, &l, 121);
    assert(l.n == 6u && l.ev[1].tick == 30u && l.ev[2].tick == 48u && l.ev[3].tick == 66u && l.ev[4].tick == 96u);
    assert(l.ev[5].tick == 97u);                    /* (early by more than the row before it has: a tick after that one) */

    /* the project's key and scale hold every note that plays (with its chain's transpose in it), but not the
     * notes of an instrument in slices or one marked free; set through the queue like the rest of the project */
    clear(&song);
    put(&song, 0, 61, 0, 0);                        /* C#4 */
    put(&song, 1, 66, 0, 0);                        /* F#4 */
    put(&song, 16, 61, 0, 0);                       /* C#4 in the chain's second phrase: + 3 = E4 */
    assert(masif_set_chain_step(&song, 0, 1, 1, 3));
    {
        masif_cell_t c = {61, 9, 0xFF, 0, 0, 0, 0};       /* tracks 2 and 3: the same C#4 on a sliced and on a free instrument */
        masif_edit_queue_t q;
        uint8_t d[7] = {0, 0, 0, 0, 0, 0, 0};
        song.instrument[9].slices = 8;
        assert(masif_set_cell(&song, 4, 0, &c));
        c.instrument = 10; song.instrument[10].flags = MASIF_INST_FREE;
        assert(masif_set_cell(&song, 8, 0, &c));
        masif_edit_queue_init(&q);
        d[0] = 0; assert(masif_post_edit(&q, MASIF_EDIT_MIX, MASIF_TRACKS + 4, 0, d));      /* key C */
        d[0] = 1; assert(masif_post_edit(&q, MASIF_EDIT_MIX, MASIF_TRACKS + 5, 0, d));      /* major */
        d[0] = 12; assert(masif_post_edit(&q, MASIF_EDIT_MIX, MASIF_TRACKS + 4, 0, d));     /* (no such key) */
        masif_runtime_init(&rt, &song, 1200u);
        memset(&l, 0, sizeof l);
        (void)masif_runtime_ticks(&rt, 0, &q, 0, 0);
        assert(song.key == 0 && song.scale == 1);
        run(&rt, &l, 17u * 24u);
        assert(l.n == 3u && l.ev[0].note == 60 && l.ev[1].note == 65 && l.ev[2].note == 64 && l.ev[2].tick == 16u * 24u);
        assert(rt.active_note[1] == 61 && rt.active_note[2] == 61);
        song.key = 1;                               /* the same song a semitone up: C# major has the C# and the F# */
        masif_runtime_init(&rt, &song, 1200u);
        memset(&l, 0, sizeof l);
        run(&rt, &l, 2u * 24u);
        assert(l.ev[0].note == 61 && l.ev[1].note == 66);
    }

    /* edits of every part of the song go through the queue, and only what can be gets in */
    {
        masif_edit_queue_t q;
        uint8_t d[7] = {0, 0, 0, 0, 0, 0, 0};
        clear(&song);
        masif_edit_queue_init(&q);
        masif_runtime_init(&rt, &song, 1200u);
        d[0] = 33; d[1] = (uint8_t)-7; assert(masif_post_edit(&q, MASIF_EDIT_CHAIN, 9, 2, d));
        d[0] = 9; assert(masif_post_edit(&q, MASIF_EDIT_SONG, 200, 3, d));
        d[0] = 5; d[1] = 90; d[2] = MASIF_FX_PAN; d[3] = 0x80; assert(masif_post_edit(&q, MASIF_EDIT_TABLE, 12, 15, d));
        d[0] = 99; assert(masif_post_edit(&q, MASIF_EDIT_INSTRUMENT, 40, (uint8_t)((uint8_t *)&song.instrument[0].volume - (uint8_t *)&song.instrument[0]), d));
        d[0] = 200; assert(masif_post_edit(&q, MASIF_EDIT_INSTRUMENT, 41, (uint8_t)((uint8_t *)&song.instrument[0].volume - (uint8_t *)&song.instrument[0]), d));
        d[0] = 80; assert(masif_post_edit(&q, MASIF_EDIT_MIX, 3, 0, d));
        d[0] = (uint8_t)-20; assert(masif_post_edit(&q, MASIF_EDIT_MIX, MASIF_TRACKS + 1, 0, d));
        d[0] = 174; assert(masif_post_edit(&q, MASIF_EDIT_MIX, MASIF_TRACKS + 3, 0, d));
        d[0] = 250; assert(masif_post_edit(&q, MASIF_EDIT_MIX, MASIF_TRACKS + 3, 0, d));      /* (not a tempo) */
        d[0] = 0xF0; assert(masif_post_edit(&q, MASIF_EDIT_CHAIN, 9, 3, d));                 /* (no such phrase) */
        assert(!masif_post_edit(&q, MASIF_EDIT_GROOVE + 1, 0, 0, d));
        assert(song.chain[9][2].phrase == MASIF_NONE);          /* (nothing before the audio side takes them) */
        (void)masif_runtime_ticks(&rt, 0, &q, 0, 0);
        assert(song.chain[9][2].phrase == 33 && song.chain[9][2].transpose == -7 && song.song[200][3] == 9);
        assert(song.table[12][15].transpose == 5 && song.table[12][15].volume == 90 && song.table[12][15].fx == MASIF_FX_PAN);
        assert(song.instrument[40].volume == 99 && song.instrument[41].volume == 127);
        assert(song.track_level[3] == 80 && song.master_eq[1] == -20 && song.tempo == 174 && song.chain[9][3].phrase == MASIF_NONE);
    }

}

int main(void)
{
    static masif_song_t song;
    masif_runtime_t rt;
    masif_edit_queue_t edits;
    masif_cell_t cell;
    capture_t cap;
    unsigned i, rows = 0;

    memset(&cap, 0, sizeof(cap));
    clear(&song);
    masif_edit_queue_init(&edits);
    masif_runtime_init(&rt, &song, 1200u);

    memset(&cell, 0, sizeof(cell));
    cell.note = 60u;
    cell.instrument = 7u;
    cell.volume = 110u;
    cell.fx = MASIF_FX_ARPEGGIO;
    cell.param = 0x37u;
    assert(masif_post_cell_edit(&edits, 0, 0, &cell));
    /* Edits are applied by the audio-side block function, not by the UI producer. */
    for (i = 0; i < 175u; ++i)
        rows += masif_runtime_audio_block(&rt, 44100u, 32u, &edits, capture, &cap);
    assert(rows == 1u);
    assert(cap.count >= 2u); /* note-on and effect event; arpeggio pitch ticks may follow */
    assert(cap.effects >= 1u && cap.notes >= 1u && cap.pitches >= 1u);

    /* Note-off and cut are explicit events, not confused with an empty row. */
    cell.note = MASIF_NOTE_OFF;
    cell.fx = MASIF_FX_NONE;
    assert(masif_post_cell_edit(&edits, 0, 1, &cell));
    rows = 0;
    for (i = 0; i < 175u; ++i)
        rows += masif_runtime_audio_block(&rt, 44100u, 32u, &edits, capture, &cap);
    assert(rows == 1u);
    assert(cap.last.type == MASIF_EVENT_NOTE_OFF);

    /* A note names its instrument; its volume is the cell's x the instrument's. */
    {
        masif_instrument_t cfg;
        memset(&cfg, 0, sizeof(cfg)); cfg.sample = 12u; cfg.root_note = 60u; cfg.volume = 64u;
        assert(masif_set_instrument(&song, 7u, &cfg));
        cell.note = 64u; cell.instrument = 7u; cell.volume = 127u; cell.fx = MASIF_FX_NONE;
        assert(masif_post_cell_edit(&edits, 0, 2, &cell));
        for (i = 0; i < 175u; ++i) (void)masif_runtime_audio_block(&rt, 44100u, 32u, &edits, capture, &cap);
        assert(cap.last.type == MASIF_EVENT_NOTE_ON);
        assert(cap.last.instrument == 7u && cap.last.note == 64u && cap.last.velocity == 64u);   /* (cell x instrument volume) */
    }

    /* Retrigger and kill execute on tracker ticks after the row event. */
    {
        unsigned retriggers_before = cap.retriggers;
        unsigned cuts_before = cap.cuts;
        rt.tracker.row[0] = 4u;
        cell.note = 70u; cell.instrument = 7u; cell.volume = 127u;
        cell.fx = MASIF_FX_RETRIGGER; cell.param = 2u;
        assert(masif_post_cell_edit(&edits, 0, 4, &cell));
        for (i = 0; i < 300u; ++i) (void)masif_runtime_audio_block(&rt, 44100u, 32u, &edits, capture, &cap);
        assert(cap.retriggers > retriggers_before);

        rt.tracker.row[0] = 5u;
        cell.note = 71u; cell.fx = MASIF_FX_KILL; cell.param = 2u;
        assert(masif_post_cell_edit(&edits, 0, 5, &cell));
        for (i = 0; i < 300u; ++i) (void)masif_runtime_audio_block(&rt, 44100u, 32u, &edits, capture, &cap);
        assert(cap.cuts > cuts_before);
    }

    /* Queue is bounded and reports overflow rather than overwriting unread edits. */
    masif_edit_queue_init(&edits);
    for (i = 0; i < MASIF_EDIT_CAPACITY - 1u; ++i)
        assert(masif_post_cell_edit(&edits, 0, (uint8_t)(i % MASIF_ROWS), &cell));
    assert(!masif_post_cell_edit(&edits, 0, 0, &cell));
    assert(edits.dropped == 1u);

    /* Host-clocked ticks: 24 ticks are one row, 0 ticks only applies edits, restart goes back to row 0. */
    {
        static masif_song_t song2;
        masif_runtime_t rt2;
        capture_t first, again;
        clear(&song2);
        masif_edit_queue_init(&edits);
        masif_runtime_init(&rt2, &song2, 1200u);
        memset(&cell, 0, sizeof(cell));
        cell.note = 60u; cell.instrument = 7u; cell.volume = 110u; cell.fx = MASIF_FX_PITCH_BEND; cell.param = 0x10u;
        assert(masif_post_cell_edit(&edits, 0, 0, &cell));
        cell.note = 62u; cell.fx = MASIF_FX_NONE; cell.param = 0u;
        assert(masif_post_cell_edit(&edits, 4, 1, &cell));         /* (track 2's row 1: its first phrase is 4) */
        memset(&cap, 0, sizeof(cap));
        assert(masif_runtime_ticks(&rt2, 0u, &edits, capture, &cap) == 0u);
        assert(edits.read_index == edits.write_index && cap.count == 0u && rt2.tracker.row[0] == 0u);
        assert(song2.phrase[0][0].note == 60u && song2.phrase[4][1].note == 62u);
        assert(masif_runtime_ticks(&rt2, 1u, &edits, capture, &cap) == 1u);   /* tick 0: row 0 */
        assert(cap.notes == 1u && rt2.tracker.row[0] == 1u && rt2.tick_in_row == 1u);
        assert(masif_runtime_ticks(&rt2, 23u, &edits, capture, &cap) == 0u);  /* ticks 1-23: its bend */
        assert(cap.pitches == 23u && rt2.tick_in_row == 0u && rt2.pitch_offset[0] == 23);
        assert(masif_runtime_ticks(&rt2, 4u, &edits, capture, &cap) == 1u);   /* tick 24: row 1, track 2's note */
        assert(cap.notes == 2u && cap.pitches == 24u && rt2.pitch_offset[0] == 24 && !rt2.bend_up[0]);
        assert(masif_runtime_ticks(&rt2, 1u, &edits, capture, &cap) == 0u);
        assert(cap.notes == 2u && rt2.tick_phase_q16 == 0u);
        first = cap;

        masif_runtime_restart(&rt2);                 /* mid-row, a bend's offset left */
        assert(rt2.tracker.row[0] == 0u && rt2.tick_in_row == 0u && rt2.pitch_offset[0] == 0 && !rt2.bend_up[0]);
        assert(rt2.active_note[0] == MASIF_NOTE_NONE && rt2.effect[0][0] == MASIF_FX_NONE && rt2.bpm10 == 1200u);
        memset(&cap, 0, sizeof(cap));
        for (i = 0; i < 29u; ++i) (void)masif_runtime_ticks(&rt2, 1u, &edits, capture, &cap);
        again = cap;
        assert(again.count == first.count && again.notes == first.notes && again.pitches == first.pitches);
        assert(!memcmp(&again.last, &first.last, sizeof(first.last)));

        assert(masif_runtime_ticks(0, 3u, &edits, capture, &cap) == 0u);
        masif_runtime_restart(0);
    }

    test_effects();
    puts("MASIF runtime tests: PASS");
    return 0;
}
