/* SPDX-License-Identifier: GPL-3.0-only */
/* zp12: the sound of an 80s 12-bit sampler. Self-contained (integer only, no tables to generate), so
 * the host tests and the web editor's preview can run the same code.
 *
 *   waves   12-bit linear samples, packed (2 in 3 bytes), recorded at 26.04 kHz or 27.5 kHz
 *   pitch   the playback rate, no interpolation: tuned up it skips samples, down it repeats them (the
 *           zero-order hold of a DAC clocked at the pitched rate); the aliasing is the sound
 *   sounds  32: a wave with TUNE / FINE, DECAY, LEVEL, PAN, START / END, REVERSE, 45->33, CHANNEL
 *   channels 8 outputs, one sound at a time each (a new hit on a channel cuts the last):
 *           1-2 a 4-pole resonant low-pass (SSM2044-style): open as CUT at the hit, it follows the decay
 *               envelope down two octaves, as the original's dynamic filters do; 3-6 a fixed 2-pole
 *               low-pass each, 7-8 no filter. The order is the hardware's: sample, VCA (the decay),
 *               DRIVE, the filter, then the channel's fader (the mixer's slider: it never changes the tone)
 *   out     a stereo mix in Q15; sp_out makes the master's 16 bits of it (-6 dB, a soft knee) */
#include <stdint.h>

#define SP_NCH 8
/* MASIF: four voices of the tracker's own after the eight channels (track t = voice SP_NCH + t). They are not
 * channels: no pad can be put on one, no fader in sp_mix, no bit in sp_mute / sp_solo, nothing saved */
#ifndef MASIF_ENABLE_TRACKER
#define MASIF_ENABLE_TRACKER 0
#endif
#if MASIF_ENABLE_TRACKER
#define SP_NTRK 4
#else
#define SP_NTRK 0
#endif
#define SP_NVOICE (SP_NCH + SP_NTRK)
#define SP_NSOUND 32
#define SP_FS 44100
#define SP_BLK 32                       /* the envelope and the filter cutoff move once a block */

typedef struct {                        /* a wave: where its packed samples are */
    const uint8_t *d;                   /* (flash, XIP) */
    uint32_t n;                         /* samples */
    uint16_t rate;                      /* Hz it was recorded at: 26040 or 27500 */
#if MASIF_ENABLE_TRACKER
    uint8_t uflags;                     /* an own sample's flags (sp_samples.c): bit 0 stored for 45->33, bit 1 at double
                                         * speed, bit 2 (MASIF): 8 bit, a signed byte a sample, in place of 12 bit packed,
                                         * bit 3 (MASIF): 4-bit IMA ADPCM in blocks (sp_adpcm_*) */
    uint8_t lp_play;                    /* the sample's own loop, as the web app set it (sp_samples.c): the play mode a new */
    uint32_t lp_a, lp_b;                /* instrument begins with, the loop's first sample and its end (lp_b 0: none), */
    uint8_t lp_tin, lp_tout;            /* and its tapers (0..128 in 1/256 of the loop) */
#endif
} sp_wave_t;

enum { SPF_REVERSE = 1, SPF_33 = 2 };   /* SPF_33: stored at 45 rpm, played at 33 1/3 (rate x 33/45) */

typedef struct {                        /* a sound (a pad) */
    uint8_t wave;                       /* index into sp_wave[] (0xFF: none) */
    int8_t tune;                        /* semitones -24..+12 */
    int8_t fine;                        /* cents -50..+50 */
    uint8_t decay;                      /* 0 short .. 127 the whole sound */
    uint8_t level;                      /* 0..127 */
    int8_t pan;                         /* -64..63 */
    uint8_t chan;                       /* 0..7 */
    uint8_t flags;                      /* SPF_* */
    uint16_t start, end;                /* truncate, 0..1000 of the wave (end 1000 = its end) */
    uint8_t cut, reso;                  /* channels 1-2: cutoff 0..127, resonance 0..127 */
    uint8_t drive;                      /* 0..127: the channel's input driven into a soft clip */
    uint8_t send[3];                    /* chorus, delay, reverb sends 0..127 (sp_fx.c) */
} sp_sound_t;

typedef struct {
    const sp_wave_t *w;
    uint32_t pos, end;                  /* the sample now (integer) and where it stops */
    uint32_t frac, step, base_step;     /* Q16: base_step retained for tracker pitch effects */
    int32_t dir;                        /* +1 / -1 */
    int32_t env, emul;                  /* Q24 level, Q16 its decay a block */
    uint8_t tail;                       /* blocks the filter still rings after the sample ended */
    int32_t gl, gr;                     /* Q12 gains: level x pan */
    int32_t cut, res;                   /* channel 1-2 filter settings */
    int32_t drv, snd[3];                /* drive (Q4 gain), sends Q15 */
    int32_t z[4];                       /* filter state */
    uint8_t on;
    uint8_t jump;                       /* the signal jumps next sample (a hit cut, a sample's end): SMOOTH bridges it */
    int32_t dc, last;                   /* SMOOTH: what bridges the jump, dying away; the last output */
    int32_t quiet;                      /* MUTE / SOLO: how far down, Q12 (0 heard, 4096 silent), ramped */
#if MASIF_ENABLE_TRACKER
    uint32_t loop, lend;                /* a tracker voice: its loop [loop, lend) ... */
    uint8_t looped;                     /* ... if this is set: 1 round it, 2 to and fro (a pad's voice never) */
    uint8_t osc;                        /* ... and as an oscillator: a new note goes on where the loop is */
    uint32_t tin, tout;                 /* ... the loop's tapers: samples after its start the level rises over, before its */
    uint32_t rtin, rtout;               /* end it falls over (0: none), and their reciprocals, Q16 (no division here) */
    /* a tracker voice's envelope and filter (an instrument's; a pad's voice: stage 1, sustain 0, no filter flag) */
    uint8_t stage;                      /* 0 attack (env rises by `att` a block), 1 decay to `sus`, 2 release */
    uint8_t flt;                        /* its filter: 0 none, 1 low-pass, 2 band-pass, 3 high-pass (cut, res) */
    uint8_t eq_on;                      /* its instrument's 3-band EQ is not flat: */
    int16_t eqg[3];                     /* the gains of low / mid / high, Q12 */
    int32_t eqz[2];                     /* and the two one-pole states that part them (250 Hz, 3 kHz) */
    int32_t att, sus, rmul;             /* Q24 a block; Q24; Q16 a block */
#endif
} sp_ch_t;

