/* SPDX-License-Identifier: GPL-3.0-only */
/* MASIF's sample pool kept by the FM-1 itself (2026-10-23: the web app only for installing, updating, backups and
 * SYNC POOL). Included after sp_samples.c (zu_dir, zu_load) and sp_link.c.
 * - The directory written as the web app writes it (web/zp12link.js writeDir / buildDir): into the older of the two
 *   copies, its generation one up, the samples' loops right after it; then read again (zu_load). Only the slots the
 *   last load took are written back (one in the app area without its marker is gone, as readDir drops it).
 * - The free room found as the web app finds it (freeRuns): whole sectors no slot uses, in the rooms zu_room allows.
 * - The recorder: the computer's audio (USB, uo.h) or MASIF's own mix (a bounce) into a new sample. As Ketch's REC
 *   (sound.c): nothing is programmed while the take runs. msfo programmed 32-byte pieces once the ring was half
 *   full, and on the device (2026-10-23) the sound and the take went to crackle from then on (IRQs off for each
 *   program). So a take is what the 128 KiB ring in the pool holds (124 KiB: 3.8 s at 22 kHz 12 bit, 5.7 s at 8 bit,
 *   twice that at 11 kHz, 10.6 s in ADPCM): mono, 44.1 / 22.05 / 11.025 kHz, 12 bit, 8 bit or 4-bit ADPCM as the pool
 *   keeps samples. ARM starts it at once; once it has ended the sectors it uses are erased and then programmed a
 *   page at a time, the sound faded out for the whole save (MR_HUSH: each erase stops the interrupts for ~45 ms and
 *   each page for a while, and the I2S looped its last half: msft buzzed at ARM, 2026-10-23; msfu, which faded only
 *   the erases and programmed after an audio block and a USB packet, still crackled and blew out the computer's sound
 *   through the programs, 2026-10-24). Nothing written may look like an update record:
 *   the byte at 0xF07 of a sector is changed by its lowest bit where 0xF06 / 0xF07 would read "AT".
 * The audio interrupt calls mr_usb (uo.h's uo_rec) and mr_mix (the mix before MASTER); the main loop mr_service. */
#include "masif_rec.h"
#define MR_RING 0x20000u                         /* the take's ring (a power of two: no division in the interrupt) */
#define MR_TAKE_MAX (MR_RING - 0x1000u)          /* a take's bytes at the most (whole sectors, inside the ring) */
#define MR_PRE_MS 20u                            /* the take starts this long before the sound that set it off */
#define MR_FS 44100u                             /* the mix's rate (and the computer's: usb.c offers 44.1 kHz only) */
#define MR_NONE 0xFFFFFFFFu
#ifndef MR_HUSH
#define MR_HUSH(on) ((void)(on))                 /* (zp12.c: the sound faded out / in, */
#define MR_HUSHED() 1                            /* and out: silence in both I2S halves) */
#endif
static uint8_t mr_buf[MR_RING] __attribute__((section(".pool")));
static struct {
    volatile uint8_t state;
    uint8_t src, start, thr, sh, bits;           /* the take's settings (sh: 0 44.1 kHz, 1 22.05, 2 11.025; MR_B*) */
    uint8_t slot, why, pad, cnt;                 /* the slot it goes into; why it failed; 12 bit: a 0 after an odd last */
    volatile uint32_t w;                         /* bytes made since the arm (the ring's write side) */
    volatile uint32_t s0;                        /* the take's first byte in it (MR_NONE: not started) */
    uint32_t r;                                  /* the take's bytes programmed */
    uint32_t at, cap, er;                        /* the flash: where, the bytes it may fill, the bytes erased */
    int32_t acc;                                 /* the samples summed for one (2 ^ sh of them) */
    int16_t odd;                                 /* 12 bit: the pair's first, while has_odd */
    uint8_t has_odd;
    volatile uint32_t peak;                      /* the input's level since the meter looked (16 bit) */
    uint32_t wmax;                               /* the longest program (us) */
    uint32_t gaps0, gaps;                        /* USB: frames lost (uo.h) at the arm, and while it ran */
    volatile uint8_t listen;                     /* the REC box is open: the meter shows the source */
    int32_t apred, aidx;                         /* ADPCM: the encoder's state, */
    uint8_t ak, anib;                            /* the block's sample now and a code waiting for its pair, */
    uint32_t ns, blk0;                           /* the samples made since the arm, the block's first byte */
    uint8_t flushed;                             /* SAVING: the last part filled in */
    char name[8];
} mr;

