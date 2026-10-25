/* usb.c's USB audio (MASIF's build: FELUCCA_UAC 1, 44.1 kHz only, with the computer's audio in) on the host:
 * the configuration descriptor walks as a host would walk it, and the rings hold over a minute of the two clocks
 * drifting (the I2S at 44,117.6 Hz, the host at 44,100): the FM-1 to the computer (uac_render_start, uac_tap,
 * uac_packet), and the computer to the FM-1 (uo_put, uo_read). The SIE is never touched.
 *   cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-int-to-pointer-cast -fsanitize=address,undefined -fno-sanitize-recover=all -Ifirmware/hal -o build/host/test_usb_audio host-tests/test_usb_audio.c && build/host/test_usb_audio */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define HALF_FRAMES 128u
#define FELUCCA_OTA 0
#define FELUCCA_CDC 0
#define FELUCCA_UAC 1
#define FELUCCA_UART 0
#define FELUCCA_UAC_48K 0                       /* (as zp12.c) */
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#include "../firmware/src/usb.c"
static void uo_rec(uint32_t at, uint32_t n) { (void)at; (void)n; }

static int bad;
static void check(int ok, const char *what) { printf("usb audio: %-96s %s\n", what, ok ? "ok" : "FAIL"); bad += !ok; }

/* AppleUSBAudio-273.4.1 AUAConfigurationDictionary::parseConfigurationDescriptor (macOS up to 15), reduced to its
 * walk of the AC collection, after Felucca's tests/usb_desc_test.c aua_parses_as (#67): the baInterfaceNr list is
 * walked with bInCollection's count; at a MIDI streaming interface its entry is removed while the walk goes on, at
 * an audio streaming interface the walk stops at the match; a look-up past the shortened list ends the parse.
 * 1: audio streaming interface `want` is reached */
static int aua_parses_as(const uint8_t *c, uint32_t n, uint8_t want)
{
    uint8_t list[8], len = 0, num = 0;
    uint32_t off, i, k;
    int have_ac = 0;
    for (off = 0; off < n && c[off] >= 2; off += c[off]) {
        const uint8_t *d = c + off;
        if (d[1] == 4 && d[5] == 1 && d[6] == 1)
            have_ac = 2;
        else if (d[1] == 0x24 && d[2] == 1 && have_ac == 2) {
            num = d[7];
            for (len = 0; len < num && len < sizeof list; len++) list[len] = d[8 + len];
            have_ac = 1;
        } else if (d[1] == 4 && d[5] == 1 && d[6] == 3) {
            for (i = 0; i < num; i++) {
                if (i >= len) return 0;
                if (list[i] == d[2]) { for (k = i; k + 1 < len; k++) list[k] = list[k + 1]; len--; }
            }
        } else if (d[1] == 4 && d[5] == 1 && d[6] == 2 && have_ac == 1 && d[3] == 0) {
            for (i = 0; i < num; i++) {
                if (i >= len) return 0;
                if (list[i] == d[2]) { if (d[2] == want) return 1; break; }
            }
        }
    }
    return 0;
}

/* the descriptor as a host reads it: every length in bounds, the interfaces and endpoints */
static void descriptors(void)
{
    const uint8_t *d = CFG_DESC;
    uint32_t n = sizeof CFG_DESC, i = 0, ifs = 0, eps = 0, ok = 1, ep84 = 0, ep03 = 0, rates = 0, k;
    uint8_t coll[8], ncoll = 0, as_if[8], nas = 0, ms_if = 0xFF;
    check(d[2] + 256u * d[3] == n && n == CFG_LEN, "wTotalLength is the descriptor's length");
    while (i < n) {
        uint32_t len = d[i];
        if (len < 2u || i + len > n) { ok = 0; break; }
        if (d[i + 1] == 4 && d[i + 3] == 0) {                      /* (an interface's first setting) */
            ifs++;
            if (d[i + 5] == 1 && d[i + 6] == 2 && nas < sizeof as_if) as_if[nas++] = d[i + 2];
            if (d[i + 5] == 1 && d[i + 6] == 3) ms_if = d[i + 2];
        }
        if (d[i + 1] == 0x24 && d[i + 2] == 1 && i < 30u)            /* (the AC header: its collection) */
            for (ncoll = 0; ncoll < d[i + 7] && ncoll < sizeof coll; ncoll++) coll[ncoll] = d[i + 8 + ncoll];
        if (d[i + 1] == 5) {
            eps++;
            if (d[i + 2] == 0x84 && (d[i + 3] & 3u) == 1u && d[i + 4] + 256u * d[i + 5] >= 46u * 4u) ep84 = 1;
            if (d[i + 2] == 0x03 && (d[i + 3] & 3u) == 1u && d[i + 4] + 256u * d[i + 5] >= 46u * 4u) ep03 = 1;
        }
        if (d[i + 1] == 0x24 && d[i + 2] == 2 && len == 11u && d[i + 7] == 1 && d[i + 8] == 0x44 && d[i + 9] == 0xAC)
            rates++;                                                   /* (type I, 44100 alone) */
        i += len;
    }
    check(ok && i == n, "every descriptor's length in bounds, they end where it ends");
    check(ifs == d[4] && ifs == 4u, "four interfaces: audio control, audio to the computer, audio from it, MIDI");
    check(eps == 4u && ep84 && ep03, "MIDI's two bulk endpoints, EP4 IN iso (46 frames), EP3 OUT iso (46 frames)");
    check(rates == 2u, "both streams 44.1 kHz alone (macOS took no 48 kHz)");
    for (k = 0; k < nas && k < ncoll && coll[k] == as_if[k] && as_if[k] == k + 1u; k++) {}
    check(nas == 2u && ncoll == 2u && k == 2u && ms_if == 3u,
          "the AC collection: the two audio streams alone, interfaces 1 and 2 right after it; MIDI 3, not in it");
    check(DEV_DESC[12] == 0x5C && DEV_DESC[13] == 0x03, "bcdDevice 3.5C (hosts cache descriptors by it)");
    check(aua_parses_as(d, n, UAC_AS_IF) && aua_parses_as(d, n, UO_IF),
          "macOS <= 15's AppleUSBAudio walk reaches both audio streaming interfaces (MIDI not in the collection)");
}

