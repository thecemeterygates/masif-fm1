/* MASIF's screens: the tracker's pages on the FM-1's 240 x 240 LCD, as a grid of 20 x 18 text cells of 12 x 13
 * pixels (a 5 x 6 font drawn double, its corners rounded off (FONT SMOOTH, Scale2x) or not (BLOCK): LSDj's 20 columns; a title row, a row of column heads and a phrase's 16
 * rows). Four shades from a palette table. A page writes characters into the grid; only the rows that changed are
 * sent to the LCD. The instrument and the modulation page also draw a picture (the sample's waveform, an envelope,
 * an LFO), as pixels, over five of the rows.
 * The pages: S C P I T in a row (Song, Chain, Phrase, Instrument, Table), shown at the top right of every page with
 * the page that is open marked; Project above Song and Mixer below it take S's place there while they are open,
 * Modulation above Instrument takes I's (as M), the Sampler below it too (as R: the rack sampler, 2026-10-23), Groove
 * above Table T's (as G).
 *
 * The controls (the user, 2026-10-21: MASIF_CONTROLS 0; the knob layout of 2026-10-12 is still there as
 * MASIF_CONTROLS 1, for an optional interface later):
 *   SEL UP  SAVE DOWN  HOME LEFT  ARP RIGHT      the arrows: the cursor (held: again and again)
 *   PLAY SHIFT  EDIT OPTION  GLO EDIT  REC PLAY  (the user moved all four, 2026-10-23)
 *                      the buttons in use lit (OCT +/-, FX, ENV, LFO, SEQ dark: no use yet); REC (PLAY) red:
 *                      stopped lit, playing a flash on each beat; a take armed 4 Hz, running 8 Hz, saving 2 Hz
 *   SELECT knob        the cursor along, ALGORITHM knob up and down (as before the arrows); with SHIFT or EDIT
 *                      held: the value (SELECT by 1, ALGORITHM by 0x10); a list open: through it
 *   SHIFT + arrows     the pages: S C P I T along, Project / Song / Mixer at S, Modulation / Instrument at I,
 *                      Groove / Table at T up and down; to the right only into what the cursor is on (no chain in the
 *                      song's cell: no chain page, no phrase in the chain's step: no phrase page)
 *   PRESETS knob       the pages along S C P I T and back, not round (as SHIFT + LEFT / RIGHT); with SHIFT: up and down
 *   EDIT               an empty cell gets what was entered last; pressed twice quickly on a song's or a chain's
 *                      empty cell, or a phrase's instrument: a new one (a chain, a phrase, an instrument nothing uses)
 *   EDIT + arrows      the value: LEFT / RIGHT by 1 (a semitone), UP / DOWN by 0x10 (an octave)
 *   EDIT held          on an effect, a setting with names, the scale, a character of a name: its list, the arrows
 *                      go through it, letting EDIT go enters the marked one. OPTION while a list is open: nothing
 *                      changes. On the instrument page's SAMPLE EDIT opens the sample pool and it stays open:
 *                      the arrows through it, PLAY hears the marked one, EDIT gives it to the instrument (* the
 *                      instrument's; the user, 2026-10-24: the list stays open), OPTION alone leaves,
 *                      OPTION + EDIT twice deletes the marked sample from the FM-1
 *   OPTION + EDIT      cut: the cell (or the selection) into the clipboard, and empty; an empty note: OFF (its
 *                      release rings, as STP 00); on the instrument pages the setting as a new instrument has it
 *   OPTION + arrows    the song page: 16 rows up / down; chain, phrase, groove, instrument, table: LEFT / RIGHT the
 *                      one before / after
 *   SHIFT + OPTION     a selection from the cursor; again: the whole column, then everything. The arrows stretch it,
 *                      EDIT + arrows change all of it, OPTION copies it, OPTION + EDIT cuts it, EDIT clones it (the
 *                      song page: its chains, EDIT again soon: their phrases too; the chain page: its phrases)
 *   SHIFT + EDIT       paste at the cursor (on the instrument pages: the whole instrument)
 *   PLAY               play / stop what the page is on (the song from the cursor's row, the chain, the phrase; on
 *                      the instrument and the modulation page the instrument's sound)   SHIFT + PLAY  the song
 *   OPTION + PLAY      the cursor's track muted / heard          EDIT + PLAY  its solo (the song page's heads: M, S)
 *   KNOB 1-4           what they were given: OPTION + a knob turned gives it the setting under the cursor (an
 *                      instrument's, a modulation slot's, the mixer's, the project's tempo / key / scale / groove),
 *                      SHIFT + OPTION + a knob turned takes it away. SHIFT + a knob: by 0x10
 *   FX                 nothing for now (the user, 2026-10-23; the knobs' banks stay at bank 1)
 *   the instrument page: the instrument's number, then RIGHT along its name (EDIT held: the characters)
 *   the mixer: EDIT a track's mute; the project: EDIT on LOAD / SAVE / NEW / TEST (the load test). OCT+ / -, ENV, LFO, SEQ: free for later
 *   the sampler page   rows as the instrument page's: SAMPLE, the take just made (on its number OPTION + EDIT: the
 *                      sampler cleared, the sample stays in the pool; RIGHT along its name, EDIT held the
 *                      characters), SOURCE / START / LEVEL / RATE / BITS (12, 8, 4 ADPCM) / MAX by EDIT + arrows;
 *                      REC (PLAY) arms a take, again stops it; the note keys hear the sample
 *   the note keys      on the phrase page: that note into the cell, and heard; elsewhere: heard
 * Where one left off (the song, the page, the cursors, the knobs) is kept a few seconds after the last touch while
 * nothing plays (masif_ui_tick) and is back at power-on (masif_ui_resume).
 * Every edit goes through the edit queue (masif_post_edit), so the song may be playing.
 * Included after masif_fm1_adapter.c (masif_song, masif_runtime, masif_edits, masif_fm1_play_what, masif_fm1_preview)
 * and gfx.c (the canvas). */
#include <stddef.h>
#include "masif_font.h"
#include "masif_rec.h"

#ifndef SP_NUSER
#define SP_NUSER 24                              /* the pool's slots (sp_ui.c) */
#endif
#ifndef KIT_NWAVE
#define KIT_NWAVE 0                              /* the firmware's own waves before them (none in MASIF's build) */
#endif
#ifndef MU_LOAD
#define MU_LOAD(peak) 0u                         /* the audio interrupt's share of its time, % (zp12.c; peak: the most) */
#endif
#ifndef MU_BOOT_KEYS
#define MU_BOOT_KEYS() 0u                        /* the buttons the start saw (zp12.c: OCT- alone is the USB rescue) */
#endif

#ifndef MASIF_SONG_STORE
#define MASIF_SONG_STORE 0
#endif
#ifndef MU_WAVE_NAME                             /* a sample's name, from who keeps them (zp12.c: sp_ui.c's wave_name) */
#define MU_WAVE_NAME(w) ""
#endif

#define MU_COLS 20u
#define MU_ROWS 18u
#define MU_CW 12u
#define MU_CH 13u
#define MU_Y0 3u                                   /* (240 = 3 + 18 x 13 + 3) */
#define MU_DATA 2u                                 /* the first of the 16 data rows: under the title and the heads */
/* a cell's attribute: the text's shade, the background's shade << 2 */
enum { MU_BG = 0, MU_DIM = 1, MU_MID = 2, MU_INK = 3 };
#define MU_ON(bg) ((uint8_t)((bg) << 2))
#define MU_CURSOR (MU_BG | MU_ON(MU_INK))        /* the cursor: dark on bright */

/* the palettes: background, dim, mid, ink (RGB565). The first is the one at power-on. (2026-10-23: dim and mid
 * further from the background, so the row numbers and the empty cells read on the device) */
static const uint16_t MU_PALETTE[][4] = {
    {RGB(0, 0, 0), RGB(100, 100, 100), RGB(176, 176, 176), RGB(250, 250, 250)},     /* white on black */
    {RGB(176, 196, 60), RGB(126, 148, 44), RGB(70, 94, 34), RGB(20, 36, 18)},       /* a rack sampler's LCD */
    {RGB(18, 12, 6), RGB(104, 60, 14), RGB(190, 110, 18), RGB(255, 184, 52)},       /* amber */
};
#define MU_NPALETTE (sizeof MU_PALETTE / sizeof MU_PALETTE[0])

enum { MU_SONG, MU_CHAIN, MU_PHRASE, MU_INST, MU_TABLE, MU_PROJECT, MU_MIXER, MU_MOD, MU_GROOVE, MU_SAMPLER, MU_NPAGE };
/* the buttons' matrix ids (the printed labels: as sp_ui.c's B_*) and the encoders */
enum { MK_OCTDN = 0, MK_OCTUP = 1, MK_FX = 2, MK_SEL = 3, MK_ENV = 4, MK_LFO = 5, MK_EDIT = 6, MK_GLO = 7, MK_HOME = 8,
       MK_SAVE = 9, MK_ARP = 10, MK_SEQ = 11, MK_PLAY = 12, MK_REC = 13 };
enum { ME_SELECT = 0, ME_ALGORITHM = 1, ME_KNOB1 = 2, ME_PRESETS = 6 };
/* the buttons on them (MASIF_CONTROLS 0; the user, 2026-10-23: the four one to the left of where they were) */
enum { MB_UP = MK_SEL, MB_DOWN = MK_SAVE, MB_LEFT = MK_HOME, MB_RIGHT = MK_ARP, MB_SHIFT = MK_PLAY, MB_OPTION = MK_EDIT,
       MB_EDIT = MK_GLO, MB_PLAY = MK_REC, MB_BANK = MK_FX };
#ifndef MASIF_CONTROLS
#define MASIF_CONTROLS 0                           /* 0: the arrows' (2026-10-21); 1: the knobs' (2026-10-12) */
#endif
#define MU_REPEAT_FIRST 350u                       /* an arrow held: again after this (ms), */
#define MU_REPEAT_NEXT 60u                         /* then every this */
#define MU_DOUBLE_MS 400u                          /* EDIT twice within this: the double tap */
#define MU_COMBO_MS 60u                            /* EDIT alone waits this long for OPTION / SHIFT (no blip) */
#define MU_KEEP_QUIET 3000u                        /* where one left off is kept this long after the last touch */

enum { PK_NONE, PK_FX, PK_PAR, PK_SCALE, PK_POOL, PK_CHAR };      /* the chooser's lists (below) */
static int mu_smp_on_char(void);
static void mu_smp_char(uint32_t c);
static uint32_t ms_char_now(void);
static uint8_t mu_ch[MU_ROWS][MU_COLS], mu_at[MU_ROWS][MU_COLS];           /* what the page wrote */
static uint8_t mu_shown_ch[MU_ROWS][MU_COLS], mu_shown_at[MU_ROWS][MU_COLS]; /* what the LCD has */
static struct {
    uint8_t page, back, palette, all;            /* all: draw every row (the first time, a new palette) */
    uint8_t font;                                /* 0 SMOOTH (the font at 10 x 12), 1 BLOCK (the 5 x 6 drawn double) */
    uint8_t shift, option;                       /* GLO, REC held */
    uint8_t option_used;                         /* ... and REC was a part of something since it went down (no copy) */
    uint8_t sel, sel_mode, sel_row, sel_col;     /* a selection: 0 to the cursor, 1 the whole column, 2 all; where it began */
    uint8_t pick, pick_n, pick_cur, pick_cols, pick_w;   /* the chooser (EDIT held): which list, how long, the marked one */
    uint8_t track;                               /* the song page's column, and the track a chain / phrase plays on */
    uint8_t song_row, song_top;
    uint8_t chain, step, ccol;                   /* the chain page: its chain, the cursor's step, 0 phrase / 1 transpose */
    uint8_t phrase, row, pcol;                   /* the phrase page: the cursor's row, its column (below) */
    uint8_t prow;                                /* the project page's row */
    uint8_t inst;                                /* the instrument the pages are on (its table is the table page's) */
    uint8_t itab, irow;                          /* the instrument page: its tab, the cursor's row in it */
    uint8_t mslot, mrow;                         /* the modulation page: its slot, the cursor's row in it */
    char sname[9];                               /* the song's name (saved with it; the working copy's too) */
    uint8_t schar;                               /* the project page: the cursor on the name's character schar - 1 (0: the slot) */
    uint8_t inum, ichar;                         /* on those two pages the cursor is on the instrument's number (ichar 0)
                                                 * or a character of its name (1..8: the instrument page's) */
    uint8_t groove, grow, gnum;                  /* the groove page: its groove, the cursor's step, or on the groove's number */
    uint8_t trow, tcol;                          /* the table page: the cursor's row and lane */
    uint8_t mcol;                                /* the mixer: 0-3 a track, 4-6 the mix's EQ */
    uint8_t slot;                                /* the project page: the song slot LOAD and SAVE mean */
    uint8_t sounding;                            /* the instrument page's PLAY is holding a note */
    uint8_t zoom;                                /* the instrument page's picture: 1, 2 or 4 times (SEL) */
    uint8_t wave_drawn;                          /* the picture on the LCD is the instrument's as it is */
    uint32_t wave_key;
    const char *msg;                             /* a word for the bottom row (SAVED, EMPTY ...), */
    uint32_t msg_until;                          /* until fm1_ms is this */
    uint8_t last_chain, last_phrase, last_note, last_inst, last_vol, last_fx;   /* what an empty cell begins with */
    /* the arrows' controls */
    uint8_t edit, edit_used;                     /* EDIT held, and it was a part of something since it went down */
    uint8_t edit_page, edit_row, edit_col;       /* where EDIT went down last, */
    uint32_t edit_at;                            /* and when (0: never): a second press there soon is the double tap */
    uint8_t pick_cancel;                         /* OPTION while a list is open: letting EDIT go changes nothing */
    uint8_t previewing;                          /* a note heard while EDIT is held (let go with it) */
    uint8_t edit_pend;                           /* EDIT down alone, its own action not done yet (MU_COMBO_MS) */
    uint32_t edit_pend_at;
    uint8_t arrow;                               /* the arrow held (its button + 1; 0 none), */
    uint32_t arrow_at;                           /* and when it goes again */
    uint8_t bank, bank_held;                     /* the knobs' bank (FX), and FX held: the bank on the screen */
    uint8_t msg_knob;                            /* the bottom row tells this knob's setting (+ 1; 0 none) */
    uint32_t input_at;                           /* the last touch of a control */
    uint32_t keep_check, song_hash;              /* where one left off: when it was looked at, the song as kept */
    uint8_t keep_next, keep_full, resumed;       /* the next record's place; the working copy did not fit; it is kept */
    uint8_t mute, solo;                          /* the tracks muted, soloed (a bit a track; masif_mute is what is heard) */
    uint32_t keys;                               /* the note keys held down (bit k: key k) */
    uint8_t undo_ok, undo_page, undo_row;        /* what EDIT's press filled in, to take back if EDIT was a combo's */
    uint8_t undo[7];
    uint32_t clone_at;                           /* when EDIT last cloned a selection (again soon: deep) */
} mu;
static void mu_cursor(int32_t dx, int32_t dy);
/* the phrase page's columns */
enum { MP_NOTE, MP_INST, MP_VOL, MP_FX1, MP_PAR1, MP_FX2, MP_PAR2, MP_NCOL };
/* the table page's */
enum { MT_N, MT_V, MT_FX1, MT_PAR1, MT_FX2, MT_PAR2, MT_NCOL };

static const char MU_NOTE[12][3] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
/* the effects' names, in masif_fx_t's order */
static const char MU_FX[][4] = {"---", "TPO", "JMP", "HOP", "SLU", "SL-", "SLD", "VIB", "ARP", "RET", "DLY", "CUT",
                                "TBL", "TIC", "PRB", "STP", "OFS", "REV", "PAN", "SND", "THO", "DED", "REP", "RTO", "RNL", "GRV", "GGR", "MTT",
                                "PBN", "SLC"};   /* (JMP, SLU, SL-, DLY, CUT, TIC, GGR: out, never in a song) */
#define MU_NFX (sizeof MU_FX / sizeof MU_FX[0])

#define MU_WAVE_R0 1u                             /* the instrument and the modulation page's picture: these rows, as pixels */
#define MU_WAVE_R1 6u

/* ---- the grid */
static void mu_clear(void)
{
    uint32_t r, c;
    for (r = 0; r < MU_ROWS; r++)
        for (c = 0; c < MU_COLS; c++) { mu_ch[r][c] = ' '; mu_at[r][c] = MU_INK; }
}
static void mu_put(uint32_t col, uint32_t row, const char *s, uint8_t at)
{
    for (; *s && col < MU_COLS && row < MU_ROWS; s++, col++) { mu_ch[row][col] = (uint8_t)*s; mu_at[row][col] = at; }
}
static void mu_putn(uint32_t col, uint32_t row, const char *s, uint32_t n, uint8_t at)
{
    for (; n && *s && col < MU_COLS && row < MU_ROWS; s++, col++, n--) { mu_ch[row][col] = (uint8_t)*s; mu_at[row][col] = at; }
}
static void mu_hex(uint32_t col, uint32_t row, uint32_t v, uint32_t digits, uint8_t at)
{
    char b[5];
    uint32_t i;
    for (i = 0; i < digits && i < 4u; i++) b[i] = "0123456789ABCDEF"[(v >> (4u * (digits - 1u - i))) & 15u];
    b[i] = 0;
    mu_put(col, row, b, at);
}
static void mu_dec(uint32_t col, uint32_t row, uint32_t v, uint32_t digits, uint8_t at)
{
    char b[6];
    uint32_t i;
    for (i = digits; i--; v /= 10u) b[i] = (char)('0' + v % 10u);
    b[digits] = 0;
    mu_put(col, row, b, at);
}
static void mu_fill(uint32_t c0, uint32_t r0, uint32_t c1, uint32_t r1, uint8_t at)   /* [c0, c1) x [r0, r1): blank cells */
{
    uint32_t r, c;
    for (r = r0; r < r1 && r < MU_ROWS; r++)
        for (c = c0; c < c1 && c < MU_COLS; c++) { mu_ch[r][c] = ' '; mu_at[r][c] = at; }
}

/* the pages with a picture over rows MU_WAVE_R0..R1 (the instrument's pool covers the whole screen) */
static int mu_pictured(void) { return (mu.page == MU_INST || mu.page == MU_MOD || mu.page == MU_SAMPLER) && mu.pick != PK_POOL; }
/* the rows that are not on the LCD as the grid has them: drawn on the canvas, sent */
static void mu_flush(void)
{
    const uint16_t *pal = MU_PALETTE[mu.palette % MU_NPALETTE];
    uint32_t r, c, gy, gx;
    if (mu.all) {                                /* (the pixel rows the grid leaves over) */
        lcd_fill(0, 0, 240, MU_Y0, pal[MU_BG]);
        lcd_fill(0, MU_Y0 + MU_ROWS * MU_CH, 240, 240u - MU_Y0 - MU_ROWS * MU_CH, pal[MU_BG]);
    }
    for (r = 0; r < MU_ROWS; r++) {
        for (c = 0; c < MU_COLS && !mu.all; c++)
            if (mu_ch[r][c] != mu_shown_ch[r][c] || mu_at[r][c] != mu_shown_at[r][c]) break;
        if (c == MU_COLS && !mu.all) continue;
        if (mu_pictured() && r >= MU_WAVE_R0 && r < MU_WAVE_R1) {   /* (the picture's rows: not text) */
            if (mu.all) mu.wave_drawn = 0;
            continue;
        }
        cv_begin(240, MU_CH, pal[MU_BG]);
        for (c = 0; c < MU_COLS; c++) {
            uint32_t ch = mu_ch[r][c], at = mu_at[r][c], fg = pal[at & 3u], bg = pal[(at >> 2) & 3u];
            const uint32_t gi = (ch >= 0x20u && ch < 0x60u ? ch : ch >= 0x60u && ch < 0x80u ? ch - 0x20u : 0x20u) - 0x20u;
            const uint8_t *g = MASIF_FONT[gi];
            if ((at >> 2) & 3u) cv_rect((int32_t)(c * MU_CW), 0, MU_CW, MU_CH, (uint16_t)bg);
            if (mu.font == 0) {                      /* (a row's runs of pixels, a rect each) */
                const uint16_t *s = MASIF_FONT_SMOOTH[gi];
                for (gy = 0; gy < 12u; gy++)
                    for (gx = 0; gx < 10u; gx++) {
                        uint32_t e = gx;
                        while (e < 10u && ((s[gy] >> (9u - e)) & 1u)) e++;
                        if (e > gx) { cv_rect((int32_t)(c * MU_CW + 1u + gx), (int32_t)gy, (int32_t)(e - gx), 1, (uint16_t)fg); gx = e; }
                    }
            } else
                for (gy = 0; gy < 6u; gy++)
                    for (gx = 0; gx < 5u; gx++)
                        if ((g[gy] >> (4u - gx)) & 1u)
                            cv_rect((int32_t)(c * MU_CW + 1u + gx * 2u), (int32_t)(gy * 2u), 2, 2, (uint16_t)fg);
            mu_shown_ch[r][c] = (uint8_t)ch; mu_shown_at[r][c] = (uint8_t)at;
        }
        cv_blit(0, MU_Y0 + r * MU_CH);
    }
    mu.all = 0;
}

/* ---- what the pages share */
static void mu_edit(uint32_t kind, uint32_t a, uint32_t b, const uint8_t *d)
{
    (void)masif_post_edit(&masif_edits, (uint8_t)kind, (uint8_t)a, (uint8_t)b, d);
}
static void mu_edit1(uint32_t kind, uint32_t a, uint32_t b, uint8_t v)     /* an edit of one byte */
{
    uint8_t d[7] = {0, 0, 0, 0, 0, 0, 0};
    d[0] = v;
    mu_edit(kind, a, b, d);
}
static int32_t mu_clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static void mu_say(const char *m) { mu.msg = m; mu.msg_until = fm1_ms + 1500u; mu.msg_knob = 0; }

/* the title row: the page's name, and at the top right S C P I T with the open page marked (Project and Mixer in
 * S's place while they are open, Modulation in I's) */
static void mu_title(const char *name)
{
    static const uint8_t at_of[MU_NPAGE] = {0, 1, 2, 3, 4, 0, 0, 3, 4, 3};
    char m[6] = "SCPIT";
    if (mu.page == MU_PROJECT) m[0] = 'P';
    if (mu.page == MU_MIXER) m[0] = 'M';
    if (mu.page == MU_MOD) m[3] = 'M';
    if (mu.page == MU_SAMPLER) m[3] = 'R';
    if (mu.page == MU_GROOVE) m[4] = 'G';
    mu_put(0, 0, name, MU_INK);
    mu_put(15, 0, m, MU_MID);
    mu_putn(15u + at_of[mu.page], 0, &m[at_of[mu.page]], 1, MU_CURSOR);
}

static void mu_note(uint32_t col, uint32_t row, uint32_t note, uint8_t at, uint8_t empty)
{
    char s[4] = "---";
    if (note == MASIF_NOTE_OFF) { mu_put(col, row, "OFF", at); return; }
    if (note == MASIF_NOTE_CUT) { mu_put(col, row, "CUT", at); return; }
    if (note < 1u || note > 120u) { mu_put(col, row, s, empty); return; }
    s[0] = MU_NOTE[note % 12u][0]; s[1] = MU_NOTE[note % 12u][1];
    s[2] = (char)('0' + (note / 12u + 9u) % 10u);               /* 60 = C-4 */
    mu_put(col, row, s, at);
}
/* the project's key: C, C#, D ... */
static void mu_key(uint32_t col, uint32_t row, uint8_t at)
{
    const char *n = MU_NOTE[masif_song.key % 12u];
    mu_putn(col, row, n, n[1] == '#' ? 2u : 1u, at);
}
/* a signed value as +NN / -NN */
static void mu_signed(uint32_t col, uint32_t row, int32_t v, uint8_t at)
{
    mu_put(col, row, v < 0 ? "-" : "+", at);
    mu_dec(col + 1u, row, (uint32_t)(v < 0 ? -v : v), 2, at);
}
/* an effect and its parameter in 5 cells */
static void mu_fx(uint32_t col, uint32_t row, uint32_t fx, uint32_t par, uint8_t at_fx, uint8_t at_par, uint8_t at_fx_none, uint8_t at_par_none)
{
    int none = !fx && !par;
    mu_put(col, row, fx < MU_NFX ? MU_FX[fx] : "???", none ? at_fx_none : at_fx);
    if (none) mu_put(col + 3u, row, "  ", at_par_none);
    else mu_hex(col + 3u, row, par, 2, at_par);
}

/* the side panel of the song and the chain pages: the track, the tempo, the key and scale, what each track plays
 * now, play / stop, the song's slot. (The rows under it are free for what comes later.) */
static void mu_panel(void)
{
    uint32_t t;
    const uint8_t on = MU_INK | MU_ON(MU_DIM), lo = MU_MID | MU_ON(MU_DIM);
    char s[3] = "T1";
    mu_fill(15, 1, 20, MU_ROWS, on);
    s[1] = (char)('1' + mu.track);
    mu_put(15, 1, s, on);
    mu_put(15, 3, "T", lo);
    mu_dec(17, 3, sq.bpm10 / 10u, 3, on);
    mu_key(15, 4, on);
    mu_putn(17, 4, masif_song.scale < MASIF_SCALES ? masif_scale_name[masif_song.scale] : "", 3, lo);
    for (t = 0; t < MASIF_TRACKS; t++) {
        s[0] = (char)('1' + t); s[1] = 0;
        mu_put(15, 6u + t, s, masif_runtime.tracker.on[t] && sq.playing ? MU_CURSOR : on);
        mu_note(17, 6u + t, sq.playing ? masif_runtime.active_note[t] : 0u, on, lo);
    }
    mu_put(15, 11, sq.playing ? "PLAY" : "STOP", lo);
    mu_put(15, 13, "SNG", lo);
    mu_dec(18, 13, mu.slot + 1u, 2, on);
}

