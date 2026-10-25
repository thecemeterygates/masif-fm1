/* Portable tracker clock, instrument resolution, and tick-level effect runner. */
#include "masif_runtime.h"
static void masif_runtime_zero(void *d, int c, unsigned n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
}

#define MASIF_PPQ 96u
#define MASIF_Q16 65536u

/* effects that add to the pitch only while they run: the row after them is back at the note's own (slid) pitch */
static int is_wobble_fx(uint8_t fx) { return fx == MASIF_FX_ARPEGGIO || fx == MASIF_FX_VIBRATO; }

/* PROBABILITY's dice: free-running, not put back by a restart (a PLAY is not the same roll again) */
static uint32_t masif_rng(masif_runtime_t *rt)
{
    rt->rng = rt->rng * 1664525u + 1013904223u;
    return rt->rng >> 16;
}

void masif_edit_queue_init(masif_edit_queue_t *q)
{
    if (q) masif_runtime_zero(q, 0, sizeof(*q));
}

int masif_post_edit(masif_edit_queue_t *q, uint8_t kind, uint8_t a, uint8_t b, const uint8_t data[7])
{
    uint8_t w, next, i;
    if (!q || !data || kind > MASIF_EDIT_GROOVE) return 0;
    w = q->write_index;
    next = (uint8_t)((w + 1u) % MASIF_EDIT_CAPACITY);
    if (next == q->read_index) {
        if (q->dropped != 0xFFFFu) q->dropped++;
        return 0;
    }
    q->items[w].kind = kind; q->items[w].a = a; q->items[w].b = b;
    for (i = 0; i < 7u; i++) q->items[w].data[i] = data[i];
    __asm__ volatile("" ::: "memory");
    q->write_index = next;
    return 1;
}

int masif_post_cell_edit(masif_edit_queue_t *q, uint8_t phrase, uint8_t row, const masif_cell_t *cell)
{
    uint8_t d[7];
    if (!cell || phrase >= MASIF_PHRASES || row >= MASIF_ROWS) return 0;
    d[0] = cell->note; d[1] = cell->instrument; d[2] = cell->volume; d[3] = cell->fx; d[4] = cell->param;
    d[5] = cell->fx2; d[6] = cell->param2;
    return masif_post_edit(q, MASIF_EDIT_CELL, phrase, row, d);
}

/* one edit into the song: through the same setters as everything else (what cannot be is dropped) */
static void apply_edit(masif_song_t *song, const masif_cell_edit_t *e)
{
    switch (e->kind) {
    case MASIF_EDIT_CELL: {
        masif_cell_t c;
        c.note = e->data[0]; c.instrument = e->data[1]; c.volume = e->data[2]; c.fx = e->data[3]; c.param = e->data[4];
        c.fx2 = e->data[5]; c.param2 = e->data[6];
        (void)masif_set_cell(song, e->a, e->b, &c);
        break;
    }
    case MASIF_EDIT_CHAIN:
        (void)masif_set_chain_step(song, e->a, e->b, e->data[0], (int8_t)e->data[1]);
        break;
    case MASIF_EDIT_SONG:
        (void)masif_set_song_cell(song, e->a, e->b, e->data[0]);
        break;
    case MASIF_EDIT_TABLE: {
        masif_table_cell_t c;
        c.transpose = (int8_t)e->data[0]; c.volume = e->data[1]; c.fx = e->data[2]; c.param = e->data[3];
        c.fx2 = e->data[4]; c.param2 = e->data[5];
        (void)masif_set_table_cell(song, e->a, e->b, c);
        break;
    }
    case MASIF_EDIT_INSTRUMENT:
        if (e->a < MASIF_INSTRUMENTS && e->b < sizeof(masif_instrument_t)) {
            masif_instrument_t in = song->instrument[e->a];
            ((uint8_t *)&in)[e->b] = e->data[0];
            (void)masif_set_instrument(song, e->a, &in);
        }
        break;
    case MASIF_EDIT_MIX: {
        int8_t v = (int8_t)e->data[0];
        if (e->a < MASIF_TRACKS) { if (e->data[0] <= 127u) song->track_level[e->a] = e->data[0]; }
        else if (e->a < MASIF_TRACKS + 3u) { if (v >= -64 && v <= 64) song->master_eq[e->a - MASIF_TRACKS] = v; }
        else if (e->a == MASIF_TRACKS + 3u) { if (!e->data[0] || (e->data[0] >= 40u && e->data[0] <= 240u)) song->tempo = e->data[0]; }
        else if (e->a == MASIF_TRACKS + 4u) { if (e->data[0] < 12u) song->key = e->data[0]; }
        else if (e->a == MASIF_TRACKS + 5u) { if (e->data[0] < MASIF_SCALES) song->scale = e->data[0]; }
        else if (e->a == MASIF_TRACKS + 6u) { if (e->data[0] < MASIF_GROOVES) song->project_groove = e->data[0]; }
        break;
    }
    case MASIF_EDIT_GROOVE:
        (void)masif_set_groove(song, e->a, e->b, e->data[0]);
        break;
    default:
        break;
    }
}

