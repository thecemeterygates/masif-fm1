/* SPDX-License-Identifier: GPL-3.0-only */
/* MASIF host test: the limits the tracker's four voices have to keep, on the real sound core, effects, punch-in
 * and adapter (sp_core.c, sp_fx.c, sp_punch.c, masif_fm1_adapter.c), built as zp12.c's unity build includes them.
 *
 *   pitch   a tracker voice's step never exceeds SP_TRK_STEP_MAX: at the note's start, at every pitch change, and
 *           back at the note's own pitch (offset 0), which is the step the note began with (no drop). Ordinary
 *           pitches and the drums' are untouched.
 *   mix     twelve voices at full scale with the sends and MASTER at their most: the mix is past what 32 bits
 *           hold once MASTER is multiplied in (and the reverb's send bus past its own product), so both are
 *           64-bit products now (sp_master, fx_buses). Two worst cases: the faders at 127 (LOUD), and the faders
 *           as set, 100, with every ringing voice cut by a new hit at once (RETRIG: SMOOTH's bridge is the most a
 *           voice puts out, ~2.8 times full scale). The path of the audio ISR (zp12.c fm1_alnk0_irq) is done here
 *           as it is there: sp_render in blocks, sp_punch on the half, sp_master on every word.
 *   sends   a voice's send, (m * snd) >> 15 in sp_channel: m is the voice after its fader, snd up to 32766, so the
 *           product is past 32 bits once |m| > 65538, twice full scale. SMOOTH's bridge takes a voice there: a
 *           sound at full scale cut by a hit whose wave jumps back from the other end. With the fader at 127 one
 *           cut does it, on a drum channel as on a tracker voice; the product is 64 bits wide now and the send is
 *           what the exact product gives (the sends' levels, and every send that fitted before, as they were).
 *
 * To be run with the overflow sanitizer, which stops at the first signed overflow anywhere in the path:
 *   cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=signed-integer-overflow \
 *      -fno-sanitize-recover=all -o /tmp/masif_voice_limits host-tests/test_voice_limits.c && /tmp/masif_voice_limits
 * The line "DRUMS ..." is the hash of an eight-drum mix at ordinary settings: the same before and after a change
 * that is not meant to be heard. Exit 0: all ok, 1: a check failed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MASIF_ENABLE_TRACKER 1
#define SP_WITH_FX 1
#include "../firmware/src/sp_core.c"
#include "../firmware/src/sp_fx.c"
#include "../firmware/src/sp_seq.c"
#include "../firmware/src/masif_tracker.c"
#include "../firmware/src/masif_runtime.c"
#include "../firmware/src/masif_fm1_adapter.c"
#include "../firmware/src/sp_punch.c"

#define NOTE "         "
static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok      " : "FAIL    ", what);
    fails += !ok;
}

/* ---- waves: full scale held (EDGE: after a first sample at the other end, what SMOOTH bridges on a new hit), and
 * a triangle to hear a pitch by */
enum { W_FULL, W_EDGE, W_TRI, W_TRI48, W_TRI8, W_JUMP, W_N };
enum { WS_FULL, WS_EDGE, WS_TRI, WS_JUMP };            /* JUMP: EDGE the other way up (a first sample at the bottom) */
#define W_LEN 30000u
static uint8_t wave_mem[W_N][(W_LEN + 1u) / 2u * 3u];
static void vl_wave(uint32_t wi, int shape, uint32_t rate)
{
    static int16_t pcm[W_LEN];
    uint32_t i;
    for (i = 0; i < W_LEN; i++) {
        uint32_t p = i % 64u;
        int32_t v = shape == WS_FULL ? -2048 : shape == WS_EDGE ? (i ? -2048 : 2047) : shape == WS_JUMP ? (i ? 2047 : -2048)
                                    : p < 32u ? 1500 - (int32_t)p * 93 : -1500 + (int32_t)(p - 32u) * 93;
        pcm[i] = (int16_t)(v * 16);
    }
    sp_pack(wave_mem[wi], pcm, W_LEN);
    sp_wave[wi].d = wave_mem[wi];
    sp_wave[wi].n = W_LEN;
    sp_wave[wi].rate = (uint16_t)rate;
}

