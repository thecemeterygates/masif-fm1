/* SPDX-License-Identifier: GPL-3.0-only */
/* MASIF host integration test: the real sound core (sp_core.c), drum sequencer (sp_seq.c), tracker core and
 * runtime (masif_tracker.c, masif_runtime.c) and the FM-1 adapter (masif_fm1_adapter.c), built as the firmware's
 * unity build includes them (zp12.c) and driven block by block in the audio ISR's order:
 *     sq_block(); masif_fm1_audio_block(); sp_render(out);
 * No factory kit is needed: the waves are synthetic, packed with sp_pack.
 *
 * The voices (milestone 1): the eight drum channels are sp_ch[0..7] as ever; with MASIF_ENABLE_TRACKER=1 the
 * tracker has four voices of its own, sp_ch[8..11] (track t = voice SP_NCH + t, TRK_VOICE), with their own
 * faders (sp_trk_mix), outside the drums' MUTE / SOLO. Built with -DMASIF_ENABLE_TRACKER=0 this file is only
 * the check of the eight-voice build: its layout and the drum reference (DRUMREF, see it_drum_ref), to be
 * compared with the line `--drum-ref` prints in the tracker build.
 *
 * Two kinds of checks, never mixed:
 *   BASELINE  what the code does today and is meant to keep doing. A failure is a regression: exit 1.
 *             "[mapping]" marks a baseline tied to the voice mapping (tracker track t plays on its own voice
 *             SP_NCH + t, see TRK_VOICE): to be updated on purpose if the tracker's voices are changed.
 *   CONTRACT  what the code is meant to do after future production changes. Each has an expected status in
 *             goal_tab[] (NOT MET until its production change lands, then MET):
 *               as expected                 reported, no failure
 *               expected NOT MET, now met   MILESTONE: exit 2 until goal_tab[] is updated on purpose
 *               expected MET, now NOT MET   REGRESSED: exit 1
 *               its scenario did not run    INVALID (nothing was tested): exit 1
 *
 * Exit: 0 all as expected, 1 regression (baseline, contract or invalid scenario), 2 milestone only,
 *       3 the harness itself, 4 --strict and contracts still NOT MET (nothing else wrong).
 *
 * The contracts, as this test words them:
 *   drum-voice isolation    MET since the tracker has its own voices. The drums and the tracker never touch
 *                           each other's voices: the mix of both is, sample for sample, the drums alone plus
 *                           the tracker alone, whatever channel a pad is on.
 *   shared-clock transport  MET since milestone 2. The tracker has no clock of its own: it runs on the
 *                           sequencer's (sp_seq.c sq.playing / sq_run / sq_ticks, read by masif_fm1_audio_block):
 *                           silent while stopped, row 0 on tick 0 at PLAY (after the count-in), halted by STOP
 *                           (its voices cut, the drums ringing out as ever), row r on tick r * 24 with no drift,
 *                           whatever the tempo does while it plays.
 *
 * Every test sets its own tempo (it_tempo) and renders its own buffers: none depends on what another left.
 *
 *   cc -std=gnu99 -O2 -Wall -Wextra -Wno-unused-function -o /tmp/masif_integration host-tests/test_integration.c
 *   /tmp/masif_integration [--strict | --drum-ref]
 *   cc -std=gnu99 -O2 -Wall -Wextra -Wno-unused-function -DMASIF_ENABLE_TRACKER=0 -o /tmp/masif_integration_off \
 *      host-tests/test_integration.c && /tmp/masif_integration_off      (the eight-voice build: layout, DRUMREF,
 *      DRUMCLK: the drums through count-in, tempo changes, STOP and PLAY again) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef MASIF_ENABLE_TRACKER
#define MASIF_ENABLE_TRACKER 1
#endif
#include "../firmware/src/sp_core.c"
#include "../firmware/src/sp_seq.c"
#if MASIF_ENABLE_TRACKER
#include "../firmware/src/masif_tracker.c"
#include "../firmware/src/masif_runtime.c"
#include "../firmware/src/masif_demo.c"             /* (loaded by t_demo only: every other test has the empty song) */
#include "../firmware/src/masif_fm1_adapter.c"
#endif

/* ---- the report */
#define NOTE "                                    "  /* a line of detail under a check */
static int base_bad;
static void base_line(int ok, const char *tag, const char *what)
{
    printf("BASELINE  %-8s %-10s        %s\n", ok ? "ok" : "FAIL", tag, what);
    base_bad += !ok;
}
static void base(int ok, const char *what) { base_line(ok, "", what); }
/* a baseline that holds only for the mapping of tracker tracks onto their own voices (TRK_VOICE) */
static void base_map(int ok, const char *what)
{
    base_line(ok, "[mapping]", what);
    if (!ok)
        printf(NOTE "(tied to track t = voice SP_NCH + t: update on purpose if the tracker's voices were changed)\n");
}

/* the contracts and the status each is expected to have today: change an entry only on purpose, with the
 * production change that moves it. ISO-*: MET since milestone 1 (the tracker's own voices). CLK-*: MET since
 * milestone 2 (the tracker on the sequencer's clock) */
enum { NOT_MET, MET };
enum { C_ISO_NOTE, C_ISO_CUT, C_ISO_SLIDE, C_ISO_PAD, C_STOPPED, C_PLAY, C_STOP, C_COUNT_IN, C_STEPS, C_DRIFT, C_N };
static const struct { const char *id; int expect; } goal_tab[C_N] = {
    [C_ISO_NOTE]  = {"ISO-1", MET},
    [C_ISO_CUT]   = {"ISO-2", MET},
    [C_ISO_SLIDE] = {"ISO-3", MET},
    [C_ISO_PAD]   = {"ISO-4", MET},
    [C_STOPPED]   = {"CLK-1", MET},
    [C_PLAY]      = {"CLK-2", MET},
    [C_STOP]      = {"CLK-3", MET},
    [C_COUNT_IN]  = {"CLK-4", MET},
    [C_STEPS]     = {"CLK-5", MET},
    [C_DRIFT]     = {"CLK-6", MET},
};
static int goal_seen[C_N], goal_met, goal_milestone, goal_regressed, goal_invalid;
/* valid: the scenario did what it takes to test the contract at all (it sounded, it reached PLAY ...) */
static void goal(int id, int valid, int ok, const char *what)
{
    int exp = goal_tab[id].expect;
    const char *verdict;
    if (goal_seen[id]++) {
        fprintf(stderr, "goal: contract %s reported twice\n", goal_tab[id].id);
        exit(3);
    }
    ok = ok != 0;
    if (!valid) {
        verdict = "INVALID";
        goal_invalid++;
    } else if (ok == exp) {
        verdict = "expected";
    } else if (ok) {
        verdict = "MILESTONE";
        goal_milestone++;
    } else {
        verdict = "REGRESSED";
        goal_regressed++;
    }
    goal_met += valid && ok;
    printf("CONTRACT  %-8s %-10s %-6s %s\n", !valid ? "-" : ok ? "met" : "NOT MET", verdict, goal_tab[id].id, what);
    if (!valid)
        printf(NOTE "the scenario did not run as set up: nothing was tested (counted as a failure)\n");
    else if (ok != exp)
        printf(NOTE "expected %s in goal_tab[]: %s\n", exp ? "MET" : "NOT MET",
               ok ? "if this is the intended production change, set it to MET"
                  : "a contract that was met is broken again");
}

/* ---- synthetic waves and the pads that play them */
enum { W_KICK, W_HAT, W_TONE, W_BLIP, W_N };
#define W_MAX 30000u
static uint8_t wave_mem[W_N][(W_MAX + 1u) / 2u * 3u];

/* a triangle of `period` samples starting at its peak (so a hit is heard in its first block), or noise */
static void it_wave(uint32_t wi, uint32_t n, uint32_t period, int decay, int noise)
{
    static int16_t pcm[W_MAX];
    uint32_t i, lcg = 12345u;
    for (i = 0; i < n; i++) {
        int32_t v;
        if (noise) {
            lcg = lcg * 1664525u + 1013904223u;
            v = (int32_t)((lcg >> 16) % 3001u) - 1500;
        } else {
            uint32_t p = i % period;
            v = p < period / 2u ? 1500 - (int32_t)(p * 6000u / period)
                                : -1500 + (int32_t)((p - period / 2u) * 6000u / period);
        }
        if (decay)
            v = v * (int32_t)(n - i) / (int32_t)n;
        pcm[i] = (int16_t)(v * 16);                 /* 12 bit in the top bits */
    }
    sp_pack(wave_mem[wi], pcm, n);
    sp_wave[wi].d = wave_mem[wi];
    sp_wave[wi].n = n;
    sp_wave[wi].rate = 26040;
}

enum { PAD_KICK1 = 0, PAD_KICK2 = 2, PAD_KICK5 = 4, PAD_HAT7 = 5, PAD_KICK6 = 6, PAD_HAT8 = 7, PAD_TONE = 8, PAD_BLIP = 9,
       PAD_KICK3 = 10, PAD_HAT4 = 11, PAD_HAT5 = 12 };
static void it_sound(uint32_t k, uint32_t w, uint32_t ch)
{
    sp_sound_t *s = &sp_sound[k];
    memset(s, 0, sizeof *s);
    s->wave = (uint8_t)w;
    s->decay = 127;
    s->level = 127;
    s->chan = (uint8_t)ch;
    s->end = 1000;
    s->cut = 127;
}

/* ---- the machine: everything back to power-on, then one audio block as fm1_alnk0_irq does it */
static uint16_t it_bpm = 1200;                      /* the tempo at power-on: set with it_tempo() only */
static int it_with_tracker = 1;                     /* 0: the tracker's block is not run (its voices stay idle) */
static uint32_t it_state;                           /* the drum voices' state after every block since it_reset, hashed */

static uint32_t it_fnv(uint32_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}
#define IT_FNV0 2166136261u

static void it_reset(void)
{
    static unsigned char sq0[sizeof sq];
    static int have;
    if (!have) {
        memcpy(sq0, &sq, sizeof sq);
        have = 1;
    }
    memcpy(&sq, sq0, sizeof sq);
    memset(sp_ch, 0, sizeof sp_ch);                 /* (every voice: the tracker's too) */
    memset(sp_bus, 0, sizeof sp_bus);
    memset(&sp_clk, 0, sizeof sp_clk);
    memset(sp_mix, 100, sizeof sp_mix);
    sp_mute = sp_solo = 0;
    it_state = IT_FNV0;
    memset(sq_seg, 0, sizeof sq_seg);
    memset(sq_lk, 0, sizeof sq_lk);
    memset(sq_rq, 0, sizeof sq_rq);
    sq_rq_w = sq_rq_r = 0;
    sq_init();
    sq_seg[0].bars = 1;
    sq.bpm10 = it_bpm;
#if MASIF_ENABLE_TRACKER
    memset(sp_trk_mix, 100, sizeof sp_trk_mix);
    masif_fm1_init();
    {   /* the tests' instruments: instrument i plays the wave of the test kit's pad i, whole, at full level, the
         * root at the wave's own pitch, no table (an instrument is a wave and how to play it, never a pad) */
        uint32_t i;
        for (i = 0; i < MASIF_INSTRUMENTS; i++) {
            masif_instrument_t *in = &masif_song.instrument[i];
            in->sample = i < SP_NSOUND && sp_sound[i].wave < MASIF_SAMPLES ? sp_sound[i].wave : MASIF_SAMPLES - 1u;
            in->table_tic = 0;
        }
        /* the tests' song: track t plays chain t round and round, chain t is the phrases t * 4 + 0..3: 64 rows of
         * its own a track (IT_ROWS), written with it_cell(track, row, ...) */
        for (i = 0; i < MASIF_TRACKS; i++) {
            uint32_t k;
            for (k = 0; k < 4u; k++) (void)masif_set_chain_step(&masif_song, (uint8_t)i, (uint8_t)k, (uint8_t)(i * 4u + k), 0);
            (void)masif_set_song_cell(&masif_song, 0, (uint8_t)i, (uint8_t)i);
        }
    }
#endif
}

/* THE MAPPING, in one place: tracker track t plays on its own voice SP_NCH + t (sp_trigger_track), after the
 * eight drum channels; no pad can be on it. Everything that goes through TRK_VOICE and every base_map() follows
 * from it */
#define TRK_VOICE(t) (&sp_ch[SP_NCH + (t)])

static void it_block(int32_t *o)
{
    memset(o, 0, 2u * SP_BLK * sizeof *o);
    sq_block();
#if MASIF_ENABLE_TRACKER
    if (it_with_tracker)
        masif_fm1_audio_block();
#endif
    sp_render(o);
    {   /* the eight drum voices only; the wave as its number, so the hash is the same in every build and run */
        static sp_ch_t v[SP_NCH];
        uint32_t i;
        memcpy(v, sp_ch, sizeof v);
        for (i = 0; i < SP_NCH; i++) {
            uint32_t wi = v[i].w ? (uint32_t)(v[i].w - sp_wave) + 1u : 0u;
            v[i].w = 0;
            it_state = it_fnv(it_fnv(it_state, &wi, sizeof wi), &v[i], sizeof v[i]);
        }
    }
}

static int it_nonzero(const int32_t *o)
{
    uint32_t i;
    for (i = 0; i < 2u * SP_BLK; i++)
        if (o[i])
            return 1;
    return 0;
}

/* ---- scenarios: what the drums play (step edits, through the request ring) and what the tracker plays */
enum { P_DRUMS = 1, P_TRACKER = 2 };
typedef void (*it_setup_fn)(unsigned parts);
typedef void (*it_script_fn)(uint32_t blk);         /* the transport keys, before block blk */

static void it_step(uint32_t pad, uint32_t tick) { sq_post(RQ_STEP, pad, tick); }
#if MASIF_ENABLE_TRACKER
#define IT_ROWS 64u                                  /* a track's rows in the tests' song: four phrases */
static void it_cell(uint32_t track, uint32_t row, uint32_t note, uint32_t inst, uint32_t fx, uint32_t param)
{
    masif_cell_t c;
    int ok;
    c.note = (uint8_t)note;
    c.instrument = (uint8_t)inst;
    c.volume = note >= 1u && note <= 120u ? 127u : 0xFFu;
    c.fx = (uint8_t)fx;
    c.param = (uint8_t)param;
    c.fx2 = c.param2 = 0;
    ok = masif_set_cell(&masif_song, (uint8_t)(track * 4u + row / MASIF_ROWS), (uint8_t)(row % MASIF_ROWS), &c);   /* (before the audio runs) */
    if (!ok) {
        fprintf(stderr, "it_cell: bad cell %u/%u\n", (unsigned)track, (unsigned)row);
        exit(3);
    }
}
#define ROOT MASIF_DEFAULT_ROOT_NOTE                /* a note at the sample's own pitch */
#define NONE 0xFFu                                  /* instrument unchanged */
#endif

static void play_at_0(uint32_t b) { if (b == 0u) sq_post(RQ_PLAY, 0, 0); }
static void play_at_100(uint32_t b) { if (b == 100u) sq_post(RQ_PLAY, 0, 0); }
#define STOP_BLK 1000u                              /* STOP is posted before this block: its first stopped block */
static void play_stop(uint32_t b)
{
    if (b == 0u) sq_post(RQ_PLAY, 0, 0);
    if (b == STOP_BLK) sq_post(RQ_STOP, 0, 0);
}
static void rec_play_at_0(uint32_t b)               /* REC armed, PLAY: a bar of count-in first */
{
    if (b == 0u) {
        sq_post(RQ_REC, 0, 0);
        sq_post(RQ_PLAY, 0, 0);
    }
}

/* the sequencer's clock step for a tempo, measured over one real block (Q16 ticks); the tempo is set after
 * power-on, as the tempo knob does it */
static uint32_t it_seq_inc(uint16_t bpm10)
{
    it_reset();
    sq.bpm10 = bpm10;
    sq_post(RQ_PLAY, 0, 0);
    sq_block();
    return sq.pos;
}
#if MASIF_ENABLE_TRACKER
/* (white box) the ticks the tracker has played since PLAY, and where that leaves it: the row and the tick in it */
static uint32_t it_trk_ticks(void) { return masif_ticks_done; }
static uint32_t it_trk_row(void) { return masif_runtime.tracker.step[0] * MASIF_ROWS + masif_runtime.tracker.row[0]; }   /* track 1's next row of its 64 */
static int it_trk_at(uint32_t ticks)
{
    return masif_runtime.tick_in_row == ticks % 24u && it_trk_row() == (ticks + 23u) / 24u % IT_ROWS;
}
#endif

/* a test's tempo: what it_reset powers on with, and the sequencer's step at it (every test calls this first) */
static uint32_t it_d;
static void it_tempo(uint16_t bpm10)
{
    it_bpm = bpm10;
    it_d = it_seq_inc(bpm10);
    if (!it_d) {
        fprintf(stderr, "it_tempo: no clock at %u\n", (unsigned)bpm10);
        exit(3);
    }
}

/* blocks to run from PLAY (posted before block 0) until the sequencer has played tick T */
static uint32_t it_blk_of_tick(uint32_t T)
{
    return (uint32_t)((((uint64_t)T + 1u) * 65536u + it_d - 1u) / it_d);
}
static uint32_t it_tick_of_blk(uint32_t b) { return (uint32_t)(((uint64_t)b * it_d) >> 16); }