/* ---- the pages */
static void mu_draw_song(void)
{
    uint32_t r, t;
    mu_title("SONG");
    for (t = 0; t < MASIF_TRACKS; t++) {            /* the tracks' heads: M muted, S soloed */
        char s[3] = {(char)('1' + t), (mu.solo >> t) & 1u ? 'S' : (mu.mute >> t) & 1u ? 'M' : 0, 0};
        mu_put(4u + t * 3u, 1, s, t == mu.track ? MU_INK : MU_MID);
    }
    for (r = 0; r < 16u; r++) {
        uint32_t row = (mu.song_top + r) & 255u;
        mu_hex(0, MU_DATA + r, row, 2, row % 4u ? MU_MID : MU_INK);
        for (t = 0; t < MASIF_TRACKS; t++) {
            uint32_t ch = masif_song.song[row][t];
            int here = row == mu.song_row && t == mu.track;
            int plays = sq.playing && masif_runtime.tracker.what == MASIF_PLAY_SONG && masif_runtime.tracker.on[t] &&
                        masif_runtime.tracker.song_row[t] == row;
            uint8_t at = here ? MU_CURSOR : plays ? (uint8_t)(MU_INK | MU_ON(MU_DIM)) : ch < MASIF_CHAINS ? MU_INK : MU_DIM;
            if (ch < MASIF_CHAINS) mu_hex(3u + t * 3u, MU_DATA + r, ch, 2, at);
            else mu_put(3u + t * 3u, MU_DATA + r, "--", at);
        }
    }
    mu_panel();
}

static void mu_draw_chain(void)
{
    uint32_t r;
    const masif_tracker_t *tr = &masif_runtime.tracker;
    mu_title("CHAIN");
    mu_hex(6, 0, mu.chain, 2, MU_INK);
    mu_put(3, 1, "PH", MU_MID);
    mu_put(6, 1, "TSP", MU_MID);
    for (r = 0; r < MASIF_CHAIN_STEPS; r++) {
        const masif_chain_step_t *c = &masif_song.chain[mu.chain % MASIF_CHAINS][r];
        int plays = sq.playing && tr->on[mu.track] && tr->chain[mu.track] == mu.chain && tr->step[mu.track] == r;
        mu_hex(0, MU_DATA + r, r, 1, plays ? MU_CURSOR : r % 4u ? MU_MID : MU_INK);
        if (c->phrase < MASIF_PHRASES) mu_hex(3, MU_DATA + r, c->phrase, 2, r == mu.step && mu.ccol == 0u ? MU_CURSOR : MU_INK);
        else mu_put(3, MU_DATA + r, "--", r == mu.step && mu.ccol == 0u ? MU_CURSOR : MU_DIM);
        mu_hex(6, MU_DATA + r, (uint8_t)c->transpose, 2, r == mu.step && mu.ccol == 1u ? MU_CURSOR : c->phrase < MASIF_PHRASES ? MU_MID : MU_DIM);
    }
    mu_panel();
}

static void mu_draw_phrase(void)
{
    uint32_t r;
    const masif_tracker_t *tr = &masif_runtime.tracker;
    mu_title("PHRASE");
    mu_hex(7, 0, mu.phrase, 2, MU_INK);
    mu_put(10, 0, "T", MU_MID);
    mu_dec(11, 0, mu.track + 1u, 1, MU_MID);
    mu_put(2, 1, "N", MU_MID); mu_put(5, 1, "I", MU_MID); mu_put(7, 1, "V", MU_MID);
    mu_put(10, 1, "FX1", MU_MID); mu_put(15, 1, "FX2", MU_MID);
    for (r = 0; r < MASIF_ROWS; r++) {
        const masif_cell_t *c = &masif_song.phrase[mu.phrase % MASIF_PHRASES][r];
        int plays = sq.playing && tr->on[mu.track] && tr->phrase[mu.track] == mu.phrase &&
                    ((tr->row[mu.track] + MASIF_ROWS - 1u) % MASIF_ROWS) == r;
#define MU_AT(col, shade) ((uint8_t)(r == mu.row && mu.pcol == (col) ? MU_CURSOR : (shade)))
        mu_hex(0, MU_DATA + r, r, 1, plays ? MU_CURSOR : r % 4u ? MU_MID : MU_INK);
        mu_note(2, MU_DATA + r, c->note, MU_AT(MP_NOTE, MU_INK), MU_AT(MP_NOTE, MU_DIM));
        if (c->instrument < MASIF_INSTRUMENTS) mu_hex(5, MU_DATA + r, c->instrument, 2, MU_AT(MP_INST, MU_MID));
        else mu_put(5, MU_DATA + r, "--", MU_AT(MP_INST, MU_DIM));
        if (c->volume <= 127u) mu_hex(7, MU_DATA + r, c->volume, 2, MU_AT(MP_VOL, MU_INK));
        else mu_put(7, MU_DATA + r, "--", MU_AT(MP_VOL, MU_DIM));
        mu_fx(10, MU_DATA + r, c->fx, c->param, MU_AT(MP_FX1, MU_INK), MU_AT(MP_PAR1, MU_MID), MU_AT(MP_FX1, MU_DIM), MU_AT(MP_PAR1, MU_DIM));
        mu_fx(15, MU_DATA + r, c->fx2, c->param2, MU_AT(MP_FX2, MU_INK), MU_AT(MP_PAR2, MU_MID), MU_AT(MP_FX2, MU_DIM), MU_AT(MP_PAR2, MU_DIM));
#undef MU_AT
    }
}

/* the table of the instrument the pages are on: N (the transpose on the phrase's note), V, FX1, FX2 */
static void mu_draw_table(void)
{
    uint32_t r;
    int runs = sq.playing && masif_runtime.table_on[mu.track] && masif_runtime.table_id[mu.track] == mu.inst &&
               masif_runtime.active_note[mu.track] != MASIF_NOTE_NONE;
    mu_title("TABLE");
    mu_hex(6, 0, mu.inst, 2, MU_INK);
    mu_put(2, 1, "N", MU_MID); mu_put(5, 1, "V", MU_MID); mu_put(8, 1, "FX1", MU_MID); mu_put(13, 1, "FX2", MU_MID);
    for (r = 0; r < MASIF_TABLE_ROWS; r++) {
        const masif_table_cell_t *c = &masif_song.table[mu.inst % MASIF_TABLES][r];
        int empty = !c->transpose && c->volume == 0xFFu && !c->fx && !c->param && !c->fx2 && !c->param2;
        int plays = runs && ((masif_runtime.table_row[mu.track] + MASIF_TABLE_ROWS - 1u) % MASIF_TABLE_ROWS) == r;
#define MU_AT(col, shade) ((uint8_t)(r == mu.trow && mu.tcol == (col) ? MU_CURSOR : (shade)))
        mu_hex(0, MU_DATA + r, r, 1, plays ? MU_CURSOR : r % 4u ? MU_MID : MU_INK);
        mu_hex(2, MU_DATA + r, (uint8_t)c->transpose, 2, MU_AT(MT_N, empty ? MU_DIM : MU_INK));
        if (c->volume <= 127u) mu_hex(5, MU_DATA + r, c->volume, 2, MU_AT(MT_V, MU_INK));
        else mu_put(5, MU_DATA + r, "--", MU_AT(MT_V, MU_DIM));
        mu_fx(8, MU_DATA + r, c->fx, c->param, MU_AT(MT_FX1, MU_INK), MU_AT(MT_PAR1, MU_MID), MU_AT(MT_FX1, MU_DIM), MU_AT(MT_PAR1, MU_DIM));
        mu_fx(13, MU_DATA + r, c->fx2, c->param2, MU_AT(MT_FX2, MU_INK), MU_AT(MT_PAR2, MU_MID), MU_AT(MT_FX2, MU_DIM), MU_AT(MT_PAR2, MU_DIM));
#undef MU_AT
    }
}

/* ---- the groove page, above the table page: a groove's 16 steps, each the tracker ticks a row of a
 * track on this groove lasts (06: a 16th; 00: the phrase's row is passed over; empty: from the first step again).
 * Beside them the sums as help (16 rows of 06 are 96) and the groove each track is on */
static void mu_draw_groove(void)
{
    const uint8_t *g = masif_song.groove[mu.groove % MASIF_GROOVES];
    uint32_t r, t, n = 0, sum = 0, phrase = 0, k = 0;
    mu_title("GROOVE");
    mu_hex(7, 0, mu.groove, 1, mu.gnum ? MU_CURSOR : MU_INK);
    mu_put(3, 1, "TIC", MU_MID);
    while (n < MASIF_GROOVE_STEPS && g[n] != 0xFFu) sum += g[n++];
    for (r = 0, t = 0; n && r < MASIF_ROWS && t < 16u * MASIF_GROOVE_STEPS; t++, k = (k + 1u) % n)  /* (16 rows that play) */
        if (g[k]) { phrase += g[k]; r++; }
    if (!n) phrase = 96;
    for (r = 0; r < MASIF_GROOVE_STEPS; r++) {
        uint8_t at = (uint8_t)(!mu.gnum && r == mu.grow ? MU_CURSOR : g[r] == 0xFFu ? MU_DIM : MU_INK);
        int plays = sq.playing && masif_runtime.tracker.on[mu.track] &&
                    masif_runtime.groove[mu.track] == mu.groove && (masif_runtime.groove_pos[mu.track] + MASIF_GROOVE_STEPS - 1u) % MASIF_GROOVE_STEPS == r;
        mu_hex(0, MU_DATA + r, r, 1, plays ? MU_CURSOR : r % 4u ? MU_MID : MU_INK);
        if (g[r] == 0xFFu) mu_put(3, MU_DATA + r, "--", at);
        else mu_hex(3, MU_DATA + r, g[r], 2, at);
    }
    mu_put(8, 2, "STEPS", MU_MID); mu_dec(16, 2, sum, 3, MU_INK);
    mu_put(8, 3, "PHRASE", MU_MID); mu_dec(16, 3, phrase > 999u ? 999u : phrase, 3, phrase == 96u ? MU_INK : MU_MID);
    mu_put(8, 5, "TRACK GRV", MU_MID);
    for (t = 0; t < MASIF_TRACKS; t++) {
        mu_hex(10, 6u + t, t + 1u, 1, t == mu.track ? MU_INK : MU_MID);
        mu_hex(16, 6u + t, sq.playing ? masif_runtime.groove[t] : masif_song.project_groove, 1, MU_INK);
    }
}

/* ---- the instrument page (the sampler) and the modulation page above it. Both: a picture over the rows
 * MU_WAVE_R0..R1, under it the instrument's number, under that the settings as rows of label and value, and at the
 * right the tabs (the instrument's) or the four slots (the modulation's) */
enum { MK_HEX, MK_SIGNED, MK_NOTE, MK_ENUM, MK_SAMPLE, MK_DEC };
enum { MI_WAVE, MI_LOOP, MI_AMP, MI_FILT, MI_EQ, MI_SEND, MI_NTAB };
typedef struct { uint8_t off, kind; int16_t lo, hi; const char *label; const char *const *names; } mu_par_t;
static const char *const MU_TAB[MI_NTAB] = {"SMPL", "LOOP", "AMP", "FILT", "EQ", "SEND"};
static const char *const MU_PLAY_NAME[MASIF_PLAY_MODES] = {"FWD", "REV", "FWDLOOP", "REVLOOP", "FWD PP", "REV PP", "OSC", "OSC REV",
                                                             "OSC PP", "REPITCH", "REP PP", "BPM", "BPM PP"};
static const char *const MU_FILT_NAME[] = {"OFF", "LOW", "BAND", "HIGH"};
static const char *const MU_FREE_NAME[] = {"PROJ", "CHRO"};   /* the project's scale, or chromatic */
static const char *const MU_MODT_NAME[MASIF_MOD_TYPES] = {"OFF", "AHD", "ADSR", "LFO", "TRG ENV"};
static const char *const MU_DEST_NAME[MASIF_DESTS] = {"OFF", "VOLUME", "PITCH", "PAN", "CUTOFF", "RES", "CHORUS", "DELAY",
                                                      "REVERB", "MOD AMT", "MOD RTE"};
static const char *const MU_DEST_SHORT[MASIF_DESTS] = {"---", "VOL", "PITCH", "PAN", "CUT", "RES", "CHO", "DLY", "REV",
                                                       "M.AMT", "M.RTE"};
static const char *const MU_SHAPE_NAME[MASIF_LFO_SHAPES] = {"TRI", "SINE", "RAMP DN", "RAMP UP", "EXP DN", "EXP UP",
                                                            "SQU DN", "SQU UP", "RANDOM", "DRUNK"};
static const char *const MU_TRIG_NAME[MASIF_LFO_TRIGS] = {"FREE", "RETRIG", "HOLD", "ONCE"};
static const char *const MU_SYNC_NAME[2] = {"OFF", "ON"};
/* a synced LFO's rates: the lengths of a cycle (masif_fm1_adapter.c MASIF_LFO_DIV), T a triplet's, . a dotted one */
static const char *const MU_DIV_NAME[MASIF_LFO_DIVS] = {"8 BAR", "4 BAR", "2 BAR", "1 BAR", "1/2", "1/4.", "1/2T", "1/4", "1/8.", "1/4T",
                                                        "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/64"};
static const mu_par_t MU_STEPS_BPM = {(uint8_t)offsetof(masif_instrument_t, steps), MK_DEC, 0, 255, "BPM", 0};
static const mu_par_t MU_LFO_SYNCED = {5, MK_ENUM, 0, MASIF_LFO_DIVS - 1, "RATE", MU_DIV_NAME};
#define MU_OFF(f) ((uint8_t)offsetof(masif_instrument_t, f))
static const struct { uint8_t tab; mu_par_t p; } MU_PARAM[] = {
    {MI_WAVE, {MU_OFF(sample), MK_SAMPLE, -1, MASIF_SAMPLES - 1, "SAMPLE", 0}},      /* (-1: none, MASIF_NO_SAMPLE) */
    {MI_WAVE, {MU_OFF(start), MK_HEX, 0, 255, "START", 0}},
    {MI_WAVE, {MU_OFF(length), MK_HEX, 0, 255, "LENGTH", 0}},
    /* the LOOP tab (2026-10-15): where it goes round, and its two tapers (the level rises after its start and falls
     * before its end: in 1/256 of the loop, 80 = half of it) */
    {MI_LOOP, {MU_OFF(loop_start), MK_HEX, 0, 255, "LOOP", 0}},
    {MI_LOOP, {MU_OFF(loop_end), MK_HEX, 0, 255, "LP END", 0}},     /* (00: the part's end) */
    {MI_LOOP, {MU_OFF(taper_in), MK_HEX, 0, MASIF_TAPER_MAX, "TPR IN", 0}},
    {MI_LOOP, {MU_OFF(taper_out), MK_HEX, 0, MASIF_TAPER_MAX, "TPR OUT", 0}},
    {MI_WAVE, {MU_OFF(play), MK_ENUM, 0, MASIF_PLAY_MODES - 1, "PLAY", MU_PLAY_NAME}},
    {MI_WAVE, {MU_OFF(steps), MK_HEX, 0, 255, "STEPS", 0}},         /* (REPITCH's; BPM's: MU_STEPS_BPM) */
    {MI_WAVE, {MU_OFF(slices), MK_HEX, 0, 64, "SLICES", 0}},
    {MI_WAVE, {MU_OFF(root_note), MK_NOTE, 1, 120, "ROOT", 0}},
    {MI_WAVE, {MU_OFF(fine), MK_SIGNED, -99, 99, "FINE", 0}},
    {MI_AMP, {MU_OFF(volume), MK_HEX, 0, 127, "VOLUME", 0}},
    {MI_AMP, {MU_OFF(pan), MK_SIGNED, -64, 64, "PAN", 0}},
    {MI_AMP, {MU_OFF(attack), MK_HEX, 0, 127, "ATTACK", 0}},
    {MI_AMP, {MU_OFF(decay), MK_HEX, 0, 127, "DECAY", 0}},
    {MI_AMP, {MU_OFF(sustain), MK_HEX, 0, 127, "SUSTAIN", 0}},
    {MI_AMP, {MU_OFF(release), MK_HEX, 0, 127, "RELEASE", 0}},
    {MI_AMP, {MU_OFF(drive), MK_HEX, 0, 127, "DRIVE", 0}},
    {MI_FILT, {MU_OFF(filter), MK_ENUM, 0, MASIF_FILTERS - 1, "TYPE", MU_FILT_NAME}},
    {MI_FILT, {MU_OFF(cutoff), MK_HEX, 0, 127, "CUTOFF", 0}},
    {MI_FILT, {MU_OFF(resonance), MK_HEX, 0, 127, "RES", 0}},
    {MI_EQ, {MU_OFF(eq[0]), MK_SIGNED, -64, 64, "LOW", 0}},
    {MI_EQ, {MU_OFF(eq[1]), MK_SIGNED, -64, 64, "MID", 0}},
    {MI_EQ, {MU_OFF(eq[2]), MK_SIGNED, -64, 64, "HIGH", 0}},
    {MI_SEND, {MU_OFF(send[0]), MK_HEX, 0, 127, "CHORUS", 0}},
    {MI_SEND, {MU_OFF(send[1]), MK_HEX, 0, 127, "DELAY", 0}},
    {MI_SEND, {MU_OFF(send[2]), MK_HEX, 0, 127, "REVERB", 0}},
    {MI_SEND, {MU_OFF(table_tic), MK_HEX, 0, 255, "TBL TIC", 0}},
    {MI_SEND, {MU_OFF(flags), MK_ENUM, 0, 1, "SCALE", MU_FREE_NAME}},
};
#define MU_NPARAM (sizeof MU_PARAM / sizeof MU_PARAM[0])
/* a modulation slot's settings: its type, where it goes and how much, then what the type has (off: the byte of
 * masif_mod_t; a label of 0 ends a type's list) */
#define MU_MOD_AT(s) ((uint8_t)(offsetof(masif_instrument_t, mod) + (s) * sizeof(masif_mod_t)))
static const mu_par_t MU_MOD_HEAD[3] = {
    {0, MK_ENUM, 0, MASIF_MOD_TYPES - 1, "TYPE", MU_MODT_NAME},
    {1, MK_ENUM, 0, MASIF_DESTS - 1, "DEST", MU_DEST_NAME},
    {2, MK_SIGNED, -64, 64, "AMOUNT", 0}};
static const mu_par_t MU_MOD_PAR[MASIF_MOD_TYPES][4] = {
    {{0, 0, 0, 0, 0, 0}},
    {{3, MK_HEX, 0, 127, "ATTACK", 0}, {4, MK_HEX, 0, 127, "HOLD", 0}, {5, MK_HEX, 0, 127, "DECAY", 0}, {0, 0, 0, 0, 0, 0}},
    {{3, MK_HEX, 0, 127, "ATTACK", 0}, {4, MK_HEX, 0, 127, "DECAY", 0}, {5, MK_HEX, 0, 127, "SUSTAIN", 0}, {6, MK_HEX, 0, 127, "RELEASE", 0}},
    {{3, MK_ENUM, 0, MASIF_LFO_SHAPES - 1, "SHAPE", MU_SHAPE_NAME}, {4, MK_ENUM, 0, MASIF_LFO_TRIGS - 1, "TRIG", MU_TRIG_NAME},
     {5, MK_HEX, 0, 127, "RATE", 0}, {6, MK_ENUM, 0, 1, "SYNC", MU_SYNC_NAME}},
    /* (SRC: 00-7F an instrument, 80-83 a track) */
    {{3, MK_HEX, 0, 127, "ATTACK", 0}, {4, MK_HEX, 0, 127, "HOLD", 0}, {5, MK_HEX, 0, 127, "DECAY", 0}, {6, MK_HEX, 0, MASIF_TRIG_SRC_MAX, "SRC", 0}}};
/* what a slot begins with when it becomes a type: a short decay; a plucked ADSR; a free triangle a quarter long,
 * in the tempo (the user, 2026-10-12: an LFO's rate is the tempo's) */
static const uint8_t MU_MOD_USUAL[MASIF_MOD_TYPES][4] = {
    {0, 0, 0, 0}, {0, 0, 40, 0}, {0, 40, 64, 40}, {MASIF_LFO_TRI, MASIF_TRIG_FREE, 7, 1}, {0, 0, 40, 0x80}};

static mu_par_t mu_pick_par;                     /* the chooser's setting, while it is one's list */
static const masif_instrument_t *mu_inst(void) { return &masif_song.instrument[mu.inst % MASIF_INSTRUMENTS]; }
/* the n-th setting of the open tab (the instrument page) or slot (the modulation page): 0 if there is none */
static int mu_par(uint32_t n, mu_par_t *o)
{
    uint32_t i;
    if (mu.page == MU_MOD) {
        const masif_mod_t *m = &mu_inst()->mod[mu.mslot % MASIF_MODS];
        if (n < 3u) *o = MU_MOD_HEAD[n];
        else if (n < 7u && m->type < MASIF_MOD_TYPES && MU_MOD_PAR[m->type][n - 3u].label) *o = MU_MOD_PAR[m->type][n - 3u];
        else return 0;
        if (m->type == MASIF_MOD_LFO && n == 5u && m->p[3]) *o = MU_LFO_SYNCED;   /* (its rate: a length, by name) */
        o->off = (uint8_t)(o->off + MU_MOD_AT(mu.mslot % MASIF_MODS));
        return 1;
    }
    for (i = 0; i < MU_NPARAM; i++)
        if (MU_PARAM[i].tab == mu.itab && !n--) {
            *o = MU_PARAM[i].p;
            if (o->off == MU_STEPS_BPM.off && mu_inst()->play >= MASIF_PLAY_BPM) *o = MU_STEPS_BPM;   /* (the sample's tempo) */
            return 1;
        }
    return 0;
}
static uint32_t mu_par_count(void) { mu_par_t p; uint32_t n = 0; while (mu_par(n, &p)) n++; return n; }
static uint32_t mu_par_row(void) { return mu.inum ? 99u : mu.page == MU_MOD ? mu.mrow : mu.irow; }   /* (99: on none) */
/* the instrument's sample, if the instrument loops where the sample's own loop is (MASIF_INST_SMPLOOP) */
static const sp_wave_t *mu_smploop(const masif_instrument_t *in)
{
    const sp_wave_t *w = &sp_wave[in->sample % MASIF_SAMPLES];
    return in->sample < MASIF_SAMPLES && (in->flags & MASIF_INST_SMPLOOP) && w->lp_b && w->n ? w : 0;
}
static uint32_t mu_of256(uint32_t pos, uint32_t n) { return (uint32_t)mu_clampi((int32_t)(((uint64_t)pos * 256u + n / 2u) / n), 0, 255); }
static int mu_is_loop_par(const mu_par_t *p)
{
    return mu.page == MU_INST && (p->off == MU_OFF(loop_start) || p->off == MU_OFF(loop_end));
}
static int32_t mu_par_value(const mu_par_t *p)
{
    const uint8_t *in = (const uint8_t *)mu_inst();
    const sp_wave_t *w;
    if (mu.page == MU_INST && p->off == MU_OFF(flags)) return in[p->off] & MASIF_INST_FREE;   /* (SCALE: bit 0 alone) */
    if (p->kind == MK_SAMPLE && in[p->off] >= MASIF_SAMPLES) return -1;
    if (mu_is_loop_par(p) && (w = mu_smploop(mu_inst())) != 0)     /* the sample's loop, as near as 1/256 says it */
        return p->off == MU_OFF(loop_start) ? (int32_t)mu_of256(w->lp_a, w->n) : w->lp_b >= w->n ? 0 : (int32_t)mu_of256(w->lp_b, w->n);
    return p->kind == MK_SIGNED ? (int8_t)in[p->off] : in[p->off];
}
/* a setting gets a value. A slot that becomes another type begins with that type's usual settings */
static void mu_par_set(const mu_par_t *p, int32_t v)
{
    uint8_t d[7] = {0, 0, 0, 0, 0, 0, 0};
    uint32_t k;
    int typed = mu.page == MU_MOD && p->names == MU_MODT_NAME;
    v = mu_clampi(v, p->lo, p->hi);
    if (typed && v == mu_par_value(p)) return;
    if (mu.page == MU_INST && p->off == MU_OFF(flags)) v = (mu_inst()->flags & ~MASIF_INST_FREE) | (v & MASIF_INST_FREE);
    if (mu_is_loop_par(p) && mu_smploop(mu_inst())) {   /* turned: the instrument's own loop from here, both ends */
        const sp_wave_t *w = mu_smploop(mu_inst());
        mu_edit1(MASIF_EDIT_INSTRUMENT, mu.inst, MU_OFF(loop_start), (uint8_t)mu_of256(w->lp_a, w->n));
        mu_edit1(MASIF_EDIT_INSTRUMENT, mu.inst, MU_OFF(loop_end), (uint8_t)(w->lp_b >= w->n ? 0u : mu_of256(w->lp_b, w->n)));
        mu_edit1(MASIF_EDIT_INSTRUMENT, mu.inst, MU_OFF(flags), (uint8_t)(mu_inst()->flags & ~MASIF_INST_SMPLOOP));
    }
    d[0] = (uint8_t)v;
    mu_edit(MASIF_EDIT_INSTRUMENT, mu.inst, p->off, d);
    if (typed && v)
        for (k = 0; k < 4u; k++) {
            d[0] = MU_MOD_USUAL[v][k];
            mu_edit(MASIF_EDIT_INSTRUMENT, mu.inst, p->off + 3u + k, d);
        }
    if (p->names == MU_SYNC_NAME && v != mu_par_value(p)) {      /* SYNC turned: the rate is another kind of number */
        d[0] = v ? 7u : 40u;
        mu_edit(MASIF_EDIT_INSTRUMENT, mu.inst, p->off - 1u, d);
    }
}