static sp_wave_t sp_wave[64];
static sp_sound_t sp_sound[SP_NSOUND];
static sp_ch_t sp_ch[SP_NVOICE];
static uint8_t sp_mix[SP_NCH] = {100, 100, 100, 100, 100, 100, 100, 100};   /* the channel faders, 0..127 */
#if MASIF_ENABLE_TRACKER
static uint8_t sp_trk_mix[SP_NTRK] = {100, 100, 100, 100};   /* the tracker voices' own faders, 0..127 (not saved, no UI yet) */
#endif
static uint8_t sp_mute, sp_solo;        /* GLO held + white key: channels muted, soloed (a solo silences the others) */
static uint8_t sp_smooth = 1;            /* GLO > OUTPUT > SMOOTH: a jump in a channel's signal bridged in ~1 ms (no click) */
static int32_t sp_bus[3][SP_BLK];       /* the send buses of the block: chorus, delay, reverb (mono, Q15) */

/* sample i of a packed wave, -2048..2047 */
static inline int32_t sp_sample(const uint8_t *d, uint32_t i)
{
    const uint8_t *p = d + (i >> 1) * 3u;
    int32_t v = (i & 1u) ? (p[1] >> 4) | (p[2] << 4) : p[0] | ((p[1] & 15) << 8);
    return (v ^ 0x800) - 0x800;
}

#if MASIF_ENABLE_TRACKER
/* 4-bit IMA ADPCM (2026-10-23: about three times 12 bit's time, near its sound): blocks of 128 samples in 68 bytes,
 * so a voice can start, loop and turn anywhere. A block: the decoder's state before its first sample (the 16-bit
 * value, little end first, and the step's index), a 0, then 128 codes, the first in the low half of a byte. The same
 * in web/zp12link.js (packAdpcm / unpackAdpcm) and masif_rec.c (the recorder's encoder). Decoded: 16-bit values */
#define SP_ADB 128u
#define SP_ADBYTES 68u
static const int16_t SP_ADSTEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t SP_ADIDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static inline void sp_adpcm_step(int32_t *pred, int32_t *idx, uint32_t c)   /* one code into the decoder's state */
{
    int32_t st = SP_ADSTEP[*idx], d = st >> 3;
    if (c & 4u) d += st;
    if (c & 2u) d += st >> 1;
    if (c & 1u) d += st >> 2;
    *pred += (c & 8u) ? -d : d;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += SP_ADIDX[c & 7u];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
}
static uint32_t sp_adpcm_bytes(uint32_t n) { return (n + SP_ADB - 1u) / SP_ADB * SP_ADBYTES; }
/* block b of a wave decoded into out (SP_ADB values) */
static void sp_adpcm_block(const uint8_t *d, uint32_t b, int16_t *out)
{
    const uint8_t *p = d + b * SP_ADBYTES;
    int32_t pred = (int16_t)(p[0] | p[1] << 8), idx = p[2] > 88u ? 88 : p[2];
    uint32_t k;
    for (k = 0; k < SP_ADB; k++) {
        sp_adpcm_step(&pred, &idx, (p[4u + (k >> 1)] >> ((k & 1u) * 4u)) & 15u);
        out[k] = (int16_t)pred;
    }
}
static int32_t sp_adpcm_at(const uint8_t *d, uint32_t i)   /* one value, from its block's start (the pictures) */
{
    const uint8_t *p = d + (i / SP_ADB) * SP_ADBYTES;
    int32_t pred = (int16_t)(p[0] | p[1] << 8), idx = p[2] > 88u ? 88 : p[2];
    uint32_t k;
    for (k = 0; k <= i % SP_ADB; k++) sp_adpcm_step(&pred, &idx, (p[4u + (k >> 1)] >> ((k & 1u) * 4u)) & 15u);
    return pred;
}
/* each voice's block decoded (it plays on, back, round a loop within it without decoding again) */
static int16_t sp_adc[SP_NVOICE][SP_ADB] __attribute__((section(".pool")));
static const uint8_t *sp_adw[SP_NVOICE];
static uint32_t sp_adb[SP_NVOICE];
static inline int32_t sp_adpcm_voice(uint32_t ch, const uint8_t *d, uint32_t pos)
{
    uint32_t b = pos / SP_ADB;
    if (sp_adw[ch] != d || sp_adb[ch] != b) { sp_adpcm_block(d, b, sp_adc[ch]); sp_adw[ch] = d; sp_adb[ch] = b; }
    return sp_adc[ch][pos % SP_ADB];
}
#endif

/* pack n 16-bit samples (already 12-bit: the low 4 bits are dropped) into 12-bit pairs; d holds
 * (n + 1) / 2 * 3 bytes */
static void sp_pack(uint8_t *d, const int16_t *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i += 2) {
        uint32_t a = (uint32_t)(s[i] >> 4) & 0xFFFu, b = i + 1u < n ? (uint32_t)(s[i + 1] >> 4) & 0xFFFu : 0u;
        d[0] = (uint8_t)a;
        d[1] = (uint8_t)((a >> 8) | (b << 4));
        d[2] = (uint8_t)(b >> 4);
        d += 3;
    }
}

static inline int32_t sp_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* 2^(c / 1200) in Q16 for c in cents, -2400..+1200 (no float: the semitones from a table, the cents
 * between them linear, < 0.1 cent off) */
static uint32_t sp_pow2_cents(int32_t c)
{
    static const uint32_t SEMI[13] = {65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032,
                                      110218, 116772, 123715, 131072};
    int32_t oct = 0, s, r;
    uint32_t a, b, v;
    while (c < 0) { c += 1200; oct--; }
    while (c >= 1200) { c -= 1200; oct++; }
    s = c / 100, r = c % 100;
    a = SEMI[s], b = SEMI[s + 1];
    v = a + (b - a) * (uint32_t)r / 100u;          /* (< 2^17 * 100: 32 bits) */
    return oct >= 0 ? v << oct : v >> -oct;
}

