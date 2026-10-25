/* MASIF song store (masif_store.c) on a flash in RAM that behaves as the chip's: an erase sets a sector to FF, a
 * write can only clear bits, and the power can go in the middle of either. What is checked: songs come back as
 * saved; a power cut at any point of a save leaves the slot's old song or its new one, whole, and every other
 * slot as it was; damage is noticed; a full store refuses without losing anything; nothing that would start the
 * device in update mode is ever left in the flash.
 *   cc -std=gnu99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Ifirmware/src \
 *      -o build/host/test_store host-tests/test_store.c firmware/src/masif_tracker.c firmware/src/masif_runtime.c \
 *      firmware/src/masif_songfile.c firmware/src/masif_store.c firmware/src/masif_demo.c && build/host/test_store */
#include "../firmware/src/masif_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int masif_demo_load(masif_song_t *song);

static uint8_t flash[0x100000];
static long ops, cut_at = -1;                    /* the op (an erase or a write) the power goes in; -1: never */
static int dead;                                 /* the power is gone: nothing more reaches the flash */
static uint32_t lcg = 7u;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

int masif_fl_read(uint32_t off, uint8_t *dst, uint32_t n) { assert(off + n <= sizeof flash); memcpy(dst, flash + off, n); return 0; }
int masif_fl_erase(uint32_t off)
{
    uint32_t i;
    assert(!(off & 0xFFFu) && off + 4096u <= sizeof flash);
    if (dead) return -1;
    if (ops++ == cut_at) {                       /* a sector half erased: some of it FF, some as it was, some neither */
        for (i = 0; i < 4096u; i++) if (rnd() % 3u) flash[off + i] |= (uint8_t)rnd();
        dead = 1;
        return -1;
    }
    memset(flash + off, 0xFF, 4096);
    return 0;
}
int masif_fl_write(uint32_t off, const uint8_t *src, uint32_t n)
{
    uint32_t i, upto = n;
    assert(off + n <= sizeof flash);
    if (dead) return -1;
    if (ops++ == cut_at) { upto = rnd() % (n + 1u); dead = 1; }
    for (i = 0; i < upto; i++) flash[off + i] &= src[i];
    if (dead && upto < n) flash[off + upto] &= (uint8_t)(src[upto] | rnd());   /* (the byte it was at: partly) */
    return dead ? -1 : 0;
}

static const masif_store_region_t pool[3] = {{0xDA000, 0xE0000}, {0xE5000, 0xE9000}, {0xFC000, 0xFF000}};
#define POOL_SECTORS 13u
static int bad;
static void check(int ok, const char *what) { printf("store: %-104s %s\n", what, ok ? "ok" : "FAIL"); bad += !ok; }
static void power_on(void) { dead = 0; cut_at = -1; ops = 0; masif_store_init(pool, 3); }

/* a song of about `cells` cells and some instruments and tables */
static void make_song(masif_song_t *s, uint32_t cells, uint32_t seed)
{
    uint32_t i;
    lcg = seed;
    masif_song_clear(s);
    for (i = 0; i < cells; i++) {
        masif_cell_t c = {(uint8_t)(1u + rnd() % 120u), (uint8_t)(rnd() % 128u), (uint8_t)(rnd() % 128u), (uint8_t)(rnd() % 22u), (uint8_t)rnd(), 0, 0};
        if (MASIF_FX_OUT(c.fx)) c.fx = MASIF_FX_NONE;    /* (a load clears those: a song has none) */
        (void)masif_set_cell(s, (uint8_t)(rnd() % MASIF_PHRASES), (uint8_t)(rnd() % MASIF_ROWS), &c);
    }
    for (i = 0; i < 10u; i++) s->instrument[rnd() % MASIF_INSTRUMENTS].volume = (uint8_t)(rnd() % 128u);
    for (i = 0; i < 10u; i++) s->table[rnd() % MASIF_TABLES][rnd() % MASIF_TABLE_ROWS].transpose = (int8_t)(rnd() % 25u);
    for (i = 0; i < 30u; i++) (void)masif_set_chain_step(s, (uint8_t)(rnd() % MASIF_CHAINS), (uint8_t)(rnd() % 16u), (uint8_t)(rnd() % MASIF_PHRASES), 0);
    for (i = 0; i < 20u; i++) (void)masif_set_song_cell(s, (uint8_t)rnd(), (uint8_t)(rnd() % MASIF_TRACKS), (uint8_t)(rnd() % MASIF_CHAINS));
}
static int nothing_outside_pool(const uint8_t *before)
{
    uint32_t a, r;
    for (a = 0; a < sizeof flash; a++) {
        for (r = 0; r < 3u && !(a >= pool[r].lo && a < pool[r].hi); r++) {}
        if (r == 3u && flash[a] != before[a]) return 0;
    }
    return 1;
}
static int no_lookalike(void)
{
    uint32_t r, a;
    for (r = 0; r < 3u; r++)
        for (a = pool[r].lo; a < pool[r].hi; a += 4096u)
            if (masif_flash_update_lookalike(a)) return 0;
    return 1;
}