/* the waves and the pads of every test */
static void it_kit(void)
{
    uint32_t i;
    it_wave(W_KICK, 8000, 260, 1, 0);               /* ~100 Hz, 0.3 s */
    it_wave(W_HAT, 2000, 0, 1, 1);                  /* noise, 77 ms */
    it_wave(W_TONE, W_MAX, 59, 0, 0);               /* ~440 Hz, 1.15 s */
    it_wave(W_BLIP, 500, 26, 0, 0);                 /* ~1 kHz, 19 ms */
    for (i = 0; i < SP_NSOUND; i++)
        sp_sound[i].wave = 0xFF;
    it_sound(PAD_KICK1, W_KICK, 0);                 /* as the factory kit: the kick on channel 1 */
    it_sound(PAD_KICK2, W_KICK, 1);
    it_sound(PAD_KICK5, W_KICK, 4);
    it_sound(PAD_KICK6, W_KICK, 5);
    it_sound(PAD_HAT7, W_HAT, 6);
    it_sound(PAD_HAT8, W_HAT, 7);
    it_sound(PAD_TONE, W_TONE, 7);                  /* tracker instruments 8 and 9 (instrument i = sound i); never */
    it_sound(PAD_BLIP, W_BLIP, 7);                  /* hit as pads: the tracker plays them on its track's voice */
    it_sound(PAD_KICK3, W_KICK, 2);                 /* the drum reference's: every channel has a pad, */
    it_sound(PAD_HAT4, W_HAT, 3);
    it_sound(PAD_HAT5, W_HAT, 4);                   /* and channel 5 two (the one cuts the other) */
}

/* ---- the drum reference: pads on all eight channels with chokes (channel 5: the hat cuts the kick, the kick
 * the hat), MUTE and SOLO turned on and off while it plays; the tracker part is every note event at once */
static void s_busy(unsigned parts)
{
    uint32_t t;
    if (parts & P_DRUMS) {
        it_step(PAD_KICK1, 0); it_step(PAD_KICK1, 192);
        it_step(PAD_KICK2, 96); it_step(PAD_KICK2, 288);
        it_step(PAD_KICK3, 48); it_step(PAD_KICK3, 240);
        it_step(PAD_KICK5, 0); it_step(PAD_HAT5, 24); it_step(PAD_KICK5, 144); it_step(PAD_HAT5, 150);
        it_step(PAD_KICK6, 96); it_step(PAD_KICK6, 300);
        for (t = 0; t < 384u; t += 48u) {
            it_step(PAD_HAT4, t + 12u);
            it_step(PAD_HAT7, t);
            it_step(PAD_HAT8, t + 24u);
        }
    }
#if MASIF_ENABLE_TRACKER
    if (parts & P_TRACKER) {
        it_cell(0, 0, ROOT, PAD_TONE, 0, 0);                         /* a note, bent up, cut, */
        it_cell(0, 1, MASIF_NOTE_NONE, NONE, MASIF_FX_PITCH_BEND, 0xF0);
        it_cell(0, 3, MASIF_NOTE_CUT, NONE, 0, 0);
        it_cell(0, 5, ROOT + 36u, PAD_TONE, 0, 0);                   /* three octaves up, bent down, off */
        it_cell(0, 6, MASIF_NOTE_NONE, NONE, MASIF_FX_PITCH_BEND, 0x0F);
        it_cell(0, 9, MASIF_NOTE_OFF, NONE, 0, 0);
        it_cell(1, 0, ROOT + 7u, PAD_BLIP, 0, 0);
        it_cell(1, 2, ROOT, PAD_TONE, 0, 0);
        it_cell(1, 4, MASIF_NOTE_OFF, NONE, 0, 0);
        it_cell(1, 8, ROOT - 12u, PAD_TONE, MASIF_FX_PITCH_BEND, 0xF0);
        it_cell(2, 1, ROOT, PAD_BLIP, MASIF_FX_RETRIGGER, 0x01);
        it_cell(2, 4, ROOT + 12u, PAD_TONE, 0, 0);
        it_cell(2, 6, MASIF_NOTE_CUT, NONE, 0, 0);
        it_cell(3, 0, ROOT - 24u, PAD_TONE, 0, 0);                   /* held under everything, */
        it_cell(3, 12, MASIF_NOTE_OFF, NONE, 0, 0);
        for (t = 16; t < IT_ROWS; t += 3u)                        /* then a note, a cut, a note ... */
            it_cell(3, t, t % 2u ? MASIF_NOTE_CUT : ROOT + t % 12u, t % 2u ? NONE : PAD_BLIP, 0, 0);
    }
#else
    (void)t;
#endif
}
static void busy_script(uint32_t b)                 /* PLAY; channel 7 muted, then channel 1 soloed, then neither */
{
    if (b == 0u) sq_post(RQ_PLAY, 0, 0);
    if (b == 300u) sp_mute = 1u << 6;
    if (b == 600u) { sp_mute = 0; sp_solo = 1u << 0; }
    if (b == 900u) sp_solo = 0;
}
#define BUSY_BLKS (it_blk_of_tick(2u * 384u - 1u))  /* the bar twice */

/* the drums alone, no tracker block run: the hash of the mix and of the eight drum voices' state block by block.
 * The same in every build (HEAD before the tracker's voices, MASIF_ENABLE_TRACKER=0 and =1) if the drums are
 * what they were */
static void it_drum_ref(void)
{
    static int32_t o[2u * SP_BLK];
    uint32_t b, n, mix = IT_FNV0, nz = 0;
    int was = it_with_tracker;
    it_tempo(1200);
    n = BUSY_BLKS;
    it_with_tracker = 0;
    it_reset();
    s_busy(P_DRUMS);
    for (b = 0; b < n; b++) {
        busy_script(b);
        it_block(o);
        nz += (uint32_t)it_nonzero(o);
        mix = it_fnv(mix, o, sizeof o);
    }
    it_with_tracker = was;
    printf("DRUMREF mix=%08x state=%08x blocks=%u sounding=%u\n", (unsigned)mix, (unsigned)it_state, (unsigned)n,
           (unsigned)nz);
}

/* ---- the transport reference: the same drums through REC + PLAY with a bar of count-in, the tempo turned, STOP,
 * PLAY again, PLAY while playing. DRUMCLK is its hash, as DRUMREF: the same in the eight-voice build and in the
 * tracker's (the sequencer's own clock is what it was, with the tracker reading it or not) */
static void it_clk_ref_script(uint32_t b)
{
    if (b == 0u) { sq.cin_bars = 1; sq_post(RQ_REC, 0, 0); sq_post(RQ_PLAY, 0, 0); }
    if (b == 1500u) sq.bpm10 = 2400;
    if (b == 1900u) sq.bpm10 = 733;
    if (b == 2400u) sq_post(RQ_STOP, 0, 0);
    if (b == 2600u) sq_post(RQ_PLAY, 0, 0);
    if (b == 3000u) sq.bpm10 = 1200;
    if (b == 3300u) sq_post(RQ_PLAY, 0, 0);
    if (b == 3900u) sq_post(RQ_STOP, 0, 0);
}
static void it_clk_ref(void)
{
    static int32_t o[2u * SP_BLK];
    uint32_t b, mix = IT_FNV0, nz = 0;
    int was = it_with_tracker;
    it_tempo(1200);
    it_with_tracker = 0;
    it_reset();
    s_busy(P_DRUMS);
    for (b = 0; b < 4000u; b++) {
        it_clk_ref_script(b);
        it_block(o);
        nz += (uint32_t)it_nonzero(o);
        mix = it_fnv(mix, o, sizeof o);
    }
    it_with_tracker = was;
    printf("DRUMCLK mix=%08x state=%08x blocks=%u sounding=%u pos=%u beat=%u playing=%u\n", (unsigned)mix,
           (unsigned)it_state, 4000u, (unsigned)nz, (unsigned)sq.pos, (unsigned)sq.beat, (unsigned)sq.playing);
}

#if !MASIF_ENABLE_TRACKER
/* ---- the build without the tracker: eight voices, as ever */
int main(void)
{
    unsigned voices = (unsigned)(sizeof sp_ch / sizeof sp_ch[0]);
    int ok = voices == 8u && SP_NCH == 8 && sizeof sp_mix == 8u;
    it_kit();
    printf("MASIF integration, MASIF_ENABLE_TRACKER=0: sp_core + sp_seq only\n");
    printf("LAYOUT  %s  voices=%u SP_NCH=%u sizeof(sp_mix)=%u sizeof(sp_ch)=%u sizeof(sq)=%u\n", ok ? "ok" : "FAIL", voices,
           (unsigned)SP_NCH, (unsigned)sizeof sp_mix, (unsigned)sizeof sp_ch, (unsigned)sizeof sq);
    it_drum_ref();
    it_clk_ref();
    return !ok;
}
#else

/* a scenario rendered into buf (n blocks of stereo Q15, the raw mix before sp_out) */
#define IT_MAXB 6000u
static int32_t it_buf[3][IT_MAXB * 2u * SP_BLK];
static void it_render(it_setup_fn setup, unsigned parts, it_script_fn script, int32_t *buf, uint32_t n)
{
    uint32_t b;
    if (n > IT_MAXB) {
        fprintf(stderr, "it_render: %u blocks\n", (unsigned)n);
        exit(3);
    }
    it_reset();
    if (setup) setup(parts);
    for (b = 0; b < n; b++) {
        if (script) script(b);
        it_block(buf + (size_t)b * 2u * SP_BLK);
    }
}

/* a scenario's onsets: the blocks where sound starts after a silent block (the mix must be silent between) */
static uint32_t it_onsets(it_setup_fn setup, unsigned parts, it_script_fn script, uint32_t n, uint32_t *list,
                          uint32_t max)
{
    int32_t o[2u * SP_BLK];
    uint32_t b, cnt = 0;
    int prev = 0;
    it_reset();
    if (setup) setup(parts);
    for (b = 0; b < n; b++) {
        int nz;
        if (script) script(b);
        it_block(o);
        nz = it_nonzero(o);
        if (nz && !prev) {
            if (cnt < max) list[cnt] = b;
            cnt++;
        }
        prev = nz;
    }
    return cnt;
}

/* superposition: drums alone + tracker alone against both together, sample for sample */
typedef struct { uint32_t ndiff, first, nz_drums, nz_tracker; } it_sup_t;
static it_sup_t it_superpose_s(it_setup_fn setup, it_script_fn script, uint32_t n)
{
    it_sup_t r = {0, 0, 0, 0};
    uint32_t b, i;
    it_render(setup, P_DRUMS, script, it_buf[0], n);
    it_render(setup, P_TRACKER, script, it_buf[1], n);
    it_render(setup, P_DRUMS | P_TRACKER, script, it_buf[2], n);
    for (b = 0; b < n; b++) {
        const int32_t *d = it_buf[0] + (size_t)b * 2u * SP_BLK, *t = it_buf[1] + (size_t)b * 2u * SP_BLK;
        const int32_t *m = it_buf[2] + (size_t)b * 2u * SP_BLK;
        r.nz_drums += (uint32_t)it_nonzero(d);
        r.nz_tracker += (uint32_t)it_nonzero(t);
        for (i = 0; i < 2u * SP_BLK; i++)
            if (d[i] + t[i] != m[i]) {
                if (!r.ndiff) r.first = b;
                r.ndiff++;
            }
    }
    return r;
}

static it_sup_t it_superpose(it_setup_fn setup, uint32_t n) { return it_superpose_s(setup, play_at_0, n); }

/* ================================ BASELINE: what holds today ================================ */

/* the tracker's notes reach real voices: cells through the edit queue, then note on, pitch, slide, note off */
static void t_adapter_voices(void)
{
    const uint32_t unity = 26040u * 65536u / SP_FS; /* a 26.04 kHz wave at its own pitch (Q16 step) */
    const sp_ch_t *v3 = TRK_VOICE(2), *v4 = TRK_VOICE(3);   /* the voices of tracks 3 and 4 */
    int32_t o[2u * SP_BLK];
    uint32_t b = 0, back = 0, p0, b0, p1, adv, uni;
    masif_cell_t c;
    int q = 1;
    it_tempo(1200);
    it_reset();
    memset(o, 0, sizeof o);
    c.note = ROOT + 12u; c.instrument = PAD_TONE; c.volume = 127; c.fx = 0; c.param = 0; c.fx2 = c.param2 = 0;
    q &= masif_post_cell_edit(&masif_edits, 8, 0, &c);           /* track 3: the tone an octave up */
    c.note = ROOT;
    q &= masif_post_cell_edit(&masif_edits, 12, 0, &c);           /* track 4: the tone, */
    c.note = MASIF_NOTE_NONE; c.instrument = NONE; c.volume = 0xFF; c.fx = MASIF_FX_PITCH_BEND; c.param = 0x10;
    q &= masif_post_cell_edit(&masif_edits, 12, 1, &c);           /* bent up for a row (ticks 25-48, 1.5 semitones), */
    c.note = MASIF_NOTE_OFF; c.fx = 0; c.param = 0;
    q &= masif_post_cell_edit(&masif_edits, 12, 3, &c);           /* and off on row 3 */
    sq_post(RQ_PLAY, 0, 0);
/* (back: the playing voice's sample position went backwards, i.e. it was retriggered) */
#define RUN_TO(T) while (b < it_blk_of_tick(T)) { uint32_t was = v4->pos; it_block(o); b++; back += v4->on && v4->pos < was; }
#define UNITY_ADV(blocks) ((uint32_t)(((uint64_t)(blocks) * SP_BLK * unity) >> 16))   /* samples at the wave's own pitch */
    RUN_TO(4);
    base(q && masif_edits.read_index == masif_edits.write_index, "cell edits posted by the main loop are applied by the audio block");
    base_map(v3->on && v3->w == &sp_wave[W_TONE] && v4->on && v4->w == &sp_wave[W_TONE] && it_nonzero(o),
             "a tracker note sounds on the real core, on its track's own voice (track t = voice 8 + t)");
    base(v3->step == 2u * unity && v4->step == unity,
         "note - root = semitones: the root at the wave's own rate, +12 at twice the step");
    p0 = v4->pos; b0 = b;
    RUN_TO(20);
    adv = v4->pos - p0; uni = UNITY_ADV(b - b0);
    base(v4->on && adv >= uni && adv <= uni + 1u, "row 0: the sample position advances at the note's pitch");
    printf(NOTE "%u blocks: %u wave samples played, %u at the wave's own pitch\n", (unsigned)(b - b0), (unsigned)adv,
           (unsigned)uni);
    RUN_TO(30);
    base(v4->step > unity && v4->base_step == unity && v4->on, "PBN raises the playing voice's step");
    p0 = v4->pos; b0 = b;
    RUN_TO(46);
    adv = v4->pos - p0; uni = UNITY_ADV(b - b0);
    p1 = v4->pos;
    base(v4->on && !back && adv > uni + 1u,
         "PBN: the playing sample goes on from where it was (no retrigger), faster than at the note's pitch");
    printf(NOTE "%u blocks of the bend: %u wave samples played, %u at the note's pitch\n", (unsigned)(b - b0),
           (unsigned)adv, (unsigned)uni);
    RUN_TO(60);
    base(v4->step > unity + unity / 16u && v4->step < unity + unity / 8u && v4->base_step == unity && v4->on && !back && v4->pos > p1,
         "the row after the bend: the pitch stays where the bend left it (1.5 semitones up), the sample still playing on");
    RUN_TO(84);
    base(!v4->on, "NOTE OFF stops the track's voice");
    RUN_TO(250);
    base(!it_nonzero(o), "everything played out: the mix is silent");
#undef UNITY_ADV
#undef RUN_TO
}

/* blips on every row of track 3 (the tracker's clock made audible) */
static void s_blip_rows(unsigned parts)
{
    uint32_t r;
    if (parts & P_TRACKER)
        for (r = 0; r < IT_ROWS; r++)
            it_cell(2, r, ROOT, PAD_BLIP, 0, 0);
}

/* the tempo knob turned while playing: power-on and PLAY at 120.0 BPM, sq.bpm10 = 240.0 from block TEMPO_BLK.
 * The rows before must be a 120 BPM row apart, the rows after a 240 BPM row apart */
static void t_tempo_link(void)
{
    enum { TEMPO_BLK = 1723, NL = 64 };             /* (10 rows at 120 BPM) */
    int32_t o[2u * SP_BLK];
    uint32_t list[NL], n = 0, b, i, n1 = 0, n2 = 0, bad1 = 0, bad2 = 0, row1, row2, lo1 = ~0u, hi1 = 0, lo2 = ~0u, hi2 = 0;
    uint16_t trk_before = 0, trk_after;
    int prev = 0;
    it_tempo(1200);
    row1 = (uint32_t)((24u * 65536u) / it_d);       /* blocks a row (24 ticks), rounded down */
    row2 = (uint32_t)((24u * 65536u) / it_seq_inc(2400));
    it_reset();
    s_blip_rows(P_TRACKER);
    sq_post(RQ_PLAY, 0, 0);
    for (b = 0; b < 2u * TEMPO_BLK; b++) {
        int nz;
        if (b == TEMPO_BLK) {
            trk_before = masif_runtime.bpm10;       /* (white box: the tracker's tempo after 10 rows of playing) */
            sq.bpm10 = 2400;                        /* as the UI writes it (zp12.c, sp_ui.c): after init, while playing */
        }
        it_block(o);
        nz = it_nonzero(o);
        if (nz && !prev && n < NL) list[n++] = b;
        prev = nz;
    }
    trk_after = masif_runtime.bpm10;
    for (i = 0; i < n; i++) {
        if (list[i] < TEMPO_BLK) n1++; else n2++;
        if (!i) continue;
        b = list[i] - list[i - 1];
        if (list[i] < TEMPO_BLK) {                  /* both rows before the change */
            bad1 += b < row1 || b > row1 + 1u;
            if (b < lo1) lo1 = b;
            if (b > hi1) hi1 = b;
        } else if (list[i - 1] >= TEMPO_BLK) {      /* both after it (the row across the change is neither) */
            bad2 += b < row2 || b > row2 + 1u;
            if (b < lo2) lo2 = b;
            if (b > hi2) hi2 = b;
        }
    }
    base(sq.playing && sq.bpm10 == 2400 && trk_before == 1200 && trk_after == 2400,
         "sq.bpm10 changed while playing reaches the tracker's clock (120.0 until the change, then 240.0)");
    base(n1 >= 3u && n2 >= 3u && n < NL && !bad1 && !bad2 && row1 >= 2u * row2 - 1u && row1 <= 2u * row2 + 1u,
         "the tracker's rows follow the new tempo during playback: a 120 BPM row apart before, a 240 BPM row after");
    printf(NOTE "rows heard: %u before block %u, %u..%u blocks apart (expected %u..%u);\n", (unsigned)n1,
           (unsigned)TEMPO_BLK, (unsigned)lo1, (unsigned)hi1, (unsigned)row1, (unsigned)(row1 + 1u));
    printf(NOTE "            %u after it, %u..%u blocks apart (expected %u..%u)\n", (unsigned)n2, (unsigned)lo2,
           (unsigned)hi2, (unsigned)row2, (unsigned)(row2 + 1u));
}

