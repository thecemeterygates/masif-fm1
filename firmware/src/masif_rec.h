/* SPDX-License-Identifier: GPL-3.0-only */
/* The pool keeper's and the recorder's words for the screens (masif_rec.c, included after them). */
#ifndef MASIF_REC_H
#define MASIF_REC_H
#include <stdint.h>
enum { MR_IDLE, MR_ARMED, MR_TAKE, MR_SAVING, MR_SAVED, MR_FAILED };
enum { MR_USB, MR_MIX };                         /* the source */
enum { MR_LEVEL, MR_PLAY, MR_NOW };              /* what starts the take: a sound over the level, the song, at once */
enum { MR_B12, MR_B8, MR_B4 };                   /* how a take is kept: 12 bit, 8 bit, 4-bit ADPCM */
enum { MR_WHY_NONE, MR_WHY_FULL, MR_WHY_ROOM, MR_WHY_FLASH, MR_WHY_SLOW, MR_WHY_DIR, MR_WHY_EMPTY };
#define MR_NTHR 10u
static const uint16_t MR_THR[MR_NTHR] = {16423, 8231, 4125, 2068, 1036, 519, 260, 130, 65, 33};   /* -6 dB .. -60 dB, 16 bit */
static int mr_arm(uint32_t src, uint32_t start, uint32_t thr, uint32_t sh, uint32_t bits, uint32_t max_s);
static void mr_stop(void);
static int mr_busy(void);
static uint32_t mr_state_now(void);
static uint32_t mr_why_now(void);
static uint32_t mr_slot_now(void);
static const char *mr_name_now(void);
static uint32_t mr_gaps_now(void);
static uint32_t mr_wmax_now(void);
static void mr_listen(int on);
static uint32_t mr_level(void);
static uint32_t mr_take_ms(void);
static uint32_t mr_cap_ms(void);
static uint32_t mr_save_pct(void);
static uint32_t mr_room_ms(uint32_t sh, uint32_t bits);
static uint32_t mr_free(uint32_t *at, uint32_t *longest);
static int mr_rename(uint32_t k, const char *name);
static int mr_delete(uint32_t k);
#endif
