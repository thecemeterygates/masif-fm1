/* MASIF runtime bridge: portable scheduler-to-audio event interface. */
#ifndef MASIF_RUNTIME_H
#define MASIF_RUNTIME_H
#include "masif_tracker.h"

#define MASIF_EDIT_CAPACITY 128u  /* (a paste of 16 song rows is 64 edits, of an instrument up to 53) */
#define MASIF_PITCH_SEMI 16       /* pitch offsets are in 1/16 semitone (6.25 cents) */
#define MASIF_PITCH_MAX (48 * MASIF_PITCH_SEMI)
#define MASIF_BEND_ON 0xFFFFu     /* PBN's F: no end */

typedef enum {
    MASIF_EVENT_NOTE_ON = 1,
    MASIF_EVENT_NOTE_OFF,
    MASIF_EVENT_CUT,
    MASIF_EVENT_EFFECT,     /* a row's or a table row's effect, after its note: fx, param (PAN, SEND, SET_TEMPO are the host's) */
    MASIF_EVENT_VOLUME,     /* the sounding note's volume is `velocity` from now on */
    MASIF_EVENT_PITCH,      /* the sounding note's pitch is `pitch_offset` from its own */
    MASIF_EVENT_RETRIGGER   /* the sounding note is hit again */
} masif_event_type_t;

typedef struct {
    uint8_t type;
    uint8_t track;
    uint8_t instrument;   /* 0..127: the song's instrument that plays (the host reads how from the song) */
    uint8_t note;
    uint8_t velocity;     /* 0..127: the cell's volume x the instrument's x the table's, and RET's ramp */
    uint8_t fx, param;    /* the row's (or the table's) two effects as they stand */
    uint8_t fx2, param2;
    int16_t pitch_offset; /* from the note's pitch, in 1/MASIF_PITCH_SEMI semitones */
} masif_audio_event_t;

/* An edit of the song while it may be playing: posted by the main loop (the screens, the USB link), applied by
 * the audio side between two ticks. What a, b and data mean: */
typedef enum {
    MASIF_EDIT_CELL = 0,    /* a phrase, b row, data: the cell's note, instrument, volume, fx, param, fx2, param2 */
    MASIF_EDIT_CHAIN,       /* a chain, b step, data[0] phrase (or MASIF_NONE), data[1] transpose */
    MASIF_EDIT_SONG,        /* a song row, b track, data[0] chain (or MASIF_NONE) */
    MASIF_EDIT_TABLE,       /* a table, b row, data: transpose, volume, fx, param, fx2, param2 */
    MASIF_EDIT_INSTRUMENT,  /* a instrument, b the byte of masif_instrument_t, data[0] its value (refused if it makes
                             * the instrument one that cannot be) */
    MASIF_EDIT_MIX          /* a: 0-7 a track's level, 8-10 the master EQ's low / mid / high, 11 the song's tempo,
                             * 12 its key (0-11), 13 its scale; MASIF_TRACKS + 6: the project's
                             * groove; data[0] the value */
    , MASIF_EDIT_GROOVE     /* a: the groove, b: its step; data[0] the tracker ticks (FF: empty) */
} masif_edit_kind_t;
typedef struct {
    uint8_t kind, a, b;
    uint8_t data[7];
} masif_cell_edit_t;

typedef struct {
    /* Single producer (main/UI), single consumer (audio ISR). One slot stays empty. */
    volatile uint8_t write_index;
    volatile uint8_t read_index;
    volatile uint16_t dropped;
    masif_cell_edit_t items[MASIF_EDIT_CAPACITY];
} masif_edit_queue_t;

typedef void (*masif_event_sink_fn)(void *context, const masif_audio_event_t *event);