static void vl_sound(uint32_t k, uint32_t w, uint32_t ch, int tune, uint32_t send)
{
    sp_sound_t *s = &sp_sound[k];
    memset(s, 0, sizeof *s);
    s->wave = (uint8_t)w;
    s->tune = (int8_t)tune;
    s->decay = 127;
    s->level = 127;
    s->chan = (uint8_t)ch;
    s->end = 1000;
    s->cut = 127;
    s->send[0] = s->send[1] = s->send[2] = (uint8_t)send;
}

static void vl_reset(void)
{
    memset(sp_ch, 0, sizeof sp_ch);
    memset(sp_bus, 0, sizeof sp_bus);
    memset(&sp_clk, 0, sizeof sp_clk);
    memset(sp_mix, 100, sizeof sp_mix);
    memset(sp_trk_mix, 100, sizeof sp_trk_mix);
    sp_mute = sp_solo = 0;
    memset(&fx, 0, sizeof fx);
    memset(dly_buf, 0, sizeof dly_buf);
    memset(cho_buf, 0, sizeof cho_buf);
    memset(rev_line, 0, sizeof rev_line);
    memset(rev_ap, 0, sizeof rev_ap);
    memset(pre_buf, 0, sizeof pre_buf);
    memset(&djf, 0, sizeof djf);
    djf.res = 40;
    memset(&pn, 0, sizeof pn);
    memset(pn_ring, 0, sizeof pn_ring);
    pn.req = pn.cur = -1;
    masif_fm1_init();
}

/* ============ the pitch cap ============ */
#define TRK(t) (&sp_ch[SP_NCH + (t)])
enum { PAD_TRI = 0, PAD_TRI48 = 1, PAD_TRI8 = 2, PAD_PITCH = 3 };

static uint32_t vl_expect(uint32_t base, int32_t off)   /* the step of a pitch offset from the note's true pitch */
{
    uint64_t s;
    off = off < -48 ? -48 : off > 48 ? 48 : off;
    s = ((uint64_t)base * sp_pow2_cents(off * 100)) >> 16;
    return s > SP_TRK_STEP_MAX ? SP_TRK_STEP_MAX : (uint32_t)s;
}

static void t_pitch_high_rate(void)
{
    const uint32_t unity48 = 48000u * 65536u / SP_FS;
    const uint32_t top = (uint32_t)(((uint64_t)unity48 * sp_pow2_cents(2450)) >> 16);   /* +24.5 semitones: 4.48 */
    const sp_ch_t *v = TRK(0);
    int32_t o[2u * SP_BLK];
    uint32_t s0, p0, adv, s_up, s_back, s_m1, s_m2, s_m12, i;
    int ok = 1;
    vl_reset();
    sp_trigger_track(PAD_TRI48, 127, 60, 0);                 /* a 48 kHz sample, as far up as a note goes */
    s0 = v->step;
    check(v->on && v->base_step == top && top > SP_TRK_STEP_MAX,
          "the scenario: a 48 kHz sample at +24.5 semitones is above the cap (its true pitch, base_step, is kept)");
    check(s0 == SP_TRK_STEP_MAX, "at the note's start the step is the cap, not the true pitch above it");
    p0 = v->pos;
    memset(o, 0, sizeof o);
    sp_render(o);
    adv = v->pos - p0;
    check(adv == 4u * SP_BLK, "a block of the capped note reads 4 wave samples an output sample, no more");
    for (i = 1; i <= 127u; i++) {                            /* every pitch up, and back to the note's own each time */
        sp_pitch_track(0, (int32_t)i);
        ok &= v->step == SP_TRK_STEP_MAX;
        sp_pitch_track(0, 0);
        ok &= v->step == s0;
    }
    s_up = v->step;
    check(ok, "pitch +1..+127 stays at the cap; back at offset 0 the step is the one the note began with (no drop)");
    sp_pitch_track(0, -1);  s_m1 = v->step;
    sp_pitch_track(0, -2);  s_m2 = v->step;
    sp_pitch_track(0, -12); s_m12 = v->step;
    sp_pitch_track(0, 0);   s_back = v->step;
    check(s_m1 == SP_TRK_STEP_MAX && s_m2 < SP_TRK_STEP_MAX && s_m2 == vl_expect(top, -2) && s_m12 == vl_expect(top, -12) &&
          s_m12 > SP_TRK_STEP_MAX / 2u && s_back == s0,
          "a pitch down counts from the note's true pitch: -1 still at the cap, -2 just under it, -12 half the true pitch");
    printf(NOTE "true pitch %u.%03u, cap 4.000; start %u.%03u, +127 %u.%03u, -2 %u.%03u, -12 %u.%03u, back at 0 %u.%03u; %u wave samples in a block\n",
           (unsigned)(top >> 16), (unsigned)((top & 0xFFFFu) * 1000u >> 16), (unsigned)(s0 >> 16),
           (unsigned)((s0 & 0xFFFFu) * 1000u >> 16), (unsigned)(s_up >> 16), (unsigned)((s_up & 0xFFFFu) * 1000u >> 16),
           (unsigned)(s_m2 >> 16), (unsigned)((s_m2 & 0xFFFFu) * 1000u >> 16), (unsigned)(s_m12 >> 16),
           (unsigned)((s_m12 & 0xFFFFu) * 1000u >> 16), (unsigned)(s_back >> 16), (unsigned)((s_back & 0xFFFFu) * 1000u >> 16),
           (unsigned)adv);
}

