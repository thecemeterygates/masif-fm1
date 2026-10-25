/* MASIF song file, portable. All little endian:
 *   0  'M' 'S' 'F', version (2)
 *   4  u16 length of the whole file, u32 CRC-32 of everything after these 10 bytes
 *  10  the project: tempo, the 4 track levels, the master EQ's 3, the key, the scale
 *      u16 n, n x {song row, its 8 chains}          (the song rows that hold a chain)
 *      n, n x {chain, steps, steps x {phrase, transpose}}        (the chains that hold something, to their last step)
 *      n, n x {instrument number, its bytes}        (the instruments that are not as an empty song has them)
 *      n, n x {table number, rows, rows x {transpose, volume, effect, parameter, effect 2, parameter 2}}
 *                                                   (the tables that hold something)
 *      n, n x {phrase, k, k x {row, mask, the cell's fields the mask names}}        (the phrases that hold something)
 *          mask: 1 note, 2 instrument, 4 volume, 8 the first effect + parameter, 16 the second; a field not
 *          named is empty
 *          k = 0xFF: the phrase as it is, 16 x 7 bytes (when that is shorter)
 * An empty song is 26 bytes; a note with instrument and volume is 5. */
#include "masif_songfile.h"

#define MASIF_FILE_HEAD 10u
#define MASIF_PAT_RAW 0xFFu

static uint8_t masif_cell_mask(const masif_cell_t *c)
{
    return (uint8_t)((c->note != MASIF_NOTE_NONE ? 1u : 0u) | (c->instrument != 0xFFu ? 2u : 0u) |
                     (c->volume != 0xFFu ? 4u : 0u) | (c->fx || c->param ? 8u : 0u) | (c->fx2 || c->param2 ? 16u : 0u));
}

/* a table's rows up to the last that holds something (0: an empty table) */
static uint32_t masif_table_rows(const masif_table_cell_t *tb)
{
    uint32_t r = MASIF_TABLE_ROWS;
    while (r && !tb[r - 1u].transpose && tb[r - 1u].volume == 0xFFu && !tb[r - 1u].fx && !tb[r - 1u].param &&
           !tb[r - 1u].fx2 && !tb[r - 1u].param2) r--;
    return r;
}

static int masif_phrase_used(const masif_cell_t *c)
{
    uint32_t r;
    for (r = 0; r < MASIF_ROWS; r++) if (masif_cell_mask(&c[r])) return 1;
    return 0;
}
static uint32_t masif_chain_steps(const masif_chain_step_t *c)
{
    uint32_t n = MASIF_CHAIN_STEPS;
    while (n && c[n - 1u].phrase == MASIF_NONE && !c[n - 1u].transpose) n--;
    return n;
}
static int masif_song_row_used(const uint8_t *row)
{
    uint32_t t;
    for (t = 0; t < MASIF_TRACKS; t++) if (row[t] != MASIF_NONE) return 1;
    return 0;
}

