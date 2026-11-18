/* SPDX-License-Identifier: GPL-3.0-only
 * auto-chre: the effects.
 *   the send (Envío FX of each track): a feedback comb filter (metallic, pitched resonance), then a tempo-synced
 *     ping-pong delay with a darkening feedback
 *   the reverb (Reverb of each track): a plate (Dattorro's figure-of-eight tank), its tail (Cola) and darkness
 *     (Oscurecer); it gets the delay's echoes too, so the repeats sit in the same room
 *   the master: saturation, the volume, and the STUTTER (beat repeat): the last 1/4 .. 1/64 note loops while a
 *     stutter key is held or an automatic stutter (Stutter auto) runs */

#define CB_N 2048u
#define DL_N 19200u                              /* 0.44 s a side */
#define ST_N 20480u                              /* the stutter's memory, 0.46 s */
static int16_t cb_buf[CB_N] __attribute__((section(".pool")));
static int16_t dl_l[DL_N] __attribute__((section(".pool"))), dl_r[DL_N] __attribute__((section(".pool")));
static int16_t st_l[ST_N] __attribute__((section(".pool"))), st_r[ST_N] __attribute__((section(".pool")));
static uint32_t cb_w, dl_w, st_w;
static int32_t dl_lp_l, dl_lp_r;
static struct { int32_t on, len, start, pos, prev; } stt;   /* the stutter that plays */
static volatile int32_t fx_stutter_now = -1;               /* for the screen */

/* ----------------------------------------------------------- reverb --- */
enum { RV_DIF0, RV_DIF1, RV_DIF2, RV_DIF3, RV_D1L, RV_D2L, RV_APL, RV_D3L, RV_D1R, RV_D2R, RV_APR, RV_D3R, RV_N };
#define RV_EXC 18u                                  /* the modulation's excursion (16) + 2 */
static const uint16_t RV_SIZE[RV_N] = {142, 107, 379, 277, 672 + 2 * RV_EXC, 4453, 1800, 3720,
                                       908 + 2 * RV_EXC, 4217, 2656, 3163};
#define RV_TOTAL (142 + 107 + 379 + 277 + 672 + 2 * RV_EXC + 4453 + 1800 + 3720 + 908 + 2 * RV_EXC + 4217 + 2656 + 3163 + RV_N)
static int16_t rv_mem[RV_TOTAL] __attribute__((section(".pool")));
static struct {
    int16_t *buf[RV_N];
    uint32_t size[RV_N], w[RV_N];
    int32_t bw, lpl, lpr, fbl, fbr;
    uint32_t lfo;
    uint8_t ready;
} rv;

