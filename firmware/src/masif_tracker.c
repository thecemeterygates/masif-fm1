/* MASIF tracker core: the song (song rows of chains, chains of phrases, phrases of cells) and the row player.
 * No platform or audio dependencies. */
#include "masif_tracker.h"
static void masif_tracker_zero(void *d, int c, unsigned n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
}

/* the scales as the semitones above the key that are in them (bit 0: the key itself) */
const char *const masif_scale_name[MASIF_SCALES] = {
    "CHROMATIC", "MAJOR", "MINOR", "DORIAN", "PHRYGIAN", "LYDIAN", "MIXOLYDIAN", "LOCRIAN",
    "HARM MINOR", "MEL MINOR", "MAJ PENTA", "MIN PENTA", "BLUES", "WHOLE TONE", "DIMINISHED", "PHRYG DOM"};
static const uint16_t masif_scale_mask[MASIF_SCALES] = {
    0xFFF,          /* chromatic */
    0xAB5,          /* major          0 2 4 5 7 9 11 */
    0x5AD,          /* natural minor  0 2 3 5 7 8 10 */
    0x6AD,          /* dorian         0 2 3 5 7 9 10 */
    0x5AB,          /* phrygian       0 1 3 5 7 8 10 */
    0xAD5,          /* lydian         0 2 4 6 7 9 11 */
    0x6B5,          /* mixolydian     0 2 4 5 7 9 10 */
    0x56B,          /* locrian        0 1 3 5 6 8 10 */
    0x9AD,          /* harmonic minor 0 2 3 5 7 8 11 */
    0xAAD,          /* melodic minor  0 2 3 5 7 9 11 */
    0x295,          /* major pentatonic 0 2 4 7 9 */
    0x4A9,          /* minor pentatonic 0 3 5 7 10 */
    0x4E9,          /* blues          0 3 5 6 7 10 */
    0x555,          /* whole tone     0 2 4 6 8 10 */
    0x6DB,          /* diminished (half, whole) 0 1 3 4 6 7 9 10 */
    0x5B3};         /* phrygian dominant 0 1 4 5 7 8 10 */

uint8_t masif_scale_note(const masif_song_t *song, uint8_t note)
{
    uint32_t mask, pc;
    if (!song || note < 1u || note > 120u || !song->scale || song->scale >= MASIF_SCALES) return note;
    mask = masif_scale_mask[song->scale];
    pc = (note + 12u - song->key % 12u) % 12u;
    while (!((mask >> pc) & 1u) && note > 1u) {     /* down to the next note of the scale (the key itself is in all) */
        note--;
        pc = (pc + 11u) % 12u;
    }
    return note;
}

void masif_instrument_default(masif_instrument_t *in, uint8_t i)
{
    masif_tracker_zero(in, 0, sizeof(*in));
    (void)i;
    in->sample = MASIF_NO_SAMPLE;                /* (the user, 2026-10-14: a new song's instruments are empty) */
    in->root_note = MASIF_DEFAULT_ROOT_NOTE;
    in->volume = 127u;
    in->decay = 127u;
    in->table_tic = 1u;
    in->cutoff = 127u;
    in->steps = 16u;
}

MASIF_SMALL void masif_song_clear(masif_song_t *song)   /* (not the audio's: small before fast) */
{
    unsigned i, r;
    if (!song) return;
    masif_tracker_zero(song, 0, sizeof(*song));
    masif_tracker_zero(song->song, MASIF_NONE, sizeof song->song);
    for (i = 0; i < MASIF_CHAINS; ++i)
        for (r = 0; r < MASIF_CHAIN_STEPS; ++r)
            song->chain[i][r].phrase = MASIF_NONE;
    for (i = 0; i < MASIF_PHRASES; ++i)
        for (r = 0; r < MASIF_ROWS; ++r) {
            song->phrase[i][r].instrument = 0xFFu;
            song->phrase[i][r].volume = 0xFFu;
        }
    for (i = 0; i < MASIF_INSTRUMENTS; ++i)
        masif_instrument_default(&song->instrument[i], (uint8_t)i);
    for (i = 0; i < MASIF_TABLES; ++i)
        for (r = 0; r < MASIF_TABLE_ROWS; ++r)
            song->table[i][r].volume = 0xFFu;
    for (i = 0; i < MASIF_TRACKS; ++i)
        song->track_level[i] = 100u;
    masif_tracker_zero(song->groove, 0xFFu, sizeof song->groove);
    for (i = 0; i < MASIF_GROOVES; ++i) song->groove[i][0] = song->groove[i][1] = 6u;
}