#define NCELL (200u * MASIF_ROWS)
#define CB ((uint32_t)sizeof(masif_cell_t))      /* a cell's bytes, in the song and (a full phrase) in the file */
int main(void)
{
    masif_song_t *a = malloc(sizeof *a), *b = malloc(sizeof *b), *c = malloc(sizeof *c), *got = malloc(sizeof *got);
    static uint8_t before[sizeof flash], snap[sizeof flash], file[MASIF_FILE_MAX];
    masif_store_info_t info;
    uint32_t i, k, n, old = 0, new = 0, cuts = 0, evil_at = 0;
    int rc, ok;
    assert(a && b && c && got);
    for (i = 0; i < sizeof flash; i++) flash[i] = (uint8_t)(i * 31u + 7u);      /* (whatever was in the flash before) */
    for (i = 0; i < 3u; i++) memset(flash + pool[i].lo, 0xFF, pool[i].hi - pool[i].lo);
    memcpy(before, flash, sizeof flash);

    power_on();
    ok = masif_store_free() == POOL_SECTORS;
    for (i = 0; i < MASIF_STORE_SLOTS; i++) ok &= masif_store_info((uint8_t)i, &info) && !info.used;
    check(ok && !masif_store_info(MASIF_STORE_SLOTS, &info) && masif_store_load(got, 5) == MASIF_STORE_EMPTY,
          "an empty store: 16 free slots, 13 free sectors; loading an empty slot leaves the song alone");

    assert(masif_demo_load(a));
    rc = masif_store_save(a, 3, "DEMO");
    check(rc == MASIF_STORE_OK && masif_store_info(3, &info) && info.used && !strcmp(info.name, "DEMO") && info.sectors == 1 &&
          info.length == masif_song_pack(a, file, sizeof file) && masif_store_free() == POOL_SECTORS - 1u,
          "the demo song into slot 3: one sector, its name and length kept");
    check(masif_store_load(got, 3) == MASIF_STORE_OK && !memcmp(got, a, sizeof *a), "... and it loads as it was saved");
    power_on();
    check(masif_store_load(got, 3) == MASIF_STORE_OK && !memcmp(got, a, sizeof *a) && masif_store_info(3, &info) && !strcmp(info.name, "DEMO"),
          "after the power was off: still there");

    make_song(b, 3000, 11);                          /* most phrases in use: several sectors */
    n = masif_song_pack(b, file, sizeof file);
    rc = masif_store_save(b, 3, "BIGGER");
    check(rc == MASIF_STORE_OK && masif_store_info(3, &info) && info.sectors == (n + MASIF_STORE_ROOM - 1u) / MASIF_STORE_ROOM && info.sectors >= 3u &&
          masif_store_load(got, 3) == MASIF_STORE_OK && !memcmp(got, b, sizeof *b) && masif_store_free() == POOL_SECTORS - info.sectors,
          "a bigger song over it (several sectors, across the pool's pieces): it is the slot's song, the old one's sector is free again");
    printf("       (%u B in %u sectors)\n", (unsigned)n, (unsigned)info.sectors);

    rc = masif_store_save(a, 0, "A");
    rc |= masif_store_save(a, 15, "LASTSLOT");
    check(rc == MASIF_STORE_OK && masif_store_save(a, MASIF_STORE_SLOTS, "X") == MASIF_STORE_BAD_SLOT && masif_store_load(got, 15) == MASIF_STORE_OK &&
          !memcmp(got, a, sizeof *a) && masif_store_load(got, 3) == MASIF_STORE_OK && !memcmp(got, b, sizeof *b),
          "other slots beside it; a slot number that is none (past the working copy's) is refused");
    check(masif_store_delete(0) == MASIF_STORE_OK && masif_store_info(0, &info) && !info.used && masif_store_load(got, 3) == MASIF_STORE_OK &&
          !memcmp(got, b, sizeof *b), "a slot deleted: empty, the others untouched");

    /* the power goes at every single step of saving a new song over the bigger one */
    make_song(c, 2000, 23);
    memcpy(snap, flash, sizeof flash);
    for (k = 0; k < 400; k++) {
        long total;
        memcpy(flash, snap, sizeof flash);
        power_on();
        cut_at = (long)k;
        rc = masif_store_save(c, 3, "NEWER");
        total = ops;
        power_on();
        rc = masif_store_load(got, 3);
        if (rc == MASIF_STORE_OK && !memcmp(got, b, sizeof *b)) old++;
        else if (rc == MASIF_STORE_OK && !memcmp(got, c, sizeof *c)) new++;
        else { printf("       power cut at step %u: slot 3 is neither (%d)\n", (unsigned)k, rc); bad++; }
        if (masif_store_load(got, 15) != MASIF_STORE_OK || memcmp(got, a, sizeof *a)) { printf("       power cut at step %u: slot 15 lost\n", (unsigned)k); bad++; }
        if (!no_lookalike()) bad++;
        cuts++;
        /* and the store works on from whatever was left: the save again, whole this time */
        if (masif_store_save(c, 3, "NEWER") != MASIF_STORE_OK || masif_store_load(got, 3) != MASIF_STORE_OK || memcmp(got, c, sizeof *c)) {
            printf("       after the power cut at step %u the next save failed\n", (unsigned)k); bad++;
        }
        if ((long)k >= total) break;                 /* (past the save's last step: it ran through) */
    }
    check(old > 3u && new > 0u && old + new == cuts, "the power cut at each step of a save: the slot has its old song or its new one, never a broken one");
    printf("       (%u steps: %u times the old song, %u the new; the other slot always there; the next save always worked)\n",
           (unsigned)cuts, (unsigned)old, (unsigned)new);

    /* damage: a bit flips in a stored song */
    memcpy(flash, snap, sizeof flash);
    power_on();
    for (i = pool[0].lo; i < pool[2].hi; i++)
        if (flash[i] != 0xFF && i % 4096u == 2000u && (flash[i - 2000] == 'M')) { flash[i] ^= 4; break; }
    power_on();
    ok = 0;
    for (i = 0; i < MASIF_STORE_SLOTS; i++) { masif_store_info((uint8_t)i, &info); ok += info.used; }
    check(ok == 1, "a stored song with one bit flipped: its slot counts as empty (the other still loads), its sectors as free");

    /* full: nothing is lost */
    memcpy(flash, snap, sizeof flash);
    power_on();
    make_song(c, 9000, 5);                           /* as full as a song gets: every table, chain and song row too */
    memset(c->table, 1, sizeof c->table);
    memset(c->chain, 1, sizeof c->chain);
    memset(c->song, 0, sizeof c->song);
    for (i = 0; i < MASIF_INSTRUMENTS; i++) c->instrument[i].volume = 77;
    n = masif_song_pack(c, file, sizeof file);
    k = masif_store_free();
    rc = masif_store_save(c, 7, "HUGE");
    check(rc == MASIF_STORE_FULL && masif_store_free() == k && masif_store_load(got, 3) == MASIF_STORE_OK && !memcmp(got, b, sizeof *b) &&
          masif_store_load(got, 15) == MASIF_STORE_OK && !memcmp(got, a, sizeof *a),
          "a song that needs more sectors than are free: refused, the store as it was");
    printf("       (%u B = %u sectors wanted, %u free)\n", (unsigned)n, (unsigned)((n + MASIF_STORE_ROOM - 1u) / MASIF_STORE_ROOM), (unsigned)k);
    check(masif_store_delete(3) == MASIF_STORE_OK && masif_store_delete(15) == MASIF_STORE_OK && masif_store_save(c, 7, "HUGE") == MASIF_STORE_OK &&
          masif_store_load(got, 7) == MASIF_STORE_OK && !memcmp(got, c, sizeof *c), "after deleting two songs it fits, and comes back whole");

    /* a song made so that its file would leave an update record's look-alike 256 below a sector's end */
    for (i = 0; i < 3u; i++) memset(flash + pool[i].lo, 0xFF, pool[i].hi - pool[i].lo);
    power_on();
    masif_song_clear(c);
    for (i = 0; i < NCELL; i++) {                    /* 200 full phrases: stored as they are, a cell's bytes as they are */
        masif_cell_t x = {(uint8_t)(1u + i % 120u), (uint8_t)(i % 128u), (uint8_t)(i * 7u % 128u), (uint8_t)(1u + i % 20u), (uint8_t)(i >> 4),
                          (uint8_t)(1u + i % 19u), (uint8_t)(i >> 3)};
        (void)masif_set_cell(c, (uint8_t)(i / MASIF_ROWS), (uint8_t)(i % MASIF_ROWS), &x);
    }
    n = masif_song_pack(c, file, sizeof file);
    {
        /* where each cell's 5 bytes are in the file (a full pattern is stored as it is), then the record's 80
         * bytes into the cells that lie 256 below the end of one of the file's sectors */
        static int32_t cell_at[MASIF_FILE_MAX];      /* file offset -> cell * 5 + byte, or -1 */
        uint8_t rec[80];
        uint32_t crc = 0, bit, j, m, at = 0;
        memset(cell_at, 0xFF, sizeof cell_at);
        for (i = 0; i < NCELL; i++)
            for (j = 0; j + CB <= n; j++)
                if (!memcmp(file + j, &c->phrase[i / MASIF_ROWS][i % MASIF_ROWS], CB)) {
                    for (bit = 0; bit < CB; bit++) cell_at[j + bit] = (int32_t)(i * CB + bit);
                    break;
                }
        for (m = 0; m < 4u && !at; m++) {            /* a sector where the record's fixed bytes (0, 1: its CRC; 6, 7: its
                                                      * mark) fall on cells; a phrase's two head bytes in between stay */
            at = m * MASIF_STORE_ROOM + 4096u - 256u - MASIF_STORE_HEAD;
            if (at + 80u > n || cell_at[at] < 0 || cell_at[at + 1u] < 0 || cell_at[at + 6u] < 0 || cell_at[at + 7u] < 0) at = 0;
        }
        assert(at);
        for (i = 0; i < 80u; i++) rec[i] = cell_at[at + i] < 0 ? file[at + i] : (uint8_t)(i * 37u + 5u);
        rec[6] = 0x41; rec[7] = 0x54;
        for (i = 2; i < 80u; i++) { crc ^= (uint32_t)rec[i] << 8; for (bit = 0; bit < 8u; bit++) crc = crc & 0x8000u ? ((crc << 1) ^ 0x1021u) & 0xFFFFu : (crc << 1) & 0xFFFFu; }
        rec[0] = (uint8_t)crc; rec[1] = (uint8_t)(crc >> 8);
        for (i = 0; i < 80u; i++) {
            uint32_t k5 = (uint32_t)cell_at[at + i];
            if (cell_at[at + i] < 0) continue;
            ((uint8_t *)&c->phrase[k5 / CB / MASIF_ROWS][k5 / CB % MASIF_ROWS])[k5 % CB] = rec[i];
        }
        n = masif_song_pack(c, file, sizeof file);
        assert(n && !memcmp(file + at, rec, 80));
        evil_at = at;
    }
    rc = masif_store_save(c, 1, "EVIL");
    check(rc == MASIF_STORE_REFUSED && no_lookalike() && masif_store_free() == POOL_SECTORS && masif_store_info(1, &info) && !info.used,
          "a song whose file would put an update record's look-alike into the flash: refused, nothing of it left");
    {                                                /* the look-alike off by one bit: just a song */
        uint32_t j;
        n = masif_song_pack(c, file, sizeof file);
        for (i = 0; i < NCELL; i++) {
            uint8_t *x = (uint8_t *)&c->phrase[i / MASIF_ROWS][i % MASIF_ROWS];
            for (j = evil_at + 20u; j < evil_at + 25u; j++)
                if (!memcmp(file + j - 4u, x, CB) || !memcmp(file + j, x, CB)) { x[4] ^= 1; i = NCELL; break; }
        }
        n = masif_song_pack(c, file, sizeof file);
    }
    rc = masif_store_save(c, 1, "FINE");
    memcpy(b, c, sizeof *c);
    (void)masif_song_sanitize(b);                    /* (a load's: its bytes may name an effect taken out) */
    check(rc == MASIF_STORE_OK && masif_store_load(got, 1) == MASIF_STORE_OK && !memcmp(got, b, sizeof *b) && no_lookalike(),
          "the same song a bit different is a song like any other: saved");

    check(nothing_outside_pool(before), "through all of it not a byte outside the store's three pieces of the flash changed");
    puts(bad ? "MASIF song store tests: FAIL" : "MASIF song store tests: PASS");
    free(a); free(b); free(c); free(got);
    return bad ? 1 : 0;
}