static uint32_t mr_wave(uint32_t slot) { return KIT_NWAVE + slot; }
static int mr_has(uint32_t slot) { return slot < SP_NUSER && sp_wave[mr_wave(slot)].n != 0u; }
static uint32_t mr_bytes(uint32_t n, uint32_t bits) { return bits == MR_B8 ? n : bits == MR_B4 ? sp_adpcm_bytes(n) : (n + 1u) / 2u * 3u; }
static uint32_t mr_samples(uint32_t bytes, uint32_t bits)   /* (as many as these bytes hold) */
{
    uint32_t rem = bytes % SP_ADBYTES;
    if (bits == MR_B8) return bytes;
    if (bits == MR_B4) return bytes / SP_ADBYTES * SP_ADB + (rem > 4u ? (rem - 4u) * 2u : 0u);
    return bytes / 3u * 2u;
}
static uint32_t mr_bits_of(uint32_t flags) { return flags & 8u ? MR_B4 : flags & 4u ? MR_B8 : MR_B12; }

/* ---- the directory */
/* slot k becomes s with its loop l (s 0: empty); the others as the last load took them. 0: written and read back */
static int mr_dir_put(uint32_t k, const zu_slot_t *s, const zu_loop_t *l)
{
    static zu_dir_t d;                           /* (static: 800 bytes not on the stack) */
    static zu_loops_t lp;
    uint32_t i, c, a, took, gen;
    for (i = 0; i < sizeof d; i++) ((uint8_t *)&d)[i] = 0;
    for (i = 0; i < sizeof lp; i++) ((uint8_t *)&lp)[i] = 0;
    for (i = 0; i < SP_NUSER; i++) {
        const sp_wave_t *w = &sp_wave[mr_wave(i)];
        if (i == k) {
            if (s) { d.s[i] = *s; if (l) lp.s[i] = *l; }
        } else if (mr_has(i)) {
            d.s[i] = zu_dir.s[i];
            lp.s[i].a = w->lp_a; lp.s[i].b = w->lp_b; lp.s[i].play = w->lp_play;
            lp.s[i].rsv[0] = w->lp_tin; lp.s[i].rsv[1] = w->lp_tout;
        }
    }
    gen = zu_dir.magic == ZU_MAGIC ? zu_dir.gen + 1u : 1u;
    c = zu_dir.magic == ZU_MAGIC ? zu_copy ^ 1u : 0u;   /* (the newer copy stays good until this one is) */
    d.magic = ZU_MAGIC; d.gen = gen; d.flags = 0;
    d.crc = zs_crc((const uint8_t *)d.s, sizeof d.s);
    lp.magic = ZU_LP_MAGIC;
    lp.crc = zs_crc((const uint8_t *)lp.s, sizeof lp.s);
    a = ZU_DIR0 + c * 0x1000u;
    fm1_wdt_feed();
    if (fl_erase4k_quiet(a, &took) || fl_write(a, (const uint8_t *)&d, sizeof d) ||
        fl_write(a + sizeof d, (const uint8_t *)&lp, sizeof lp))
        return -1;
    zu_load();
    return zu_dir.magic == ZU_MAGIC && zu_dir.gen == gen ? 0 : -1;
}
static int mr_rename(uint32_t k, const char *name)
{
    zu_slot_t s;
    zu_loop_t l;
    const sp_wave_t *w = &sp_wave[mr_wave(k)];
    uint32_t i;
    if (!mr_has(k)) return -1;
    s = zu_dir.s[k];
    for (i = 0; i < 8u; i++) s.name[i] = name[i];
    l.a = w->lp_a; l.b = w->lp_b; l.play = w->lp_play; l.rsv[0] = w->lp_tin; l.rsv[1] = w->lp_tout; l.rsv[2] = 0;
    return mr_dir_put(k, &s, &l);
}
static int mr_delete(uint32_t k) { return mr_has(k) ? mr_dir_put(k, 0, 0) : -1; }

/* ---- the room: the rooms zu_room allows (the app area's whether or not its marker is there yet: it goes in before
 * the first erase there), whole sectors no slot that played uses */
