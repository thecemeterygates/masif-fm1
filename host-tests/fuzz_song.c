/* MASIF: no song content may make the player misbehave. Songs of random bytes (as a damaged save or a hostile
 * file over USB would be), put in range by masif_song_sanitize() as every load has to, then played for a few
 * thousand rows with random cell edits coming in. Built with the address and undefined-behaviour sanitizers: an
 * access outside the song or the player's state stops the run. Checked besides: every event the player emits
 * names a track 0-3, an instrument and a sound 0-31 and a pitch inside its range, and the player's position
 * stays inside the song.
 *   cc -std=gnu99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Ifirmware/src \
 *      -o build/host/fuzz_song host-tests/fuzz_song.c firmware/src/masif_tracker.c firmware/src/masif_runtime.c \
 *      && build/host/fuzz_song [songs]   */
#include "../firmware/src/masif_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t lcg = 0x1234567u;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

static unsigned long events, bad;
static void sink(void *ctx, const masif_audio_event_t *e)
{
    (void)ctx;
    events++;
    if (e->track >= MASIF_TRACKS) bad++;
    if (e->type < MASIF_EVENT_NOTE_ON || e->type > MASIF_EVENT_RETRIGGER) bad++;
    if (e->type == MASIF_EVENT_NOTE_ON || e->type == MASIF_EVENT_RETRIGGER)
        if (e->instrument >= MASIF_INSTRUMENTS || e->velocity > 127u) bad++;
    if (e->pitch_offset > (48 + 48 + 16) * MASIF_PITCH_SEMI ||      /* a slide's most + a table's + an arpeggio's or a vibrato's */
        e->pitch_offset < -(48 + 48 + 2) * MASIF_PITCH_SEMI) bad++;
}

int main(int argc, char **argv)
{
    unsigned songs = argc > 1 ? (unsigned)atoi(argv[1]) : 300u, s, i, fixed_total = 0, again = 0;
    masif_song_t *song = malloc(sizeof *song);      /* (on the heap: the sanitizer's guard is right behind it) */
    masif_runtime_t rt;
    masif_edit_queue_t q;
    if (!song) return 2;
    for (s = 0; s < songs; s++) {
        unsigned char *raw = (unsigned char *)song;
        unsigned mode = s % 3u;
        /* 0: all random; 1: random cells and tables in a song that is otherwise in order (the player's own checks);
         * 2: mostly effect commands the player knows, with every parameter */
        if (mode == 0u) {
            for (i = 0; i < sizeof *song; i++) raw[i] = (unsigned char)rnd();
        } else {
            masif_song_clear(song);
            for (i = 0; i < sizeof song->phrase; i++) {
                unsigned char v = (unsigned char)rnd();
                if (mode == 2u && (i % sizeof(masif_cell_t) == 3u || i % sizeof(masif_cell_t) == 5u)) v = (unsigned char)(v % 24u);
                if (mode == 2u && i % sizeof(masif_cell_t) == 1u && (v & 1u)) v &= 127u;   /* (instruments that are there) */
                ((unsigned char *)song->phrase)[i] = v;
            }
            for (i = 0; i < sizeof song->table; i++) ((unsigned char *)song->table)[i] = (unsigned char)(mode == 2u && i % 4u == 2u ? rnd() % 24u : rnd());
            for (i = 0; i < sizeof song->instrument; i++) ((unsigned char *)song->instrument)[i] = (unsigned char)rnd();
            for (i = 0; i < MASIF_CHAINS; i++) {     /* chains of 0-16 phrases, songs of a few runs of chains */
                unsigned k, steps = rnd() % 17u;
                for (k = 0; k < steps; k++) (void)masif_set_chain_step(song, (uint8_t)i, (uint8_t)k, (uint8_t)(rnd() % MASIF_PHRASES), (int8_t)(rnd() % 25u) - 12);
            }
            for (i = 0; i < 40u; i++)
                (void)masif_set_song_cell(song, (uint8_t)(rnd() % (s & 1u ? 12u : 256u)), (uint8_t)(rnd() % MASIF_TRACKS), (uint8_t)(rnd() % MASIF_CHAINS));
        }
        if (s & 2u) { unsigned g; for (g = 0; g < sizeof song->groove; g++) (&song->groove[0][0])[g] = (uint8_t)rnd(); }   /* any grooves */
        fixed_total += masif_song_sanitize(song);
        again += masif_song_sanitize(song);         /* (once is enough) */
        masif_edit_queue_init(&q);
        masif_runtime_init(&rt, song, 1200u);
        masif_runtime_restart(&rt);
        for (i = 0; i < 3000u; i++) {
            if (rnd() % 50u == 0u) {               /* any edit of any part, with any values */
                uint8_t d[7];
                unsigned k;
                for (k = 0; k < 7u; k++) d[k] = (uint8_t)rnd();
                (void)masif_post_edit(&q, (uint8_t)(rnd() % 8u), (uint8_t)rnd(), (uint8_t)rnd(), d);
            }
            if (rnd() % 700u == 0u) masif_runtime_start(&rt, (uint8_t)(rnd() % 4u), (uint8_t)rnd(), (uint8_t)rnd());
            (void)masif_runtime_ticks(&rt, (uint8_t)(rnd() % 9u), &q, sink, 0);
            if (rt.ticks_per_row < 1u || rt.ticks_per_row > 96u) bad++;
            for (unsigned t = 0; t < MASIF_TRACKS; t++)
                if (rt.row_tick[t] > rt.row_len[t] || rt.groove[t] >= MASIF_GROOVES || rt.groove_pos[t] > MASIF_GROOVE_STEPS ||
                    rt.tracker.row[t] >= MASIF_ROWS || rt.tracker.step[t] >= MASIF_CHAIN_STEPS ||
                    (rt.tracker.chain[t] >= MASIF_CHAINS && rt.tracker.chain[t] != MASIF_NONE) ||
                    rt.pitch_offset[t] > MASIF_PITCH_MAX || rt.pitch_offset[t] < -MASIF_PITCH_MAX ||
                    rt.table_row[t] >= MASIF_TABLE_ROWS || rt.table_id[t] >= MASIF_TABLES || rt.active_instrument[t] >= MASIF_INSTRUMENTS) bad++;
        }
    }
    free(song);
    printf("fuzz: %u songs, %lu events, %u values put in range by the sanitizer (%u on a second pass), %lu out of range\n",
           songs, events, fixed_total, again, bad);
    puts(bad || again ? "MASIF song fuzz: FAIL" : "MASIF song fuzz: PASS");
    return bad || again ? 1 : 0;
}
