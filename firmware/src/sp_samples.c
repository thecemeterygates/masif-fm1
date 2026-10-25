/* SPDX-License-Identifier: GPL-3.0-only */
/* zp12's own samples, put there by the web editor (web/editor.html over sp_link.c): a directory in flash, two
 * copies (0xD8000, 0xD9000) with a generation and a CRC, the newest good one counts; the samples themselves
 * packed as the kit's (12 bit, 2 in 3 bytes) in the free rooms, read through the plain XIP window. Here they
 * are only read: at the start, and when the editor says RELOAD. They play as waves KIT_NWAVE + slot.
 *   directory: magic "ZPSM", gen, flags (bit 0: sloopDX's bank room holds samples), CRC32 of the slots,
 *              24 slots of {flash offset (0: empty), samples, rate Hz, flags (bit 0: stored for 45->33, bit 1: at double speed), -, name[8]}
 *   MASIF: then the samples' own loops (zu_loops_t) */
#define ZU_DIR0 0xD8000u
#define ZU_MAGIC 0x4D53505Au                     /* "ZPSM" */
typedef struct { uint32_t off, n; uint16_t rate; uint8_t flags, rsv; char name[8]; } zu_slot_t;
typedef struct { uint32_t magic, gen, flags, crc; zu_slot_t s[SP_NUSER]; } zu_dir_t;
static zu_dir_t zu_dir;
static uint8_t zu_copy;                          /* which copy zu_dir came from (the next write goes into the other) */
#if MASIF_ENABLE_TRACKER
/* MASIF's loops: right after the directory in the same sector (zp12's editor erases it with the sector, and then
 * there are none), magic "MSLP", CRC32 of the slots, a slot each: the loop's first sample, its end (0: no loop),
 * the play mode an instrument begins with when the sample pool gives it the sample (masif_play_t), and the
 * loop's tapers in, out (rsv[0], rsv[1]: 0..128 in 1/256 of the loop) */
#define ZU_LP_MAGIC 0x504C534Du                  /* "MSLP" */
typedef struct { uint32_t a, b; uint8_t play, rsv[3]; } zu_loop_t;
typedef struct { uint32_t magic, crc; zu_loop_t s[SP_NUSER]; } zu_loops_t;
#endif
#define ZU_MAX_N 1000000u                        /* samples a slot at the most (the app room: ~480 KiB of 8 bit) */

#ifndef MASIF_APP_ROOM
#define MASIF_APP_ROOM 0
#endif
#if MASIF_APP_ROOM
/* MASIF's third room (2026-10-21): the app area past the image, [masif_app_lo + 4 KiB, MASIF_APP_HI) (zp12.c), read
 * through the same plain XIP window as the data flash. Its first sector (masif_app_lo) holds a marker, written before
 * the first sample goes in: a firmware update rewrites the app area, the marker is gone with it, and the samples the
 * directory still has there are not played (the web app sees the marker's state in HELLO and puts them back). */
#define ZU_APP_MAGIC 0x5241534Du                 /* "MSAR" */
static uint8_t zu_app_ok;
#define ZU_APP_IN(off, n) (zu_app_ok && FL_IN(off, n, masif_app_lo + 0x1000u, MASIF_APP_HI))
static int zu_app_marked(void)
{
    uint32_t m[4];
    if (!masif_app_lo || st_read(masif_app_lo, m, sizeof m)) return 0;
    return m[0] == ZU_APP_MAGIC && m[1] == masif_app_lo && m[2] == MASIF_APP_HI && m[3] == zs_crc((const uint8_t *)m, 12);
}
/* before the link erases or writes in the room: the marker, if it is not there yet */
static int zu_app_mark(void)
{
    uint32_t m[4], took;
    if (zu_app_marked()) return zu_app_ok = 1;
    m[0] = ZU_APP_MAGIC; m[1] = masif_app_lo; m[2] = MASIF_APP_HI; m[3] = zs_crc((const uint8_t *)m, 12);
    if (fl_erase4k_quiet(masif_app_lo, &took) || fl_write(masif_app_lo, (const uint8_t *)m, sizeof m)) return 0;
    return zu_app_ok = (uint8_t)zu_app_marked();
}
#else
#define ZU_APP_IN(off, n) 0
#endif

static int zu_room(uint32_t off, uint32_t bytes)  /* inside one room the editor may fill */
{
#if MASIF_ENABLE_TRACKER
    /* MASIF has the data flash to itself (no SLOOP, Felucca or sloopDX saves on its devices): the two big
     * pieces are the samples', 0x93000-0xC3FFF and 0xEA000-0xFBFFF, 268 KiB: 6.9 s at 26.04 kHz. (Between them
     * zp12's store and this directory, the songs (masif_fm1_store.c) and the update loader's staging.) And with
     * MASIF's screens the app area past the image (ZU_APP_IN). */
    return FL_IN(off, bytes, 0x93000u, 0xC4000u) || FL_IN(off, bytes, 0xEA000u, 0xFC000u) || ZU_APP_IN(off, bytes);
#else
    return FL_IN(off, bytes, 0xA0000u, 0xC4000u) || FL_IN(off, bytes, 0xDA000u, 0xDC000u) ||
           FL_IN(off, bytes, 0xE5000u, 0xE9000u) || FL_IN(off, bytes, 0xEA000u, 0xFC000u);
#endif
}