/* character k of the instrument's name becomes c (0: none); a name has no holes: none before it become spaces */
static void mu_name_set(uint32_t k, uint8_t c)
{
    const uint8_t *nm = mu_inst()->name;
    uint32_t i;
    if (k >= MASIF_NAME_LEN) return;
    if (c) { for (i = 0; i < k; i++) if (!nm[i]) mu_edit1(MASIF_EDIT_INSTRUMENT, mu.inst, MU_OFF(name) + i, ' '); }
    else { for (i = k + 1u; i < MASIF_NAME_LEN && !nm[i]; i++) {} if (i < MASIF_NAME_LEN) c = ' '; }   /* (in the middle: a space) */
    mu_edit1(MASIF_EDIT_INSTRUMENT, mu.inst, MU_OFF(name) + k, c);
}
/* a setting's value as the pages write it */
static void mu_par_text(uint32_t col, uint32_t r, const mu_par_t *p, int32_t v, uint8_t at)
{
    switch (p->kind) {
    case MK_SIGNED: mu_signed(col, r, v, at); break;
    case MK_NOTE: mu_note(col, r, (uint32_t)v, at, at); break;
    case MK_DEC: mu_dec(col, r, (uint32_t)v, 3, at); break;
    case MK_ENUM: mu_put(col, r, p->names[(uint32_t)v % (uint32_t)(p->hi + 1)], at); break;
    case MK_SAMPLE:
        if (v < 0) mu_put(col, r, "--", at);
        else mu_hex(col, r, (uint32_t)v, 2, at);
        break;
    default:
        if (mu_is_loop_par(p) && mu_smploop(mu_inst())) mu_put(col, r, "SMP", at);   /* (the sample's own) */
        else mu_hex(col, r, (uint32_t)v, 2, at);
        break;
    }
}
/* the settings' rows, and the instrument's number under the picture (marked while the instrument is selected) */
static void mu_draw_pars(void)
{
    mu_par_t p;
    uint32_t n;
    uint8_t num = mu.inum ? MU_CURSOR : mu.sel ? (uint8_t)(MU_BG | MU_ON(MU_MID)) : MU_INK;
    mu_put(0, 6, "INST", mu.sel ? num : MU_MID);
    if (mu.sel) mu_put(4, 6, " ", MU_BG | MU_ON(MU_MID));
    mu_hex(5, 6, mu.inst, 2, mu.inum && mu.ichar ? MU_INK : num);
    if (mu.page == MU_INST)                          /* the name, a character a cell ('.': none) */
        for (n = 0; n < MASIF_NAME_LEN; n++) {
            uint8_t ch = mu_inst()->name[n];
            uint8_t at = mu.inum && mu.ichar == n + 1u ? MU_CURSOR : ch ? MU_INK : MU_DIM;
            char c[2] = {ch ? (char)ch : '.', 0};
            mu_put(8u + n, 6, c, at);
        }
    for (n = 0; mu_par(n, &p); n++) {
        uint32_t r = 7u + n;
        int32_t v = mu_par_value(&p);
        uint8_t at = n == mu_par_row() ? MU_CURSOR : MU_INK;
        mu_put(0, r, p.label, MU_MID);
        mu_par_text(8, r, &p, v, at);
    }
}

static void mu_draw_inst(void)
{
    const masif_instrument_t *in = mu_inst();
    const char *name = in->sample < MASIF_SAMPLES ? MU_WAVE_NAME(in->sample) : "";
    uint32_t i;
    mu_title("");
    if (in->name[0] && in->name[0] != ' ') mu_putn(0, 0, (const char *)in->name, MASIF_NAME_LEN, MU_INK);   /* its own name */
    else if (in->sample >= MASIF_SAMPLES) mu_put(0, 0, "INIT EDIT:POOL", MU_INK);   /* (empty: waiting for a sample) */
    else if (name[0]) mu_putn(0, 0, name, 12, MU_INK);   /* the sample's name: the page's title */
    else { mu_put(0, 0, "SAMPLE", MU_INK); mu_hex(7, 0, in->sample, 2, MU_INK); }
    mu_draw_pars();
    mu_fill(15, 7, 20, 7u + MI_NTAB, MU_INK | MU_ON(MU_DIM));
    for (i = 0; i < MI_NTAB; i++)
        mu_put(15, 7u + i, MU_TAB[i], i == mu.itab ? MU_CURSOR : (uint8_t)(MU_INK | MU_ON(MU_DIM)));
}

/* the modulation page: the open slot's settings, and at the right all four: what each is and where it goes */
static void mu_draw_mod(void)
{
    const masif_instrument_t *in = mu_inst();
    uint32_t s;
    mu_title("MOD");
    mu_draw_pars();
    mu_fill(15, 7, 20, 7u + 2u * MASIF_MODS, MU_INK | MU_ON(MU_DIM));
    for (s = 0; s < MASIF_MODS; s++) {
        const masif_mod_t *m = &in->mod[s];
        uint8_t at = s == mu.mslot ? MU_CURSOR : (uint8_t)(MU_INK | MU_ON(MU_DIM));
        mu_dec(15, 7u + 2u * s, s + 1u, 1, at);
        mu_put(16, 7u + 2u * s, " ", at);
        mu_putn(17, 7u + 2u * s, m->type < MASIF_MOD_TYPES ? MU_MODT_NAME[m->type] : "?", 3, at);
        if (m->type) mu_put(15, 8u + 2u * s, MU_DEST_SHORT[m->dest % MASIF_DESTS], MU_MID | MU_ON(MU_DIM));
    }
}

/* ---- the picture over rows MU_WAVE_R0..R1 (on the canvas, 240 pixels wide, h high) */
/* the instrument's sample: the part it plays brighter, its start, its loop's two ends and its end marked, the slices as ticks */
/* the loop as the adapter plays it, and its tapers (in samples), for the picture */
static void mu_loop_of(const masif_instrument_t *in, uint32_t n, uint32_t a, uint32_t b, uint32_t *la, uint32_t *lb, uint32_t *ti, uint32_t *to)
{
    const sp_wave_t *w = mu_smploop(in);
    uint32_t lp = (n >> 8) * in->loop_start + (((n & 255u) * in->loop_start) >> 8);
    uint32_t le = in->loop_end ? (n >> 8) * in->loop_end + (((n & 255u) * in->loop_end) >> 8) : 0u;
    if (w) { lp = w->lp_a; le = w->lp_b; }
    *la = lp >= a && lp + 1u < b ? lp : a;
    *lb = le && le <= b && le > *la + 1u ? le : b;
    *ti = (uint32_t)(((uint64_t)(*lb - *la) * in->taper_in) >> 8);
    *to = (uint32_t)(((uint64_t)(*lb - *la) * in->taper_out) >> 8);
}
/* the part [a, b), its loop and tapers, and the view [vs, vs + vn) the picture shows */
static void mu_wave_view(const masif_instrument_t *in, uint32_t n, uint32_t *a, uint32_t *b, uint32_t *la, uint32_t *lb,
                         uint32_t *ti, uint32_t *to, uint32_t *vs, uint32_t *vn)
{
    uint32_t c;
    mu_par_t p;
    *a = (n >> 8) * in->start + (((n & 255u) * in->start) >> 8);
    *b = in->length ? *a + (n >> 8) * in->length + (((n & 255u) * in->length) >> 8) : n;
    if (*b > n) *b = n;
    mu_loop_of(in, n, *a, *b, la, lb, ti, to);
    *vs = 0; *vn = n;
    if (mu.zoom > 1u && n >= 240u * mu.zoom) {      /* the view: round the cursor's setting */
        c = (*a + *b) / 2u;
        if (!mu.inum && mu_par(mu_par_row(), &p)) {
            if (p.off == MU_OFF(start)) c = *a;
            else if (p.off == MU_OFF(length)) c = *b;
            else if (p.off == MU_OFF(loop_start)) c = *la;
            else if (p.off == MU_OFF(loop_end)) c = *lb;
            else if (p.off == MU_OFF(taper_in)) c = *la + *ti;
            else if (p.off == MU_OFF(taper_out)) c = *lb - *to;
        }
        *vn = n / mu.zoom;
        *vs = c > *vn / 2u ? c - *vn / 2u : 0u;
        if (*vs + *vn > n) *vs = n - *vn;
    }
}
/* the playhead: where track t's voice is in the picture if it plays the page's instrument's sample (-1: it does not,
 * or not in the view). (Read from the audio's voice as it runs: a word at a time, a frame late at the most.) */
static int32_t mu_head_x(uint32_t t, const sp_wave_t *w, uint32_t vs, uint32_t vn)
{
    const sp_ch_t *c = &sp_ch[SP_NCH + t];
    uint32_t pos = c->pos;
    if (!c->on || c->w != w || masif_track_inst[t] != mu.inst || pos < vs || pos >= vs + vn || !vn) return -1;
    return (int32_t)((pos - vs) * 239u / vn);       /* (32 bits: a sample is 400000 at most; no 64-bit division here) */
}
/* the instrument's sample: the part it plays brighter, its loop brightest with a bar under it; top to bottom (the
 * user, 2026-10-23) START and END as thick lines, the loop's ends as thin ones with a flag at the top pointing into
 * the loop (shown while it loops or the LOOP tab is open, also where the loop is the whole part), the slices as
 * dotted lines, the playheads as lines dark at their sides; the tapers as short dashes. ZOOM shows a half or a quarter of the sample round the setting under the cursor */
static void mu_graph_wave(const masif_instrument_t *in, const uint16_t *pal, uint32_t h)
{
    const sp_wave_t *w = &sp_wave[in->sample % MASIF_SAMPLES];
    const uint32_t mid = h / 2u, n = in->sample < MASIF_SAMPLES ? w->n : 0u;   /* (no sample: an empty picture) */
    const int looping = in->play >= MASIF_PLAY_LOOP, marks = looping || (mu.page == MU_INST && mu.itab == MI_LOOP);
    uint32_t a, b, la, lb, ti, to, x, k, vs, vn, t;
    if (!n) return;
    mu_wave_view(in, n, &a, &b, &la, &lb, &ti, &to, &vs, &vn);
    for (x = 0; x < 240u; x++) {
        uint32_t i0 = vs + (uint32_t)(((uint64_t)x * vn) / 240u), span = vn / 240u ? vn / 240u : 1u, peak = 0, y;
        int inloop = looping && i0 >= la && i0 < lb;
        for (k = 0; k < 6u; k++) {                   /* six looks into the column's samples: the highest */
            uint32_t i = i0 + span * k / 6u;
            int32_t v;
            if (i >= n) break;
#if MASIF_ENABLE_TRACKER
            v = (w->uflags & 8u) ? sp_adpcm_at(w->d, i) / 16 : (w->uflags & 4u) ? (int32_t)(int8_t)w->d[i] * 16 : sp_sample(w->d, i);
#else
            v = sp_sample(w->d, i);
#endif
            if (v < 0) v = -v;
            if ((uint32_t)v > peak) peak = (uint32_t)v;
        }
        y = peak * (mid - 4u) / 2048u;
        cv_rect((int32_t)x, (int32_t)(mid - y), 1, (int32_t)(2u * y + 1u), inloop ? pal[MU_INK] : i0 >= a && i0 < b ? pal[MU_MID] : pal[MU_BG]);
        if (inloop) cv_rect((int32_t)x, (int32_t)h - 3, 1, 3, pal[MU_MID]);           /* (the loop: a bar under it) */
    }
#define MU_X(pos) (((int32_t)(pos) - (int32_t)vs) * 239 / (int32_t)vn)     /* (32 bits: a sample is 400000 at most; no 64-bit division here) */
#define MU_IN(px) ((px) >= -2 && (px) < 242)
    if (in->slices > 1u)                            /* (dotted: one on, one off) */
        for (k = 1; k < in->slices; k++) {
            int32_t sx = MU_X(a + (b - a) / in->slices * k), y;
            if (MU_IN(sx)) for (y = 0; y < (int32_t)h; y += 2) cv_rect(sx, y, 1, 1, pal[MU_INK]);
        }
    if (MU_IN(MU_X(a))) cv_rect(MU_X(a), 0, 2, (int32_t)h, pal[MU_INK]);
    if (MU_IN(MU_X(b))) cv_rect(MU_X(b) - 1, 0, 2, (int32_t)h, pal[MU_INK]);
    if (marks) {                                    /* (the flags: a triangle 6 wide, into the loop) */
        int32_t lx = MU_X(la), rx = MU_X(lb) - 1, y;
        if (MU_IN(lx)) { cv_rect(lx, 0, 1, (int32_t)h, pal[MU_INK]); for (y = 0; y < 6; y++) cv_rect(lx, y, 6 - y, 1, pal[MU_INK]); }
        if (MU_IN(rx)) { cv_rect(rx, 0, 1, (int32_t)h, pal[MU_INK]); for (y = 0; y < 6; y++) cv_rect(rx - 5 + y, y, 6 - y, 1, pal[MU_INK]); }
    }
    if (looping) {
        if (ti && MU_IN(MU_X(la + ti))) for (k = 1; k < h; k += 5u) cv_rect(MU_X(la + ti), (int32_t)k, 1, 2, pal[MU_INK]);
        if (to && MU_IN(MU_X(lb - to))) for (k = 1; k < h; k += 5u) cv_rect(MU_X(lb - to), (int32_t)k, 1, 2, pal[MU_INK]);
    }
    for (t = 0; t < MASIF_TRACKS; t++) {            /* the playheads: a bright line, dark at its sides to stand out */
        int32_t hx = mu_head_x(t, w, vs, vn);
        if (hx < 0) continue;
        cv_rect(hx - 1, 0, 3, (int32_t)h, pal[MU_BG]);
        cv_rect(hx, 0, 1, (int32_t)h, pal[MU_INK]);
    }
#undef MU_IN
#undef MU_X
}

/* a level 0..256 at column x: the area under it filled, its outline two pixels thick, joined to the column before */
static void mu_graph_col(const uint16_t *pal, uint32_t h, uint32_t x, int32_t level, int32_t *prev, int fill)
{
    int32_t base = (int32_t)h - 4, y = base - level * ((int32_t)h - 10) / 256, lo, hi;
    if (*prev < 0) *prev = y;
    lo = y < *prev ? y : *prev; hi = y < *prev ? *prev : y;
    if (fill && y < base) cv_rect((int32_t)x, y, 1, base - y, pal[MU_MID]);
    cv_rect((int32_t)x, lo, 1, hi - lo + 2, pal[MU_INK]);
    *prev = y;
}
/* an envelope as everyone draws one: attack, hold, decay to the sustain level, and (if it holds there: an ADSR) the
 * held note and its release, each as wide as its time is long. curved: the decay and the release as the level's
 * envelope has them (fast at first), not as straight lines */
static void mu_graph_env(const uint16_t *pal, uint32_t h, uint32_t a, uint32_t hold, uint32_t d, uint32_t s, uint32_t r,
                         int curved, int holds)
{
    struct { int32_t w, y0, y1, curve; } seg[5];
    int32_t sus = holds ? (int32_t)s * 256 / 127 : 0, x, prev = (int32_t)h - 4, k, x0, off_x;
    seg[0].w = (int32_t)a * 56 / 127; seg[0].y0 = 0; seg[0].y1 = 256; seg[0].curve = 0;
    seg[1].w = (int32_t)hold * 56 / 127; seg[1].y0 = seg[1].y1 = 256; seg[1].curve = 0;
    seg[2].w = (int32_t)d * 56 / 127; seg[2].y0 = 256; seg[2].y1 = sus; seg[2].curve = curved;
    seg[4].w = holds ? (int32_t)r * 56 / 127 : 0; seg[4].y0 = sus; seg[4].y1 = 0; seg[4].curve = curved;
    seg[3].w = holds ? 232 - seg[0].w - seg[1].w - seg[2].w - 56 : 0; seg[3].y0 = seg[3].y1 = sus; seg[3].curve = 0;
    off_x = 4 + seg[0].w + seg[1].w + seg[2].w + seg[3].w;
    for (x = 4; x < 236; x++) {
        int32_t level = 0;
        for (k = 0, x0 = 4; k < 5; x0 += seg[k].w, k++)
            if (x < x0 + seg[k].w) {
                int32_t t = (x - x0) * 256 / seg[k].w, u = 256 - t;
                level = seg[k].curve ? seg[k].y1 + (seg[k].y0 - seg[k].y1) * u / 256 * u / 256
                                     : seg[k].y0 + (seg[k].y1 - seg[k].y0) * t / 256;
                break;
            }
        mu_graph_col(pal, h, (uint32_t)x, level, &prev, 1);
    }
    if (holds)                                       /* where the note is let go */
        for (k = 2; k < (int32_t)h - 4; k += 4) cv_rect(off_x, k, 1, 2, pal[MU_BG]);
}
/* an LFO: two of its cycles (ONCE: one, and where it stands then) */
static void mu_graph_lfo(const uint16_t *pal, uint32_t h, const masif_mod_t *m)
{
    static const int16_t STEPS[8] = {120, -200, 60, 230, -90, -250, 180, -30};
    uint32_t shape = m->p[0] % MASIF_LFO_SHAPES, x, rnd = 12345u;
    int32_t prev = -1, v = 0;
    for (x = 0; x < 240u; x += 4u) cv_rect((int32_t)x, (int32_t)h / 2 - 1, 2, 1, pal[MU_MID]);
    for (x = 4; x < 236u; x++) {
        uint32_t ph = (x - 4u) * 565u;                /* (232 columns: 2 x 65536) */
        if (m->p[1] % MASIF_LFO_TRIGS == MASIF_TRIG_ONCE && ph > 0xFFFFu) ph = 0xFFFFu;
        if (shape == MASIF_LFO_RANDOM) v = STEPS[(x - 4u) / 29u];
        else if (shape == MASIF_LFO_DRUNK) {
            rnd = rnd * 1664525u + 1013904223u;
            v = mu_clampi(v + (int32_t)((rnd >> 16) % 61u) - 30, -256, 256);
        } else v = masif_lfo_shape(shape, ph);
        mu_graph_col(pal, h, x, (v + 256) / 2, &prev, 0);
    }
}

/* the picture: the sample (the instrument page), the level's envelope (its AMP tab), the open slot's envelope or
 * LFO (the modulation page). Drawn again only when something it shows changed. */
static const masif_instrument_t *mu_smp_inst(void);
static void mu_draw_graph(void)
{
    const uint16_t *pal = MU_PALETTE[mu.palette % MU_NPALETTE];
    const masif_instrument_t *in = mu.page == MU_SAMPLER ? mu_smp_inst() : mu_inst();
    const uint32_t h = (MU_WAVE_R1 - MU_WAVE_R0) * MU_CH;
    uint32_t key = 2166136261u, i, r, c, n = sp_wave[in->sample % MASIF_SAMPLES].n;
#define MU_MIX(b) key = (key ^ (uint8_t)(b)) * 16777619u
    for (i = 0; i < sizeof *in; i++) MU_MIX(((const uint8_t *)in)[i]);
    MU_MIX(mu.page); MU_MIX(mu.itab); MU_MIX(mu.mslot); MU_MIX(mu.inst); MU_MIX(mu.palette);
    MU_MIX(n); MU_MIX(n >> 8); MU_MIX(n >> 16); MU_MIX(mu.zoom); MU_MIX(mu_par_row());
    n = sp_wave[in->sample % MASIF_SAMPLES].lp_a; MU_MIX(n); MU_MIX(n >> 8); MU_MIX(n >> 16);
    n = sp_wave[in->sample % MASIF_SAMPLES].lp_b; MU_MIX(n); MU_MIX(n >> 8); MU_MIX(n >> 16);
    if (mu.page == MU_INST && mu.itab != MI_AMP && in->sample < MASIF_SAMPLES && sp_wave[in->sample].n) {   /* (the playheads move) */
        uint32_t a, b, la, lb, ti, to, vs, vn, t;
        mu_wave_view(in, sp_wave[in->sample].n, &a, &b, &la, &lb, &ti, &to, &vs, &vn);
        for (t = 0; t < MASIF_TRACKS; t++) MU_MIX(mu_head_x(t, &sp_wave[in->sample], vs, vn) + 1);
    }
#undef MU_MIX
    if (mu.wave_drawn && key == mu.wave_key) return;
    mu.wave_drawn = 1; mu.wave_key = key;
    cv_begin(240, h, pal[MU_DIM]);
    if (mu.page == MU_MOD) {
        const masif_mod_t *m = &in->mod[mu.mslot % MASIF_MODS];
        if (m->type == MASIF_MOD_AHD || m->type == MASIF_MOD_TRIG) mu_graph_env(pal, h, m->p[0], m->p[1], m->p[2], 0, 0, 0, 0);
        else if (m->type == MASIF_MOD_ADSR) mu_graph_env(pal, h, m->p[0], 0, m->p[1], m->p[2], m->p[3], 0, 1);
        else if (m->type == MASIF_MOD_LFO) mu_graph_lfo(pal, h, m);
        else cv_rect(4, (int32_t)h - 4, 232, 2, pal[MU_MID]);
    } else if (mu.page != MU_SAMPLER && mu.itab == MI_AMP) {   /* (DECAY 7F: none, the level stays up) */
        mu_graph_env(pal, h, in->attack, 0, in->decay == 127u ? 0u : in->decay, in->decay == 127u ? 127u : in->sustain,
                     in->release, 1, 1);
    } else {
        mu_graph_wave(in, pal, h);
    }
    cv_blit(0, MU_Y0 + MU_WAVE_R0 * MU_CH);
    for (r = MU_WAVE_R0; r < MU_WAVE_R1; r++)       /* (these rows of the LCD are no text now: any text page redraws them) */
        for (c = 0; c < MU_COLS; c++) mu_shown_ch[r][c] = 0xFFu;
}

/* ---- the mixer: the four tracks' levels as bars, their mutes, the mix's EQ */
static void mu_draw_mixer(void)
{
    static const char *const band[3] = {"LO", "MID", "HI"};
    uint32_t t, r, k;
    mu_title("MIXER");
    for (t = 0; t < MASIF_TRACKS; t++) {
        uint32_t lv = masif_song.track_level[t], cells = (lv * 10u + 63u) / 127u, col = 1u + t * 3u;
        int muted = (masif_mute >> t) & 1u;
        mu_dec(col, 1, t + 1u, 1, MU_MID);
        for (r = 0; r < 10u; r++)
            mu_fill(col, MU_DATA + r, col + 2u, MU_DATA + r + 1u, (uint8_t)(MU_INK | MU_ON(10u - r <= cells ? (muted ? MU_DIM : MU_MID) : MU_BG)));
        mu_hex(col, 13, lv, 2, mu.mcol == t ? MU_CURSOR : MU_INK);
        mu_put(col, 14, (mu.solo >> t) & 1u ? "SO" : muted ? "MU" : "ON", muted || ((mu.solo >> t) & 1u) ? MU_INK : MU_DIM);
    }
    mu_put(14, 1, "EQ", MU_MID);
    for (k = 0; k < 3u; k++) {
        mu_put(14, 3u + k * 2u, band[k], MU_MID);
        mu_signed(17, 3u + k * 2u, masif_song.master_eq[k], mu.mcol == 4u + k ? MU_CURSOR : MU_INK);
    }
}

/* ---- the project */
enum { MJ_SLOT, MJ_TEMPO, MJ_KEY, MJ_SCALE, MJ_GROOVE, MJ_LOAD, MJ_SAVE, MJ_NEW, MJ_TEST, MJ_COLOR, MJ_FONT, MJ_N };

static void mu_sname_set(uint32_t k, char c);
static void mu_sname_turn(uint32_t k, int32_t d);
static void mu_draw_project(void)
{
    static const struct { uint8_t row; const char *label; } L[MJ_N] = {
        {2, "SONG"}, {3, "TEMPO"}, {4, "KEY"}, {5, "SCALE"}, {6, "GROOVE"}, {8, "LOAD"}, {9, "SAVE"}, {10, "NEW"}, {11, "TEST"}, {12, "THEME"}, {13, "FONT"}};
    uint32_t i;
    mu_title("PROJECT");
    for (i = 0; i < MJ_N; i++) mu_put(0, L[i].row, L[i].label, i == mu.prow && i >= MJ_LOAD && i <= MJ_TEST ? MU_CURSOR : MU_MID);
#define MU_AT(n) ((uint8_t)(mu.prow == (n) ? MU_CURSOR : MU_INK))
    mu_dec(8, 2, mu.slot + 1u, 2, mu.prow == MJ_SLOT && !mu.schar ? MU_CURSOR : MU_INK);
    for (i = 0; i < 8u; i++) {                      /* the song's name, a character a cell ('.': none) */
        char c[2] = {mu.sname[i] ? mu.sname[i] : '.', 0};
        mu_put(11u + i, 2, c, mu.prow == MJ_SLOT && mu.schar == i + 1u ? MU_CURSOR : mu.sname[i] ? MU_INK : MU_DIM);
    }
#if MASIF_SONG_STORE
    {
        masif_store_info_t info;                    /* (what LOAD would bring: the slot's song) */
        (void)masif_store_info(mu.slot, &info);
        mu_put(8, 8, info.used ? info.name : "EMPTY", MU_DIM);
        mu_put(0, 14, "ROOM", MU_MID);
        mu_dec(8, 14, masif_store_free() * 4u, 2, MU_INK);
        mu_put(11, 14, "KB", MU_MID);
    }
#endif
    mu_dec(8, 3, masif_song.tempo ? masif_song.tempo : sq.bpm10 / 10u, 3, MU_AT(MJ_TEMPO));
    mu_key(8, 4, MU_AT(MJ_KEY));
    mu_put(8, 5, masif_song.scale < MASIF_SCALES ? masif_scale_name[masif_song.scale] : "?", MU_AT(MJ_SCALE));
    mu_hex(8, 6, masif_song.project_groove, 2, MU_AT(MJ_GROOVE));  /* (the groove the tracks start on: GRV goes before it) */
    mu_put(8, 11, "LOAD THE HEAVY", MU_DIM);       /* (EDIT: the song for the audio's load, mu_load_test) */
    mu_dec(8, 12, mu.palette % MU_NPALETTE + 1u, 1, MU_AT(MJ_COLOR));
    mu_put(8, 13, mu.font ? "BLOCK" : "SMOOTH", MU_AT(MJ_FONT));
    mu_put(0, 15, "CPU", MU_MID);                   /* the sound's share of the time (the most in the last second) */
    mu_dec(8, 15, MU_LOAD(0), 3, MU_INK); mu_put(11, 15, "%", MU_MID);
    mu_put(13, 15, "MAX", MU_MID); mu_dec(16, 15, MU_LOAD(1), 3, MU_INK); mu_put(19, 15, "%", MU_MID);
    mu_put(0, 16, "KEYS AT START", MU_MID);         /* (the USB rescue wants OCT- alone: 0001) */
    mu_hex(14, 16, MU_BOOT_KEYS() & 0x3FFFu, 4, MU_INK);
#undef MU_AT
}

