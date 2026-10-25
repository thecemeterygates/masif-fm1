/* MASIF song store: songs as files (masif_songfile.c) in a pool of flash sectors. Portable: the flash is the
 * host's three functions below. */
#ifndef MASIF_STORE_H
#define MASIF_STORE_H
#include "masif_songfile.h"

#define MASIF_STORE_NAMED 16u                    /* the songs one saves and loads: numbers 0..15 */
#define MASIF_STORE_WORK 16u                     /* and one more: the song being worked on, kept by itself (MASIF_UI) */
#define MASIF_STORE_SLOTS 17u
#define MASIF_STORE_SECT 4096u
#define MASIF_STORE_HEAD 32u
#define MASIF_STORE_ROOM (MASIF_STORE_SECT - MASIF_STORE_HEAD)   /* a sector's share of a song */
#define MASIF_STORE_MAXSECT 32u                  /* the pool at the most */

/* The flash, from the host: 0 = done. Offsets are the flash's; erase takes a sector's first byte. */
int masif_fl_read(uint32_t off, uint8_t *dst, uint32_t n);
int masif_fl_erase(uint32_t off);
int masif_fl_write(uint32_t off, const uint8_t *src, uint32_t n);

typedef struct { uint32_t lo, hi; } masif_store_region_t;   /* [lo, hi), whole sectors */
typedef struct { uint8_t used, sectors; uint16_t length; char name[9]; } masif_store_info_t;

enum { MASIF_STORE_OK = 0, MASIF_STORE_BAD_SLOT = -1, MASIF_STORE_FULL = -2, MASIF_STORE_FLASH = -3,
       MASIF_STORE_REFUSED = -4, MASIF_STORE_EMPTY = -5, MASIF_STORE_DAMAGED = -6 };

/* Reads the pool (at power-on, and after anything else wrote into it). The regions must stay where they are. */
void masif_store_init(const masif_store_region_t *regions, uint32_t count);
/* What is in a slot; 0 if the slot number is none. */
int masif_store_info(uint8_t slot, masif_store_info_t *out);
/* Sectors that hold nothing a slot needs. */
uint32_t masif_store_free(void);
/* The song into the slot, in place of what was there. The old one stays good until the new one is whole: a power
 * cut leaves one of the two. FULL: not enough free sectors beside the old one (delete something). */
int masif_store_save(const masif_song_t *song, uint8_t slot, const char *name);
/* The slot into the song. EMPTY: nothing there, the song is untouched. DAMAGED: the song is empty. */
int masif_store_load(masif_song_t *song, uint8_t slot);
int masif_store_delete(uint8_t slot);
/* 1: the 80 bytes that end 176 before the 4 KiB boundary above `off` would be taken for an update record by the
 * SPL and the loader (bytes 6-7 0x5441, a CRC16 that fits): nothing may leave that in the flash. */
int masif_flash_update_lookalike(uint32_t off);
#endif
