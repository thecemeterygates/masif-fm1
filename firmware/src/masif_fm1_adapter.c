/* Experimental adapter, included by zp12.c only with MASIF_ENABLE_TRACKER=1.
 * Runs inside the existing 32-frame audio block cadence; no UI is wired yet: the song is empty at power-on,
 * or the built-in demo (masif_demo.c) with MASIF_DEMO_SONG=1.
 * The tracker has no clock of its own: it runs on the sequencer's transport (sp_seq.c
 * sq.playing, sq_run, sq_ticks). Row r is the sequencer's tick r * 24 since PLAY. */
#include "masif_runtime.h"

static masif_song_t masif_song;
static masif_runtime_t masif_runtime;
static masif_edit_queue_t masif_edits;
static uint8_t masif_running;      /* following a PLAY (its count-in included) */
static uint8_t masif_run_seen;     /* sq_run of that PLAY */
static uint32_t masif_ticks_done;  /* of sq_ticks, already played */
#define MASIF_BLOCK_TICKS 8u       /* at the most a block (240 BPM is 0.28): bounds the ISR's work */

/* one voice of its own a track (sp_core.c: track t = voice SP_NCH + t, never a drum channel) */
typedef char masif_tracks_fit_voices[MASIF_TRACKS <= SP_NTRK ? 1 : -1];
typedef char masif_samples_are_waves[MASIF_SAMPLES == sizeof sp_wave / sizeof sp_wave[0] ? 1 : -1];

static int32_t masif_clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The tracks whose notes go through their instrument's filter and EQ: all four. (By the host's figures,
 * host-tests/bench_voices.c, four voices with both are below the load zp12 is known to carry on the chip; set
 * this lower if the device says otherwise: the tracks from it on then play their instruments plain.) */
#ifndef MASIF_FILTER_TRACKS
#define MASIF_FILTER_TRACKS MASIF_TRACKS
#endif

/* What PAN and SEND set for a track until the next PLAY, in place of what its instruments say. */
#define MASIF_OWN 0xFFu            /* not set: the instrument's own */
static uint8_t masif_track_pan[MASIF_TRACKS];       /* PAN's param: 0 left, 128 the middle, 255 right */
static uint8_t masif_track_pan_set[MASIF_TRACKS];
static uint8_t masif_track_send[MASIF_TRACKS][3];   /* 0..127 */
static uint8_t masif_track_level[MASIF_TRACKS];     /* the sounding note's volume */
static uint8_t masif_track_inst[MASIF_TRACKS];      /* ... and its instrument (MASIF_AUDITION: the sample pool's) */
/* the sample pool's instrument (masif_fm1_audition): a sample as it is, once, at its own pitch */
#define MASIF_AUDITION MASIF_INSTRUMENTS
static masif_instrument_t masif_audition;
#define MASIF_INST(i) ((i) < MASIF_INSTRUMENTS ? &masif_song.instrument[i] : &masif_audition)
static int16_t masif_track_cents[MASIF_TRACKS];     /* ... and its pitch from the note's own (the effects', the table's) */
/* the sounding note's modulation (its instrument's four slots), as masif_fm1_mods left it */
typedef struct {
    uint16_t phase;                 /* an LFO's place in its cycle */
    uint16_t count;                 /* an envelope's hold: blocks to go */
    uint16_t frac;                  /* a synced LFO: what its steps have left over (so it keeps the tempo's time) */
    int32_t env;                    /* an envelope's level, Q16 */
    int16_t value;                  /* RANDOM's and DRUNK's value, and what HOLD holds: -256..256 */
    uint8_t stage;                  /* an envelope: 0 attack, 1 hold and decay, 3 release; an LFO: 1 = it stands */
} masif_mod_state_t;
static masif_mod_state_t masif_mod[MASIF_TRACKS][MASIF_MODS];
static uint8_t masif_mod_trig[MASIF_TRACKS][MASIF_MODS];    /* 1 + the instrument whose TRIG slot the state is (0: none's) */
static uint32_t masif_mod_rng = 0x2F6E2B1u;
static int16_t masif_track_volf[MASIF_TRACKS];      /* what the slots make of the level (Q8, 256: as it is) */
static int8_t masif_track_panmod[MASIF_TRACKS];     /* ... and add to the pan */