/* every rate an own sample may have, every TUNE, every note, every pitch offset */
static void t_pitch_sweep(void)
{
    static const uint16_t rates[] = {8000, 22050, 26040, 27500, 32000, 44100, 48000};
    static const int8_t tunes[] = {-24, -12, 0, 7, 12};
    uint32_t r, t, n = 0, capped = 0, over = 0, wrong = 0, drop = 0, down = 0;
    int32_t semis, off;
    for (r = 0; r < sizeof rates / sizeof rates[0]; r++)
        for (t = 0; t < sizeof tunes; t++) {
            vl_wave(W_TRI8, WS_TRI, rates[r]);
            vl_sound(PAD_PITCH, W_TRI8, 7, tunes[t], 0);
            for (semis = -60; semis <= 120; semis += 3) {
                const sp_ch_t *v = TRK((uint32_t)(semis + 60) % SP_NTRK);
                uint32_t track = (uint32_t)(v - TRK(0)), s0, last = 0;
                vl_reset();
                sp_trigger_track(PAD_PITCH, 127, semis, track);
                s0 = v->step;
                over += s0 > SP_TRK_STEP_MAX;
                wrong += s0 != vl_expect(v->base_step, 0);
                capped += v->base_step > SP_TRK_STEP_MAX;
                for (off = -128; off <= 127; off++) {
                    sp_pitch_track(track, off);
                    over += v->step > SP_TRK_STEP_MAX;
                    wrong += v->step != vl_expect(v->base_step, off);
                    down += v->step < last;                  /* (a higher offset is never a lower pitch) */
                    last = v->step;
                    n++;
                }
                sp_pitch_track(track, 0);
                drop += v->step != s0;
            }
        }
    check(n && capped && !over, "rates 8-48 kHz, TUNE -24..+12, notes -60..+120, offsets -128..+127: the step is never above the cap");
    check(!wrong && !drop && !down,
          "... and is always the true pitch times the offset, capped: rising with the offset, back at 0 as the note began");
    printf(NOTE "%u pitch changes checked, %u notes with a true pitch above the cap\n", (unsigned)n, (unsigned)capped);
    vl_wave(W_TRI8, WS_TRI, 8000);
}