int masif_set_groove(masif_song_t *song, uint8_t groove, uint8_t step, uint8_t ticks)
{
    if (!song || groove >= MASIF_GROOVES || step >= MASIF_GROOVE_STEPS || (ticks > MASIF_GROOVE_MAX && ticks != 0xFFu)) return 0;
    song->groove[groove][step] = ticks;
    return 1;
}

MASIF_SMALL unsigned masif_song_sanitize(masif_song_t *song)
{
    unsigned fixed = 0, i, t;
    if (!song) return 0;
    for (i = 0; i < MASIF_INSTRUMENTS; ++i) {
        masif_instrument_t *in = &song->instrument[i];
        if (in->sample >= MASIF_SAMPLES && in->sample != MASIF_NO_SAMPLE) { in->sample = MASIF_NO_SAMPLE; fixed++; }
        if (in->root_note < 1u || in->root_note > 120u) { in->root_note = MASIF_DEFAULT_ROOT_NOTE; fixed++; }
        if (in->fine < -99 || in->fine > 99) { in->fine = 0; fixed++; }
        if (in->volume > 127u) { in->volume = 127u; fixed++; }
        if (in->pan < -64 || in->pan > 64) { in->pan = 0; fixed++; }
        if (in->play >= MASIF_PLAY_MODES) { in->play = MASIF_PLAY_FWD; fixed++; }
        if (in->taper_in > MASIF_TAPER_MAX) { in->taper_in = MASIF_TAPER_MAX; fixed++; }
        if (in->taper_out > MASIF_TAPER_MAX) { in->taper_out = MASIF_TAPER_MAX; fixed++; }
        if (in->slices > 64u) { in->slices = 64u; fixed++; }
        if (in->decay > 127u) { in->decay = 127u; fixed++; }
        for (t = 0; t < 3u; ++t) {
            if (in->send[t] > 127u) { in->send[t] = 127u; fixed++; }
            if (in->eq[t] < -64 || in->eq[t] > 64) { in->eq[t] = 0; fixed++; }
        }
        if (in->attack > 127u) { in->attack = 127u; fixed++; }
        if (in->sustain > 127u) { in->sustain = 127u; fixed++; }
        if (in->release > 127u) { in->release = 127u; fixed++; }
        if (in->filter >= MASIF_FILTERS) { in->filter = MASIF_FILTER_OFF; fixed++; }
        if (in->cutoff > 127u) { in->cutoff = 127u; fixed++; }
        if (in->resonance > 127u) { in->resonance = 127u; fixed++; }
        if (in->drive > 127u) { in->drive = 0u; fixed++; }
        for (t = 0; t < MASIF_NAME_LEN; ++t)
            if (!MASIF_NAME_OK(in->name[t])) { in->name[t] = ' '; fixed++; }
        for (t = 0; t < MASIF_MODS; ++t) {
            masif_mod_t *m = &in->mod[t];
            unsigned k;
            if (m->type >= MASIF_MOD_TYPES) { m->type = MASIF_MOD_OFF; fixed++; }
            if (m->dest >= MASIF_DESTS) { m->dest = MASIF_DEST_OFF; fixed++; }
            if (m->amount < -64 || m->amount > 64) { m->amount = 0; fixed++; }
            for (k = 0; k < 4u; ++k)
                if (m->p[k] > 127u && !(m->type == MASIF_MOD_TRIG && k == 3u && m->p[k] <= MASIF_TRIG_SRC_MAX)) { m->p[k] = 127u; fixed++; }
        }
    }
    for (i = 0; i < MASIF_SONG_ROWS; ++i)
        for (t = 0; t < MASIF_TRACKS; ++t)
            if (song->song[i][t] >= MASIF_CHAINS && song->song[i][t] != MASIF_NONE) { song->song[i][t] = MASIF_NONE; fixed++; }
    for (i = 0; i < MASIF_CHAINS; ++i)
        for (t = 0; t < MASIF_CHAIN_STEPS; ++t) {
            masif_chain_step_t *c = &song->chain[i][t];
            if (c->phrase >= MASIF_PHRASES && c->phrase != MASIF_NONE) { c->phrase = MASIF_NONE; fixed++; }
            if (c->transpose < -48 || c->transpose > 48) { c->transpose = 0; fixed++; }
        }
    if (song->tempo && (song->tempo < 40u || song->tempo > 240u)) { song->tempo = 0u; fixed++; }
    for (t = 0; t < MASIF_TRACKS; ++t)
        if (song->track_level[t] > 127u) { song->track_level[t] = 127u; fixed++; }
    for (t = 0; t < 3u; ++t)
        if (song->master_eq[t] < -64 || song->master_eq[t] > 64) { song->master_eq[t] = 0; fixed++; }
    for (t = 0; t < MASIF_GROOVES * MASIF_GROOVE_STEPS; t++) {
        uint8_t *g = &song->groove[0][0] + t;
        if (*g > MASIF_GROOVE_MAX && *g != 0xFFu) { *g = 0xFFu; fixed++; }
    }
    for (i = 0; i < MASIF_PHRASES; ++i)              /* (the effects taken out: gone from what is loaded) */
        for (t = 0; t < MASIF_ROWS; ++t) {
            masif_cell_t *c = &song->phrase[i][t];
            if (MASIF_FX_OUT(c->fx)) { c->fx = MASIF_FX_NONE; c->param = 0u; fixed++; }
            if (MASIF_FX_OUT(c->fx2)) { c->fx2 = MASIF_FX_NONE; c->param2 = 0u; fixed++; }
        }
    for (i = 0; i < MASIF_TABLES; ++i)
        for (t = 0; t < MASIF_TABLE_ROWS; ++t) {
            masif_table_cell_t *c = &song->table[i][t];
            if (MASIF_FX_OUT(c->fx)) { c->fx = MASIF_FX_NONE; c->param = 0u; fixed++; }
            if (MASIF_FX_OUT(c->fx2)) { c->fx2 = MASIF_FX_NONE; c->param2 = 0u; fixed++; }
        }
    if (song->project_groove >= MASIF_GROOVES) { song->project_groove = 0u; fixed++; }
    if (song->key > 11u) { song->key = 0u; fixed++; }
    if (song->scale >= MASIF_SCALES) { song->scale = 0u; fixed++; }
    return fixed;
}