/* the level and the pan of the track's voice: the note's volume and what its modulation makes of it; PAN's, or
 * its instrument's */
static void masif_fm1_voice_level(uint8_t t)
{
    int32_t pan = MASIF_INST(masif_track_inst[t])->pan;
    int32_t level = (int32_t)masif_track_level[t] * masif_track_volf[t] / 256;
    if (masif_track_pan_set[t])                     /* 0 .. 128 .. 255 -> -64 .. 0 .. 64 */
        pan = masif_track_pan[t] >= 128u ? ((int32_t)masif_track_pan[t] - 128) * 64 / 127
                                         : ((int32_t)masif_track_pan[t] - 128) / 2;
    sp_level_track(t, (uint32_t)masif_clamp_i32(level, 0, 127), masif_clamp_i32(pan + masif_track_panmod[t], -64, 64));
}

static void masif_fm1_voice_pitch(uint8_t t, int32_t pitch_offset)
{
    masif_track_cents[t] = (int16_t)masif_clamp_i32(pitch_offset * 100 / MASIF_PITCH_SEMI, -9600, 9600);
    sp_pitch_track_cents(t, masif_track_cents[t]);
}

/* ---- the modulation. Everything runs once a block (1378 a second). MASIF_MODULATION 0 leaves it out: the slots
 * are in the song but do nothing (tools/build.py with the factory kit, whose image has no room for it; as the
 * song store there). */
#ifndef MASIF_MODULATION
#define MASIF_MODULATION 1
#endif
#if MASIF_MODULATION
/* an envelope's time of 0..127 in blocks: 1 .. ~2.5 s, the short times fine */
static int32_t masif_mod_blocks(uint32_t p) { return 1 + (int32_t)(p * p * 3445u / 16129u); }
/* an LFO's step of its 16-bit phase a block: rate 0..127 = 0.08 .. 45 Hz */
static uint32_t masif_lfo_step(int32_t rate)
{
    uint32_t r = (uint32_t)masif_clamp_i32(rate, 0, 127);
    return 4u + r + r * r / 8u;
}
/* A synced LFO: its cycle is a length in the tempo. The lengths in clock ticks (96 a quarter), 8 bars .. a 64th
 * (the names: masif_ui.c MU_DIV_NAME); the step a block is the clock's ticks a block (Q16, as sp_seq.c counts
 * them) through the length: what the division leaves over is kept, so the cycle is as long as its bars */
static const uint16_t MASIF_LFO_DIV[MASIF_LFO_DIVS] = {3072, 1536, 768, 384, 192, 144, 128, 96, 72, 64, 48, 32, 24, 16, 12, 6};
static uint32_t masif_lfo_sync_step(const masif_mod_t *m, masif_mod_state_t *st, int32_t rate_add)
{
    uint32_t len = MASIF_LFO_DIV[masif_clamp_i32((int32_t)m->p[2] + rate_add / 8, 0, MASIF_LFO_DIVS - 1)];
    uint32_t inc = (uint32_t)sq.bpm10 * 76087u / 10000u + st->frac, step = inc / len;
    st->frac = (uint16_t)(inc - step * len);
    return step;
}
/* the shapes that are a function of the phase: -256..256 (RANDOM and DRUNK: 0, they have a value of their own) */
static int32_t masif_lfo_shape(uint32_t shape, uint32_t phase)
{
    int32_t x = (int32_t)((phase >> 8) & 255u);
    switch (shape % MASIF_LFO_SHAPES) {
    case MASIF_LFO_TRI: return x < 64 ? x * 4 : x < 192 ? (128 - x) * 4 : (x - 256) * 4;
    case MASIF_LFO_SIN: return x < 128 ? x * (128 - x) / 16 : -((x - 128) * (256 - x) / 16);
    case MASIF_LFO_RAMP_DOWN: return 256 - x * 2;
    case MASIF_LFO_RAMP_UP: return x * 2 - 256;
    case MASIF_LFO_EXP_DOWN: return (((255 - x) * (255 - x)) >> 7) - 252;
    case MASIF_LFO_EXP_UP: return ((x * x) >> 7) - 252;
    case MASIF_LFO_SQU_DOWN: return x < 128 ? 256 : -256;
    case MASIF_LFO_SQU_UP: return x < 128 ? -256 : 256;
    default: return 0;
    }
}
static int32_t masif_mod_random(int32_t range)       /* -range..range */
{
    masif_mod_rng = masif_mod_rng * 1664525u + 1013904223u;
    return (int32_t)((masif_mod_rng >> 16) % (uint32_t)(2 * range + 1)) - range;
}
/* a slot's LFO as it stands: its value now */
static int32_t masif_lfo_now(const masif_mod_t *m, const masif_mod_state_t *st)
{
    uint32_t shape = m->p[0] % MASIF_LFO_SHAPES;
    return shape >= MASIF_LFO_RANDOM ? st->value : masif_lfo_shape(shape, st->phase);
}

