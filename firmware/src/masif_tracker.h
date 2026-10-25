/* MASIF tracker core: the song and its row player. Portable C, independent of the FM-1 HAL. */
#ifndef MASIF_TRACKER_H
#define MASIF_TRACKER_H
#include <stdint.h>

#define MASIF_TRACKS 4u
/* The song as a tracker's (after LSDj): a SONG of rows, each naming a CHAIN for every track; a chain is up to 16 PHRASES,
 * each with a transpose; a phrase is 16 rows of cells. Every track goes down its own column of the song. */
#define MASIF_SONG_ROWS 256u
#define MASIF_CHAINS 128u
#define MASIF_CHAIN_STEPS 16u
#define MASIF_PHRASES 255u
#define MASIF_ROWS 16u                   /* rows a phrase */
#define MASIF_NONE 0xFFu                 /* no chain in a song cell, no phrase in a chain step */
#define MASIF_INSTRUMENTS 128u
#define MASIF_NAME_LEN 8u                /* an instrument's name */
#define MASIF_TABLES MASIF_INSTRUMENTS   /* table i is instrument i's own: it starts with each of its notes */
#define MASIF_TABLE_ROWS 16u
#define MASIF_ORDER_STEPS 64u
#define MASIF_SAMPLES 64u                /* the sampler's waves: the kit's, then the user's sample slots */
#define MASIF_NO_SAMPLE 0xFFu            /* an instrument's sample: none (an empty instrument; a new song's are all) */
#define MASIF_NOTE_NONE 0u
#define MASIF_NOTE_CUT  0xFEu
#define MASIF_NOTE_OFF  0xFFu
#define MASIF_DEFAULT_ROOT_NOTE 60u      /* notes are MIDI numbers: 60 = C4 */
/* The clock is the drum sequencer's: 96 ticks a quarter, 24 a row of 16ths. A TRACKER TICK is 4 of them: 6 a row,
 * as a ProTracker's at its usual speed. Every effect parameter that counts time counts tracker ticks. */
#define MASIF_TICK 4u
#define MASIF_ROW_TICKS 6u               /* tracker ticks a row where no groove says otherwise */

/* The effects. "xy": the parameter's two hex digits. */
/* small before fast, for what never runs in the audio interrupt (the factory-kit build has no bytes to spare) */
#if defined(__clang__)
#define MASIF_SMALL __attribute__((minsize))
#else
#define MASIF_SMALL
#endif
#define MASIF_LFO_DIVS 16u
#define MASIF_GROOVES 16u
#define MASIF_GROOVE_STEPS 16u
#define MASIF_GROOVE_MAX 0x3Fu          /* tracker ticks a step at the most */
/* The effects. Their numbers are kept (saved songs): the ones taken out (the user, 2026-10-23: other effects do what
 * they did) are MASIF_FX_X_*, cleared from a song where it is loaded (masif_song_sanitize) and never played. */
