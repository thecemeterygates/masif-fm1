/* MASIF's song store on the FM-1 (masif_store.c on the device's flash), and its command of the USB link. Included
 * by zp12.c after the adapter (masif_song, masif_fm1_song_changed) and before sp_link.c; the host's link test
 * (tests/zp12_link_host.c) includes it the same way.
 * The pool: what the samples do not get of the data flash and nothing else uses: 0xDA000-0xDFFFF, 0xE5000-0xE8FFF
 * and 0xFC000-0xFEFFF, 13 sectors (52 KiB). A song is a few KB as a rule, the largest possible one 53 KB. */
#include "masif_store.h"

static const masif_store_region_t masif_fm1_pool[3] = {{0xDA000u, 0xE0000u}, {0xE5000u, 0xE9000u}, {0xFC000u, 0xFF000u}};

int masif_fl_read(uint32_t off, uint8_t *dst, uint32_t n) { return st_read(off, dst, n); }
/* (the watchdog is fed here: a big song's save is up to 26 erases, each up to 0.4 s by the flash's sheet, and the
 * watchdog's time is 8 s; sp_store.c does the same) */
int masif_fl_erase(uint32_t off) { uint32_t took; fm1_wdt_feed(); return fl_erase4k_quiet(off, &took); }
int masif_fl_write(uint32_t off, const uint8_t *src, uint32_t n) { fm1_wdt_feed(); return fl_write(off, src, n); }

static void masif_fm1_store_init(void) { masif_store_init(masif_fm1_pool, 3); }

/* The link's SONG command (sp_link.c, 10): op 0 LIST -> a slot's {used, sectors, length:2, name[8]} x 16, then the
 * free sectors (not the working copy's slot: the screens'); 1 SAVE (data: the name) the song in RAM into the slot; 2 LOAD the slot into RAM; 3 DELETE.
 * Only while the transport is stopped (the audio must not read a song that is being replaced, and an erase stops
 * the audio for its time). Returns the link's rc: 0 done, 6 playing, 10 + what masif_store says. */
static uint32_t masif_fm1_song_cmd(uint32_t op, uint32_t slot, const uint8_t *in, uint32_t n, uint8_t *out, uint32_t *outn)
{
    int rc = MASIF_STORE_OK;
    uint32_t i, k;
    *outn = 0;
    if (op == 0u) {
        for (i = 0; i < MASIF_STORE_NAMED; i++) {
            masif_store_info_t info;
            (void)masif_store_info((uint8_t)i, &info);
            out[i * 12u] = info.used; out[i * 12u + 1u] = info.sectors;
            out[i * 12u + 2u] = (uint8_t)info.length; out[i * 12u + 3u] = (uint8_t)(info.length >> 8);
            for (k = 0; k < 8u && info.name[k]; k++) out[i * 12u + 4u + k] = (uint8_t)info.name[k];
            for (; k < 8u; k++) out[i * 12u + 4u + k] = 0;
        }
        out[MASIF_STORE_NAMED * 12u] = (uint8_t)masif_store_free();
        *outn = MASIF_STORE_NAMED * 12u + 1u;
        return 0;
    }
    if (slot >= MASIF_STORE_NAMED) return 10u + (uint32_t)-MASIF_STORE_BAD_SLOT;   /* (the working copy is the screens') */
    if (!MASIF_TRANSPORT_STOPPED()) return 6;
    if (op == 1u) {
        char name[9];
        for (i = 0; i < 8u && i < n && in[i] >= 32u && in[i] < 127u; i++) name[i] = (char)in[i];
        name[i] = 0;
        rc = masif_store_save(&masif_song, (uint8_t)slot, name);
    } else if (op == 2u) {
        rc = masif_store_load(&masif_song, (uint8_t)slot);
        if (rc != MASIF_STORE_EMPTY && rc != MASIF_STORE_BAD_SLOT) masif_fm1_song_changed();
    } else if (op == 3u) {
        rc = masif_store_delete((uint8_t)slot);
    } else {
        return 7;
    }
    return rc == MASIF_STORE_OK ? 0u : 10u + (uint32_t)-rc;
}