/* ---- the pages that are grids of cells (song, chain, phrase, table). A row of any of them as bytes, a byte a
 * column: the song's four chains; a chain step's phrase and transpose; a phrase's or a table's row as it is */
static int mu_is_grid(uint32_t page) { return page == MU_SONG || page == MU_CHAIN || page == MU_PHRASE || page == MU_TABLE || page == MU_GROOVE; }
static uint32_t mu_ncol(uint32_t page)
{
    return page == MU_SONG ? MASIF_TRACKS : page == MU_CHAIN ? 2u : page == MU_PHRASE ? (uint32_t)MP_NCOL : page == MU_GROOVE ? 1u : (uint32_t)MT_NCOL;
}
static uint32_t mu_nrow(uint32_t page) { return page == MU_SONG ? MASIF_SONG_ROWS : 16u; }
/* the first effect column (an effect and its parameter are two columns that go together), or past the last */
static uint32_t mu_fx0(uint32_t page) { return page == MU_PHRASE ? (uint32_t)MP_FX1 : page == MU_TABLE ? (uint32_t)MT_FX1 : 99u; }
static void mu_cur(uint32_t *row, uint32_t *col)
{
    switch (mu.page) {
    case MU_SONG: *row = mu.song_row; *col = mu.track; break;
    case MU_CHAIN: *row = mu.step; *col = mu.ccol; break;
    case MU_PHRASE: *row = mu.row; *col = mu.pcol; break;
    case MU_GROOVE: *row = mu.grow; *col = 0; break;
    default: *row = mu.trow; *col = mu.tcol; break;
    }
}
static void mu_row_get(uint32_t page, uint32_t r, uint8_t *d)
{
    uint32_t t;
    for (t = 0; t < 7u; t++) d[t] = 0;
    switch (page) {
    case MU_SONG:
        for (t = 0; t < MASIF_TRACKS; t++) d[t] = masif_song.song[r & 255u][t];
        break;
    case MU_CHAIN: {
        const masif_chain_step_t *c = &masif_song.chain[mu.chain % MASIF_CHAINS][r % MASIF_CHAIN_STEPS];
        d[0] = c->phrase; d[1] = (uint8_t)c->transpose;
        break;
    }
    case MU_PHRASE: {
        const masif_cell_t *c = &masif_song.phrase[mu.phrase % MASIF_PHRASES][r % MASIF_ROWS];
        d[0] = c->note; d[1] = c->instrument; d[2] = c->volume; d[3] = c->fx; d[4] = c->param; d[5] = c->fx2; d[6] = c->param2;
        break;
    }
    case MU_GROOVE: d[0] = masif_song.groove[mu.groove % MASIF_GROOVES][r % MASIF_GROOVE_STEPS]; break;
    default: {
        const masif_table_cell_t *c = &masif_song.table[mu.inst % MASIF_TABLES][r % MASIF_TABLE_ROWS];
        d[0] = (uint8_t)c->transpose; d[1] = c->volume; d[2] = c->fx; d[3] = c->param; d[4] = c->fx2; d[5] = c->param2;
        break;
    }
    }
}
static void mu_row_post(uint32_t page, uint32_t r, const uint8_t *d)
{
    uint32_t t;
    switch (page) {
    case MU_SONG:                                    /* (a cell an edit: only those that change) */
        for (t = 0; t < MASIF_TRACKS; t++)
            if (masif_song.song[r & 255u][t] != d[t]) mu_edit1(MASIF_EDIT_SONG, r, t, d[t]);
        break;
    case MU_CHAIN: mu_edit(MASIF_EDIT_CHAIN, mu.chain, r, d); break;
    case MU_PHRASE: mu_edit(MASIF_EDIT_CELL, mu.phrase, r, d); break;
    case MU_GROOVE: mu_edit1(MASIF_EDIT_GROOVE, mu.groove, r, d[0]); break;
    default: mu_edit(MASIF_EDIT_TABLE, mu.inst, r, d); break;
    }
}
/* what an empty cell of a column holds */
static uint8_t mu_col_empty(uint32_t page, uint32_t col)
{
    if (page == MU_SONG) return MASIF_NONE;
    if (page == MU_CHAIN) return col ? 0u : MASIF_NONE;
    if (page == MU_PHRASE) return col == MP_INST || col == MP_VOL ? 0xFFu : 0u;
    if (page == MU_GROOVE) return 0xFFu;
    return col == MT_V ? 0xFFu : 0u;
}

/* ---- the selection (shift + option): from where it began to the cursor; shift + option again: the
 * whole column (16 rows), then every column. On the song page at the most 16 rows, and the second step is all
 * four tracks. 1 if there is one; without one the rectangle is the cursor's cell. pairs: an effect and its
 * parameter go together (always, in a selection) */
static int mu_rect(uint32_t *r0, uint32_t *r1, uint32_t *c0, uint32_t *c1, int pairs)
{
    uint32_t row, col, fx0 = mu_fx0(mu.page);
    mu_cur(&row, &col);
    *r0 = *r1 = row; *c0 = *c1 = col;
    if (mu.sel) {
        pairs = 1;
        if (mu.sel_row < row) *r0 = mu.sel_row; else *r1 = mu.sel_row;
        if (mu.sel_col < col) *c0 = mu.sel_col; else *c1 = mu.sel_col;
        if (mu.sel_mode >= 1u && mu.page != MU_SONG) { *r0 = 0; *r1 = 15; }
        if (mu.sel_mode >= 2u || (mu.sel_mode && mu.page == MU_SONG)) { *c0 = 0; *c1 = mu_ncol(mu.page) - 1u; }
        if (*r1 - *r0 > 15u) *r1 = *r0 + 15u;
    }
    if (pairs && !mu.sel && mu.page == MU_PHRASE && col == MP_NOTE) *c1 = MP_VOL;   /* a note with its instrument and volume */
    if (pairs && *c0 >= fx0) *c0 = fx0 + ((*c0 - fx0) & ~1u);
    if (pairs && *c1 >= fx0) *c1 = (fx0 + ((*c1 - fx0) & ~1u)) + 1u;
    return mu.sel;
}
/* shift + option: a selection begins at the cursor, or the one there is grows. On the instrument and the
 * modulation page it is the whole instrument */
static void mu_sel_step(void)
{
    uint32_t row, col;
    if (!mu_is_grid(mu.page) && mu.page != MU_INST && mu.page != MU_MOD) return;
    if (mu.page == MU_GROOVE && mu.gnum) return;
    if (!mu.sel) {
        mu_cur(&row, &col);
        mu.sel = 1; mu.sel_mode = 0; mu.sel_row = (uint8_t)row; mu.sel_col = (uint8_t)col;
    } else if (mu_is_grid(mu.page)) {
        mu.sel_mode = (uint8_t)((mu.sel_mode + 1u) % (mu.page == MU_SONG ? 2u : 3u));
    }
}

/* the clipboard: rows of a grid page (which columns, and of which page), and an instrument */
static struct {
    uint8_t page, c0, c1, n, inst_ok;
    uint8_t d[16][7];
    masif_instrument_t inst;
} mu_clip;

static void mu_inst_post(const masif_instrument_t *to)    /* the instrument the pages are on becomes *to */
{
    const uint8_t *a = (const uint8_t *)mu_inst(), *b = (const uint8_t *)to;
    uint32_t i;
    for (i = 0; i < sizeof *to; i++)
        if (a[i] != b[i]) mu_edit1(MASIF_EDIT_INSTRUMENT, mu.inst, i, b[i]);
}
/* option: the selection (or the cursor's cell) into the clipboard */
static void mu_copy(void)
{
    uint32_t r0, r1, c0, c1, i;
    if (mu.page == MU_INST || mu.page == MU_MOD) { mu_clip.inst = *mu_inst(); mu_clip.inst_ok = 1; return; }
    if (!mu_is_grid(mu.page)) return;
    (void)mu_rect(&r0, &r1, &c0, &c1, 1);
    mu_clip.page = mu.page; mu_clip.c0 = (uint8_t)c0; mu_clip.c1 = (uint8_t)c1; mu_clip.n = (uint8_t)(r1 - r0 + 1u);
    for (i = 0; i < mu_clip.n; i++) mu_row_get(mu.page, r0 + i, mu_clip.d[i]);
}
/* option + enter: cut. The selection (or the cursor's cell) into the clipboard, and empty. A selected instrument:
 * as an empty song has it; on those pages without a selection: the setting under the cursor */
static void mu_cut(void)
{
    uint32_t r0, r1, c0, c1, r, c;
    uint8_t d[7];
    if (mu.page == MU_INST || mu.page == MU_MOD) {
        masif_instrument_t def;
        mu_par_t p;
        masif_instrument_default(&def, mu.inst);
        if (mu.sel) { mu_copy(); mu_inst_post(&def); mu.sel = 0; mu_say("CUT"); }
        else if (mu_par(mu_par_row(), &p)) mu_edit1(MASIF_EDIT_INSTRUMENT, mu.inst, p.off, ((const uint8_t *)&def)[p.off]);
        return;
    }
    if (!mu_is_grid(mu.page)) return;
    mu_copy();
    if (mu_rect(&r0, &r1, &c0, &c1, 1)) mu_say("CUT");
    for (r = r0; r <= r1; r++) {
        mu_row_get(mu.page, r, d);
        for (c = c0; c <= c1; c++) d[c] = mu_col_empty(mu.page, c);
        mu_row_post(mu.page, r, d);
    }
    mu.sel = 0;
}
/* shift + enter: the clipboard in at the cursor, downwards. Its columns go where they came from; on the song page
 * from the cursor's track on; one effect column's also into the other, if the cursor is there. The cursor ends
 * under what was pasted */
static void mu_paste(void)
{
    uint32_t row, col, i, c, n = mu_ncol(mu.page), fx0 = mu_fx0(mu.page);
    int32_t shift = 0;
    uint8_t d[7];
    if (mu.page == MU_INST || mu.page == MU_MOD) {
        if (!mu_clip.inst_ok) { mu_say("NOTHING COPIED"); return; }
        mu_inst_post(&mu_clip.inst);
        mu.sel = 0;
        mu_say("PASTED");
        return;
    }
    if (!mu_is_grid(mu.page)) return;
    if (mu_clip.page != mu.page || !mu_clip.n) { mu_say("NOTHING COPIED"); return; }
    mu.sel = 0;
    mu_cur(&row, &col);
    if (mu.page == MU_SONG) shift = (int32_t)col - mu_clip.c0;
    else if (mu_clip.c0 >= fx0 && mu_clip.c1 == mu_clip.c0 + 1u && col >= fx0)
        shift = (int32_t)(fx0 + ((col - fx0) & ~1u)) - mu_clip.c0;
    for (i = 0; i < mu_clip.n && row + i < mu_nrow(mu.page); i++) {
        mu_row_get(mu.page, row + i, d);
        for (c = mu_clip.c0; c <= mu_clip.c1; c++)
            if ((uint32_t)((int32_t)c + shift) < n) d[(int32_t)c + shift] = mu_clip.d[i][c];
        mu_row_post(mu.page, row + i, d);
    }
    mu_cursor(0, (int32_t)i);
    mu_say("PASTED");
}

/* the selection on the screen: its cells dark on the middle shade (the cursor stays the brightest) */
static void mu_draw_sel(void)
{
    static const uint8_t PH[MP_NCOL][2] = {{2, 5}, {5, 7}, {7, 9}, {10, 13}, {13, 15}, {15, 18}, {18, 20}};
    static const uint8_t TB[MT_NCOL][2] = {{2, 4}, {5, 7}, {8, 11}, {11, 13}, {13, 16}, {16, 18}};
    uint32_t r0, r1, c0, c1, r, c, x, top = mu.page == MU_SONG ? mu.song_top : 0u;
    if (!mu.sel || !mu_is_grid(mu.page)) return;
    (void)mu_rect(&r0, &r1, &c0, &c1, 1);
    for (r = r0; r <= r1; r++) {
        if (r < top || r >= top + 16u) continue;
        for (c = c0; c <= c1; c++) {
            uint32_t x0, x1;
            if (mu.page == MU_PHRASE) { x0 = PH[c][0]; x1 = PH[c][1]; }
            else if (mu.page == MU_TABLE) { x0 = TB[c][0]; x1 = TB[c][1]; }
            else { x0 = 3u + c * 3u; x1 = x0 + 2u; }
            for (x = x0; x < x1; x++)
                if (mu_at[MU_DATA + r - top][x] != MU_CURSOR) mu_at[MU_DATA + r - top][x] = MU_BG | MU_ON(MU_MID);
        }
    }
}

/* ---- the chooser: EDIT held on a cell that is one of a list (an effect; a setting with names; the project's
 * scale) shows the list, the knobs go through it, letting EDIT go enters what is marked */
/* The effects' own list (EDIT or FX held on an effect column): the whole screen (the user, 2026-10-12): at the top
 * the marked command's name, then the commands by their letters, 5 a row (room for 55), and at the bottom what
 * the marked one does and what its two digits are. (No MIDI commands: there are none yet.) */
#define MU_FXP_COLS 5u
#define MU_FXP_LIST 2u                              /* the commands' first row */
#define MU_FXP_HELP (MU_ROWS - 4u)                  /* the help's (the last row is the messages')  */
static const char *const MU_FX_LONG[MU_NFX] = {
    "NONE", "TEMPO", "", "PHRASE HOP", "", "", "SLIDE", "VIBRATO", "ARPEGGIO",
    "RETRIGGER", "", "", "TABLE", "", "PROBABILITY", "STOP", "SAMPLE OFFSET", "REVERSE",
    "PAN", "SEND", "TABLE HOP", "KILL", "REPEAT", "REPEAT TO", "RANDOM LEFT", "GROOVE", "",
    "MICRO TIMING", "PITCH BEND", "SLICE"};
static const char *const MU_FX_HELP[MU_NFX][3] = {
    {"NO COMMAND", "", ""},
    {"THE SONG'S TEMPO", "", "XX 28-F0 = 40-240"},
    {"", "", ""},
    {"PHRASE ENDS, THE", "NEXT STARTS AT ROW Y", "Y ROW 0-F"},
    {"", "", ""},
    {"", "", ""},
    {"SLIDE UP OR DOWN TO", "THIS NOTE, NO NEW HIT", "XX SPEED"},
    {"PITCH VIBRATO", "", "X SPEED  Y DEPTH"},
    {"NOTE, +X, +Y,", "A TICK EACH", "X Y SEMITONES"},
    {"HIT THE NOTE AGAIN", "EVERY Y TICKS", "X 1-7 FADE 9-F SWELL"},
    {"", "", ""},
    {"", "", ""},
    {"RUN TABLE XX ON", "THIS TRACK", "XX TABLE 00-7F"},
    {"", "", ""},
    {"CHANCE THAT THE", "NOTE PLAYS", "XX PERCENT 00-99"},
    {"LET THE NOTE GO:", "ITS RELEASE RINGS", "XX TICKS  00 AT ONCE"},
    {"START INTO THE", "SAMPLE, FROM ITS START", "XX 00-FF POSITION"},
    {"NOTE PLAYS BACKWARDS", "", "XX 01 ON"},
    {"PAN FOR THIS STEP", "", "XX 00 L  80 MID FF R"},
    {"SET A SEND LEVEL", "X 0 CHO 1 DLY 2 REV", "Y AMOUNT 0-F"},
    {"TABLE GOES ON AT", "ITS ROW Y", "Y ROW 0-F"},
    {"KILL: NOTE AND ALL", "EFFECTS OFF, AT ONCE", "XX TICKS  00 AT ONCE"},
    {"REPEAT LAST COMMAND", "EVERY ROW, VALUE +XX", "80-FF DOWN  00 OFF"},
    {"AFTER REP: THE VALUE", "ITS REPEATS STOP AT", "XX VALUE"},
    {"RANDOM VALUE FOR THE", "FX AT LEFT (OR NOTE)", "X Y RANGE PER DIGIT"},
    {"THIS TRACK'S ROWS", "FOLLOW GROOVE XX", "XX GROOVE 00-0F"},
    {"", "", ""},
    {"ROW STARTS LATE OR", "EARLY: 18 = ONE ROW", "01+ LATE  FF- EARLY"},
    {"BEND UP X ROWS, THEN", "DOWN Y ROWS  F: ON", "X UP  Y DOWN  ROWS"},
    {"PLAY SLICE XX OF A", "SLICED INSTRUMENT", "XX SLICE 00-"}};
/* the effects by their letters (none first): the list's order, and the order a turned effect cell goes in (the
 * ones taken out are not in it) */
static const uint8_t MU_FX_ORDER[] = {
    MASIF_FX_NONE, MASIF_FX_ARPEGGIO, MASIF_FX_KILL, MASIF_FX_GROOVE, MASIF_FX_BREAK, MASIF_FX_MICROTIME,
    MASIF_FX_SAMPLE_OFFSET, MASIF_FX_PAN, MASIF_FX_PITCH_BEND, MASIF_FX_PROBABILITY, MASIF_FX_REPEAT, MASIF_FX_RETRIGGER,
    MASIF_FX_REVERSE, MASIF_FX_RANDOM_LEFT, MASIF_FX_REPEAT_TO, MASIF_FX_SLICE, MASIF_FX_SLIDE, MASIF_FX_SEND,
    MASIF_FX_STOP, MASIF_FX_SET_TABLE, MASIF_FX_TABLE_HOP, MASIF_FX_SET_TEMPO, MASIF_FX_VIBRATO};
#define MU_NFXL (sizeof MU_FX_ORDER / sizeof MU_FX_ORDER[0])
static uint32_t mu_pick_fx(uint32_t i) { return MU_FX_ORDER[i % MU_NFXL]; }
static uint32_t mu_fx_index(uint32_t fx)
{
    uint32_t i;
    for (i = 0; i < MU_NFXL; i++) if (MU_FX_ORDER[i] == fx) return i;
    return 0;
}
/* ---- the sample pool (the user, 2026-10-14): EDIT held on the instrument page's SAMPLE or its number shows every
 * sample there is, PLAY hears the marked one as it is, letting EDIT go gives it to an instrument: the page's own if
 * that one is empty (as an empty song has it and in no phrase: "created first"), else the next empty one after
 * it, and the page goes there. A sample with its own loop (the web app's) gives the instrument that loop and its
 * play mode. Letting go on the instrument's own sample, or HOME, changes nothing. */
#define MU_POOL_ROW0 2u                             /* the list's first row, */
#define MU_POOL_ROWS 14u                            /* and how many are seen at once */
static uint8_t mu_pool_cancel;
/* OPTION + EDIT asks once; again within 3 s on the same thing (k: a slot, 0xFE: every sample) it is done */
#define MU_DEL_MS 3000u
static uint8_t mu_del_k = 0xFFu;
static uint32_t mu_del_at;
static int mu_del_again(uint32_t k)
{
    if (mu_del_k == k && (uint32_t)(fm1_ms - mu_del_at) <= MU_DEL_MS) { mu_del_k = 0xFFu; return 1; }
    mu_del_k = (uint8_t)k; mu_del_at = fm1_ms;
    return 0;
}
static int mu_del_asked(uint32_t k) { return mu_del_k == k && (uint32_t)(fm1_ms - mu_del_at) <= MU_DEL_MS; }
static uint32_t mu_pool_count(void)
{
    uint32_t w, k = 0;
    for (w = 0; w < MASIF_SAMPLES; w++) if (sp_wave[w].n) k++;
    return k;
}
static uint32_t mu_pool_wave(uint32_t i)            /* the i-th sample there is: its wave */
{
    uint32_t w;
    for (w = 0; w < MASIF_SAMPLES; w++) if (sp_wave[w].n && !i--) return w;
    return 0;
}
static int mu_inst_empty(uint32_t i) { return masif_song.instrument[i % MASIF_INSTRUMENTS].sample >= MASIF_SAMPLES; }
static int32_t mu_next_empty(void)                  /* the first instrument without a sample (-1: none is) */
{
    uint32_t i;
    for (i = 0; i < MASIF_INSTRUMENTS; i++) if (mu_inst_empty(i)) return (int32_t)i;
    return -1;
}
static void mu_pool_open(void)
{
    uint32_t i, n = mu_pool_count();
    mu.pick = PK_POOL; mu.pick_n = (uint8_t)n; mu.pick_cur = 0; mu.pick_cols = 1; mu.pick_w = 0;
    mu_pool_cancel = 0;
    for (i = 0; i < n; i++) if (mu_pool_wave(i) == mu_inst()->sample) mu.pick_cur = (uint8_t)i;
}
static void mu_give_sample(uint32_t w);
static void mu_pool_delete(void)                    /* OPTION + EDIT in the pool: the marked sample deleted (twice) */
{
    uint32_t k = mu_pool_wave(mu.pick_cur) - KIT_NWAVE;
    if (!mu.pick_n || k >= SP_NUSER) return;
    if (mr_busy()) { mu_say("RECORDING"); return; }
    if (!mu_del_again(k)) { mu_say("AGAIN: DELETE IT"); return; }
    masif_fm1_preview(mu.track, 0, 0, 0);
    mu_say(mr_delete(k) ? "NOT DELETED" : "DELETED");
    mu.pick_n = (uint8_t)mu_pool_count();
    if (mu.pick_cur && mu.pick_cur >= mu.pick_n) mu.pick_cur = (uint8_t)(mu.pick_n - 1u);
    mu.wave_drawn = 0;
}
static void mu_pool_close(void)
{
    mu.wave_drawn = 0;
    if (mu_pool_cancel || !mu.pick_n) return;
    mu_give_sample(mu_pool_wave(mu.pick_cur));
}
/* the instrument the pages are on gets wave w (an empty one gets it, a full one's is replaced), as the pool lets it
 * begin: its own loop, play mode and tapers */
static void mu_give_sample(uint32_t w)
{
    static char said[20];
    uint32_t t, k;
    const sp_wave_t *sw;
    const char *name;
    if (w == mu_inst()->sample) return;
    t = mu.inst;
    sw = &sp_wave[w];
    mu_edit1(MASIF_EDIT_INSTRUMENT, t, MU_OFF(sample), (uint8_t)w);
    k = sw->lp_b && sw->lp_play < MASIF_PLAY_LOOP ? MASIF_PLAY_LOOP : sw->lp_play;   /* (a loop is there to loop) */
    mu_edit1(MASIF_EDIT_INSTRUMENT, t, MU_OFF(play), (uint8_t)k);
    mu_edit1(MASIF_EDIT_INSTRUMENT, t, MU_OFF(flags),
             (uint8_t)((mu_inst()->flags & ~MASIF_INST_SMPLOOP) | (sw->lp_b ? MASIF_INST_SMPLOOP : 0u)));
    mu_edit1(MASIF_EDIT_INSTRUMENT, t, MU_OFF(taper_in), sw->lp_b ? sw->lp_tin : 0u);     /* (the web app's tapers) */
    mu_edit1(MASIF_EDIT_INSTRUMENT, t, MU_OFF(taper_out), sw->lp_b ? sw->lp_tout : 0u);
    mu.irow = 0; mu.inum = 0;
    name = MU_WAVE_NAME(w);
    for (k = 0; k < 5u; k++) said[k] = "INST "[k];
    said[5] = "0123456789ABCDEF"[t >> 4]; said[6] = "0123456789ABCDEF"[t & 15u]; said[7] = ' ';
    for (k = 0; k < 11u && name[k]; k++) said[8u + k] = name[k];
    said[8u + k] = 0;
    mu_say(said);
}
static void mu_draw_pool(void)
{
    const uint8_t box = MU_INK | MU_ON(MU_DIM);
    uint32_t i, top;
    mu_fill(0, 0, MU_COLS, MU_ROWS, box);
    mu_put(0, 0, "SAMPLE POOL", MU_BG | MU_ON(MU_INK));
    if (mu.page != MU_SAMPLER) {
        mu_put(12, 0, "INST", MU_MID | MU_ON(MU_DIM));
        mu_hex(17, 0, mu.inst, 2, MU_MID | MU_ON(MU_DIM));
    }
    mu_put(0, 1, "NO NAME     B  SEC", MU_MID | MU_ON(MU_DIM));
    if (!mu.pick_n) {
        mu_put(1, 4, "NO SAMPLES YET:", box);
        mu_put(1, 5, "RECORD ONE ON THE", box);
        mu_put(1, 6, "SAMPLER (BELOW)", box);
        mu_put(1, 7, "OR SYNC THE POOL", box);
        mu_put(1, 8, "IN THE WEB APP", box);
        return;
    }
    top = mu.pick_cur < MU_POOL_ROWS / 2u ? 0u : mu.pick_cur - MU_POOL_ROWS / 2u;
    if (top + MU_POOL_ROWS > mu.pick_n) top = mu.pick_n > MU_POOL_ROWS ? mu.pick_n - MU_POOL_ROWS : 0u;
    for (i = top; i < mu.pick_n && i < top + MU_POOL_ROWS; i++) {
        uint32_t w = mu_pool_wave(i), r = MU_POOL_ROW0 + i - top, rate = sp_wave[w].rate, cs;
        uint8_t at = i == mu.pick_cur ? MU_CURSOR : box;
        int mine = mu.page != MU_SAMPLER && w == mu_inst()->sample;   /* (the instrument's: *) */
        if (sp_wave[w].uflags & 1u) rate = rate * 33u / 45u;        /* (as it plays: stored for 45->33, or x2) */
        if (sp_wave[w].uflags & 2u) rate /= 2u;
        cs = rate ? (uint32_t)((uint64_t)sp_wave[w].n * 100u / rate) : 0u;
        mu_fill(0, r, MU_COLS, r + 1u, at);
        mu_hex(0, r, w, 2, at);
        if (mine) mu_put(2, r, "*", at);
        mu_putn(3, r, MU_WAVE_NAME(w), 8, at);
        if (sp_wave[w].lp_b) mu_put(11, r, "L", at);                 /* (it has its own loop) */
#if MASIF_ENABLE_TRACKER
        mu_put(12, r, sp_wave[w].uflags & 8u ? "4" : sp_wave[w].uflags & 4u ? "8" : "", at);   /* (12 bit: nothing) */
#endif
        if (cs >= 1000u) mu_dec(14, r, cs / 100u > 99u ? 99u : cs / 100u, 2, at);
        else mu_dec(15, r, cs / 100u, 1, at);
        mu_put(16, r, ".", at);
        mu_dec(17, r, cs % 100u, 2, at);
    }
#if MASIF_CONTROLS == 0                             /* (it stays open: EDIT takes, OPTION alone leaves) */
    mu_put(0, MU_ROWS - 2u, "EDIT TAKE  PLAY HEAR", MU_MID | MU_ON(MU_DIM));
    if (mu_del_asked(mu_pool_wave(mu.pick_cur) - KIT_NWAVE)) mu_put(0, MU_ROWS - 1u, "OPT+EDIT AGAIN: DEL", MU_BG | MU_ON(MU_INK));
    else mu_put(0, MU_ROWS - 1u, "OPT OUT OPT+EDIT DEL", MU_MID | MU_ON(MU_DIM));
#else
    mu_put(0, MU_ROWS - 2u, "PLAY: HEAR  HOME: NO", MU_MID | MU_ON(MU_DIM));
#endif
}