static void reverb_init(void)
{
    uint32_t i, off = 0;
    for (i = 0; i < RV_N; i++) {
        rv.buf[i] = rv_mem + off;
        rv.size[i] = RV_SIZE[i] + 1u;
        off += rv.size[i];
        rv.w[i] = 0;
    }
    rv.ready = 1;
}
static inline int32_t rv_tap(uint32_t l, uint32_t d)
{
    uint32_t w = rv.w[l], r = w >= d ? w - d : w + rv.size[l] - d;
    return rv.buf[l][r];
}
static inline void rv_push(uint32_t l, int32_t v)
{
    rv.buf[l][rv.w[l]] = (int16_t)sat16(v);
    if (++rv.w[l] == rv.size[l])
        rv.w[l] = 0;
}
static inline int32_t rv_allpass(uint32_t l, int32_t x, int32_t g, uint32_t d)
{
    int32_t del = rv_tap(l, d), v = x - mulq15(g, del);
    rv_push(l, v);
    return del + mulq15(g, v);
}
static inline int32_t rv_modallpass(uint32_t l, int32_t x, int32_t g, uint32_t d_q8)
{
    uint32_t whole = d_q8 >> 8;
    int32_t fr = (int32_t)(d_q8 & 255u), s0 = rv_tap(l, whole), s1 = rv_tap(l, whole + 1u), del, v;
    del = s0 + (((s1 - s0) * fr) >> 8);
    v = x - mulq15(g, del);
    rv_push(l, v);
    return del + mulq15(g, v);
}
/* one sample of the plate: in (mono), adds the wet to *ol, *or_ x mix (Q15) */
static inline void rv_step(int32_t in, int32_t *ol, int32_t *or_, int32_t decay, int32_t dampc, int32_t dif2, int32_t mix)
{
    static const uint16_t TL[7] = {266, 2974, 1913, 1996, 1990, 187, 1066}, TR[7] = {353, 3627, 1228, 2673, 2111, 335, 121};
    int32_t x, a, b, a1, a2, a3, b1, b2, b3, lfo, wl, wr;
    rv.bw += mulq15(26000, in - rv.bw);
    x = rv.bw;
    x = rv_allpass(RV_DIF0, x, 24576, 142);
    x = rv_allpass(RV_DIF1, x, 24576, 107);
    x = rv_allpass(RV_DIF2, x, 20480, 379);
    x = rv_allpass(RV_DIF3, x, 20480, 277);
    rv.lfo += 87650u;                                                       /* 0.9 Hz */
    lfo = sin32(rv.lfo);
    a = x + mulq15(decay, rv.fbr);
    a = rv_modallpass(RV_D1L, a, -22938, (672u << 8) + (uint32_t)((lfo * 16 * 256) >> 15));
    a1 = rv_tap(RV_D2L, 4453);
    rv_push(RV_D2L, a);
    rv.lpl += mulq15(dampc, a1 - rv.lpl);
    a2 = rv_allpass(RV_APL, mulq15(rv.lpl, decay), dif2, 1800);
    a3 = rv_tap(RV_D3L, 3720);
    rv_push(RV_D3L, a2);
    b = x + mulq15(decay, rv.fbl);
    b = rv_modallpass(RV_D1R, b, -22938, (908u << 8) - (uint32_t)((lfo * 16 * 256) >> 15));
    b1 = rv_tap(RV_D2R, 4217);
    rv_push(RV_D2R, b);
    rv.lpr += mulq15(dampc, b1 - rv.lpr);
    b2 = rv_allpass(RV_APR, mulq15(rv.lpr, decay), dif2, 2656);
    b3 = rv_tap(RV_D3R, 3163);
    rv_push(RV_D3R, b2);
    rv.fbl = a3;
    rv.fbr = b3;
    wl = rv_tap(RV_D2R, TL[0]) + rv_tap(RV_D2R, TL[1]) - rv_tap(RV_APR, TL[2]) + rv_tap(RV_D3R, TL[3]) -
         rv_tap(RV_D2L, TL[4]) - rv_tap(RV_APL, TL[5]) - rv_tap(RV_D3L, TL[6]);
    wr = rv_tap(RV_D2L, TR[0]) + rv_tap(RV_D2L, TR[1]) - rv_tap(RV_APL, TR[2]) + rv_tap(RV_D3L, TR[3]) -
         rv_tap(RV_D2R, TR[4]) - rv_tap(RV_APR, TR[5]) - rv_tap(RV_D3R, TR[6]);
    *ol += mulq15(mix, wl);
    *or_ += mulq15(mix, wr);
}