void masif_tracker_init(masif_tracker_t *tr, masif_song_t *song)
{
    if (!tr) return;
    masif_tracker_zero(tr, 0, sizeof(*tr));
    tr->song = song;
    masif_tracker_start_song(tr, 0u);
}

int masif_set_instrument(masif_song_t *song, uint8_t instrument, const masif_instrument_t *config)
{
    unsigned m, k;
    if (config)
        for (m = 0; m < MASIF_NAME_LEN; ++m)
            if (!MASIF_NAME_OK(config->name[m])) return 0;
    if (config)
        for (m = 0; m < MASIF_MODS; ++m) {
            const masif_mod_t *x = &config->mod[m];
            if (x->type >= MASIF_MOD_TYPES || x->dest >= MASIF_DESTS || x->amount < -64 || x->amount > 64) return 0;
            for (k = 0; k < 4u; ++k)
                if (x->p[k] > 127u && !(x->type == MASIF_MOD_TRIG && k == 3u && x->p[k] <= MASIF_TRIG_SRC_MAX)) return 0;
        }
    if (!song || !config || instrument >= MASIF_INSTRUMENTS ||
        (config->sample >= MASIF_SAMPLES && config->sample != MASIF_NO_SAMPLE) || config->root_note < 1u || config->root_note > 120u ||
        config->volume > 127u || config->play >= MASIF_PLAY_MODES || config->slices > 64u || config->decay > 127u ||
        config->pan < -64 || config->pan > 64 || config->fine < -99 || config->fine > 99 ||
        config->send[0] > 127u || config->send[1] > 127u || config->send[2] > 127u ||
        config->attack > 127u || config->sustain > 127u || config->release > 127u || config->cutoff > 127u ||
        config->filter >= MASIF_FILTERS ||
        config->resonance > 127u || config->drive > 127u ||
        config->eq[0] < -64 || config->eq[0] > 64 || config->eq[1] < -64 || config->eq[1] > 64 ||
        config->eq[2] < -64 || config->eq[2] > 64 || config->taper_in > MASIF_TAPER_MAX || config->taper_out > MASIF_TAPER_MAX)
        return 0;
    song->instrument[instrument] = *config;
    return 1;
}