static const char MU_CHARSET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.#!?+*/:";   /* a name's characters */
#define MU_NCHARSET (sizeof MU_CHARSET - 1u)
static const char *mu_pick_name(uint32_t i)
{
    static char one[2];
    if (mu.pick == PK_CHAR) { one[0] = MU_CHARSET[i % MU_NCHARSET]; return one; }
    if (mu.pick == PK_FX) return MU_FX[mu_pick_fx(i)];
    if (mu.pick == PK_PAR) return mu_pick_par.names[i];
    return masif_scale_name[i];
}
static void mu_pick_open(void)
{
    uint32_t row, col, i, w = 0, fx0 = mu_fx0(mu.page);
    uint8_t d[7];
    mu.pick = PK_NONE;
    if (mu.sel) return;
    if (mu.page == MU_PHRASE || mu.page == MU_TABLE) {
        uint32_t fx;
        mu_cur(&row, &col);
        if (col < fx0) return;
#if MASIF_CONTROLS == 0
        if ((col - fx0) & 1u) return;               /* (on a parameter EDIT + the arrows turn it) */
#endif
        mu_row_get(mu.page, row, d);
        fx = d[fx0 + ((col - fx0) & ~1u)];
        if (!fx || fx >= MU_NFX) fx = mu.last_fx;
        mu.pick = PK_FX; mu.pick_n = MU_NFXL; mu.pick_cur = (uint8_t)mu_fx_index(fx);
        mu.pick_w = 4; mu.pick_cols = MU_FXP_COLS;
        return;
    } else if (mu.page == MU_INST && mu.inum && mu.ichar) {   /* a character of the name: the characters */
        uint8_t ch = mu_inst()->name[mu.ichar - 1u];
        mu.pick = PK_CHAR; mu.pick_n = (uint8_t)MU_NCHARSET; mu.pick_cur = 1;   /* (none yet: A) */
        for (i = 0; i < MU_NCHARSET; i++) if ((uint8_t)MU_CHARSET[i] == ch) mu.pick_cur = (uint8_t)i;
        mu.pick_w = 2; mu.pick_cols = 9;
        return;
    } else if (mu.page == MU_INST && (mu.inum || (mu_par(mu_par_row(), &mu_pick_par) && mu_pick_par.kind == MK_SAMPLE))) {
#if MASIF_CONTROLS == 0
        if (mu.inum) return;                        /* (EDIT + the arrows there are another instrument) */
#endif
        mu_pool_open();
        return;
    } else if (mu.page == MU_INST || mu.page == MU_MOD) {
        if (!mu_par(mu_par_row(), &mu_pick_par) || mu_pick_par.kind != MK_ENUM) return;
        mu.pick = PK_PAR; mu.pick_n = (uint8_t)(mu_pick_par.hi + 1);
        mu.pick_cur = (uint8_t)((uint32_t)mu_par_value(&mu_pick_par) % mu.pick_n);
    } else if (mu.page == MU_SAMPLER) {             /* the sample's name: the characters (no pool here) */
        if (!mu_smp_on_char()) return;
        mu.pick = PK_CHAR; mu.pick_n = (uint8_t)MU_NCHARSET; mu.pick_cur = (uint8_t)ms_char_now();
        mu.pick_w = 2; mu.pick_cols = 9;
        return;
    } else if (mu.page == MU_PROJECT && mu.prow == MJ_SCALE) {
        mu.pick = PK_SCALE; mu.pick_n = MASIF_SCALES; mu.pick_cur = (uint8_t)(masif_song.scale % MASIF_SCALES);
    } else return;
    for (i = 0; i < mu.pick_n; i++) {               /* as many columns as the longest name lets in (3 at the most) */
        const char *s = mu_pick_name(i);
        uint32_t n = 0;
        while (s[n]) n++;
        if (n > w) w = n;
    }
    mu.pick_w = (uint8_t)(w + 1u);
    mu.pick_cols = (uint8_t)mu_clampi((int32_t)(19u / (w + 1u)), 1, 3);
}
static void mu_pick_close(void)
{
    uint32_t kind = mu.pick, row, col, fxc;
    uint8_t d[7] = {0, 0, 0, 0, 0, 0, 0};
    mu.pick = PK_NONE;
    if (kind == PK_POOL) {
        mu_pool_close();
    } else if (kind == PK_FX) {
        uint32_t fx = mu_pick_fx(mu.pick_cur);
        mu_cur(&row, &col);
        fxc = mu_fx0(mu.page) + ((col - mu_fx0(mu.page)) & ~1u);
        mu_row_get(mu.page, row, d);
        d[fxc] = (uint8_t)fx;
        if (fx) mu.last_fx = (uint8_t)fx;
        else d[fxc + 1u] = 0;                        /* (none: its parameter goes too) */
        mu_row_post(mu.page, row, d);
    } else if (kind == PK_CHAR && mu.page == MU_SAMPLER) {
        mu_smp_char(mu.pick_cur);
    } else if (kind == PK_CHAR) {                    /* the character in, and the cursor on to the next */
        mu_name_set(mu.ichar - 1u, (uint8_t)MU_CHARSET[mu.pick_cur % MU_NCHARSET]);
        if (mu.ichar < MASIF_NAME_LEN) mu.ichar++;
    } else if (kind == PK_PAR) {
        mu_par_set(&mu_pick_par, mu.pick_cur);
    } else if (kind == PK_SCALE) {
        d[0] = mu.pick_cur;
        mu_edit(MASIF_EDIT_MIX, MASIF_TRACKS + 5u, 0, d);
    }
}
static void mu_draw_pick(void)
{
    const uint8_t box = MU_INK | MU_ON(MU_DIM);
    uint32_t rows = (mu.pick_n + mu.pick_cols - 1u) / mu.pick_cols, i;
    uint32_t r0 = mu.page == MU_INST || mu.page == MU_MOD || mu.page == MU_SAMPLER ? 6u : (MU_ROWS - rows - 1u) / 2u;   /* (under the picture) */
    if (mu.pick == PK_POOL) { mu_draw_pool(); return; }
    if (mu.pick == PK_FX) {
        uint32_t fx = mu_pick_fx(mu.pick_cur);
        mu_fill(0, 0, MU_COLS, MU_ROWS, box);
        mu_put(0, 0, MU_FX[fx], MU_BG | MU_ON(MU_INK));
        mu_put(4, 0, MU_FX_LONG[fx], MU_MID | MU_ON(MU_DIM));
        for (i = 0; i < mu.pick_n; i++)
            mu_put((i % MU_FXP_COLS) * 4u, MU_FXP_LIST + i / MU_FXP_COLS, MU_FX[mu_pick_fx(i)], i == mu.pick_cur ? MU_CURSOR : box);
        for (i = 0; i < 3u; i++)                    /* (the help in the title's shade, to tell it from the list) */
            mu_put(0, MU_FXP_HELP + i, MU_FX_HELP[fx][i], MU_MID | MU_ON(MU_DIM));
        return;
    }
    mu_fill(0, r0, MU_COLS, r0 + rows + 1u, box);
    mu_put(1, r0, mu.pick == PK_PAR ? mu_pick_par.label : mu.pick == PK_CHAR ? "NAME" : "SCALE",
           MU_MID | MU_ON(MU_DIM));
    for (i = 0; i < mu.pick_n; i++)
        mu_put(1u + (i % mu.pick_cols) * mu.pick_w, r0 + 1u + i / mu.pick_cols, mu_pick_name(i), i == mu.pick_cur ? MU_CURSOR : box);
}

/* ---- the sampler page (the rack sampler, below the instrument page; laid out as the instrument page after the user
 * found msfo's boxes clunky, 2026-10-23): the recorder of the pool the FM-1 keeps itself (masif_rec.c). Its sample
 * is the take just made (none at the start, none after OPTION + EDIT: the sampler is cleared, the sample stays in
 * the pool; deleting is the pool list's, on the instrument page). Its picture, then rows: SAMPLE (its number, RIGHT
 * along its name: EDIT held the characters, kept when the cursor leaves the row or the page), and the recorder's
 * settings (EDIT + arrows). REC (the button: PLAY's function) arms a take, again stops it; the IN meter shows the
 * source while the page is open. The note keys hear the sample at their pitch. SHIFT + PLAY still plays the song:
 * with START PLAY that is the bounce. */
enum { MS_SAMPLE, MS_SRC, MS_START, MS_LEVEL, MS_RATE, MS_BITS, MS_MAX, MS_NROW };
static const uint8_t MS_ROW_AT[MS_NROW] = {7, 8, 9, 10, 11, 12, 13};   /* (the screen's rows) */
#define MS_NONE 0xFFu
static struct {
    uint8_t slot;                                  /* the sampler's sample (MS_NONE: none) */
    uint8_t row, schar;                            /* the cursor's row; on SAMPLE 0 the number, 1-8 the name */
    char name[8];                                  /* the name as edited, not kept yet (dirty) */
    uint8_t dirty, name_slot;
    uint32_t name_at;                              /* the last change of it */
    uint8_t src, start, thr, sh, bits, max_s;      /* the recorder's settings (bits: MR_B*) */
    uint8_t seen;                                  /* the recorder's state the page saw last, */
    uint32_t seen_at;                              /* and since when */
    uint32_t meter;                                /* the level shown (falls back slowly) */
} ms;
static masif_instrument_t mu_smp_in;
static int mu_smp_has(uint32_t k) { return k < SP_NUSER && sp_wave[KIT_NWAVE + k].n != 0u; }
static uint32_t mu_smp_count(void) { uint32_t k, n = 0; for (k = 0; k < SP_NUSER; k++) n += (uint32_t)mu_smp_has(k); return n; }
/* the name as edited into the pool's directory (one write: when the cursor leaves the row, the page, or after 5 s) */
static void mu_smp_name_keep(void)
{
    char nm[8];
    uint32_t i, end = 8;
    if (!ms.dirty) return;
    ms.dirty = 0;
    if (!mu_smp_has(ms.name_slot) || mr_busy()) return;
    while (end && !ms.name[end - 1u]) end--;        /* (a gap inside the name: a space; none at its end) */
    for (i = 0; i < 8u; i++) nm[i] = i < end ? (ms.name[i] ? ms.name[i] : ' ') : 0;
    mu_say(mr_rename(ms.name_slot, nm) ? "NOT RENAMED" : "RENAMED");
}
static void mu_smp_name_load(void)                  /* the name: the sample's */
{
    const char *nm = MU_WAVE_NAME(KIT_NWAVE + ms.slot % SP_NUSER);
    uint32_t i;
    for (i = 0; i < 8u; i++) ms.name[i] = 0;
    if (mu_smp_has(ms.slot)) for (i = 0; i < 8u && nm[i]; i++) ms.name[i] = nm[i];
    ms.name_slot = ms.slot;
}
static void mu_smp_fix(void)                        /* (a sample gone from the pool meanwhile: none) */
{
    if (ms.slot != MS_NONE && !mu_smp_has(ms.slot)) { ms.slot = MS_NONE; ms.dirty = 0; mu.wave_drawn = 0; }
    if (!ms.dirty && ms.name_slot != ms.slot) mu_smp_name_load();
}
static void mu_smp_mark(uint32_t k)                 /* the sampler's sample now k (the name kept first) */
{
    if (k == ms.slot) return;
    mu_smp_name_keep();
    ms.slot = (uint8_t)k;
    mu_smp_name_load();
    mu.wave_drawn = 0;
}
/* the sample as an instrument would begin with it (for the picture: its own loop and tapers) */
static const masif_instrument_t *mu_smp_inst(void)
{
    const sp_wave_t *w = &sp_wave[KIT_NWAVE + ms.slot % SP_NUSER];
    masif_instrument_default(&mu_smp_in, 0);
    mu_smp_in.sample = mu_smp_has(ms.slot) ? (uint8_t)(KIT_NWAVE + ms.slot) : (uint8_t)MASIF_SAMPLES;
    if (mu_smp_has(ms.slot) && w->lp_b) {
        mu_smp_in.play = w->lp_play >= MASIF_PLAY_LOOP ? w->lp_play : MASIF_PLAY_LOOP;
        mu_smp_in.flags |= MASIF_INST_SMPLOOP;
        mu_smp_in.taper_in = w->lp_tin; mu_smp_in.taper_out = w->lp_tout;
    }
    return &mu_smp_in;
}
static uint32_t mu_cs(const sp_wave_t *w)           /* a sample's length as it plays, 1/100 s */
{
    uint32_t rate = w->rate;
    if (w->uflags & 1u) rate = rate * 33u / 45u;    /* (stored for 45->33, or x2) */
    if (w->uflags & 2u) rate /= 2u;
    return rate ? (uint32_t)((uint64_t)w->n * 100u / rate) : 0u;
}
static void mu_secs(uint32_t col, uint32_t row, uint32_t cs, uint8_t at)   /* 9.99 or 99.9 */
{
    if (cs >= 1000u) { mu_dec(col, row, cs / 100u > 99u ? 99u : cs / 100u, 2, at); mu_put(col + 2u, row, ".", at); mu_dec(col + 3u, row, cs / 10u % 10u, 1, at); }
    else { mu_dec(col, row, cs / 100u, 1, at); mu_put(col + 1u, row, ".", at); mu_dec(col + 2u, row, cs % 100u, 2, at); }
}
static const char *const MU_SRC_NAME[2] = {"USB", "MIX"};
static const char *const MU_START_NAME[3] = {"LEVEL", "PLAY", "NOW"};
static const char *const MU_RATE_NAME[3] = {"44K", "22K", "11K"};
static const char *const MU_WHY_NAME[] = {"", "FULL", "NO ROOM", "FLASH ERROR", "TOO SLOW", "POOL ERROR", "NOTHING CAME"};
static void mu_rec_go(void)                         /* REC: arm; armed: not; a take running: it ends, kept */
{
    uint32_t st = mr_state_now(), rc;
    if (st == MR_ARMED || st == MR_TAKE) { mr_stop(); return; }
    if (st == MR_SAVING) return;
    mu_smp_name_keep();
    rc = (uint32_t)mr_arm(ms.src, ms.start, ms.thr, ms.sh, ms.bits, ms.max_s);
    if (rc) mu_say(MU_WHY_NAME[rc < sizeof MU_WHY_NAME / sizeof MU_WHY_NAME[0] ? rc : 0]);
    ms.seen = (uint8_t)mr_state_now();
}
static int mu_smp_on_char(void) { return ms.row == MS_SAMPLE && ms.schar && mu_smp_has(ms.slot); }
static uint32_t ms_char_now(void)                  /* the name's character under the cursor: its place in MU_CHARSET */
{
    uint32_t i;
    char c = ms.schar ? ms.name[(ms.schar - 1u) & 7u] : 0;
    for (i = 0; i < MU_NCHARSET; i++) if (MU_CHARSET[i] == (c ? c : ' ')) return i;
    return 0;
}
static void mu_smp_set_char(uint32_t c)
{
    ms.name[(ms.schar - 1u) & 7u] = MU_CHARSET[c % MU_NCHARSET] == ' ' ? 0 : MU_CHARSET[c % MU_NCHARSET];
    ms.dirty = 1; ms.name_slot = ms.slot; ms.name_at = fm1_ms;
}
static void mu_smp_char(uint32_t c)                /* from the characters' list: in, and the cursor on */
{
    if (!mu_smp_on_char()) return;
    mu_smp_set_char(c);
    if (ms.schar < 8u) ms.schar++;
}
/* EDIT + arrows on the page: the cursor's row by d (on SAMPLE: the name's character) */
static void mu_smp_value(int32_t d)
{
    if (ms.row != MS_SAMPLE && mr_busy()) return;   /* (as they are while a take is on its way: shown dim, no message */
                                                    /* over the take's time) */
    switch (ms.row) {
    case MS_SAMPLE:
        if (mu_smp_on_char())
            mu_smp_set_char((uint32_t)(((int32_t)ms_char_now() + d % (int32_t)MU_NCHARSET + (int32_t)MU_NCHARSET) % (int32_t)MU_NCHARSET));
        break;
    case MS_SRC: ms.src = (uint8_t)mu_clampi((int32_t)ms.src + d, 0, 1); break;
    case MS_START: ms.start = (uint8_t)mu_clampi((int32_t)ms.start + d, 0, 2); break;
    case MS_LEVEL: ms.thr = (uint8_t)mu_clampi((int32_t)ms.thr - d, 0, MR_NTHR - 1); break;   /* (up: louder) */
    case MS_RATE: ms.sh = (uint8_t)mu_clampi((int32_t)ms.sh - d, 0, 2); break;
    case MS_BITS: ms.bits = (uint8_t)mu_clampi((int32_t)ms.bits - d, MR_B12, MR_B4); break;   /* (up: more bits) */
    default: ms.max_s = (uint8_t)mu_clampi((int32_t)ms.max_s + d, 1, 60); break;
    }
}
static void mu_smp_cursor(int32_t dx, int32_t dy)
{
    if (dy) {
        ms.row = (uint8_t)mu_clampi((int32_t)ms.row + dy, 0, MS_NROW - 1);
        if (ms.row != MS_SAMPLE) { ms.schar = 0; mu_smp_name_keep(); }
    }
    if (dx && ms.row == MS_SAMPLE) ms.schar = (uint8_t)mu_clampi((int32_t)ms.schar + dx, 0, mu_smp_has(ms.slot) ? 8 : 0);
}
/* OPTION + EDIT on SAMPLE: on its number the sampler cleared (the sample stays in the pool), on the name the
 * character */
static void mu_smp_delete(void)
{
    if (ms.row != MS_SAMPLE || !mu_smp_has(ms.slot)) return;
    if (ms.schar) { ms.name[(ms.schar - 1u) & 7u] = 0; ms.dirty = 1; ms.name_slot = ms.slot; ms.name_at = fm1_ms; return; }
    mu_smp_name_keep();
    masif_fm1_preview(mu.track, 0, 0, 0);
    ms.slot = MS_NONE;
    mu_smp_name_load();
    mu.wave_drawn = 0;
    mu_say("CLEARED: IN THE POOL");
}
/* a note key on the page: the sample at that pitch (as the pool would give it to an instrument) */
static void mu_smp_hear(uint8_t note)
{
    if (!mu_smp_has(ms.slot) || masif_preview.pending) return;
    masif_instrument_default(&masif_audition, (uint8_t)(KIT_NWAVE + ms.slot));
    masif_audition.sample = (uint8_t)(KIT_NWAVE + ms.slot);
    masif_fm1_preview(mu.track, MASIF_AUDITION, note, 127);
}
static void mu_draw_sampler(void)
{
    static const char *const LABEL[MS_NROW] = {"SAMPLE", "SOURCE", "START", "LEVEL", "RATE", "BITS", "MAX"};
    uint32_t n, i, st = mr_state_now(), lvl, free, longest, room;
    const sp_wave_t *w = &sp_wave[KIT_NWAVE + ms.slot % SP_NUSER];
    const int busy = mr_busy(), has = mu_smp_has(ms.slot);
    mu_title("SAMPLER");
    if (st == MR_SAVED && ms.seen != MR_SAVED) mu_smp_mark(mr_slot_now());   /* (a new take: the sampler's) */
    if (st != ms.seen) ms.seen_at = fm1_ms;
    ms.seen = (uint8_t)st;
    if ((st == MR_SAVED && (uint32_t)(fm1_ms - ms.seen_at) > 4000u) || (st == MR_FAILED && (uint32_t)(fm1_ms - ms.seen_at) > 10000u))
        st = MR_IDLE;                               /* (how it went: a while, then the row's hint again) */
    mu_smp_fix();
    if (has) {                                      /* under the picture: rate, bits, length */
        mu_dec(0, MU_WAVE_R1, (w->rate + 500u) / 1000u, 2, MU_MID);
        mu_put(2, MU_WAVE_R1, "KHZ", MU_MID);
        mu_put(7, MU_WAVE_R1, w->uflags & 8u ? " 4 BIT" : w->uflags & 4u ? " 8 BIT" : "12 BIT", MU_MID);
        if (w->lp_b) mu_put(14, MU_WAVE_R1, "L", MU_MID);   /* (its own loop) */
        mu_secs(15, MU_WAVE_R1, mu_cs(w), MU_MID);
        mu_put(19, MU_WAVE_R1, "S", MU_MID);
    }
    for (i = 0; i < MS_NROW; i++) mu_put(0, MS_ROW_AT[i], LABEL[i], MU_MID);
    if (has) {                                      /* its number, then its name a character a cell ('.': none, while */
        mu_hex(7, MS_ROW_AT[MS_SAMPLE], KIT_NWAVE + ms.slot, 2, ms.row == MS_SAMPLE && !ms.schar ? MU_CURSOR : MU_INK);
        for (i = 0; i < 8u; i++) {                  /* the cursor is on the row) */
            char c[2] = {ms.name[i] ? ms.name[i] : ms.row == MS_SAMPLE ? '.' : ' ', 0};
            mu_put(10u + i, MS_ROW_AT[MS_SAMPLE], c, ms.row == MS_SAMPLE && ms.schar == i + 1u ? MU_CURSOR : ms.name[i] ? MU_INK : MU_DIM);
        }
    } else {
        mu_put(7, MS_ROW_AT[MS_SAMPLE], "--", ms.row == MS_SAMPLE ? MU_CURSOR : MU_INK);
        mu_put(10, MS_ROW_AT[MS_SAMPLE], "NONE", MU_DIM);
    }
#define MS_VAT(r) ((uint8_t)(ms.row == (r) ? MU_CURSOR : busy ? MU_MID : MU_INK))
    mu_put(7, MS_ROW_AT[MS_SRC], MU_SRC_NAME[ms.src & 1u], MS_VAT(MS_SRC));
    mu_put(7, MS_ROW_AT[MS_START], MU_START_NAME[ms.start % 3u], MS_VAT(MS_START));
    mu_put(7, MS_ROW_AT[MS_LEVEL], "-", MS_VAT(MS_LEVEL));
    mu_dec(8, MS_ROW_AT[MS_LEVEL], 6u * (ms.thr + 1u), 2, MS_VAT(MS_LEVEL));
    mu_put(10, MS_ROW_AT[MS_LEVEL], "DB", MS_VAT(MS_LEVEL));
    mu_put(7, MS_ROW_AT[MS_RATE], MU_RATE_NAME[ms.sh % 3u], MS_VAT(MS_RATE));
    mu_put(7, MS_ROW_AT[MS_BITS], ms.bits == MR_B4 ? "4 ADPCM" : ms.bits == MR_B8 ? "8" : "12", MS_VAT(MS_BITS));
    mu_dec(7, MS_ROW_AT[MS_MAX], ms.max_s, 2, MS_VAT(MS_MAX));
    mu_put(9, MS_ROW_AT[MS_MAX], "S", MS_VAT(MS_MAX));
    room = mr_room_ms(ms.sh, ms.bits) / 100u;      /* (what one take can be: the ring and the longest free run) */
    mu_put(12, MS_ROW_AT[MS_MAX], "(", MU_DIM);
    mu_secs(13, MS_ROW_AT[MS_MAX], room * 10u, MU_DIM);
    mu_put(17, MS_ROW_AT[MS_MAX], "S)", MU_DIM);
#undef MS_VAT
    n = mu_smp_count();                             /* the pool: its samples, its room (as the web app counts it: */
    free = mr_free(0, &longest);                    /* 12 bit at 26.04 kHz) */
    mu_put(0, 15, "POOL", MU_DIM);
    mu_dec(7, 15, n, 2, MU_DIM); mu_put(9, 15, "/", MU_DIM); mu_dec(10, 15, SP_NUSER, 2, MU_DIM);
    mu_secs(13, 15, free / 39060u * 100u + free % 39060u * 100u / 39060u, MU_DIM);
    mu_put(17, 15, "S", MU_DIM);
    lvl = mr_level();                               /* the meter: a cell each 6 dB from -60, the level's marked */
    ms.meter = lvl > ms.meter ? lvl : ms.meter - (ms.meter >> 3);
    mu_put(0, 16, "IN", MU_MID);
    for (i = 0; i < MR_NTHR; i++) {
        uint32_t k = MR_NTHR - 1u - i;
        mu_put(3u + i, 16, ms.meter > MR_THR[k] ? "#" : "-", ms.start == MR_LEVEL && k == ms.thr ? MU_CURSOR : MU_INK);
    }
    if (ms.meter >= 32000u) mu_put(14, 16, "CLIP", MU_INK);
    switch (st) {                                   /* the recorder's state on the bottom row (a message goes over it) */
    case MR_ARMED:
        mu_put(0, 17, ms.start == MR_LEVEL ? "WAITING FOR SOUND" : ms.start == MR_PLAY ? "WAITING FOR PLAY" : "ARMED", MU_INK);
        break;
    case MR_TAKE:
        mu_put(0, 17, "REC", MU_CURSOR);
        mu_secs(7, 17, mr_take_ms() / 10u, MU_INK); mu_put(12, 17, "OF", MU_MID); mu_secs(15, 17, mr_cap_ms() / 10u, MU_INK);
        mu_put(19, 17, "S", MU_MID);
        break;
    case MR_SAVING:                                 /* (the sound is quiet while it erases: the first half) */
        mu_put(0, 17, "SAVING", MU_CURSOR); mu_dec(7, 17, mr_save_pct(), 3, MU_INK); mu_put(10, 17, "%", MU_INK);
        mu_secs(13, 17, mr_take_ms() / 10u, MU_MID); mu_put(18, 17, "S", MU_MID);
        break;
    case MR_SAVED:
        mu_put(0, 17, "SAVED AS", MU_MID); mu_put(9, 17, mr_name_now(), MU_INK);
        break;
    case MR_FAILED:
        mu_put(0, 17, "FAILED:", MU_MID);
        mu_put(8, 17, MU_WHY_NAME[mr_why_now() < sizeof MU_WHY_NAME / sizeof MU_WHY_NAME[0] ? mr_why_now() : 0], MU_INK);
        break;
    default:
#if MASIF_CONTROLS == 0                             /* (what the cursor's OPTION + EDIT / EDIT does; else REC) */
        mu_put(0, 17, ms.row != MS_SAMPLE || !has ? "REC: RECORD" : ms.schar ? "EDIT: LETTERS" : "OPT+EDIT: CLEAR", MU_DIM);
#endif
        break;
    }
}

