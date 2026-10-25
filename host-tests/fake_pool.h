/* The sample pool on the host: the flash as RAM (the samples' rooms, the directory), and what sp_samples.c and
 * masif_rec.c take from zp12.c and the HAL. Included by the screen tests before masif_ui.c (after sp_core.c,
 * sp_seq.c and the tracker), so the pages work the real pool keeper and recorder. */
#include <stdint.h>
#include <string.h>
#ifndef FELUCCA_UAC_OUT
#define FELUCCA_UAC_OUT 0
#endif
#define MASIF_APP_ROOM 0
#define SP_NUSER 24
#define KIT_NWAVE 0
static uint8_t hf_flash[0x100000];               /* (the whole part: the directory at 0xD8000, the rooms) */
static uint32_t hf_erases, hf_programs, hf_fail_at = 0xFFFFFFFFu;
static uint32_t hf_ticks;
static uint8_t hf_hush, hf_silent;               /* masif_rec.c's MR_HUSH: silent at the third look after it; */
static uint32_t hf_loud_erases;                  /* a sample room's sector erased with the sound not silent */
static uint32_t hf_loud_programs;                /* a sample room's page programmed with the sound not silent */
#define MR_HUSH(on) ((void)((hf_hush = (uint8_t)!!(on)) ? 0 : (hf_silent = 0)))
#define MR_HUSHED() (hf_hush && ++hf_silent > 2u)
static uint8_t flash_ok = 1;
static char ui_uname[SP_NUSER][9];
static uint8_t ui_uflags[SP_NUSER];
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && (uint32_t)(n) <= (hi) - (uint32_t)(off))
#define FL_XIP(off) ((uintptr_t)(hf_flash + (off)))
#define FL_FAR(fn) fn
#define FM1_TICKS_PER_US 24u
static uint32_t fm1_ticks(void) { return hf_ticks += 24u * 50u; }   /* (50 us a look) */
static void fm1_wdt_feed(void) {}
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static int hf_ok(uint32_t off, uint32_t n)       /* only where MASIF's samples and their directory are */
{
    return FL_IN(off, n, 0x93000u, 0xC4000u) || FL_IN(off, n, 0xD8000u, 0xDA000u) || FL_IN(off, n, 0xEA000u, 0xFC000u);
}
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, hf_flash + off, n); return 0; }
static int fl_erase4k_quiet(uint32_t off, uint32_t *took)
{
    *took = 45000u;
    if ((off & 0xFFFu) || !hf_ok(off, 0x1000u) || off == hf_fail_at) return -1;
    memset(hf_flash + off, 0xFF, 0x1000u);
    hf_erases++;
    if ((!hf_hush || hf_silent < 3u) && !FL_IN(off, 0x1000u, 0xD8000u, 0xDA000u)) hf_loud_erases++;
    return 0;
}
static int fl_prog_ram(uint32_t off, const uint8_t *src, uint32_t n, uint32_t *took)
{
    uint32_t k;
    *took = 100u;
    if (!n || n > 256u || ((off & 0xFFu) + n) > 256u || !hf_ok(off, n)) return -1;
    for (k = 0; k < n; k++) hf_flash[off + k] &= src[k];   /* (as NOR: only 1 -> 0) */
    hf_programs++;
    if ((!hf_hush || hf_silent < 3u) && !FL_IN(off, n, 0xD8000u, 0xDA000u)) hf_loud_programs++;
    return 0;
}
static int fl_write(uint32_t off, const uint8_t *src, uint32_t n)
{
    uint32_t took;
    while (n) {
        uint32_t c = 256u - (off & 0xFFu);
        if (c > n) c = n;
        if (fl_prog_ram(off, src, c, &took)) return -1;
        off += c; src += c; n -= c;
    }
    return 0;
}
static uint32_t zs_crc(const uint8_t *p, uint32_t n)   /* (sp_store.c's) */
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
#include "sp_samples.c"
#include "masif_rec.c"