static uint32_t mr_rooms(uint32_t lo[3], uint32_t hi[3])
{
    uint32_t n = 2;
    lo[0] = 0x93000u; hi[0] = 0xC4000u;
    lo[1] = 0xEA000u; hi[1] = 0xFC000u;
#if MASIF_APP_ROOM
    if (masif_app_lo) { lo[2] = masif_app_lo + 0x1000u; hi[2] = MASIF_APP_HI; n = 3; }
#endif
    return n;
}
static int mr_used(uint32_t a)                   /* the sector at a */
{
    uint32_t i;
    for (i = 0; i < SP_NUSER; i++) {
        uint32_t off = zu_dir.s[i].off, b;
        if (!mr_has(i)) continue;
        b = mr_bytes(zu_dir.s[i].n, mr_bits_of(zu_dir.s[i].flags));
        if (a + 0x1000u > off && a < off + b) return 1;
    }
    return 0;
}
/* the free runs: bytes in all, the longest run's start and bytes */
static uint32_t mr_free(uint32_t *at, uint32_t *longest)
{
    static uint32_t gen_seen = MR_NONE, all, best_at, best;
    uint32_t lo[3], hi[3], n = mr_rooms(lo, hi), r, a, run_at = 0, run = 0;
    if (zu_dir.gen != gen_seen || zu_dir.magic != ZU_MAGIC) {   /* (worked out again when the directory changed) */
        gen_seen = zu_dir.magic == ZU_MAGIC ? zu_dir.gen : MR_NONE;
        all = best = best_at = 0;
        for (r = 0; r < n; r++) {
            for (a = lo[r]; a <= hi[r]; a += 0x1000u) {
                int free = a < hi[r] && !mr_used(a);
                if (free) { if (!run) run_at = a; run += 0x1000u; all += 0x1000u; continue; }
                if (run > best) { best = run; best_at = run_at; }
                run = 0;
            }
        }
    }
    if (at) *at = best_at;
    if (longest) *longest = best;
    return all;
}