static void mu_draw_knobs(void);
static void mu_knob_row(uint32_t i, uint32_t r, uint8_t at);
static void masif_ui_draw(void)
{
    mu_clear();
    switch (mu.page) {
    case MU_SONG: mu_draw_song(); break;
    case MU_CHAIN: mu_draw_chain(); break;
    case MU_PHRASE: mu_draw_phrase(); break;
    case MU_GROOVE: mu_draw_groove(); break;
    case MU_INST: mu_draw_inst(); break;
    case MU_TABLE: mu_draw_table(); break;
    case MU_PROJECT: mu_draw_project(); break;
    case MU_MOD: mu_draw_mod(); break;
    case MU_SAMPLER: mu_draw_sampler(); break;
    default: mu_draw_mixer(); break;
    }
    mu_draw_sel();
    if (mu.pick) mu_draw_pick();
    else if (mu.bank_held) mu_draw_knobs();
    if (mu.msg && (int32_t)(mu.msg_until - fm1_ms) > 0) {
        if (mu.msg_knob) mu_knob_row(mu.msg_knob - 1u, MU_ROWS - 1u, MU_CURSOR);   /* (a knob's: its value as it is now) */
        else {                                      /* (the sampler's and the pool's hint row: not left showing after it; */
            if (mu.page == MU_SAMPLER || mu.pick == PK_POOL) mu_fill(0, MU_ROWS - 1u, MU_COLS, MU_ROWS, MU_INK);   /* a grid's */
            mu_put(0, MU_ROWS - 1u, mu.msg, MU_CURSOR);   /* last row stays) */
        }
    } else { mu.msg = 0; mu.msg_knob = 0; }
    mu_flush();
    if (mu_pictured()) mu_draw_graph();
}

/* ---- moving */
static void mu_goto(uint32_t page)
{
    if (page == mu.page) return;
    if (mu.page == MU_SAMPLER) mu_smp_name_keep();
    mr_listen(page == MU_SAMPLER);                  /* (the IN meter: only while it is shown) */
    mu.back = mu.page;
    mu.page = (uint8_t)page;
    mu.wave_drawn = 0;
    mu.sel = 0;
    mu.pick = PK_NONE;
}

/* to the right along S C P I T, round: the page opens what the cursor is on */
static void mu_page_right(void)
{
    switch (mu.page) {
    case MU_SONG: {
        uint32_t c = masif_song.song[mu.song_row][mu.track];
        if (c < MASIF_CHAINS) mu.chain = (uint8_t)c;
        mu_goto(MU_CHAIN);
        break;
    }
    case MU_CHAIN: {
        uint32_t p = masif_song.chain[mu.chain % MASIF_CHAINS][mu.step].phrase;
        if (p < MASIF_PHRASES) mu.phrase = (uint8_t)p;
        mu_goto(MU_PHRASE);
        break;
    }
    case MU_PHRASE: {                                /* the row's instrument; a row without one: the first empty */
        uint32_t i = masif_song.phrase[mu.phrase % MASIF_PHRASES][mu.row].instrument;
        int32_t e = mu_next_empty();
        if (i < MASIF_INSTRUMENTS) mu.inst = (uint8_t)i;
        else if (e >= 0) mu.inst = (uint8_t)e;
        mu_goto(MU_INST);
        break;
    }
    case MU_INST: case MU_MOD: case MU_SAMPLER: mu_goto(MU_TABLE); break;
    case MU_TABLE: case MU_GROOVE: mu_goto(MU_SONG); break;
    default: mu_goto(MU_CHAIN); break;              /* (from Project or Mixer, which stand at S: on along the row) */
    }
}
/* to the left, round: back to the page as it was left */
static void mu_page_left(void)
{
    switch (mu.page) {
    case MU_CHAIN: mu_goto(MU_SONG); break;
    case MU_PHRASE: mu_goto(MU_CHAIN); break;
    case MU_INST: case MU_MOD: case MU_SAMPLER: mu_goto(MU_PHRASE); break;
    case MU_TABLE: case MU_GROOVE: mu_goto(MU_INST); break;
    default: mu_goto(MU_TABLE); break;              /* (from Song, and from Project or Mixer at S) */
    }
}
/* up and down off the row, round: at S Project / Song / Mixer, at I Modulation / Instrument / Sampler, at T Groove /
 * Table. dir > 0: down */
static void mu_page_vert(int32_t dir)
{
    static const uint8_t S[3] = {MU_PROJECT, MU_SONG, MU_MIXER}, I[3] = {MU_MOD, MU_INST, MU_SAMPLER};
    uint32_t i;
    for (i = 0; i < 3u; i++)
        if (S[i] == mu.page) { mu_goto(S[(i + (dir > 0 ? 1u : 2u)) % 3u]); return; }
    for (i = 0; i < 3u; i++)
        if (I[i] == mu.page) { mu_goto(I[(i + (dir > 0 ? 1u : 2u)) % 3u]); return; }
    if (mu.page == MU_TABLE) mu_goto(MU_GROOVE);
    else if (mu.page == MU_GROOVE) mu_goto(MU_TABLE);
}

static void mu_cursor(int32_t dx, int32_t dy)
{
    switch (mu.page) {
    case MU_SONG: {
        int32_t lo = 0, hi = MASIF_SONG_ROWS - 1;
        if (mu.sel) {                                /* (a selection: 16 rows at the most) */
            lo = mu_clampi((int32_t)mu.sel_row - 15, 0, hi);
            hi = mu_clampi((int32_t)mu.sel_row + 15, 0, hi);
        }
        mu.track = (uint8_t)mu_clampi((int32_t)mu.track + dx, 0, MASIF_TRACKS - 1);
        mu.song_row = (uint8_t)mu_clampi((int32_t)mu.song_row + dy, lo, hi);
        if (mu.song_row < mu.song_top) mu.song_top = mu.song_row;
        if (mu.song_row > mu.song_top + 15u) mu.song_top = (uint8_t)(mu.song_row - 15u);
        break;
    }
    case MU_CHAIN:
        mu.ccol = (uint8_t)mu_clampi((int32_t)mu.ccol + dx, 0, 1);
        mu.step = (uint8_t)mu_clampi((int32_t)mu.step + dy, 0, MASIF_CHAIN_STEPS - 1);
        break;
    case MU_PHRASE:
        mu.pcol = (uint8_t)mu_clampi((int32_t)mu.pcol + dx, 0, MP_NCOL - 1);
        mu.row = (uint8_t)mu_clampi((int32_t)mu.row + dy, 0, MASIF_ROWS - 1);
        break;
    case MU_TABLE:
        mu.tcol = (uint8_t)mu_clampi((int32_t)mu.tcol + dx, 0, MT_NCOL - 1);
        mu.trow = (uint8_t)mu_clampi((int32_t)mu.trow + dy, 0, MASIF_TABLE_ROWS - 1);
        break;
    case MU_GROOVE:                                  /* up from the first step: onto the groove's number */
        if (mu.gnum && dy > 0) { mu.gnum = 0; dy--; }
        else if (!mu.gnum && !mu.sel && dy < 0 && (int32_t)mu.grow + dy < 0) { mu.gnum = 1; mu.grow = 0; }
        if (!mu.gnum) mu.grow = (uint8_t)mu_clampi((int32_t)mu.grow + dy, 0, MASIF_GROOVE_STEPS - 1);
        break;
    case MU_INST: case MU_MOD: {                     /* left / right: the tabs (the slots); up / down: the settings, */
        uint8_t *row = mu.page == MU_INST ? &mu.irow : &mu.mrow;   /* and above the first the instrument's number */
        if (dx && mu.page == MU_INST && mu.inum) mu.ichar = (uint8_t)mu_clampi((int32_t)mu.ichar + dx, 0, MASIF_NAME_LEN);   /* (the name) */
        else if (dx && mu.page == MU_INST) { mu.itab = (uint8_t)mu_clampi((int32_t)mu.itab + dx, 0, MI_NTAB - 1); mu.irow = 0; }
        if (dx && mu.page == MU_MOD) mu.mslot = (uint8_t)mu_clampi((int32_t)mu.mslot + dx, 0, MASIF_MODS - 1);
        if (mu.inum && dy > 0) { mu.inum = 0; mu.ichar = 0; dy--; }
        else if (!mu.inum && dy < 0 && (int32_t)*row + dy < 0) { mu.inum = 1; mu.ichar = 0; *row = 0; }
        *row = (uint8_t)mu_clampi((int32_t)*row + (mu.inum ? 0 : dy), 0, (int32_t)mu_par_count() - 1);
        break;
    }
    case MU_MIXER:
        mu.mcol = (uint8_t)mu_clampi((int32_t)mu.mcol + dx + dy, 0, 6);
        break;
    case MU_SAMPLER:
        mu_smp_cursor(dx, dy);
        break;
    case MU_PROJECT:                                 /* on SONG: right along the name */
        if (dx && mu.prow == MJ_SLOT) mu.schar = (uint8_t)mu_clampi((int32_t)mu.schar + dx, 0, 8);
        if (dy) mu.schar = 0;
        mu.prow = (uint8_t)mu_clampi((int32_t)mu.prow + dy, 0, MJ_N - 1);
        break;
    default:
        break;
    }
}

/* ---- changing what the cursor is on: by `small` steps of one, `big` steps of an octave / 0x10. An empty cell
 * begins with what was entered last */
/* an effect cell turned: through the effects by their letters, as the list has them, round; an empty one begins
 * with the effect entered last */
static uint8_t mu_fx_step(uint8_t fx, int32_t turn)
{
    int32_t n = (int32_t)MU_NFXL, i = (int32_t)mu_fx_index(fx ? fx : mu.last_fx) + turn - (fx ? 0 : turn > 0 ? 1 : -1);
    uint8_t f = MU_FX_ORDER[((i % n) + n) % n];
    if (f) mu.last_fx = f;
    return f;
}
/* a number that may be empty (a chain, a phrase, an instrument, a volume) turned. fill: an empty one begins with
 * `last`; else it stays empty */
static void mu_num_turn(uint8_t *v, uint8_t *last, int32_t step, int32_t top, int fill)
{
    if (*v <= top) *v = (uint8_t)mu_clampi((int32_t)*v + step, 0, top);
    else if (fill) *v = *last;
    else return;
    *last = *v;
}
/* one column of a grid page's row (as mu_row_get gives it) turned. fill 0, as it is for every cell of a
 * selection: what is empty stays empty and the effects stay what they are (their parameters turn) */
static void mu_col_turn(uint32_t page, uint32_t col, uint8_t *d, int32_t small, int32_t big, int fill)
{
    int32_t step = small + big * 16, semis = small + big * 12;
    uint32_t fx0 = mu_fx0(page);
    if (col >= fx0) {
        if (!((col - fx0) & 1u)) { if (fill) d[col] = mu_fx_step(d[col], small + big); }
        else if (fill || d[col - 1u]) d[col] = (uint8_t)mu_clampi((int32_t)d[col] + step, 0, 255);
        return;
    }
    switch (page) {
    case MU_SONG: mu_num_turn(&d[col], &mu.last_chain, step, MASIF_CHAINS - 1, fill); break;
    case MU_CHAIN:
        if (col == 0u) mu_num_turn(&d[0], &mu.last_phrase, step, MASIF_PHRASES - 1, fill);
        else d[1] = (uint8_t)mu_clampi((int32_t)(int8_t)d[1] + semis, -48, 48);
        break;
    case MU_PHRASE:
        if (col == MP_NOTE) {
            if (d[0] >= 1u && d[0] <= 120u) d[0] = (uint8_t)mu_clampi((int32_t)d[0] + semis, 1, 120);
            else if (fill) {                         /* a new note: with the instrument entered last */
                d[0] = mu.last_note;
                if (d[1] >= MASIF_INSTRUMENTS) d[1] = mu.last_inst;
            } else break;
            mu.last_note = d[0];
        } else if (col == MP_INST) mu_num_turn(&d[1], &mu.last_inst, step, MASIF_INSTRUMENTS - 1, fill);
        else mu_num_turn(&d[2], &mu.last_vol, step, 127, fill);
        break;
    case MU_GROOVE: { uint8_t last = 6; mu_num_turn(&d[0], &last, step, MASIF_GROOVE_MAX, fill); break; }
    default:
        if (col == MT_N) d[0] = (uint8_t)mu_clampi((int32_t)(int8_t)d[0] + semis, -48, 48);
        else { uint8_t last = mu.last_vol; mu_num_turn(&d[1], &last, step, 127, fill); }
        break;
    }
}

static void mu_value(int32_t small, int32_t big)
{
    uint8_t d[7] = {0, 0, 0, 0, 0, 0, 0};
    if (!small && !big) return;
    if (mu.page == MU_SAMPLER) { mu_smp_value(small + big); return; }
    if (mu.page == MU_GROOVE && mu.gnum) {           /* another groove on the page */
        mu.groove = (uint8_t)mu_clampi((int32_t)mu.groove + small + big, 0, MASIF_GROOVES - 1);
        return;
    }
    if (mu_is_grid(mu.page)) {                       /* the cursor's cell, or every cell of the selection */
        uint32_t r0, r1, c0, c1, r, c;
        int sel = mu_rect(&r0, &r1, &c0, &c1, 0);
        for (r = r0; r <= r1; r++) {
            mu_row_get(mu.page, r, d);
            for (c = c0; c <= c1; c++) mu_col_turn(mu.page, c, d, small, big, !sel);
            mu_row_post(mu.page, r, d);
        }
        return;
    }
    switch (mu.page) {
    case MU_INST: case MU_MOD: {
        mu_par_t p;
        if (mu.inum && mu.ichar && mu.page == MU_INST) {   /* a character of the name, through the font's */
            mu_name_set(mu.ichar - 1u, (uint8_t)mu_clampi((int32_t)(mu_inst()->name[mu.ichar - 1u] ? mu_inst()->name[mu.ichar - 1u] : 'A' - small - big * 16) + small + big * 16, 0x20, 0x5F));
            break;
        }
        if (mu.inum) {                               /* another instrument on the page */
            mu.inst = (uint8_t)mu_clampi((int32_t)mu.inst + small + big * 16, 0, MASIF_INSTRUMENTS - 1);
            mu.sel = 0;
            break;
        }
        if (!mu_par(mu_par_row(), &p)) break;
        mu_par_set(&p, mu_par_value(&p) + small + big * (p.kind == MK_NOTE ? 12 : p.kind == MK_ENUM ? 1 : 16));
        break;
    }
    case MU_MIXER:
        if (mu.mcol < MASIF_TRACKS) d[0] = (uint8_t)mu_clampi((int32_t)masif_song.track_level[mu.mcol] + small + big * 16, 0, 127);
        else d[0] = (uint8_t)mu_clampi((int32_t)masif_song.master_eq[mu.mcol - MASIF_TRACKS] + small + big * 8, -64, 64);
        mu_edit(MASIF_EDIT_MIX, mu.mcol, 0, d);
        break;
    case MU_PROJECT:
        if (mu.prow == MJ_SLOT && mu.schar) {
            mu_sname_turn(mu.schar - 1u, small + big);
        } else if (mu.prow == MJ_SLOT) {
            mu.slot = (uint8_t)mu_clampi((int32_t)mu.slot + small + big, 0, 15);
        } else if (mu.prow == MJ_TEMPO) {
            uint32_t t = masif_song.tempo ? masif_song.tempo : sq.bpm10 / 10u;
            d[0] = (uint8_t)mu_clampi((int32_t)t + small + big * 10, 40, 240);
            mu_edit(MASIF_EDIT_MIX, MASIF_TRACKS + 3u, 0, d);
            sq.bpm10 = (uint16_t)(d[0] * 10u);       /* (heard at once: the machine's tempo is the song's) */
        } else if (mu.prow == MJ_KEY) {
            d[0] = (uint8_t)(((int32_t)masif_song.key + small + big + 120) % 12);
            mu_edit(MASIF_EDIT_MIX, MASIF_TRACKS + 4u, 0, d);
        } else if (mu.prow == MJ_SCALE) {
            d[0] = (uint8_t)(((int32_t)masif_song.scale + small + big + 16 * (int32_t)MASIF_SCALES) % (int32_t)MASIF_SCALES);
            mu_edit(MASIF_EDIT_MIX, MASIF_TRACKS + 5u, 0, d);
        } else if (mu.prow == MJ_GROOVE) {
            d[0] = (uint8_t)mu_clampi((int32_t)masif_song.project_groove + small + big, 0, MASIF_GROOVES - 1);
            mu_edit(MASIF_EDIT_MIX, MASIF_TRACKS + 6u, 0, d);
        } else if (mu.prow == MJ_COLOR) {
            mu.palette = (uint8_t)(((int32_t)mu.palette + small + big + 16 * (int32_t)MU_NPALETTE) % (int32_t)MU_NPALETTE);
            mu.all = 1;
        } else if (mu.prow == MJ_FONT) {
            mu.font = (uint8_t)((mu.font + (uint32_t)(small + big)) & 1u);
            mu.all = 1;
        }
        break;
    default:
        break;
    }
}

/* the song's name: character k becomes c (0: none); no holes: none before it become spaces, one gone in the middle
 * a space */
static void mu_sname_set(uint32_t k, char c)
{
    uint32_t i;
    if (k >= 8u) return;
    if (c) { for (i = 0; i < k; i++) if (!mu.sname[i]) mu.sname[i] = ' '; }
    else if (k + 1u < 8u && mu.sname[k + 1u]) c = ' ';
    mu.sname[k] = c;
}
/* EDIT + the arrows on a character: through A-Z, 0-9, space, round (from none: A one way, the space the other) */
static void mu_sname_turn(uint32_t k, int32_t d)
{
    static const char SET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
    const int32_t n = (int32_t)sizeof SET - 1;
    int32_t i;
    if (k >= 8u || !d) return;
    for (i = 0; i < n && SET[i] != mu.sname[k]; i++) {}
    if (i == n) i = d > 0 ? -1 : n;                 /* (none, or one not in the set) */
    mu_sname_set(k, SET[(((i + d) % n) + n) % n]);
}
static void mu_sname_from(uint8_t slot)             /* the name a slot's song was saved with ("WORK": none) */
{
#if MASIF_SONG_STORE
    masif_store_info_t info;
    uint32_t i;
    for (i = 0; i < 9u; i++) mu.sname[i] = 0;
    if (!masif_store_info(slot, &info) || !info.used || (info.name[0] == 'W' && info.name[1] == 'O' && info.name[2] == 'R' && info.name[3] == 'K' && !info.name[4])) return;
    for (i = 0; i < 8u && info.name[i]; i++) mu.sname[i] = info.name[i];
#else
    (void)slot;
#endif
}

/* the project page's TEST: a song that loads the audio as much as a song can (the user, 2026-10-24: is there crackle
 * under load?), in place of the one in memory (SAVE keeps it). Every track a note every row on its own looping
 * instrument, filtered (LP, BP, HP, LP: resonant, driven, EQ'd, sent to all three buses, two LFOs and an envelope on
 * it), RET 32nds on each beat and ARP between, VIB in the second column, the instrument's table running, the chain
 * transposed; the pool's first four samples (round if fewer). 0: loaded; -1: the pool is empty */
static int mu_load_test(void)
{
    static const int8_t tr[4] = {0, 5, -2, 3};      /* (the chain's four steps) */
    static const int8_t tt[8] = {0, 12, 7, 0, -12, 7, 12, 0};
    uint32_t n = mu_pool_count(), t, r;
    if (!n) return -1;
    masif_song_clear(&masif_song);
    masif_song.tempo = 160;
    masif_song.master_eq[0] = 16; masif_song.master_eq[1] = -8; masif_song.master_eq[2] = 16;
    for (t = 0; t < MASIF_TRACKS; t++) {
        masif_instrument_t *in = &masif_song.instrument[t];
        in->sample = (uint8_t)mu_pool_wave(t % n);
        in->play = MASIF_PLAY_LOOP;
        in->pan = (int8_t)(t & 1u ? 40 : -40);
        in->volume = 100; in->decay = 60; in->sustain = 90; in->release = 60;
        in->filter = (uint8_t)(t == 1u ? MASIF_FILTER_BP : t == 2u ? MASIF_FILTER_HP : MASIF_FILTER_LP);
        in->cutoff = 70; in->resonance = 90; in->drive = 40;
        in->eq[0] = 16; in->eq[1] = -16; in->eq[2] = 16;
        in->send[0] = in->send[1] = in->send[2] = 60;
        in->mod[0].type = MASIF_MOD_LFO; in->mod[0].dest = MASIF_DEST_CUTOFF; in->mod[0].amount = 40;
        in->mod[0].p[0] = MASIF_LFO_SIN; in->mod[0].p[2] = 60;
        in->mod[1].type = MASIF_MOD_LFO; in->mod[1].dest = MASIF_DEST_PITCH; in->mod[1].amount = 8;
        in->mod[1].p[0] = MASIF_LFO_TRI; in->mod[1].p[2] = 80;
        in->mod[2].type = MASIF_MOD_ADSR; in->mod[2].dest = MASIF_DEST_RES; in->mod[2].amount = 30;
        in->mod[2].p[1] = 40; in->mod[2].p[2] = 60; in->mod[2].p[3] = 40;
        in->mod[3].type = MASIF_MOD_TRIG; in->mod[3].dest = MASIF_DEST_VOLUME; in->mod[3].amount = -30;
        in->mod[3].p[2] = 30; in->mod[3].p[3] = 0x80u;   /* (ducked by track 1's notes) */
        in->name[0] = 'L'; in->name[1] = 'O'; in->name[2] = 'A'; in->name[3] = 'D'; in->name[4] = (uint8_t)('1' + t);
        for (r = 0; r < 8u; r++) masif_song.table[t][r].transpose = tt[r];
        for (r = 0; r < MASIF_ROWS; r++) {
            masif_cell_t *c = &masif_song.phrase[t][r];
            c->note = (uint8_t)(48u + t * 7u + (r * 5u) % 12u);
            c->instrument = (uint8_t)t; c->volume = 110;
            if (r % 4u == 0u) { c->fx = MASIF_FX_RETRIGGER; c->param = 0x03; }
            else { c->fx = MASIF_FX_ARPEGGIO; c->param = 0x37; }
            c->fx2 = MASIF_FX_VIBRATO; c->param2 = 0x46;
        }
        for (r = 0; r < 4u; r++) { masif_song.chain[t][r].phrase = (uint8_t)t; masif_song.chain[t][r].transpose = tr[r]; }
        masif_song.song[0][t] = (uint8_t)t;
    }
    (void)masif_song_sanitize(&masif_song);
    return 0;
}

/* enter on the project page: LOAD, SAVE, NEW, TEST (only while stopped: the audio must not play a song that is being
 * replaced, and a save stops the audio for its erases) */
static void mu_project_enter(void)
{
    uint32_t rc0;
    if (mu.prow < MJ_LOAD || mu.prow > MJ_TEST) return;
    if (sq.playing) { mu_say("STOP FIRST"); return; }
    mu.mute = mu.solo = 0;                          /* (a song in place of this one: all heard, as masif_fm1_song_changed does) */
    if (mu.prow == MJ_NEW) {
        masif_song_clear(&masif_song);
        masif_fm1_song_changed();
        for (rc0 = 0; rc0 < 9u; rc0++) mu.sname[rc0] = 0;
        mu_say("NEW SONG");
        return;
    }
    if (mu.prow == MJ_TEST) {
        if (mu_load_test()) { mu_say("NO SAMPLES"); return; }
        masif_fm1_song_changed();
        for (rc0 = 0; rc0 < 9u; rc0++) mu.sname[rc0] = "LOADTEST"[rc0 < 8u ? rc0 : 8u];
        mu_say("LOAD TEST: PLAY");
        return;
    }
#if MASIF_SONG_STORE
    if (mu.prow == MJ_SAVE) {
        int rc;
        if (!mu.sname[0]) {                         /* (no name yet: SONG and the slot's number) */
            for (rc0 = 0; rc0 < 9u; rc0++) mu.sname[rc0] = "SONG 00"[rc0 < 7u ? rc0 : 7u];
            mu.sname[5] = (char)('0' + (mu.slot + 1u) / 10u); mu.sname[6] = (char)('0' + (mu.slot + 1u) % 10u);
        }
        for (rc0 = 8u; rc0 > 0u && (!mu.sname[rc0 - 1u] || mu.sname[rc0 - 1u] == ' '); rc0--) mu.sname[rc0 - 1u] = 0;   /* (no spaces at the end) */
        rc = masif_store_save(&masif_song, mu.slot, mu.sname);
        mu_say(rc == MASIF_STORE_OK ? "SAVED" : rc == MASIF_STORE_FULL ? "NO ROOM" : "NOT SAVED");
    } else {
        int rc = masif_store_load(&masif_song, mu.slot);
        if (rc != MASIF_STORE_EMPTY) masif_fm1_song_changed();
        if (rc == MASIF_STORE_OK) mu_sname_from(mu.slot);
        mu_say(rc == MASIF_STORE_OK ? "LOADED" : rc == MASIF_STORE_EMPTY ? "EMPTY" : "DAMAGED");
    }
#else
    mu_say("NO STORE");
#endif
}