/* an ordinary note: the tracker voice is, sample for sample, a pad on channel 8 (no filter either) playing it */
static void t_pitch_ordinary(void)
{
    static int32_t a[64u * 2u * SP_BLK], b[64u * 2u * SP_BLK];
    static const int8_t notes[] = {-24, -7, 0, 5, 12, 19, 24};
    uint32_t i, k, same = 1, steps = 1, heard = 0;
    for (k = 0; k < sizeof notes; k++) {
        vl_reset();
        vl_sound(PAD_TRI, W_TRI, 7, 0, 0);
        memset(a, 0, sizeof a);
        sp_trigger_at(PAD_TRI, 127, notes[k]);
        for (i = 0; i < 64u; i++) {
            if (i == 20u) sp_ch[7].step = (uint32_t)(((uint64_t)sp_ch[7].base_step * sp_pow2_cents(700)) >> 16);
            if (i == 40u) sp_ch[7].step = sp_ch[7].base_step;
            sp_render(a + i * 2u * SP_BLK);
        }
        vl_reset();
        memset(b, 0, sizeof b);
        sp_trigger_track(PAD_TRI, 127, notes[k], 2);
        steps &= TRK(2)->step == TRK(2)->base_step && TRK(2)->step < SP_TRK_STEP_MAX;
        for (i = 0; i < 64u; i++) {
            if (i == 20u) sp_pitch_track(2, 7);              /* a slide up a fifth, and back */
            if (i == 40u) sp_pitch_track(2, 0);
            sp_render(b + i * 2u * SP_BLK);
        }
        same &= !memcmp(a, b, sizeof a);
        for (i = 0; i < sizeof a / sizeof a[0]; i++) heard |= a[i] != 0;
    }
    check(heard && steps && same,
          "ordinary notes (-24..+24, a slide of +7 and back): uncapped, sample for sample what a pad on channel 8 plays");
    vl_reset();
    vl_sound(PAD_TRI48, W_TRI48, 7, 12, 0);
    sp_trigger_at(PAD_TRI48, 127, 60);
    check(sp_ch[7].on && sp_ch[7].step == sp_ch[7].base_step && sp_ch[7].step > SP_TRK_STEP_MAX && !TRK(0)->on,
          "a pad's pitch is as before: the same 48 kHz sample on a drum channel is not capped");
}

/* through the adapter, as the runtime's events come: note on (with a pitch offset or without), pitch, retrigger */
static void t_pitch_adapter(void)
{
    masif_audio_event_t e;
    uint32_t t, s_on, s_off, s_ret;
    int ok = 1;
    vl_reset();
    for (t = 0; t < SP_NTRK; t++) {
        memset(&e, 0, sizeof e);
        masif_song.instrument[PAD_TRI48].sample = W_TRI48;      /* (an instrument is a wave, not a pad) */
        masif_song.instrument[PAD_TRI48].root_note = 1;
        e.track = (uint8_t)t; e.instrument = PAD_TRI48; e.note = 120; e.velocity = 127;
        e.type = MASIF_EVENT_NOTE_ON;
        masif_audio_event(0, &e);
        s_on = TRK(t)->step;
        ok &= TRK(t)->on && TRK(t)->base_step > SP_TRK_STEP_MAX && s_on == SP_TRK_STEP_MAX;
        e.type = MASIF_EVENT_PITCH; e.pitch_offset = 127 * MASIF_PITCH_SEMI;
        masif_audio_event(0, &e);
        ok &= TRK(t)->step == SP_TRK_STEP_MAX;
        e.pitch_offset = 0;
        masif_audio_event(0, &e);
        s_off = TRK(t)->step;
        e.type = MASIF_EVENT_RETRIGGER;
        masif_audio_event(0, &e);
        s_ret = TRK(t)->step;
        e.type = MASIF_EVENT_NOTE_ON; e.pitch_offset = 12 * MASIF_PITCH_SEMI;
        masif_audio_event(0, &e);
        ok &= s_off == s_on && s_ret == s_on && TRK(t)->step == SP_TRK_STEP_MAX;
    }
    check(ok, "the adapter's events on tracks 1-4 (note on, pitch +127, pitch 0, retrigger, note on with an offset): capped alike, no drop");
}

/* ============ the mix and MASTER ============ */
#define MASTER_MAX 4088                             /* zp12.c: (k10 * k10) >> 8 for the knob's 10 bits, 1023 */
#define HALF 128u                                   /* the ISR's half: 4 blocks */
typedef struct {
    int32_t peak;                                   /* the mix at its most, before MASTER */
    uint32_t n, past32, bad, differ, sat;           /* words; whose product is past 32 bits; out of range; not as before */
    uint32_t hash;
} vl_mix_t;

/* what the ISR's line was before sp_master, where 32 bits held its product (fits) */
static int32_t vl_old_master(int32_t v, int32_t q12, int *fits)
{
    int64_t p = (int64_t)v * q12;
    *fits = p >= INT32_MIN && p <= INT32_MAX;
    return *fits ? sp_out((int32_t)p >> 12) : 0;
}

