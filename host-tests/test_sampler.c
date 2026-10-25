/* The pool kept by the FM-1 and its recorder (firmware/src/masif_rec.c), and the sampler page (masif_ui.c), on the
 * host: the flash as RAM (host-tests/fake_pool.h). A take set off by its level with the pre-roll before it, one long
 * as long as the ring holds (nothing programmed while it runs), one with START PLAY (the bounce), the update record's look
 * never written, the directory as the web app reads it (its two copies, the generation), rename, delete, the room,
 * a full pool; then the page and its REC box through the buttons.
 *   cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=all \
 *      -Ifirmware/src -Ifirmware/gen -Ibuild/gen -o build/host/test_sampler host-tests/test_sampler.c && build/host/test_sampler [dir.bin] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i, j;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) if (x + i < 240u && y + j < 240u) fb[(y + j) * 240u + x + i] = p[j * w + i];
}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) if (x + i < 240u && y + j < 240u) fb[(y + j) * 240u + x + i] = c;
}
#define __attribute__(x)
#define MASIF_ENABLE_TRACKER 1
#define MASIF_SONG_STORE 1
#define MASIF_CONTROLS 0
#define SQ_NSEG 1u
#define SQ_MAXEV 8u
static uint32_t fm1_ms = 1000;
#include "gfx.c"
#include "sp_core.c"
#include "sp_seq.c"
#include "masif_tracker.c"
#include "masif_runtime.c"
#include "masif_songfile.c"
#include "masif_store.c"
#include "masif_demo.c"
#include "masif_fm1_adapter.c"
#include "fake_pool.h"
#define MU_WAVE_NAME(w) ((w) < SP_NUSER ? ui_uname[w] : "")
#include "masif_ui.c"

static uint8_t flash[0x100000];                  /* (the song store's) */
int masif_fl_read(uint32_t off, uint8_t *dst, uint32_t n) { memcpy(dst, flash + off, n); return 0; }
int masif_fl_erase(uint32_t off) { memset(flash + off, 0xFF, 4096); return 0; }
int masif_fl_write(uint32_t off, const uint8_t *src, uint32_t n) { uint32_t k; for (k = 0; k < n; k++) flash[off + k] &= src[k]; return 0; }
static const masif_store_region_t pool[1] = {{0xDA000, 0xE0000}};