/* ---- the controls, from the main loop */
static void masif_ui_init(void)
{
    uint32_t r, c;
    for (r = 0; r < sizeof mu; r++) ((uint8_t *)&mu)[r] = 0;
    for (r = 0; r < MU_ROWS; r++) for (c = 0; c < MU_COLS; c++) mu_shown_ch[r][c] = mu_shown_at[r][c] = 0;
    mu.all = 1;
    mu.last_note = 60; mu.last_vol = 100; mu.last_fx = MASIF_FX_RETRIGGER;
    mu.input_at = fm1_ms;
    for (r = 0; r < sizeof ms; r++) ((uint8_t *)&ms)[r] = 0;
    ms.thr = 7; ms.sh = 1; ms.max_s = 10; ms.slot = ms.name_slot = MS_NONE;           /* (REC: USB, from -48 dB, 22 kHz, 12 bit, 10 s at the most) */
}

/* PLAY on a page: what the page is on (the song from the cursor's row, the chain, the phrase), or stop; on the
 * instrument and the modulation page the instrument's sound at its root note, and let go */
static void mu_play_page(void)
{
    if (mu.page == MU_SAMPLER) { mu_rec_go(); return; }   /* (REC records there; the note keys hear) */
    if (mu.page == MU_INST || mu.page == MU_MOD) {
        mu.sounding = (uint8_t)!mu.sounding;
        masif_fm1_preview(mu.track, mu.inst, mu.sounding ? mu_inst()->root_note : 0u, 127);
        return;
    }
    if (sq.playing) { sq_post(RQ_STOP, 0, 0); return; }
    if (mu.page == MU_CHAIN) masif_fm1_play_what(MASIF_PLAY_CHAIN, mu.track, mu.chain);
    else if (mu.page == MU_PHRASE || mu.page == MU_GROOVE) masif_fm1_play_what(MASIF_PLAY_PHRASE, mu.track, mu.phrase);
    else masif_fm1_play_what(MASIF_PLAY_SONG, 0, mu.page == MU_SONG ? mu.song_row : 0u);
    sq_post(RQ_PLAY, 0, 0);
}

/* ---- KNOB 1-4: given a setting each (2026-10-21): OPTION + a knob turned gives it the setting under
 * the cursor (which instrument, slot and row: it stays that one wherever the cursor goes), SHIFT + OPTION + a knob
 * turned takes it away. 4 banks of 4: FX goes round them */
#define MU_BANKS 4u
typedef struct { uint8_t on, page, inst, sub, row; } mu_knob_t;      /* sub: the tab, or the slot */
static mu_knob_t mu_knobs[MU_BANKS][4];
static uint8_t mu_was[sizeof mu];                   /* the screens as they were, while a knob's setting is worked on */
/* mu as if the cursor were on the knob's setting; mu_unaim puts back what was */
static int mu_aim(const mu_knob_t *k)
{
    memcpy(mu_was, &mu, sizeof mu);
    if (!k->on) return 0;
    mu.page = k->page; mu.sel = 0; mu.pick = PK_NONE; mu.inum = 0;
    switch (k->page) {
    case MU_INST: mu.inst = k->inst; mu.itab = k->sub; mu.irow = k->row; return 1;
    case MU_MOD: mu.inst = k->inst; mu.mslot = k->sub; mu.mrow = k->row; return 1;
    case MU_MIXER: mu.mcol = k->row; return 1;
    case MU_PROJECT: mu.prow = k->row; return 1;
    default: return 0;
    }
}
static void mu_unaim(void) { memcpy(&mu, mu_was, sizeof mu); }
/* knob i of the bank as a row of the grid: "1 I05 CUTOFF   4F", "2 M05.1 AMOUNT +20", "3 MIX T1        64" */
static void mu_knob_row(uint32_t i, uint32_t r, uint8_t at)
{
    static const char *const MIXN[7] = {"MIX T1", "MIX T2", "MIX T3", "MIX T4", "MIX LO", "MIX MID", "MIX HI"};
    const mu_knob_t *k = &mu_knobs[mu.bank % MU_BANKS][i & 3u];
    mu_par_t p;
    mu_fill(0, r, MU_COLS, r + 1u, at);
    mu_dec(0, r, (i & 3u) + 1u, 1, at);
    if (!mu_aim(k)) { mu_unaim(); mu_put(2, r, "-- OPT+TURN: SET", at); return; }
    if (k->page == MU_INST || k->page == MU_MOD) {
        mu_put(2, r, k->page == MU_INST ? "I" : "M", at);
        mu_hex(3, r, k->inst, 2, at);
        if (k->page == MU_MOD) { mu_put(5, r, ".", at); mu_dec(6, r, k->sub + 1u, 1, at); }
        if (mu_par(mu_par_row(), &p)) { mu_putn(k->page == MU_INST ? 6u : 8u, r, p.label, 7, at); mu_par_text(15, r, &p, mu_par_value(&p), at); }
    } else if (k->page == MU_MIXER) {
        mu_put(2, r, MIXN[k->row % 7u], at);
        if (k->row < MASIF_TRACKS) mu_hex(15, r, masif_song.track_level[k->row], 2, at);
        else mu_signed(15, r, masif_song.master_eq[(k->row - MASIF_TRACKS) % 3u], at);
    } else if (k->row == MJ_TEMPO) {
        mu_put(2, r, "TEMPO", at);
        mu_dec(15, r, masif_song.tempo ? masif_song.tempo : sq.bpm10 / 10u, 3, at);
    } else if (k->row == MJ_KEY) {
        mu_put(2, r, "KEY", at); mu_key(15, r, at);
    } else if (k->row == MJ_SCALE) {
        mu_put(2, r, "SCALE", at);
        mu_putn(11, r, masif_song.scale < MASIF_SCALES ? masif_scale_name[masif_song.scale] : "?", 9, at);
    } else {
        mu_put(2, r, "GROOVE", at); mu_hex(15, r, masif_song.project_groove, 2, at);
    }
    mu_unaim();
}
/* FX held: the bank's four knobs */
static void mu_draw_knobs(void)
{
    const uint8_t box = MU_INK | MU_ON(MU_DIM);
    uint32_t i;
    mu_fill(0, 11, MU_COLS, 16, box);
    mu_put(0, 11, "KNOBS BANK", MU_MID | MU_ON(MU_DIM));
    mu_dec(11, 11, mu.bank % MU_BANKS + 1u, 1, box);
    for (i = 0; i < 4u; i++) mu_knob_row(i, 12u + i, box);
}
static void mu_knob_said(uint32_t i) { mu_say(""); mu.msg_knob = (uint8_t)(i + 1u); }
/* OPTION + knob i: the setting under the cursor is the knob's (clear: the knob has none) */
static void mu_knob_set(uint32_t i, int clear)
{
    mu_knob_t *k = &mu_knobs[mu.bank % MU_BANKS][i], n;
    mu_par_t p;
    n.on = 1; n.page = mu.page; n.inst = mu.inst; n.sub = 0; n.row = 0;
    if (clear) { k->on = 0; mu.sel = 0; mu_knob_said(i); return; }
    if ((mu.page == MU_INST || mu.page == MU_MOD) && !mu.inum && mu_par(mu_par_row(), &p)) {
        n.sub = mu.page == MU_INST ? mu.itab : mu.mslot;
        n.row = (uint8_t)mu_par_row();
    } else if (mu.page == MU_MIXER) {
        n.row = mu.mcol;
    } else if (mu.page == MU_PROJECT && mu.prow >= MJ_TEMPO && mu.prow <= MJ_GROOVE) {
        n.row = mu.prow;
    } else {
        mu_say("NO SETTING HERE");
        return;
    }
    *k = n;
    mu_knob_said(i);
}
/* knob i turned by d: its setting by d (with SHIFT by 0x10 / an octave) */
static void mu_knob_turn(uint32_t i, int32_t d)
{
    int big = mu.shift;
    if (mu_aim(&mu_knobs[mu.bank % MU_BANKS][i])) mu_value(big ? 0 : d, big ? d : 0);
    mu_unaim();
    mu_knob_said(i);
}

/* ---- where one left off: the song in RAM is kept in the store's working slot (MASIF_STORE_WORK), the page, the
 * cursors and the knobs as records in a sector of their own, each a little after the last touch while nothing
 * plays (a save stops the audio for its erases). At power-on both are back (zp12.c loads the working copy) */
#if MASIF_SONG_STORE
#ifndef MU_KEEP_AT
#define MU_KEEP_AT 0xC4000u                         /* zp12's store's first sector: MASIF_UI writes no zp12 save */
#endif
#define MU_KEEP_REC 128u
#define MU_KEEP_RECS 30u                            /* (not the 31st: where the loader looks for an update record) */
#define MU_KEEP_VER 4u                              /* (4: FONT at the end of MU_KEPT; a 3 is read without it) */
/* what is kept, and the top of each (a value at or above it is 0; 0: any) */
static const struct { uint8_t *p; uint8_t top; } MU_KEPT[] = {
    {&mu.page, MU_NPAGE}, {&mu.back, MU_NPAGE}, {&mu.track, MASIF_TRACKS}, {&mu.song_row, 0}, {&mu.song_top, 0},
    {&mu.chain, MASIF_CHAINS}, {&mu.step, MASIF_CHAIN_STEPS}, {&mu.ccol, 2}, {&mu.phrase, MASIF_PHRASES},
    {&mu.row, MASIF_ROWS}, {&mu.pcol, MP_NCOL}, {&mu.prow, MJ_N}, {&mu.inst, MASIF_INSTRUMENTS}, {&mu.itab, MI_NTAB},
    {&mu.irow, 16}, {&mu.mslot, MASIF_MODS}, {&mu.mrow, 8}, {&mu.groove, MASIF_GROOVES}, {&mu.grow, MASIF_GROOVE_STEPS},
    {&mu.trow, MASIF_TABLE_ROWS}, {&mu.tcol, MT_NCOL}, {&mu.mcol, 7}, {&mu.slot, MASIF_STORE_NAMED},
    {&mu.palette, MU_NPALETTE}, {&mu.zoom, 5}, {&mu.last_chain, MASIF_CHAINS}, {&mu.last_phrase, MASIF_PHRASES},
    {&mu.last_note, 121}, {&mu.last_inst, MASIF_INSTRUMENTS}, {&mu.last_vol, 128}, {&mu.last_fx, MU_NFX},
    {&mu.bank, MU_BANKS}, {&mu.font, 2}};
#define MU_NKEPT (sizeof MU_KEPT / sizeof MU_KEPT[0])
static uint8_t mu_kept[MU_KEEP_REC];                /* the record as the flash has it last */
static uint32_t mu_keep_pack(uint8_t *rec)
{
    uint32_t i, n = 8, c;
    for (i = 0; i < MU_KEEP_REC; i++) rec[i] = 0xFFu;
    for (i = 0; i < MU_NKEPT; i++) rec[n++] = *MU_KEPT[i].p;
    for (i = 0; i < MU_BANKS * 4u; i++) {
        const mu_knob_t *k = &mu_knobs[i / 4u][i % 4u];
        rec[n++] = k->on; rec[n++] = k->page; rec[n++] = k->inst; rec[n++] = k->sub; rec[n++] = k->row;
    }
    rec[0] = 'M'; rec[1] = 'K'; rec[2] = MU_KEEP_VER; rec[3] = (uint8_t)(n - 8u);
    c = ms_crc(0xFFFFFFFFu, rec + 8, n - 8u);
    rec[4] = (uint8_t)c; rec[5] = (uint8_t)(c >> 8); rec[6] = (uint8_t)(c >> 16); rec[7] = (uint8_t)(c >> 24);
    return n;
}
static uint32_t mu_keep_nkept(uint32_t ver) { return ver == 3u ? MU_NKEPT - 1u : MU_NKEPT; }
static int mu_keep_good(const uint8_t *rec)
{
    uint32_t c;
    if (rec[0] != 'M' || rec[1] != 'K' || (rec[2] != MU_KEEP_VER && rec[2] != 3u) ||
        rec[3] != mu_keep_nkept(rec[2]) + MU_BANKS * 4u * 5u) return 0;
    c = ms_crc(0xFFFFFFFFu, rec + 8, rec[3]);
    return rec[4] == (uint8_t)c && rec[5] == (uint8_t)(c >> 8) && rec[6] == (uint8_t)(c >> 16) && rec[7] == (uint8_t)(c >> 24);
}
static void mu_keep_unpack(const uint8_t *rec)
{
    uint32_t i, n = 8;
    for (i = 0; i < MU_NKEPT; i++, n++) {
        if (i >= mu_keep_nkept(rec[2])) { *MU_KEPT[i].p = 0; n--; continue; }   /* (an older record: 0) */
        *MU_KEPT[i].p = MU_KEPT[i].top && rec[n] >= MU_KEPT[i].top ? 0u : rec[n];
    }
    for (i = 0; i < MU_BANKS * 4u; i++, n += 5u) {
        mu_knob_t *k = &mu_knobs[i / 4u][i % 4u];
        k->on = rec[n] == 1u; k->page = rec[n + 1u]; k->inst = rec[n + 2u] & 127u; k->sub = rec[n + 3u]; k->row = rec[n + 4u];
        if ((k->page == MU_INST && k->sub >= MI_NTAB) || (k->page == MU_MOD && k->sub >= MASIF_MODS) || k->row >= 16u ||
            (k->page != MU_INST && k->page != MU_MOD && k->page != MU_MIXER && k->page != MU_PROJECT) ||
            (k->page == MU_MIXER && k->row >= 7u) || (k->page == MU_PROJECT && (k->row < MJ_TEMPO || k->row > MJ_GROOVE)))
            k->on = 0;
    }
    if (!mu.last_note) mu.last_note = 60;
    if (MASIF_FX_OUT(mu.last_fx)) mu.last_fx = MASIF_FX_NONE;   /* (an effect taken out since) */
    if (mu.zoom == 3u) mu.zoom = 2;
    if (mu.song_row < mu.song_top || mu.song_row > mu.song_top + 15u) mu.song_top = mu.song_row;
}
/* the newest good record into the screens, and where the next goes */
static void mu_keep_load(void)
{
    static uint8_t rec[MU_KEEP_REC];
    uint32_t i, best = MU_KEEP_RECS;
    mu.keep_next = MU_KEEP_RECS;                    /* (nothing erased: the next save erases the sector first) */
    for (i = 0; i < MU_KEEP_RECS; i++) {
        if (masif_fl_read(MU_KEEP_AT + i * MU_KEEP_REC, rec, MU_KEEP_REC)) return;
        if (rec[0] == 0xFFu && rec[1] == 0xFFu) { mu.keep_next = (uint8_t)i; break; }
        if (mu_keep_good(rec)) best = i;
    }
    if (best == MU_KEEP_RECS || masif_fl_read(MU_KEEP_AT + best * MU_KEEP_REC, rec, MU_KEEP_REC)) return;
    mu_keep_unpack(rec);
    (void)mu_keep_pack(mu_kept);                    /* (as it stands now, after the checks) */
}
static void mu_keep_save(void)
{
    static uint8_t rec[MU_KEEP_REC];
    uint32_t i;
    (void)mu_keep_pack(rec);
    for (i = 0; i < MU_KEEP_REC && rec[i] == mu_kept[i]; i++) {}
    if (i == MU_KEEP_REC) return;                   /* (no change) */
    if (mu.keep_next >= MU_KEEP_RECS) {
        if (masif_fl_erase(MU_KEEP_AT)) return;
        mu.keep_next = 0;
    }
    if (masif_fl_write(MU_KEEP_AT + (uint32_t)mu.keep_next * MU_KEEP_REC, rec, MU_KEEP_REC)) return;
    mu.keep_next++;
    for (i = 0; i < MU_KEEP_REC; i++) mu_kept[i] = rec[i];
}
static uint32_t mu_song_hash(void)
{
    const uint8_t *p = (const uint8_t *)&masif_song;
    uint32_t h = 2166136261u, i;
    for (i = 0; i < sizeof masif_song; i++) h = (h ^ p[i]) * 16777619u;
    for (i = 0; i < 8u; i++) h = (h ^ (uint8_t)mu.sname[i]) * 16777619u;   /* (a new name is kept too) */
    return h;
}
/* a second after the last look, MU_KEEP_QUIET after the last touch, while stopped and no take on its way (each
 * program stops the interrupts: a take's sound with them): what changed is kept */
static void mu_keep_step(void)
{
    uint32_t h;
    if (sq.playing || mr_busy() || (uint32_t)(fm1_ms - mu.input_at) < MU_KEEP_QUIET || (uint32_t)(fm1_ms - mu.keep_check) < 1000u) return;
    mu.keep_check = fm1_ms;
    h = mu_song_hash();
    if (h != mu.song_hash) {
        int rc = masif_store_save(&masif_song, MASIF_STORE_WORK, mu.sname);
        if (rc == MASIF_STORE_OK) { mu.song_hash = h; mu.keep_full = 0; }
        else if (!mu.keep_full) { mu.keep_full = 1; mu_say(rc == MASIF_STORE_FULL ? "NO ROOM TO KEEP SONG" : "SONG NOT KEPT"); }
    }
    mu_keep_save();
}
/* at power-on, after masif_ui_init, if the store is there (the song in RAM is the one zp12.c loaded) */
static void masif_ui_resume(void)
{
    mu_keep_load();
#if MASIF_CONTROLS == 0
    mu.bank = 0;                                    /* (FX does nothing now: the bank a resume had stays out) */
#endif
    mu_sname_from(MASIF_STORE_WORK);
    mu.song_hash = mu_song_hash();
    mu.keep_check = fm1_ms;
    mu.input_at = fm1_ms;
    mu.resumed = 1;
    mu.all = 1;
}
#endif

#if MASIF_CONTROLS == 0
/* ---- the arrows' controls (MASIF_CONTROLS 0) */
/* EDIT while a phrase's note is under the cursor: that note heard (until EDIT goes up) */
static void mu_edit_heard(void)
{
    uint8_t i;
    if (mu.page != MU_PHRASE || mu.pcol != MP_NOTE || mu.sel) return;
    i = masif_song.phrase[mu.phrase % MASIF_PHRASES][mu.row].instrument;
    if (i >= MASIF_INSTRUMENTS) i = mu.last_inst;
    masif_fm1_preview(mu.track, i, mu.last_note, mu.last_vol);
    mu.previewing = 1;
}
/* to the right into what the cursor is on, only if there is something: from the song (and the pages at
 * S) a chain, from a chain a phrase */
static int mu_can_right(void)
{
    if ((mu.page == MU_SONG || mu.page == MU_PROJECT || mu.page == MU_MIXER) &&
        masif_song.song[mu.song_row][mu.track] >= MASIF_CHAINS) { mu_say("NO CHAIN HERE"); return 0; }
    if (mu.page == MU_CHAIN && masif_song.chain[mu.chain % MASIF_CHAINS][mu.step].phrase >= MASIF_PHRASES) {
        mu_say("NO PHRASE HERE");
        return 0;
    }
    return 1;
}
/* SHIFT + an arrow: the pages, not round */
static void mu_nav(int32_t dx, int32_t dy)
{
    const uint32_t p = mu.page;
    int at_s = p == MU_SONG || p == MU_PROJECT || p == MU_MIXER;
    if (dx > 0) { if (p != MU_TABLE && p != MU_GROOVE && p != MU_PROJECT && p != MU_MIXER && mu_can_right()) mu_page_right(); }
    else if (dx < 0) { if (!at_s) mu_page_left(); }
    else if (dy < 0) {
        if (p == MU_SONG) mu_goto(MU_PROJECT);
        else if (p == MU_MIXER) mu_goto(MU_SONG);
        else if (p == MU_TABLE) mu_goto(MU_GROOVE);
        else if (p == MU_INST) mu_goto(MU_MOD);
        else if (p == MU_SAMPLER) mu_goto(MU_INST);
    } else {
        if (p == MU_PROJECT) mu_goto(MU_SONG);
        else if (p == MU_SONG) mu_goto(MU_MIXER);
        else if (p == MU_GROOVE) mu_goto(MU_TABLE);
        else if (p == MU_MOD) mu_goto(MU_INST);
        else if (p == MU_INST) mu_goto(MU_SAMPLER);
    }
}
/* OPTION + an arrow: the song a page of 16 rows up / down; the one before / after (a chain, a phrase, a groove, an
 * instrument) */
static void mu_opt_arrow(int32_t dx, int32_t dy)
{
    switch (mu.page) {
    case MU_SONG: if (dy) mu_cursor(0, dy * 16); break;
    case MU_CHAIN: mu.chain = (uint8_t)mu_clampi((int32_t)mu.chain + dx, 0, MASIF_CHAINS - 1); break;
    case MU_PHRASE: mu.phrase = (uint8_t)mu_clampi((int32_t)mu.phrase + dx, 0, MASIF_PHRASES - 1); break;
    case MU_GROOVE: mu.groove = (uint8_t)mu_clampi((int32_t)mu.groove + dx, 0, MASIF_GROOVES - 1); break;
    case MU_INST: case MU_MOD: case MU_TABLE:
        if (dx) { mu.inst = (uint8_t)mu_clampi((int32_t)mu.inst + dx, 0, MASIF_INSTRUMENTS - 1); mu.sel = 0; }
        break;
    default:
        break;
    }
}
static void mu_edit_act(void);
/* an arrow (its button), pressed or again while held */
static void mu_arrow(uint32_t b, int again)
{
    int32_t dx = b == MB_LEFT ? -1 : b == MB_RIGHT ? 1 : 0, dy = b == MB_UP ? -1 : b == MB_DOWN ? 1 : 0;
    mu.input_at = fm1_ms;
    if (mu.edit_pend) mu_edit_act();
    if (mu.pick) {                                  /* a list: through it (the pool a page at a time to the sides) */
        int32_t step = mu.pick == PK_POOL ? (dx ? dx * (int32_t)MU_POOL_ROWS : dy) : dx + dy * (int32_t)mu.pick_cols;
        if (mu.pick_n) mu.pick_cur = (uint8_t)mu_clampi((int32_t)mu.pick_cur + step, 0, (int32_t)mu.pick_n - 1);
        return;
    }
    if (mu.edit) { mu.edit_used = 1; mu.edit_at = 0; mu_value(dx, -dy); mu_edit_heard(); return; }   /* (no double tap after it) */
    if (mu.shift) { if (!again) mu_nav(dx, dy); return; }
    if (mu.option) { mu.option_used = 1; mu_opt_arrow(dx, dy); return; }
    mu_cursor(dx, dy);
}

/* an empty cell gets what was entered last (a note with its instrument; a chain; a phrase; a volume ...) */
static void mu_insert(void)
{
    uint32_t row, col, c;
    uint8_t d[7], was[7];
    mu_cur(&row, &col);
    mu_row_get(mu.page, row, d);
    if (d[col] != mu_col_empty(mu.page, col)) {     /* a note there: it is the one heard, and the next one entered */
        if (mu.page == MU_PHRASE && col == MP_NOTE && d[0] >= 1u && d[0] <= 120u) {
            mu.last_note = d[0];
            if (d[1] < MASIF_INSTRUMENTS) mu.last_inst = d[1];
        }
        mu_edit_heard();
        return;
    }
    for (c = 0; c < 7u; c++) was[c] = mu.undo[c] = d[c];
    mu_col_turn(mu.page, col, d, 0, 0, 1);
    for (c = 0; c < 7u && d[c] == was[c]; c++) {}
    if (c < 7u) { mu_row_post(mu.page, row, d); mu.undo_ok = 1; mu.undo_page = mu.page; mu.undo_row = (uint8_t)row; }
    mu_edit_heard();
}
/* the double tap: a chain nothing plays and that is empty, a phrase no chain has and that is empty, an instrument
 * without a sample (the cell's own counts as not used) */