/* a note starts on track t: its instrument's slots from their beginning */
static void masif_fm1_mods_start(uint8_t t, const masif_instrument_t *in)
{
    uint32_t s;
    for (s = 0; s < MASIF_MODS; ++s) {
        const masif_mod_t *m = &in->mod[s];
        masif_mod_state_t *st = &masif_mod[t][s];
        if (m->type == MASIF_MOD_TRIG) {            /* not this note's to start: it goes on as its source left it */
            if (masif_mod_trig[t][s] != 1u + masif_track_inst[t]) { st->env = 0; st->stage = 1; st->count = 0; }
            masif_mod_trig[t][s] = (uint8_t)(1u + masif_track_inst[t]);
            continue;
        }
        masif_mod_trig[t][s] = 0;
        if (m->type == MASIF_MOD_LFO) {
            uint32_t trig = m->p[1] % MASIF_LFO_TRIGS;
            st->stage = 0;
            if (trig == MASIF_TRIG_RETRIG || trig == MASIF_TRIG_ONCE) {
                st->phase = 0;
                st->value = (int16_t)(m->p[0] % MASIF_LFO_SHAPES == MASIF_LFO_RANDOM ? masif_mod_random(256) : 0);
            } else if (trig == MASIF_TRIG_HOLD) {
                st->value = (int16_t)masif_lfo_now(m, st);
                st->stage = 1;
            }
        } else {                                    /* an envelope (attack 0: at the top at once) */
            st->env = m->p[0] ? 0 : 65536;
            st->stage = m->p[0] ? 0u : 1u;
            st->count = (uint16_t)(m->type == MASIF_MOD_AHD && m->p[1] ? masif_mod_blocks(m->p[1]) : 0);
        }
    }
    masif_track_volf[t] = 256;
    masif_track_panmod[t] = 0;
}
/* a note of `instrument` began on track t: the TRIG slots that listen to it (or to its track) begin, on every
 * track, in the instrument that track last played */
static void masif_fm1_mods_trigger(uint8_t t, uint8_t instrument)
{
    uint32_t k, s;
    for (k = 0; k < MASIF_TRACKS; ++k)
        for (s = 0; s < MASIF_MODS; ++s) {
            const masif_mod_t *m = &MASIF_INST(masif_track_inst[k])->mod[s];
            masif_mod_state_t *st = &masif_mod[k][s];
            if (m->type != MASIF_MOD_TRIG || (m->p[3] < 0x80u ? m->p[3] != instrument : m->p[3] != 0x80u + t)) continue;
            st->stage = m->p[0] ? 0u : 1u;          /* (from where it is: a hit inside the last one's decay has no step) */
            if (!m->p[0]) st->env = 65536;
            st->count = (uint16_t)(m->p[1] ? masif_mod_blocks(m->p[1]) : 0);
            masif_mod_trig[k][s] = (uint8_t)(1u + masif_track_inst[k]);
        }
}
/* NOTE OFF: the ADSR slots go into their release */
static void masif_fm1_mods_release(uint8_t t)
{
    uint32_t s;
    const masif_instrument_t *in = MASIF_INST(masif_track_inst[t]);
    for (s = 0; s < MASIF_MODS; ++s)
        if (in->mod[s].type == MASIF_MOD_ADSR) {
            masif_mod[t][s].stage = 3;
            if (!in->mod[s].p[3]) masif_mod[t][s].env = 0;
        }
}

