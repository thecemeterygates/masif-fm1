/* MASIF song store, portable. A song's file is cut into the sectors it needs, anywhere in the pool; each sector
 * begins with 32 bytes:
 *    0  'M' 'S' 'S' '1'
 *    4  u32 generation (every save has the next one)
 *    8  slot, index of this sector in the song, sectors of the song, 0
 *   12  u32 length of the song's file
 *   16  name[8]
 *   24  u16 bytes of the file in this sector, u16 0
 *   28  u32 CRC-32 of those bytes of the file, then bytes 0-27
 * A copy counts when all its sectors are there and whole; a slot's copy with the highest generation is the song,
 * everything else is free room. A sector's head is written last, so a sector cut short is nothing; the old copy
 * is erased after the new one is whole. Called from the main loop only, the transport stopped and quiet (an
 * erase stops the audio for its ~50 ms). */
#include "masif_store.h"

typedef struct { uint8_t ok, slot, index, count; uint16_t bytes, length; uint32_t gen; char name[8]; } masif_sect_t;

static const masif_store_region_t *ms_region;
static uint32_t ms_regions, ms_nsect, ms_gen;
static masif_sect_t ms_sect[MASIF_STORE_MAXSECT];
static uint32_t ms_best[MASIF_STORE_SLOTS];      /* the generation that is the slot's song; 0: none */

static uint32_t ms_addr(uint32_t i)
{
    uint32_t r;
    for (r = 0; r < ms_regions; r++) {
        uint32_t n = (ms_region[r].hi - ms_region[r].lo) / MASIF_STORE_SECT;
        if (i < n) return ms_region[r].lo + i * MASIF_STORE_SECT;
        i -= n;
    }
    return 0;
}

static uint32_t ms_crc(uint32_t c, const uint8_t *p, uint32_t n)
{
    uint32_t k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}
static uint32_t ms_rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void ms_wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

int masif_flash_update_lookalike(uint32_t off)
{
    uint8_t r[80];
    uint32_t i, k, c = 0;
    if (masif_fl_read((off | 0xFFFu) + 1u - 0x100u, r, sizeof r)) return 0;
    if (r[6] != 0x41u || r[7] != 0x54u || !(r[0] | r[1])) return 0;
    for (i = 2; i < sizeof r; i++) {
        c ^= (uint32_t)r[i] << 8;
        for (k = 0; k < 8u; k++) c = c & 0x8000u ? ((c << 1) ^ 0x1021u) & 0xFFFFu : (c << 1) & 0xFFFFu;
    }
    return c == (r[0] | (uint32_t)r[1] << 8);
}

/* one sector as the flash has it: its head taken in if the sector is whole */
static void ms_read_sector(uint32_t i)
{
    masif_sect_t *s = &ms_sect[i];
    uint8_t h[MASIF_STORE_HEAD], b[128];
    uint32_t a = ms_addr(i), c, n, k;
    s->ok = 0;
    if (masif_fl_read(a, h, sizeof h) || h[0] != 'M' || h[1] != 'S' || h[2] != 'S' || h[3] != '1') return;
    n = h[24] | (uint32_t)h[25] << 8;
    if (h[8] >= MASIF_STORE_SLOTS || !h[10] || h[10] > MASIF_STORE_MAXSECT || h[9] >= h[10] || n > MASIF_STORE_ROOM ||
        !ms_rd32(h + 4) || ms_rd32(h + 12) > 0xFFFFu)
        return;
    c = 0xFFFFFFFFu;
    for (k = 0; k < n; k += sizeof b) {
        uint32_t m = n - k < sizeof b ? n - k : sizeof b;
        if (masif_fl_read(a + MASIF_STORE_HEAD + k, b, m)) return;
        c = ms_crc(c, b, m);
    }
    if (~ms_crc(c, h, 28) != ms_rd32(h + 28)) return;
    s->gen = ms_rd32(h + 4); s->slot = h[8]; s->index = h[9]; s->count = h[10];
    s->length = (uint16_t)ms_rd32(h + 12); s->bytes = (uint16_t)n;
    for (k = 0; k < 8u; k++) s->name[k] = (char)h[16 + k];
    s->ok = 1;
}

/* is the copy (slot, gen) all there: every index once */
static int ms_whole(uint32_t slot, uint32_t gen)
{
    uint32_t i, seen = 0, count = 0, total = 0;
    for (i = 0; i < ms_nsect; i++) {
        const masif_sect_t *s = &ms_sect[i];
        if (!s->ok || s->slot != slot || s->gen != gen) continue;
        if (!count) count = s->count;
        if (s->count != count || (seen >> s->index) & 1u) return 0;
        seen |= 1u << s->index;
        total += s->bytes;
        if (total > 0xFFFFu) return 0;
    }
    return count && seen == (count >= 32u ? 0xFFFFFFFFu : (1u << count) - 1u);
}