/* The file's bytes after its 10-byte head, one by one into put(). */
static void masif_song_body(const masif_song_t *song, masif_put_fn put, void *ctx)
{
    uint32_t i, t, r, pass;
#define PUT(v) put(ctx, (uint8_t)(v))
    PUT(song->tempo);
    for (t = 0; t < MASIF_TRACKS; t++) PUT(song->track_level[t]);
    for (t = 0; t < 3u; t++) PUT(song->master_eq[t]);
    PUT(song->key); PUT(song->scale); PUT(song->project_groove);
    for (pass = 0; pass < 2u; pass++) {             /* how many, then those */
        uint32_t n = 0;
        for (i = 0; i < MASIF_SONG_ROWS; i++) {
            if (!masif_song_row_used(song->song[i])) continue;
            n++;
            if (pass) { PUT(i); for (t = 0; t < MASIF_TRACKS; t++) PUT(song->song[i][t]); }
        }
        if (!pass) { PUT(n); PUT(n >> 8); }
    }
    for (pass = 0; pass < 2u; pass++) {
        uint32_t n = 0;
        for (i = 0; i < MASIF_CHAINS; i++) {
            uint32_t steps = masif_chain_steps(song->chain[i]);
            if (!steps) continue;
            n++;
            if (pass) { PUT(i); PUT(steps); for (r = 0; r < steps; r++) { PUT(song->chain[i][r].phrase); PUT(song->chain[i][r].transpose); } }
        }
        if (!pass) PUT(n);
    }
    for (pass = 0; pass < 2u; pass++) {
        uint32_t n = 0;
        for (i = 0; i < MASIF_INSTRUMENTS; i++) {
            masif_instrument_t def;
            const uint8_t *x = (const uint8_t *)&song->instrument[i], *d = (const uint8_t *)&def;
            masif_instrument_default(&def, (uint8_t)i);
            for (r = 0; r < sizeof def && x[r] == d[r]; r++) {}
            if (r == sizeof def) continue;
            n++;
            if (pass) { PUT(i); for (r = 0; r < sizeof def; r++) PUT(x[r]); }
        }
        if (!pass) PUT(n);
    }
    for (pass = 0; pass < 2u; pass++) {
        uint32_t n = 0;
        for (i = 0; i < MASIF_TABLES; i++) {
            uint32_t rows = masif_table_rows(song->table[i]);
            if (!rows) continue;
            n++;
            if (pass) {
                PUT(i); PUT(rows);
                for (r = 0; r < rows; r++) {
                    PUT(song->table[i][r].transpose); PUT(song->table[i][r].volume);
                    PUT(song->table[i][r].fx); PUT(song->table[i][r].param);
                    PUT(song->table[i][r].fx2); PUT(song->table[i][r].param2);
                }
            }
        }
        if (!pass) PUT(n);
    }
    for (pass = 0; pass < 2u; pass++) {
        uint32_t n = 0;
        for (i = 0; i < MASIF_PHRASES; i++) {
            const masif_cell_t *c = song->phrase[i];
            uint32_t k = 0, size = 0;
            if (!masif_phrase_used(c)) continue;
            n++;
            if (!pass) continue;
            for (r = 0; r < MASIF_ROWS; r++) {
                uint8_t m = masif_cell_mask(&c[r]);
                if (m) { k++; size += 2u + (m & 1u) + ((m >> 1) & 1u) + ((m >> 2) & 1u) + ((m & 8u) ? 2u : 0u) + ((m & 16u) ? 2u : 0u); }
            }
            PUT(i);
            if (size >= MASIF_ROWS * 7u) {
                PUT(MASIF_PAT_RAW);
                for (r = 0; r < MASIF_ROWS; r++) { PUT(c[r].note); PUT(c[r].instrument); PUT(c[r].volume); PUT(c[r].fx); PUT(c[r].param); PUT(c[r].fx2); PUT(c[r].param2); }
                continue;
            }
            PUT(k);
            for (r = 0; r < MASIF_ROWS; r++) {
                uint8_t m = masif_cell_mask(&c[r]);
                if (!m) continue;
                PUT(r); PUT(m);
                if (m & 1u) PUT(c[r].note);
                if (m & 2u) PUT(c[r].instrument);
                if (m & 4u) PUT(c[r].volume);
                if (m & 8u) { PUT(c[r].fx); PUT(c[r].param); }
                if (m & 16u) { PUT(c[r].fx2); PUT(c[r].param2); }
            }
        }
        if (!pass) PUT(n);
    }
    for (pass = 0; pass < 2u; pass++) {             /* the grooves that are not an empty song's */
        uint32_t n = 0;
        for (i = 0; i < MASIF_GROOVES; i++) {
            const uint8_t *g = song->groove[i];
            for (r = 2; r < MASIF_GROOVE_STEPS && g[r] == 0xFFu; r++) ;
            if (g[0] == 6u && g[1] == 6u && r == MASIF_GROOVE_STEPS) continue;
            n++;
            if (pass) { PUT(i); for (r = 0; r < MASIF_GROOVE_STEPS; r++) PUT(g[r]); }
        }
        if (!pass) PUT(n);
    }
#undef PUT
}

/* a CRC-32 that takes its bytes one at a time (start with 0xFFFFFFFF, invert at the end) */
static uint32_t masif_crc_step(uint32_t c, uint8_t b)
{
    uint32_t k;
    c ^= b;
    for (k = 0; k < 8u; k++)
        c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    return c;
}