int masif_set_cell(masif_song_t *song, uint8_t phrase, uint8_t row, const masif_cell_t *cell)
{
    if (!song || !cell || phrase >= MASIF_PHRASES || row >= MASIF_ROWS) return 0;
    song->phrase[phrase][row] = *cell;
    return 1;
}

int masif_set_chain_step(masif_song_t *song, uint8_t chain, uint8_t step, uint8_t phrase, int8_t transpose)
{
    if (!song || chain >= MASIF_CHAINS || step >= MASIF_CHAIN_STEPS || (phrase >= MASIF_PHRASES && phrase != MASIF_NONE) ||
        transpose < -48 || transpose > 48)
        return 0;
    song->chain[chain][step].phrase = phrase;
    song->chain[chain][step].transpose = transpose;
    return 1;
}

int masif_set_song_cell(masif_song_t *song, uint8_t row, uint8_t track, uint8_t chain)
{
    if (!song || track >= MASIF_TRACKS || (chain >= MASIF_CHAINS && chain != MASIF_NONE)) return 0;
    song->song[row][track] = chain;                 /* (row: all 256 are there) */
    return 1;
}

int masif_set_table_cell(masif_song_t *song, uint8_t table, uint8_t row, masif_table_cell_t cell)
{
    if (!song || table >= MASIF_TABLES || row >= MASIF_TABLE_ROWS) return 0;
    song->table[table][row] = cell;
    return 1;
}

/* a track at the top of the chain its song row names */
static void enter_song_row(masif_tracker_t *tr, uint8_t t, uint8_t row)
{
    uint8_t chain = tr->song ? tr->song->song[row][t] : MASIF_NONE;
    tr->song_row[t] = row;
    tr->chain[t] = chain;
    tr->step[t] = 0u;
    tr->row[t] = 0u;
    tr->on[t] = (uint8_t)(chain < MASIF_CHAINS);
    tr->phrase[t] = tr->on[t] ? tr->song->chain[chain][0].phrase : MASIF_NONE;
}

void masif_tracker_start_song(masif_tracker_t *tr, uint8_t row)
{
    uint8_t t;
    if (!tr) return;
    tr->what = MASIF_PLAY_SONG;
    for (t = 0; t < MASIF_TRACKS; ++t) enter_song_row(tr, t, row);
}

static void rest_all(masif_tracker_t *tr)
{
    uint8_t t;
    for (t = 0; t < MASIF_TRACKS; ++t) { tr->on[t] = 0u; tr->step[t] = 0u; tr->row[t] = 0u; tr->chain[t] = tr->phrase[t] = MASIF_NONE; }
}

void masif_tracker_start_chain(masif_tracker_t *tr, uint8_t track, uint8_t chain)
{
    if (!tr || !tr->song || track >= MASIF_TRACKS || chain >= MASIF_CHAINS) return;
    rest_all(tr);
    tr->what = MASIF_PLAY_CHAIN;
    tr->chain[track] = chain;
    tr->phrase[track] = tr->song->chain[chain][0].phrase;
    tr->on[track] = 1u;
}