typedef enum {
    MASIF_FX_NONE = 0,
    MASIF_FX_SET_TEMPO,     /* the tempo in BPM, 28-F0 (40-240): the sequencer's, drums and all */
    MASIF_FX_X_JUMP,        /* (JMP: out) */
    MASIF_FX_BREAK,         /* HOP: this track's phrase ends here: on to its next phrase at row x (0-F) */
    MASIF_FX_X_SLIDE_UP,    /* (SLU: out, PBN bends) */
    MASIF_FX_X_SLIDE_DOWN,  /* (the old SLD: out) */
    MASIF_FX_SLIDE,         /* SLD: the row's note is not hit: the sounding one slides to it, up or down as the note is,
                             * xx / 16 semitones a clock tick (PRT until 2026-10-23) */
    MASIF_FX_VIBRATO,       /* x speed, y depth (y / 8 semitones) */
    MASIF_FX_ARPEGGIO,      /* the note, + x, + y semitones, a tracker tick each */
    MASIF_FX_RETRIGGER,     /* RET xy: hit again every y tracker ticks (3: 32nds, 6: 16ths, C: 8ths; 0: once, where it
                             * begins), on over the rows that repeat it; x: each hit softer (1-7, 7 the fastest) or
                             * louder (9-F), 0 and 8: as it is */
    MASIF_FX_X_NOTE_DELAY,  /* (DLY: out, MTT moves a row) */
    MASIF_FX_X_NOTE_CUT,    /* (CUT: out, DED and STP end a note) */
    MASIF_FX_SET_TABLE,     /* TBL: run table xx on this track from its first row */
    MASIF_FX_X_TABLE_SPEED, /* (TIC: out, the instrument's table speed) */
    MASIF_FX_PROBABILITY,   /* the row's note plays in xx of 100 passes (decimal value: 32 hex = 50 %) */
    MASIF_FX_STOP,          /* STP: the note is let go after xx tracker ticks (00: at the row's start): its release
                             * rings out. (CONDITION's number until 2026-10-12: that one was never played) */
    MASIF_FX_SAMPLE_OFFSET, /* OFS: the note starts xx / 256 into its sample, from the sample's own start (not its
                             * part's or its slice's); to its part's end, or the sample's if that is before it */
    MASIF_FX_REVERSE,       /* the note plays backwards (xx not 0) */
    MASIF_FX_PAN,           /* 00 left, 80 the middle, FF right: this step's (until the track's next note) */
    MASIF_FX_SEND,          /* x the bus (0 chorus, 1 delay, 2 reverb), y how much: until the next PLAY */
    MASIF_FX_TABLE_HOP,     /* THO: the track's table goes on at its row x (in a table: a loop) */
    MASIF_FX_KILL,          /* DED: after xx tracker ticks (00: at once) the track is dead: its voice cut (no release),
                             * its table, its running effects, its slide and a REP with it */
    MASIF_FX_REPEAT,        /* REP xx: the last command of this column again on every row from here, its
                             * value + xx a row (80-FF: down), also on rows that leave the column empty, until
                             * another command or REP 00. The value stays inside 00..FF */
    MASIF_FX_REPEAT_TO,     /* RTO xx: after REP: the value its repeats stop at (they go on with it) */
    MASIF_FX_RANDOM_LEFT,   /* RNL xy: what is left of it is random on every pass. In the second column: the
                             * first column's value, its first digit + 0..x, its second + 0..y (round past F; F: any).
                             * In the first: the row's note + 0..x semitones and its instrument + 0..y (in a table:
                             * the transpose + 0..x, the volume - 0..y eighths of the whole) */
    MASIF_FX_GROOVE,        /* GRV xx: this track's rows take their lengths from groove xx, from this row on */
    MASIF_FX_X_GROOVE_ALL,  /* (GGR: out, GRV on each track) */
    MASIF_FX_MICROTIME,     /* MTT xx: this row begins late (01-7F) or early (FF-81 = 1-127) by that many clock ticks,
                             * 18 hex = a 16th: the row before it is that much longer or shorter, the row itself the
                             * other way, so the rows after it are where they were (swing for one step). At the most
                             * to the row before's begin and to its own end */
    MASIF_FX_PITCH_BEND,    /* PBN xy: the pitch bends up for x rows (x 1-E; F: until the next note), then down for y,
                             * 1/16 semitone a clock tick (1.5 semitones a row of 6 ticks); it stays where it ends */
    MASIF_FX_SLICE,         /* SLC xx: a sliced instrument's note plays slice xx (00 the first; past the last: round),
                             * at the note's pitch (the root note: the slice's own) */
    MASIF_FX_COUNT,
    MASIF_FX_USER0 = 0x80
} masif_fx_t;
#define MASIF_FX_OUT(f) ((f) == MASIF_FX_X_JUMP || (f) == MASIF_FX_X_SLIDE_UP || (f) == MASIF_FX_X_SLIDE_DOWN || \
                         (f) == MASIF_FX_X_NOTE_DELAY || (f) == MASIF_FX_X_NOTE_CUT || (f) == MASIF_FX_X_TABLE_SPEED || \
                         (f) == MASIF_FX_X_GROOVE_ALL)