/* drums on channels 5-8 (two kicks, two hats), the tracker on tracks 1-4 (its own voices): notes, a slide,
 * a cut, a retrigger */
static void s_disjoint(unsigned parts)
{
    uint32_t t;
    if (parts & P_DRUMS) {
        it_step(PAD_KICK5, 0);
        it_step(PAD_KICK5, 192);
        it_step(PAD_KICK6, 96);
        it_step(PAD_KICK6, 288);
        for (t = 0; t < 384u; t += 48u) {
            it_step(PAD_HAT7, t);
            it_step(PAD_HAT8, t + 24u);
        }
    }
    if (parts & P_TRACKER) {
        it_cell(0, 0, ROOT, PAD_TONE, 0, 0);
        it_cell(0, 2, MASIF_NOTE_NONE, NONE, MASIF_FX_PITCH_BEND, 0xF0);
        it_cell(0, 4, MASIF_NOTE_CUT, NONE, 0, 0);
        it_cell(1, 1, ROOT + 7u, PAD_BLIP, 0, 0);
        it_cell(2, 3, ROOT + 12u, PAD_BLIP, 0, 0);
        it_cell(3, 2, ROOT, PAD_BLIP, MASIF_FX_RETRIGGER, 0x01);
    }
}

static void t_disjoint(void)
{
    uint32_t n, i, diff = 0, nz = 0, b;
    it_sup_t r;
    it_tempo(1200);
    n = it_blk_of_tick(383);
    r = it_superpose(s_disjoint, n);
    base_map(r.nz_drums && r.nz_tracker && !r.ndiff,
             "pads on channels 5-8 and the tracker on tracks 1-4 (its own voices): the mix is exactly drums + tracker");
    printf(NOTE "%u samples differ over %u blocks (drums sound in %u, the tracker in %u)\n", (unsigned)r.ndiff,
           (unsigned)n, (unsigned)r.nz_drums, (unsigned)r.nz_tracker);
    /* its own two renders: the drums with an empty tracker song running, and with no tracker in the build */
    it_render(s_disjoint, P_DRUMS, play_at_0, it_buf[0], n);
    it_with_tracker = 0;
    it_render(s_disjoint, P_DRUMS, play_at_0, it_buf[1], n);
    it_with_tracker = 1;
    for (b = 0; b < n; b++)
        nz += (uint32_t)it_nonzero(it_buf[0] + (size_t)b * 2u * SP_BLK);
    for (i = 0; i < n * 2u * SP_BLK; i++)
        diff += it_buf[0][i] != it_buf[1][i];
    base(nz && !diff, "an empty tracker song changes nothing: the drums are as without the tracker");
}

/* a hat on every quarter of the sequencer's bar, a blip on every fourth row of the tracker */
static void s_quarters(unsigned parts)
{
    uint32_t i;
    if (parts & P_DRUMS)
        for (i = 0; i < 4u; i++)
            it_step(PAD_HAT7, i * SQ_PPQ);
    if (parts & P_TRACKER)
        for (i = 0; i < IT_ROWS; i += 4u)
            it_cell(2, i, ROOT, PAD_BLIP, 0, 0);
}

#define IT_NLIST 512u
/* the beats of both, block by block: how many there are of each and how far apart they get (at it_tempo's tempo) */
static uint32_t it_beats(uint32_t bars, uint32_t *nd, uint32_t *nt)
{
    static uint32_t on_d[IT_NLIST], on_t[IT_NLIST];
    uint32_t n = it_blk_of_tick(bars * SQ_BAR - 1u), i, k, worst = 0;
    *nd = it_onsets(s_quarters, P_DRUMS, play_at_0, n, on_d, IT_NLIST);
    *nt = it_onsets(s_quarters, P_TRACKER, play_at_0, n, on_t, IT_NLIST);
    k = *nd < *nt ? *nd : *nt;
    if (k > IT_NLIST) k = IT_NLIST;
    for (i = 0; i < k; i++) {
        uint32_t e = on_d[i] > on_t[i] ? on_d[i] - on_t[i] : on_t[i] - on_d[i];
        if (e > worst) worst = e;
    }
    return worst;
}

#define DRIFT_BARS 64u
static void t_aligned_when_equal(void)
{
    uint32_t nd, nt, worst;
    it_tempo(1200);
    worst = it_beats(DRIFT_BARS, &nd, &nt);
    base(nd == DRIFT_BARS * 4u && nt == nd && worst == 0u,
         "120.0 BPM, PLAY in the first block after power-on: no drift, every tracker beat in the sequencer's block");
    printf(NOTE "clock step a block (Q16 ticks): %u; over %u bars beats %u / %u, worst %u blocks apart\n",
           (unsigned)it_d, (unsigned)DRIFT_BARS, (unsigned)nd, (unsigned)nt, (unsigned)worst);
}

/* REC armed + PLAY with COUNT = 1 bar, set here and not left to the power-on default */
static void s_count_in(unsigned parts);
static uint32_t first_diff(const int32_t *a, const int32_t *b, uint32_t n);
static void t_count_in_seq(void)
{
    int32_t o[2u * SP_BLK];
    uint32_t n, b, waited = 0, moved = 0, clicks = 0, beats = 0, expect, d;
    int armed = 0;
    it_tempo(1200);
    n = it_blk_of_tick(2u * SQ_BAR);
    expect = ((SQ_BAR << 16) - 1u) / it_d;          /* blocks that end with count-in left: a bar of ticks */
    it_reset();
    s_count_in(P_DRUMS);
    sq_post(RQ_REC, 0, 0);
    sq_post(RQ_PLAY, 0, 0);
    for (b = 0; b < n; b++) {
        it_block(o);
        if (!b)
            armed = sq.cin_bars == 1u && sq.playing && sq.recording && sq.cin_len == SQ_BAR && sq.countin > 0;
        if (sq.countin > 0) {
            waited++;
            moved += sq.pos != 0u;
            clicks += (uint32_t)it_nonzero(o);
            beats = sq.beat;
        }
    }
    base(armed, "count-in set up: sq.cin_bars = 1, REC armed, PLAY: a bar (384 ticks) of count-in starts");
    base(waited == expect && !moved && beats == 4u && clicks,
         "count-in: the sequencer waits its bar (the playhead stays at 0, four clicks)");
    printf(NOTE "%u blocks of count-in (a bar is %u), %u beats clicked\n", (unsigned)waited, (unsigned)expect,
           (unsigned)beats);
    it_render(s_count_in, 0, rec_play_at_0, it_buf[0], n);
    it_render(s_count_in, P_DRUMS, rec_play_at_0, it_buf[1], n);
    d = first_diff(it_buf[0], it_buf[1], n);
    base(d != 0xFFFFFFFFu && d >= waited && d - waited <= it_blk_of_tick(0),
         "count-in: the sequencer's tick 0 sounds right after it, not before");
    printf(NOTE "sequencer tick 0 in block %u\n", (unsigned)d);
}

/* =================== BASELINE: the tracker's own voices (milestone 1) =================== */

static uint32_t it_ndiff(const int32_t *a, const int32_t *b, uint32_t n)
{
    uint32_t i, d = 0;
    for (i = 0; i < n * 2u * SP_BLK; i++)
        d += a[i] != b[i];
    return d;
}
static uint32_t it_peak(const int32_t *a, uint32_t n)
{
    uint32_t i, pk = 0;
    for (i = 0; i < n * 2u * SP_BLK; i++) {
        uint32_t v = (uint32_t)(a[i] < 0 ? -a[i] : a[i]);
        if (v > pk) pk = v;
    }
    return pk;
}

static void t_voice_layout(void)
{
    base(SP_NCH == 8 && sizeof sp_mix == 8u && sizeof sp_mute == 1u && sizeof sp_solo == 1u,
         "the drums' layout is as ever: SP_NCH 8, eight faders in sp_mix (what is saved), eight MUTE / SOLO bits");
    base(SP_NTRK == MASIF_TRACKS && SP_NVOICE == 12 && sizeof sp_ch / sizeof sp_ch[0] == 12u && sizeof sp_trk_mix == 4u,
         "MASIF_ENABLE_TRACKER=1: four tracker voices after the eight channels (sp_ch[8..11]), their own four faders");
    printf(NOTE "sizeof(sp_ch_t) %u: the four voices cost %u bytes of RAM on the host (the target's is to be measured)\n",
           (unsigned)sizeof(sp_ch_t), (unsigned)(SP_NTRK * sizeof(sp_ch_t) + sizeof sp_trk_mix));
}

/* the drum reference with the tracker playing notes, slides, note-offs, cuts and retriggers at the same time */
static void t_drums_identical(void)
{
    uint32_t n, idle, ref, trk, both, b, nd = 0, nt = 0;
    it_tempo(1200);
    n = BUSY_BLKS;
    it_with_tracker = 0;
    it_render(s_busy, 0, busy_script, it_buf[0], n);
    idle = it_state;
    it_render(s_busy, P_DRUMS, busy_script, it_buf[0], n);       /* the reference: no tracker block at all */
    ref = it_state;
    it_with_tracker = 1;
    it_render(s_busy, P_TRACKER, busy_script, it_buf[1], n);
    trk = it_state;
    it_render(s_busy, P_DRUMS | P_TRACKER, busy_script, it_buf[2], n);
    both = it_state;
    for (b = 0; b < n; b++) {
        nd += (uint32_t)it_nonzero(it_buf[0] + (size_t)b * 2u * SP_BLK);
        nt += (uint32_t)it_nonzero(it_buf[1] + (size_t)b * 2u * SP_BLK);
    }
    for (b = 0; b < n * 2u * SP_BLK; b++)
        it_buf[0][b] += it_buf[1][b];
    base(nd && nt && ref == both && ref != idle,
         "the eight drum voices, block by block, are the drums-only reference's while the tracker plays, slides, stops and cuts");
    base(nt && trk == idle, "the tracker alone never touches a drum voice (all eight stay as at power-on)");
    base(nd && nt && !it_ndiff(it_buf[0], it_buf[2], n),
         "all eight channels with chokes, MUTE and SOLO + the busy tracker: the mix is the drums-only reference + the tracker");
    printf(NOTE "%u blocks: drums sound in %u, the tracker in %u; drum voice state %08x (reference %08x)\n", (unsigned)n,
           (unsigned)nd, (unsigned)nt, (unsigned)both, (unsigned)ref);
}

/* a second pad on a channel cuts the first: without the tracker, then with its voices busy (to the bit the same) */
static void t_choke(void)
{
    static sp_ch_t ref[SP_NCH];
    int32_t o[2u * SP_BLK];
    uint32_t pass, i, t, on;
    int ok = 1, same = 0;
    it_tempo(1200);
    for (pass = 0; pass < 2u; pass++) {
        const sp_ch_t *c = &sp_ch[4];
        it_reset();
        sp_trigger(PAD_KICK5, 127);
        if (pass)
            for (t = 0; t < SP_NTRK; t++)
                sp_trigger_track(PAD_TONE, 127, (int32_t)t, t);
        for (i = 0; i < 10u; i++) { memset(o, 0, sizeof o); sp_render(o); }
        ok &= c->on && c->w == &sp_wave[W_KICK] && c->pos > 0u;
        if (pass) {
            sp_pitch_track(0, 48);
            sp_stop_track(1);
            sp_trigger_track(PAD_BLIP, 127, 0, 2);
            ok &= TRK_VOICE(0)->on && !TRK_VOICE(1)->on && TRK_VOICE(2)->w == &sp_wave[W_BLIP] && TRK_VOICE(3)->on;
        }
        sp_trigger(PAD_HAT5, 127);                  /* the same channel: the kick is cut */
        for (on = 0, i = 0; i < SP_NCH; i++) on += sp_ch[i].on;
        ok &= on == 1u && c->on && c->w == &sp_wave[W_HAT] && c->pos == 0u && c->jump;
        for (i = 0; i < 5u; i++) { memset(o, 0, sizeof o); sp_render(o); }
        ok &= c->w == &sp_wave[W_HAT] && !c->jump;
        if (!pass) memcpy(ref, sp_ch, sizeof ref);
        else same = !memcmp(ref, sp_ch, sizeof ref);
    }
    base(ok, "choke: a second pad on a channel cuts the first (one sound a channel, SMOOTH bridging the cut)");
    base(same, "choke: the drum voices are to the bit the same with the tracker's voices sounding, slid, stopped, retriggered");
}

static void it_event(uint8_t type, uint8_t track)
{
    masif_audio_event_t e;
    memset(&e, 0, sizeof e);
    e.type = type; e.track = track; e.instrument = PAD_TONE;
    e.note = ROOT + 12u; e.velocity = 127; e.pitch_offset = 12 * MASIF_PITCH_SEMI;
    masif_audio_event(0, &e);
}

/* all eight drum channels ringing: what the tracker's calls may and may not reach */
static void t_track_bounds(void)
{
    static const uint32_t bad[] = {4, 5, 7, 8, 11, 12, 255, 256, 0x80000000u, 0xFFFFFFFCu, 0xFFFFFFFFu};
    static const uint8_t types[] = {MASIF_EVENT_NOTE_ON, MASIF_EVENT_PITCH, MASIF_EVENT_NOTE_OFF, MASIF_EVENT_CUT,
                                    MASIF_EVENT_RETRIGGER};
    static const uint8_t pads[SP_NCH] = {PAD_KICK1, PAD_KICK2, PAD_KICK3, PAD_HAT4, PAD_KICK5, PAD_KICK6, PAD_HAT7, PAD_HAT8};
    static sp_ch_t snap[SP_NVOICE];
    int32_t o[2u * SP_BLK];
    uint32_t i, k, t, on = 0;
    int refused, own = 1;
    it_tempo(1200);
    it_reset();
    for (i = 0; i < SP_NCH; i++) sp_trigger(pads[i], 127);
    sp_trigger_track(PAD_TONE, 127, 0, 1);
    for (i = 0; i < 3u; i++) { memset(o, 0, sizeof o); sp_render(o); }
    for (i = 0; i < SP_NCH; i++) on += sp_ch[i].on;
    memcpy(snap, sp_ch, sizeof snap);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        sp_trigger_track(PAD_TONE, 127, 12, bad[i]);
        sp_pitch_track(bad[i], 24);
        sp_stop_track(bad[i]);
    }
    for (t = MASIF_TRACKS; t < 256u; t++)           /* and through the adapter, as the runtime's events come */
        for (k = 0; k < sizeof types; k++)
            it_event(types[k], (uint8_t)t);
    refused = !memcmp(snap, sp_ch, sizeof snap);
    base(on == SP_NCH && TRK_VOICE(1)->on && refused,
         "a track outside 0-3 is refused by note on, pitch, note off, cut and retrigger: no voice changes (never wrapped onto a channel)");
    for (t = 0; t < SP_NTRK; t++) {
        sp_trigger_track(PAD_TONE, 127, 0, t);
        own &= TRK_VOICE(t)->on && TRK_VOICE(t)->w == &sp_wave[W_TONE];
        sp_pitch_track(t, 48);
        sp_pitch_track(t, -48);
        own &= TRK_VOICE(t)->step < TRK_VOICE(t)->base_step;
        own &= !memcmp(snap, sp_ch, SP_NCH * sizeof sp_ch[0]);
        sp_stop_track(t);
        own &= !TRK_VOICE(t)->on;
        for (k = 0; k < sizeof types; k++)
            it_event(types[k], (uint8_t)t);
    }
    own &= !memcmp(snap, sp_ch, SP_NCH * sizeof sp_ch[0]);
    base(own, "note on, pitch, note off, cut and retrigger on tracks 0-3 change that track's voice only: eight ringing drum voices untouched");
}