void masif_tracker_start_phrase(masif_tracker_t *tr, uint8_t track, uint8_t phrase)
{
    if (!tr || !tr->song || track >= MASIF_TRACKS || phrase >= MASIF_PHRASES) return;
    rest_all(tr);
    tr->what = MASIF_PLAY_PHRASE;
    tr->phrase[track] = phrase;
    tr->on[track] = 1u;
}

/* The track's phrase is over: its next one. In a chain the next step, or (the chain over: an empty step, or its
 * 16th done) the chain of the next song row; at the end of the track's run of chains back up to the run's first. */
static void next_phrase(masif_tracker_t *tr, uint8_t t)
{
    const masif_song_t *song = tr->song;
    uint8_t step;
    if (tr->what == MASIF_PLAY_PHRASE || tr->chain[t] >= MASIF_CHAINS) return;      /* (the same phrase again) */
    step = (uint8_t)(tr->step[t] + 1u);
    if (step < MASIF_CHAIN_STEPS && song->chain[tr->chain[t]][step].phrase != MASIF_NONE) {
        tr->step[t] = step;
        tr->phrase[t] = song->chain[tr->chain[t]][step].phrase;
        return;
    }
    if (tr->what == MASIF_PLAY_SONG) {
        uint32_t row = tr->song_row[t], next = row + 1u;
        if (next >= MASIF_SONG_ROWS || song->song[next][t] >= MASIF_CHAINS) {
            next = row;                             /* up to where this run of chains begins */
            while (next > 0u && song->song[next - 1u][t] < MASIF_CHAINS) next--;
        }
        tr->song_row[t] = (uint8_t)next;
        tr->chain[t] = song->song[next][t];
        if (tr->chain[t] >= MASIF_CHAINS) { tr->on[t] = 0u; return; }              /* (edited away under it) */
    }
    tr->step[t] = 0u;
    tr->phrase[t] = song->chain[tr->chain[t]][0].phrase;
}

void masif_tick_row(masif_tracker_t *tr, masif_row_event_t out[MASIF_TRACKS])
{
    uint8_t t;
    if (!tr || !out) return;
    for (t = 0; t < MASIF_TRACKS; ++t) masif_tick_track(tr, t, &out[t]);
}

void masif_tick_track(masif_tracker_t *tr, uint8_t t, masif_row_event_t *out)
{
    uint8_t flow, arg;
    if (!tr || !out || t >= MASIF_TRACKS) return;
    out -= t;                                       /* (the lines below are a loop's: out[t]) */
    do {
        masif_tracker_zero(&out[t], 0, sizeof(out[t]));
        out[t].instrument = 0xFFu;
        out[t].volume = 0xFFu;
        if (!tr->song || !tr->on[t]) break;
        if (tr->phrase[t] < MASIF_PHRASES) {        /* (a chain whose first step is empty: 16 rows of nothing) */
            const masif_cell_t *c = &tr->song->phrase[tr->phrase[t]][tr->row[t] % MASIF_ROWS];
            out[t].note = c->note; out[t].instrument = c->instrument; out[t].volume = c->volume;
            out[t].fx = c->fx; out[t].param = c->param; out[t].fx2 = c->fx2; out[t].param2 = c->param2;
            if (c->note >= 1u && c->note <= 120u && tr->chain[t] < MASIF_CHAINS) {  /* the chain step's transpose */
                int32_t n = (int32_t)c->note + tr->song->chain[tr->chain[t]][tr->step[t] % MASIF_CHAIN_STEPS].transpose;
                out[t].note = (uint8_t)(n < 1 ? 1 : n > 120 ? 120 : n);
            }
        }
        /* on: BREAK (HOP) to the next phrase at a row, or the next row. (In either effect column; the first) */
        {
            uint8_t first = out[t].fx == MASIF_FX_BREAK;
            flow = first ? out[t].fx : out[t].fx2;
            arg = first ? out[t].param : out[t].param2;
        }
        if (flow == MASIF_FX_BREAK) {
            next_phrase(tr, t);
            tr->row[t] = (uint8_t)(arg % MASIF_ROWS);
        } else if (++tr->row[t] >= MASIF_ROWS) {
            tr->row[t] = 0u;
            next_phrase(tr, t);
        }
    } while (0);
}