static void vl_half(int32_t q12, vl_mix_t *m)       /* fm1_alnk0_irq, less the sequencer and the tracker's clock */
{
    int32_t o[2u * HALF];
    uint32_t i, b;
    memset(o, 0, sizeof o);
    for (b = 0; b < HALF; b += SP_BLK)
        sp_render(o + 2u * b);
    sp_punch(o, HALF);
    for (i = 0; i < 2u * HALF; i++) {
        int fits;
        int32_t a = o[i] < 0 ? -o[i] : o[i], y = sp_master(o[i], q12), old = vl_old_master(o[i], q12, &fits), w = y << 8;
        if (a > m->peak) m->peak = a;
        m->n++;
        m->past32 += !fits;
        m->bad += y < -32767 || y > 32767 || (y && (y < 0) != (o[i] < 0));
        m->differ += fits && y != old;
        m->sat += !fits && y != (o[i] < 0 ? -32767 : 32767);
        m->hash = (m->hash ^ (uint32_t)w) * 16777619u;
    }
}

/* LOUD: full scale held, the faders at 127. RETRIG: the faders as set (100), every voice cut by a new hit at once
 * when the delay is full. Both: the sends, the FX pages and MASTER at their most */
enum { M_LOUD = 1, M_TRACKER = 2, M_DJF = 4, M_PUNCH = 8, M_TRK127 = 16, M_RETRIG = 32 };
static vl_mix_t vl_mix(unsigned how)
{
    vl_mix_t m;
    uint32_t h, k;
    int loud = (how & (M_LOUD | M_RETRIG)) != 0;
    memset(&m, 0, sizeof m);
    m.hash = 2166136261u;
    vl_reset();
    for (k = 0; k < SP_NVOICE; k++)                  /* pads 1-8 on channels 1-8, four more for the tracks */
        vl_sound(8u + k, how & M_RETRIG ? W_EDGE : loud ? W_FULL : W_TRI, k % SP_NCH, 0, loud ? 127 : 40);
    if (loud) {
        if (how & M_LOUD) memset(sp_mix, 127, sizeof sp_mix);
        fxp.cmix = 127; fxp.dmix = 127; fxp.fdbk = 127; fxp.colr = 127; fxp.size = 127; fxp.damp = 0; fxp.pre = 0;
    } else {
        fxp.cmix = 127; fxp.dmix = 90; fxp.fdbk = 60; fxp.colr = 70; fxp.size = 90; fxp.damp = 60; fxp.pre = 0;
    }
    if (how & M_TRK127) memset(sp_trk_mix, 127, sizeof sp_trk_mix);
    for (h = 0; h < 300u; h++) {                     /* 0.87 s: the delay and the reverb are full */
        if (!h || (how & M_RETRIG && h == 130u)) {
            for (k = 0; k < SP_NCH; k++) sp_trigger(8u + k, 127);
            if (how & M_TRACKER)
                for (k = 0; k < SP_NTRK; k++) sp_trigger_track(8u + SP_NCH + k, 127, 0, k);
        }
        if (loud && h % 16u == 0u) sp_click(1);
        if (how & M_DJF) djf.v = (int8_t)(h < 150u ? -40 : 40);
        if (how & M_PUNCH) pn.req = (int8_t)(h < 100u ? -1 : h < 160u ? PN_ROLL16 : h < 220u ? PN_REV : PN_STOP);
        vl_half(loud ? MASTER_MAX : 2048, &m);
    }
    return m;
}