static void zu_load(void)
{
    static zu_dir_t d[2];
    uint32_t i, best = 2;
    for (i = 0; i < SP_NUSER; i++) {             /* none first (a voice on one of them stops) */
        sp_wave[KIT_NWAVE + i].n = 0;
        ui_uname[i][0] = 0;
        ui_uflags[i] = 0;
    }
    for (i = 0; i < sizeof sp_ch / sizeof sp_ch[0]; i++)   /* (every voice: the tracker's too, if built in) */
        if (sp_ch[i].w >= &sp_wave[KIT_NWAVE]) sp_ch[i].on = 0;
    if (!flash_ok)
        return;
#if MASIF_APP_ROOM
    zu_app_ok = (uint8_t)zu_app_marked();
#endif
    for (i = 0; i < 2u; i++)
        if (!st_read(ZU_DIR0 + i * 0x1000u, &d[i], sizeof d[i]) && d[i].magic == ZU_MAGIC &&
            zs_crc((const uint8_t *)d[i].s, sizeof d[i].s) == d[i].crc && (best == 2u || d[i].gen > d[best].gen))
            best = i;
    if (best == 2u) {
        zu_dir.magic = 0;
        return;
    }
    zu_dir = d[best];
    zu_copy = (uint8_t)best;
#if MASIF_ENABLE_TRACKER
    {
        zu_loops_t lp;                           /* (the stack's: only while loading) */
        int ok = !st_read(ZU_DIR0 + best * 0x1000u + sizeof(zu_dir_t), &lp, sizeof lp) && lp.magic == ZU_LP_MAGIC &&
                 zs_crc((const uint8_t *)lp.s, sizeof lp.s) == lp.crc;
        for (i = 0; i < SP_NUSER; i++) {
            sp_wave_t *w = &sp_wave[KIT_NWAVE + i];
            uint32_t n = zu_dir.s[i].n;
            w->lp_a = w->lp_b = 0; w->lp_play = 0; w->lp_tin = w->lp_tout = 0;
            if (!ok) continue;
            if (lp.s[i].play < MASIF_PLAY_MODES) w->lp_play = lp.s[i].play;
            if (lp.s[i].b && lp.s[i].b <= n && lp.s[i].a + 1u < lp.s[i].b) {
                w->lp_a = lp.s[i].a; w->lp_b = lp.s[i].b;
                w->lp_tin = lp.s[i].rsv[0] > 128u ? 128u : lp.s[i].rsv[0];
                w->lp_tout = lp.s[i].rsv[1] > 128u ? 128u : lp.s[i].rsv[1];
            }
        }
    }
#endif
    for (i = 0; i < SP_NUSER; i++) {
        const zu_slot_t *s = &zu_dir.s[i];
        uint32_t k;
#if MASIF_ENABLE_TRACKER                         /* (flags bit 2: an 8-bit sample, a byte each; bit 3: ADPCM) */
        if (!s->off || s->n < 2u || s->n > ZU_MAX_N || s->rate < 8000u || s->rate > 48000u || (s->flags & 12u) == 12u ||
            !zu_room(s->off, s->flags & 8u ? (s->n + 127u) / 128u * 68u : s->flags & 4u ? s->n : (s->n + 1u) / 2u * 3u))
            continue;
#else
        if (!s->off || s->n < 2u || s->n > 400000u || s->rate < 8000u || s->rate > 48000u || !zu_room(s->off, (s->n + 1u) / 2u * 3u))
            continue;
#endif
        sp_wave[KIT_NWAVE + i].d = (const uint8_t *)FL_XIP(s->off);
        sp_wave[KIT_NWAVE + i].rate = s->rate;
        ui_uflags[i] = s->flags;
#if MASIF_ENABLE_TRACKER
        sp_wave[KIT_NWAVE + i].uflags = s->flags;   /* (a tracker instrument plays the wave itself, not a pad's sound) */
#endif
        for (k = 0; k < 8u && s->name[k] >= 32 && s->name[k] < 127; k++) ui_uname[i][k] = s->name[k];
        ui_uname[i][k] = 0;
        if (!k) { ui_uname[i][0] = 'U'; ui_uname[i][1] = (char)('A' + i); ui_uname[i][2] = 0; }
        sp_wave[KIT_NWAVE + i].n = s->n;         /* (last: the sample is there before a voice can take it) */
    }
}