/* one slot a block on: what it gives now (an envelope 0..256, an LFO -256..256), then its next step */
static int32_t masif_mod_run(const masif_mod_t *m, masif_mod_state_t *st, int32_t rate_add, int sounding)
{
    int32_t v;
    if (m->type == MASIF_MOD_LFO) {
        uint32_t shape = m->p[0] % MASIF_LFO_SHAPES, trig = m->p[1] % MASIF_LFO_TRIGS;
        uint32_t step = m->p[3] ? masif_lfo_sync_step(m, st, rate_add) : masif_lfo_step((int32_t)m->p[2] + rate_add), was = st->phase;
        if (trig == MASIF_TRIG_HOLD && st->stage) {  /* (held: the cycle runs on underneath) */
            st->phase = (uint16_t)(was + step);
            return st->value;
        }
        v = masif_lfo_now(m, st);
        if (trig == MASIF_TRIG_ONCE && st->stage) return v;
        st->phase = (uint16_t)(was + step);
        if (shape == MASIF_LFO_DRUNK)
            st->value = (int16_t)masif_clamp_i32(st->value + masif_mod_random(1 + (int32_t)step / 32), -256, 256);
        if (st->phase < was) {                      /* a cycle done */
            if (shape == MASIF_LFO_RANDOM) st->value = (int16_t)masif_mod_random(256);
            if (trig == MASIF_TRIG_ONCE) { st->phase = 0xFFFFu; st->stage = 1; }
        }
        return v;
    }
    v = st->env >> 8;
    if (!sounding && m->type != MASIF_MOD_TRIG) return v;        /* (a TRIG's time is its source's, not this note's) */
    if (st->stage == 0u) {
        st->env += 65536 / masif_mod_blocks(m->p[0]);
        if (st->env >= 65536) { st->env = 65536; st->stage = 1; }
    } else if (st->stage == 3u) {
        st->env -= 65536 / masif_mod_blocks(m->p[3]);
        if (st->env < 0) st->env = 0;
    } else if (m->type != MASIF_MOD_ADSR) {         /* AHD, TRIG */
        if (st->count) st->count--;
        else {
            st->env -= 65536 / masif_mod_blocks(m->p[2]);
            if (st->env < 0) st->env = 0;
        }
    } else {                                        /* ADSR: down to the sustain level, and there it holds */
        int32_t sus = (int32_t)m->p[2] * 65536 / 127;
        st->env -= 65536 / masif_mod_blocks(m->p[1]);
        if (st->env < sus) st->env = sus;
    }
    return v;
}

/* Every track's slots, once a block: each gives its value to its destination of the sounding note. The LFOs run on
 * while a track is silent (FREE and HOLD count on that). */