static void t_master(void)
{
    static const int32_t qs[] = {0, 1, 2048, 4087, MASTER_MAX, 4095, 4096};
    static const int32_t vs[] = {0, 1, 49152, 65536, 525311, 525312, 525313, 572000, 625000, 1000000, 16777216,
                                 INT32_MAX - 1, INT32_MAX};
    uint32_t i, k, bad = 0, differ = 0, past = 0, sat = 0, n = 0, down = 0;
    int32_t v, last = -32768;
    for (i = 0; i < sizeof qs / sizeof qs[0]; i++)
        for (k = 0; k < 2u * sizeof vs / sizeof vs[0]; k++) {
            int fits;
            int32_t x = k & 1u ? -vs[k / 2u] - (vs[k / 2u] == INT32_MAX) : vs[k / 2u];   /* (INT32_MIN too) */
            int32_t y = sp_master(x, qs[i]), old = vl_old_master(x, qs[i], &fits);
            bad += y < -32767 || y > 32767 || (y && (y < 0) != (x < 0));
            differ += fits && y != old;
            past += !fits;
            sat += !fits && qs[i] >= 4087 && y != (x < 0 ? -32767 : 32767);
            n++;
        }
    check(n && past && !bad && !differ && !sat,
          "sp_master, MASTER 0..4096, a mix up to all 32 bits: within 16 bits, the sign kept, as the 32-bit line where that held");
    for (v = -700000; v <= 700000; v += 7) {         /* (the whole range a mix can have, and the knee) */
        int32_t y = sp_master(v, MASTER_MAX);
        down += y < last;
        last = y;
    }
    check(!down && sp_master(525312, MASTER_MAX) == 32767 && sp_master(-525312, MASTER_MAX) == -32767,
          "sp_master at MASTER's most rises with the mix through +-700000 (no wrap where 32 bits ended, 525312)");
    printf(NOTE "%u values, %u of them past 32 bits as (mix * MASTER)\n", (unsigned)n, (unsigned)past);
}

static void t_mix_headroom(void)
{
    vl_mix_t d = vl_mix(M_LOUD), t = vl_mix(M_LOUD | M_TRACKER), t127 = vl_mix(M_LOUD | M_TRACKER | M_TRK127);
    vl_mix_t rd = vl_mix(M_RETRIG), rt = vl_mix(M_RETRIG | M_TRACKER);
    vl_mix_t f = vl_mix(M_RETRIG | M_TRACKER | M_DJF), p = vl_mix(M_RETRIG | M_TRACKER | M_PUNCH), q;
    printf(NOTE "the mix at its most (before MASTER; 32 bits hold MASTER %d up to 525311), words past 32 bits of %u:\n",
           MASTER_MAX, (unsigned)d.n);
    printf(NOTE "  LOUD    eight drums                        %7d  %6u\n", (int)d.peak, (unsigned)d.past32);
    printf(NOTE "  LOUD    + four tracker voices (fader 100)  %7d  %6u\n", (int)t.peak, (unsigned)t.past32);
    printf(NOTE "  LOUD    + four tracker voices (fader 127)  %7d  %6u\n", (int)t127.peak, (unsigned)t127.past32);
    printf(NOTE "  RETRIG  eight drums                        %7d  %6u\n", (int)rd.peak, (unsigned)rd.past32);
    printf(NOTE "  RETRIG  + four tracker voices (fader 100)  %7d  %6u\n", (int)rt.peak, (unsigned)rt.past32);
    printf(NOTE "  RETRIG  ... with the DJ filter swept       %7d  %6u\n", (int)f.peak, (unsigned)f.past32);
    printf(NOTE "  RETRIG  ... with ROLL, REVERSE, TAPE STOP  %7d  %6u\n", (int)p.peak, (unsigned)p.past32);
    check(d.peak > 0 && !d.past32 && !d.differ && !d.bad && rd.peak > 0 && !rd.past32 && !rd.differ && !rd.bad,
          "eight drums, LOUD and RETRIG: 32 bits held MASTER's product, and its output is word for word the 32-bit line's");
    check(rt.past32 > 0u && (int64_t)rt.peak * MASTER_MAX > INT32_MAX && !t.past32 && t127.past32 > 0u,
          "the scenario: with the tracker's four voices (fader 100) RETRIG is past 32 bits: the old line overflowed here");
    check(!t.bad && !t.differ && !t.sat && !t127.bad && !t127.differ && !t127.sat && !rt.bad && !rt.differ && !rt.sat,
          "twelve voices: MASTER's output within 16 bits, full scale where 32 bits ended, unchanged below");
    check(f.peak > 0 && !f.bad && !f.differ && !f.sat && p.peak > 0 && !p.bad && !p.differ && !p.sat,
          "twelve voices through the DJ filter and the punch-ins: the same");
    q = vl_mix(0);
    check(q.peak > 0 && !q.past32 && !q.bad && !q.differ, "eight drums at ordinary settings: far from it, as before");
    printf("DRUMS    %08x  (eight drums, faders 100, sends 40, MASTER 2048: peak %d)\n", (unsigned)q.hash, (int)q.peak);
    printf("DRUMSMAX %08x %08x  (eight drums, LOUD and RETRIG)\n", (unsigned)d.hash, (unsigned)rd.hash);
}

