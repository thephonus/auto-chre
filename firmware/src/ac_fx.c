/* SPDX-License-Identifier: GPL-3.0-only
 * auto-chre: the effects.
 *   the send (Envío FX of each track): a feedback comb filter (metallic, pitched resonance), then a tempo-synced
 *     ping-pong delay with a darkening feedback
 *   the master: saturation, the volume, and the STUTTER (beat repeat): the last 1/4 .. 1/64 note loops while a
 *     stutter key is held or an automatic stutter (Stutter auto) runs */

#define CB_N 2048u
#define DL_N 22050u                              /* 0.5 s a side */
#define ST_N 24576u                              /* the stutter's memory, 0.56 s */
static int16_t cb_buf[CB_N] __attribute__((section(".pool")));
static int16_t dl_l[DL_N] __attribute__((section(".pool"))), dl_r[DL_N] __attribute__((section(".pool")));
static int16_t st_l[ST_N] __attribute__((section(".pool"))), st_r[ST_N] __attribute__((section(".pool")));
static uint32_t cb_w, dl_w, st_w;
static int32_t dl_lp_l, dl_lp_r;
static struct { int32_t on, len, start, pos, prev; } stt;   /* the stutter that plays */
static volatile int32_t fx_stutter_now = -1;               /* for the screen */

static void fx_render(int32_t *l, int32_t *r, const int32_t *snd, uint32_t n)
{
    uint32_t i, spt = seq_samples_per_tick_q8();
    int32_t cfb = gp[G_COMBFB] * 327, cmix = gp[G_COMBMIX] * 327, dfb = gp[G_DLYFB] * 327, dmix = gp[G_DLYMIX] * 327;
    int32_t drive = 32768 + gp[G_DRIVE] * 1600, vol = gp[G_VOLUME] * 327;
    uint32_t cd = (uint32_t)clampi((int32_t)(FS * 16u / (uint32_t)ex_range(gp[G_COMBHZ], 40 * 16, (int32_t)(5.6 * 65536))), 8, CB_N - 1);
    uint32_t dd = (uint32_t)clampi((int32_t)(((uint64_t)DIV_TICKS[clampi(gp[G_DLYT], 0, 5)] * spt) >> 8), 64, DL_N - 1);
    int32_t stut = seq_stutter();
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