/* however far a tracker pitch goes, its voice steps 4 samples a sample at the most; a pad's pitch is as it was */
static void t_pitch_clamp(void)
{
    const uint32_t unity = 26040u * 65536u / SP_FS;
    const uint32_t top = (uint32_t)(((uint64_t)unity * sp_pow2_cents(2450)) >> 16);   /* a hit's most: +24.5 semitones */
    const sp_ch_t *v = TRK_VOICE(0);
    int32_t o[2u * SP_BLK];
    uint32_t p0, adv, s_up, s_down, s_ev;
    it_tempo(1200);
    it_reset();
    sp_trigger_track(PAD_TONE, 127, 120, 0);
    sp_pitch_track(0, 127);
    s_up = v->step;
    p0 = v->pos;
    memset(o, 0, sizeof o);
    sp_render(o);
    adv = v->pos - p0;
    sp_pitch_track(0, -128);
    s_down = v->step;
    it_reset();
    it_event(MASIF_EVENT_NOTE_ON, 0);
    it_event(MASIF_EVENT_PITCH, 0);
    {
        masif_audio_event_t e;
        memset(&e, 0, sizeof e);
        masif_song.instrument[PAD_TONE].root_note = 1;
        e.type = MASIF_EVENT_NOTE_ON; e.instrument = PAD_TONE; e.note = 120; e.velocity = 127; e.pitch_offset = 127 * MASIF_PITCH_SEMI;
        masif_audio_event(0, &e);
    }
    s_ev = v->step;
    base(v->base_step > SP_TRK_STEP_MAX && top < SP_TRK_STEP_MAX && s_up == SP_TRK_STEP_MAX && s_ev == SP_TRK_STEP_MAX && adv <= 4u * SP_BLK && adv > 0u && s_down > 0u,
         "an extreme tracker pitch is capped: the voice's step stops at 4.0, at most 4 wave samples an output sample");
    printf(NOTE "note +120 semitones, pitch +127: step %u.%03u (cap %u.000), %u wave samples in a block of %u; pitch -128: step 0.%03u\n",
           (unsigned)(s_up >> 16), (unsigned)((s_up & 0xFFFFu) * 1000u >> 16), (unsigned)(SP_TRK_STEP_MAX >> 16), (unsigned)adv,
           (unsigned)SP_BLK, (unsigned)((s_down & 0xFFFFu) * 1000u >> 16));
    it_reset();
    sp_trigger_at(PAD_TONE, 127, 60);               /* a pad (channel 8), MULTI PITCH far up */
    base(sp_ch[7].on && sp_ch[7].step == top && sp_ch[7].base_step == top && !TRK_VOICE(0)->on,
         "a pad's pitch is as before: its own clamp (+24.5 semitones), no tracker cap, on its own channel");
}

/* the tracker voices' own fader, and the drums' MUTE / SOLO / faders that do not reach them */
static void s_busy_mute_all(unsigned parts) { s_busy(parts); sp_mute = 0xFF; }
static void s_busy_solo1(unsigned parts) { s_busy(parts); sp_solo = 1; }
static void s_busy_faders0(unsigned parts) { s_busy(parts); memset(sp_mix, 0, sizeof sp_mix); }
static void s_busy_trk0(unsigned parts) { s_busy(parts); memset(masif_song.track_level, 0, sizeof masif_song.track_level); }
static void s_busy_trk50(unsigned parts) { s_busy(parts); memset(masif_song.track_level, 50, sizeof masif_song.track_level); }

static void t_gain_mute(void)
{
    uint32_t n, pk, pk50, d_mute, d_solo, d_fad;
    it_tempo(1200);
    n = BUSY_BLKS;
    it_render(s_busy, P_TRACKER, play_at_0, it_buf[0], n);
    pk = it_peak(it_buf[0], n);
    it_render(s_busy_mute_all, P_TRACKER, play_at_0, it_buf[1], n);
    d_mute = it_ndiff(it_buf[0], it_buf[1], n);
    it_render(s_busy_solo1, P_TRACKER, play_at_0, it_buf[1], n);
    d_solo = it_ndiff(it_buf[0], it_buf[1], n);
    it_render(s_busy_faders0, P_TRACKER, play_at_0, it_buf[1], n);
    d_fad = it_ndiff(it_buf[0], it_buf[1], n);
    base(pk && !d_mute && !d_solo,
         "the drums' MUTE (all eight) and SOLO (channel 1) do not reach the tracker's voices: heard as without, sample for sample");
    base(pk && !d_fad, "the eight drum faders (sp_mix) at 0 do not reach the tracker's voices");
    it_render(s_busy_trk50, P_TRACKER, play_at_0, it_buf[1], n);
    pk50 = it_peak(it_buf[1], n);
    it_render(s_busy_trk0, P_TRACKER, play_at_0, it_buf[2], n);
    base(pk && pk50 * 4u >= pk - pk / 20u && pk50 * 4u <= pk + pk / 20u && !it_peak(it_buf[2], n),
         "the tracker's mixer (the song's track levels, sp_trk_mix): 100 as set, 50 a quarter (-12 dB), 0 silent");
    printf(NOTE "tracker peak %u at 100, %u at 50\n", (unsigned)pk, (unsigned)pk50);
    it_render(s_busy, P_DRUMS, play_at_0, it_buf[0], n);
    it_render(s_busy_trk0, P_DRUMS | P_TRACKER, play_at_0, it_buf[1], n);
    it_render(s_busy_mute_all, P_DRUMS, play_at_0, it_buf[2], n);
    base(it_peak(it_buf[0], n) && !it_ndiff(it_buf[0], it_buf[1], n) && !it_peak(it_buf[2], n),
         "the tracker's faders do not reach the drums (at 0 the mix is the drums alone); MUTE still mutes the drums");
}

/* ============ CONTRACT: drum-voice isolation (MET since the tracker's own voices) ============
 * A pad and a track with the same number, which shared a channel before milestone 1 (track t played on channel
 * t): they may never collide again. */

static void s_note_on_pad_channel(unsigned parts)   /* a kick on channel 1 rings; track 1 plays a note */
{
    if (parts & P_DRUMS) it_step(PAD_KICK1, 0);
    if (parts & P_TRACKER) it_cell(0, 1, ROOT, PAD_TONE, 0, 0);
}
static void s_cut_on_pad_channel(unsigned parts)    /* a kick on channel 1 rings; track 1 has a note cut */
{
    if (parts & P_DRUMS) it_step(PAD_KICK1, 0);
    if (parts & P_TRACKER) it_cell(0, 1, MASIF_NOTE_CUT, NONE, 0, 0);
}
static void s_slide_on_pad_channel(unsigned parts)  /* track 1's blip is over, its slide runs on; then the kick */
{
    if (parts & P_DRUMS) it_step(PAD_KICK1, 30);
    if (parts & P_TRACKER) {
        it_cell(0, 0, ROOT, PAD_BLIP, 0, 0);
        it_cell(0, 1, MASIF_NOTE_NONE, NONE, MASIF_FX_PITCH_BEND, 0xF0);
    }
}
static void s_pad_on_track_channel(unsigned parts)  /* track 2 holds a tone; a kick on channel 2 */
{
    if (parts & P_DRUMS) it_step(PAD_KICK2, 48);
    if (parts & P_TRACKER) it_cell(1, 0, ROOT, PAD_TONE, 0, 0);
}

static void t_isolation(int id, const char *what, it_setup_fn setup, int tracker_sounds)
{
    it_sup_t r;
    int valid;
    it_tempo(1200);
    r = it_superpose(setup, it_blk_of_tick(383));
    valid = r.nz_drums && (r.nz_tracker || !tracker_sounds);
    goal(id, valid, !r.ndiff, what);
    if (!valid)
        printf(NOTE "(drums sounded in %u blocks, the tracker in %u)\n", (unsigned)r.nz_drums, (unsigned)r.nz_tracker);
    else if (r.ndiff)
        printf(NOTE "drums + tracker is not the mix of both: %u samples differ, from block %u (tick %u)\n",
               (unsigned)r.ndiff, (unsigned)r.first, (unsigned)it_tick_of_blk(r.first));
}

/* ====================== CONTRACT: shared-clock transport (milestone 2) ====================== */

static void t_stopped_silent(void)
{
    uint32_t list[4], n, played;
    it_tempo(1200);
    played = it_onsets(s_blip_rows, P_TRACKER, play_at_0, 400, list, 4);   /* (the scenario does sound with PLAY) */
    n = it_onsets(s_blip_rows, P_TRACKER, 0, 2000, list, 4);
    goal(C_STOPPED, played > 0u, n == 0u, "transport stopped (no PLAY): the tracker does not run");
    if (n)
        printf(NOTE "%u tracker rows sounded in 2000 blocks without PLAY, the first in block %u\n",
               (unsigned)n, (unsigned)list[0]);
}

static void s_downbeat(unsigned parts)              /* one hat on the sequencer's 1, one blip on row 0 */
{
    if (parts & P_DRUMS) it_step(PAD_HAT7, 0);
    if (parts & P_TRACKER) it_cell(2, 0, ROOT, PAD_BLIP, 0, 0);
}
static void s_count_in(unsigned parts)              /* the same, with GLO > CLICK > COUNT at one bar */
{
    sq.cin_bars = 1;
    s_downbeat(parts);
}

static uint32_t first_from(const uint32_t *list, uint32_t n, uint32_t from)
{
    uint32_t i;
    for (i = 0; i < n && i < 64u; i++)
        if (list[i] >= from)
            return list[i];
    return 0xFFFFFFFFu;
}

static void t_play_aligns(void)
{
    uint32_t ld[64], lt[64], n = 100u + 12000u, nd, nt, d, t;
    it_tempo(1200);
    nd = it_onsets(s_downbeat, P_DRUMS, play_at_100, n, ld, 64);
    nt = it_onsets(s_downbeat, P_TRACKER, play_at_100, n, lt, 64);
    d = first_from(ld, nd, 100);
    t = first_from(lt, nt, 100);
    goal(C_PLAY, d != 0xFFFFFFFFu, d == t, "PLAY after 100 idle blocks: tracker row 0 sounds with sequencer tick 0");
    if (d != t) {
        if (t == 0xFFFFFFFFu)
            printf(NOTE "sequencer tick 0 in block %u; tracker row 0 not heard in %u blocks after PLAY\n",
                   (unsigned)d, (unsigned)(n - 100u));
        else
            printf(NOTE "sequencer tick 0 in block %u, tracker row 0 in block %u: %d blocks (~%d ticks) apart\n",
                   (unsigned)d, (unsigned)t, (int)(t - d), (int)it_tick_of_blk(t > d ? t - d : d - t));
    }
}

static void t_stop_halts(void)
{
    uint32_t list[64], n, i, before = 0, after = 0;
    it_tempo(1200);
    n = it_onsets(s_blip_rows, P_TRACKER, play_stop, STOP_BLK + 2000u, list, 64);
    for (i = 0; i < n && i < 64u; i++) {
        if (list[i] >= STOP_BLK) after++;           /* (STOP_BLK is the first stopped block: a row in it is after STOP) */
        else before++;
    }
    goal(C_STOP, before > 0u && n <= 64u, after == 0u, "STOP: the tracker stops with the sequencer");
    printf(NOTE "tracker rows heard: %u before STOP (blocks 0-%u), %u from block %u on (2000 blocks)\n",
           (unsigned)before, (unsigned)(STOP_BLK - 1u), (unsigned)after, (unsigned)STOP_BLK);
}

/* REC armed + PLAY: a bar of clicks first. The clicks are in every render, so a part is what differs from none */
static uint32_t first_diff(const int32_t *a, const int32_t *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n * 2u * SP_BLK; i++)
        if (a[i] != b[i])
            return i / (2u * SP_BLK);
    return 0xFFFFFFFFu;
}

static void t_count_in(void)
{
    uint32_t n, d, t;
    it_tempo(1200);
    n = it_blk_of_tick(2u * SQ_BAR);
    it_render(s_count_in, 0, rec_play_at_0, it_buf[0], n);
    it_render(s_count_in, P_DRUMS, rec_play_at_0, it_buf[1], n);
    it_render(s_count_in, P_TRACKER, rec_play_at_0, it_buf[2], n);
    d = first_diff(it_buf[0], it_buf[1], n);
    t = first_diff(it_buf[0], it_buf[2], n);
    goal(C_COUNT_IN, d != 0xFFFFFFFFu && d >= SQ_BAR * 65536u / it_d, d == t,
         "count-in (1 bar): the tracker waits for it, row 0 with the sequencer's tick 0");
    if (t == 0xFFFFFFFFu)
        printf(NOTE "sequencer tick 0 in block %u; tracker row 0 not heard in %u blocks\n", (unsigned)d, (unsigned)n);
    else
        printf(NOTE "sequencer tick 0 in block %u (after the count-in), tracker row 0 in block %u\n", (unsigned)d,
               (unsigned)t);
}

/* the tempos the sequencer can be set to (zp12.c, sp_ui.c, sp_store.c clamp sq.bpm10 to 400..2400), then the
 * beats over 64 bars at a tempo where the tracker's own clock, when it had one, stepped differently (229.5 BPM:
 * 17461 against the sequencer's 17462 a block) */
#define BPM_MIN 400u
#define BPM_MAX 2400u
#define DRIFT_BPM 2295u
#define CLOCK_TICKS 50u                             /* a tempo is run until the sequencer has played two rows */
static void t_one_clock(void)
{
    uint32_t bpm, differ = 0, first = 0, nd, nt, worst, blocks = 0, ran = 0;
    for (bpm = BPM_MIN; bpm <= BPM_MAX; bpm++) {
        uint32_t d = it_seq_inc((uint16_t)bpm), b, bad = 0;
        uint64_t clk = 0;                           /* the sequencer's clock, kept here: d a block from PLAY */
        it_reset();
        sq.bpm10 = (uint16_t)bpm;
        s_blip_rows(P_TRACKER);
        sq_post(RQ_PLAY, 0, 0);
        for (b = 0; (clk >> 16) < CLOCK_TICKS; b++) {
            sq_block();
            masif_fm1_audio_block();
            clk += d;
            bad += it_trk_ticks() != (uint32_t)(clk >> 16) || !it_trk_at((uint32_t)(clk >> 16)) ||
                   (sq.pos >> 16) != (uint32_t)(clk >> 16) % SQ_BAR;
        }
        blocks += b;
        ran += it_trk_ticks() == CLOCK_TICKS && TRK_VOICE(2)->on;
        if (bad) {
            differ++;
            if (!first) first = bpm;
        }
    }
    goal(C_STEPS, ran == BPM_MAX - BPM_MIN + 1u, differ == 0u,
         "one clock: in every block the tracker is on the sequencer's tick, at every supported tempo (40.0-240.0 BPM)");
    printf(NOTE "%u of %u tempos in 40.0-240.0 BPM leave the tracker off the sequencer's tick (%u blocks checked)",
           (unsigned)differ, (unsigned)(BPM_MAX - BPM_MIN + 1u), (unsigned)blocks);
    if (differ)
        printf(" (the first: %u.%u BPM)", (unsigned)(first / 10u), (unsigned)(first % 10u));
    printf("\n");

    it_tempo(DRIFT_BPM);
    worst = it_beats(DRIFT_BARS, &nd, &nt);
    goal(C_DRIFT, nd == DRIFT_BARS * 4u, nd == nt && worst == 0u,
         "one clock: no drift over 64 bars at 229.5 BPM (where the tracker's own clock used to step differently)");
    printf(NOTE "at %u.%u BPM (step %u): beats %u / %u, up to %u blocks (%u samples) apart after %u bars\n",
           (unsigned)(DRIFT_BPM / 10u), (unsigned)(DRIFT_BPM % 10u), (unsigned)it_d, (unsigned)nd,
           (unsigned)nt, (unsigned)worst, (unsigned)(worst * SP_BLK), (unsigned)DRIFT_BARS);
}

/* ================== BASELINE: the shared clock, case by case (milestone 2) ================== */

/* zp12.c's autosave: "quiet" is the transport stopped and no voice sounding (the tracker's too) */
static int it_quiet(void)
{
    uint32_t k, quiet = !sq.playing;
    for (k = 0; k < SP_NVOICE; k++) quiet &= !sp_ch[k].on;
    return (int)quiet;
}
static uint32_t it_trk_on(void)
{
    uint32_t t, n = 0;
    for (t = 0; t < SP_NTRK; t++) n += TRK_VOICE(t)->on;
    return n;
}

/* a blip on every row, and the blip as a pad (channel 8) on every row's tick: the two clocks made audible, one
 * against the other (19 ms: silent again before the next row at any tempo) */
static void s_rows_both(unsigned parts)
{
    uint32_t r;
    if (parts & P_DRUMS)
        for (r = 0; r < SQ_BAR / 24u; r++)
            it_step(PAD_BLIP, r * 24u);
    s_blip_rows(parts);
}
/* tones held on all four tracks from row 0 (1.15 s each: still sounding whenever STOP comes), a kick on the 1 */
static void s_held(unsigned parts)
{
    uint32_t t;
    if (parts & P_DRUMS) it_step(PAD_KICK5, 0);
    if (parts & P_TRACKER)
        for (t = 0; t < MASIF_TRACKS; t++)
            it_cell(t, 0, ROOT - 12u, PAD_TONE, 0, 0);   /* (an octave down: 2.3 s) */
}

/* the drums' and the tracker's onsets of a scenario under a transport script: the same blocks, one for one */
static int it_same_onsets(it_setup_fn setup, it_script_fn script, uint32_t n, uint32_t *count)
{
    static uint32_t on_d[IT_NLIST], on_t[IT_NLIST];
    uint32_t nd = it_onsets(setup, P_DRUMS, script, n, on_d, IT_NLIST);
    uint32_t nt = it_onsets(setup, P_TRACKER, script, n, on_t, IT_NLIST);
    *count = nd;
    return nd && nd <= IT_NLIST && nd == nt && !memcmp(on_d, on_t, nd * sizeof on_d[0]);
}