/* the decay a block for DECAY d: 127 = none; else a time constant from ~15 ms to ~2.5 s */
static int32_t sp_decay_mul(uint32_t d)
{
    uint32_t ms, blocks;
    if (d >= 127u)
        return 65536;
    ms = 15u + (d * d * 155u) / 1000u;               /* 15 ms .. ~2.5 s, finer at the short end */
    blocks = ms * (SP_FS / 1000u) / SP_BLK;
    return 65536 - (int32_t)(65536u / (blocks + 1u));  /* e^(-1) after ~blocks blocks */
}

/* the one-pole coefficient (Q15) for a cutoff in Hz: 1 - e^(-2 pi f / fs), e^-x as (1 - x/256)^256
 * (eight squarings in Q30: within 0.3 % up to 20 kHz) */
static int32_t sp_onepole(uint32_t hz)
{
    int64_t x = (int64_t)hz * 152982;              /* 2 pi f / fs in Q30 (2^30 * 2 pi / 44100; no 64-bit division) */
    int64_t y = (1LL << 30) - (x >> 8);
    uint32_t i;
    for (i = 0; i < 8u; i++)
        y = (y * y) >> 30;
    return (int32_t)(((1LL << 30) - y) >> 15);
}

/* channels 1-2: CUT 0..127 -> 60 Hz .. 16 kHz, exponential */
static uint32_t sp_cut_hz(int32_t c)
{
    return (uint32_t)(60u * sp_pow2_cents(c * 9600 / 127) >> 16);
}

/* a soft clip, x in +-49152 (1.5 in Q15): 1.5 (q - q^3 / 3) for q = x / 1.5, 32-bit */
static int32_t sp_soft(int32_t x)
{
    int32_t q = x * 2 / 3, q3 = ((q * q) >> 15) * q >> 15;
    return (q - q3 / 3) * 3 / 2;
}

/* a hit's own TUNE + FINE, DECAY and CUT (recorded with it while they were turned, sp_seq.c), one word:
 * bit 31 set, cents + 2560 in 0-11, decay in 12-18, cut in 19-25; 0 = the sound's own */
#define SP_LOCK 0x80000000u
static uint32_t sp_lock_of(const sp_sound_t *s)
{
    return SP_LOCK | (uint32_t)(s->tune * 100 + s->fine + 2560) | (uint32_t)(s->decay & 127u) << 12 |
           (uint32_t)(s->cut & 127u) << 19;
}

/* hit sound k at velocity 1..127, `semis` semitones from its TUNE (MULTI PITCH), with a hit's lock (0: none);
 * voice < 0: on the sound's own channel, else on that voice of sp_ch (the caller has checked it) */
static void sp_trigger_lk_on_channel(uint32_t k, uint32_t vel, int32_t semis, uint32_t lk, int32_t voice)
{
    const sp_sound_t *s = &sp_sound[k % SP_NSOUND];
    sp_ch_t *c;
    const sp_wave_t *w;
    uint32_t a, b, rate;
    int32_t cents, lv;
    if (s->wave >= 64u || !sp_wave[s->wave].n)
        return;
    w = &sp_wave[s->wave];
    c = &sp_ch[voice >= 0 ? (uint32_t)voice : s->chan % SP_NCH];
    a = w->n / 1000u * s->start + w->n % 1000u * s->start / 1000u;   /* (32 bits, no 64-bit division) */
    b = s->end ? w->n / 1000u * s->end + w->n % 1000u * s->end / 1000u : w->n;
    if (b > w->n) b = w->n;
    if (b <= a + 1u)
        return;
    cents = (lk ? (int32_t)(lk & 0xFFFu) - 2560 : s->tune * 100 + s->fine) + semis * 100;
    cents = sp_clamp(cents, -3650, 2450);
    rate = w->rate;
    if (s->flags & SPF_33)
        rate = rate * 33u / 45u;
    c->w = w;
#if MASIF_ENABLE_TRACKER
    sp_adw[c - sp_ch] = 0;                            /* (the flash under the same address may be another sample now) */
#endif
    c->dir = (s->flags & SPF_REVERSE) ? -1 : 1;
    c->pos = c->dir > 0 ? a : b - 1u;
    c->end = c->dir > 0 ? b : a;
    c->frac = 0;
    c->base_step = (uint32_t)(((uint64_t)(rate * 65536u / SP_FS) * sp_pow2_cents(cents)) >> 16);
    c->step = c->base_step;
    c->env = 1 << 24;
    c->emul = sp_decay_mul(lk ? (lk >> 12) & 127u : s->decay);
    lv = (int32_t)s->level * s->level / 127 * (int32_t)vel / 127;   /* 0..127: LEVEL in an audio taper (squared) */
    c->gl = lv * (64 - (s->pan > 0 ? s->pan : 0)) / 3;   /* Q12: 127 * 64 / 3 = 2709, -3.6 dB a channel */
    c->gr = lv * (64 + (s->pan < 0 ? s->pan : 0)) / 3;   /* (all at once round off in sp_out's knee: sp_master) */
    c->cut = lk ? (int32_t)((lk >> 19) & 127u) : s->cut;
    c->res = s->reso;
    c->drv = s->drive ? 16 + s->drive / 2 : 0;          /* 1x .. ~5x into the clip */
    c->snd[0] = s->send[0] * 258;
    c->snd[1] = s->send[1] * 258;
    c->snd[2] = s->send[2] * 258;
    c->tail = 8;
    /* SMOOTH only where it clicks: a sound still sounding cut, or a start inside the wave (TRUNC); a sample from
     * its own beginning keeps its attack */
    c->jump = (uint8_t)(c->on || ((s->flags & SPF_REVERSE) ? s->end < 1000u && s->end : s->start > 0u));
    if (!c->jump) c->dc = 0;
    c->on = 1;
}

