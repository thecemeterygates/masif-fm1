/* MASIF: what the tracker adds to the audio interrupt's work, measured on the host. Not the chip's time (no
 * FPU there, another compiler): the ratio between the cases is what carries over, roughly. The real core with
 * its effects and the factory kit (build/gen/zp12_kit.h), every voice kept sounding by a hit every 64 blocks.
 *   cc -std=gnu99 -O2 -Wall -Wextra -Wno-unused-function -Ifirmware/src -Ibuild/gen -o build/host/bench_voices \
 *      host-tests/bench_voices.c && build/host/bench_voices */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#define __attribute__(x)
#define MASIF_ENABLE_TRACKER 1
#define SP_WITH_FX 1
#include "sp_core.c"
#include "sp_fx.c"
#include "zp12_kit.h"
#include "sp_seq.c"
#include "masif_tracker.c"
#include "masif_runtime.c"
#include "masif_demo.c"
#include "masif_fm1_adapter.c"

static volatile int32_t sink;
/* drums: 8 = a pad on every channel; tracks: 0-4 voices; semis: their pitch (24: at the step cap, the most
 * work a voice is); song: 1 = the demo song played by the runtime at 240 BPM, 2 = every track an effect a tick */
static double run(unsigned drums, unsigned tracks, int semis, int song)
{
    static const uint8_t pad_of_ch[8] = {0, 1, 4, 3, 6, 2, 8, 10};   /* KICK SNARE HAT CLAP TOM RIM CRASH COWBL */
    const uint32_t blocks = 60000;                  /* 43.5 s of audio */
    uint32_t b, t;
    clock_t c0;
    memset(sp_ch, 0, sizeof sp_ch);
    sq_init();
    sq.bpm10 = 2400;
    fx_bpm10 = 2400;
    masif_fm1_init();
    if (song == 1) (void)masif_demo_load(&masif_song);
    masif_song.instrument[27].sample = 19;          /* VIBES */
    for (t = 0; t < MASIF_TRACKS; t++) {            /* track t: phrase t, round and round */
        (void)masif_set_chain_step(&masif_song, (uint8_t)t, 0, (uint8_t)t, 0);
        (void)masif_set_song_cell(&masif_song, 0, (uint8_t)t, (uint8_t)t);
    }
    if (song == 2)
        for (t = 0; t < MASIF_TRACKS; t++)
            for (b = 0; b < MASIF_ROWS; b++) {
                masif_cell_t c = {(uint8_t)(b % 4u ? 0 : 60), (uint8_t)(b % 4u ? 0xFF : 27), 0xFF,
                                  (uint8_t)(t & 1u ? MASIF_FX_VIBRATO : MASIF_FX_RETRIGGER), (uint8_t)(t & 1u ? 0xFF : 0x01), 0, 0};
                (void)masif_set_cell(&masif_song, (uint8_t)t, (uint8_t)b, &c);
            }
    if (song == 1 || song == 2) sq_post(RQ_PLAY, 0, 0);
    c0 = clock();
    for (b = 0; b < blocks; b++) {
        int32_t o[2 * SP_BLK];
        if (b % 64u == 0u) {
            for (t = 0; t < drums; t++) sp_trigger(pad_of_ch[t], 127);
            if (!song || song >= 3) {
                static const uint8_t adsr[4] = {0, 110, 0, 0}, send[3] = {30, 20, 40};
                for (t = 0; t < tracks; t++) {      /* VIBES, with all three sends */
                    sp_trigger_track_wave(t, 19, 0, sp_wave[19].n, 0, 0, 0, semis * 100, adsr, song == 3 || song == 5 ? 1u + t % 3u : 0u, 90, 60, send);
                    sp_level_track(t, 113, -10);
                    if (song >= 4) { static const int8_t eq[3] = {20, -10, 15}; sp_eq_track(t, eq); }
                }
            }
        }
        memset(o, 0, sizeof o);
        sq_block();
        if (song == 1 || song == 2) masif_fm1_audio_block();
        sp_render(o);
        sink += o[0] + o[2 * SP_BLK - 1];
    }
    return (double)(clock() - c0) / CLOCKS_PER_SEC;
}

int main(void)
{
    uint32_t i;
    double base, x;
    for (i = 0; i < KIT_NWAVE; i++) {
        sp_wave[i].d = KIT_DATA + KIT_WAVE[i].off;
        sp_wave[i].n = KIT_WAVE[i].n;
        sp_wave[i].rate = 26040;
    }
    for (i = 0; i < SP_NSOUND; i++) sp_sound[i] = KIT_PADS[i];
    (void)run(8, 0, 0, 0);                          /* (warm) */
    base = run(8, 0, 0, 0);
    printf("8 drum channels busy (zp12 as it is)                 %6.3f s   100 %%\n", base);
    x = run(8, 4, 0, 0);  printf("+ 4 tracker voices at the sample's own pitch         %6.3f s   %3.0f %%\n", x, 100.0 * x / base);
    x = run(8, 4, 24, 0); printf("+ 4 tracker voices two octaves up (the step cap)     %6.3f s   %3.0f %%\n", x, 100.0 * x / base);
    x = run(0, 4, 0, 0);  printf("4 tracker voices alone (the drum machine silent)     %6.3f s   %3.0f %%\n", x, 100.0 * x / base);
    x = run(0, 4, 0, 3);  printf("4 tracker voices alone, each through a filter       %6.3f s   %3.0f %%\n", x, 100.0 * x / base);
    x = run(0, 4, 0, 4);  printf("4 tracker voices alone, each through its EQ          %6.3f s   %3.0f %%\n", x, 100.0 * x / base);
    x = run(0, 4, 0, 5);  printf("4 tracker voices alone, each through filter and EQ   %6.3f s   %3.0f %%\n", x, 100.0 * x / base);
    x = run(8, 0, 0, 1);  printf("+ the demo song at 240 BPM                           %6.3f s   %3.0f %%\n", x, 100.0 * x / base);
    x = run(8, 0, 0, 2);  printf("+ 4 tracks, an effect every tick, at 240 BPM         %6.3f s   %3.0f %%\n", x, 100.0 * x / base);
    x = run(0, 0, 0, 0);  printf("nothing sounding (the effects and the mix alone)     %6.3f s   %3.0f %%\n", x, 100.0 * x / base);
    return 0;
}