static void ms_index(void)
{
    uint32_t i, slot;
    ms_gen = 0;
    for (slot = 0; slot < MASIF_STORE_SLOTS; slot++) ms_best[slot] = 0;
    for (i = 0; i < ms_nsect; i++) {
        const masif_sect_t *s = &ms_sect[i];
        if (!s->ok) continue;
        if (s->gen > ms_gen) ms_gen = s->gen;
        if (s->gen > ms_best[s->slot] && ms_whole(s->slot, s->gen)) ms_best[s->slot] = s->gen;
    }
}

void masif_store_init(const masif_store_region_t *regions, uint32_t count)
{
    uint32_t r, i;
    ms_region = regions; ms_regions = count; ms_nsect = 0;
    for (r = 0; r < count; r++) ms_nsect += (regions[r].hi - regions[r].lo) / MASIF_STORE_SECT;
    if (ms_nsect > MASIF_STORE_MAXSECT) ms_nsect = MASIF_STORE_MAXSECT;
    for (i = 0; i < ms_nsect; i++) ms_read_sector(i);
    ms_index();
}

/* a sector that is part of some slot's song */
static int ms_kept(uint32_t i)
{
    return ms_sect[i].ok && ms_best[ms_sect[i].slot] == ms_sect[i].gen;
}

uint32_t masif_store_free(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < ms_nsect; i++) n += !ms_kept(i);
    return n;
}

int masif_store_info(uint8_t slot, masif_store_info_t *out)
{
    uint32_t i, k;
    if (slot >= MASIF_STORE_SLOTS || !out) return 0;
    out->used = 0; out->sectors = 0; out->length = 0; out->name[0] = 0;
    if (!ms_best[slot]) return 1;
    for (i = 0; i < ms_nsect; i++) {
        const masif_sect_t *s = &ms_sect[i];
        if (!s->ok || s->slot != slot || s->gen != ms_best[slot]) continue;
        out->used = 1; out->sectors = s->count; out->length = s->length;
        for (k = 0; k < 8u && s->name[k] >= 32 && s->name[k] < 127; k++) out->name[k] = s->name[k];
        out->name[k] = 0;
    }
    return 1;
}

/* ---- saving: the file's bytes as they come, into one sector after the other */
typedef struct {
    uint32_t sect[MASIF_STORE_MAXSECT];          /* the sectors it gets, in the file's order */
    uint32_t count, at, fill, crc, gen, length;
    uint8_t slot, name[8];
    uint8_t buf[256];
    uint32_t nbuf, boff;                         /* buf holds nbuf bytes for the sector's offset boff */
    int err;
} ms_writer_t;

static void ms_flush(ms_writer_t *w)
{
    if (w->nbuf && !w->err && masif_fl_write(ms_addr(w->sect[w->at]) + w->boff, w->buf, w->nbuf))
        w->err = MASIF_STORE_FLASH;
    w->boff += w->nbuf;
    w->nbuf = 0;
}

/* the sector's head, last: only now the sector is something */
static void ms_finish_sector(ms_writer_t *w)
{
    uint8_t h[MASIF_STORE_HEAD];
    uint32_t a = ms_addr(w->sect[w->at]), k;
    ms_flush(w);
    if (w->err) return;
    if (masif_flash_update_lookalike(a)) { w->err = MASIF_STORE_REFUSED; return; }
    h[0] = 'M'; h[1] = 'S'; h[2] = 'S'; h[3] = '1';
    ms_wr32(h + 4, w->gen);
    h[8] = w->slot; h[9] = (uint8_t)w->at; h[10] = (uint8_t)w->count; h[11] = 0;
    ms_wr32(h + 12, w->length);
    for (k = 0; k < 8u; k++) h[16 + k] = w->name[k];
    h[24] = (uint8_t)w->fill; h[25] = (uint8_t)(w->fill >> 8); h[26] = h[27] = 0;
    ms_wr32(h + 28, ~ms_crc(w->crc, h, 28));
    if (masif_fl_write(a, h, sizeof h)) w->err = MASIF_STORE_FLASH;
}

static void ms_put(void *ctx, uint8_t b)
{
    ms_writer_t *w = ctx;
    if (w->err) return;
    if (w->fill == MASIF_STORE_ROOM) {           /* this sector is full: on to the next */
        ms_finish_sector(w);
        if (w->err) return;
        if (++w->at >= w->count) { w->err = MASIF_STORE_FLASH; return; }
        w->fill = 0; w->crc = 0xFFFFFFFFu; w->boff = MASIF_STORE_HEAD;
    }
    w->buf[w->nbuf++] = b;
    w->crc = ms_crc(w->crc, &b, 1);
    w->fill++;
    if (((w->boff + w->nbuf) & 255u) == 0u) ms_flush(w);     /* (a flash page at a time) */
}