typedef struct {
    uint8_t note;       /* 1..120 note, 0 none, FE cut, FF note-off */
    uint8_t instrument; /* 0..127; 0xFF means unchanged */
    uint8_t volume;     /* 0..127; 0xFF means unchanged */
    uint8_t fx;         /* the row's two effects, each with its parameter. Where both would do the same thing */
    uint8_t param;      /* (two retriggers, two delays ...) the first one counts */
    uint8_t fx2;
    uint8_t param2;
} masif_cell_t;
#define MASIF_FX_COLS 2u

/* A table row (16 a table), a tracker tick long unless TIC says otherwise; after row F the table goes on at row 0.
 * Its lanes: N, V, FX1, FX2. An effect is any of the effects above: it becomes the track's running effect in its
 * column as if a phrase row had it. */
typedef struct {
    int8_t transpose;   /* N: semitones on top of the phrase's note, while this row lasts */
    uint8_t volume;     /* V: 0..127: the note's volume from here on; 0xFF: as it is */
    uint8_t fx, param;  /* FX1 */
    uint8_t fx2, param2;/* FX2 */
} masif_table_cell_t;

/* A chain's step: a phrase (MASIF_NONE: the chain ends before this step) and what it adds to the phrase's notes. */
typedef struct { uint8_t phrase; int8_t transpose; } masif_chain_step_t;

/* How a note plays its part, nine of them: once, or round its loop (loop_start .. loop_end) once
 * it has reached it. The REV ones begin at the part's end. PING (PP): to and fro between the loop's two
 * ends. OSC: the loop alone, as an oscillator: the start is not played and a new note does not begin it again
 * while it sounds. Odd: backwards (the first nine; sp_trigger_track_wave takes those numbers). REPITCH: FWDLOOP at
 * the pitch that makes the whole part last `steps` phrase rows (16ths) at the tempo as it is when the note starts;
 * BPM: at the pitch that brings a sample recorded at `steps` BPM to the tempo. Their PING: FWD PP so. */
typedef enum { MASIF_PLAY_FWD = 0, MASIF_PLAY_REV, MASIF_PLAY_LOOP, MASIF_PLAY_REV_LOOP, MASIF_PLAY_PING,
               MASIF_PLAY_REV_PING, MASIF_PLAY_OSC, MASIF_PLAY_OSC_REV, MASIF_PLAY_OSC_PING,
               MASIF_PLAY_REPITCH, MASIF_PLAY_REPITCH_PING, MASIF_PLAY_BPM, MASIF_PLAY_BPM_PING, MASIF_PLAY_MODES } masif_play_t;
#define MASIF_INST_FREE 1u
#define MASIF_INST_SMPLOOP 2u            /* its loop is its sample's own (sp_wave lp_a .. lp_b, sample-exact) */
/* An instrument's modulation: MASIF_MODS slots, each an envelope or an LFO on one of the instrument's
 * settings while a note sounds. (The envelope on the level itself is the instrument's own: attack .. release.) */
#define MASIF_MODS 4u
typedef enum { MASIF_MOD_OFF = 0,
               MASIF_MOD_AHD,       /* an envelope: p[0] attack, p[1] hold, p[2] decay (0 at once .. 127 ~2.5 s) */
               MASIF_MOD_ADSR,      /* p[0] attack, p[1] decay, p[2] sustain (0..127), p[3] release (at NOTE OFF) */
               MASIF_MOD_LFO,       /* p[0] masif_lfo_shape_t, p[1] masif_lfo_trig_t, p[2] rate, p[3] sync. Sync 0: the
                                     * rate 0..127 is 0.08 .. 45 Hz; 1: it is a length in the tempo, one of
                                     * MASIF_LFO_DIVS (8 bars .. a 64th; past the last: the last) */
               MASIF_MOD_TRIG,      /* the trigger envelope: an AHD (p[0] attack, p[1] hold, p[2] decay) that this
                                     * instrument's own notes do not start: every note of the source p[3] does
                                     * (00-7F: that instrument, on any track; 80-83: anything on track 1-4).
                                     * At rest it is 0: with a negative amount on the volume it ducks (a side chain) */
               MASIF_MOD_TYPES } masif_mod_type_t;