static void masif_fm1_mods(void)
{
    uint8_t t;
    for (t = 0; t < MASIF_TRACKS; ++t) {
        const masif_instrument_t *in = MASIF_INST(masif_track_inst[t]);
        int sounding = sp_ch[SP_NCH + t].on;
        int32_t volf = 256, add[MASIF_DESTS], next_amt = 256, next_rate = 0;
        uint32_t s, used = 0, b;
        for (s = 0; s < MASIF_DESTS; ++s) add[s] = 0;
        for (s = 0; s < MASIF_MODS; ++s) {
            const masif_mod_t *m = &in->mod[s];
            int32_t amt_f = next_amt, rate_add = next_rate, v, u, amt, a;
            next_amt = 256; next_rate = 0;
            if (!m->type) continue;
            v = masif_mod_run(m, &masif_mod[t][s], rate_add, sounding);
            if (!m->dest || !m->amount || !sounding) continue;
            amt = (int32_t)m->amount * amt_f / 256;
            a = (amt < 0 ? -amt : amt) * 4;         /* 0..256 */
            u = m->type == MASIF_MOD_LFO ? (v + 256) / 2 : v;                /* 0..256 */
            if (amt < 0) u = 256 - u;
            used |= 1u << m->dest;
            switch (m->dest) {
            case MASIF_DEST_VOLUME: volf = volf * (256 - a + a * u / 256) / 256; break;
            case MASIF_DEST_MOD_AMT: next_amt = 256 - a + a * u / 256; break;
            case MASIF_DEST_PITCH: add[MASIF_DEST_PITCH] += v * amt * (amt < 0 ? -amt : amt) / 1024 * 2400 / 1024; break;
            case MASIF_DEST_PAN: add[MASIF_DEST_PAN] += v * amt / 256; break;
            case MASIF_DEST_MOD_RATE: next_rate = v * amt * 127 / (256 * 64); break;
            default: add[m->dest] += v * amt * 127 / (256 * 64); break;
            }
        }
        if (!sounding) continue;
        if (used & (1u << MASIF_DEST_VOLUME | 1u << MASIF_DEST_PAN)) {
            masif_track_volf[t] = (int16_t)volf;
            masif_track_panmod[t] = (int8_t)masif_clamp_i32(add[MASIF_DEST_PAN], -127, 127);
            masif_fm1_voice_level(t);
        }
        if (used & (1u << MASIF_DEST_PITCH)) sp_pitch_track_cents(t, masif_track_cents[t] + add[MASIF_DEST_PITCH]);
        if (in->filter && t < MASIF_FILTER_TRACKS) {
            if (used & (1u << MASIF_DEST_CUTOFF)) sp_cutoff_track(t, (int32_t)in->cutoff + add[MASIF_DEST_CUTOFF]);
            if (used & (1u << MASIF_DEST_RES)) sp_res_track(t, (int32_t)in->resonance + add[MASIF_DEST_RES]);
        }
        for (b = 0; b < 3u; ++b)
            if (used & (1u << (MASIF_DEST_CHORUS + b))) {
                int32_t own = masif_track_send[t][b] != MASIF_OWN ? masif_track_send[t][b] : in->send[b];
                sp_send_track(t, b, (uint32_t)masif_clamp_i32(own + add[MASIF_DEST_CHORUS + b], 0, 127));
            }
    }
}
#else
static void masif_fm1_mods_start(uint8_t t, const masif_instrument_t *in) { (void)in; masif_track_volf[t] = 256; masif_track_panmod[t] = 0; }
static void masif_fm1_mods_release(uint8_t t) { (void)t; }
static void masif_fm1_mods_trigger(uint8_t t, uint8_t instrument) { (void)t; (void)instrument; }
static void masif_fm1_mods(void) {}
#endif

static void masif_fm1_voice_sends(uint8_t t)
{
    uint8_t b;
    for (b = 0; b < 3u; ++b)
        if (masif_track_send[t][b] != MASIF_OWN) sp_send_track(t, b, masif_track_send[t][b]);
}