typedef struct { uint32_t n, crc; masif_put_fn put; void *ctx; } masif_count_t;
static void masif_count_put(void *ctx, uint8_t b)
{
    masif_count_t *c = ctx;
    c->n++;
    c->crc = masif_crc_step(c->crc, b);
    if (c->put) c->put(c->ctx, b);
}

uint32_t masif_song_pack_to(const masif_song_t *song, masif_put_fn put, void *ctx)
{
    masif_count_t c = {0, 0xFFFFFFFFu, 0, 0};
    uint32_t len, crc;
    if (!song) return 0;
    masif_song_body(song, masif_count_put, &c);     /* once for the length and the checksum, */
    len = c.n + MASIF_FILE_HEAD;
    crc = ~c.crc;
    if (len > 0xFFFFu) return 0;
    if (!put) return len;
    put(ctx, 'M'); put(ctx, 'S'); put(ctx, 'F'); put(ctx, MASIF_FILE_VERSION);
    put(ctx, (uint8_t)len); put(ctx, (uint8_t)(len >> 8));
    put(ctx, (uint8_t)crc); put(ctx, (uint8_t)(crc >> 8)); put(ctx, (uint8_t)(crc >> 16)); put(ctx, (uint8_t)(crc >> 24));
    masif_song_body(song, put, ctx);                /* and once more for the bytes */
    return len;
}

typedef struct { uint8_t *out; uint32_t max, w; } masif_mem_t;
static void masif_mem_put(void *ctx, uint8_t b)
{
    masif_mem_t *m = ctx;
    if (m->w < m->max) m->out[m->w] = b;
    m->w++;
}

uint32_t masif_song_pack(const masif_song_t *song, uint8_t *out, uint32_t max)
{
    masif_mem_t m = {out, max, 0};
    uint32_t len;
    if (!song || !out) return 0;
    len = masif_song_pack_to(song, 0, 0);
    if (!len || len > max) return 0;                /* (nothing is written if it does not fit) */
    return masif_song_pack_to(song, masif_mem_put, &m);
}

/* The file's body from get(): 1 if it is a song's and ends where `left` says. */
typedef struct { masif_get_fn get; void *ctx; uint32_t left, crc; int bad; } masif_src_t;
static uint8_t masif_src_get(masif_src_t *s)
{
    int v;
    if (!s->left || s->bad) { s->bad = 1; return 0; }
    v = s->get(s->ctx);
    if (v < 0) { s->bad = 1; return 0; }
    s->left--;
    s->crc = masif_crc_step(s->crc, (uint8_t)v);
    return (uint8_t)v;
}