typedef struct {
    masif_tracker_t tracker;
    uint32_t tick_phase_q16;
    uint8_t tick_in_row;                /* (track 1's row_tick: what the tests look at) */
    /* every track has its rows' time to itself (grooves, MTT): clock ticks into its row, the row's length, by how
     * much MTT moved the row's begin, its groove and the step in it that comes next */
    uint8_t row_tick[MASIF_TRACKS];
    uint8_t row_len[MASIF_TRACKS];      /* 0: its first row is still to come */
    int8_t row_shift[MASIF_TRACKS];
    uint8_t groove[MASIF_TRACKS];
    uint8_t groove_pos[MASIF_TRACKS];
    uint16_t bpm10; /* BPM x 10, e.g. 1200 = 120.0 BPM */
    uint8_t ticks_per_row; /* clock ticks: 24 = 16th rows at 96 PPQ (a row's own length: row_len, its groove's) */
    uint8_t active_instrument[MASIF_TRACKS];
    uint8_t active_volume[MASIF_TRACKS];
    uint8_t active_note[MASIF_TRACKS];
    uint8_t effect[MASIF_TRACKS][MASIF_FX_COLS];        /* the two effect columns' running effects */
    uint8_t effect_param[MASIF_TRACKS][MASIF_FX_COLS];
    uint8_t effect_tick[MASIF_TRACKS][MASIF_FX_COLS];   /* clock ticks each has run */
    int16_t pitch_offset[MASIF_TRACKS]; /* where SLD and PBN left the note: kept until the next note */
    int16_t porta_target[MASIF_TRACKS]; /* SLD's: where it slides to */
    uint16_t bend_up[MASIF_TRACKS];     /* PBN: clock ticks it bends up yet, then down (MASIF_BEND_ON: until the */
    uint16_t bend_down[MASIF_TRACKS];   /* next note) */
    uint8_t vib_phase[MASIF_TRACKS];
    int16_t ret_ramp[MASIF_TRACKS];     /* RET's volume ramp so far, on top of the note's volume */
    uint8_t rep_fx[MASIF_TRACKS][MASIF_FX_COLS];    /* REP: the column's last command, its value as it stands, */
    uint8_t rep_val[MASIF_TRACKS][MASIF_FX_COLS];
    int8_t rep_inc[MASIF_TRACKS][MASIF_FX_COLS];    /* what a row adds (0: no REP runs), */
    uint8_t rep_to[MASIF_TRACKS][MASIF_FX_COLS];    /* RTO's value, if rep_to_on */
    uint8_t rep_to_on[MASIF_TRACKS][MASIF_FX_COLS];
    uint8_t ret_count[MASIF_TRACKS];    /* clock ticks since RET's last hit (it runs on over rows that repeat it) */
    /* the track's table: which, where, how fast, and what its rows have set for the sounding note */
    uint8_t table_on[MASIF_TRACKS];
    uint8_t table_id[MASIF_TRACKS];
    uint8_t table_row[MASIF_TRACKS];
    uint8_t table_tic[MASIF_TRACKS];    /* tracker ticks a row; 0: it stands */
    uint8_t table_wait[MASIF_TRACKS];   /* clock ticks until its next row */
    int8_t table_transpose[MASIF_TRACKS];
    uint8_t table_volume[MASIF_TRACKS]; /* 0..127 */
    uint32_t rng; /* PROBABILITY's dice */
} masif_runtime_t;

void masif_edit_queue_init(masif_edit_queue_t *q);
int masif_post_edit(masif_edit_queue_t *q, uint8_t kind, uint8_t a, uint8_t b, const uint8_t data[7]);
int masif_post_cell_edit(masif_edit_queue_t *q, uint8_t phrase, uint8_t row, const masif_cell_t *cell);
void masif_runtime_init(masif_runtime_t *rt, masif_song_t *song, uint16_t bpm10);
void masif_runtime_set_bpm(masif_runtime_t *rt, uint16_t bpm10);
/* Shared-clock entry points: the host owns the transport and its tick clock (96 PPQ).
 * restart: the song from its first row with all per-track play state cleared; the
 * song data and bpm10 are kept.
 * ticks: apply pending edits, then run `ticks` clock ticks (0 is valid: edits only).
 * Returns the number of rows emitted. tick_phase_q16 and bpm10 are not used here. */
void masif_runtime_restart(masif_runtime_t *rt);
/* The same for what the screens play: the song from a row (track ignored), or one chain / one phrase on one track,
 * round and round. what: masif_play_what_t. */
void masif_runtime_start(masif_runtime_t *rt, uint8_t what, uint8_t track, uint8_t index);
uint8_t masif_runtime_ticks(masif_runtime_t *rt, uint8_t ticks, masif_edit_queue_t *edits,
                            masif_event_sink_fn sink, void *sink_context);
/* Free-running clock (portable hosts without a transport): advance by one audio block
 * at bpm10. Returns number of tracker rows emitted (0 or more).
 * sample_rate=44100 and block_frames=32 match the supplied zp12 audio engine. */
uint8_t masif_runtime_audio_block(masif_runtime_t *rt, uint32_t sample_rate,
                                  uint8_t block_frames, masif_edit_queue_t *edits,
                                  masif_event_sink_fn sink, void *sink_context);
#endif