/* the row's effect fx's value (1) or none (0) */
static int masif_fm1_has(const masif_audio_event_t *e, uint8_t fx, uint32_t *v)
{
    if (e->fx == fx) { *v = e->param; return 1; }
    if (e->fx2 == fx) { *v = e->param2; return 1; }
    return 0;
}
/* a note or a retrigger: the instrument's part of its sample on the track's voice, then what the effects say */
static void masif_fm1_note(const masif_audio_event_t *e)
{
    uint8_t t = e->track;
    const masif_instrument_t *in;
    const sp_wave_t *w;
    uint32_t a, b, n, len, la, lb, part, play, v;
    int32_t cents;
    if (e->instrument > MASIF_AUDITION || e->note < 1u || e->note > 120u) return;
    in = MASIF_INST(e->instrument);
    if (in->sample >= MASIF_SAMPLES) return;
    w = &sp_wave[in->sample];
    n = w->n;
    if (!e->velocity || !n) {                       /* (a hit at no volume, or no sample there: the voice stops) */
        sp_stop_track(t);
        return;
    }
    a = (n >> 8) * in->start + (((n & 255u) * in->start) >> 8);              /* (32 bits: n < 2^24) */
    len = in->length ? (n >> 8) * in->length + (((n & 255u) * in->length) >> 8) : n - a;
    if (a + len > n) len = n - a;
    part = len;
    play = in->play;
    if (play >= MASIF_PLAY_REPITCH) play = (play - MASIF_PLAY_REPITCH) & 1u ? MASIF_PLAY_PING : MASIF_PLAY_LOOP;
    cents = in->fine;
    if (in->slices > 1u && masif_fm1_has(e, MASIF_FX_SLICE, &v)) {   /* SLC: that slice, at the note's pitch */
        a += len / in->slices * (v % in->slices);
        len /= in->slices;
        cents += ((int32_t)e->note - (int32_t)in->root_note) * 100;
    } else if (in->slices > 1u) {                   /* a slice a note from the root up, at the sample's own pitch */
        uint32_t k = e->note >= in->root_note ? (uint32_t)(e->note - in->root_note) : 0u;
        if (k >= in->slices) k = in->slices - 1u;
        a += len / in->slices * k;
        len /= in->slices;
    } else {
        cents += ((int32_t)e->note - (int32_t)in->root_note) * 100;
    }
    b = a + len;
    if (masif_fm1_has(e, MASIF_FX_SAMPLE_OFFSET, &v) && v) {   /* OFS: from the sample's start, to the part's end */
        a = (n >> 8) * v + (((n & 255u) * v) >> 8);                /* (or the sample's, if that is before it) */
        if (a + 1u >= b) b = n;
    }
    la = a; lb = b;
    if (in->play >= MASIF_PLAY_LOOP) {              /* its loop; an end of it outside the part: the part's */
        if ((in->flags & MASIF_INST_SMPLOOP) && w->lp_b) {          /* (the sample's own, to the sample) */
            la = w->lp_a;
            if (la < a || la + 1u >= b) la = a;
            lb = w->lp_b;
            if (lb > b || lb <= la + 1u) lb = b;
        } else {
            la = (n >> 8) * in->loop_start + (((n & 255u) * in->loop_start) >> 8);
            if (la < a || la + 1u >= b) la = a;
            if (in->loop_end) {
                lb = (n >> 8) * in->loop_end + (((n & 255u) * in->loop_end) >> 8);
                if (lb > b || lb <= la + 1u) lb = b;
            }
        }
    }
    {
        uint8_t adsr[4];
        adsr[0] = in->attack; adsr[1] = in->decay; adsr[2] = in->sustain; adsr[3] = in->release;
        sp_trigger_track_wave(t, in->sample, a, b, la, lb, play, cents, adsr,
                              t < MASIF_FILTER_TRACKS ? in->filter : MASIF_FILTER_OFF, in->cutoff, in->resonance, in->send);
        sp_taper_track(t, in->taper_in, in->taper_out);
    }
    if (in->play >= MASIF_PLAY_REPITCH && in->steps && part > 1u) {
        /* the tempo is the pitch. REPITCH: the part in `steps` rows, a row 150 / bpm10 seconds: part x bpm10 /
         * (steps x 150) Hz. BPM: the wave's own rate x the tempo / the sample's (and its stored flags, as a note) */
        uint32_t hz16;
        if (in->play >= MASIF_PLAY_BPM) {
            hz16 = (uint32_t)w->rate * 16u / in->steps * sq.bpm10 / 10u;
            if (w->uflags & 1u) hz16 = hz16 / 45u * 33u;
            if (w->uflags & 2u) cents -= 1200;
        } else {
            uint32_t x = (part < (1u << 19) ? part * sq.bpm10 : (part >> 4) * sq.bpm10 * 16u) / in->steps;
            if (x > 1200u * SP_FS) x = 1200u * SP_FS;
            hz16 = x * 16u / 150u;
        }
        sp_rate_track(t, hz16, cents);
    }
    {
        static const int8_t flat[3] = {0, 0, 0};
        sp_eq_track(t, t < MASIF_FILTER_TRACKS ? in->eq : flat);
        sp_drive_track(t, in->drive);
    }
    masif_track_inst[t] = e->instrument;
    masif_track_level[t] = e->velocity;
    masif_track_cents[t] = 0;
    masif_fm1_mods_start(t, in);
    if (e->instrument < MASIF_INSTRUMENTS) masif_fm1_mods_trigger(t, e->instrument);
    masif_fm1_voice_level(t);
    masif_fm1_voice_sends(t);
    if (e->type == MASIF_EVENT_NOTE_ON) {
        if ((e->fx == MASIF_FX_REVERSE && e->param) || (e->fx2 == MASIF_FX_REVERSE && e->param2)) sp_reverse_track(t);
    }
    if (e->pitch_offset) masif_fm1_voice_pitch(t, e->pitch_offset);
}

/* NOTE OFF: the instrument's release, if it has one (else the voice stops), and its ADSR slots' */
static void masif_fm1_note_off(uint8_t t)
{
    masif_fm1_mods_release(t);
    sp_release_track(t);
}