static void apply_pending_edits(masif_runtime_t *rt, masif_edit_queue_t *q)
{
    if (!rt || !rt->tracker.song || !q) return;
    while (q->read_index != q->write_index) {
        uint8_t r = q->read_index;
        masif_cell_edit_t edit;
        __asm__ volatile("" ::: "memory");
        edit = q->items[r];
        q->read_index = (uint8_t)((r + 1u) % MASIF_EDIT_CAPACITY);
        apply_edit(rt->tracker.song, &edit);
    }
}

static void reset_play_state(masif_runtime_t *rt)
{
    uint8_t t;
    rt->tick_phase_q16 = 0u;
    rt->tick_in_row = 0u;
    for (t = 0; t < MASIF_TRACKS; ++t) {
        rt->active_instrument[t] = 0u;
        rt->active_volume[t] = 100u;
        rt->active_note[t] = MASIF_NOTE_NONE;
        rt->effect[t][0] = rt->effect[t][1] = MASIF_FX_NONE; rt->effect_param[t][0] = rt->effect_param[t][1] = 0u;
        rt->effect_tick[t][0] = rt->effect_tick[t][1] = 0u;
        rt->bend_up[t] = rt->bend_down[t] = 0u;
        rt->pitch_offset[t] = 0; rt->porta_target[t] = 0; rt->vib_phase[t] = 0u; rt->ret_ramp[t] = 0; rt->ret_count[t] = 0u;
        rt->table_on[t] = 0u; rt->table_transpose[t] = 0; rt->table_volume[t] = 127u;
        rt->rep_fx[t][0] = rt->rep_fx[t][1] = MASIF_FX_NONE; rt->rep_inc[t][0] = rt->rep_inc[t][1] = 0;
        rt->rep_to_on[t][0] = rt->rep_to_on[t][1] = 0u;
        rt->row_tick[t] = 0u; rt->row_len[t] = 0u; rt->row_shift[t] = 0; rt->groove_pos[t] = 0u;
        rt->groove[t] = rt->tracker.song ? rt->tracker.song->project_groove % MASIF_GROOVES : 0u;
    }
    rt->ticks_per_row = MASIF_ROW_TICKS * MASIF_TICK;
}

void masif_runtime_init(masif_runtime_t *rt, masif_song_t *song, uint16_t bpm10)
{
    if (!rt) return;
    masif_runtime_zero(rt, 0, sizeof(*rt));
    masif_tracker_init(&rt->tracker, song);
    rt->bpm10 = bpm10 ? bpm10 : 1200u;
    rt->rng = 0x2545F491u;
    reset_play_state(rt);
}

void masif_runtime_start(masif_runtime_t *rt, uint8_t what, uint8_t track, uint8_t index)
{
    if (!rt) return;
    reset_play_state(rt);
    if (what == MASIF_PLAY_CHAIN) masif_tracker_start_chain(&rt->tracker, track, index);
    else if (what == MASIF_PLAY_PHRASE) masif_tracker_start_phrase(&rt->tracker, track, index);
    else masif_tracker_start_song(&rt->tracker, index);
}

void masif_runtime_restart(masif_runtime_t *rt)
{
    masif_runtime_start(rt, MASIF_PLAY_SONG, 0u, 0u);
}

void masif_runtime_set_bpm(masif_runtime_t *rt, uint16_t bpm10)
{
    if (rt && bpm10 >= 200u && bpm10 <= 3000u) rt->bpm10 = bpm10;
}

/* the sounding note's pitch before an arpeggio or a vibrato: where the slides left it + the table row's transpose */
static int32_t note_pitch(const masif_runtime_t *rt, uint8_t t)
{
    return rt->pitch_offset[t] + (int32_t)rt->table_transpose[t] * MASIF_PITCH_SEMI;
}