static void sp_trigger_lk(uint32_t k, uint32_t vel, int32_t semis, uint32_t lk)
{
    sp_trigger_lk_on_channel(k, vel, semis, lk, -1);
}
static void sp_trigger_at(uint32_t k, uint32_t vel, int32_t semis) { sp_trigger_lk(k, vel, semis, 0); }
static void sp_trigger(uint32_t k, uint32_t vel) { sp_trigger_at(k, vel, 0); }
#if MASIF_ENABLE_TRACKER
/* MASIF: tracker track 0..3 plays on its own voice and on nothing else; any other track is refused (never
 * wrapped onto a channel), so nothing here can reach a drum's voice */
#define SP_TRK_STEP_MAX (4u << 16)      /* a tracker voice's step at the most: the pitch loop in sp_channel then
                                         * runs 4 times a sample at the most, however far a slide went (a pad's
                                         * own most is ~2.6 with the kit's waves; an own sample of 48 kHz
                                         * at +24.5 semitones would be 4.48). base_step is the note's true
                                         * pitch, never capped: a pitch offset counts from it, so back at 0
                                         * the step is what the note began with */
static uint32_t sp_trk_step(uint64_t step) { return step > SP_TRK_STEP_MAX ? SP_TRK_STEP_MAX : (uint32_t)step; }
static void sp_trigger_track(uint32_t k, uint32_t vel, int32_t semis, uint32_t track)
{
    sp_ch_t *c;
    if (track >= SP_NTRK) return;
    c = &sp_ch[SP_NCH + track];
    c->looped = 0; c->osc = 0; c->stage = 1; c->sus = 0; c->flt = 0; c->eq_on = 0; c->tin = c->tout = 0;
    sp_trigger_lk_on_channel(k, vel, semis, 0, (int32_t)(SP_NCH + track));
    c->step = sp_trk_step(c->step);                 /* (the note's start as every pitch after it) */
}
/* A tracker instrument's note: not a pad's sound but a part of a wave, as the instrument says. [a, b) in samples
 * of wave w; play (masif_play_t's numbers): 0 once, 1 once backwards (from b - 1 down), 2 round the loop [la, lb)
 * inside the part once it is there, 3 the same beginning backwards from b - 1, 4 and 5 to and fro in it
 * (ping-pong), 6-8 the loop alone as an oscillator (forwards, backwards, to and fro): nothing outside it is
 * played, and while the voice sounds that loop a new note leaves it where it is; a loop that is not inside the
 * part: once. cents from the wave's own pitch. adsr: attack, decay, sustain, release, 0..127 each (times as a
 * pad's DECAY; decay 127: none; release 0: a NOTE OFF cuts). filter: 0 none, 1 low-pass, 2 band-pass, 3 high-pass. Its level and pan with
 * sp_level_track. */
static void sp_trigger_track_wave(uint32_t track, uint32_t wi, uint32_t a, uint32_t b, uint32_t la, uint32_t lb, uint32_t play,
                                  int32_t cents, const uint8_t *adsr, uint32_t filter, uint32_t cutoff, uint32_t reso,
                                  const uint8_t *send)
{
    sp_ch_t *c;
    const sp_wave_t *w;
    uint32_t rate;
    const int backwards = (int)(play & 1u), ping = play == 4u || play == 5u || play == 8u;
    uint8_t looped, osc;
    if (track >= SP_NTRK || wi >= 64u || !sp_wave[wi].n) return;
    c = &sp_ch[SP_NCH + track];
    w = &sp_wave[wi];
    if (b > w->n) b = w->n;
    if (b <= a + 1u) return;
    rate = w->rate;
    if (w->uflags & 1u) rate = rate * 33u / 45u;    /* an own sample stored for 45 -> 33 */
    if (w->uflags & 2u) cents -= 1200;              /* ... or at double speed */
    looped = (uint8_t)(play < 2u || la < a || lb > b || la + 1u >= lb ? 0u : ping ? 2u : 1u);
    osc = (uint8_t)(looped && play >= 6u);
    if (osc && c->on && c->osc && c->w == w && c->looped == looped && c->loop == la && c->lend == lb &&
        (looped == 2u || (c->dir < 0) == backwards)) {
        osc = 2;                                    /* (it runs on: only the pitch, the envelope and the rest are new) */
    } else {
        if (osc) { a = la; b = lb; }
        c->dir = backwards ? -1 : 1;
        c->pos = backwards ? b - 1u : a;
        if (looped) c->end = backwards ? la : lb;   /* (what is past the loop's far end is never reached) */
        else c->end = backwards ? a : b;
        c->frac = 0;
    }
    c->w = w;
    sp_adw[c - sp_ch] = 0;
    c->looped = looped; c->osc = (uint8_t)(osc != 0u);
    c->loop = la; c->lend = lb;
    c->tin = c->tout = 0;                           /* (sp_taper_track sets them) */
    c->base_step = (uint32_t)(((uint64_t)(rate * 65536u / SP_FS) * sp_pow2_cents(sp_clamp(cents, -4800, 4800))) >> 16);
    c->step = sp_trk_step(c->base_step);
    c->emul = sp_decay_mul(adsr[1]);
    c->sus = (int32_t)adsr[2] * ((1 << 24) / 127);
    c->rmul = adsr[3] ? sp_decay_mul(adsr[3]) : 0;
    if (adsr[0]) {                                  /* (the attack's time is the time DECAY of that value takes) */
        c->att = (1 << 24) - (int32_t)(((int64_t)(1 << 24) * sp_decay_mul(adsr[0])) >> 16);
        if (c->att < 1) c->att = 1;
        c->env = c->on ? c->env : 0;                /* (a voice still sounding goes on from its level: no step) */
        c->stage = 0;
    } else {
        c->env = 1 << 24;
        c->stage = 1;
    }
    c->flt = (uint8_t)(filter & 3u);
    if (!c->on || !c->flt) c->z[0] = c->z[1] = c->z[2] = c->z[3] = 0;
    c->cut = (int32_t)cutoff; c->res = (int32_t)reso; c->drv = 0;
    c->snd[0] = send[0] * 258; c->snd[1] = send[1] * 258; c->snd[2] = send[2] * 258;
    c->tail = 8;
    c->jump = (uint8_t)(c->on || a > 0u || backwards);
    if (!c->jump) c->dc = 0;
    c->on = 1;
}
/* The 3-band EQ of the tracker's voices and of the mix: a value -64..64 is -12..+12 dB (Q12 gain); the bands part
 * at 250 Hz and 3 kHz. */