/* PLAY: row r is the sequencer's tick r * 24, heard in the block the sequencer plays that tick in */
static void play_at_333(uint32_t b) { if (b == 333u) sq_post(RQ_PLAY, 0, 0); }
static void t_clk_play(void)
{
    static const uint16_t bpms[] = {400, 900, 1200, 1337, 2400};
    uint32_t list[IT_NLIST], i, k, n, rows = 0, bad = 0, cnt;
    int same = 1;
    for (k = 0; k < sizeof bpms / sizeof bpms[0]; k++) {
        it_tempo(bpms[k]);
        n = it_onsets(s_blip_rows, P_TRACKER, play_at_333, 333u + it_blk_of_tick(40u * 24u), list, IT_NLIST);
        bad += n != 41u;
        for (i = 0; i < n && i < IT_NLIST; i++)
            bad += list[i] != 333u + it_blk_of_tick(i * 24u) - 1u;
        rows += n;
        same &= it_same_onsets(s_rows_both, play_at_333, 333u + it_blk_of_tick(2u * SQ_BAR - 1u), &cnt) && cnt == 32u;
    }
    base(rows && !bad, "PLAY (after 333 idle blocks) at 40, 90, 120, 133.7, 240 BPM: row r sounds in the block of the sequencer's tick r * 24");
    base(same, "... and a blip a row starts, block for block, with a pad on every 24th tick, over the loop's end too (2 bars)");
    printf(NOTE "%u rows timed against the sequencer's playhead, %u off\n", (unsigned)rows, (unsigned)bad);
}

/* STOP with a tone held on every track and a kick ringing: the tracker's voices are cut in that block, nothing
 * of it moves while stopped, the drums ring out as without it, and the autosave's quiet comes when they have */
static void t_clk_stop(void)
{
    int32_t o[2u * SP_BLK];
    uint32_t b, on_before = 0, on_stop = 9, late = 0, moved = 0, t0 = 0, r0 = 0, quiet_at = 0, quiet_ref = 0, st, st_ref, pass;
    uint32_t tail = 0, loud = 0;
    int kick_rings = 0, never = 1, drums_same;
    it_tempo(1200);
    for (pass = 0; pass < 2u; pass++) {             /* the drums alone with no tracker in the block, then both */
        it_with_tracker = (int)pass;
        it_reset();
        s_held(pass ? P_DRUMS | P_TRACKER : P_DRUMS);
        for (b = 0; b < 1200u; b++) {
            if (b == 0u) sq_post(RQ_PLAY, 0, 0);
            if (b == 100u) sq_post(RQ_STOP, 0, 0);
            it_block(o);
            if (!pass) {
                if (!quiet_ref && it_quiet()) quiet_ref = b;
                continue;
            }
            if (b == 99u) on_before = it_trk_on();
            if (b == 100u) {
                on_stop = it_trk_on();
                kick_rings = sp_ch[4].on;
                t0 = it_trk_ticks();
                r0 = it_trk_row();
            }
            if (b >= 100u) {
                late += it_trk_on();
                moved += it_trk_ticks() != t0 || it_trk_row() != r0;
                if (!quiet_at && it_quiet()) quiet_at = b;
                never &= !(it_quiet() && b < 100u);
            }
            if (b >= 100u && b < 110u) tail += (uint32_t)it_nonzero(o);
            if (b >= 600u) loud += (uint32_t)it_nonzero(o);
        }
        if (!pass) st_ref = it_state; else st = it_state;
    }
    it_with_tracker = 1;
    drums_same = st == st_ref;
    base(on_before == SP_NTRK && on_stop == 0u && !late,
         "STOP: all four tracker voices, tones still sounding, are off in the block STOP is taken in, and stay off");
    base(t0 > 0u && !moved, "STOP: the tracker's playhead does not move while the transport is stopped (1100 blocks)");
    base(kick_rings && drums_same && tail && !loud,
         "STOP: the drums ring out as without the tracker (their voices block by block the same); the tracker's cut fades (SMOOTH)");
    base(never && quiet_at > 100u && quiet_at == quiet_ref,
         "autosave: quiet (stopped, no voice on) exactly when the drums alone would be: a held tracker note does not hold the save back");
    printf(NOTE "STOP in block 100: tracker voices on %u before, %u in it; quiet from block %u (drums alone: %u)\n",
           (unsigned)on_before, (unsigned)on_stop, (unsigned)quiet_at, (unsigned)quiet_ref);

    it_reset();                                     /* never played: a song is there, nothing runs */
    s_held(P_TRACKER);
    never = 1;
    for (b = 0; b < 2000u; b++) {
        it_block(o);
        never &= it_quiet() && !it_nonzero(o) && !it_trk_ticks() && !it_trk_row();
    }
    base(never && masif_edits.read_index == masif_edits.write_index,
         "autosave: with a tracker song and no PLAY since power-on every block is quiet (the tracker never starts by itself)");
}

/* the count-in at 0, 1 and 2 bars (GLO > CLICK > COUNT), REC armed: the tracker waits as the sequencer does */
static uint8_t cin_set;
static void s_count_n(unsigned parts)
{
    sq.cin_bars = cin_set;
    s_held(parts & P_TRACKER);
    if (parts & P_DRUMS) it_step(PAD_HAT7, 0);
}
static void t_clk_count_in(void)
{
    int32_t o[2u * SP_BLK];
    uint32_t n, b, d, t, waited, early, ok = 1, started, expect;
    for (cin_set = 0; cin_set <= 2u; cin_set++) {
        it_tempo(2000);
        expect = cin_set ? (((uint32_t)cin_set * SQ_BAR << 16) - 1u) / it_d : 0u;   /* blocks that end with count-in left */
        n = it_blk_of_tick((cin_set + 1u) * SQ_BAR);
        it_reset();
        s_count_n(P_TRACKER);
        sq_post(RQ_REC, 0, 0);
        sq_post(RQ_PLAY, 0, 0);
        waited = early = started = 0;
        for (b = 0; b < n; b++) {
            it_block(o);
            if (sq.countin > 0) {
                waited++;
                early += it_trk_ticks() || it_trk_on() || it_trk_row();
            } else if (!started && it_trk_on() == SP_NTRK) {
                started = b;
            }
        }
        it_render(s_count_n, 0, rec_play_at_0, it_buf[0], n);
        it_render(s_count_n, P_DRUMS, rec_play_at_0, it_buf[1], n);
        it_render(s_count_n, P_TRACKER, rec_play_at_0, it_buf[2], n);
        d = first_diff(it_buf[0], it_buf[1], n);
        t = first_diff(it_buf[0], it_buf[2], n);
        ok &= !early && d == t && d != 0xFFFFFFFFu && started == d && waited == expect && d >= waited &&
              d - waited <= it_blk_of_tick(0);
        printf(NOTE "COUNT %u: %u blocks of count-in, sequencer tick 0 in block %u, tracker row 0 in block %u\n",
               (unsigned)cin_set, (unsigned)waited, (unsigned)d, (unsigned)t);
    }
    base(ok != 0u, "count-in 0, 1 and 2 bars: nothing of the tracker moves or sounds in it; row 0 with the sequencer's tick 0 after it");
    cin_set = 1;                                    /* PLAY alone (REC not armed) has no count-in, whatever COUNT is */
    it_tempo(1200);
    n = it_blk_of_tick(SQ_BAR);
    it_render(s_count_n, 0, play_at_0, it_buf[0], n);
    it_render(s_count_n, P_TRACKER, play_at_0, it_buf[2], n);
    base(first_diff(it_buf[0], it_buf[2], n) == it_blk_of_tick(0) - 1u,
         "PLAY without REC (COUNT 1): no count-in for the sequencer, none for the tracker: row 0 on tick 0 at once");
}

/* the tempo knob turned while playing, up and down, also in the middle of a row */
static const struct { uint32_t blk; uint16_t bpm10; } tempo_tab[] = {
    {0, 1200}, {700, 2400}, {1111, 400}, {2900, 1337}, {3333, 2295}, {3334, 900}, {4000, 2400}, {4700, 1200}};
#define TEMPO_N (sizeof tempo_tab / sizeof tempo_tab[0])
#define TEMPO_BLKS 5600u
static void tempo_script(uint32_t b)
{
    uint32_t i;
    if (b == 0u) sq_post(RQ_PLAY, 0, 0);
    for (i = 1; i < TEMPO_N; i++)
        if (b == tempo_tab[i].blk) sq.bpm10 = tempo_tab[i].bpm10;   /* as the UI writes it, while playing */
}
static void t_clk_tempo(void)
{
    uint32_t dtab[TEMPO_N], i, b, bad = 0, cnt, k = 0;
    uint64_t clk = 0;
    int same;
    for (i = 0; i < TEMPO_N; i++) dtab[i] = it_seq_inc(tempo_tab[i].bpm10);
    it_tempo(1200);
    it_reset();
    s_blip_rows(P_TRACKER);
    for (b = 0; b < TEMPO_BLKS; b++) {
        tempo_script(b);
        if (k + 1u < TEMPO_N && b == tempo_tab[k + 1u].blk) k++;
        sq_block();
        masif_fm1_audio_block();
        clk += dtab[k];                             /* the sequencer's clock, kept here: each tempo's own step */
        bad += it_trk_ticks() != (uint32_t)(clk >> 16) || !it_trk_at((uint32_t)(clk >> 16)) ||
               (sq.pos >> 16) != (uint32_t)(clk >> 16) % SQ_BAR || masif_runtime.bpm10 != tempo_tab[k].bpm10;
    }
    base(k == TEMPO_N - 1u && (clk >> 16) > SQ_BAR && !bad,
         "tempo turned 7 times while playing (40-240 BPM, also mid-row): in every block the tracker is on the sequencer's tick");
    same = it_same_onsets(s_rows_both, tempo_script, TEMPO_BLKS, &cnt);
    base(same && cnt >= 30u, "... and heard: a blip a row and a pad on every 24th tick start in the same blocks through all of it");
    printf(NOTE "%u blocks, %u ticks, %u blocks off the sequencer's tick; %u rows heard with their pads\n",
           (unsigned)TEMPO_BLKS, (unsigned)(clk >> 16), (unsigned)bad, (unsigned)cnt);
}

/* PLAY and STOP again and again: STOP mid-note, PLAY in the next block, PLAY while playing (from the top), STOP
 * and PLAY in one block, STOP while stopped. Every PLAY is the same performance from row 0 */
static const uint32_t cyc_play[] = {0, 900, 1501, 2300, 2900, 3700, 4301};   /* 2900: while playing */
static const uint32_t cyc_stop[] = {700, 1500, 2100, 2200, 3500, 4300};      /* 2200: while stopped; 4300 + 4301: a block apart */
#define CYC_N (sizeof cyc_play / sizeof cyc_play[0])
#define CYC_LEN 180u                                /* blocks of every PLAY compared (the shortest runs 200) */
#define CYC_FADE 16u                                /* ... from here: before it the cut of the run before still fades (SMOOTH, 8 blocks) */
#define CYC_BLKS 5000u
static void cycle_script(uint32_t b)
{
    uint32_t i;
    for (i = 0; i < sizeof cyc_stop / sizeof cyc_stop[0]; i++)
        if (b == cyc_stop[i]) sq_post(RQ_STOP, 0, 0);
    for (i = 0; i < CYC_N; i++)
        if (b == cyc_play[i]) sq_post(RQ_PLAY, 0, 0);
    if (b == 3900u) { sq_post(RQ_STOP, 0, 0); sq_post(RQ_PLAY, 0, 0); }   /* both in one block */
}
static void t_clk_cycles(void)
{
    int32_t o[2u * SP_BLK];
    uint32_t i, b, differ = 0, fade = 0, sound = 0, stopped_on = 0, stopped_moved = 0, t_stop = 0, playing = 0, nplay = 0, top = 0;
    uint32_t off = 0, mine = 0;
    uint64_t clk = 0;                               /* the transport, kept here: 0 at every PLAY, the tempo's step a playing block */
    int was = 0;
    it_tempo(1200);
    it_render(s_busy, P_TRACKER, cycle_script, it_buf[0], CYC_BLKS);
    for (i = 1; i <= CYC_N; i++) {
        const int32_t *w = it_buf[0] + (size_t)(i < CYC_N ? cyc_play[i] : 3900u) * 2u * SP_BLK;
        differ += it_ndiff(w + CYC_FADE * 2u * SP_BLK, it_buf[0] + CYC_FADE * 2u * SP_BLK, CYC_LEN - CYC_FADE);
        fade += it_ndiff(w, it_buf[0], CYC_FADE);
    }
    for (b = 0; b < CYC_LEN; b++) sound += (uint32_t)it_nonzero(it_buf[0] + (size_t)b * 2u * SP_BLK);
    base(sound > CYC_LEN / 2u && !differ,
         "every PLAY is the first one again, sample for sample: notes, slides, cuts and retriggers from row 0, nothing kept from the run before");
    it_reset();
    s_busy(P_TRACKER);
    for (b = 0; b < CYC_BLKS; b++) {
        cycle_script(b);
        for (i = 0; i < sizeof cyc_stop / sizeof cyc_stop[0]; i++)
            if (b == cyc_stop[i]) mine = 0;
        for (i = 0; i < CYC_N; i++)
            if (b == cyc_play[i]) { mine = 1; clk = 0; }
        if (b == 3900u) { mine = 1; clk = 0; }
        it_block(o);
        if (mine) {
            clk += it_d;
            off += it_trk_ticks() != (uint32_t)(clk >> 16) || !it_trk_at((uint32_t)(clk >> 16)) ||
                   (sq.pos >> 16) != (uint32_t)(clk >> 16) % SQ_BAR;
        }
        off += mine != sq.playing;
        if (!sq.playing) {
            if (was) t_stop = it_trk_ticks();
            stopped_on += it_trk_on();
            stopped_moved += it_trk_ticks() != t_stop;
        } else {
            playing++;
            if (!was) { nplay++; top += it_trk_ticks() <= 1u; }
        }
        was = sq.playing;
    }
    base(playing > 4000u && !off,
         "PLAY / STOP cycles (STOP mid-note, PLAY in the next block, PLAY while playing, STOP + PLAY in one block): in every playing block the tracker is on the sequencer's tick, counted from that PLAY");
    base(nplay == 6u && top == nplay && !stopped_on && !stopped_moved,
         "in every stopped block of the cycles no tracker voice is on and its playhead stands; every PLAY starts it at tick 0");
    printf(NOTE "%u blocks, %u PLAYs seen from stopped, %u playing blocks, %u off the tick; %u samples differ between the PLAYs\n",
           (unsigned)CYC_BLKS, (unsigned)nplay, (unsigned)playing, (unsigned)off, (unsigned)differ);
    printf(NOTE "(and %u in a PLAY's first %u blocks: the cut of the run before, fading)\n", (unsigned)fade, (unsigned)CYC_FADE);
}

/* the drums through all of the transport (count-in, tempo, STOP, PLAY again) with an empty tracker song in the
 * block, against no tracker in the block: nothing of the drums or the sequencer may notice the tracker's clock */
static void t_clk_drums(void)
{
    uint32_t n = 4000, mix[2], st[2], pass, b, nz = 0;
    unsigned char sq_end[2][sizeof sq];
    it_tempo(1200);
    for (pass = 0; pass < 2u; pass++) {
        it_with_tracker = (int)pass;
        it_render(s_busy, P_DRUMS, it_clk_ref_script, it_buf[pass], n);
        mix[pass] = it_fnv(IT_FNV0, it_buf[pass], (size_t)n * 2u * SP_BLK * sizeof(int32_t));
        st[pass] = it_state;
        memcpy(sq_end[pass], &sq, sizeof sq);
    }
    it_with_tracker = 1;
    for (b = 0; b < n; b++) nz += (uint32_t)it_nonzero(it_buf[0] + (size_t)b * 2u * SP_BLK);
    base(nz > 1000u && mix[0] == mix[1] && st[0] == st[1] && !memcmp(sq_end[0], sq_end[1], sizeof sq),
         "the drums and the sequencer through count-in, tempo changes, STOP and PLAY again: the same with the tracker's block as without");
}

/* ---- the effects that reach into a track's voice: PAN, SEND, SAMPLE OFFSET, REVERSE (sp_core.c sp_*_track) */
static void s_voice_fx(unsigned parts)
{
    if (!(parts & P_TRACKER)) return;
    it_cell(0, 0, ROOT, PAD_TONE, MASIF_FX_PAN, 0);             /* track 1: hard left, */
    it_cell(0, 2, ROOT, PAD_TONE, 0, 0);                        /* (the next note: its instrument's pan again) */
    it_cell(0, 4, MASIF_NOTE_NONE, NONE, MASIF_FX_PAN, 255);    /* the sounding note to the right, */
    it_cell(0, 6, MASIF_NOTE_NONE, NONE, MASIF_FX_PAN, 128);    /* and the middle */
    it_cell(1, 0, ROOT, PAD_TONE, MASIF_FX_SEND, 0x1F);         /* track 2: the delay send full, */
    it_cell(1, 2, ROOT, PAD_TONE, MASIF_FX_SEND, 0x20);         /* the reverb send none, the delay's stays */
    it_cell(2, 0, ROOT, PAD_TONE, MASIF_FX_SAMPLE_OFFSET, 0x80);    /* track 3: from the middle of the sample */
    it_cell(3, 0, ROOT, PAD_TONE, MASIF_FX_REVERSE, 1);         /* track 4: the tone backwards */
}