#define MASIF_TRIG_SRC_MAX (0x80u + MASIF_TRACKS - 1u)
typedef enum { MASIF_DEST_OFF = 0,
               MASIF_DEST_VOLUME,   /* as a VCA: an envelope opens the level (amount < 0: closes it), an LFO dips it */
               MASIF_DEST_PITCH,    /* +-24 semitones at the full amount; the amount counts squared (8: a vibrato) */
               MASIF_DEST_PAN, MASIF_DEST_CUTOFF, MASIF_DEST_RES,           /* added to the instrument's own */
               MASIF_DEST_CHORUS, MASIF_DEST_DELAY, MASIF_DEST_REVERB,      /* the sends */
               MASIF_DEST_MOD_AMT,  /* the next slot's amount, as VOLUME does a level */
               MASIF_DEST_MOD_RATE, /* the next slot's rate, if it is an LFO */
               MASIF_DESTS } masif_dest_t;
typedef enum { MASIF_LFO_TRI = 0, MASIF_LFO_SIN, MASIF_LFO_RAMP_DOWN, MASIF_LFO_RAMP_UP, MASIF_LFO_EXP_DOWN,
               MASIF_LFO_EXP_UP, MASIF_LFO_SQU_DOWN, MASIF_LFO_SQU_UP, MASIF_LFO_RANDOM, MASIF_LFO_DRUNK,
               MASIF_LFO_SHAPES } masif_lfo_shape_t;
typedef enum { MASIF_TRIG_FREE = 0, /* it runs on, whatever the notes do */
               MASIF_TRIG_RETRIG,   /* from its start with every note */
               MASIF_TRIG_HOLD,     /* where it is when the note starts, for the whole note */
               MASIF_TRIG_ONCE,     /* one cycle from the note's start, then it stands */
               MASIF_LFO_TRIGS } masif_lfo_trig_t;