static int masif_song_read(masif_song_t *song, masif_src_t *src, uint32_t version)
{
    uint32_t i, t, p, r, n, k;
#define GET(dst) do { (dst) = masif_src_get(src); if (src->bad) return 0; } while (0)
    GET(song->tempo);
    for (t = 0; t < MASIF_TRACKS; t++) GET(song->track_level[t]);
    for (t = 0; t < 3u; t++) { GET(k); song->master_eq[t] = (int8_t)k; }
    GET(song->key); GET(song->scale); GET(song->project_groove);
    GET(n); GET(k);
    n |= k << 8;
    if (n > MASIF_SONG_ROWS) return 0;
    for (i = 0; i < n; i++) {
        GET(p);
        for (t = 0; t < MASIF_TRACKS; t++) GET(song->song[p][t]);
    }
    GET(n);
    if (n > MASIF_CHAINS) return 0;
    for (i = 0; i < n; i++) {
        GET(p); GET(k);
        if (p >= MASIF_CHAINS || !k || k > MASIF_CHAIN_STEPS) return 0;
        for (r = 0; r < k; r++) { GET(song->chain[p][r].phrase); GET(t); song->chain[p][r].transpose = (int8_t)t; }
    }
    GET(n);
    if (n > MASIF_INSTRUMENTS) return 0;
    for (i = 0; i < n; i++) {
        uint8_t *x;
        GET(p);
        if (p >= MASIF_INSTRUMENTS) return 0;
        x = (uint8_t *)&song->instrument[p];
        for (r = 0; r < (version == 6u ? MASIF_FILE_V6_INST : (uint32_t)sizeof(masif_instrument_t)); r++) GET(x[r]);
    }
    GET(n);
    if (n > MASIF_TABLES) return 0;
    for (i = 0; i < n; i++) {
        uint32_t rows;
        GET(p); GET(rows);
        if (p >= MASIF_TABLES || !rows || rows > MASIF_TABLE_ROWS) return 0;
        for (r = 0; r < rows; r++) {
            GET(t); song->table[p][r].transpose = (int8_t)t;
            GET(song->table[p][r].volume); GET(song->table[p][r].fx); GET(song->table[p][r].param);
            GET(song->table[p][r].fx2); GET(song->table[p][r].param2);
        }
    }
    GET(n);
    if (n > MASIF_PHRASES) return 0;
    for (i = 0; i < n; i++) {
        masif_cell_t *c;
        GET(p); GET(k);
        if (p >= MASIF_PHRASES) return 0;
        c = song->phrase[p];
        if (k == MASIF_PAT_RAW) {
            for (r = 0; r < MASIF_ROWS; r++) { GET(c[r].note); GET(c[r].instrument); GET(c[r].volume); GET(c[r].fx); GET(c[r].param); GET(c[r].fx2); GET(c[r].param2); }
            continue;
        }
        if (k > MASIF_ROWS) return 0;
        for (t = 0; t < k; t++) {
            uint32_t m;
            GET(r); GET(m);
            if (r >= MASIF_ROWS || m > 31u || !m) return 0;
            if (m & 1u) GET(c[r].note);
            if (m & 2u) GET(c[r].instrument);
            if (m & 4u) GET(c[r].volume);
            if (m & 8u) { GET(c[r].fx); GET(c[r].param); }
            if (m & 16u) { GET(c[r].fx2); GET(c[r].param2); }
        }
    }
    GET(n);
    if (n > MASIF_GROOVES) return 0;
    for (i = 0; i < n; i++) {
        GET(p);
        if (p >= MASIF_GROOVES) return 0;
        for (r = 0; r < MASIF_GROOVE_STEPS; r++) GET(song->groove[p][r]);
    }
#undef GET
    return src->left == 0u;                         /* (nothing left over) */
}

int masif_song_unpack_from(masif_song_t *song, masif_get_fn get, void *ctx, uint32_t n)
{
    masif_src_t src = {get, ctx, MASIF_FILE_HEAD, 0, 0};
    uint8_t h[MASIF_FILE_HEAD];
    uint32_t len, crc, i;
    if (!song) return 0;
    masif_song_clear(song);
    if (!get || n < MASIF_FILE_HEAD) return 0;
    for (i = 0; i < MASIF_FILE_HEAD; i++) h[i] = masif_src_get(&src);
    len = h[4] | (uint32_t)h[5] << 8;
    crc = h[6] | (uint32_t)h[7] << 8 | (uint32_t)h[8] << 16 | (uint32_t)h[9] << 24;
    if (src.bad || h[0] != 'M' || h[1] != 'S' || h[2] != 'F' || (h[3] != MASIF_FILE_VERSION && h[3] != 6u) || len < MASIF_FILE_HEAD || len > n)
        return 0;
    src.left = len - MASIF_FILE_HEAD;
    src.crc = 0xFFFFFFFFu;
    /* read into the song as it comes (there is no room for the file beside it), the checksum at the end: a
     * file that fails either way leaves the empty song */
    if (!masif_song_read(song, &src, h[3]) || ~src.crc != crc) {
        masif_song_clear(song);
        return 0;
    }
    (void)masif_song_sanitize(song);
    return 1;
}

typedef struct { const uint8_t *in; uint32_t n, r; } masif_mem_src_t;
static int masif_mem_get(void *ctx)
{
    masif_mem_src_t *m = ctx;
    return m->r < m->n ? m->in[m->r++] : -1;
}

int masif_song_unpack(masif_song_t *song, const uint8_t *in, uint32_t n)
{
    masif_mem_src_t m = {in, n, 0};
    if (!in) { if (song) masif_song_clear(song); return 0; }
    return masif_song_unpack_from(song, masif_mem_get, &m, n);
}
