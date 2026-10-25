/* MASIF demo song, rendered on the host as the device would play it: the real sound core with its effects, the
 * factory kit (build/gen/zp12_kit.h: any build, or python3 tools/gen_kit.py, makes it), the drum sequencer with
 * a plain beat, the tracker with the demo song (masif_demo.c), the audio ISR's block order and its master stage
 * at the MASTER knob's middle (zp12.c fm1_alnk0_irq). To hear the demo, and its level, before a flash.
 *   cc -std=gnu99 -O2 -Wall -Wextra -Wno-unused-function -Ifirmware/src -Ibuild/gen -o build/host/render_demo \
 *      host-tests/render_demo.c && build/host/render_demo build/host/masif-demo.wav [bpm] [nodrums]   */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define __attribute__(x)
#define MASIF_ENABLE_TRACKER 1
#define MASIF_DEMO_SONG 1
#define SP_WITH_FX 1
#include "sp_core.c"
#include "sp_fx.c"
#include "zp12_kit.h"
#include "sp_seq.c"
#include "masif_tracker.c"
#include "masif_runtime.c"
#include "masif_demo.c"
#include "masif_fm1_adapter.c"

static void put32(FILE *f, uint32_t v) { fputc((int)(v & 255), f); fputc((int)(v >> 8 & 255), f); fputc((int)(v >> 16 & 255), f); fputc((int)(v >> 24), f); }

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/host/masif-demo.wav";
    uint32_t bpm = argc > 2 ? (uint32_t)atoi(argv[2]) : 100u, drums = !(argc > 3 && !strcmp(argv[3], "nodrums"));
    uint32_t i, t, frames = 0, knee = 0, clip = 0, sounded = 0;
    int32_t peak = 0;
    FILE *f;
    if (bpm < 40u || bpm > 240u) { fprintf(stderr, "bpm 40..240\n"); return 2; }
    for (i = 0; i < KIT_NWAVE; i++) {                   /* as zp12_main */
        sp_wave[i].d = KIT_DATA + KIT_WAVE[i].off;
        sp_wave[i].n = KIT_WAVE[i].n;
        sp_wave[i].rate = 26040;
    }
    for (i = 0; i < SP_NSOUND; i++)
        sp_sound[i] = KIT_PADS[i];
    sq_init();
    sq.bpm10 = (uint16_t)(bpm * 10u);
    fx_bpm10 = sq.bpm10;
    masif_fm1_init();
    sq_seg[0].bars = 1;
    if (drums)
        for (t = 0; t < 384u; t += 48u) {               /* kick on 1 and 3 (and the "and" of 3), snare on 2 and 4, hats */
            if (t % 192u == 0u || t == 240u) sq_post(RQ_STEP, 0, t);
            if (t % 192u == 96u) sq_post(RQ_STEP, 1, t);
            sq_post(RQ_STEP, 4, t);
            sq_block();                                 /* (the request ring is short: taken as they come) */
        }
    sq_post(RQ_PLAY, 0, 0);
    f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0", 1, 24, f);
    put32(f, SP_FS); put32(f, SP_FS * 4u); fwrite("\x04\0\x10\0data\0\0\0\0", 1, 12, f);
    /* the song once and its first step again (the loop), then STOP and the tails */
    while (sq.playing || frames < SP_FS) {
        int32_t o[2 * SP_BLK];
        if (sq.playing && sq_ticks >= (MASIF_DEMO_STEPS + 1u) * 64u * 24u) { sq_post(RQ_STOP, 0, 0); frames = 0; }
        memset(o, 0, sizeof o);
        sq_block();
        masif_fm1_audio_block();
        sp_render(o);
        for (t = 0; t < MASIF_TRACKS; t++) sounded |= (uint32_t)sp_ch[SP_NCH + t].on << t;
        for (i = 0; i < 2u * SP_BLK; i++) {
            int32_t v = ((int32_t)(((int64_t)o[i] * 2048) >> 12)) >> 1;   /* sp_master's input to the knee */
            int16_t s = (int16_t)sp_master(o[i], 2048);
            if (v < 0) v = -v;
            if (v > peak) peak = v;
            knee += v > 24576;
            clip += v > 24576 + (768 << 5);
            fwrite(&s, 2, 1, f);
        }
        frames += SP_BLK;
        if (ftell(f) > 200000000L) break;
    }
    i = (uint32_t)ftell(f) - 44u;
    fseek(f, 4, SEEK_SET); put32(f, i + 36u);
    fseek(f, 40, SEEK_SET); put32(f, i);
    fclose(f);
    printf("%s: %.1f s at %u BPM, drums %s, tracks sounded %s%s%s%s\n", path, i / 4.0 / SP_FS, (unsigned)bpm,
           drums ? "on" : "off", sounded & 1u ? "1" : "-", sounded & 2u ? "2" : "-", sounded & 4u ? "3" : "-", sounded & 8u ? "4" : "-");
    printf("peak before the master's knee %d of 32767 (knee at 24576): %u samples in the knee, %u at its limit\n",
           (int)peak, (unsigned)knee, (unsigned)clip);
    return sounded == 15u ? 0 : 1;
}