typedef struct {
    uint8_t type;       /* masif_mod_type_t */
    uint8_t dest;       /* masif_dest_t */
    int8_t amount;      /* -64..64 */
    uint8_t p[4];       /* what the type says, 0..127 each (a shape or a trig mode out of range counts round; a
                         * TRIG's source to MASIF_TRIG_SRC_MAX) */
} masif_mod_t;
typedef enum { MASIF_FILTER_OFF = 0, MASIF_FILTER_LP, MASIF_FILTER_BP, MASIF_FILTER_HP, MASIF_FILTERS } masif_filter_t;
/* An instrument: a sample (or a part of one) and how a note plays it. Positions are in 1/256 of the sample. */
typedef struct {
    uint8_t sample;     /* 0..63: which wave of the sampler; MASIF_NO_SAMPLE: none, the instrument is empty */
    uint8_t root_note;  /* the note that plays the sample at its own pitch */
    int8_t fine;        /* cents, -99..99 */
    uint8_t volume;     /* 0..127 */
    int8_t pan;         /* -64 left .. 64 right */
    uint8_t play;       /* masif_play_t: once forwards or backwards, or round its loop, or to and fro in it */
    uint8_t start;      /* where the part begins */
    uint8_t loop_start; /* a looping one: where its loop begins (inside the part; outside it: at the part's start) */
    uint8_t length;     /* the part's length from start; 0: to the sample's end */
    uint8_t slices;     /* 2..64: the part is that many equal slices and a note picks one, the root note the first,
                         * each at the sample's own pitch; 0 or 1: the note is the pitch */
    uint8_t decay;      /* the envelope after its attack, down to `sustain`: 0 short .. 126 ~2.5 s, 127: none */
    uint8_t send[3];    /* chorus, delay, reverb, 0..127 */
    uint8_t table_tic;  /* tracker ticks a row of its table; 0: its table does not run */
    uint8_t flags;      /* bit 0 (MASIF_INST_FREE): its notes are not held to the project's scale (drums, effects);
                         * bit 1 (MASIF_INST_SMPLOOP): a looping one loops where its sample's own loop is, not at
                         * loop_start / loop_end (set when the sample pool gives it a sample that has one) */
    /* the envelope's other three, the filter */
    uint8_t attack;     /* 0 at once .. 127 ~2.5 s */
    uint8_t sustain;    /* 0..127: the level the decay ends at and holds (0: the note dies away, as a drum) */
    uint8_t release;    /* after NOTE OFF: 0 cut at once .. 126 ~2.5 s */
    uint8_t filter;     /* masif_filter_t: none, low-pass, band-pass, high-pass (12 dB an octave, resonant) */
    uint8_t cutoff;     /* 0..127: 60 Hz .. 12 kHz, 8 octaves */
    uint8_t resonance;  /* 0..127 */
    int8_t eq[3];       /* a 3-band EQ: low (below 250 Hz), mid, high (above 3 kHz), -64..64 = -12..+12 dB; 0: flat */
    masif_mod_t mod[MASIF_MODS];    /* its modulation: envelopes and LFOs */
    uint8_t drive;      /* 0: none; 1..127: louder into a soft clip, 1x .. 9x (saturation up to distortion), after
                         * the envelope and before the filter */
    uint8_t loop_end;   /* a looping one: where its loop ends; 0 (or outside the part): at the part's end */
    uint8_t steps;      /* REPITCH: the phrase rows the part lasts; BPM: the sample's own tempo; 0: its own pitch */
    uint8_t taper_in;   /* a looping one: the level rises over this much of the loop after its start, and falls */
    uint8_t taper_out;  /* over this much before its end: 0..128 in 1/256 of the loop (0: none; the user, 2026-10-15) */
    uint8_t name[MASIF_NAME_LEN];   /* its name: ' '..'_', ending at the first 0 (all 0: none) */
} masif_instrument_t;
#define MASIF_TAPER_MAX 128u
#define MASIF_NAME_OK(c) ((c) == 0u || ((c) >= 0x20u && (c) < 0x60u))   /* a name's character: none, or the font's */

typedef struct {
    uint8_t song[MASIF_SONG_ROWS][MASIF_TRACKS];             /* chain numbers */
    masif_chain_step_t chain[MASIF_CHAINS][MASIF_CHAIN_STEPS];
    masif_cell_t phrase[MASIF_PHRASES][MASIF_ROWS];
    masif_table_cell_t table[MASIF_TABLES][MASIF_TABLE_ROWS];
    masif_instrument_t instrument[MASIF_INSTRUMENTS];
    /* the project and the mixer */
    uint8_t tempo;                      /* BPM 40..240 the song starts at; 0: as the machine is set */
    uint8_t track_level[MASIF_TRACKS];  /* the mixer's faders, 0..127 (100: as an instrument is set) */
    int8_t master_eq[3];                /* the mix's 3-band EQ, as an instrument's */
    /* the project's key and scale: every note that is played is held to them (masif_scale_note) */
    uint8_t key;                        /* 0..11: C, C#, D ... B */
    uint8_t scale;                      /* 0: chromatic (every note as written), else one of masif_scale_name[] */
    /* the grooves: how many tracker ticks each row of a track lasts, step after step, round. A step
     * 01..3F; 00: the phrase's row is passed over; FF (empty): back to the first step. A track is on the project's
     * groove until GRV says otherwise; an empty song's grooves are 06 06 (16ths) */
    uint8_t groove[MASIF_GROOVES][MASIF_GROOVE_STEPS];
    uint8_t project_groove;             /* the groove every track is on after PLAY */
} masif_song_t;