/* ============ the sends ============ */
/* one voice (no filter: channel 8 or a tracker voice) plays W_JUMP at its own rate, a step of 1: its first sample
 * at the bottom, then the top held. Every block from the second on begins with a new hit that cuts it: SMOOTH
 * goes on from the top, and the wave's second sample is the top again on what is left of the bridge, ~2.9 times
 * full scale. The model here is sp_channel's for such a voice (no decay, no drive, no filter), its sends in 64 bits */
enum { PAD_JUMP = 4 };
typedef struct {
    uint32_t n, past32, wrong, wrong_fit, out_bad, over;   /* sends; past 32 bits as (m * snd); not the exact one; ... */
    int32_t peak_m, peak_bus;
} vl_send_t;

static vl_send_t vl_sends(int tracker, uint32_t fader, const uint8_t *send, uint32_t cuts)
{
    vl_send_t r;
    sp_ch_t *c = tracker ? TRK(1) : &sp_ch[7];
    int32_t o[2u * SP_BLK], last = 0, dc = 0;
    uint32_t b, i, k;
    memset(&r, 0, sizeof r);
    vl_reset();
    vl_sound(PAD_JUMP, W_JUMP, 7, 0, 0);
    for (k = 0; k < 3u; k++) sp_sound[PAD_JUMP].send[k] = send[k];
    memset(sp_mix, (int)fader, sizeof sp_mix);
    memset(sp_trk_mix, (int)fader, sizeof sp_trk_mix);
    for (b = 0; b <= cuts; b++) {
        int32_t gl, gr;
        if (tracker) sp_trigger_track(PAD_JUMP, 127, 0, 1);
        else sp_trigger(PAD_JUMP, 127);
        gl = c->gl * (int32_t)fader * (int32_t)fader / 10000;
        gr = c->gr * (int32_t)fader * (int32_t)fader / 10000;
        memset(o, 0, sizeof o);
        sp_render(o);                               /* (sp_bus keeps the block's sends: the effects only read them) */
        for (i = 0; i < SP_BLK; i++) {
            int32_t x = (i ? 2047 : -2048) * 16, y, m;
            if (b && !i) dc = last - x;             /* the cut: SMOOTH's bridge */
            y = x + dc;
            dc -= dc / 24;
            last = y;
            m = (int32_t)(((int64_t)y * (gl + gr)) >> 13);
            if ((m < 0 ? -m : m) > r.peak_m) r.peak_m = m < 0 ? -m : m;
            for (k = 0; k < 3u; k++) {
                int64_t p = (int64_t)m * c->snd[k];
                int32_t want = (int32_t)(p >> 15), got = sp_bus[k][i], fits = p >= INT32_MIN && p <= INT32_MAX;
                r.n++;
                r.past32 += !fits;
                r.wrong += got != want;
                r.wrong_fit += fits && got != want; /* (where 32 bits held the product: the send as it ever was) */
                r.over += (got < 0 ? -got : got) > (m < 0 ? -m : m) || (got && (got < 0) != (m < 0));
                if ((got < 0 ? -got : got) > r.peak_bus) r.peak_bus = got < 0 ? -got : got;
            }
        }
        r.out_bad += c->last != last || c->dc != dc || c->step != 65536u || !c->on;
    }
    return r;
}

