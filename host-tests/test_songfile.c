/* MASIF song file (masif_songfile.c): what is packed comes back the same; anything else is refused whole.
 *   cc -std=gnu99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Ifirmware/src \
 *      -o build/host/test_songfile host-tests/test_songfile.c firmware/src/masif_tracker.c \
 *      firmware/src/masif_runtime.c firmware/src/masif_songfile.c firmware/src/masif_demo.c && build/host/test_songfile */
#include "../firmware/src/masif_songfile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int masif_demo_load(masif_song_t *song);
static uint32_t lcg = 99u;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) { c ^= *p++; for (k = 0; k < 8u; k++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1; }
    return ~c;
}

int main(void)
{
    masif_song_t *a = malloc(sizeof *a), *b = malloc(sizeof *b), *empty = malloc(sizeof *empty);
    uint8_t *f = malloc(MASIF_FILE_MAX + 16u), *g = malloc(MASIF_FILE_MAX + 16u);
    uint32_t n, n_empty, n_demo, i, k, t, refused = 0, tried = 0, biggest = 0;
    assert(a && b && empty && f && g);
    masif_song_clear(empty);

    n_empty = masif_song_pack(empty, f, MASIF_FILE_MAX);
    assert(n_empty == 28u && masif_song_unpack(b, f, n_empty) && !memcmp(b, empty, sizeof *b));

    assert(masif_demo_load(a));
    n_demo = masif_song_pack(a, f, MASIF_FILE_MAX);
    assert(n_demo && n_demo < 1400u && masif_song_unpack(b, f, n_demo) && !memcmp(a, b, sizeof *a));
    assert(masif_song_unpack(b, f, n_demo + 7u) && !memcmp(a, b, sizeof *a));     /* (room after the file: fine) */
    for (k = 0; k < n_demo; k++) assert(!masif_song_pack(a, g, k));                /* too small a buffer: 0, never past it */

    /* a damaged file: every single bit flipped, every length it could be cut to: refused, the song left empty */
    for (k = 0; k < n_demo * 8u; k++) {
        memcpy(g, f, n_demo);
        g[k / 8u] ^= (uint8_t)(1u << (k % 8u));
        tried++;
        if (!masif_song_unpack(b, g, n_demo)) { refused++; assert(!memcmp(b, empty, sizeof *b)); }
    }
    assert(refused == tried);
    for (k = 0; k < n_demo; k++) {
        uint8_t *cut = malloc(k ? k : 1u);          /* (its own block: reading past the cut is caught) */
        memcpy(cut, f, k);
        assert(!masif_song_unpack(b, cut, k) && !memcmp(b, empty, sizeof *b));
        free(cut);
    }

    /* random songs in range (sparse, dense, full) come back the same; random bytes as a file are refused or,
     * with a right checksum by accident or on purpose, give a song that is in range */
    for (i = 0; i < 400u; i++) {
        uint32_t density = i % 4u == 0u ? 1u : i % 4u == 1u ? 6u : i % 4u == 2u ? 40u : 400u;
        masif_song_clear(a);
        for (k = 0; k < sizeof a->phrase; k += sizeof(masif_cell_t))
            if (rnd() % density == 0u) {
                masif_cell_t c = {(uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd()};
                if (rnd() & 1u) c.instrument = 0xFF;
                if (rnd() & 1u) c.volume = 0xFF;
                if (rnd() & 1u) c.fx = c.param = 0;
                if (rnd() & 1u) c.fx2 = c.param2 = 0;
                memcpy((unsigned char *)a->phrase + k, &c, sizeof c);
            }
        for (k = 0; k < MASIF_TABLES; k++)
            if (rnd() % (density < 40u ? 2u : 20u) == 0u)
                for (t = 0; t < MASIF_TABLE_ROWS; t++)
                    if (rnd() & 1u) { masif_table_cell_t tc = {(int8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd()}; a->table[k][t] = tc; }
        for (k = 0; k < MASIF_INSTRUMENTS; k++)
            if (rnd() % (density < 40u ? 2u : 20u) == 0u)
                for (t = 0; t < sizeof(masif_instrument_t); t++) ((unsigned char *)&a->instrument[k])[t] = (unsigned char)rnd();
        for (k = 0; k < sizeof a->chain; k++) if (rnd() % density == 0u) ((unsigned char *)a->chain)[k] = (unsigned char)rnd();
        for (k = 0; k < sizeof a->song; k++) if (rnd() % (density * 4u) == 0u) ((unsigned char *)a->song)[k] = (unsigned char)rnd();
        a->tempo = (uint8_t)rnd(); a->key = (uint8_t)rnd(); a->scale = (uint8_t)rnd();
        for (k = 0; k < MASIF_TRACKS; k++) a->track_level[k] = (uint8_t)rnd();
        for (k = 0; k < 3u; k++) a->master_eq[k] = (int8_t)rnd();
        (void)masif_song_sanitize(a);
        n = masif_song_pack(a, f, MASIF_FILE_MAX);
        assert(n && n <= MASIF_FILE_MAX);
        if (n > biggest) biggest = n;
        assert(masif_song_unpack(b, f, n) && !memcmp(a, b, sizeof *a));
    }
    memset(a->phrase, 0x55, sizeof a->phrase);      /* every cell full: every phrase stored as it is */
    memset(a->table, 1, sizeof a->table);           /* every table full, every instrument its own, */
    for (k = 0; k < MASIF_INSTRUMENTS; k++) a->instrument[k].volume = 99;
    memset(a->chain, 1, sizeof a->chain);           /* every chain 16 steps, every song row in use */
    memset(a->song, 0, sizeof a->song);
    memset(a->groove, 7, sizeof a->groove);         /* and every groove */
    n = masif_song_pack(a, f, MASIF_FILE_MAX);
    assert(n == MASIF_FILE_MAX && masif_song_unpack(b, f, n) && !memcmp(a, b, sizeof *a));
    for (i = 0; i < 3000u; i++) {
        uint32_t len = 10u + rnd() % 600u;
        uint8_t *x = malloc(len);
        for (k = 0; k < len; k++) x[k] = (uint8_t)rnd();
        if (i & 1u) {                               /* a right header and checksum over a random body */
            uint32_t c = 0xFFFFFFFFu, j, bit;
            x[0] = 'M'; x[1] = 'S'; x[2] = 'F'; x[3] = MASIF_FILE_VERSION; x[4] = (uint8_t)len; x[5] = (uint8_t)(len >> 8);
            for (j = 10; j < len; j++) { c ^= x[j]; for (bit = 0; bit < 8u; bit++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
            c = ~c; x[6] = (uint8_t)c; x[7] = (uint8_t)(c >> 8); x[8] = (uint8_t)(c >> 16); x[9] = (uint8_t)(c >> 24);
        }
        if (masif_song_unpack(b, x, len)) assert(masif_song_sanitize(b) == 0u);
        else assert(!memcmp(b, empty, sizeof *b));
        free(x);
    }
    {   /* a version 6 file (before the instruments' names, 58 bytes an instrument): still read, the name none */
        uint32_t c;
        masif_song_clear(a);
        a->instrument[3].sample = 5; a->instrument[3].volume = 90;
        n = masif_song_pack(a, f, MASIF_FILE_MAX);
        assert(n && f[3] == 7u && f[24] == 1u && f[25] == 3u && sizeof(masif_instrument_t) == MASIF_FILE_V6_INST + MASIF_NAME_LEN);
        memmove(f + 26u + MASIF_FILE_V6_INST, f + 26u + sizeof(masif_instrument_t), n - 26u - sizeof(masif_instrument_t));
        n -= MASIF_NAME_LEN;
        f[3] = 6u; f[4] = (uint8_t)n; f[5] = (uint8_t)(n >> 8);
        c = crc32(f + 10, n - 10u);
        f[6] = (uint8_t)c; f[7] = (uint8_t)(c >> 8); f[8] = (uint8_t)(c >> 16); f[9] = (uint8_t)(c >> 24);
        assert(masif_song_unpack(b, f, n) && !memcmp(a, b, sizeof *a));
        f[3] = 5u;
        assert(!masif_song_unpack(b, f, n));
        a->instrument[3].name[0] = 'B'; a->instrument[3].name[1] = 0x7F;          /* (a name's character the font has not) */
        n = masif_song_pack(a, f, MASIF_FILE_MAX);
        assert(masif_song_unpack(b, f, n) && b->instrument[3].name[0] == 'B' && b->instrument[3].name[1] == ' ');
        puts("song file: a version 6 file is read (its instruments without a name), 5 refused, a name kept, a bad character mended");
    }
    printf("song file: empty %u B, the demo %u B, the largest of 400 random songs %u B, the most %u B (the song in RAM: %u B)\n",
           (unsigned)n_empty, (unsigned)n_demo, (unsigned)biggest, (unsigned)MASIF_FILE_MAX, (unsigned)sizeof *a);
    puts("MASIF song file tests: PASS");
    free(a); free(b); free(empty); free(f); free(g);
    return 0;
}