/* the FM-1 to the computer: a render of HALF_FRAMES every 128 / 44117.6 s, a packet every 1 ms */
static void to_computer(int r48)
{
    static int32_t blk[2u * 32u];
    uint32_t t_us = 0, next_render = 0, next_pkt = 0, i, sent = 0, made = 0, pk = 0;
    memset(&uac, 0, sizeof uac);
    ua_w = ua_r = 0;
    uac.alt = 1; uac.flowing = 1; usb.config = 1; uac.r48 = (uint8_t)r48;
    uac.fill_lo = 0xFFFFFFFFu;
    for (i = 0; i < 2u * 32u; i++) blk[i] = 1000;
    while (t_us < 60000000u) {                                   /* a minute, in 10 us steps */
        if (t_us >= next_render) {
            uint32_t b;
            uac_render_start();
            for (b = 0; b < HALF_FRAMES; b += 32u) uac_tap(blk, 32u);
            made += HALF_FRAMES;
            next_render = (uint32_t)((uint64_t)made * 10000000u / 441176u);   /* (44,117.6 Hz) */
        }
        if (t_us >= next_pkt) {
            static uint32_t d[UA_MAXF];
            sent += uac_packet(d);
            pk++;
            next_pkt += 1000u;
        }
        t_us += 10u;
    }
    printf("usb audio: %s: %u packets, %u frames; underruns %u, overruns %u, fill %u..%u, +1 %u, -1 %u\n",
           r48 ? "48 kHz" : "44.1 kHz", pk, sent, uac.underruns, uac.overruns, uac.fill_lo, uac.fill_hi, uac.adj_up, uac.adj_down);
    check(uac.underruns == 0 && uac.overruns == 0, r48 ? "48 kHz to the computer: a minute, no frame repeated or dropped"
                                                       : "44.1 kHz to the computer: a minute, no frame repeated or dropped");
    check(uac.fill_lo >= 40u && uac.fill_hi < UA_N - HALF_FRAMES - UA_MAXF, "the ring's fill stays in its band");
}

/* the computer to the FM-1: a packet every 1 ms (44 / 45 frames at 44,100), a block read every 128 / 44117.6 s */
static void from_computer(void)
{
    static int16_t pcm[2u * 46u];
    static int32_t o[2u * HALF_FRAMES];
    uint32_t t_us = 0, next_blk = 0, next_pkt = 0, blocks = 0, acc = 0, i, wrong = 0;
    uo_w = uo_r = 0; uo_go = 0; uo_under = uo_over = 0; uo_alt = 1;
    for (i = 0; i < 2u * 46u; i++) pcm[i] = (int16_t)(i & 1u ? -2000 : 3000);
    while (t_us < 60000000u) {
        if (t_us >= next_pkt) {
            uint32_t n = 44u;
            acc += 100u;
            if (acc >= 1000u) { acc -= 1000u; n++; }
            uo_put(pcm, n);
            next_pkt += 1000u;
        }
        if (t_us >= next_blk) {
            memset(o, 0, sizeof o);
            uo_read(o, HALF_FRAMES);
            if (uo_go) for (i = 0; i < HALF_FRAMES; i++) wrong += o[2u * i] != 3000 || o[2u * i + 1u] != -2000;
            blocks++;
            next_blk = (uint32_t)((uint64_t)blocks * HALF_FRAMES * 10000000u / 441176u);
        }
        t_us += 10u;
    }
    printf("usb audio: from the computer: %u blocks; cut short %u, frames dropped %u\n", blocks, uo_under, uo_over);
    check(uo_under <= 1u && uo_over == 0 && !wrong, "the computer's audio: a minute, no gap (after the first), nothing dropped, L and R in place");
    uo_alt = 0;
    memset(o, 0, sizeof o);
    uo_read(o, HALF_FRAMES);
    for (i = 0; i < 2u * HALF_FRAMES && !o[i]; i++) {}
    check(i == 2u * HALF_FRAMES && uo_r == uo_w, "the host stops (alt 0): silence, the ring let go");
}

int main(void)
{
    descriptors();
    to_computer(0);
    from_computer();
    puts(bad ? "MASIF USB audio tests: FAIL" : "MASIF USB audio tests: PASS");
    return bad ? 1 : 0;
}