static void mu_new_unused(void)
{
    static uint8_t used[32];
    uint32_t row, col, i, k;
    int32_t got = -1;
    uint8_t d[7];
    mu_cur(&row, &col);
    for (i = 0; i < sizeof used; i++) used[i] = 0;
    if (mu.page == MU_SONG) {
        for (i = 0; i < MASIF_SONG_ROWS; i++)
            for (k = 0; k < MASIF_TRACKS; k++) {
                uint32_t c = masif_song.song[i][k];
                if (c < MASIF_CHAINS && !(i == row && k == col)) used[c >> 3] |= (uint8_t)(1u << (c & 7u));
            }
        for (i = 0; i < MASIF_CHAINS && got < 0; i++) {
            for (k = 0; k < MASIF_CHAIN_STEPS && masif_song.chain[i][k].phrase >= MASIF_PHRASES; k++) {}
            if (k == MASIF_CHAIN_STEPS && !((used[i >> 3] >> (i & 7u)) & 1u)) got = (int32_t)i;
        }
        if (got >= 0) { mu_edit1(MASIF_EDIT_SONG, row, col, (uint8_t)got); mu.last_chain = (uint8_t)got; }
    } else if (mu.page == MU_CHAIN && col == 0u) {
        for (i = 0; i < MASIF_CHAINS; i++)
            for (k = 0; k < MASIF_CHAIN_STEPS; k++) {
                uint32_t p = masif_song.chain[i][k].phrase;
                if (p < MASIF_PHRASES && !(i == mu.chain && k == row)) used[p >> 3] |= (uint8_t)(1u << (p & 7u));
            }
        for (i = 0; i < MASIF_PHRASES && got < 0; i++) {
            for (k = 0; k < MASIF_ROWS; k++) {
                const masif_cell_t *c = &masif_song.phrase[i][k];
                if (c->note || c->instrument < MASIF_INSTRUMENTS || c->volume <= 127u || c->fx || c->param || c->fx2 || c->param2) break;
            }
            if (k == MASIF_ROWS && !((used[i >> 3] >> (i & 7u)) & 1u)) got = (int32_t)i;
        }
        if (got >= 0) { mu_row_get(MU_CHAIN, row, d); d[0] = (uint8_t)got; mu_row_post(MU_CHAIN, row, d); mu.last_phrase = (uint8_t)got; }
    } else if (mu.page == MU_PHRASE && col == MP_INST) {
        got = mu_next_empty();
        if (got >= 0) { mu_row_get(MU_PHRASE, row, d); d[1] = (uint8_t)got; mu_row_post(MU_PHRASE, row, d); mu.last_inst = (uint8_t)got; }
    } else {
        return;
    }
    if (got < 0) mu_say("NONE FREE");
}
/* OPTION + EDIT: cut: the selection or the cell under the cursor into the clipboard and empty (SHIFT +
 * EDIT pastes it); an empty note: OFF (the user, 2026-10-21: the note let go there, its release rings, as STP 00);
 * a name's character: gone; on the instrument pages the setting as a new instrument has it */
static void mu_delete(void)
{
    if (mu.page == MU_PHRASE && mu.pcol == MP_NOTE && !mu.sel &&
        masif_song.phrase[mu.phrase % MASIF_PHRASES][mu.row].note == MASIF_NOTE_NONE) {
        uint8_t d[7];
        mu_row_get(MU_PHRASE, mu.row, d);
        d[0] = MASIF_NOTE_OFF;
        mu_row_post(MU_PHRASE, mu.row, d);
        return;
    }
    if (mu.page == MU_INST && mu.inum && mu.ichar) { mu_name_set(mu.ichar - 1u, 0); return; }
    if (mu.page == MU_PROJECT && mu.prow == MJ_SLOT && mu.schar) { mu_sname_set(mu.schar - 1u, 0); return; }
    if (mu.page == MU_GROOVE && mu.gnum) return;
    if (mu.page == MU_SAMPLER) { mu_smp_delete(); return; }
    mu_cut();
}
/* OPTION going down while EDIT is held and was part of nothing (EDIT a few ms first: the user pressed OPTION + EDIT
 * and got EDIT's insert or its note heard, 2026-10-21): it is OPTION + EDIT. What EDIT's press put into an empty
 * cell is taken back as the row it was (the queue may not have it yet), with OFF on an empty note */
static void mu_edit_option(void)
{
    if (mu.edit_pend) { mu.edit_pend = 0; mu.edit_used = 1; mu_delete(); return; }
    mu.edit_used = 1; mu.edit_at = 0;
    if (mu.previewing) { masif_fm1_preview(mu.track, 0, 0, 0); mu.previewing = 0; }
    if (mu.undo_ok) {
        mu.undo_ok = 0;
        if (mu.undo_page == MU_PHRASE && mu.pcol == MP_NOTE) mu.undo[0] = MASIF_NOTE_OFF;
        mu_row_post(mu.undo_page, mu.undo_row, mu.undo);
        return;
    }
    mu_delete();
}
/* track t's mute (solo: its solo) the other way round; what is heard: the muted ones off, and with a solo all but
 * the soloed ones */
static void mu_mute_set(uint32_t t, int solo)
{
    uint8_t *m = solo ? &mu.solo : &mu.mute;
    *m ^= (uint8_t)(1u << (t % MASIF_TRACKS));
    masif_mute = (uint8_t)(mu.mute | (mu.solo ? ~mu.solo & ((1u << MASIF_TRACKS) - 1u) : 0u));
}
/* ---- cloning (EDIT on a selection): on the song page its chains become copies of themselves (new
 * chains, the same phrases); EDIT again soon: deep, their phrases copies too. On the chain page its phrases become
 * copies. The copies go into chains / phrases nothing uses and that are empty, written straight into the song (no
 * one plays them before the cells say so; the cells are single bytes) */
static uint8_t mu_taken[32];                        /* (chains or phrases this clone has taken already) */
static int mu_taken_bit(uint32_t i) { return (mu_taken[i >> 3] >> (i & 7u)) & 1u; }
static int32_t mu_free_chain(void)
{
    uint32_t i, r, t, k;
    for (i = 0; i < MASIF_CHAINS; i++) {
        if (mu_taken_bit(i)) continue;
        for (k = 0; k < MASIF_CHAIN_STEPS && masif_song.chain[i][k].phrase >= MASIF_PHRASES; k++) {}
        if (k < MASIF_CHAIN_STEPS) continue;
        for (r = 0; r < MASIF_SONG_ROWS; r++) { for (t = 0; t < MASIF_TRACKS && masif_song.song[r][t] != i; t++) {} if (t < MASIF_TRACKS) break; }
        if (r == MASIF_SONG_ROWS) { mu_taken[i >> 3] |= (uint8_t)(1u << (i & 7u)); return (int32_t)i; }
    }
    return -1;
}
static int32_t mu_free_phrase(void)
{
    uint32_t i, c, k;
    for (i = 0; i < MASIF_PHRASES; i++) {
        if (mu_taken_bit(i)) continue;
        for (k = 0; k < MASIF_ROWS; k++) {
            const masif_cell_t *x = &masif_song.phrase[i][k];
            if (x->note || x->instrument < MASIF_INSTRUMENTS || x->volume <= 127u || x->fx || x->param || x->fx2 || x->param2) break;
        }
        if (k < MASIF_ROWS) continue;
        for (c = 0; c < MASIF_CHAINS * MASIF_CHAIN_STEPS && (&masif_song.chain[0][0])[c].phrase != i; c++) {}
        if (c == MASIF_CHAINS * MASIF_CHAIN_STEPS) { mu_taken[i >> 3] |= (uint8_t)(1u << (i & 7u)); return (int32_t)i; }
    }
    return -1;
}
/* chain c's phrases: copies (the same phrase twice in it: one copy). 0: none was free */
static int mu_clone_phrases_of(uint32_t c, uint8_t *map)
{
    uint32_t k;
    for (k = 0; k < MASIF_CHAIN_STEPS; k++) {
        uint32_t p = masif_song.chain[c][k].phrase;
        if (p >= MASIF_PHRASES) continue;
        if (map[p] == MASIF_NONE) {
            int32_t n = mu_free_phrase();
            if (n < 0) return 0;
            memcpy(masif_song.phrase[n], masif_song.phrase[p], sizeof masif_song.phrase[0]);
            map[p] = (uint8_t)n;
        }
        masif_song.chain[c][k].phrase = map[p];
    }
    return 1;
}
static void mu_clone(void)
{
    static uint8_t map[256], seen[16];              /* old -> its copy in this clone; the chains deepened */
    uint32_t r0, r1, c0, c1, r, c, i;
    int deep = mu.clone_at && (uint32_t)(fm1_ms - mu.clone_at) < MU_DOUBLE_MS, ok = 1;
    if (mu.page != MU_SONG && mu.page != MU_CHAIN) return;
    (void)mu_rect(&r0, &r1, &c0, &c1, 0);
    for (i = 0; i < sizeof mu_taken; i++) mu_taken[i] = 0;
    for (i = 0; i < sizeof map; i++) map[i] = MASIF_NONE;
    for (i = 0; i < sizeof seen; i++) seen[i] = 0;
    if (mu.page == MU_SONG) {
        for (r = r0; r <= r1 && ok; r++)
            for (c = c0; c <= c1 && ok; c++) {
                uint8_t *cell = &masif_song.song[r & 255u][c % MASIF_TRACKS];
                if (*cell >= MASIF_CHAINS) continue;
                if (deep) {                         /* (the chains are this clone's: their phrases, once each) */
                    if (!((seen[*cell >> 3] >> (*cell & 7u)) & 1u)) { seen[*cell >> 3] |= (uint8_t)(1u << (*cell & 7u)); ok = mu_clone_phrases_of(*cell, map); }
                    continue;
                }
                if (map[*cell] == MASIF_NONE) {
                    int32_t n = mu_free_chain();
                    if (n < 0) { ok = 0; break; }
                    memcpy(masif_song.chain[n], masif_song.chain[*cell], sizeof masif_song.chain[0]);
                    map[*cell] = (uint8_t)n;
                }
                *cell = map[*cell];
            }
    } else {
        for (r = r0; r <= r1 && ok; r++) {
            masif_chain_step_t *st = &masif_song.chain[mu.chain % MASIF_CHAINS][r % MASIF_CHAIN_STEPS];
            uint32_t p = st->phrase;
            if (p >= MASIF_PHRASES) continue;
            if (map[p] == MASIF_NONE) {
                int32_t n = mu_free_phrase();
                if (n < 0) { ok = 0; break; }
                memcpy(masif_song.phrase[n], masif_song.phrase[p], sizeof masif_song.phrase[0]);
                map[p] = (uint8_t)n;
            }
            st->phrase = map[p];
        }
    }
    mu.clone_at = deep ? 0u : fm1_ms;
    mu_say(!ok ? "NONE FREE" : deep ? "DEEP CLONED" : "CLONED");
}
static void mu_edit_down(void)
{
    if (mu.pick == PK_POOL) {                       /* the pool (it stays open): EDIT takes, OPTION + EDIT deletes */
        mu.edit = 1; mu.edit_used = 1; mu.edit_pend = 0;
        if (mu.option) { mu.option_used = 1; mu_pool_delete(); }
        else if (mu.pick_n) mu_give_sample(mu_pool_wave(mu.pick_cur));   /* (OPTION alone leaves) */
        return;
    }
    mu.edit = 1; mu.edit_used = 0; mu.pick_cancel = 0; mu.undo_ok = 0;
    if (mu.shift) { mu.edit_used = 1; mu_paste(); return; }
    if (mu.option) { mu.edit_used = 1; mu.option_used = 1; mu_delete(); return; }
    if (mu.sel) return;
    mu.edit_pend = 1; mu.edit_pend_at = fm1_ms + MU_COMBO_MS;   /* (OPTION or SHIFT a moment later: their combo) */
}
/* EDIT's own press, once no OPTION or SHIFT came with it (or it was let go, or an arrow came first) */
static void mu_edit_act(void)
{
    uint32_t row, col;
    int twice;
    mu.edit_pend = 0;
    mu_pick_open();                                 /* (an effect, the sample: its list while EDIT is held) */
    if (mu.pick) return;
    if (!mu_is_grid(mu.page) || (mu.page == MU_GROOVE && mu.gnum)) return;
    mu_cur(&row, &col);
    twice = mu.edit_at && (uint32_t)(fm1_ms - mu.edit_at) < MU_DOUBLE_MS && mu.edit_page == mu.page &&
            mu.edit_row == row && mu.edit_col == col;
    mu.edit_page = mu.page; mu.edit_row = (uint8_t)row; mu.edit_col = (uint8_t)col;
    mu.edit_at = twice ? 0u : fm1_ms;               /* (a third press is a first again) */
    if (twice) mu_new_unused();
    else mu_insert();
}
static void mu_edit_up(void)
{
    if (mu.edit_pend) mu_edit_act();
    mu.edit = 0;
    if (mu.previewing) { masif_fm1_preview(mu.track, 0, 0, 0); mu.previewing = 0; }
    if (mu.pick == PK_POOL && !mu.pick_cancel) return;   /* (the pool stays open: EDIT again takes, OPTION leaves) */
    if (mu.pick) {
        if (!mu.pick_cancel) { mu_pick_close(); return; }
        if (mu.pick == PK_POOL) { mu.wave_drawn = 0; masif_fm1_preview(mu.track, 0, 0, 0); }
        mu.pick = PK_NONE;
        return;
    }
    if (mu.edit_used) return;
    if (mu.sel) mu_clone();
    else if (mu.page == MU_PROJECT) mu_project_enter();
    else if (mu.page == MU_MIXER && mu.mcol < MASIF_TRACKS) mu_mute_set(mu.mcol, 0);
}
static void mu_play(void)
{
    uint32_t t = mu.page == MU_MIXER && mu.mcol < MASIF_TRACKS ? mu.mcol : mu.track;
    if (mu.pick == PK_POOL) { if (mu.pick_n) masif_fm1_audition(mu.track, (uint8_t)mu_pool_wave(mu.pick_cur)); return; }
    if (mu.pick) return;
    if (mu.option) { mu.option_used = 1; mu_mute_set(t, 0); return; }   /* OPTION + PLAY: the track's mute */
    if (mu.edit) {                                  /* EDIT + PLAY: its solo (what EDIT's press filled in: back) */
        mu.edit_used = 1; mu.edit_at = 0; mu.edit_pend = 0;
        if (mu.undo_ok) { mu_row_post(mu.undo_page, mu.undo_row, mu.undo); mu.undo_ok = 0; }
        mu_mute_set(t, 1);
        return;
    }
    if (mu.shift) {                                 /* the song, from the cursor's row of the song page */
        if (sq.playing) { sq_post(RQ_STOP, 0, 0); return; }
        masif_fm1_play_what(MASIF_PLAY_SONG, 0, mu.song_row);
        sq_post(RQ_PLAY, 0, 0);
        return;
    }
    mu_play_page();
}

/* a knob turned by d clicks. enc: 0 SELECT, 1 ALGORITHM, 2-5 KNOB 1-4, 6 PRESETS */
static void masif_ui_knob(uint32_t enc, int32_t d)
{
    mu.input_at = fm1_ms;
    if (enc == ME_SELECT || enc == ME_ALGORITHM) {  /* as before the arrows (the user, 2026-10-23): SELECT the cursor */
        int32_t alg = enc == ME_ALGORITHM;          /* along, ALGORITHM up and down; with SHIFT or EDIT the value */
        if (mu.edit_pend) mu_edit_act();
        if (mu.pick) {                              /* a list: through it (ALGORITHM by rows) */
            int32_t step = alg ? d * (mu.pick == PK_POOL ? 1 : (int32_t)mu.pick_cols) : d;
            if (mu.pick_n) mu.pick_cur = (uint8_t)mu_clampi((int32_t)mu.pick_cur + step, 0, (int32_t)mu.pick_n - 1);
        } else if (mu.edit || mu.shift) {
            mu_value(alg ? 0 : d, alg ? d : 0);
            if (mu.edit) { mu.edit_used = 1; mu.edit_at = 0; mu_edit_heard(); }   /* (heard until EDIT goes up) */
        } else mu_cursor(alg ? 0 : d, alg ? d : 0);
        return;
    }
    if (mu.pick) return;
    if (enc >= ME_KNOB1 && enc < ME_KNOB1 + 4u) {
        if (mu.option) { mu.option_used = 1; mu_knob_set(enc - ME_KNOB1, mu.shift); }
        else mu_knob_turn(enc - ME_KNOB1, d);
    } else if (enc == ME_PRESETS) {                 /* the pages along S C P I T and back, not round (as SHIFT + LEFT / */
        int32_t n = d < 0 ? -d : d;                 /* RIGHT: S is the end on the left, nothing past an empty chain step) */
        while (n--) {                               /* with SHIFT up and down (as SHIFT + UP / DOWN) */
            uint32_t was = mu.page;
            if (mu.shift) mu_nav(0, d < 0 ? -1 : 1);
            else mu_nav(d < 0 ? -1 : 1, 0);
            if (mu.page == was) break;
        }
    }
}

static void masif_ui_button(uint32_t b, int down)
{
    mu.input_at = fm1_ms;
    switch (b) {
    case MB_SHIFT:
        mu.shift = (uint8_t)(down != 0);
        if (down && mu.edit_pend) { mu.edit_pend = 0; mu.edit_used = 1; mu_paste(); }   /* (EDIT a moment first) */
        break;
    case MB_EDIT: if (down) mu_edit_down(); else mu_edit_up(); break;
    case MB_OPTION:
        if (down) {
            if (mu.pick == PK_POOL) { mu.option = 1; mu.option_used = 0; break; }   /* (OPTION + EDIT: delete; alone: leave) */
            if (mu.pick) { mu.pick_cancel = 1; break; }   /* (a list open: letting EDIT go changes nothing) */
            mu.option = 1; mu.option_used = mu.shift;
            if (mu.edit && !mu.edit_used && !mu.shift) { mu.option_used = 1; mu_edit_option(); }
            else if (mu.shift) mu_sel_step();
        } else {
            mu.option = 0;
            if (mu.pick == PK_POOL) {                   /* OPTION alone in the pool: it closes (what EDIT took stays) */
                if (!mu.option_used) {
                    mu_pool_cancel = 1; mu.pick = PK_NONE; mu.wave_drawn = 0;
                    masif_fm1_preview(mu.track, 0, 0, 0);
                }
                break;
            }
            if (!mu.option_used && mu.sel) { mu_copy(); mu.sel = 0; mu_say("COPIED"); }
        }
        break;
    case MB_BANK: break;                            /* (FX: nothing for now, the user 2026-10-23: the knobs' bank 1) */
    case MB_UP: case MB_DOWN: case MB_LEFT: case MB_RIGHT:
        if (down) { mu.arrow = (uint8_t)(b + 1u); mu.arrow_at = fm1_ms + MU_REPEAT_FIRST; mu_arrow(b, 0); }
        else if (mu.arrow == b + 1u) mu.arrow = 0;
        break;
    case MB_PLAY: if (down) mu_play(); break;
    default: break;                                 /* (OCT+ / -, ENV, LFO, SEQ: free for later) */
    }
}

/* what is lit (the user, 2026-10-21, after the LED test): btn bit i = button i, the white ones always. PLAY is on
 * REC now, whose LED is red only (the LED probe found no green, 2026-10-24): lit while stopped; playing, a short flash
 * on each beat (half a row: msfv's whole row read as dark on the beat); a take armed, blinking at 4 Hz, running 8 Hz, saving 2 Hz. The printed PLAY (SHIFT now) has a
 * red LED of its own and a green one (`green` bit 0: the spare at column 8, line 1): green always, as the white ones.
 * keys bit k = note key k (F3 ..): held down, or its note sounding on any track */
static void masif_ui_leds(uint32_t *btn, uint32_t *keys, uint32_t *green)
{
    uint32_t lit = ((1u << 14) - 1u) & ~(1u << MK_REC) & ~(1u << MK_PLAY), k = mu.keys, t, st = mr_state_now(), on;
    lit &= ~(1u << MK_OCTDN | 1u << MK_OCTUP | 1u << MK_FX | 1u << MK_ENV | 1u << MK_LFO | 1u << MK_SEQ);   /* (no use yet: */
                                                    /* dark, the user 2026-10-23) */
    if (st == MR_ARMED) on = (fm1_ms / 125u) & 1u;
    else if (st == MR_TAKE) on = (fm1_ms / 62u) & 1u;
    else if (st == MR_SAVING) on = (fm1_ms / 250u) & 1u;
    else on = !sq.playing || sq_ticks % 96u < 12u;
    lit |= on << MK_REC;
    *green = 1u;
    for (t = 0; t < MASIF_TRACKS; t++) {
        uint32_t n = masif_runtime.active_note[t];
        if (n >= 53u && n <= 79u && sp_ch[SP_NCH + t].on) k |= 1u << (n - 53u);
    }
    *btn = lit; *keys = k;
}
#else
/* a knob turned by d clicks. enc: 0 SELECT, 1 ALGORITHM, 2-5 KNOB 1-4, 6 PRESETS */
static void masif_ui_knob(uint32_t enc, int32_t d)
{
    if (mu.pick) {                                   /* the chooser is open: through its list (ALGORITHM: by rows) */
        if (enc == ME_SELECT || enc == ME_ALGORITHM)
            mu.pick_cur = (uint8_t)mu_clampi((int32_t)mu.pick_cur + d * (enc == ME_ALGORITHM ? (int32_t)mu.pick_cols : 1), 0, (int32_t)mu.pick_n - 1);
        return;
    }
    if (enc == ME_SELECT) { if (mu.shift) mu_value(d, 0); else mu_cursor(d, 0); }
    else if (enc == ME_ALGORITHM) { if (mu.shift) mu_value(0, d); else mu_cursor(0, d); }
    else if (enc == ME_PRESETS) {                    /* the pages: along the row, with shift off it */
        int32_t n = d < 0 ? -d : d;
        while (n--) {
            if (mu.shift) mu_page_vert(d);
            else if (d > 0) mu_page_right();
            else mu_page_left();
        }
    }
}

static void masif_ui_button(uint32_t b, int down)
{
    if (b == MK_GLO) { mu.shift = (uint8_t)(down != 0); return; }
    if (b == MK_EDIT) {
        if (down) mu_pick_open();
        else if (mu.pick) mu_pick_close();
        return;
    }
    if (b == MK_FX) {                                /* FX held on an effect column: the effects' list (as EDIT there) */
        if (down) { if (!mu.pick && (mu.page == MU_PHRASE || mu.page == MU_TABLE)) mu_pick_open(); }
        else if (mu.pick == PK_FX) mu_pick_close();
        return;
    }
    if (mu.pick == PK_POOL && down && mu.pick_n) {   /* the pool: PLAY hears the marked sample, HOME lets it be */
        if (b == MK_PLAY) masif_fm1_audition(mu.track, (uint8_t)mu_pool_wave(mu.pick_cur));
        if (b == MK_HOME) { mu_pool_cancel = 1; masif_fm1_preview(mu.track, 0, 0, 0); }
    }
    if (mu.pick) return;
    if (b == MK_REC) {                               /* option. With shift: select; alone, let go again: copy */
        if (down) {
            mu.option = 1; mu.option_used = mu.shift;
            if (mu.shift) mu_sel_step();
        } else {
            mu.option = 0;
            if (!mu.option_used && mu.sel) { mu_copy(); mu.sel = 0; mu_say("COPIED"); }
        }
        return;
    }
    if (!down) return;
    switch (b) {
    case MK_OCTUP: mu_page_right(); break;
    case MK_OCTDN: mu_page_vert(1); break;
    case MK_HOME:                                    /* back: out of a selection, else to the page before */
        if (mu.sel) mu.sel = 0;
        else { uint8_t p = mu.back; mu.back = mu.page; mu.page = p; mu.wave_drawn = 0; }
        break;
    case MK_SAVE:
        if (mu.option) { mu.option_used = 1; mu_cut(); }
        else if (mu.shift) mu_paste();
        else if (mu.sel) break;
        else if (mu.page <= MU_PHRASE) mu_page_right();
        else if (mu.page == MU_PROJECT) mu_project_enter();
        else if (mu.page == MU_MIXER && mu.mcol < MASIF_TRACKS) masif_mute ^= (uint8_t)(1u << mu.mcol);
        break;
    case MK_SEL:                                     /* the instrument page: ZOOM, 1x 2x 4x round */
        if (mu.page == MU_INST) {
            mu.zoom = (uint8_t)(mu.zoom >= 4u ? 1u : mu.zoom ? mu.zoom * 2u : 2u);
            mu_say(mu.zoom == 4u ? "ZOOM 4X" : mu.zoom == 2u ? "ZOOM 2X" : "ZOOM 1X");
        }
        break;
    case MK_PLAY: mu_play_page(); break;
    default:
        break;
    }
}
/* the buttons that are lit: PLAY while it plays, GLO and REC while they are held (REC also while there is a
 * selection), EDIT while its list is open */
static void masif_ui_leds(uint32_t *btn, uint32_t *keys, uint32_t *green)
{
    *green = 0;
    *btn = (sq.playing ? 1u << MK_PLAY : 0u) | (mu.shift ? 1u << MK_GLO : 0u) | (mu.option || mu.sel ? 1u << MK_REC : 0u) |
           (mu.pick ? 1u << MK_EDIT : 0u);
    *keys = 0;
}
#endif

/* from the main loop, as often as it goes round: an arrow held goes again, where one left off is kept */
static void masif_ui_tick(void)
{
#if MASIF_CONTROLS == 0
    if (mu.edit_pend && (int32_t)(fm1_ms - mu.edit_pend_at) >= 0) mu_edit_act();
    if (mu.arrow && (int32_t)(fm1_ms - mu.arrow_at) >= 0) {
        mu.arrow_at = fm1_ms + MU_REPEAT_NEXT;
        mu_arrow(mu.arrow - 1u, 1);
    }
#endif
#if MASIF_SONG_STORE
    if (mu.resumed) mu_keep_step();
#endif
    if (ms.dirty && (uint32_t)(fm1_ms - ms.name_at) > 5000u) mu_smp_name_keep();   /* (NAME left alone a while) */
}

/* a note key: 0 = F3 ... 26 = G5 (MIDI 53 ...). On the phrase page, in the note column, it is the cell's note */
static void masif_ui_key(uint32_t k, int down)
{
    uint32_t note = 53u + k;
    mu.input_at = fm1_ms;
    if (k > 26u) return;
    if (down) mu.keys |= 1u << k; else mu.keys &= ~(1u << k);   /* (lit while held) */
    if (!down) { masif_fm1_preview(mu.track, 0, 0, 0); return; }
    if (mu.page == MU_SAMPLER && !mu.pick) { mu_smp_hear((uint8_t)note); return; }
    if (mu.page == MU_PHRASE && mu.pcol == MP_NOTE && !mu.sel && !mu.pick) {
        masif_cell_t c = masif_song.phrase[mu.phrase % MASIF_PHRASES][mu.row];
        c.note = (uint8_t)note;
        if (c.instrument >= MASIF_INSTRUMENTS) c.instrument = mu.last_inst;
        mu.last_note = c.note; mu.last_inst = c.instrument;
        (void)masif_post_cell_edit(&masif_edits, mu.phrase, mu.row, &c);
        mu.inst = c.instrument;
    }
    masif_fm1_preview(mu.track, mu.page == MU_PHRASE ? mu.last_inst : mu.inst, (uint8_t)note, mu.last_vol);
}