/* a tracker voice's loop tapers: in and out 0..128 in 1/256 of its loop (a voice that does not loop: none). The
 * level rises from silence over the first part of the loop and falls to it over the last, every time round */
static void sp_taper_track(uint32_t track, uint32_t in, uint32_t out)
{
    sp_ch_t *c;
    uint32_t len;
    if (track >= SP_NTRK) return;
    c = &sp_ch[SP_NCH + track];
    len = c->looped ? c->lend - c->loop : 0u;
    if (in > 128u) in = 128u;
    if (out > 128u) out = 128u;
    c->tin = len >> 8 ? (len >> 8) * in + (((len & 255u) * in) >> 8) : len * in >> 8;
    c->tout = len >> 8 ? (len >> 8) * out + (((len & 255u) * out) >> 8) : len * out >> 8;
    c->rtin = c->tin ? 65536u / c->tin : 0u;
    c->rtout = c->tout ? 65536u / c->tout : 0u;
}

static int32_t sp_eq_a[2];
static int32_t sp_eq_gain(int32_t v) { return (int32_t)(sp_pow2_cents(sp_clamp(v, -64, 64) * 2400 / 64) >> 4); }
static void sp_eq_init(void)
{
    if (sp_eq_a[0]) return;
    sp_eq_a[0] = sp_onepole(250u);
    sp_eq_a[1] = sp_onepole(3000u);
}
/* a track's voice: its EQ, also while it sounds (all three 0: none, and no cost) */
static void sp_eq_track(uint32_t track, const int8_t *eq)
{
    sp_ch_t *c;
    if (track >= SP_NTRK) return;
    c = &sp_ch[SP_NCH + track];
    sp_eq_init();
    c->eqg[0] = (int16_t)sp_eq_gain(eq[0]); c->eqg[1] = (int16_t)sp_eq_gain(eq[1]); c->eqg[2] = (int16_t)sp_eq_gain(eq[2]);
    if (!c->eq_on) c->eqz[0] = c->eqz[1] = 0;
    c->eq_on = (uint8_t)(eq[0] || eq[1] || eq[2]);
}
/* the mix (stereo, `frames` frames of Q15): the same three bands on everything, drums and tracker */
static struct { int32_t g[3], z[4]; uint8_t on; } sp_meq;
static void sp_master_eq_set(const int8_t *eq)
{
    sp_eq_init();
    sp_meq.g[0] = sp_eq_gain(eq[0]); sp_meq.g[1] = sp_eq_gain(eq[1]); sp_meq.g[2] = sp_eq_gain(eq[2]);
    if (!sp_meq.on) sp_meq.z[0] = sp_meq.z[1] = sp_meq.z[2] = sp_meq.z[3] = 0;
    sp_meq.on = (uint8_t)(eq[0] || eq[1] || eq[2]);
}
static void sp_master_eq(int32_t *o, uint32_t frames)
{
    uint32_t i, k;
    if (!sp_meq.on) return;
    for (i = 0; i < frames; i++)
        for (k = 0; k < 2u; k++) {
            int32_t y = o[2u * i + k], *z = &sp_meq.z[2u * k];
            z[0] += (int32_t)(((int64_t)(y - z[0]) * sp_eq_a[0]) >> 15);
            z[1] += (int32_t)(((int64_t)(y - z[1]) * sp_eq_a[1]) >> 15);
            o[2u * i + k] = (int32_t)(((int64_t)z[0] * sp_meq.g[0] + (int64_t)(z[1] - z[0]) * sp_meq.g[1] +
                                       (int64_t)(y - z[1]) * sp_meq.g[2]) >> 12);
        }
}
/* For a screen that shows the sound (a scope, a spectrum on the mixer's page): the last 256 samples of the mix,
 * mono, 16 bit, taken only while sp_scope_on is set. sp_scope_w is where the next one goes. */
#define SP_SCOPE 256u
static int16_t sp_scope[SP_SCOPE];
static volatile uint8_t sp_scope_on, sp_scope_w;
static void sp_scope_tap(const int32_t *o, uint32_t frames)
{
    uint32_t i, w = sp_scope_w;
    if (!sp_scope_on) return;
    for (i = 0; i < frames; i++) {
        sp_scope[w] = (int16_t)sp_clamp((o[2u * i] + o[2u * i + 1u]) >> 5, -32768, 32767);
        w = (w + 1u) % SP_SCOPE;
    }
    sp_scope_w = (uint8_t)w;
}
static void sp_stop_track(uint32_t track);
/* NOTE OFF: the envelope's release, if the instrument has one; else the voice stops (sp_stop_track) */
static void sp_release_track(uint32_t track)
{
    sp_ch_t *c;
    if (track >= SP_NTRK) return;
    c = &sp_ch[SP_NCH + track];
    if (c->on && c->rmul) c->stage = 2;
    else sp_stop_track(track);
}
/* the filter's cutoff of a track's voice while it sounds (0..126; the LFO's) */
static void sp_cutoff_track(uint32_t track, int32_t cutoff)
{
    if (track < SP_NTRK) sp_ch[SP_NCH + track].cut = sp_clamp(cutoff, 0, 127);
}
/* the playing note at a rate of its own in place of its wave's (Hz x 16; a part that has to last so long), cents
 * from there: REPITCH and BPM */
