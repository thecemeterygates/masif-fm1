/* USB audio out of the host (the FM-1 as a UAC1 speaker): packets of 16-bit stereo frames -> a ring -> mixed into the
 * master block (S5; S6 records the same ring). Kept apart from usb.c so the emulator, which has no SIE, builds the
 * same ring and mix (dinghy_wasm.c emu_uo_feed). The host's 44,100 Hz against our 44,117.6 drifts one frame per
 * ~2,500: the reader keeps the fill left after a block in UO_LO..UO_HI by repeating or dropping one frame. */
#define UO_N 512u                /* ring, frames (16-bit L | R << 16); a power of two */
#define UO_LO 48u                /* a packet (46 frames) and more stays after a block */
#define UO_HI 128u
_Static_assert(UO_HI + HALF_FRAMES + 46u < UO_N, "the ring holds the band, a block and a packet");
static uint32_t uo_ring[UO_N];
static volatile uint32_t uo_w, uo_r;
static volatile uint8_t uo_alt;                         /* the host streams (SET_INTERFACE alt 1) */
static uint8_t uo_go;                                   /* primed: the reader plays the ring */
static void uo_rec(uint32_t at, uint32_t n);          /* sound.c: S6's take reads the frames the reader consumes */
static uint32_t uo_under, uo_over;                      /* blocks cut short (ring empty), frames dropped (ring full) */

static void uo_put(const int16_t *pcm, uint32_t frames)    /* the producer: the packet taker */
{
    uint32_t i;
    for (i = 0; i < frames; i++) {
        if (uo_w - uo_r >= UO_N) {
            uo_over += frames - i;
            return;
        }
        uo_ring[uo_w % UO_N] = (uint16_t)pcm[2u * i] | (uint32_t)(uint16_t)pcm[2u * i + 1u] << 16;
        RING_PUBLISH();
        uo_w++;
    }
}

static void uo_read(int32_t *o, uint32_t n)             /* the consumer: adds n frames to o (L R, Q15) */
{
    uint32_t avail = uo_w - uo_r, adv = n, i;
    if (!uo_alt) {
        uo_r = uo_w;
        uo_go = 0;
        return;
    }
    if (!uo_go) {
        if (avail < n + UO_LO)
            return;
        uo_go = 1;
        if (avail - n > UO_HI)                          /* a block is 256 frames of arrival: start mid-band, */
            uo_r = uo_w - n - (UO_LO + UO_HI) / 2u, avail = n + (UO_LO + UO_HI) / 2u;   /* not a frame a block down */
    }
    if (avail < n) {                                    /* underrun: silence, prime again */
        uo_under++;
        uo_go = 0;
        return;
    }
    if (avail - n < UO_LO)
        adv = n - 1u;                                   /* the last frame plays twice */
    else if (avail - n > UO_HI)
        adv = n + 1u;                                   /* one frame dropped */
    for (i = 0; i < n; i++) {
        uint32_t f = uo_ring[(uo_r + (i < adv ? i : adv - 1u)) % UO_N];
        o[2u * i] += (int16_t)f;
        o[2u * i + 1u] += (int16_t)(f >> 16);
    }
    uo_rec(uo_r, adv);
    uo_r += adv;
}