static void t_voice_fx(void)
{
    const sp_ch_t *v1 = TRK_VOICE(0), *v2 = TRK_VOICE(1), *v3 = TRK_VOICE(2), *v4 = TRK_VOICE(3);
    int32_t o[2u * SP_BLK];
    uint32_t b = 0, p3, p4, back = 1, n = sp_wave[W_TONE].n;
    int ok;
    it_tempo(1200);
    it_reset();
    masif_song.instrument[PAD_TONE].send[2] = 50;   /* (the instrument's own reverb send, for SEND to take away) */
    s_voice_fx(P_TRACKER);
    sq_post(RQ_PLAY, 0, 0);
#define RUN_TO(T) while (b < it_blk_of_tick(T)) { uint32_t was = v4->pos, won = v4->on; it_block(o); b++; \
        if (won && v4->on) back &= v4->pos <= was; }
    RUN_TO(4);
    p3 = v3->pos; p4 = v4->pos;
    ok = v1->on && v1->gr == 0 && v1->gl > 0;
    base(ok, "PAN 00 on the note's row: the track's voice is hard left");
    base(v2->on && v2->snd[1] == 127u * 258u && v2->snd[2] == 50u * 258u && v2->snd[0] == 0u,
         "SEND 1F: the voice's delay send is full, the instrument's own reverb send is kept");
    base(v3->on && p3 >= n / 2u - 128u && p3 < n / 2u + 1500u,
         "SAMPLE OFFSET 80: the note starts in the middle of the sample");
    base(v4->on && v4->dir < 0 && p4 > n - 1500u && p4 < n, "REVERSE: the note starts at the sample's end, going backwards");
    RUN_TO(50);
    base(v1->on && v1->gl == v1->gr && v1->gl > 0 && v2->snd[1] == 127u * 258u && v2->snd[2] == 0u,
         "PAN is its step's: the track's next note has its instrument's pan; the SENDs stay (the reverb send now none)");
    RUN_TO(98);
    base(v1->on && v1->gl == 0 && v1->gr > 0, "PAN FF on a row without a note: the sounding note goes hard right");
    RUN_TO(146);
    base(v1->on && v1->gl == v1->gr && v1->gl > 0, "PAN 80: the middle, both sides the same");
    RUN_TO(240);
    base(!v4->on && back && !v3->on, "REVERSE: it played back to the sample's start and ended there (as the forward note at its end)");
    sq_post(RQ_STOP, 0, 0);
    it_block(o);
    it_reset();                                     /* (the song away: a plain note after PLAY) */
    masif_song.instrument[PAD_TONE].send[2] = 50;
    it_cell(0, 0, ROOT, PAD_TONE, 0, 0);
    it_cell(1, 0, ROOT, PAD_TONE, 0, 0);
    sq_post(RQ_PLAY, 0, 0);
    b = 0;
    RUN_TO(4);
    base(v1->on && v1->gl == v1->gr && v2->on && v2->snd[1] == 0u && v2->snd[2] == 50u * 258u,
         "PLAY: PAN and SEND are the instruments' own again");
#undef RUN_TO
}

/* ---- a loop with two ends of its own, and the ways round it: forwards, to and fro, and both beginning backwards */
static void t_loops(void)
{
    const uint32_t n = sp_wave[W_TONE].n;
    const sp_ch_t *v = TRK_VOICE(0);
    static const char *const what[8] = {
        "FWDLOOP with an end of its own: forwards into the loop and round it, never past its end",
        "FWD PP: to and fro between the loop's two ends, no seam",
        "REVLOOP: from the part's end backwards into the loop, round it backwards",
        "REV PP: from the part's end backwards, then to and fro in the loop",
        "a loop's end at 00: the loop ends where the part does",
        "OSC: the loop alone from its first sample, the part's start is not played; a new note does not begin it again",
        "OSC REV: the loop alone, backwards from its end; a new note does not begin it again",
        "OSC PP: the loop alone, to and fro; a new note does not begin it again"};
    static const uint8_t mode[8] = {MASIF_PLAY_LOOP, MASIF_PLAY_PING, MASIF_PLAY_REV_LOOP, MASIF_PLAY_REV_PING, MASIF_PLAY_LOOP,
                                    MASIF_PLAY_OSC, MASIF_PLAY_OSC_REV, MASIF_PLAY_OSC_PING};
    uint32_t k;
    for (k = 0; k < 8u; k++) {
        masif_instrument_t in;
        static const uint8_t adsr[4] = {0, 127, 127, 0}, send[3] = {0, 0, 0};
        const int osc = mode[k] >= MASIF_PLAY_OSC;
        int32_t o[2u * SP_BLK];
        const uint32_t a = 0, b = n / 4u, la = n / 16u, lb = k == 4u ? b : n / 8u;
        uint32_t i, lo = ~0u, hi = 0, ups = 0, downs = 0, jumps = 0, turns = 0, was = 0, first, inside = 0, out = 0;
        int32_t wasdir = 0;
        int ok, back = mode[k] & 1;
        it_tempo(1200);
        it_reset();
        masif_instrument_default(&in, 0);
        in.sample = W_TONE; in.table_tic = 0; in.play = mode[k];
        in.start = 0; in.length = 64; in.loop_start = 16; in.loop_end = k == 4u ? 0 : 32;
        ok = masif_set_instrument(&masif_song, 20, &in);
        it_cell(0, 0, ROOT, 20, 0, 0);
        sq_post(RQ_PLAY, 0, 0);
        for (i = 0; i < it_blk_of_tick(0); i++) it_block(o);
        first = v->pos;
        for (i = 0; i < 900u; i++) {
            it_block(o);
            if (!v->on) break;
            if (v->pos >= la && v->pos < lb) inside = 1;
            if (inside) {
                if (v->pos < la || v->pos >= lb) out++;
                if (v->pos < lo) lo = v->pos;
                if (v->pos > hi) hi = v->pos;
                if (wasdir && v->dir != wasdir) turns++;
                else if (wasdir > 0 && v->pos < was) jumps++;
                else if (wasdir < 0 && v->pos > was) jumps++;
                if (v->dir > 0) ups++; else downs++;
                wasdir = v->dir; was = v->pos;
            }
        }
        ok = ok && v->on && inside && !out && lo < la + 64u && hi + 64u > lb;
        if (osc) ok = ok && (back ? first + 64u > lb && first < lb : first >= la && first < la + 64u);
        else ok = ok && (back ? first + 64u > b : first < a + 64u);
        if (mode[k] == MASIF_PLAY_LOOP || mode[k] == MASIF_PLAY_OSC) ok = ok && jumps > 1u && !turns && !downs;
        if (mode[k] == MASIF_PLAY_REV_LOOP || mode[k] == MASIF_PLAY_OSC_REV) ok = ok && jumps > 3u && !turns && !ups;
        if (mode[k] == MASIF_PLAY_PING || mode[k] == MASIF_PLAY_REV_PING || mode[k] == MASIF_PLAY_OSC_PING)
            ok = ok && turns > 3u && !jumps && ups && downs;
        while (v->pos < la + 300u || v->pos + 300u > lb) it_block(o);      /* (well inside the loop) */
        was = v->pos;
        sp_trigger_track_wave(0, W_TONE, a, b, la, lb, mode[k], 0, adsr, 0, 0, 0, send);    /* the same note again */
        ok = ok && (osc ? v->pos == was : v->pos == (back ? b - 1u : a));
        base(ok, what[k]);
        printf(NOTE "loop %u..%u of a part %u..%u: begins at %u, then %u..%u, %u times round, %u turns\n", (unsigned)la,
               (unsigned)lb, (unsigned)a, (unsigned)b, (unsigned)first, (unsigned)lo, (unsigned)hi, (unsigned)jumps, (unsigned)turns);
        sq_post(RQ_STOP, 0, 0);
        it_block(o);
    }
}

/* ---- the loop's tapers (the user, 2026-10-15): the level rises after the loop's start and falls before its end,
 * every time round; without them the loop is at full level there */
static void t_tapers(void)
{
    const uint32_t n = sp_wave[W_TONE].n, la = n / 16u, lb = n / 8u, len = lb - la;
    const sp_ch_t *v = TRK_VOICE(0);
    int32_t o[2u * SP_BLK], pk[2][3];
    uint32_t pass;
    for (pass = 0; pass < 2u; pass++) {             /* (0: with tapers of 40 / 100 = a quarter of the loop each; 1: none) */
        masif_instrument_t in;
        uint32_t i, rounds = 0, was = 0;
        int ok;
        it_tempo(1200);
        it_reset();
        masif_instrument_default(&in, 0);
        in.sample = W_TONE; in.table_tic = 0; in.play = MASIF_PLAY_LOOP;
        in.start = 0; in.length = 64; in.loop_start = 16; in.loop_end = 32;
        in.taper_in = pass ? 0u : 64u; in.taper_out = pass ? 0u : 64u;
        ok = masif_set_instrument(&masif_song, 20, &in);
        (void)ok;
        it_cell(0, 0, ROOT, 20, 0, 0);
        sq_post(RQ_PLAY, 0, 0);
        pk[pass][0] = pk[pass][1] = pk[pass][2] = 0;
        for (i = 0; i < it_blk_of_tick(0); i++) it_block(o);      /* (to the note) */
        for (i = 0; i < 3000u && rounds < 6u; i++) {  /* the loudest output where the voice is: its first tenth, the middle, its last tenth */
            uint32_t k, at;
            int32_t m = 0;
            it_block(o);
            if (!v->on) break;
            if (v->pos < was) rounds++;
            was = v->pos;
            if (!rounds) continue;                    /* (round at least once: what plays now is the loop) */
            for (k = 0; k < 2u * SP_BLK; k++) m = o[k] > m ? o[k] : -o[k] > m ? -o[k] : m;
            at = v->pos - la;
            if (at < len / 10u) { if (m > pk[pass][0]) pk[pass][0] = m; }
            else if (at > len * 4u / 10u && at < len * 6u / 10u) { if (m > pk[pass][1]) pk[pass][1] = m; }
            else if (at > len * 9u / 10u) { if (m > pk[pass][2]) pk[pass][2] = m; }
        }
        sq_post(RQ_STOP, 0, 0);
        it_block(o);
    }
    printf(NOTE "loop's loudest: first tenth %d, middle %d, last tenth %d with tapers; %d, %d, %d without\n", (int)pk[0][0], (int)pk[0][1],
           (int)pk[0][2], (int)pk[1][0], (int)pk[1][1], (int)pk[1][2]);
    base(pk[0][1] > 0 && pk[0][0] * 2 < pk[0][1] && pk[0][2] * 2 < pk[0][1] && pk[1][0] * 4 > pk[1][1] * 3 && pk[1][2] * 4 > pk[1][1] * 3,
         "the loop's tapers: quiet where the loop begins and ends, full in its middle; without them full everywhere");
}

/* ---- the tempo as the pitch (REPITCH, BPM), and an envelope another instrument starts (TRIG: a side chain) */
static void t_tempo_trig(void)
{
    const uint32_t unity = 26040u * 65536u / SP_FS, n = sp_wave[W_TONE].n;
    const sp_ch_t *v1 = TRK_VOICE(0), *v2 = TRK_VOICE(1);
    masif_instrument_t in;
    int32_t o[2u * SP_BLK];
    uint32_t b = 0, want, s120, pass;
    int ok;
#define RUN_TO(T) while (b < it_blk_of_tick(T)) { it_block(o); b++; }
#define NEAR(x, y) ((x) + (y) / 500u >= (y) && (y) + (y) / 500u >= (x))
    for (pass = 0; pass < 2u; pass++) {             /* at 120 BPM, then at 180 */
        it_tempo(pass ? 1800 : 1200);
        it_reset();
        masif_instrument_default(&in, 0);
        in.sample = W_TONE; in.table_tic = 0; in.play = MASIF_PLAY_REPITCH; in.steps = 32;
        ok = masif_set_instrument(&masif_song, 20, &in);
        in.play = MASIF_PLAY_BPM; in.steps = 120;
        ok &= masif_set_instrument(&masif_song, 21, &in);
        it_cell(0, 0, ROOT, 20, 0, 0);
        it_cell(1, 0, ROOT, 21, 0, 0);
        b = 0;
        sq_post(RQ_PLAY, 0, 0);
        RUN_TO(0);
        /* 32 rows = 8 quarters: 4 s at 120 BPM; the whole wave in that time */
        want = (uint32_t)(((uint64_t)n << 16) / ((pass ? 8u : 12u) * SP_FS / 3u));
        if (!pass) {
            s120 = v1->step;
            base(ok && v1->on && v1->looped == 1u && NEAR(v1->step, want),
                 "REPITCH: the pitch that makes the part last its STEPS rows at the tempo (32 rows at 120 BPM: 4 s), on a loop");
            printf(NOTE "a wave of %u samples: step %u, for 4 s %u\n", (unsigned)n, (unsigned)v1->step, (unsigned)want);
            base(v2->on && NEAR(v2->step, unity), "BPM: a sample of 120 BPM at 120 BPM plays at its own pitch");
        } else {
            base(ok && NEAR(v1->step, want) && NEAR(v1->step, s120 * 3u / 2u) && NEAR(v2->step, unity * 3u / 2u),
                 "at 180 BPM both are half as fast again: the part still lasts its rows");
        }
        sq_post(RQ_STOP, 0, 0);
        it_block(o);
    }
    for (pass = 0; pass < 2u; pass++) {             /* the source an instrument, then a track */
        int16_t before, own, hit, later, end;
        it_tempo(1200);
        it_reset();
        masif_instrument_default(&in, 0);
        in.sample = W_TONE; in.table_tic = 0; in.play = MASIF_PLAY_LOOP;
        in.mod[0].type = MASIF_MOD_TRIG; in.mod[0].dest = MASIF_DEST_VOLUME; in.mod[0].amount = -64;
        in.mod[0].p[0] = 0; in.mod[0].p[1] = 0; in.mod[0].p[2] = 40; in.mod[0].p[3] = (uint8_t)(pass ? 0x81 : 21);
        ok = masif_set_instrument(&masif_song, 20, &in);                    /* 20: a pad that ducks ... */
        masif_instrument_default(&in, 0);
        in.sample = W_KICK; in.table_tic = 0;
        ok &= masif_set_instrument(&masif_song, 21, &in);                   /* ... when 21 (the kick, on track 2) hits */
        it_cell(0, 0, ROOT, 20, 0, 0);
        it_cell(1, 4, ROOT, 21, 0, 0);
        it_cell(0, 12, ROOT, 20, 0, 0);
        b = 0;
        sq_post(RQ_PLAY, 0, 0);
        RUN_TO(2);
        own = masif_track_volf[0];
        RUN_TO(4 * 24 - 2);
        before = masif_track_volf[0];
        RUN_TO(4 * 24 + 2);
        hit = masif_track_volf[0];
        RUN_TO(5 * 24);
        later = masif_track_volf[0];
        RUN_TO(12 * 24 + 2);
        end = masif_track_volf[0];
        base(ok && own == 256 && before == 256 && hit < 40 && later > hit && later < 256 && end == 256 && v1->on,
             pass ? "TRIG with a track as its source (81: track 2): the same" :
                    "TRIG -64 on the volume, its source the kick's instrument: the pad's own notes leave it alone, the kick's hit ducks it and it comes back");
        printf(NOTE "the pad's level of 256: at its own note %d, before the kick %d, at the kick %d, a row on %d, at the end %d\n",
               own, before, hit, later, end);
        sq_post(RQ_STOP, 0, 0);
        it_block(o);
    }
#undef RUN_TO
#undef NEAR
}

/* ---- instruments: a part of a sample, once, backwards, looped or in slices; their own level, pan and table; an
 * own sample's flags; and what a song may set of the machine (the tempo) */
