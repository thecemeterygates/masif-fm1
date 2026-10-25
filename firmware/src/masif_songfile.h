/* MASIF song file: the song as compact note data (no audio), for the flash and for the web app. */
#ifndef MASIF_SONGFILE_H
#define MASIF_SONGFILE_H
#include "masif_tracker.h"

#define MASIF_FILE_VERSION 7u           /* (2: an instrument with its four modulation slots; 3: the grooves; 4: a loop's end and STEPS, 56 bytes an instrument; TRIG slots;
                                         * 5: an empty song's instruments have no sample, so one left out of the file has none;
                                         * 6: the loop's two tapers, 58 bytes an instrument; 7: its name, 66 bytes: a 6 is still read) */
#define MASIF_FILE_V6_INST 58u          /* (an instrument's bytes in a version 6 file: all but the name) */
/* the most a song can pack to: every pattern full (stored as it is), every table full */
#define MASIF_FILE_MAX (10u + 7u + MASIF_TRACKS + 2u + MASIF_SONG_ROWS * (1u + MASIF_TRACKS) + \
                        1u + MASIF_CHAINS * (2u + MASIF_CHAIN_STEPS * 2u) + \
                        1u + MASIF_INSTRUMENTS * (1u + (uint32_t)sizeof(masif_instrument_t)) + \
                        1u + MASIF_TABLES * (2u + MASIF_TABLE_ROWS * 6u) + \
                        1u + MASIF_PHRASES * (2u + MASIF_ROWS * 7u) + \
                        1u + MASIF_GROOVES * (1u + MASIF_GROOVE_STEPS))

/* The same without a buffer for the whole file (the FM-1 has no RAM for one beside the song): the file's bytes
 * go to put() one by one; with put = 0 only the length is returned. From get() (the next byte, or -1 at the end)
 * a file of at most n bytes is read straight into the song. */
typedef void (*masif_put_fn)(void *ctx, uint8_t byte);
typedef int (*masif_get_fn)(void *ctx);
uint32_t masif_song_pack_to(const masif_song_t *song, masif_put_fn put, void *ctx);
int masif_song_unpack_from(masif_song_t *song, masif_get_fn get, void *ctx, uint32_t n);
/* Packs the song into out[0..max). Returns the file's length, 0 if it does not fit. */
uint32_t masif_song_pack(const masif_song_t *song, uint8_t *out, uint32_t max);
/* Unpacks a file into the song. 1: the song is the file's, in range (masif_song_sanitize). 0: not a song file,
 * damaged or cut short; the song is then empty (masif_song_clear), never half of something. */
int masif_song_unpack(masif_song_t *song, const uint8_t *in, uint32_t n);
#endif