static void sp_rate_track(uint32_t track, uint32_t hz16, int32_t cents)
{
    sp_ch_t *c;
    uint32_t step;
    if (track >= SP_NTRK) return;
    c = &sp_ch[SP_NCH + track];
    if (hz16 > 128u * SP_FS) hz16 = 128u * SP_FS;   /* (8 times the output's: the step is capped far below) */
    step = (hz16 / SP_FS) * 4096u + (hz16 % SP_FS) * 4096u / SP_FS;
    c->base_step = (uint32_t)(((uint64_t)step * sp_pow2_cents(sp_clamp(cents, -4800, 4800))) >> 16);
    c->step = sp_trk_step(c->base_step);
}
/* its drive, as a drum channel's but further: 0 none, 1..127 = 1x .. 9x into the soft clip */
static void sp_drive_track(uint32_t track, uint32_t drive)
{
    if (track < SP_NTRK) sp_ch[SP_NCH + track].drv = drive ? 16 + (int32_t)(drive > 127u ? 127u : drive) : 0;
}
/* ... and its resonance (0..127) */
static void sp_res_track(uint32_t track, int32_t res)
{
    if (track < SP_NTRK) sp_ch[SP_NCH + track].res = sp_clamp(res, 0, 127);
}
/* the level (0..127, linear) and the pan (-64 left .. 64 right) of a track's voice, also while it sounds */
static void sp_level_track(uint32_t track, uint32_t level, int32_t pan)
{
    sp_ch_t *c;
    if (track >= SP_NTRK) return;
    c = &sp_ch[SP_NCH + track];
    pan = sp_clamp(pan, -64, 64);
    if (level > 127u) level = 127u;
    c->gl = (int32_t)level * (64 - (pan > 0 ? pan : 0)) / 3;   /* (as a pad's: Q12, 2709 the most) */
    c->gr = (int32_t)level * (64 + (pan < 0 ? pan : 0)) / 3;
}
static void sp_pitch_track_cents(uint32_t track, int32_t cents)
{
    sp_ch_t *c;
    if (track >= SP_NTRK) return;
    c = &sp_ch[SP_NCH + track];
    if (!c->base_step) return;
    if (cents < -4800) cents = -4800;
    if (cents > 4800) cents = 4800;
    c->step = sp_trk_step(((uint64_t)c->base_step * sp_pow2_cents(cents)) >> 16);
}
static void sp_pitch_track(uint32_t track, int32_t semis)
{
    sp_pitch_track_cents(track, semis < -48 ? -4800 : semis > 48 ? 4800 : semis * 100);
}
/* the playing note of a track: a send (bus 0-2: chorus, delay, reverb; 0..127), its direction */
static void sp_send_track(uint32_t track, uint32_t bus, uint32_t amount)
{
    if (track < SP_NTRK && bus < 3u)
        sp_ch[SP_NCH + track].snd[bus] = (amount > 127u ? 127u : amount) * 258u;
}
static void sp_reverse_track(uint32_t track)
{
    sp_ch_t *c;
    uint32_t pos;
    if (track >= SP_NTRK) return;
    c = &sp_ch[SP_NCH + track];
    if (!c->on || c->end == c->pos) return;
    pos = c->pos;                                   /* forwards [pos, end) <-> backwards from end - 1 down to pos */
    if (c->dir > 0) { c->pos = c->end - 1u; c->end = pos; c->dir = -1; }
    else { c->pos = c->end; c->end = pos + 1u; c->dir = 1; }
    c->looped = 0; c->osc = 0;                      /* (backwards: once) */
    c->jump = 1;
}
static void sp_stop_track(uint32_t track)
{
    sp_ch_t *c;
    if (track >= SP_NTRK) return;
    c = &sp_ch[SP_NCH + track];
    if (c->on || c->tail) {
        c->on = 0;
        c->tail = 8; /* let the existing filter/smoothing path die away */
        c->jump = 1;
    }
}
#endif

/* channel ch's next SP_BLK samples into out (stereo, added). ch >= SP_NCH is a tracker voice: no filter (as
 * channels 7-8), its own fader (sp_trk_mix), the sound's sends, and outside MUTE / SOLO: a muted channel does
 * not mute it and a soloed channel does not silence it (the eight bits are the drums' only) */