static void masif_audio_event(void *context, const masif_audio_event_t *e)
{
    uint8_t t;
    (void)context;
    if (!e || e->track >= MASIF_TRACKS) return;
    t = e->track;
    switch (e->type) {
    case MASIF_EVENT_NOTE_ON:
        masif_track_pan_set[t] = 0;                 /* PAN is a step's: a new note has its instrument's pan again */
        /* fall through */
    case MASIF_EVENT_RETRIGGER:
        masif_fm1_note(e);
        break;
    case MASIF_EVENT_NOTE_OFF:
        masif_fm1_note_off(t);
        break;
    case MASIF_EVENT_CUT:
        sp_stop_track(t);
        break;
    case MASIF_EVENT_VOLUME:
        masif_track_level[t] = e->velocity > 127u ? 127u : e->velocity;
        masif_fm1_voice_level(t);
        break;
    case MASIF_EVENT_PITCH:
        masif_fm1_voice_pitch(t, e->pitch_offset);
        break;
    case MASIF_EVENT_EFFECT:
        /* (after the row's note.) PAN: param 0 left .. 128 .. 255 right. SEND: the bus (0 chorus, 1 delay,
         * 2 reverb) and the amount 0-F. PAN for the sounding note only (its step), SEND also for the notes after it. SET_TEMPO:
         * the sequencer's tempo, 40-240 BPM, as its knob sets it (the delay follows in the main loop). The other
         * effects run in the portable runtime. */
        if (e->fx == MASIF_FX_PAN) {
            masif_track_pan[t] = e->param; masif_track_pan_set[t] = 1;
            masif_fm1_voice_level(t);
        } else if (e->fx == MASIF_FX_SEND && (e->param >> 4) < 3u) {
            masif_track_send[t][e->param >> 4] = (uint8_t)((e->param & 15u) * 127u / 15u);
            masif_fm1_voice_sends(t);
        } else if (e->fx == MASIF_FX_SET_TEMPO && e->param >= 40u && e->param <= 240u) {
            sq.bpm10 = (uint16_t)(e->param * 10u);
        }
        break;
    default:
        break;
    }
}

/* Per-track state the song's effects move: back to its start, so every PLAY sounds the same. */
static void masif_fm1_track_defaults(void)
{
    uint8_t t;
    for (t = 0; t < MASIF_TRACKS; ++t) {
        masif_track_volf[t] = 256;
        masif_track_panmod[t] = 0;
        masif_track_pan_set[t] = 0;
        masif_track_send[t][0] = masif_track_send[t][1] = masif_track_send[t][2] = MASIF_OWN;
    }
}

/* Cut the tracker's own voices (SMOOTH bridges the cut). The drums ring out as ever. */
static void masif_fm1_silence(void)
{
    uint8_t t;
    for (t = 0; t < MASIF_TRACKS; ++t) sp_stop_track(t);
}

/* ---- for the screens (the main loop): what PLAY plays, a note to hear, the mixer's mutes */
static struct { uint8_t what, track, index; } masif_play;          /* masif_play_what_t; the song from row `index` */
static void masif_fm1_play_what(uint8_t what, uint8_t track, uint8_t index)
{
    masif_play.what = what; masif_play.track = track; masif_play.index = index;      /* (taken at the next PLAY) */
}
static volatile uint8_t masif_mute;                                 /* bit t: track t + 1 is muted */
/* one note of an instrument on a track, at once, also while stopped (note 0: that track's NOTE OFF) */
static struct { volatile uint8_t pending; uint8_t track, instrument, note, velocity; } masif_preview;
static void masif_fm1_preview(uint8_t track, uint8_t instrument, uint8_t note, uint8_t velocity)
{
    if (masif_preview.pending) return;              /* (the one before it is still on its way) */
    masif_preview.track = track; masif_preview.instrument = instrument; masif_preview.note = note;
    masif_preview.velocity = velocity;
    masif_preview.pending = 1;
}

/* a sample of the pool heard as it is (wave 0..63), once, at its own pitch, also while stopped; the screens' */
static void masif_fm1_audition(uint8_t track, uint8_t wave)
{
    if (masif_preview.pending) return;
    masif_instrument_default(&masif_audition, wave);
    masif_audition.sample = wave;
    masif_fm1_preview(track, MASIF_AUDITION, masif_audition.root_note, 127);
}