static void t_sends(void)
{
    static const uint8_t full[3] = {127, 127, 127}, mixed[3] = {127, 64, 1}, low[3] = {40, 40, 40};
    vl_send_t d = vl_sends(0, 127, full, 1), t = vl_sends(1, 127, full, 1), dm = vl_sends(0, 127, mixed, 1);
    vl_send_t d100 = vl_sends(0, 100, full, 1), r100 = vl_sends(0, 100, full, 40), r127 = vl_sends(1, 127, full, 40);
    vl_send_t q = vl_sends(0, 100, low, 40), one = vl_sends(0, 127, full, 0);
    printf(NOTE "a voice after its fader (m) at its most, its send at its most, sends past 32 bits as (m * snd) of %u:\n",
           (unsigned)d.n);
    printf(NOTE "  drum channel 8, fader 127, no cut        %7d %7d  %4u\n", (int)one.peak_m, (int)one.peak_bus, (unsigned)one.past32);
    printf(NOTE "  drum channel 8, fader 127, one cut       %7d %7d  %4u\n", (int)d.peak_m, (int)d.peak_bus, (unsigned)d.past32);
    printf(NOTE "  tracker voice,  fader 127, one cut       %7d %7d  %4u\n", (int)t.peak_m, (int)t.peak_bus, (unsigned)t.past32);
    printf(NOTE "  drum channel 8, fader 100, one cut       %7d %7d  %4u\n", (int)d100.peak_m, (int)d100.peak_bus, (unsigned)d100.past32);
    printf(NOTE "  drum channel 8, fader 100, a cut a block %7d %7d  %4u  (of %u)\n", (int)r100.peak_m, (int)r100.peak_bus,
           (unsigned)r100.past32, (unsigned)r100.n);
    printf(NOTE "  tracker voice,  fader 127, a cut a block %7d %7d  %4u  (of %u)\n", (int)r127.peak_m, (int)r127.peak_bus,
           (unsigned)r127.past32, (unsigned)r127.n);
    check(!d.out_bad && !t.out_bad && !dm.out_bad && !d100.out_bad && !r100.out_bad && !r127.out_bad && !q.out_bad && !one.out_bad,
          "the scenario's model is the voice: its last output, SMOOTH's bridge and its step after every block");
    check(d.past32 > 0u && t.past32 > 0u && (int64_t)d.peak_m * 32766 > INT32_MAX && (int64_t)t.peak_m * 32766 > INT32_MAX &&
          !one.past32,
          "the scenario: fader 127, sends 127, a full-scale sound cut by a hit: (m * snd) is past 32 bits, on a drum channel and a tracker voice");
    check(!d.wrong && !t.wrong && !dm.wrong && !r100.wrong && !r127.wrong && !d.over && !t.over && !dm.over && !r100.over && !r127.over,
          "... and every send is the exact product's, never above the voice itself, the sign kept (sends 127 / 64 / 1 too, a cut a block too)");
    check(dm.past32 > 0u && dm.past32 < d.past32, "sends 127 / 64 / 1: only the full send's product is past 32 bits");
    check(!one.wrong_fit && !d100.wrong_fit && !q.wrong_fit && !d100.wrong && !q.wrong && !one.wrong && !q.past32 && q.peak_bus > 0,
          "where 32 bits held the product (no cut; the fader as set; sends 40 with a cut a block) the send is as before");
}

int main(void)
{
    vl_wave(W_FULL, WS_FULL, 26040);
    vl_wave(W_EDGE, WS_EDGE, 26040);
    vl_wave(W_TRI, WS_TRI, 26040);
    vl_wave(W_TRI48, WS_TRI, 48000);
    vl_wave(W_TRI8, WS_TRI, 8000);
    vl_wave(W_JUMP, WS_JUMP, SP_FS);
    vl_sound(PAD_TRI, W_TRI, 7, 0, 0);
    vl_sound(PAD_TRI48, W_TRI48, 7, 12, 0);
    vl_sound(PAD_TRI8, W_TRI8, 7, 0, 0);
    sq_init();

    printf("MASIF voice limits: sp_core + sp_fx + sp_punch + masif_fm1_adapter, MASIF_ENABLE_TRACKER=1\n");
    printf("\n-- the tracker's pitch cap (SP_TRK_STEP_MAX %u.0)\n", (unsigned)(SP_TRK_STEP_MAX >> 16));
    t_pitch_high_rate();
    t_pitch_sweep();
    t_pitch_ordinary();
    t_pitch_adapter();
    printf("\n-- the mix of twelve voices and MASTER\n");
    t_master();
    printf("\n-- a voice's sends\n");
    t_sends();
    t_mix_headroom();
    printf("\nresult:  %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