/* ---- the recorder: the audio interrupt's side */
/* one sample of the take (16 bit, mono, at the take's rate) into the ring */
static void mr_put(int32_t v)
{
    uint32_t w = mr.w;
    if (mr.bits == MR_B4) {                      /* ADPCM: a block's header, then a code (two to a byte) */
        int32_t st = SP_ADSTEP[mr.aidx], d;
        uint32_t c = 0;
        if (!mr.ak) {
            mr.blk0 = w;
            mr_buf[w & (MR_RING - 1u)] = (uint8_t)mr.apred;
            mr_buf[(w + 1u) & (MR_RING - 1u)] = (uint8_t)((uint32_t)mr.apred >> 8);
            mr_buf[(w + 2u) & (MR_RING - 1u)] = (uint8_t)mr.aidx;
            mr_buf[(w + 3u) & (MR_RING - 1u)] = 0;
            w += 4u;
        }
        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        d = v - mr.apred;
        if (d < 0) { c = 8u; d = -d; }
        if (d >= st) { c |= 4u; d -= st; }
        if (d >= st >> 1) { c |= 2u; d -= st >> 1; }
        if (d >= st >> 2) c |= 1u;
        sp_adpcm_step(&mr.apred, &mr.aidx, c);   /* (the decoder's own steps: it stays where the player will be) */
        if (!(mr.ak & 1u)) mr.anib = (uint8_t)c;
        else mr_buf[w++ & (MR_RING - 1u)] = (uint8_t)(mr.anib | c << 4);
        mr.ak = (uint8_t)((mr.ak + 1u) & (SP_ADB - 1u));
        mr.ns++;
        mr.w = w;
        return;
    }
    if (mr.bits == MR_B8) {
        v = (v + 128) >> 8;
        mr_buf[w & (MR_RING - 1u)] = (uint8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
        mr.w = w + 1u;
        return;
    }
    v = (v + 8) >> 4;
    v = v > 2047 ? 2047 : v < -2048 ? -2048 : v;
    if (!mr.has_odd) { mr.odd = (int16_t)v; mr.has_odd = 1; return; }
    {
        uint32_t a = (uint32_t)mr.odd & 0xFFFu, b = (uint32_t)v & 0xFFFu;
        mr_buf[w & (MR_RING - 1u)] = (uint8_t)a;
        mr_buf[(w + 1u) & (MR_RING - 1u)] = (uint8_t)((a >> 8) | ((b << 4) & 0xF0u));
        mr_buf[(w + 2u) & (MR_RING - 1u)] = (uint8_t)(b >> 4);
        mr.w = w + 3u;
        mr.has_odd = 0;
    }
}
/* one frame of the input (16 bit, mono, 44.1 kHz) */
static void mr_frame(int32_t m)
{
    uint32_t a = (uint32_t)(m < 0 ? -m : m), st = mr.state, pre;
    int took = 0;
    if (a > mr.peak) mr.peak = a;
    if (st != MR_ARMED && st != MR_TAKE) return;
    mr.acc += m;
    if (++mr.cnt < 1u << mr.sh) return;
    m = mr.acc >> mr.sh;
    mr.acc = 0; mr.cnt = 0;
    if (st == MR_ARMED) {                        /* the ring runs on (the pre-roll) until the take starts */
        if (!(mr.start == MR_NOW || (mr.start == MR_PLAY && sq.playing) ||
              (mr.start == MR_LEVEL && (m < 0 ? -m : m) > (int32_t)MR_THR[mr.thr % MR_NTHR]))) {
            mr_put(m);
            return;
        }
        if (mr.bits == MR_B12 && mr.has_odd) { mr_put(m); took = 1; }   /* (12 bit: the take starts on a whole pair) */
        pre = mr.start == MR_LEVEL ? ((MR_FS >> mr.sh) * MR_PRE_MS / 1000u) & ~1u : 0u;
        if (mr.bits == MR_B4) {                  /* (ADPCM: on a block's start, the pre-roll in whole blocks) */
            uint32_t at = mr.ak ? mr.blk0 : mr.w;
            pre = (pre + SP_ADB - 1u) / SP_ADB * SP_ADBYTES;
            mr.s0 = at > pre ? at - pre : 0u;
        } else {
            pre = mr_bytes(pre, mr.bits);
            mr.s0 = mr.w > pre ? mr.w - pre : 0u;
        }
        mr.state = MR_TAKE;
        if (took) return;
    } else if (mr.start == MR_PLAY && !sq.playing) {   /* the song stopped: so does the take */
        mr.state = MR_SAVING;
        return;
    }
    if (mr.w - mr.s0 + (mr.bits == MR_B4 ? 5u : mr.bits == MR_B8 ? 1u : mr.has_odd ? 3u : 0u) > mr.cap) {   /* (the bytes this one adds: none past the room) */
        mr.state = MR_SAVING; mr.why = MR_WHY_FULL;
        return;
    }
    if (mr.w - mr.s0 - mr.r >= MR_RING - 4u) { mr.state = MR_FAILED; mr.why = MR_WHY_SLOW; return; }
    mr_put(m);
}
#if FELUCCA_UAC_OUT
static void mr_usb(uint32_t at, uint32_t n)       /* uo.h: n ring frames from at, as they are played */
{
    uint32_t i;
    if (mr.src != MR_USB || (!mr.listen && mr.state != MR_ARMED && mr.state != MR_TAKE)) return;
    for (i = 0; i < n; i++) {
        uint32_t f = uo_ring[(at + i) % UO_N];
        mr_frame(((int32_t)(int16_t)f + (int16_t)(f >> 16)) >> 1);
    }
}
#endif
static void mr_mix(const int32_t *o, uint32_t frames)   /* the mix before MASTER, as the computer gets it */
{
    uint32_t i;
    if (mr.src != MR_MIX || (!mr.listen && mr.state != MR_ARMED && mr.state != MR_TAKE)) return;
    for (i = 0; i < frames; i++) mr_frame((sp_out(o[2u * i]) + sp_out(o[2u * i + 1u])) >> 1);
}

/* ---- the recorder: the main loop's side */
static uint32_t mr_gapcount(void)
{
#if FELUCCA_UAC_OUT
    return uo_over + uo_under;
#else
    return 0;
#endif
}
/* the take's next len bytes into the flash (one program, IRQs off: within a page and the ring) */
static int mr_prog(uint32_t len)
{
    uint32_t k = mr.r, addr = mr.at + k, i = (mr.s0 + k) & (MR_RING - 1u), f, t0, dt, took;
    int rc;
    if (len > MR_RING - i) len = MR_RING - i;
    if (len > 256u - (addr & 255u)) len = 256u - (addr & 255u);
    if ((addr & 0xFFFu) <= 0xF07u && (addr & 0xFFFu) + len > 0xF07u) {   /* (no update record's look: 0xF06 / 0xF07 not "AT") */
        uint32_t k7 = k + 0xF07u - (addr & 0xFFFu);
        uint8_t *b6 = &mr_buf[(mr.s0 + k7 - 1u) & (MR_RING - 1u)], *b7 = &mr_buf[(mr.s0 + k7) & (MR_RING - 1u)];
        if (*b6 == 0x41u && *b7 == 0x54u) *b7 ^= 1u;
    }
    f = irq_save();
    t0 = fm1_ticks();
    rc = FL_FAR(fl_prog_ram)(addr, &mr_buf[i], len, &took);
    dt = fm1_ticks() - t0;
    irq_restore(f);
    if (rc) return rc;
    dt /= FM1_TICKS_PER_US;
    if (dt > mr.wmax) mr.wmax = dt;
    mr.r += len;
    return 0;
}
static void mr_fail(uint32_t why) { MR_HUSH(0); mr.state = MR_FAILED; mr.why = (uint8_t)why; }
/* a name no sample has: REC01 ... */
static void mr_name_new(char *name)
{
    uint32_t k, i, j;
    for (k = 1; k < 100u; k++) {
        name[0] = 'R'; name[1] = 'E'; name[2] = 'C';
        name[3] = (char)('0' + k / 10u); name[4] = (char)('0' + k % 10u);
        name[5] = name[6] = name[7] = 0;
        for (i = 0; i < SP_NUSER; i++) {
            if (!mr_has(i)) continue;
            for (j = 0; j < 8u && zu_dir.s[i].name[j] == name[j]; j++) ;
            if (j == 8u) break;
        }
        if (i == SP_NUSER) return;
    }
}
static uint32_t mr_first_free(void)
{
    uint32_t k;
    for (k = 0; k < SP_NUSER && mr_has(k); k++) ;
    return k;
}
static int mr_busy(void) { return mr.state == MR_ARMED || mr.state == MR_TAKE || mr.state == MR_SAVING; }
/* armed, the room for max_s seconds (or what the longest free run holds) kept for it. 0: armed, else MR_WHY_* */
static int mr_arm(uint32_t src, uint32_t start, uint32_t thr, uint32_t sh, uint32_t bits, uint32_t max_s)
{
    uint32_t at, longest, want;
    if (mr_busy()) return MR_WHY_NONE;
    if (!flash_ok) return MR_WHY_FLASH;
    if (mr_first_free() >= SP_NUSER) return MR_WHY_ROOM;
    (void)mr_free(&at, &longest);
    if (bits > MR_B4) bits = MR_B12;
    want = (mr_bytes((MR_FS >> sh) * max_s, bits) + 0xFFFu) & ~0xFFFu;
    if (want > MR_TAKE_MAX) want = MR_TAKE_MAX;
    if (want > longest) want = longest;
    if (want < 0x1000u) return MR_WHY_ROOM;
    mr.src = (uint8_t)src; mr.start = (uint8_t)start; mr.thr = (uint8_t)thr; mr.sh = (uint8_t)sh; mr.bits = (uint8_t)bits;
    mr.slot = (uint8_t)mr_first_free();
    mr_name_new(mr.name);
    mr.at = at; mr.cap = want; mr.er = 0; mr.why = MR_WHY_NONE; mr.pad = 0;
    mr.w = 0; mr.s0 = MR_NONE; mr.r = 0; mr.acc = 0; mr.cnt = 0; mr.has_odd = 0; mr.flushed = 0;
    mr.apred = 0; mr.aidx = 0; mr.ak = 0; mr.ns = 0; mr.blk0 = 0;
    mr.wmax = 0; mr.gaps0 = mr_gapcount(); mr.gaps = 0;
    mr.state = MR_ARMED;                         /* (last: the interrupt sees a ready recorder) */
    return 0;
}
/* the take: stopped and kept (REC again, or OPTION); before it began: nothing is kept */
static void mr_stop(void)
{
    uint32_t f = irq_save(), st = mr.state;       /* (the interrupt may end the take itself meanwhile) */
    if (st == MR_TAKE) mr.state = MR_SAVING;
    else if (st == MR_ARMED) mr.state = MR_IDLE;
    irq_restore(f);
}
/* SAVING, once a call: the last part filled in; the sound faded out, then a sector the take uses erased, or a page
 * into the flash; at the end the directory and the sound back */
static void mr_finish(void)
{
    uint32_t total, n;
    zu_slot_t s;
    zu_loop_t l;
    if (!mr.flushed) {
        mr.flushed = 1;
        if (mr.bits == MR_B4 && (mr.ak & 1u)) {  /* (ADPCM: a code without its pair: with a 0; room was kept for it) */
            mr_buf[mr.w & (MR_RING - 1u)] = mr.anib;
            mr.w++;
        }
        if (mr.has_odd) {                        /* (12 bit: an odd last sample: a 0 makes the pair, if there is room) */
            if (mr.w - mr.s0 + 3u <= mr.cap) { mr_put(0); mr.pad = 1; }
            mr.has_odd = 0;
        }
        mr.gaps = mr_gapcount() - mr.gaps0;      /* (the take's: the erases below lose packets) */
        return;
    }
    total = mr.w - mr.s0;
    MR_HUSH(1);
    if (!MR_HUSHED()) return;
    if (mr.er < total) {
        uint32_t took;
#if MASIF_APP_ROOM
        if (!mr.er && masif_app_lo && mr.at > masif_app_lo && mr.at < MASIF_APP_HI && !zu_app_mark()) { mr_fail(MR_WHY_FLASH); return; }
#endif
        fm1_wdt_feed();
        if (fl_erase4k_quiet(mr.at + mr.er, &took)) { mr_fail(MR_WHY_FLASH); return; }
        mr.er += 0x1000u;
        return;
    }
    if (mr.r < total) {
        fm1_wdt_feed();
        if (mr_prog(total - mr.r)) mr_fail(MR_WHY_FLASH);
        return;
    }
    n = mr.bits == MR_B4 ? mr.ns - mr.s0 / SP_ADBYTES * SP_ADB : mr.bits == MR_B8 ? total : total / 3u * 2u - mr.pad;
    if (n < 2u) { mr_fail(MR_WHY_EMPTY); return; }
    if (mr_has(mr.slot)) mr.slot = (uint8_t)mr_first_free();   /* (the pool changed meanwhile) */
    for (total = 0; total < sizeof s; total++) ((uint8_t *)&s)[total] = 0;
    for (total = 0; total < sizeof l; total++) ((uint8_t *)&l)[total] = 0;
    s.off = mr.at; s.n = n; s.rate = (uint16_t)(MR_FS >> mr.sh);
    s.flags = (uint8_t)(mr.bits == MR_B8 ? 4u : mr.bits == MR_B4 ? 8u : 0u);
    for (total = 0; total < 8u; total++) s.name[total] = mr.name[total];
    if (mr.slot >= SP_NUSER || mr_dir_put(mr.slot, &s, &l)) { mr_fail(MR_WHY_DIR); return; }
    MR_HUSH(0);
    mr.state = MR_SAVED;
}
static void mr_service(void)
{
    switch (mr.state) {
    case MR_SAVING:
        mr_finish();
        break;
    default:
        break;
    }
}
/* for the REC box: the input's peak since the last look (16 bit), the take's length and room (ms), the save (%: the
 * erase its first half, the programs the second) */
static uint32_t mr_level(void) { uint32_t p = mr.peak; mr.peak = 0; return p; }
static uint32_t mr_ms_of(uint32_t bytes)
{
    uint32_t n = mr_samples(bytes, mr.bits), fs = MR_FS >> mr.sh;
    return n / fs * 1000u + n % fs * 1000u / fs;
}
static uint32_t mr_take_ms(void) { return mr.state == MR_TAKE || mr.state == MR_SAVING ? mr_ms_of(mr.w - mr.s0) : 0u; }
static uint32_t mr_cap_ms(void) { return mr_ms_of(mr.cap); }
static uint32_t mr_save_pct(void)
{
    uint32_t total = mr.w - mr.s0;
    if (mr.state != MR_SAVING || mr.s0 == MR_NONE || !total) return 0u;
    return (mr.er < total ? mr.er : total) * 50u / total + mr.r * 50u / total;
}
/* the room left for a take at 44.1 kHz >> sh (ms; the longest free run) */
static uint32_t mr_room_ms(uint32_t sh, uint32_t bits)
{
    uint32_t longest, n, fs = MR_FS >> sh;
    (void)mr_free(0, &longest);
    if (longest > MR_TAKE_MAX) longest = MR_TAKE_MAX;   /* (and what the ring holds) */
    n = mr_samples(longest, bits);
    return n / fs * 1000u + n % fs * 1000u / fs;
}
static uint32_t mr_state_now(void) { return mr.state; }
static uint32_t mr_why_now(void) { return mr.why; }
static uint32_t mr_slot_now(void) { return mr.slot; }
static const char *mr_name_now(void) { static char n[9]; uint32_t i; for (i = 0; i < 8u; i++) n[i] = mr.name[i]; n[8] = 0; return n; }
static uint32_t mr_gaps_now(void) { return mr.state == MR_TAKE ? mr_gapcount() - mr.gaps0 : mr.gaps; }
static uint32_t mr_wmax_now(void) { return mr.wmax; }
static void mr_listen(int on) { mr.listen = (uint8_t)!!on; mr.peak = 0; }