static void t_instruments(void)
{
    const uint32_t unity = 26040u * 65536u / SP_FS, n = sp_wave[W_TONE].n;
    const sp_ch_t *v1 = TRK_VOICE(0), *v2 = TRK_VOICE(1), *v3 = TRK_VOICE(2), *v4 = TRK_VOICE(3);
    masif_instrument_t in;
    masif_table_cell_t tc = {12, 0xFF, 0, 0, 0, 0};
    int32_t o[2u * SP_BLK];
    uint32_t b = 0, wraps = 0, lo = ~0u, hi = 0, was = 0, p2, p3, e2, e3, gl0;
    int ok;
    it_tempo(1200);
    it_reset();
    masif_instrument_default(&in, 0);
    in.sample = W_TONE; in.table_tic = 0;
    in.play = MASIF_PLAY_LOOP; in.length = 16; in.loop_start = 8;           /* 20: the first 1/16, round its 2nd half */
    ok = masif_set_instrument(&masif_song, 20, &in);
    in.play = MASIF_PLAY_FWD; in.length = 0; in.loop_start = 0; in.slices = 4;      /* 21: the tone in 4 slices */
    ok &= masif_set_instrument(&masif_song, 21, &in);
    in.slices = 0; in.play = MASIF_PLAY_REV; in.start = 64; in.length = 64;         /* 22: its 2nd quarter, backwards */
    ok &= masif_set_instrument(&masif_song, 22, &in);
    in.play = MASIF_PLAY_FWD; in.start = 0; in.length = 0; in.pan = -64; in.volume = 64; in.table_tic = 1;
    ok &= masif_set_instrument(&masif_song, 23, &in);                              /* 23: left, half level, a table */
    ok &= masif_set_table_cell(&masif_song, 23, 0, tc);                            /* its row 0 an octave up, */
    tc.transpose = 0; tc.fx = MASIF_FX_TABLE_HOP; tc.param = 1;
    ok &= masif_set_table_cell(&masif_song, 23, 1, tc);                            /* row 1 at the note, and there it stays */
    it_cell(0, 0, ROOT + 5u, 20, 0, 0);
    it_cell(0, 6, MASIF_NOTE_OFF, NONE, 0, 0);
    it_cell(1, 0, ROOT + 2u, 21, 0, 0);
    it_cell(2, 0, ROOT, 22, 0, 0);
    it_cell(3, 0, ROOT, 23, 0, 0);
    it_cell(3, 7, MASIF_NOTE_NONE, NONE, MASIF_FX_SET_TEMPO, 0x10);                /* (16 BPM: not a tempo, ignored) */
    it_cell(3, 8, MASIF_NOTE_NONE, NONE, MASIF_FX_SET_TEMPO, 0xA0);                /* 160 BPM */
    {
        masif_cell_t c = {MASIF_NOTE_NONE, NONE, 40, 0, 0, 0, 0};                        /* the volume column alone */
        ok &= masif_set_cell(&masif_song, 12, 1, &c);
    }
    sq_post(RQ_PLAY, 0, 0);
#define RUN_TO(T) while (b < it_blk_of_tick(T)) { it_block(o); b++; \
        if (v1->on) { wraps += v1->pos < was; was = v1->pos; if (v1->pos < lo) lo = v1->pos; if (v1->pos > hi) hi = v1->pos; } }
    RUN_TO(0);
    p2 = v2->pos; e2 = v2->end; p3 = v3->pos; e3 = v3->end; gl0 = (uint32_t)v4->gl;
    base(ok && v2->on && p2 >= n / 2u && p2 < n / 2u + 64u && e2 == n / 4u * 3u && v2->step == unity,
         "a sliced instrument: the note picks the slice (the root the first), played at the sample's own pitch to the slice's end");
    base(v3->on && v3->dir < 0 && p3 < n / 2u && p3 > n / 2u - 64u && e3 == n / 4u,
         "a backwards instrument: its part of the sample from the part's end down to its start");
    base(v4->on && v4->gr == 0 && v4->gl == 64 * 64 / 3 && v4->step == 2u * unity,
         "an instrument's own level and pan; its table's first row transposes the note as it starts (an octave up)");
    RUN_TO(4);
    base(v4->step == unity, "the table's next row a tracker tick later: back at the note");
    RUN_TO(24 + 2);
    base((uint32_t)v4->gl < gl0 && v4->gl == 40 * 64 / 127 * 64 / 3 && v4->gr == 0,
         "the volume column on a row without a note: the sounding note's level (x the instrument's)");
    RUN_TO(120);
    base(v1->on && wraps > 8u && lo < n / 32u && hi < n / 16u && v1->pos >= n / 32u - 2u && !v2->on && !v3->on,
         "a looped instrument plays its part and then round its loop until a NOTE OFF; the others ended on their own");
    printf(NOTE "%u times round, positions %u..%u of a part 0..%u with the loop from %u\n", (unsigned)wraps, (unsigned)lo,
           (unsigned)hi, (unsigned)(n / 16u), (unsigned)(n / 32u));
    RUN_TO(6 * 24 + 4);
    base(!v1->on, "NOTE OFF ends the loop");
    RUN_TO(8 * 24 - 2);
    base(sq.bpm10 == 1200, "SET TEMPO outside 40-240 BPM is ignored");
    for (was = 0; was < 40u; was++) it_block(o);    /* (past row 8) */
    base(sq.bpm10 == 1600, "SET TEMPO A0: the sequencer's tempo is 160 BPM, for the drums too");
    sp_wave[W_TONE].uflags = 2;                     /* an own sample's flags (the web editor's), on the wave itself */
    sp_wave[W_BLIP].uflags = 1;
    b = 0;
    it_tempo(1200);
    it_reset();
    it_cell(0, 0, ROOT, PAD_TONE, 0, 0);
    it_cell(1, 0, ROOT, PAD_BLIP, 0, 0);
    sq_post(RQ_PLAY, 0, 0);
    RUN_TO(0);
    base(v1->step == unity / 2u && v2->step == (26040u * 33u / 45u) * 65536u / SP_FS,
         "an own sample stored at double speed plays an octave down, one stored for 45 -> 33 at 33/45 of its rate");
    sp_wave[W_TONE].uflags = sp_wave[W_BLIP].uflags = 0;
    {   /* SLC xx: a sliced instrument plays slice xx (round past the last) at the note's pitch; OFS xx: the note
         * starts xx / 256 into the whole sample, before the instrument's part too, and plays to the part's end */
        const uint32_t a40 = (n >> 8) * 0x40u + (((n & 255u) * 0x40u) >> 8), aC0 = (n >> 8) * 0xC0u + (((n & 255u) * 0xC0u) >> 8);
        it_reset();
        masif_instrument_default(&in, 0);
        in.sample = W_TONE; in.table_tic = 0; in.slices = 4;
        ok = masif_set_instrument(&masif_song, 21, &in);
        in.slices = 0; in.start = 128;                                              /* 22: the second half */
        ok &= masif_set_instrument(&masif_song, 22, &in);
        it_cell(0, 0, ROOT + 12u, 21, MASIF_FX_SLICE, 1);
        it_cell(1, 0, ROOT, 21, MASIF_FX_SLICE, 6);
        it_cell(2, 0, ROOT, 22, MASIF_FX_SAMPLE_OFFSET, 0x40);
        it_cell(3, 0, ROOT, 22, MASIF_FX_SAMPLE_OFFSET, 0xC0);
        sq_post(RQ_PLAY, 0, 0);
        b = 0;
        RUN_TO(0);
        base(ok && v1->on && v1->pos >= n / 4u && v1->pos < n / 4u + 128u && v1->end == n / 4u * 2u && v1->step == 2u * unity &&
             v2->on && v2->pos >= n / 4u * 2u && v2->pos < n / 4u * 2u + 64u && v2->end == n / 4u * 3u && v2->step == unity,
             "SLC: the slice it names (6 of 4: the 3rd), at the note's pitch (an octave up: twice the step)");
        base(v3->on && v3->pos >= a40 && v3->pos < a40 + 64u && v3->end == n && v4->on && v4->pos >= aC0 && v4->pos < aC0 + 64u && v4->end == n,
             "OFS: from the whole sample's start, also before the instrument's part (40: a quarter in, C0: three quarters)");
        sq_post(RQ_STOP, 0, 0);
        it_block(o);
    }
    {   /* an 8-bit sample (flag bit 2: a signed byte each) sounds as the same values stored in 12 bit */
        static uint8_t w8[4000], w12[6000];
        static int16_t pcm[4000];
        static int32_t o8[2u * SP_BLK], o12[2u * SP_BLK];
        const sp_wave_t keep = sp_wave[W_BLIP];
        uint32_t i, pass, diff = 0, loud = 0;
        for (i = 0; i < 4000u; i++) {
            int32_t v8 = (int32_t)(i * 37u % 251u) - 125;       /* any values of 8 bit */
            w8[i] = (uint8_t)v8;
            pcm[i] = (int16_t)(v8 * 16 * 16);                   /* the same as 12 bit (in a 16-bit word's top) */
        }
        sp_pack(w12, pcm, 4000);
        for (pass = 0; pass < 2u; pass++) {
            sp_wave[W_BLIP].d = pass ? w8 : w12; sp_wave[W_BLIP].n = 4000; sp_wave[W_BLIP].uflags = pass ? 4 : 0;
            it_reset();
            it_cell(0, 0, ROOT + 3u, PAD_BLIP, 0, 0);
            it_step(PAD_BLIP, 0);                               /* (and the same wave on a drum pad) */
            sq_post(RQ_PLAY, 0, 0);
            b = 0;
            RUN_TO(0);
            for (i = 0; i < 40u; i++) {
                int32_t *o1 = pass ? o8 : o12;
                uint32_t j;
                it_block(o);
                if (i == 20u) memcpy(o1, o, sizeof o8);
                for (j = 0; pass && i == 20u && j < 2u * SP_BLK; j++) { diff += o8[j] != o12[j]; loud += o8[j] != 0; }
            }
        }
        base(!diff && loud > SP_BLK, "an 8-bit sample plays as the same values in 12 bit, on a tracker voice and on a pad: sample for sample");
        sp_wave[W_BLIP] = keep;
    }
#undef RUN_TO
}

/* ---- the sampler side of an instrument: the envelope (attack, decay to sustain, release), the filter, the LFO */
static int32_t it_peak_blk(const int32_t *o)
{
    int32_t pk = 0;
    uint32_t i;
    for (i = 0; i < 2u * SP_BLK; i++) { int32_t a = o[i] < 0 ? -o[i] : o[i]; if (a > pk) pk = a; }
    return pk;
}
static void t_sampler(void)
{
    const uint32_t unity = 26040u * 65536u / SP_FS;
    const sp_ch_t *v1 = TRK_VOICE(0), *v2 = TRK_VOICE(1), *v3 = TRK_VOICE(2), *v4 = TRK_VOICE(3);
    masif_instrument_t in;
    int32_t o[2u * SP_BLK], e_first, e_sus, pk_open = 0, pk_lp = 0, g_lo = 1 << 30, g_hi = 0, c_lo = 999, c_hi = -1;
    uint32_t b = 0, s_lo = ~0u, s_hi = 0, pass;
    int ok, rising = 1;
    it_tempo(1200);
    it_reset();
    masif_instrument_default(&in, 0);
    in.sample = W_TONE; in.table_tic = 0;
    in.attack = 20; in.decay = 15; in.sustain = 64; in.release = 15;       /* 30: an envelope with all four, */
    in.play = MASIF_PLAY_LOOP;                                              /* on a loop: only the envelope ends it */
    ok = masif_set_instrument(&masif_song, 30, &in);
    in.attack = in.sustain = in.release = 0; in.decay = 127; in.play = MASIF_PLAY_FWD;
    in.mod[0].type = MASIF_MOD_LFO; in.mod[0].p[0] = MASIF_LFO_TRI; in.mod[0].p[1] = MASIF_TRIG_RETRIG; in.mod[0].p[2] = 100;
    in.mod[0].dest = MASIF_DEST_PITCH; in.mod[0].amount = 18;               /* 31-33: an LFO on the pitch, */
    ok &= masif_set_instrument(&masif_song, 31, &in);
    in.mod[0].dest = MASIF_DEST_VOLUME; in.mod[0].amount = 64;              /* the volume, */
    ok &= masif_set_instrument(&masif_song, 32, &in);
    in.mod[0].dest = MASIF_DEST_CUTOFF; in.filter = MASIF_FILTER_LP; in.cutoff = 64; in.resonance = 40;   /* and the cutoff */
    ok &= masif_set_instrument(&masif_song, 33, &in);
    it_cell(0, 0, ROOT, 30, 0, 0);
    it_cell(0, 5, MASIF_NOTE_OFF, NONE, 0, 0);
    it_cell(1, 0, ROOT, 31, 0, 0);
    it_cell(2, 0, ROOT, 32, 0, 0);
    it_cell(3, 0, ROOT, 33, 0, 0);
    sq_post(RQ_PLAY, 0, 0);
#define RUN_TO(T) while (b < it_blk_of_tick(T)) { int32_t was = v1->env; it_block(o); b++; \
        if (v1->on && v1->stage == 0u) rising &= v1->env > was; \
        if (v2->on) { if (v2->step < s_lo) s_lo = v2->step; if (v2->step > s_hi) s_hi = v2->step; } \
        if (v3->on) { if (v3->gl < g_lo) g_lo = v3->gl; if (v3->gl > g_hi) g_hi = v3->gl; } \
        if (v4->on) { if (v4->cut < c_lo) c_lo = v4->cut; if (v4->cut > c_hi) c_hi = v4->cut; } }
    RUN_TO(0);
    e_first = v1->env;
    base(ok && v1->on && v1->stage == 0u && e_first > 0 && e_first < (1 << 24) / 8,
         "ATTACK: the note starts from silence and rises");
    RUN_TO(100);
    e_sus = v1->env;
    base(rising && v1->on && v1->stage == 1u && e_sus > 64 * ((1 << 24) / 127) - (1 << 17) && e_sus < 64 * ((1 << 24) / 127) + (1 << 17),
         "... to the full level, then DECAY takes it down to SUSTAIN, where it holds");
    RUN_TO(5 * 24 + 3);
    base(v1->on && v1->stage == 2u && v1->env < e_sus, "NOTE OFF with a RELEASE: the note is not cut, it fades");
    RUN_TO(5 * 24 + 120);
    base(!v1->on, "... and ends, though its sample loops");
    base(s_lo < unity && s_hi > unity && s_hi < unity * 9u / 8u + 64u && s_lo > unity * 8u / 9u - 64u,
         "LFO on the pitch: the voice's step swings around the note's, 2 semitones at the most at that amount");
    base(g_hi <= 127 * 64 / 3 && g_hi > 127 * 64 / 3 * 15 / 16 && g_lo < g_hi / 8, "LFO on the volume: from the note's level down towards silence and back");
    base(v4->flt && c_lo < 40 && c_hi > 88 && c_lo >= 0 && c_hi <= 127, "LFO on the cutoff: the filter's cutoff swings around the instrument's");
    printf(NOTE "step %u..%u around %u; level %d..%d; cutoff %d..%d around 64\n", (unsigned)s_lo, (unsigned)s_hi,
           (unsigned)unity, (int)g_lo, (int)g_hi, (int)c_lo, (int)c_hi);
    {   /* the filters heard: the 440 Hz tone through each (the peak of the mix once it has settled) */
        static const struct { uint8_t type, cutoff, res; const char *what; } f[] = {
            {MASIF_FILTER_OFF, 127, 0, "no filter"},
            {MASIF_FILTER_LP, 8, 0, "low-pass at 85 Hz"}, {MASIF_FILTER_LP, 110, 0, "low-pass at 7 kHz"},
            {MASIF_FILTER_HP, 8, 0, "high-pass at 85 Hz"}, {MASIF_FILTER_HP, 110, 0, "high-pass at 7 kHz"},
            {MASIF_FILTER_BP, 46, 60, "band-pass at 440 Hz"}, {MASIF_FILTER_BP, 110, 60, "band-pass at 7 kHz"},
            {MASIF_FILTER_LP, 46, 127, "low-pass at 440 Hz, full resonance"}};
        int32_t pk[8];
        int types = 1;
        for (pass = 0; pass < 8u; pass++) {
            it_reset();
            masif_instrument_default(&in, 0);
            in.sample = W_TONE; in.table_tic = 0; in.filter = f[pass].type; in.cutoff = f[pass].cutoff; in.resonance = f[pass].res;
            ok = masif_set_instrument(&masif_song, 30, &in);
            it_cell(3, 0, ROOT, 30, 0, 0);
            sq_post(RQ_PLAY, 0, 0);
            pk[pass] = 0;
            for (b = 0; b < 300u; b++) { int32_t k; it_block(o); k = it_peak_blk(o); if (b > 100u && k > pk[pass]) pk[pass] = k; }
            types &= ok && v4->on && v4->flt == f[pass].type;
            printf(NOTE "%-36s peak %6d\n", f[pass].what, (int)pk[pass]);
        }
        pk_open = pk[0]; pk_lp = pk[1];
        base(types, "an instrument's filter type reaches its voice: none, low-pass, band-pass, high-pass");
        base(pk_open > 1000 && pk_lp < pk_open / 3 && pk[2] > pk_open * 3 / 4, "LOW-PASS: a low cutoff takes most of the tone away, a high one leaves it");
        base(pk[3] > pk_open * 3 / 4 && pk[4] < pk_open / 3, "HIGH-PASS: the other way round");
        base(pk[5] > pk_open / 2 && pk[6] < pk_open / 3, "BAND-PASS: the tone passes where the band is on it, not where it is far off");
        base(pk[7] > pk_open && pk[7] < pk_open * 12, "full resonance on the tone's pitch: louder, and bounded");
    }
    {   /* every cutoff and resonance, a loud tone, the cutoff moved under it: nothing runs away */
        int32_t worst = 0;
        uint32_t c, r;
        it_reset();
        masif_instrument_default(&in, 0);
        in.sample = W_TONE; in.table_tic = 0; in.play = MASIF_PLAY_LOOP; in.filter = MASIF_FILTER_BP;
        ok = masif_set_instrument(&masif_song, 30, &in);
        it_cell(3, 0, ROOT, 30, 0, 0);
        sq_post(RQ_PLAY, 0, 0);
        for (b = 0; b < 20u; b++) it_block(o);
        for (r = 0; r < 128u; r += 127u)
            for (pass = 1; pass < 4u; pass++)
                for (c = 0; c < 128u; c++) {
                    int32_t k;
                    TRK_VOICE(3)->flt = (uint8_t)pass; TRK_VOICE(3)->res = (int32_t)r;
                    sp_cutoff_track(3, (int32_t)c);
                    it_block(o);
                    it_block(o);
                    k = it_peak_blk(o);
                    if (k > worst) worst = k;
                }
        base(v4->on && worst > 0 && worst <= 98304 * 4369 / 4096 + 1, "the three filters swept over every cutoff, without and with full resonance: the output stays inside its bound");
        printf(NOTE "the highest peak of the sweep %d\n", (int)worst);
    }
#undef RUN_TO
}

/* ---- the EQs, the mixer, and what the screens will ask of the audio side: a note to hear, a chain or a phrase
 * alone, a track muted, an edit of the mix while it plays */