static void sp_channel(uint32_t ch, int32_t *out)
{
    sp_ch_t *c = &sp_ch[ch];
    uint32_t i;
    int32_t a1 = 0, k = 0, x, y, e0, e1, env1, gl, gr, q0 = c->quiet, q1, qt, mix;
    uint32_t fmode = ch < 2u ? 2u : ch < 6u ? 1u : 0u;   /* the channel's filter: 2 the 4-pole, 1 the fixed, 0 none; */
#if MASIF_ENABLE_TRACKER                              /* 3: a tracker voice's state-variable filter */
    int32_t sv1 = 0, sv2 = 0, sv3 = 0, svk = 0;
#endif
#if MASIF_ENABLE_TRACKER
    if (ch >= SP_NCH) {
        qt = 0;
        mix = sp_trk_mix[ch - SP_NCH];
    } else
#endif
    {
        qt = (sp_solo ? !((sp_solo >> ch) & 1u) : (sp_mute >> ch) & 1u) ? 4096 : 0;
        mix = sp_mix[ch];
    }
    q1 = q0 < qt ? (q0 + 1024 < qt ? q0 + 1024 : qt) : (q0 - 1024 > qt ? q0 - 1024 : qt);   /* (~3 ms: no click) */
    c->quiet = q1;
    if (!c->on && !c->tail) {
        c->quiet = qt;                                /* (silent anyway: there at once) */
        return;
    }
    e0 = c->env >> 9;                                 /* Q15: the envelope now, and at the block's end: the level */
    env1 = (int32_t)(((int64_t)c->env * c->emul) >> 16);
#if MASIF_ENABLE_TRACKER
    if (ch >= SP_NCH) {                               /* an instrument's envelope: attack, decay to sustain, release */
        if (c->stage == 0u) {
            env1 = c->env + c->att;
            if (env1 >= (1 << 24)) { env1 = 1 << 24; c->stage = 1; }
        } else if (c->stage == 2u) {
            env1 = (int32_t)(((int64_t)c->env * c->rmul) >> 16);
        } else {
            env1 = c->sus + (int32_t)(((int64_t)(c->env - c->sus) * c->emul) >> 16);
        }
        fmode = c->flt ? 3u : 0u;
    }
#endif
    e1 = env1 >> 9;                                   /* ramps between them (no steps at the block rate) */
    gl = c->gl * mix * mix / 10000;                   /* the fader, an audio taper as the original's sliders: */
    gr = c->gr * mix * mix / 10000;                   /* 100 as set, 50 -12 dB, 25 -24 dB, 127 +4 dB */
    if (ch < 2u) {                                    /* the dynamic filter: CUT at the hit, two octaves down as it decays */
        uint32_t hz = sp_cut_hz(c->cut) * sp_pow2_cents(-2400 + ((e0 * 2400) >> 15)) >> 16;
        a1 = sp_onepole(hz < 30u ? 30u : hz > 18000u ? 18000u : hz);
        k = c->res * 125;                             /* Q12 feedback, 0 .. ~3.9 (self-oscillation near 4) */
    } else if (ch < 6u) {
        a1 = sp_onepole(ch < 4u ? 9000u : 12000u);    /* the fixed output filters */
    }
#if MASIF_ENABLE_TRACKER
    if (fmode == 3u) {
        /* a state-variable filter (the trapezoidal one: stable at any cutoff): low, band and high from the same
         * two states. g = tan(pi f / fs) by its series (f to 12 kHz: within 1 %), k = 1 / Q from 1.41 (flat) to 0.1 */
        uint32_t hz = sp_cut_hz(c->cut), x, x2, g, den;
        if (hz < 30u) hz = 30u;
        if (hz > 12000u) hz = 12000u;
        x = hz * 4669u / 1000u;                       /* Q16 of pi f / fs */
        x2 = (x * x) >> 16;
        g = x + (uint32_t)(((uint64_t)x * x2 / 3u) >> 16);
        g += (uint32_t)(((uint64_t)x * ((x2 * x2) >> 16) * 2u / 15u) >> 16);
        g += (uint32_t)(((uint64_t)x * ((((x2 * x2) >> 16) * x2) >> 16) * 17u / 315u) >> 16);
        svk = 92682 - c->res * 650;
        den = 65536u + (uint32_t)(((uint64_t)g * (g + (uint32_t)svk)) >> 16);
        sv1 = (int32_t)(0xFFFFFFFFu / den);           /* Q16: 1 / (1 + g (g + k)) */
        sv2 = (int32_t)(((uint64_t)g * (uint32_t)sv1) >> 16);
        sv3 = (int32_t)(((uint64_t)g * (uint32_t)sv2) >> 16);
    }
#endif
    for (i = 0; i < SP_BLK; i++) {
        if (c->on) {
#if MASIF_ENABLE_TRACKER
            if (c->w->uflags & 8u)
                x = sp_adpcm_voice(ch, c->w->d, c->pos);      /* ADPCM: 16 bit, Q15 as it is */
            else if (c->w->uflags & 4u)
                x = (int32_t)(int8_t)c->w->d[c->pos] * 256;   /* an 8-bit sample: Q15 too */
            else
#endif
            x = sp_sample(c->w->d, c->pos) * 16;      /* Q15 */
#if MASIF_ENABLE_TRACKER
            if (c->looped && (c->tin | c->tout)) {    /* the loop's tapers: its ends faded, so it goes round without a click */
                const uint32_t p = c->pos;
                if (p >= c->loop && p - c->loop < c->tin) x = (x * (int32_t)(((p - c->loop) * c->rtin) >> 1)) >> 15;
                if (p < c->lend && c->lend - 1u - p < c->tout) x = (x * (int32_t)(((c->lend - 1u - p) * c->rtout) >> 1)) >> 15;
            }
#endif
            c->frac += c->step;                       /* zero-order hold: the sample now, no interpolation */
            while (c->frac >= 65536u) {
                c->frac -= 65536u;
                c->pos += (uint32_t)c->dir;
                if (c->pos == c->end || (c->dir < 0 && c->pos == 0xFFFFFFFFu)) {
#if MASIF_ENABLE_TRACKER
                    if (c->looped == 2u) {            /* ping-pong: it turns round at the loop's end (no seam) */
                        if (c->dir > 0) { c->dir = -1; c->pos = c->lend - 1u; c->end = c->loop; }
                        else { c->dir = 1; c->end = c->lend; }
                        continue;
                    }
                    if (c->looped) {                  /* round the loop (SMOOTH bridges the seam) */
                        c->pos = c->dir > 0 ? c->loop : c->lend - 1u;
                        c->jump = 1;
                        continue;
                    }
#endif
                    c->on = 0;
                    c->jump = 1;                      /* (the sample ends: from where it was to silence) */
                    break;
                }
            }
        } else {
            x = 0;
        }
        x = (x * (e0 + (((e1 - e0) * (int32_t)i) >> 5))) >> 15;   /* (|x| < 2^15, e < 2^15: 32 bits) */
        if (c->drv) {                                 /* DRIVE: louder into a soft clip, the level kept */
            x = sp_clamp((x * c->drv) >> 4, -49152, 49152);
            x = sp_soft(x);
            x = x * 3 / 4;
        }
        if (fmode == 2u) {                            /* 4 one-pole stages, the 4th fed back, a soft clip in */
            int32_t in = x - ((c->z[3] * k) >> 12);
            in = sp_clamp(in, -49152, 49152);
            in = sp_soft(in);                         /* x - x^3/3 shape */
            c->z[0] += ((in - c->z[0]) * a1) >> 15;
            c->z[1] += ((c->z[0] - c->z[1]) * a1) >> 15;
            c->z[2] += ((c->z[1] - c->z[2]) * a1) >> 15;
            c->z[3] += ((c->z[2] - c->z[3]) * a1) >> 15;
            y = c->z[3];
        } else if (fmode == 1u) {
            c->z[0] += ((x - c->z[0]) * a1) >> 15;
            c->z[1] += ((c->z[0] - c->z[1]) * a1) >> 15;
            y = c->z[1];
#if MASIF_ENABLE_TRACKER
        } else if (fmode == 3u) {                     /* (z[0], z[1]: its two states, kept inside +-3 million: not
                                                       * 2^22, whose negative is a constant the build's check takes
                                                       * for a ROM address) */
            int32_t v3 = x - c->z[1];
            int32_t v1 = (int32_t)(((int64_t)sv1 * c->z[0] + (int64_t)sv2 * v3) >> 16);
            int32_t v2 = c->z[1] + (int32_t)(((int64_t)sv2 * c->z[0] + (int64_t)sv3 * v3) >> 16);
            c->z[0] = sp_clamp(2 * v1 - c->z[0], -3000000, 3000000);
            c->z[1] = sp_clamp(2 * v2 - c->z[1], -3000000, 3000000);
            y = c->flt == 1u ? v2 : c->flt == 2u ? v1 : x - (int32_t)(((int64_t)svk * v1) >> 16) - v2;
            y = sp_clamp(y, -98304, 98304);           /* (a resonance's peak: not past what the mix is made for) */
#endif
        } else {
            y = x;
        }
#if MASIF_ENABLE_TRACKER
        if (ch >= SP_NCH && c->eq_on) {               /* low | mid | high, parted by two one-poles: flat gains give y back */
            c->eqz[0] += (int32_t)(((int64_t)(y - c->eqz[0]) * sp_eq_a[0]) >> 15);
            c->eqz[1] += (int32_t)(((int64_t)(y - c->eqz[1]) * sp_eq_a[1]) >> 15);
            y = (int32_t)(((int64_t)c->eqz[0] * c->eqg[0] + (int64_t)(c->eqz[1] - c->eqz[0]) * c->eqg[1] +
                           (int64_t)(y - c->eqz[1]) * c->eqg[2]) >> 12);
            y = sp_clamp(y, -98304, 98304);
        }
#endif
        if (c->jump) {                                /* SMOOTH: go on from where it was, into the new signal */
            c->jump = 0;
            if (sp_smooth) c->dc = c->last - y;
        }
        y += c->dc;
        c->dc -= c->dc / 24;                          /* (gone in ~1 ms: e^-1 every 24 samples) */
        c->last = y;
        if (q0 | q1)                                  /* muted: the channel plays on, unheard (sends too) */
            y = (y * (4096 - (q0 + (((q1 - q0) * (int32_t)i) >> 5)))) >> 12;
        out[2u * i] += (y * gl) >> 12;
        out[2u * i + 1u] += (y * gr) >> 12;
        if (c->snd[0] | c->snd[1] | c->snd[2]) {      /* the sends: post fader, before pan. The products in 64 */
            int32_t m = (y * (gl + gr)) >> 13;        /* bits: 32 hold them up to |m| = 65538, and SMOOTH's bridge */
            sp_bus[0][i] += (int32_t)(((int64_t)m * c->snd[0]) >> 15);   /* of a full-scale cut is past that */
            sp_bus[1][i] += (int32_t)(((int64_t)m * c->snd[1]) >> 15);   /* (host-tests/test_voice_limits.c) */
            sp_bus[2][i] += (int32_t)(((int64_t)m * c->snd[2]) >> 15);
        }
    }
    c->env = env1;
    if (c->env < (1 << 12)                             /* below ~-72 dB: done */
#if MASIF_ENABLE_TRACKER
        && !(ch >= SP_NCH && c->stage == 0u)           /* (an attack starts there) */
#endif
        )
        c->on = 0;
    if (!c->on && c->tail && !--c->tail)
        c->z[0] = c->z[1] = c->z[2] = c->z[3] = 0;
}