static int bad;
static void check(int ok, const char *what) { printf("sampler: %-96s %s\n", what, ok ? "ok" : "FAIL"); bad += !ok; }
static void service(uint32_t n) { while (n--) mr_service(); }
static void save(void) { uint32_t k; for (k = 0; k < 2000u && mr.state == MR_SAVING; k++) mr_service(); }   /* (a page a call) */
static void until_armed(void) { mr_service(); }   /* (armed at once: nothing to erase before a take) */
/* frames of the input: a square wave at the level a, period p frames (0: silence) */
static uint32_t phase;
static void feed(uint32_t frames, int32_t a, uint32_t p)
{
    while (frames--) { mr_frame(p ? ((phase / (p / 2u)) & 1u ? -a : a) : 0); phase++; }
}
static int32_t sample_of(uint32_t slot, uint32_t i)   /* the stored sample i, 12 bit */
{
    const sp_wave_t *w = &sp_wave[KIT_NWAVE + slot];
    return (w->uflags & 8u) ? sp_adpcm_at(w->d, i) / 16 : (w->uflags & 4u) ? (int32_t)(int8_t)w->d[i] * 16 : sp_sample(w->d, i);
}
static const char *line(uint32_t r)
{
    static char s[MU_COLS + 1];
    masif_ui_draw();
    memcpy(s, mu_ch[r], MU_COLS);
    s[MU_COLS] = 0;
    return s;
}
static int has(uint32_t r, const char *what) { return strstr(line(r), what) != 0; }
static const char *shots;                        /* (argv[2]: where the pictures go) */
static void shot(const char *name)
{
    char p[300];
    FILE *f;
    uint32_t i;
    if (!shots) return;
    masif_ui_draw();
    snprintf(p, sizeof p, "%s/%s.ppm", shots, name);
    if (!(f = fopen(p, "wb"))) return;
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t v = (uint16_t)((fb[i] >> 8) | (fb[i] << 8));
        uint8_t rgb[3] = {(uint8_t)((v >> 11) * 255 / 31), (uint8_t)(((v >> 5) & 63) * 255 / 63), (uint8_t)((v & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static void audio(uint32_t blocks) { int32_t o[2 * SP_BLK]; while (blocks--) { memset(o, 0, sizeof o); sq_block(); masif_fm1_audio_block(); sp_render(o); } }
static void tick(uint32_t ms) { fm1_ms += ms; masif_ui_tick(); mr_service(); audio(1); }
static void down(uint32_t b) { masif_ui_button(b, 1); audio(1); if (b == MB_EDIT) tick(MU_COMBO_MS); }
static void up(uint32_t b) { masif_ui_button(b, 0); audio(1); }
static void press(uint32_t b) { down(b); up(b); }
static void with(uint32_t held, uint32_t b) { down(held); press(b); up(held); }

int main(int argc, char **argv)
{
    uint32_t i, k, at, longest, free0, n, ok;
    shots = argc > 2 ? argv[2] : 0;
    memset(hf_flash, 0xFF, sizeof hf_flash);
    memset(flash, 0xFF, sizeof flash);
    sq_init();
    masif_fm1_init();
    masif_store_init(pool, 1);
    masif_ui_init();
    zu_load();
    check(zu_dir.magic == 0 && mu_smp_count() == 0, "an empty flash: no directory, no samples");
    free0 = mr_free(&at, &longest);
    check(free0 == 0x31000u + 0x12000u && at == 0x93000u && longest == 0x31000u, "the room: both data rooms free, the longest the first");

    /* 1. a take set off by its level: 22 kHz, 12 bit, 2 s at the most */
    k = hf_erases;
    check(mr_arm(MR_USB, MR_LEVEL, 7, 1, 0, 2) == 0 && mr.state == MR_ARMED && hf_erases == k && mr.cap == 0x11000u,
          "ARM: armed at once, the room for 2 s kept (17 sectors), nothing erased");
    feed(4000, 50, 100);                        /* (quiet: under -48 dB) */
    check(mr.state == MR_ARMED && mr.s0 == MR_NONE, "quiet: still waiting");
    feed(2, 3000, 100);
    check(mr.state == MR_TAKE, "a sound over the level: the take begins");
    check(mr.w - mr.s0 == 660u, "with 20 ms before it (440 samples, 660 bytes)");
    feed(22050u * 2u - 2u, 3000, 100);          /* one second more */
    check(mr.state == MR_TAKE && mr.r == 0, "a second in: nothing programmed yet (the ring is not half full)");
    {
        uint32_t b1, b2, kk, g;
        masif_ui_leds(&b1, &kk, &g); fm1_ms += 62u; masif_ui_leds(&b2, &kk, &g);
        check(((b1 ^ b2) >> MK_REC & 1u) != 0, "the take running: REC blinks at 8 Hz");
        mr_stop();
        check(mr.state == MR_SAVING, "REC: the take stops");
        masif_ui_leds(&b1, &kk, &g); fm1_ms += 125u; masif_ui_leds(&b2, &kk, &g);
        fm1_ms += 125u; masif_ui_leds(&b1, &kk, &g);
        check(((b1 ^ b2) >> MK_REC & 1u) != 0, "saving: at 2 Hz");
    }
    k = hf_erases;
    service(3);
    check(mr.state == MR_SAVING && hf_hush && hf_erases == k, "saving: the sound fades out first (no erase yet)");
    service(2);
    check(hf_erases - k == 2u && hf_hush, "then a sector a turn of the main loop, the sound quiet");
    service(7);
    check(hf_erases - k == 9u && hf_hush && !hf_loud_erases, "the take's 9 sectors (not the 17 kept), all quiet");
    k = hf_programs;
    service(3);
    check(mr.state == MR_SAVING && hf_programs - k <= 3u && hf_hush, "then a page a turn of the main loop, still quiet");
    save();
    check(!hf_hush && !hf_loud_programs, "every page programmed quiet (msfu: the computer's sound blew out); the sound back");
    check(mr.state == MR_SAVED && mr.slot == 0 && zu_dir.magic == ZU_MAGIC && zu_dir.gen == 1u && zu_copy == 0,
          "saved: into slot 0, the directory's first copy, generation 1");
    n = zu_dir.s[0].n;
    check(n == 440u + 22050u && zu_dir.s[0].rate == 22050u && zu_dir.s[0].off == 0x93000u && !(zu_dir.s[0].flags & 4u),
          "the slot: 440 + 22050 samples at 22050 Hz, 12 bit, at 0x93000");
    check(!strcmp(ui_uname[0], "REC01") && sp_wave[0].n == n, "named REC01, and it plays (the pool has it)");
    for (i = 0, ok = 1; i < 440u; i++) ok &= sample_of(0, i) == ((50 + 8) >> 4) || sample_of(0, i) == ((-50 + 8) >> 4);
    check(ok, "the pre-roll: the quiet before the sound");
    for (i = 440u, ok = 1; i < n; i++) { int32_t v = sample_of(0, i); ok &= v == ((3000 + 8) >> 4) || v == ((-3000 + 8) >> 4); }
    check(ok, "the take: the sound as it came (12 bit)");
    check(mr_free(0, 0) == free0 - 0x9000u, "the room: less the take's 9 sectors");

    /* 2. a long one: 44.1 kHz, 12 bit: nothing programmed while it runs (msfo did, and crackled on the device), the
     * take what the ring holds */
    check(mr_arm(MR_MIX, MR_NOW, 0, 0, 0, 3) == 0 && mr.cap == MR_TAKE_MAX, "ARM: 44 kHz, START NOW, 3 s: what the ring holds (124 KiB)");
    until_armed();
    phase = 0;
    k = hf_programs; ok = 1;
    for (i = 0; i < 44100u * 4u && (mr.state == MR_ARMED || mr.state == MR_TAKE); i += 441u) {
        feed(441, 1000 + (int32_t)(i / 441u) * 7, 4);   /* (10 ms at a time, louder each; the main loop between) */
        if (mr.state == MR_TAKE) { service(1); ok &= hf_programs == k; }
        else service(1);
    }
    save();
    check(mr.state == MR_SAVED && mr.why == MR_WHY_FULL && mr.w - mr.s0 <= mr.cap, "the take ends by itself when its room is full (not a byte past it)");
    check(ok, "nothing programmed while the take ran");
    check(mr.state == MR_SAVED && mr.slot == 1 && zu_dir.gen == 2u && zu_copy == 1, "saved: slot 1, the other copy, generation 2");
    n = zu_dir.s[1].n;
    check(n * 3u / 2u + 3u > mr.cap - 3u && mr_bytes(n, 0) <= mr.cap, "the take fills its room");
    for (i = 0, ok = 1; i < n; i++) {
        int32_t lv = 1000 + (int32_t)(i / 441u) * 7, v = sample_of(1, i), want = ((i / 2u) & 1u ? -lv : lv);
        ok &= v == (want + 8) >> 4;
    }
    check(ok, "every sample as it came (the pre-roll's turns round the ring before it)");
    check(sp_wave[0].n && !strcmp(ui_uname[0], "REC01") && !strcmp(ui_uname[1], "REC02"), "the first is still there; the new one REC02");

    /* 3. no update record's look: "AT" at 0xF06 of a sector is never written (8 bit, 44 kHz: a byte a frame) */
    check(mr_arm(MR_USB, MR_NOW, 0, 0, 1, 1) == 0, "ARM: 8 bit");
    until_armed();
    for (i = 0; i < 0x2000u; i++) {
        uint32_t o = i & 0xFFFu, b = o == 0xF06u ? 0x41u : o == 0xF07u ? 0x54u : 0x10u;
        mr_frame((int32_t)(int8_t)b * 256);
    }
    mr_stop(); save();
    at = zu_dir.s[2].off;
    check(mr.state == MR_SAVED && (zu_dir.s[2].flags & 4u) && zu_dir.s[2].n == 0x2000u, "saved: 8 bit, 8 KiB");
    check(hf_flash[at + 0xF06u] == 0x41u && hf_flash[at + 0xF07u] == 0x55u && hf_flash[at + 0x1F07u] == 0x55u &&
          hf_flash[at + 0xF08u] == 0x10u, "0xF07 changed by its lowest bit where 0xF06 / 0xF07 would read AT (each sector)");

    /* 4. START PLAY: the bounce */
    check(mr_arm(MR_MIX, MR_PLAY, 0, 2, 0, 1) == 0, "ARM: START PLAY, 11 kHz");
    until_armed();
    feed(1000, 2000, 8);
    check(mr.state == MR_ARMED, "waits for the song");
    sq.playing = 1;
    feed(4400, 2000, 8);
    check(mr.state == MR_TAKE && mr.s0 == mr.w - 1650u, "the song plays: the take runs (no pre-roll)");
    sq.playing = 0;
    feed(4, 2000, 8);
    check(mr.state == MR_SAVING, "the song stops: so does the take");
    save();
    check(mr.state == MR_SAVED && zu_dir.s[3].rate == 11025u && zu_dir.s[3].n == 1100u, "saved at 11025 Hz: 1100 samples");

    /* 4b. 4-bit ADPCM: blocks of 128 in 68 bytes, the take on a block's start, near what came in */
    check(mr_arm(MR_USB, MR_LEVEL, 7, 1, MR_B4, 2) == 0, "ARM: ADPCM, 22 kHz, START LEVEL");
    until_armed();
    feed(3001, 50, 100);                        /* (the pre-roll: some blocks in, one begun) */
    check(mr.state == MR_ARMED, "quiet: waiting");
    {
        int64_t sig = 0, err = 0;
        uint32_t f, first, ns0 = mr.ns;          /* (3001 frames: one waits for its pair, so sample j is frames 2j-1, 2j) */
        for (f = 0; f < 44100u; f++) {          /* a 441 Hz sine at -12 dB, after the trigger */
            int32_t x = (int32_t)(8000.0 * __builtin_sin(f * 2.0 * 3.141592653589793 * 441.0 / 44100.0)) + (f ? 0 : 6000);
            mr_frame(x);
        }
        check(mr.state == MR_TAKE && mr.s0 % SP_ADBYTES == 0u && mr.w - mr.s0 >= 68u * 4u, "the take starts on a block, the pre-roll in whole blocks");
        first = mr.s0 / SP_ADBYTES * SP_ADB;     /* (the samples before the take) */
        mr_stop(); save();
        k = mr_slot_now();
        check(mr.state == MR_SAVED && (zu_dir.s[k].flags & 12u) == 8u && sp_wave[k].uflags == 8u, "saved with flags bit 3 (ADPCM)");
        n = zu_dir.s[k].n;
        check(n == mr.ns - first && sp_adpcm_bytes(n) <= mr.cap, "its samples: what the blocks hold, within the room");
        check(mr_bytes(n, MR_B4) * 10u < mr_bytes(n, MR_B12) * 4u, "under 40 % of 12 bit's bytes");
        for (i = 0, ok = 1; i < n; i++) ok &= sp_adpcm_at(sp_wave[k].d, i) == sp_adpcm_voice(0, sp_wave[k].d, i);
        for (i = n; i-- > 0;) ok &= sp_adpcm_at(sp_wave[k].d, i) == sp_adpcm_voice(1, sp_wave[k].d, i);
        check(ok, "a voice's blocks (on and back) give what the picture's decoder gives");
        if (argc > 1) {                         /* (for the web app's unpackAdpcm: n, the bytes, the 16-bit values) */
            char fn[300];
            FILE *f;
            snprintf(fn, sizeof fn, "%s.adpcm", argv[1]);
            if ((f = fopen(fn, "wb"))) {
                fwrite(&n, 4, 1, f);
                fwrite(sp_wave[k].d, 1, sp_adpcm_bytes(n), f);
                for (i = 0; i < n; i++) { int16_t v = (int16_t)sp_adpcm_at(sp_wave[k].d, i); fwrite(&v, 2, 1, f); }
                fclose(f);
            }
        }
        {                                       /* the sine's part (from 2 ms after the trigger): its error */
            for (i = ns0 - first + 50u; i < n; i++) {
                uint32_t fr = (i + first - ns0) * 2u - 1u;
                int32_t a = (int32_t)(8000.0 * __builtin_sin(fr * 2.0 * 3.141592653589793 * 441.0 / 44100.0));
                int32_t b = (int32_t)(8000.0 * __builtin_sin((fr + 1u) * 2.0 * 3.141592653589793 * 441.0 / 44100.0));
                int32_t x = (a + b) >> 1, y = sp_adpcm_at(sp_wave[k].d, i);
                sig += (int64_t)x * x; err += (int64_t)(y - x) * (y - x);
            }
            printf("sampler: ADPCM signal / error %.1f dB\n", 10.0 * __builtin_log10((double)sig / (double)(err ? err : 1)));
            check(err * 1000 < sig, "the sine comes back within -30 dB");
        }
    }

    /* 5. rename, delete; the directory as written is what the web app reads (its two copies, the newest counts) */
    check(mr_rename(1, "LOOP A\0\0") == 0 && !strcmp(ui_uname[1], "LOOP A") && sp_wave[1].n == zu_dir.s[1].n, "rename: LOOP A");
    check(mr_delete(0) == 0 && !sp_wave[0].n && sp_wave[1].n && sp_wave[2].n, "delete: slot 0 gone, the others stay");
    for (k = 0, n = 0; k < SP_NUSER; k++) if (sp_wave[k].n) n += (mr_bytes(zu_dir.s[k].n, mr_bits_of(zu_dir.s[k].flags)) + 0xFFFu) & ~0xFFFu;
    check(mr_free(0, 0) == free0 - n, "its room is free again (what is left: the others' sectors)");
    {
        uint32_t g = zu_dir.gen;
        zu_load();
        check(zu_dir.gen == g && !strcmp(ui_uname[1], "LOOP A"), "read again at the start: the same");
    }
    if (argc > 1) {                             /* (for the web app's parseDir: tests/masif_web_test.mjs) */
        FILE *f = fopen(argv[1], "wb");
        if (f) { fwrite(hf_flash + 0xD8000u, 1, 0x2000u, f); fclose(f); }
    }

    /* 6. no room, a full pool */
    (void)mr_free(&at, &longest);
    check(mr_arm(MR_USB, MR_NOW, 0, 1, 0, 60) == 0 && mr.cap == longest && mr.at == at, "60 s wanted: what the longest free run holds");
    mr_stop();
    check(mr.state == MR_IDLE, "OPTION before the take: nothing kept");
    for (k = 0; k < SP_NUSER; k++) if (!sp_wave[k].n) { sp_wave[k].n = 2; }
    check(mr_arm(MR_USB, MR_NOW, 0, 1, 0, 1) == MR_WHY_ROOM, "24 samples: no place for another");
    zu_load();

    /* 7. the page through the buttons: rows as the instrument page's; its sample the take just made (none at the
     * start), REC records, the note keys hear, OPTION + EDIT clears the sampler (the sample stays in the pool) */
    mu.page = MU_INST; mu.all = 1;
    with(MB_SHIFT, MB_DOWN);
    check(mu.page == MU_SAMPLER && has(0, "SAMPLER") && line(0)[18] == 'R', "SHIFT + DOWN from the instrument: the sampler (R)");
    check(mr.listen, "the IN meter listens while the page is open");
    check(has(7, "SAMPLE") && has(7, "NONE") && has(8, "SOURCE") && has(13, "MAX") && has(15, "POOL") && has(15, "/24"),
          "the rows (no sample yet), the pool's count and room");
    shot("sampler");
    press(MB_EDIT);
    check(mu.pick == PK_NONE, "EDIT on SAMPLE: no pool here");
    for (k = 0; k < 6u; k++) press(MB_DOWN);
    check(ms.row == MS_MAX, "down to MAX");
    with(MB_EDIT, MB_LEFT);
    check(ms.max_s == 9, "EDIT + LEFT: 9 s");
    press(MB_PLAY);
    tick(1);
    check(mr.state == MR_ARMED && has(17, "WAITING FOR SOUND"), "REC (the button): armed at once, waiting for the sound");
    with(MB_EDIT, MB_LEFT);
    check(ms.max_s == 9, "the settings stay as they are while armed");
    {
        uint32_t b1, b2, kk, g;
        masif_ui_leds(&b1, &kk, &g);
        fm1_ms += 125u;
        masif_ui_leds(&b2, &kk, &g);
        check(((b1 ^ b2) >> MK_REC & 1u) != 0 && g == 1u, "the REC button's LED blinks red while armed (4 Hz)");
    }
    feed(10, 20000, 10);
    check(mr.state == MR_TAKE && has(17, "REC") && has(16, "#"), "a sound: recording, the meter up");
    shot("sampler-rec");
    feed(22050, 20000, 10);
    press(MB_PLAY);
    tick(1);
    for (k = 0; k < 2000u && mr.state == MR_SAVING; k++) tick(1);
    check(mr.state == MR_SAVED && has(17, "SAVED AS") && has(17, "REC01"), "REC again: saved as REC01");
    check(!strcmp(MU_WAVE_NAME(KIT_NWAVE + ms.slot), "REC01") && has(7, "REC01"), "the new one is the sampler's");
    audio(2);
    masif_ui_key(12, 1);
    check(masif_preview.pending && masif_preview.instrument == MASIF_AUDITION && masif_preview.note == 65u &&
          masif_audition.sample == KIT_NWAVE + ms.slot, "a note key: the sample at its pitch");
    audio(2);
    masif_ui_key(12, 0);
    audio(2);
    for (k = 0; k < 8u; k++) press(MB_UP);
    check(ms.row == MS_SAMPLE && ms.schar == 0, "up to SAMPLE, on its number");
    {
        char want[9];
        const char *cs = MU_CHARSET;
        uint32_t c = 0;
        memcpy(want, MU_WAVE_NAME(KIT_NWAVE + ms.slot), 9);
        while (cs[c] && cs[c] != (want[3] ? want[3] : ' ')) c++;
        want[3] = cs[(c + 1u) % MU_NCHARSET];
        press(MB_RIGHT); press(MB_RIGHT); press(MB_RIGHT); press(MB_RIGHT);
        down(MB_EDIT);
        check(mu.pick == PK_CHAR, "RIGHT along the name, EDIT held: the characters");
        press(MB_RIGHT);
        up(MB_EDIT);
        check(ms.name[3] == want[3] && ms.schar == 5u && ms.dirty, "RIGHT in them, EDIT let go: the next character, the cursor on");
        shot("sampler-name");
        press(MB_DOWN);
        check(!ms.dirty && !strcmp(MU_WAVE_NAME(KIT_NWAVE + ms.slot), want), "off the row: renamed in the pool");
        fm1_ms += 5000u;                        /* (its message gone from the bottom row) */
        press(MB_UP);
    }
    check(ms.row == MS_SAMPLE && ms.schar == 0 && has(17, "OPT+EDIT: CLEAR"), "back on its number: what OPTION + EDIT does");
    k = ms.slot; n = mu_smp_count();
    with(MB_OPTION, MB_EDIT);
    check(ms.slot == MS_NONE && mu_smp_has(k) && mu_smp_count() == n && has(7, "NONE"), "OPTION + EDIT: the sampler cleared, the sample still in the pool");
    shot("sampler-cleared");
    with(MB_SHIFT, MB_UP);
    check(mu.page == MU_INST && !mr.listen, "SHIFT + UP: back on the instrument page, the meter off");

    /* 8. the instrument's pool: it stays open; EDIT takes, OPTION alone leaves, OPTION + EDIT twice deletes */
    mu.irow = 0; mu.inum = 0; mu.itab = 0; mu.all = 1;
    for (k = 0; k < 20u && !(mu_par(mu_par_row(), &mu_pick_par) && mu_pick_par.kind == MK_SAMPLE); k++) press(MB_DOWN);
    press(MB_EDIT);
    check(mu.pick == PK_POOL, "EDIT on SAMPLE, let go: the pool stays open");
    shot("pool");
    n = mu.pick_n;
    press(MB_DOWN);
    press(MB_OPTION);
    check(mu.pick == PK_NONE && mu_pool_cancel, "OPTION alone: closed, nothing taken");
    press(MB_EDIT);
    press(MB_DOWN);
    k = mu_pool_wave(mu.pick_cur);
    press(MB_EDIT);
    audio(2);
    check(mu.pick == PK_POOL && mu_inst()->sample == k, "EDIT again: the instrument plays the marked one, the pool still open");
    k = mu_pool_wave(mu.pick_cur) - KIT_NWAVE;
    with(MB_OPTION, MB_EDIT);
    check(mu.pick == PK_POOL && mr_has(k) && has(17, "AGAIN"), "OPTION + EDIT: asks again (the list stays)");
    shot("pool-delete");
    with(MB_OPTION, MB_EDIT);
    check(mu.pick == PK_POOL && !mr_has(k) && mu.pick_n == n - 1u, "again: deleted, the list one shorter");
    press(MB_OPTION);

    /* 9. FX does nothing; the buttons without a use dark */
    press(MB_BANK);
    check(mu.bank == 0 && !mu.bank_held, "FX: nothing");
    {
        uint32_t b, kk, g;
        masif_ui_leds(&b, &kk, &g);
        check(!(b & (1u << MK_OCTDN | 1u << MK_OCTUP | 1u << MK_FX | 1u << MK_ENV | 1u << MK_LFO | 1u << MK_SEQ)) &&
              (b & 1u << MK_SEL) && (b & 1u << MK_GLO), "OCT -, OCT +, FX, ENV, LFO, SEQ dark; the others lit");
    }

    /* 10. the project page's TEST: the load test, the pool's samples on four filtered tracks, a note every row */
    mu.page = MU_PROJECT; mu.prow = MJ_TEST; mu.all = 1;
    check(has(11, "TEST") && mu_pool_count() > 0u, "PROJECT: TEST, and samples in the pool");
    press(MB_EDIT);
    {
        static masif_song_t copy;
        uint32_t t, on = 0;
        copy = masif_song;
        check(masif_song.tempo == 160u && sq.bpm10 == 1600u && !strcmp(mu.sname, "LOADTEST") &&
              masif_song.instrument[0].sample == mu_pool_wave(0) && masif_song.instrument[3].filter == MASIF_FILTER_LP &&
              masif_song.song[0][3] == 3u && masif_song.song[1][0] == MASIF_NONE && masif_song_sanitize(&copy) == 0u,
              "EDIT: the load test in memory (160 BPM, LOADTEST, the pool's samples, sound as it is)");
        mu.page = MU_SONG; mu.song_row = 0; mu.track = 0;
        press(MB_PLAY);
        for (k = 0; k < 3000u; k++) {
            audio(1);
            for (t = 0; t < MASIF_TRACKS; t++) on |= (uint32_t)(sp_ch[SP_NCH + t].on != 0) << t;
        }
        check(sq.playing && on == 15u, "PLAY: all four tracks sound");
        press(MB_PLAY); audio(4);
        check(!sq.playing, "and stop");
    }

    printf(bad ? "MASIF sampler tests: %d FAILED\n" : "MASIF sampler tests: PASS\n", bad);
    return bad != 0;
}