static void fx_render(int32_t *l, int32_t *r, const int32_t *snd, const int32_t *rsnd, uint32_t n)
{
    uint32_t i, spt = seq_samples_per_tick_q8();
    int32_t cfb = gp[G_COMBFB] * 327, cmix = gp[G_COMBMIX] * 327, dfb = gp[G_DLYFB] * 327, dmix = gp[G_DLYMIX] * 327;
    int32_t drive = 32768 + gp[G_DRIVE] * 1600, vol = gp[G_VOLUME] * 327;
    uint32_t cd = (uint32_t)clampi((int32_t)(FS * 16u / (uint32_t)ex_range(gp[G_COMBHZ], 40 * 16, (int32_t)(5.6 * 65536))), 8, CB_N - 1);
    uint32_t dd = (uint32_t)clampi((int32_t)(((uint64_t)DIV_TICKS[clampi(gp[G_DLYT], 0, 5)] * spt) >> 8), 64, DL_N - 1);
    int32_t stut = seq_stutter();
    int32_t rdecay = gp[G_REVDECAY] * 327, rdamp = 32768 - gp[G_REVDAMP] * 300, rmix = gp[G_REVMIX] * 262;   /* up to 0.8 */
    int32_t rdif2 = clampi(rdecay + 4915, 8192, 16384);
    glitch_feed(l, r, n);
    for (i = 0; i < n; i++) {
        int32_t s = snd[i], c, a, b, ol, or_;
        /* the comb */
        c = s + mulq15(cfb, cb_buf[(cb_w + CB_N - cd) & (CB_N - 1u)]);
        cb_buf[cb_w] = (int16_t)sat16(c);
        cb_w = (cb_w + 1u) & (CB_N - 1u);
        s += mulq15(cmix, c - s);
        /* the ping-pong delay */
        a = dl_l[(dl_w + DL_N - dd) % DL_N];
        b = dl_r[(dl_w + DL_N - dd) % DL_N];
        dl_lp_l += (a - dl_lp_l) >> 2;
        dl_lp_r += (b - dl_lp_r) >> 2;
        dl_l[dl_w] = (int16_t)sat16(s + mulq15(dfb, dl_lp_r));
        dl_r[dl_w] = (int16_t)sat16(mulq15(dfb, dl_lp_l));
        dl_w = dl_w + 1u >= DL_N ? 0u : dl_w + 1u;
        ol = l[i] + (s >> 1) + mulq15(dmix, a);
        or_ = r[i] + (s >> 1) + mulq15(dmix, b);
        /* the plate: the reverb sends and a little of the echoes */
        if (rv.ready)
            rv_step(rsnd[i] + (mulq15(dmix, a + b) >> 2), &ol, &or_, rdecay, rdamp, rdif2, rmix);
        /* the master */
        ol = mulq15(soft(mulq15(ol, drive) >> 1), vol) * 5 / 4;
        or_ = mulq15(soft(mulq15(or_, drive) >> 1), vol) * 5 / 4;
        if (stut < 0) {                                              /* the stutter's memory stands still while it loops */
            st_l[st_w] = (int16_t)sat16(ol);
            st_r[st_w] = (int16_t)sat16(or_);
            st_w = st_w + 1u >= ST_N ? 0u : st_w + 1u;
        }
        if (stut >= 0) {
            int32_t len = clampi((int32_t)(((uint64_t)STUT_TICKS[stut] * spt) >> 8), 256, (int32_t)ST_N / 2);
            if (!stt.on || stt.len != len) {                         /* a new stutter: loops the last `len` */
                stt.on = 1;
                stt.len = len;
                stt.start = ((int32_t)st_w - len + (int32_t)ST_N * 2) % (int32_t)ST_N;
                stt.pos = 0;
            }
            {
                int32_t at = (stt.start + stt.pos) % (int32_t)ST_N, fade = 64;
                int32_t sl = st_l[at], sr = st_r[at];
                if (stt.pos < fade || stt.len - stt.pos < fade) {        /* soft loop points */
                    int32_t g = stt.pos < fade ? stt.pos : stt.len - stt.pos;
                    sl = sl * g / fade;
                    sr = sr * g / fade;
                }
                ol = sl;
                or_ = sr;
                if (++stt.pos >= stt.len)
                    stt.pos = 0;
            }
        } else {
            stt.on = 0;
        }
        l[i] = ol;
        r[i] = or_;
    }
    fx_stutter_now = stut;
}