typedef struct { uint8_t note, instrument, volume, fx, param, fx2, param2; } masif_row_event_t;

/* What plays: the song from a row on, or (for the screens) one chain or one phrase on one track, round and round. */
typedef enum { MASIF_PLAY_SONG = 0, MASIF_PLAY_CHAIN, MASIF_PLAY_PHRASE } masif_play_what_t;
typedef struct {
    masif_song_t *song;
    uint8_t what;                       /* masif_play_what_t */
    uint8_t on[MASIF_TRACKS];           /* this track plays */
    uint8_t song_row[MASIF_TRACKS];     /* where each track is: its song row, */
    uint8_t chain[MASIF_TRACKS];        /* the chain there (or the one that loops), */
    uint8_t step[MASIF_TRACKS];         /* the step in it, */
    uint8_t phrase[MASIF_TRACKS];       /* the phrase there (or the one that loops), */
    uint8_t row[MASIF_TRACKS];          /* and the row in it that plays next */
} masif_tracker_t;

void masif_song_clear(masif_song_t *song);
/* The scales a project can be in. A note that is not in the project's key and scale plays as the next one below
 * that is (C# in C major: C). Not held to it: the notes of an instrument cut in slices (a note is a slice there)
 * and of an instrument marked free (MASIF_INST_FREE). What tables, arpeggios and slides add is not held either. */
#define MASIF_SCALES 16u
extern const char *const masif_scale_name[MASIF_SCALES];
uint8_t masif_scale_note(const masif_song_t *song, uint8_t note);
/* instrument i of an empty song: no sample (MASIF_NO_SAMPLE); when it gets one: the whole of it, once, at its own level */
void masif_instrument_default(masif_instrument_t *in, uint8_t i);
/* A song from outside (flash, USB): everything the player indexes with is put in range, so that no content can
 * make it read or write outside the song. Cells and table rows may hold anything (the player checks them as it
 * plays). Returns the number of values it had to change (0: the song was sound). */
unsigned masif_song_sanitize(masif_song_t *song);
int masif_set_instrument(masif_song_t *song, uint8_t instrument, const masif_instrument_t *config);
int masif_set_cell(masif_song_t *song, uint8_t phrase, uint8_t row, const masif_cell_t *cell);
int masif_set_chain_step(masif_song_t *song, uint8_t chain, uint8_t step, uint8_t phrase, int8_t transpose);
int masif_set_song_cell(masif_song_t *song, uint8_t row, uint8_t track, uint8_t chain);
int masif_set_table_cell(masif_song_t *song, uint8_t table, uint8_t row, masif_table_cell_t cell);
void masif_tracker_init(masif_tracker_t *tracker, masif_song_t *song);
/* From the top of song row `row`: every track that has a chain there plays, each down its own column; at the end
 * of its column (an empty cell, or the song's last row) a track goes back up to the first row of that run of
 * chains. So a song loops, and tracks with chains of different lengths drift apart. */
void masif_tracker_start_song(masif_tracker_t *tracker, uint8_t row);
/* One chain, or one phrase, on one track, round and round (the other tracks rest). */
void masif_tracker_start_chain(masif_tracker_t *tracker, uint8_t track, uint8_t chain);
void masif_tracker_start_phrase(masif_tracker_t *tracker, uint8_t track, uint8_t phrase);
/* Emits every track's current row (the chain's transpose in its note), then moves each track on (BREAK
 * is played here). */
void masif_tick_row(masif_tracker_t *tracker, masif_row_event_t out[MASIF_TRACKS]);
void masif_tick_track(masif_tracker_t *tracker, uint8_t track, masif_row_event_t *out);   /* one track's next row */
int masif_set_groove(masif_song_t *song, uint8_t groove, uint8_t step, uint8_t ticks);

#endif