/* its volume: the cell's x the instrument's x the table's, and RET's ramp on top */
static uint8_t note_velocity(const masif_runtime_t *rt, uint8_t t)
{
    const masif_song_t *song = rt->tracker.song;
    int32_t v = rt->active_volume[t];
    if (song) v = v * song->instrument[rt->active_instrument[t]].volume / 127;
    v = v * rt->table_volume[t] / 127 + rt->ret_ramp[t];
    return (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
}

static void emit(masif_runtime_t *rt, uint8_t t, uint8_t type, int32_t pitch, masif_event_sink_fn sink, void *ctx)
{
    masif_audio_event_t e;
    masif_runtime_zero(&e, 0, sizeof(e));
    e.type = type; e.track = t; e.instrument = rt->active_instrument[t]; e.note = rt->active_note[t];
    e.velocity = note_velocity(rt, t);
    e.fx = rt->effect[t][0]; e.param = rt->effect_param[t][0]; e.fx2 = rt->effect[t][1]; e.param2 = rt->effect_param[t][1];
    pitch = pitch > 32767 ? 32767 : pitch < -32768 ? -32768 : pitch;
    e.pitch_offset = (int16_t)pitch;
    if (sink) sink(ctx, &e);
}
static void emit_simple(masif_runtime_t *rt, uint8_t t, uint8_t type, masif_event_sink_fn sink, void *ctx)
{
    emit(rt, t, type, note_pitch(rt, t), sink, ctx);
}

/* A table from its first row, at the next table step (this tick if the note has just been hit). */
static void table_start(masif_runtime_t *rt, uint8_t t, uint8_t table)
{
    if (table >= MASIF_TABLES) return;
    rt->table_id[t] = table; rt->table_row[t] = 0u; rt->table_wait[t] = 0u; rt->table_on[t] = 1u;
}

/* A note is hit: at its own pitch and volume, its instrument's table from the top. */
static void emit_note(masif_runtime_t *rt, uint8_t t, uint8_t note, masif_event_sink_fn sink, void *ctx)
{
    const masif_song_t *song = rt->tracker.song;
    uint8_t inst = rt->active_instrument[t];
    if (!song || inst >= MASIF_INSTRUMENTS) return;
    rt->pitch_offset[t] = 0; rt->porta_target[t] = 0; rt->vib_phase[t] = 0u; rt->ret_ramp[t] = 0;
    rt->bend_up[t] = rt->bend_down[t] = 0u;
    rt->table_transpose[t] = 0; rt->table_volume[t] = 127u;
    rt->table_tic[t] = song->instrument[inst].table_tic;
    rt->table_on[t] = 0u;
    if (rt->table_tic[t]) table_start(rt, t, inst);
    rt->active_note[t] = note;
    emit(rt, t, MASIF_EVENT_NOTE_ON, 0, sink, ctx);
}

static void emit_effect(masif_runtime_t *rt, uint8_t t, uint8_t fx, uint8_t param, masif_event_sink_fn sink, void *ctx)
{
    masif_audio_event_t e;
    masif_runtime_zero(&e, 0, sizeof(e)); e.type = MASIF_EVENT_EFFECT; e.track = t;
    e.instrument = rt->active_instrument[t]; e.note = rt->active_note[t]; e.velocity = note_velocity(rt, t);
    e.fx = fx; e.param = param;
    if (sink) sink(ctx, &e);
}

/* The effects that only set something about the track's table; 1 if fx was one of them. */
static int table_command(masif_runtime_t *rt, uint8_t t, uint8_t fx, uint8_t param)
{
    if (fx == MASIF_FX_SET_TABLE) table_start(rt, t, param);
    else if (fx == MASIF_FX_TABLE_HOP) { rt->table_row[t] = param & (MASIF_TABLE_ROWS - 1u); rt->table_wait[t] = 0u; }
    else return 0;
    return 1;
}

/* RET xy hits the sounding note again; x is what each hit adds to its volume (1-7 softer, 9-F louder) */
static void ret_hit(masif_runtime_t *rt, uint8_t t, uint8_t param, masif_event_sink_fn sink, void *ctx)
{
    int32_t x = param >> 4, ramp = rt->ret_ramp[t] + (x == 0 || x == 8 ? 0 : x < 8 ? -6 * x : 6 * (x - 8));
    rt->ret_ramp[t] = (int16_t)(ramp > 127 ? 127 : ramp < -127 ? -127 : ramp);
    emit_simple(rt, t, MASIF_EVENT_RETRIGGER, sink, ctx);
}

/* does the row have this effect, in either column: its parameter (the first column's if both have it) */
static int row_has(const masif_row_event_t *r, uint8_t fx, uint8_t *param)
{
    if (r->fx == fx) { *param = r->param; return 1; }
    if (r->fx2 == fx) { *param = r->param2; return 1; }
    return 0;
}

/* DED: the track is dead at once: the voice cut, and nothing of it left running */
static void kill_track(masif_runtime_t *rt, uint8_t t, masif_event_sink_fn sink, void *ctx)
{
    uint8_t k;
    rt->pitch_offset[t] = 0; rt->porta_target[t] = 0; rt->table_on[t] = 0u; rt->table_transpose[t] = 0;
    rt->bend_up[t] = rt->bend_down[t] = 0u;
    for (k = 0; k < MASIF_FX_COLS; ++k) { rt->effect[t][k] = MASIF_FX_NONE; rt->rep_inc[t][k] = 0; }
    emit_simple(rt, t, MASIF_EVENT_CUT, sink, ctx);
    rt->active_note[t] = MASIF_NOTE_NONE;
}

/* REP and RTO, a column of a phrase row: what the column plays on this row is left in *fx, *par.
 * A command is remembered; REP makes it play again on every row from its own on, its value moved; RTO is where
 * the value stops. (Not what moves the track through the song: HOP is the tracker's own.) */
static void rep_column(masif_runtime_t *rt, uint8_t t, uint8_t k, uint8_t *fx, uint8_t *par)
{
    if (*fx == MASIF_FX_REPEAT) {
        rt->rep_inc[t][k] = (int8_t)*par;
    } else if (*fx == MASIF_FX_REPEAT_TO) {
        rt->rep_to[t][k] = *par; rt->rep_to_on[t][k] = 1u;
    } else if (*fx != MASIF_FX_NONE) {
        rt->rep_fx[t][k] = *fx == MASIF_FX_BREAK ? MASIF_FX_NONE : *fx;
        rt->rep_val[t][k] = *par; rt->rep_inc[t][k] = 0; rt->rep_to_on[t][k] = 0u;
        return;
    }
    if (*fx != MASIF_FX_NONE) { *fx = MASIF_FX_NONE; *par = 0u; }   /* (REP and RTO themselves play nothing) */
    if (rt->rep_inc[t][k] && rt->rep_fx[t][k] != MASIF_FX_NONE) {
        int32_t v = (int32_t)rt->rep_val[t][k] + rt->rep_inc[t][k], to = rt->rep_to[t][k];
        if (rt->rep_to_on[t][k] && (rt->rep_inc[t][k] > 0 ? v > to : v < to)) v = to;
        rt->rep_val[t][k] = (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v);
        *fx = rt->rep_fx[t][k]; *par = rt->rep_val[t][k];
    }
}

/* RNL in the second column: the first column's value, digit by digit */
static uint8_t rnl_value(masif_runtime_t *rt, uint8_t v, uint8_t xy)
{
    uint32_t hi = ((v >> 4) + masif_rng(rt) % ((xy >> 4) + 1u)) & 15u, lo = ((v & 15u) + masif_rng(rt) % ((xy & 15u) + 1u)) & 15u;
    return (uint8_t)(hi << 4 | lo);
}

/* the groove's next step for the track: its tracker ticks (0: pass the row over). An empty step: from the first
 * again; a groove with nothing in it: 6 */
static uint8_t groove_step(masif_runtime_t *rt, uint8_t t)
{
    const uint8_t *g;
    uint8_t v;
    if (!rt->tracker.song) return 6u;
    g = rt->tracker.song->groove[rt->groove[t] % MASIF_GROOVES];
    if (rt->groove_pos[t] >= MASIF_GROOVE_STEPS || g[rt->groove_pos[t]] == 0xFFu) rt->groove_pos[t] = 0u;
    v = g[rt->groove_pos[t]];
    if (v == 0xFFu) return 6u;
    rt->groove_pos[t]++;
    return v > MASIF_GROOVE_MAX ? MASIF_GROOVE_MAX : v;
}

/* MTT on the row the track plays next: by how many clock ticks its begin moves (+ late, - early) */
static int32_t next_shift(const masif_runtime_t *rt, uint8_t t)
{
    masif_tracker_t peek = rt->tracker;
    masif_row_event_t ev;
    masif_tick_track(&peek, t, &ev);
    if (ev.fx == MASIF_FX_MICROTIME) return (int8_t)ev.param;
    if (ev.fx2 == MASIF_FX_MICROTIME) return (int8_t)ev.param2;
    return 0;
}

/* A track's next row: what it plays, and how long it lasts. */
/* PBN xy: up for x rows, then down for y (F: no end) */
static void bend_start(masif_runtime_t *rt, uint8_t t, uint8_t p)
{
    if (rt->active_note[t] == MASIF_NOTE_NONE) return;
    rt->bend_up[t] = (uint16_t)((p >> 4) == 15u ? MASIF_BEND_ON : (p >> 4) * 24u);
    rt->bend_down[t] = (uint16_t)((p & 15u) == 15u ? MASIF_BEND_ON : (p & 15u) * 24u);
}

static void emit_row(masif_runtime_t *rt, uint8_t t, masif_event_sink_fn sink, void *ctx)
{
    masif_row_event_t row[MASIF_TRACKS];
    uint8_t k, p, ticks = 6u, pass;
    int32_t len, sh;
    /* the row's length: its groove's step: a step of 00 passes a row of the phrase over */
    for (pass = 0; pass <= MASIF_GROOVE_STEPS && !(ticks = groove_step(rt, t)); pass++) masif_tick_track(&rt->tracker, t, &row[t]);
    if (!ticks) ticks = 6u;                                 /* (a groove of nothing but 00) */
    masif_tick_track(&rt->tracker, t, &row[t]);
    for (k = 0; k < MASIF_FX_COLS; ++k) {                   /* GRV: from this row on, the groove from its first step */
        uint8_t f = k ? row[t].fx2 : row[t].fx, g = (uint8_t)((k ? row[t].param2 : row[t].param) % MASIF_GROOVES);
        if (f == MASIF_FX_GROOVE) { rt->groove[t] = g; rt->groove_pos[t] = 0u; }
        else continue;
        if (!(ticks = groove_step(rt, t))) ticks = 6u;
    }
    /* MTT: this row began row_shift late (or early); the next one's MTT ends it late or early: at the most so
     * that a tick of this row is left, and late by no more than this row's own length less a tick (the next
     * row's is its groove's to say: taken as the same) */
    len = (int32_t)ticks * MASIF_TICK;
    sh = next_shift(rt, t);
    if (sh > len - 1) sh = len - 1;
    if (sh < 1 - (len - rt->row_shift[t])) sh = 1 - (len - rt->row_shift[t]);
    len = len - rt->row_shift[t] + sh;
    rt->row_len[t] = (uint8_t)(len < 1 ? 1 : len > 255 ? 255 : len);
    rt->row_shift[t] = (int8_t)sh;
    rt->row_tick[t] = 0u;
    {
        uint8_t note = row[t].note;
        uint8_t fx[MASIF_FX_COLS], par[MASIF_FX_COLS];
        const uint8_t was[MASIF_FX_COLS] = {rt->effect[t][0], rt->effect[t][1]};
        const uint8_t was_par[MASIF_FX_COLS] = {rt->effect_param[t][0], rt->effect_param[t][1]};
        rep_column(rt, t, 0, &row[t].fx, &row[t].param);
        rep_column(rt, t, 1, &row[t].fx2, &row[t].param2);
        if (row[t].fx2 == MASIF_FX_RANDOM_LEFT && row[t].fx != MASIF_FX_NONE)
            row[t].param = rnl_value(rt, row[t].param, row[t].param2);
        if (row[t].fx == MASIF_FX_RANDOM_LEFT) {            /* the first column's: the note and the instrument */
            uint32_t n = note + masif_rng(rt) % ((row[t].param >> 4) + 1u), i = row[t].instrument + masif_rng(rt) % ((row[t].param & 15u) + 1u);
            if (note >= 1u && note <= 120u) note = (uint8_t)(n > 120u ? 120u : n);
            if (row[t].instrument < MASIF_INSTRUMENTS) row[t].instrument = (uint8_t)(i >= MASIF_INSTRUMENTS ? MASIF_INSTRUMENTS - 1u : i);
        }
        fx[0] = row[t].fx; fx[1] = row[t].fx2; par[0] = row[t].param; par[1] = row[t].param2;
        /* PROBABILITY (param = percent): one roll for the row's note. Lost: the row plays nothing of its
         * note, instrument or volume column (the sounding note goes on) */
        if (row_has(&row[t], MASIF_FX_PROBABILITY, &p) && note >= 1u && note <= 120u && p < 100u && masif_rng(rt) % 100u >= p) {
            note = MASIF_NOTE_NONE; row[t].instrument = 0xFFu; row[t].volume = 0xFFu;
        }
        /* (an instrument that is not there is no change; a volume past the most is the most) */
        if (row[t].instrument < MASIF_INSTRUMENTS) rt->active_instrument[t] = row[t].instrument;
        /* the project's key and scale: the note as written, held to them (not a sliced or a free instrument's) */
        if (note >= 1u && note <= 120u && rt->tracker.song && rt->tracker.song->scale) {
            const masif_instrument_t *in = &rt->tracker.song->instrument[rt->active_instrument[t]];
            if (in->slices <= 1u && !(in->flags & MASIF_INST_FREE)) note = masif_scale_note(rt->tracker.song, note);
        }
        if (row[t].volume != 0xFFu) rt->active_volume[t] = row[t].volume > 127u ? 127u : row[t].volume;
        for (k = 0; k < MASIF_FX_COLS; ++k) { rt->effect[t][k] = fx[k]; rt->effect_param[t][k] = par[k]; rt->effect_tick[t][k] = 0; }
        /* SLD with a note: the sounding note is not hit again, it slides to the new one (up or down) */
        if (row_has(&row[t], MASIF_FX_SLIDE, &p) && note >= 1u && note <= 120u && rt->active_note[t] != MASIF_NOTE_NONE) {
            int32_t to = ((int32_t)note - (int32_t)rt->active_note[t]) * MASIF_PITCH_SEMI;
            rt->porta_target[t] = (int16_t)(to > MASIF_PITCH_MAX ? MASIF_PITCH_MAX : to < -MASIF_PITCH_MAX ? -MASIF_PITCH_MAX : to);
            note = MASIF_NOTE_NONE;
        }
        if (note == MASIF_NOTE_NONE && rt->active_note[t] != MASIF_NOTE_NONE && (is_wobble_fx(was[0]) || is_wobble_fx(was[1])))
            emit_simple(rt, t, MASIF_EVENT_PITCH, sink, ctx);   /* (also arpeggio after arpeggio: its first note again) */
        if (note == MASIF_NOTE_NONE && row[t].volume != 0xFFu && rt->active_note[t] != MASIF_NOTE_NONE)
            emit_simple(rt, t, MASIF_EVENT_VOLUME, sink, ctx);  /* the volume column alone: the sounding note's */
        if (note == MASIF_NOTE_CUT) { emit_simple(rt, t, MASIF_EVENT_CUT, sink, ctx); rt->active_note[t] = MASIF_NOTE_NONE; }
        else if (note == MASIF_NOTE_OFF) { emit_simple(rt, t, MASIF_EVENT_NOTE_OFF, sink, ctx); rt->active_note[t] = MASIF_NOTE_NONE; }
        else if (note != MASIF_NOTE_NONE) {
            emit_note(rt, t, note, sink, ctx);
        } else if (rt->active_note[t] != MASIF_NOTE_NONE) {
            /* RET on a row without a note. A row that repeats the RET before it (in the same column): its count
             * runs on (8ths and longer, an even roll over rows); a RET that begins: a hit now */
            for (k = 0; k < MASIF_FX_COLS; ++k) {
                if (fx[k] != MASIF_FX_RETRIGGER) continue;
                if (was[k] == MASIF_FX_RETRIGGER && was_par[k] == par[k]) {
                    if ((par[k] & 15u) && ++rt->ret_count[t] >= (par[k] & 15u) * MASIF_TICK) { rt->ret_count[t] = 0u; ret_hit(rt, t, par[k], sink, ctx); }
                } else { rt->ret_count[t] = 0u; ret_hit(rt, t, par[k], sink, ctx); }
                break;
            }
        }
        if (note != MASIF_NOTE_NONE) rt->ret_count[t] = 0u;
        /* STP 00 and DED 00: at the row's start (after its note, if it has one: that is then let go or dead at once) */
        if (row_has(&row[t], MASIF_FX_STOP, &p) && !p && rt->active_note[t] != MASIF_NOTE_NONE) {
            emit_simple(rt, t, MASIF_EVENT_NOTE_OFF, sink, ctx); rt->active_note[t] = MASIF_NOTE_NONE;
        }
        if (row_has(&row[t], MASIF_FX_KILL, &p) && !p) { kill_track(rt, t, sink, ctx); return; }
        /* PBN xy: up x rows, then down y (F: no end), from the row's note or from where the pitch is */
        if (row_has(&row[t], MASIF_FX_PITCH_BEND, &p)) bend_start(rt, t, p);
        for (k = 0; k < MASIF_FX_COLS; ++k) {
            (void)table_command(rt, t, fx[k], par[k]);          /* (after the note: TBL in place of its instrument's table) */
            if (fx[k] != MASIF_FX_NONE) emit_effect(rt, t, fx[k], par[k], sink, ctx);
        }
    }
}

/* PBN, a clock tick of it: on every tick, the rows' first too, so x rows are x * 24 ticks (1/16 semitone each) */
static void bend_tick(masif_runtime_t *rt, uint8_t t, masif_event_sink_fn sink, void *ctx)
{
    int32_t to;
    uint16_t *left;
    if ((!rt->bend_up[t] && !rt->bend_down[t]) || rt->active_note[t] == MASIF_NOTE_NONE) return;
    to = rt->pitch_offset[t] + (rt->bend_up[t] ? 1 : -1);
    left = rt->bend_up[t] ? &rt->bend_up[t] : &rt->bend_down[t];
    if (*left != MASIF_BEND_ON) (*left)--;
    rt->pitch_offset[t] = (int16_t)(to > MASIF_PITCH_MAX ? MASIF_PITCH_MAX : to < -MASIF_PITCH_MAX ? -MASIF_PITCH_MAX : to);
    emit_simple(rt, t, MASIF_EVENT_PITCH, sink, ctx);
}

static void tick_effects(masif_runtime_t *rt, uint8_t t, masif_event_sink_fn sink, void *ctx)
{
    uint8_t k;
    {
        for (k = 0; k < MASIF_FX_COLS; ++k) {
            uint8_t fx = rt->effect[t][k], p = rt->effect_param[t][k];
            uint8_t now = (uint8_t)(rt->effect_tick[t][k] + 1u);    /* clock ticks since the effect's own (a row's: the tick in the row) */
            if (rt->effect_tick[t][k] < 254u) rt->effect_tick[t][k]++;
            /* (DED also where no note is held: a note that was let go may still ring) */
            if (fx == MASIF_FX_KILL && p > 0u && now == (uint32_t)p * MASIF_TICK) { kill_track(rt, t, sink, ctx); break; }
            if (rt->active_note[t] == MASIF_NOTE_NONE) continue;
            if (k && fx == rt->effect[t][0]) continue;          /* (the same effect in both columns: the first one's) */
            if (fx == MASIF_FX_STOP && p > 0u && now == (uint32_t)p * MASIF_TICK) {
                emit_simple(rt, t, MASIF_EVENT_NOTE_OFF, sink, ctx); rt->active_note[t] = MASIF_NOTE_NONE;
            } else if (fx == MASIF_FX_RETRIGGER && (p & 15u) && ++rt->ret_count[t] >= (p & 15u) * MASIF_TICK) {
                rt->ret_count[t] = 0u;
                ret_hit(rt, t, p, sink, ctx);
            } else if (fx == MASIF_FX_ARPEGGIO && now % MASIF_TICK == 0u) {
                /* the note, + the high nibble, + the low nibble, a tracker tick each: an event where it changes */
                uint8_t phase = (uint8_t)(now / MASIF_TICK % 3u);
                uint8_t semis = phase == 1u ? (p >> 4) : (phase == 2u ? (p & 0x0Fu) : 0u);
                emit(rt, t, MASIF_EVENT_PITCH, note_pitch(rt, t) + semis * MASIF_PITCH_SEMI, sink, ctx);
            } else if (fx == MASIF_FX_SLIDE && rt->pitch_offset[t] != rt->porta_target[t]) {
                int32_t d = rt->porta_target[t] - rt->pitch_offset[t], s = p ? p : 1u;
                rt->pitch_offset[t] = (int16_t)(rt->pitch_offset[t] + (d > s ? s : d < -s ? -s : d));
                emit_simple(rt, t, MASIF_EVENT_PITCH, sink, ctx);
            } else if (fx == MASIF_FX_VIBRATO) {
                /* param = speed, depth: a triangle of 256 phase steps (speed a clock tick), +-depth / 8 semitones */
                int32_t x, v;
                rt->vib_phase[t] = (uint8_t)(rt->vib_phase[t] + (p >> 4));
                x = rt->vib_phase[t];
                v = x < 64 ? x : x < 192 ? 128 - x : x - 256;     /* -64..64 */
                emit(rt, t, MASIF_EVENT_PITCH, note_pitch(rt, t) + (int32_t)(p & 0x0Fu) * v / 32, sink, ctx);
            }
        }
    }
}

/* The tracks' tables, every clock tick, after the row's notes and the effects: a table row when its time has come.
 * Its transpose and volume are the sounding note's from then on; its effect becomes the track's running effect. */
static void tick_tables(masif_runtime_t *rt, masif_event_sink_fn sink, void *ctx)
{
    uint8_t t;
    for (t = 0; t < MASIF_TRACKS; ++t) {
        const masif_table_cell_t *c;
        if (!rt->table_on[t] || !rt->tracker.song || rt->active_note[t] == MASIF_NOTE_NONE) continue;
        if (rt->table_wait[t]) { rt->table_wait[t]--; continue; }
        if (!rt->table_tic[t]) continue;            /* TIC 00: it stands on its row */
        c = &rt->tracker.song->table[rt->table_id[t]][rt->table_row[t]];
        rt->table_row[t] = (uint8_t)((rt->table_row[t] + 1u) % MASIF_TABLE_ROWS);
        rt->table_wait[t] = (uint8_t)(rt->table_tic[t] > 63u ? 251u : rt->table_tic[t] * MASIF_TICK - 1u);
        {
            int32_t tsp = c->transpose, vol = c->volume;
            if (c->fx == MASIF_FX_RANDOM_LEFT) {    /* RNL in a table's first column: its transpose and its volume */
                tsp += (int32_t)(masif_rng(rt) % ((c->param >> 4) + 1u));
                if (vol != 0xFF) vol = (vol > 127 ? 127 : vol) - (int32_t)(masif_rng(rt) % ((c->param & 15u) + 1u)) * 16;
                if (vol < 0) vol = 0;
            }
            tsp = tsp > 48 ? 48 : tsp < -48 ? -48 : tsp;
            if (tsp != rt->table_transpose[t]) {
                rt->table_transpose[t] = (int8_t)tsp;
                emit_simple(rt, t, MASIF_EVENT_PITCH, sink, ctx);
            }
            if (vol != 0xFF) {
                rt->table_volume[t] = (uint8_t)(vol > 127 ? 127 : vol);
                emit_simple(rt, t, MASIF_EVENT_VOLUME, sink, ctx);
            }
        }
        {
            const uint8_t fx[MASIF_FX_COLS] = {c->fx, c->fx2};
            const uint8_t par[MASIF_FX_COLS] = {(uint8_t)(c->fx2 == MASIF_FX_RANDOM_LEFT && c->fx ? rnl_value(rt, c->param, c->param2) : c->param), c->param2};
            uint8_t k;
            for (k = 0; k < MASIF_FX_COLS; ++k) {
                if (fx[k] == MASIF_FX_NONE) continue;
                if (fx[k] == MASIF_FX_TABLE_HOP) {  /* a loop: this row lasts as any other, then on at row x */
                    rt->table_row[t] = par[k] & (MASIF_TABLE_ROWS - 1u);
                } else if (!table_command(rt, t, fx[k], par[k])) {
                    rt->effect[t][k] = fx[k]; rt->effect_param[t][k] = par[k]; rt->effect_tick[t][k] = 0u;
                    if (fx[k] == MASIF_FX_SLIDE) rt->porta_target[t] = 0;   /* (no note to slide to in a table: back to the note) */
                    if (fx[k] == MASIF_FX_PITCH_BEND) bend_start(rt, t, par[k]);
                }
                emit_effect(rt, t, fx[k], par[k], sink, ctx);
            }
        }
    }
}

/* One clock tick: a new row on the row's first tick, effects on the others, then the tables. Returns rows emitted. */
static uint8_t run_tick(masif_runtime_t *rt, masif_event_sink_fn sink, void *ctx)
{
    uint8_t rows = 0u, t;
    for (t = 0; t < MASIF_TRACKS; ++t) {
        if (rt->row_tick[t] >= rt->row_len[t]) { bend_tick(rt, t, sink, ctx); emit_row(rt, t, sink, ctx); rows = 1u; }
        else { bend_tick(rt, t, sink, ctx); tick_effects(rt, t, sink, ctx); }
        rt->row_tick[t]++;
    }
    tick_tables(rt, sink, ctx);
    rt->tick_in_row = (uint8_t)(rt->row_tick[0] >= rt->row_len[0] ? 0u : rt->row_tick[0]);
    return rows;
}

/* Calculate (numerator / denominator) in unsigned Q16 without 64-bit math. */
static uint32_t masif_ratio_q16(uint32_t numerator, uint32_t denominator)
{
    uint32_t whole, rem, frac = 0u;
    unsigned i;

    if (!denominator) return 0u;
    whole = numerator / denominator;
    rem = numerator % denominator;

    for (i = 0u; i < 16u; ++i) {
        frac <<= 1;
        if (rem >= denominator - rem) {
            rem -= denominator - rem;
            frac |= 1u;
        } else {
            rem <<= 1;
        }
    }

    return (whole << 16) | frac;
}

uint8_t masif_runtime_audio_block(masif_runtime_t *rt, uint32_t sample_rate,
                                  uint8_t block_frames, masif_edit_queue_t *edits,
                                  masif_event_sink_fn sink, void *sink_context)
{
    uint32_t numerator;
    uint32_t denominator;
    uint32_t add;
    uint8_t ticks = 0u, rows = 0u;
    if (!rt || !sample_rate || !block_frames || !rt->bpm10 || !rt->ticks_per_row) return 0u;
    apply_pending_edits(rt, edits);
    /* One tracker tick is 1/96 quarter-note; BPM is stored as BPM x 10. */
    numerator = (uint32_t)rt->bpm10 * MASIF_PPQ * block_frames;
    denominator = 600u * sample_rate;
    add = masif_ratio_q16(numerator, denominator);
    rt->tick_phase_q16 += add;
    while (rt->tick_phase_q16 >= MASIF_Q16 && ticks < 8u) {
        rt->tick_phase_q16 -= MASIF_Q16;
        rows = (uint8_t)(rows + run_tick(rt, sink, sink_context));
        ticks++;
    }
    return rows;
}

uint8_t masif_runtime_ticks(masif_runtime_t *rt, uint8_t ticks, masif_edit_queue_t *edits,
                            masif_event_sink_fn sink, void *sink_context)
{
    uint8_t rows = 0u;
    if (!rt || !rt->ticks_per_row) return 0u;
    apply_pending_edits(rt, edits);
    while (ticks--) rows = (uint8_t)(rows + run_tick(rt, sink, sink_context));
    return rows;
}