/* the master: the Q15 mix to 16 bits at -6 dB (the full scale was too loud), with a soft knee above 3/4 so a
 * pile of hits rounds off like a mixer's bus instead of clipping hard (tanh as u (27 + u^2) / (27 + 9 u^2)) */
static inline int32_t sp_out(int32_t v)
{
    int32_t a, u, f;
    v >>= 1;
    a = v < 0 ? -v : v;
    if (a <= 24576)
        return v;
    u = (a - 24576) >> 5;                              /* Q8 above the knee, 3.0 the most */
    if (u > 768) u = 768;
    f = u * (27 * 65536 + u * u) / (27 * 65536 + 9 * u * u);
    a = 24576 + (f << 5);
    if (a > 32767) a = 32767;
    return v < 0 ? -a : a;
}

/* the MASTER knob (Q12, 4088 the most: zp12.c) on a sample of the mix, then sp_out. The product in 64 bits: 32
 * hold it only up to a mix of 525311. At full scale, full sends and the faders as set (100), all cut at once by
 * new hits (SMOOTH's bridge), eight channels stay below that (~490000); with the tracker's four voices on top
 * the mix is ~720000 (host-tests/test_voice_limits.c) */
static inline int32_t sp_master(int32_t v, int32_t q12)
{
    return sp_out((int32_t)(((int64_t)v * q12) >> 12));
}

/* the mix of all channels for one block (stereo Q15, out zeroed by the caller or added to) */
/* the metronome: a short square blip, the bar's first beat higher; not on a channel, not on the faders */
static struct { uint32_t ph, inc; int32_t env; } sp_clk;
static void sp_click(int accent)
{
    sp_clk.inc = (accent ? 1760u : 1175u) * 97391u;   /* 2^32 / 44100 = 97391.5 */
    sp_clk.ph = 0;
    sp_clk.env = 9000;
}
static void sp_click_block(int32_t *out)
{
    uint32_t i;
    if (sp_clk.env < 16)
        return;
    for (i = 0; i < SP_BLK; i++) {
        int32_t v = (sp_clk.ph & 0x80000000u) ? sp_clk.env : -sp_clk.env;
        sp_clk.ph += sp_clk.inc;
        out[2u * i] += v;
        out[2u * i + 1u] += v;
    }
    sp_clk.env = sp_clk.env * 27 / 32;          /* ~-1.5 dB a block: gone in ~25 ms */
}

#ifdef SP_WITH_FX
static void sp_fx_block(int32_t *out);         /* sp_fx.c */
#endif
static void sp_render(int32_t *out)
{
    uint32_t ch, i;
    for (i = 0; i < SP_BLK; i++)
        sp_bus[0][i] = sp_bus[1][i] = sp_bus[2][i] = 0;
    for (ch = 0; ch < SP_NVOICE; ch++)             /* the eight channels, then the tracker's voices (if built in) */
        sp_channel(ch, out);
#ifdef SP_WITH_FX
    sp_fx_block(out);
#endif
    sp_click_block(out);
}