static void t_screens(void)
{
    const sp_ch_t *v1 = TRK_VOICE(0), *v3 = TRK_VOICE(2), *v6 = TRK_VOICE(3);
    static const int8_t eqs[5][3] = {{0, 0, 0}, {0, -64, 0}, {64, 0, 0}, {32, 32, 32}, {-64, -64, -64}};
    masif_instrument_t in;
    int32_t o[2u * SP_BLK], pk[5], m0, m1;
    uint32_t b, pass, i;
    uint8_t d[7] = {0, 0, 0, 0, 0, 0, 0};
    int ok = 1;
    it_tempo(1200);
    for (pass = 0; pass < 5u; pass++) {             /* an instrument's EQ on the 440 Hz tone (it is in the mid band) */
        it_reset();
        masif_instrument_default(&in, 0);
        in.sample = W_TONE; in.table_tic = 0; in.volume = 60;
        in.eq[0] = eqs[pass][0]; in.eq[1] = eqs[pass][1]; in.eq[2] = eqs[pass][2];
        ok &= masif_set_instrument(&masif_song, 30, &in);
        it_cell(0, 0, ROOT, 30, 0, 0);
        sq_post(RQ_PLAY, 0, 0);
        pk[pass] = 0;
        for (b = 0; b < 300u; b++) { int32_t k; it_block(o); k = it_peak_blk(o); if (b > 100u && k > pk[pass]) pk[pass] = k; }
        ok &= v1->on && v1->eq_on == (pass != 0u);
    }
    base(ok && pk[0] > 1000 && pk[1] < pk[0] * 3 / 5 && pk[1] > pk[0] / 4 && pk[2] > pk[0] * 13 / 10 && pk[2] < pk[0] * 5 / 2,
         "an instrument's EQ (gentle bands, 6 dB an octave): -12 dB of mid takes the tone down by half, +12 dB of low lifts it (440 Hz is near the low band)");
    base(pk[3] > pk[0] * 19 / 10 && pk[3] < pk[0] * 21 / 10 && pk[4] > pk[0] * 9 / 40 && pk[4] < pk[0] * 11 / 40,
         "all three bands the same way are a plain gain (+6 dB: twice, -12 dB: a quarter): the bands add up to the sound");
    printf(NOTE "peak flat %d, mid -12 dB %d, low +12 dB %d, all +6 dB %d, all -12 dB %d\n", (int)pk[0], (int)pk[1], (int)pk[2], (int)pk[3], (int)pk[4]);

    it_reset();                                     /* the mix's EQ and the mixer, edited while it plays */
    it_cell(0, 0, ROOT, PAD_TONE, 0, 0);
    it_cell(2, 0, ROOT + 7u, PAD_TONE, 0, 0);
    sq_post(RQ_PLAY, 0, 0);
    for (b = 0; b < 60u; b++) it_block(o);
    m0 = it_peak_blk(o);
    d[0] = 40; ok = masif_post_edit(&masif_edits, MASIF_EDIT_MIX, 2, 0, d);
    d[0] = (uint8_t)-64; ok &= masif_post_edit(&masif_edits, MASIF_EDIT_MIX, MASIF_TRACKS + 1, 0, d);
    it_block(o);
    base(ok && sp_trk_mix[2] == 40 && sp_trk_mix[0] == 100 && masif_song.master_eq[1] == -64 && sp_meq.on,
         "the mixer through the edit queue while it plays: a track's level and the mix's EQ are the audio side's at the next block");
    {
        int32_t buf[2u * SP_BLK * 8u], a = 0, e = 0;
        for (i = 0; i < 8u; i++) it_block(buf + i * 2u * SP_BLK);
        for (i = 0; i < 2u * SP_BLK * 8u; i++) { int32_t x = buf[i] < 0 ? -buf[i] : buf[i]; if (x > a) a = x; }
        sp_master_eq(buf, SP_BLK * 8u);             /* (what zp12.c's interrupt does to the mix) */
        for (i = SP_BLK * 8u; i < 2u * SP_BLK * 8u; i++) { int32_t x = buf[i] < 0 ? -buf[i] : buf[i]; if (x > e) e = x; }
        base(a > 1000 && e < a * 2 / 5, "the mix's EQ: -12 dB of mid on everything");
        sp_scope_w = 0;
        sp_scope_tap(buf, SP_BLK * 8u);             /* off: nothing is taken */
        ok = sp_scope_w == 0;
        sp_scope_on = 1;
        sp_scope_tap(buf, SP_BLK * 8u);
        for (i = 0, a = 0; i < SP_SCOPE; i++) a |= sp_scope[i];
        base(ok && sp_scope_w == 0 && a != 0, "the mix's last 256 samples for a screen to draw (a scope, a spectrum): taken only when asked for");
        sp_scope_on = 0;
    }
    masif_mute = 1u << 0;
    it_block(o);
    m1 = it_peak_blk(o);
    base(sp_trk_mix[0] == 0 && sp_trk_mix[2] == 40 && v1->on && m1 < m0 / 2 && m1 > 0, "a muted track plays on unheard; the others are as they were");
    masif_mute = 0;
    sq_post(RQ_STOP, 0, 0);
    for (b = 0; b < 30u; b++) it_block(o);

    masif_fm1_preview(3, PAD_TONE, ROOT + 12u, 90); /* a note to hear, while stopped */
    it_block(o);
    base(!sq.playing && v6->on && v6->w == &sp_wave[W_TONE] && v6->gl == 90 * 64 / 3 && it_nonzero(o),
         "the screens' note to hear: an instrument's note on a track at once, also while stopped");
    masif_fm1_preview(3, 0, 0, 0);
    for (b = 0; b < 30u; b++) it_block(o);
    base(!v6->on, "... and its end");

    it_reset();                                     /* PLAY plays what the screen is on: one phrase alone, on its track */
    it_cell(0, 0, ROOT, PAD_TONE, 0, 0);
    it_cell(2, 0, ROOT, PAD_TONE, 0, 0);
    it_cell(2, 16, ROOT + 5u, PAD_BLIP, 0, 0);      /* (track 3's second phrase, 9) */
    masif_fm1_play_what(MASIF_PLAY_PHRASE, 2, 9);
    sq_post(RQ_PLAY, 0, 0);
    b = 0;
    while (b < it_blk_of_tick(2)) { it_block(o); b++; }
    base(v3->on && v3->w == &sp_wave[W_BLIP] && !v1->on && masif_runtime.tracker.what == MASIF_PLAY_PHRASE,
         "PLAY with a phrase chosen: that phrase alone on its track, the song's other tracks rest");
    while (b < it_blk_of_tick(16 * 24 + 2)) { it_block(o); b++; }
    base(masif_runtime.tracker.phrase[2] == 9 && masif_runtime.tracker.row[2] == 1, "... round and round");
}

/* ---- the built-in demo song (masif_demo.c, MASIF_DEMO_SONG=1): the drums of s_disjoint under it. Its pads get
 * instruments get the test kit's waves here */
static int demo_ok;
static void s_demo(unsigned parts)
{
    static const uint8_t wave[MASIF_DEMO_INSTRUMENTS] = {W_TONE, W_TONE, W_TONE, W_TONE, W_HAT};
    uint32_t i;
    s_disjoint(parts & P_DRUMS);
    if (!(parts & P_TRACKER)) return;
    demo_ok = masif_demo_load(&masif_song);
    for (i = 0; i < MASIF_DEMO_INSTRUMENTS; i++) {  /* (the factory kit's waves are not here: the test kit's) */
        demo_ok &= masif_song.instrument[i].sample >= W_N;
        masif_song.instrument[i].sample = wave[i];
    }
}

static void t_demo(void)
{
    static const uint8_t wave_of[MASIF_TRACKS] = {W_TONE, W_TONE, W_TONE, W_TONE};
    const uint32_t pass = MASIF_DEMO_STEPS * IT_ROWS * 24u;     /* the song once, in ticks */
    int32_t o[2u * SP_BLK];
    uint32_t b = 0, t, r, p, notes = 0, bad = 0, sounded = 0, late = 0, dive = 0, arp = 0, roll = 0, wrong = 0;
    uint32_t was[MASIF_TRACKS] = {0, 0, 0, 0}, n, nz = 0;
    int step2, looped, cut;
    it_sup_t sup;

    it_tempo(1200);
    it_reset();
    s_demo(P_TRACKER);
    for (t = 0; t < MASIF_TRACKS; t++)              /* every note: an instrument with a pad, in the core's pitch range */
        for (p = 0; p < MASIF_DEMO_STEPS; p++)
            for (r = 0; t < 4u && r < IT_ROWS; r++) {          /* (the demo's phrases: (p * 4 + its track) * 4 + 0..3) */
                const masif_cell_t *c = &masif_song.phrase[(p * 4u + t) * 4u + r / MASIF_ROWS][r % MASIF_ROWS];
                int32_t semis;
                if (c->note < 1u || c->note > 120u) continue;
                notes++;
                if (c->instrument >= MASIF_DEMO_INSTRUMENTS || !sp_wave[masif_song.instrument[c->instrument].sample].n) { bad++; continue; }
                semis = (int32_t)c->note - masif_song.instrument[c->instrument].root_note;
                bad += semis < -24 || semis > 24;
            }
    base(demo_ok && masif_song.song[0][1] == 1u && masif_song.song[1][1] == 5u && masif_song.song[2][0] == MASIF_NONE &&
         notes > 60u && !bad,
         "the demo song loads: two song rows of chains on four tracks; every note has an instrument with a sample, within two octaves of its root");
    printf(NOTE "%u notes, %u bad\n", (unsigned)notes, (unsigned)bad);

    n = IT_MAXB;                                    /* (the first two bars and a bit at 120 BPM) */
    sup = it_superpose(s_demo, n);
    base_map(demo_ok && sup.nz_drums && sup.nz_tracker && !sup.ndiff,
             "the demo over the drums: the mix is exactly drums + tracker");
    printf(NOTE "%u samples differ over %u blocks (drums sound in %u, the tracker in %u)\n", (unsigned)sup.ndiff,
           (unsigned)n, (unsigned)sup.nz_drums, (unsigned)sup.nz_tracker);

    it_tempo(2400);                                 /* the whole song and round again, at the device test's tempo */
    it_reset();
    s_demo(P_TRACKER);
    sq_post(RQ_PLAY, 0, 0);
#define RUN_TO(T) while (b < it_blk_of_tick(T)) { \
        uint32_t row = it_tick_of_blk(b) / 24u % IT_ROWS; \
        it_block(o); b++; nz += (uint32_t)it_nonzero(o); \
        for (t = 0; t < MASIF_TRACKS; t++) { \
            const sp_ch_t *v = TRK_VOICE(t); \
            if (!v->on) { was[t] = 0; continue; } \
            sounded |= 1u << t; \
            if (b > it_blk_of_tick(pass)) late |= 1u << t; \
            wrong += v->w != &sp_wave[t == 3u && v->w == &sp_wave[W_HAT] ? W_HAT : wave_of[t]]; \
            dive += t == 0u && row == 60u && v->step < v->base_step - v->base_step / 32u; \
            arp += t == 1u && v->step > v->base_step; \
            roll += t == 3u && row >= 60u && v->w == &sp_wave[W_TONE] && v->pos < was[t]; \
            was[t] = v->pos; \
        } }
    RUN_TO(IT_ROWS * 24u + 12u);
    step2 = masif_runtime.tracker.song_row[1] == 1u && masif_runtime.tracker.chain[1] == 5u &&
            masif_runtime.tracker.chain[2] == 6u && masif_runtime.tracker.chain[0] == 0u && masif_runtime.tracker.on[3];
    RUN_TO(pass + 12u);
    looped = masif_runtime.tracker.song_row[0] == 0u && masif_runtime.tracker.song_row[1] == 0u && masif_runtime.tracker.row[0] == 1u && masif_runtime.tracker.on[0];
    RUN_TO(pass + 16u * 24u);
    base_map(demo_ok && sounded == 15u && !wrong && nz > 1000u,
             "PLAY plays the demo on the real core: all four tracks sound, each on its own voice with its instrument's wave");
    base(dive && arp && roll >= 12u,
         "the demo's effects reach the voices: the bass's PBN dive, the ARPEGGIO, the RETRIGGER roll");
    printf(NOTE "blocks with the bend half a semitone down or more: %u; with the arpeggio above the note: %u; retriggers in the roll: %u\n",
           (unsigned)dive, (unsigned)arp, (unsigned)roll);
    base(step2 && looped && late == 15u,
         "the song: its second row gives tracks 2 and 3 their other chains, at its end every track is back at the first row and plays on");
    sq_post(RQ_STOP, 0, 0);
    it_block(o);
    cut = 1;
    for (t = 0; t < MASIF_TRACKS; t++) cut &= !TRK_VOICE(t)->on;
    for (r = 0; r < 200u; r++) it_block(o);
    base(cut && !it_nonzero(o), "STOP cuts the demo: no tracker voice plays on, the mix falls silent");
    sq_post(RQ_PLAY, 0, 0);
    b = 0;
    sounded = 0;
    RUN_TO(1);
    base(masif_runtime.tracker.song_row[0] == 0u && (sounded & 3u) == 3u, "PLAY again: the demo starts from its first row");
#undef RUN_TO
}

int main(int argc, char **argv)
{
    int strict = argc > 1 && !strcmp(argv[1], "--strict");
    int missing = 0, not_met, bad;
    uint32_t i;
    it_kit();
    if (argc > 1 && !strcmp(argv[1], "--drum-ref")) {   /* to compare with the MASIF_ENABLE_TRACKER=0 build's */
        it_drum_ref();
        it_clk_ref();
        return 0;
    }

    printf("MASIF integration: sp_core + sp_seq + masif_tracker + masif_runtime + masif_fm1_adapter\n");
    printf("\n-- baseline (current behaviour, must hold; [mapping]: holds for track t = voice 8 + t)\n");
    t_adapter_voices();
    t_tempo_link();
    t_disjoint();
    t_aligned_when_equal();
    t_count_in_seq();

    printf("\n-- baseline: the tracker's own voices (drums as the drums-only reference, bounds, pitch cap, faders, MUTE / SOLO)\n");
    t_voice_layout();
    it_drum_ref();
    t_drums_identical();
    t_choke();
    t_track_bounds();
    t_pitch_clamp();
    t_gain_mute();

    printf("\n-- contract: drum-voice isolation (met: the tracker has its own voices)\n");
    t_isolation(C_ISO_NOTE, "a tracker note on track 1 leaves a pad ringing on channel 1 alone", s_note_on_pad_channel, 1);
    t_isolation(C_ISO_CUT, "a tracker NOTE CUT on track 1 leaves a pad ringing on channel 1 alone", s_cut_on_pad_channel, 0);
    t_isolation(C_ISO_SLIDE, "a tracker slide on track 1 does not bend a pad hit on channel 1", s_slide_on_pad_channel, 1);
    t_isolation(C_ISO_PAD, "a pad hit on channel 2 leaves track 2's note alone", s_pad_on_track_channel, 1);

    printf("\n-- contract: shared-clock transport (met: the tracker runs on the sequencer's clock)\n");
    t_stopped_silent();
    t_play_aligns();
    t_stop_halts();
    t_count_in();
    t_one_clock();

    printf("\n-- baseline: the shared clock, case by case (PLAY, STOP, count-in, tempo, start / stop cycles, the drums)\n");
    t_clk_play();
    t_clk_stop();
    t_clk_count_in();
    t_clk_tempo();
    t_clk_cycles();
    it_clk_ref();
    t_clk_drums();

    printf("\n-- baseline: effects on the track's voice (PAN, SEND, SAMPLE OFFSET, REVERSE)\n");
    t_voice_fx();

    printf("\n-- baseline: instruments (slices, backwards, loops, level, pan, tables), the song's tempo\n");
    t_instruments();
    t_loops();
    t_tapers();
    t_tempo_trig();

    printf("\n-- baseline: the sampler side of an instrument (envelope, filter, LFO)\n");
    t_sampler();

    printf("\n-- baseline: the EQs, the mixer, and what the screens need (a note to hear, a phrase alone, mutes)\n");
    t_screens();

    printf("\n-- baseline: the built-in demo song (MASIF_DEMO_SONG=1)\n");
    t_demo();

    for (i = 0; i < C_N; i++)
        if (!goal_seen[i]) {
            printf("CONTRACT  %-8s %-10s %-6s was not checked\n", "-", "MISSING", goal_tab[i].id);
            missing++;
        }
    not_met = C_N - goal_met;
    bad = base_bad || goal_regressed || goal_invalid || missing;
    printf("\nbaseline:  %s (%d failed)\n", base_bad ? "FAIL" : "PASS", base_bad);
    printf("contracts: %d of %d met, %d NOT MET; against goal_tab[]: %d milestone, %d regressed, %d invalid, %d missing\n",
           goal_met, (int)C_N, not_met - goal_invalid - missing, goal_milestone, goal_regressed, goal_invalid, missing);
    if (bad) {
        printf("result:    FAIL (exit 1)\n");
        return 1;
    }
    if (goal_milestone) {
        printf("result:    MILESTONE: %d contract(s) newly met; set them to MET in goal_tab[] (exit 2)\n", goal_milestone);
        return 2;
    }
    if (strict && not_met) {
        printf("result:    --strict: %d contract(s) NOT MET, as expected (exit 4)\n", not_met);
        return 4;
    }
    printf("result:    PASS: baseline holds, every contract has its expected status (exit 0)\n");
    return 0;
}
#endif