/* the mixer and the project of the song in RAM, as the audio side uses them: every block (cheap), so an edit of
 * them is heard at once */
static int8_t masif_meq_set[3];
static void masif_fm1_mixer(void)
{
    uint8_t t;
    for (t = 0; t < MASIF_TRACKS; ++t)
        sp_trk_mix[t] = (masif_mute >> t) & 1u ? 0u : masif_song.track_level[t];
    if (masif_meq_set[0] != masif_song.master_eq[0] || masif_meq_set[1] != masif_song.master_eq[1] ||
        masif_meq_set[2] != masif_song.master_eq[2]) {
        for (t = 0; t < 3u; ++t) masif_meq_set[t] = masif_song.master_eq[t];
        sp_master_eq_set(masif_meq_set);
    }
}

/* The song in RAM was replaced (loaded): the player from its top, the song's tempo if it has one. Only while the
 * transport is stopped. */
static void masif_fm1_song_changed(void)
{
    masif_edit_queue_init(&masif_edits);
    if (masif_song.tempo >= 40u && masif_song.tempo <= 240u) sq.bpm10 = (uint16_t)(masif_song.tempo * 10u);
    masif_runtime_init(&masif_runtime, &masif_song, sq.bpm10);
    masif_play.what = MASIF_PLAY_SONG; masif_play.track = 0; masif_play.index = 0;
}

static void masif_fm1_init(void)
{
    masif_song_clear(&masif_song);
#if MASIF_DEMO_SONG
    (void)masif_demo_load(&masif_song);
#endif
    masif_edit_queue_init(&masif_edits);
    masif_runtime_init(&masif_runtime, &masif_song, sq.bpm10);
    masif_play.what = MASIF_PLAY_SONG; masif_play.track = 0; masif_play.index = 0;
    masif_mute = 0;
    masif_preview.pending = 0;
    masif_fm1_track_defaults();
    masif_running = 0;
    masif_run_seen = sq_run;
    masif_ticks_done = 0;
}

/* One audio block, after sq_block(): the ticks the sequencer's playhead has run since the
 * tracker last looked are the tracker's ticks. Stopped: nothing plays and its voices are
 * cut at once (the autosave waits for every voice, zp12.c). PLAY, also while playing:
 * from row 0. During the count-in sq_ticks stays 0, so row 0 waits for tick 0. */
static void masif_fm1_audio_block(void)
{
    uint32_t due = 0;
    /* Tempo is the sequencer's: kept here for display / effects only, it clocks nothing. */
    masif_runtime_set_bpm(&masif_runtime, sq.bpm10);
    if (!sq.playing) {
        if (masif_running) {
            masif_running = 0;
            masif_fm1_silence();
        }
    } else {
        if (!masif_running || masif_run_seen != sq_run) {
            masif_running = 1;
            masif_run_seen = sq_run;
            masif_ticks_done = 0;
            masif_fm1_silence();
            masif_fm1_track_defaults();
            masif_runtime_start(&masif_runtime, masif_play.what, masif_play.track, masif_play.index);
        }
        due = sq_ticks - masif_ticks_done;
        if (due > MASIF_BLOCK_TICKS) due = MASIF_BLOCK_TICKS;   /* (the rest in the next block: late, never lost) */
        masif_ticks_done += due;
    }
    (void)masif_runtime_ticks(&masif_runtime, (uint8_t)due, &masif_edits, masif_audio_event, 0);
    if (masif_preview.pending) {                    /* the screens' note to hear */
        masif_audio_event_t e;
        e.type = MASIF_EVENT_NOTE_ON; e.track = masif_preview.track; e.instrument = masif_preview.instrument;
        e.note = masif_preview.note; e.velocity = masif_preview.velocity; e.fx = e.param = e.fx2 = e.param2 = 0; e.pitch_offset = 0;
        if (e.track < MASIF_TRACKS) { if (e.note) masif_fm1_note(&e); else masif_fm1_note_off(e.track); }
        masif_preview.pending = 0;
    }
    masif_fm1_mixer();
    masif_fm1_mods();
}