int masif_store_save(const masif_song_t *song, uint8_t slot, const char *name)
{
    static ms_writer_t w;                        /* (not on the main loop's stack) */
    uint32_t len, i, k;
    if (slot >= MASIF_STORE_SLOTS || !song) return MASIF_STORE_BAD_SLOT;
    len = masif_song_pack_to(song, 0, 0);
    if (!len) return MASIF_STORE_REFUSED;
    w.count = (len + MASIF_STORE_ROOM - 1u) / MASIF_STORE_ROOM;
    for (i = 0, k = 0; i < ms_nsect && k < w.count; i++)
        if (!ms_kept(i)) w.sect[k++] = i;
    if (k < w.count) return MASIF_STORE_FULL;
    w.err = 0;
    for (i = 0; i < w.count && !w.err; i++) {
        ms_sect[w.sect[i]].ok = 0;
        if (masif_fl_erase(ms_addr(w.sect[i]))) w.err = MASIF_STORE_FLASH;
    }
    w.at = 0; w.fill = 0; w.crc = 0xFFFFFFFFu; w.nbuf = 0; w.boff = MASIF_STORE_HEAD;
    w.gen = ms_gen + 1u; w.length = len; w.slot = slot;
    for (i = 0; i < 8u; i++) w.name[i] = 0;
    for (i = 0; name && i < 8u && name[i]; i++) w.name[i] = (uint8_t)name[i];
    if (!w.err) {
        (void)masif_song_pack_to(song, ms_put, &w);
        ms_finish_sector(&w);
    }
    for (i = 0; i < w.count && !w.err; i++) {    /* read back: every sector whole and this copy's */
        ms_read_sector(w.sect[i]);
        if (!ms_sect[w.sect[i]].ok || ms_sect[w.sect[i]].gen != w.gen || ms_sect[w.sect[i]].index != i) w.err = MASIF_STORE_FLASH;
    }
    if (w.err) {                                 /* nothing of a failed save stays */
        for (i = 0; i < w.count; i++) { (void)masif_fl_erase(ms_addr(w.sect[i])); ms_sect[w.sect[i]].ok = 0; }
        ms_index();
        return w.err;
    }
    ms_index();
    for (i = 0; i < ms_nsect; i++)               /* the copy before it: free room now */
        if (ms_sect[i].ok && ms_sect[i].slot == slot && ms_sect[i].gen != ms_best[slot]) {
            (void)masif_fl_erase(ms_addr(i));
            ms_sect[i].ok = 0;
        }
    return MASIF_STORE_OK;
}

/* ---- loading: the song's sectors in their order, a byte at a time */
typedef struct { uint32_t sect[MASIF_STORE_MAXSECT]; uint32_t count, at, pos; uint8_t buf[128]; uint32_t nbuf, rbuf; } ms_reader_t;

static int ms_get(void *ctx)
{
    ms_reader_t *r = ctx;
    if (r->rbuf == r->nbuf) {
        const masif_sect_t *s;
        uint32_t n;
        if (r->at >= r->count) return -1;
        s = &ms_sect[r->sect[r->at]];
        if (r->pos >= s->bytes) {
            r->at++; r->pos = 0;
            return ms_get(ctx);
        }
        n = s->bytes - r->pos < sizeof r->buf ? s->bytes - r->pos : sizeof r->buf;
        if (masif_fl_read(ms_addr(r->sect[r->at]) + MASIF_STORE_HEAD + r->pos, r->buf, n)) return -1;
        r->pos += n; r->nbuf = n; r->rbuf = 0;
    }
    return r->buf[r->rbuf++];
}

int masif_store_load(masif_song_t *song, uint8_t slot)
{
    static ms_reader_t r;
    uint32_t i, length = 0;
    if (slot >= MASIF_STORE_SLOTS || !song) return MASIF_STORE_BAD_SLOT;
    if (!ms_best[slot]) return MASIF_STORE_EMPTY;
    r.count = 0; r.at = 0; r.pos = 0; r.nbuf = r.rbuf = 0;
    for (i = 0; i < ms_nsect; i++) {
        const masif_sect_t *s = &ms_sect[i];
        if (!s->ok || s->slot != slot || s->gen != ms_best[slot]) continue;
        r.sect[s->index] = i;
        r.count = s->count;
        length = s->length;
    }
    return masif_song_unpack_from(song, ms_get, &r, length) ? MASIF_STORE_OK : MASIF_STORE_DAMAGED;
}

int masif_store_delete(uint8_t slot)
{
    uint32_t i;
    int rc = MASIF_STORE_OK;
    if (slot >= MASIF_STORE_SLOTS) return MASIF_STORE_BAD_SLOT;
    for (i = 0; i < ms_nsect; i++)
        if (ms_sect[i].ok && ms_sect[i].slot == slot) {
            if (masif_fl_erase(ms_addr(i))) rc = MASIF_STORE_FLASH;
            ms_sect[i].ok = 0;
        }
    ms_index();
    return rc;
}
