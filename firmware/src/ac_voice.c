/* SPDX-License-Identifier: GPL-3.0-only
 * auto-chre: the six drum voices, one per track (a new hit cuts the last), all synthesis:
 *
 *   Bombo   a sine with a falling pitch, a click and saturation
 *   Metal   six square oscillators at inharmonic ratios (the 808 cymbal's), ring-modulated, high-passed, noise
 *   Ruido   white or sample-and-held noise through a state-variable filter (LP .. BP .. HP) with a sweep
 *   Clic    short blips (sine, square or impulses) that fall in pitch, repeated in bursts
 *   Glitch  a slice of what auto-chre just played (the last 1.5 s), pitched, reversed, looped
 *   FM      two-operator FM with a decaying index and feedback, inharmonic ratios
 *   Cuerda  a plucked string (Karplus-Strong): a burst of noise or a pulse in a tuned delay line that darkens
 *   Modal   a struck object: four tuned resonators (bar, glass, bell, membrane), a hard or soft strike
 *   Ácido   a saw or square through a resonant low-pass whose cutoff falls with the note
 *   Chip    a pulse wave, its width, a fast arpeggio over the scale, a slide and vibrato
 *
 * The tonal engines (Cuerda, Modal, Ácido, Chip) sound the root's note in the octave Tono sets; on every engine the
 * Notas of the track move the pitch by degrees of its scale (Fija, Azar, Sube, Baja, Péndulo, Turing), and with
 * Cuantizar, or Notas other than Fija, everything lands in the scale.
 *
 * Every hit is a little different: Variación moves the engine's settings, the pitch and the level at random. Bits and
 * Decimar crush each voice on its own. The engine settings are 0 .. 100; the curves are below. */

#define GL_N 32768u                              /* the glitch's memory: the mix, mono, at FS / 2 (1.49 s) */
static int16_t gl_buf[GL_N] __attribute__((section(".pool")));
static uint32_t gl_w;                            /* where the next sample goes */

typedef struct {
    uint8_t on, engine, crush_bits, decim;
    int8_t rev;
    uint32_t t;                                  /* samples since the hit */
    int32_t amp;                                 /* the amplitude envelope, Q30 */
    uint32_t adec;                               /* its decay per sample, Q31 */
    int32_t gain;                                /* velocity x level, Q15 */
    int32_t gl, gr, gs, grs;                     /* pan, the delay's and the reverb's sends, Q15 */
    int32_t e[6];                                /* the engine settings of this hit, 0 .. 100 */
    uint32_t ratio_q16;                          /* Afinar and the variation, as a pitch ratio */
    uint32_t ph[6], dph[6];
    int32_t env2;                                /* a second envelope (pitch, index, filter), Q30 */
    uint32_t dec2;
    int32_t lp, bp, hp1, prev;                   /* filter states */
    int32_t f, q;                                /* SVF coefficients, Q15 */
    uint32_t nz;                                 /* noise */
    int32_t hold;
    uint32_t holdn, holdc;
    uint32_t len, gap, bursts;                   /* Clic */
    int32_t gpos, gstep, gstart, glen, gloops;   /* Glitch: Q16 positions in gl_buf */
    int32_t held;                                /* the decimator's sample */
    uint32_t dcount;
    int16_t *ks;                                 /* Cuerda: the delay line (KS_N) */
    int32_t ksp;                                 /* its period, Q16 samples */
    uint32_t ksw;
    int32_t ksfb, kslp;                          /* feedback (Q15), the loop's low-pass state */
    int64_t my1[4], my2[4];                      /* Modal: the resonators' states and coefficients (Q30) */
    int32_t ma1[4], ma2[4], mg[4];
    uint32_t arp_ph, arp_inc, vib;               /* Chip */
    uint32_t arp_dph[3];
} voice_t;
#define KS_N 1024u                               /* the string's longest period: 43 Hz */
static int16_t ks_mem[NTRK + 1][KS_N] __attribute__((section(".pool")));    /* + 1 for the screen's preview */
static voice_t vo[NTRK];
static volatile uint32_t ac_hits[NTRK];          /* the screen's flashes: hits counted per track */

/* a decay of `ms` milliseconds (to 1/e): the step per sample, Q31 */
static uint32_t dec_ms(int32_t ms10)              /* ms x 10 */
{
    uint32_t n = (uint32_t)ms10 * (FS / 100u) / 100u + 1u;     /* samples */
    return 0x80000000u - 0x80000000u / n;
}
/* lo x 2^(oct x v / 100): an exponential range (oct in Q16) */
static int32_t ex_range(int32_t v, int32_t lo, int32_t oct_q16)
{
    return (int32_t)(((int64_t)lo * exp2_q16((uint32_t)((int64_t)oct_q16 * clampi(v, 0, 100) / 100))) >> 16);
}
static inline int32_t env_step(int32_t a, uint32_t dec) { return (int32_t)(((int64_t)a * dec) >> 31); }
static inline int32_t nz_next(voice_t *v)
{
    v->nz ^= v->nz << 13;
    v->nz ^= v->nz >> 17;
    v->nz ^= v->nz << 5;
    return (int32_t)(v->nz >> 17) - 16384;              /* Q15 / 2 */
}
static inline int32_t soft(int32_t x)                    /* Q15 soft clip */
{
    int32_t x2;
    x = clampi(x, -49152, 49152);
    x2 = mulq15(x, x);
    return x - mulq15(mulq15(x2, x), 10923);
}
/* the state-variable filter's tuning: f = 2 sin(pi fc / fs), fc capped for stability */
static int32_t svf_f(int32_t hz)
{
    hz = clampi(hz, 20, 7000);
    return (int32_t)(((int64_t)hz * 2 * 102944) / FS);     /* 2 pi fc / fs, Q15 (102944 = pi x 32768) */
}

/* the scale of a track: its own, or the global one */
static inline int32_t trk_mode(const int16_t *p) { return p[T_SCALE] ? p[T_SCALE] - 1 : gp[G_MODE]; }
/* degree d (any integer) of mode m, in semitones above the root */
static int32_t deg_semis(int32_t m, int32_t d)
{
    int32_t n = MODE_N[m], o = d >= 0 ? d / n : -((-d + n - 1) / n);
    return 12 * o + MODE_DEG[m][d - o * n];
}
/* the nearest note of mode m (relative to the root) to st256 (semitones x 256) */
static int32_t snap_mode(int32_t st256, int32_t m)
{
    int32_t st = (st256 + (st256 >= 0 ? 128 : -128)) / 256, rel = ((st % 12) + 12) % 12, k, best = 0, bd = 99;
    for (k = 0; k <= MODE_N[m]; k++) {
        int32_t s = k < MODE_N[m] ? MODE_DEG[m][k] : 12, d = absi(s - rel);
        if (d < bd) {
            bd = d;
            best = s;
        }
    }
    return (st - rel + best) * 256;
}

/* sets voice v for a hit with the track values p: velocity 1 .. 127; deg: the note's degree of the track's scale
 * (Notas); var: with the per-hit variation */
static void voice_setup(voice_t *v, const int16_t *p, uint32_t vel, int32_t deg, int with_var)
{
    int32_t var = with_var ? p[T_VAR] : 0, i, st, mode = trk_mode(p);
    uint32_t pan;
    v->engine = (uint8_t)p[T_TYPE];
    for (i = 0; i < 6; i++)                                   /* every hit a little different */
        v->e[i] = clampi(p[T_E1 + i] + (int32_t)(var ? ((int32_t)ac_rnd((uint32_t)var + 1u) - var / 2) : 0), 0, 100);
    st = p[T_TUNE] * 256 + deg_semis(mode, deg) * 256 + (var > 30 ? (int32_t)ac_rnd((uint32_t)(var - 30) * 8u) - (var - 30) * 4 : 0);
    if (gp[G_QUANT] || p[T_NOTES])
        st = snap_mode(st, mode);
    v->ratio_q16 = exp2s_q16((int32_t)(((int64_t)st * 5461) >> 8));
    v->gain = (int32_t)vel * 258 * p[T_LEVEL] / 100;          /* Q15 */
    if (var > 50)
        v->gain = mulq15(v->gain, 32768 - (int32_t)ac_rnd((uint32_t)(var - 50) * 200u));
    pan = (uint32_t)(p[T_PAN] + 50) * (0x40000000u / 100u);  /* 0 .. pi/2 */
    v->gl = sin32(pan + 0x40000000u);
    v->gr = sin32(pan);
    v->gs = p[T_SEND] * 327;
    v->grs = p[T_RSEND] * 327;
    v->crush_bits = (uint8_t)p[T_BITS];
    v->decim = (uint8_t)p[T_DECIM];
    v->dcount = 0;
    v->held = 0;
    v->t = 0;
    v->amp = 1 << 30;
    v->env2 = 1 << 30;
    v->lp = v->bp = v->hp1 = v->prev = 0;
    v->nz = ac_rand() | 1u;
    for (i = 0; i < 6; i++)
        v->ph[i] = 0;
    switch (v->engine) {
    case EN_KICK: {
        int32_t hz16 = ex_range(v->e[0], 30 * 16, 2 << 16);              /* 30 .. 120 Hz */
        v->dph[0] = (uint32_t)(((uint64_t)ph_of_hz16((uint32_t)hz16) * v->ratio_q16) >> 16);
        v->adec = dec_ms(ex_range(v->e[1], 300, 5 << 16));                /* 30 ms .. 960 ms */
        v->dph[1] = 0;
        v->dec2 = dec_ms(ex_range(v->e[3], 20, 7 << 16));               /* the sweep: 2 .. 256 ms */
        break;
    }
    case EN_METAL: {
        static const int32_t R[6] = {65536, 94838, 105971, 126257, 164024, 174566};    /* 808 ratios, Q16 */
        int32_t hz16 = ex_range(v->e[0], 120 * 16, (int32_t)(4.0 * 65536)), spread = 30000 + v->e[2] * 700;
        for (i = 0; i < 6; i++) {
            int32_t r = 65536 + (int32_t)(((int64_t)(R[i] - 65536) * spread) >> 16);
            uint32_t d = ph_of_hz16((uint32_t)hz16);
            d = (uint32_t)(((uint64_t)d * (uint32_t)r) >> 16);
            v->dph[i] = (uint32_t)(((uint64_t)d * v->ratio_q16) >> 16);
            v->ph[i] = ac_rand();
        }
        v->adec = dec_ms(ex_range(v->e[1], 80, 8 << 16));                /* 8 ms .. 2 s */
        v->f = svf_f(ex_range(v->e[4], 700, 3 << 16));                    /* the high-pass corner */
        break;
    }
    case EN_NOISE:
        v->f = svf_f(ex_range(v->e[0], 100, (int32_t)(6.1 * 65536)));
        v->q = 32768 * 2 - v->e[1] * 600;                                 /* damping 2 .. 0.17 */
        v->adec = dec_ms(ex_range(v->e[2], 30, (int32_t)(8.6 * 65536)));  /* 3 ms .. 1.2 s */
        v->dec2 = dec_ms(ex_range(v->e[2], 20, 7 << 16));
        v->holdn = 1u + (uint32_t)(v->e[5] * v->e[5] / 150);              /* sample and hold: 1 .. 67 */
        v->holdc = 0;
        break;
    case EN_CLICK: {
        int32_t hz16 = ex_range(v->e[0], 200 * 16, 5 << 16);             /* 200 Hz .. 6.4 kHz */
        v->dph[0] = (uint32_t)(((uint64_t)ph_of_hz16((uint32_t)hz16) * v->ratio_q16) >> 16);
        v->len = (uint32_t)ex_range(v->e[1], 5, (int32_t)(6.4 * 65536)) * (FS / 1000u) / 10u + 8u;   /* 0.5 .. 40 ms */
        v->gap = (uint32_t)ex_range(v->e[5], 10, (int32_t)(5.3 * 65536)) * (FS / 1000u) / 10u + v->len;
        v->bursts = 1u + (uint32_t)v->e[4] * 7u / 100u;
        v->dec2 = dec_ms((int32_t)(v->len * 10u / (FS / 1000u)) + 5);
        v->adec = 0x7FFFFFFFu;
        break;
    }
    case EN_GLITCH: {
        int32_t back = ex_range(v->e[2], 1100, 5 << 16);                  /* 50 ms .. 1.4 s back, x22.05 samples */
        int32_t len = ex_range(v->e[1], 110, (int32_t)(5.9 * 65536));    /* 5 .. 300 ms at FS / 2 */
        int32_t semis = (v->e[0] - 50) * 48 / 100;                        /* -24 .. +24 */
        uint32_t rate = (uint32_t)(((uint64_t)exp2s_q16(semis * 5461) * v->ratio_q16) >> 16);
        back = back * 1005 / 1000;
        len = clampi(len, 64, (int32_t)GL_N / 2);
        v->gstart = (int32_t)((gl_w - (uint32_t)back - (uint32_t)ac_rnd(256u)) & (GL_N - 1u));
        v->glen = len;
        v->gstep = (int32_t)(rate >> 1);                                  /* the buffer runs at FS / 2 */
        v->rev = (int8_t)(ac_rnd(100u) < (uint32_t)v->e[3]);
        v->gloops = 1 + v->e[4] * 7 / 100;
        v->gpos = 0;
        v->adec = dec_ms((int32_t)((6000u * exp2s_q16(-(v->e[5] * 4 * 65536 / 100))) >> 16) + 20);   /* flat .. decaying */
        if (v->e[5] < 8)
            v->adec = 0x7FFFFFFFu;
        break;
    }
    case EN_PLUCK:
    case EN_MODAL:
    case EN_ACID:
    case EN_CHIP: {
        uint32_t j;
        /* the root's note in octave 1 .. 6, times the pitch of the note: a phase step */
        int32_t note = 12 * (2 + v->e[0] * 5 / 100) + gp[G_ROOT];                 /* C1 .. B6 */
        int32_t hz16 = (int32_t)(((int64_t)7040 * exp2s_q16((note - 69) * 5461)) >> 16);    /* 440 x 16 */
        uint32_t d = (uint32_t)(((uint64_t)ph_of_hz16((uint32_t)hz16) * v->ratio_q16) >> 16);
        v->dph[0] = d;
        if (v->engine == EN_PLUCK) {
            uint32_t hz = (uint32_t)(((uint64_t)hz16 * v->ratio_q16) >> 20);      /* Hz */
            int32_t exc = v->e[3];
            v->ksp = (int32_t)(((uint64_t)FS << 16) / (hz < 44u ? 44u : hz));
            if (v->ksp > (int32_t)((KS_N - 2u) << 16))
                v->ksp = (int32_t)((KS_N - 2u) << 16);
            v->ksfb = 32768 - (int32_t)(((int64_t)(101 - v->e[1]) * 6000) / 100) / 8;        /* 0.977 .. 0.9998 */
            v->kslp = 0;
            v->ksw = 0;
            {   /* the excitation over one period: noise blended with a pulse, softened by Posición */
                uint32_t per = (uint32_t)(v->ksp >> 16), passes = (uint32_t)v->e[4] / 20u, q;
                for (j = 0; j < KS_N; j++) {
                    int32_t x = 0;
                    if (j < per) {
                        int32_t nz = nz_next(v) * 2, pl = j * 2u < per ? 24000 : -24000;
                        x = nz + (pl - nz) * exc / 100;
                    }
                    v->ks[j] = (int16_t)x;
                }
                for (q = 0; q < passes; q++)
                    for (j = 1; j < per; j++)
                        v->ks[j] = (int16_t)((v->ks[j] + v->ks[j - 1]) >> 1);
            }
            v->ksw = (uint32_t)(v->ksp >> 16);
            v->adec = dec_ms(ex_range(v->e[1], 1500, 4 << 16));                  /* 150 ms .. 2.4 s */
        } else if (v->engine == EN_MODAL) {
            static const int32_t SETS[4][4] = {{65536, 180600, 354153, 585400},     /* bar: 1, 2.756, 5.404, 8.933 */
                                               {65536, 152043, 278528, 434503},     /* glass */
                                               {65536, 131072, 157286, 196608},     /* bell: 1, 2, 2.4, 3 */
                                               {65536, 104202, 140247, 173670}};    /* membrane: 1, 1.59, 2.14, 2.65 */
            int32_t m = v->e[2] * 3, a = m / 100, f = m % 100, tau = ex_range(v->e[1], 200, (int32_t)(5.5 * 65536));
            for (j = 0; j < 4; j++) {
                int32_t ra = SETS[a][j], rb = SETS[a < 3 ? a + 1 : 3][j], rr = ra + (rb - ra) * f / 100;
                uint32_t w;
                int64_t rq, cw;
                int32_t n;
                rr = 65536 + (int32_t)(((int64_t)(rr - 65536) * (80 + v->e[5] * 40 / 100)) / 100);
                w = (uint32_t)(((uint64_t)d * (uint32_t)rr) >> 16);
                if (w > 0x30000000u)
                    w = 0x30000000u;                                              /* below ~8 kHz */
                n = tau * (int32_t)(FS / 1000u) / 10 / (int32_t)(1u + j * j);        /* higher modes die sooner */
                rq = (1LL << 30) - (1LL << 30) / (n > 4 ? n : 4);
                cw = sin32(w + 0x40000000u);
                v->ma1[j] = (int32_t)((rq * cw) >> 15);                           /* 2 r cos w, Q29 */
                v->ma2[j] = (int32_t)((rq * rq) >> 31);                           /* r^2, Q29 */
                v->mg[j] = sin32(w) / (int32_t)(j + 1u);                          /* unit peak, Q15 */
                v->my1[j] = v->my2[j] = 0;
            }
            v->len = (uint32_t)(2 + (100 - v->e[3]) * 2);                            /* the strike: 2 .. 202 samples */
            v->adec = dec_ms(tau * 3 / 2);
        } else if (v->engine == EN_ACID) {
            v->adec = dec_ms(ex_range(v->e[1], 300, 5 << 16));
            v->dec2 = dec_ms(ex_range(v->e[1], 200, 4 << 16));
            v->q = 65536 - v->e[3] * 620;                                           /* damping 2 .. 0.14 */
        } else {                                                                    /* Chip */
            int32_t k2;
            v->adec = dec_ms(ex_range(v->e[1], 200, 6 << 16));
            for (k2 = 0; k2 < 3; k2++) {
                int32_t s2 = deg_semis(mode, 2 * k2) * 256;                          /* the triad of the scale */
                v->arp_dph[k2] = (uint32_t)(((uint64_t)d * exp2s_q16(s2 * 5461 / 256)) >> 16);
            }
            v->arp_inc = (uint32_t)v->e[3] * 6000u;                                    /* 0 .. 24 notes a second */
            v->arp_ph = 0;
            v->vib = 0;
            v->dec2 = dec_ms(ex_range(v->e[4], 100, 5 << 16));
        }
        break;
    }
    default: {                                                            /* FM */
        static const int32_t RAT[11] = {32768, 65536, 92406, 131072, 180879, 196608, 229376, 275251, 353894, 458752, 530842};
        int32_t hz16 = ex_range(v->e[0], 40 * 16, (int32_t)(5.6 * 65536)), ri = v->e[1] * 10, a = ri / 100, f = ri % 100;
        int32_t r = a >= 10 ? RAT[10] : RAT[a] + (RAT[a + 1] - RAT[a]) * f / 100;
        v->dph[0] = (uint32_t)(((uint64_t)ph_of_hz16((uint32_t)hz16) * v->ratio_q16) >> 16);
        v->dph[1] = (uint32_t)(((uint64_t)v->dph[0] * (uint32_t)r) >> 16);
        v->dec2 = dec_ms(ex_range(v->e[3], 20, (int32_t)(8.0 * 65536)));
        v->adec = dec_ms(ex_range(v->e[4], 100, (int32_t)(7.2 * 65536)));
        break;
    }
    }
    v->on = 1;
}

/* the Notas of each track: a counter, and the Turing loop of 8 degrees */
static uint32_t nt_i[NTRK];
static int8_t nt_seq[NTRK][8];
static int32_t note_degree(uint32_t k)
{
    const int16_t *p = tp[k];
    int32_t top = p[T_RANGE] * MODE_N[trk_mode(p)], i = (int32_t)(nt_i[k]++), d;
    switch (p[T_NOTES]) {
    case 1: return (int32_t)ac_rnd((uint32_t)top);                              /* Azar */
    case 2: return i % top;                                                     /* Sube */
    case 3: return top - 1 - i % top;                                           /* Baja */
    case 4: d = i % (2 * top - 2 > 0 ? 2 * top - 2 : 1); return d < top ? d : 2 * top - 2 - d;     /* Péndulo */
    case 5:                                                                     /* Turing: a loop of 8 that mutates */
        d = i % 8;
        if (ac_rnd(100u) < (uint32_t)p[T_TURING] / 2u + 3u || nt_seq[k][d] >= top)
            nt_seq[k][d] = (int8_t)ac_rnd((uint32_t)top);
        return nt_seq[k][d];
    default: return 0;                                                          /* Fija */
    }
}

/* a hit on track k: velocity 1 .. 127 */
static void voice_hit(uint32_t k, uint32_t vel)
{
    vo[k].ks = ks_mem[k];
    voice_setup(&vo[k], tp[k], vel, note_degree(k), 1);
    ac_hits[k]++;
}

/* one sample of track k's engine, Q15 (before level, crush, pan) */
static inline int32_t voice_sample(voice_t *v)
{
    int32_t s = 0, i;
    switch (v->engine) {
    case EN_KICK: {
        int32_t sweep = (int32_t)(((int64_t)v->env2 * (v->e[2] * 3)) >> 30) * 874;    /* up to 4 octaves, Q16 */
        uint32_t d = v->dph[0];
        if ((v->t & 7u) == 0u)
            v->dph[1] = (uint32_t)(((uint64_t)d * exp2_q16((uint32_t)sweep)) >> 16);
        v->ph[0] += v->dph[1] ? v->dph[1] : d;
        s = sin32(v->ph[0]);
        if (v->t < 90u)                                                   /* the click */
            s += mulq15(nz_next(v) * 2, v->e[4] * 327) * (int32_t)(90u - v->t) / 90;
        v->env2 = env_step(v->env2, v->dec2);
        s = soft(mulq15(s, 32768 + v->e[5] * 1300));
        break;
    }
    case EN_METAL: {
        int32_t a = 0, b = 0, hp;
        for (i = 0; i < 6; i++) {
            v->ph[i] += v->dph[i];
            if (i < 3)
                a += (int32_t)(v->ph[i] >> 31) ? 5000 : -5000;
            else
                b += (int32_t)(v->ph[i] >> 31) ? 5000 : -5000;
        }
        s = a + b + mulq15(mulq15(a, b) * 4, v->e[3] * 327) - mulq15(a + b, v->e[3] * 200);
        s += mulq15(nz_next(v) * 2, v->e[5] * 200);
        hp = s - v->hp1;                                                  /* a two-pole-ish high-pass */
        v->hp1 += mulq15(v->f, hp);
        s = hp - v->lp;
        v->lp += mulq15(v->f, s);
        s = s * 2;
        break;
    }
    case EN_NOISE: {
        int32_t in, hpo, f = v->f, sweep;
        if (v->holdc == 0u) {
            v->hold = nz_next(v) * 2;
            v->holdc = v->holdn;
        }
        v->holdc--;
        in = v->hold;
        sweep = (int32_t)(((int64_t)v->env2 * v->e[4]) >> 30);            /* the cutoff falls from up to 3 octaves above */
        if (sweep > 0)
            f = clampi(f + mulq15(f, sweep * 980), 0, 20000);
        v->env2 = env_step(v->env2, v->dec2);
        v->lp += mulq15(f, v->bp);
        hpo = in - v->lp - mulq15(v->q, v->bp);
        v->bp += mulq15(f, hpo);
        v->lp = clampi(v->lp, -60000, 60000);
        v->bp = clampi(v->bp, -60000, 60000);
        if (v->e[3] < 50)
            s = v->lp + (v->bp - v->lp) * v->e[3] / 50;
        else
            s = v->bp + (hpo - v->bp) * (v->e[3] - 50) / 50;
        break;
    }
    case EN_CLICK: {
        uint32_t n = v->t % v->gap, k = v->t / v->gap;
        if (k >= v->bursts) {
            v->on = 0;
            return 0;
        }
        if (n == 0u)
            v->env2 = (1 << 30) - (int32_t)k * ((1 << 30) / 8);           /* each repeat a bit softer */
        if (n < v->len) {
            uint32_t drop = (uint32_t)(((uint64_t)n * (uint32_t)v->e[3] * 3u << 16) / (v->len * 100u));
            uint32_t d = (uint32_t)(((uint64_t)v->dph[0] * exp2s_q16(-(int32_t)drop)) >> 16);
            int32_t w;
            v->ph[0] += d;
            if (v->e[2] < 34)
                w = sin32(v->ph[0]);
            else if (v->e[2] < 67)
                w = (v->ph[0] >> 31) ? 26000 : -26000;
            else
                w = (v->ph[0] >> 26) == 0u ? 32000 : 0;                      /* a narrow impulse train */
            s = (int32_t)(((int64_t)w * (v->env2 >> 15)) >> 15);
            if (n + 32u > v->len)
                s = s * (int32_t)(v->len - n) / 32;
        }
        break;
    }
    case EN_GLITCH: {
        int32_t whole, fr, a, b, at;
        uint32_t idx;
        if (v->gpos >= v->glen << 16) {
            if (--v->gloops <= 0) {
                v->on = 0;
                return 0;
            }
            v->gpos -= v->glen << 16;
            v->amp = 1 << 30;
        }
        whole = v->gpos >> 16;
        fr = (v->gpos >> 1) & 0x7FFF;
        at = v->rev ? v->glen - 1 - whole : whole;
        idx = (uint32_t)(v->gstart + at) & (GL_N - 1u);
        a = gl_buf[idx];
        b = gl_buf[(idx + (v->rev ? GL_N - 1u : 1u)) & (GL_N - 1u)];
        s = a + (((b - a) * fr) >> 15);
        {   /* 2 ms fades at the ends of the slice */
            int32_t edge = 44, left = v->glen - whole;
            if (whole < edge)
                s = s * whole / edge;
            else if (left < edge)
                s = s * left / edge;
        }
        v->gpos += v->gstep;
        break;
    }
    case EN_PLUCK: {
        int32_t rd = (int32_t)(v->ksw << 16) - v->ksp, whole, fr, a, b, y;
        uint32_t i0;
        rd += (int32_t)(KS_N << 16);
        whole = (rd >> 16) % (int32_t)KS_N;
        fr = (rd >> 1) & 0x7FFF;
        i0 = (uint32_t)whole;
        a = v->ks[i0];
        b = v->ks[(i0 + 1u) % KS_N];
        y = a + (((b - a) * fr) >> 15);
        v->kslp += mulq15(6000 + v->e[2] * 260, y - v->kslp);              /* Brillo: the loop's low-pass */
        v->ks[v->ksw] = (int16_t)sat16(mulq15(v->kslp, v->ksfb));
        v->ksw = (v->ksw + 1u) % KS_N;
        s = soft(mulq15(y, 32768 + v->e[5] * 650));
        break;
    }
    case EN_MODAL: {
        int32_t x = 0, j;
        if (v->t < v->len)                                                  /* the strike: a click and some noise */
            x = (v->t == 0u ? 30000 : 0) + mulq15(nz_next(v) * 2, v->e[4] * 300) * (int32_t)(v->len - v->t) / (int32_t)v->len;
        for (j = 0; j < 4; j++) {
            int64_t y = ((int64_t)v->ma1[j] * v->my1[j] - (int64_t)v->ma2[j] * v->my2[j]) >> 29;     /* Q8 */
            y += ((int64_t)x * v->mg[j]) >> 7;
            v->my2[j] = v->my1[j];
            v->my1[j] = y;
            s += (int32_t)(y >> 8);
        }
        s = clampi(s, -60000, 60000);
        break;
    }
    case EN_ACID: {
        int32_t w, f, hpo;
        v->ph[0] += v->dph[0];
        w = v->e[5] < 50 ? (int32_t)(v->ph[0] >> 16) - 32768 : ((v->ph[0] >> 31) ? 26000 : -26000);
        f = svf_f(ex_range(v->e[2], 80, 6 << 16) + (int32_t)(((int64_t)v->env2 * v->e[4] * 60) >> 30));
        v->env2 = env_step(v->env2, v->dec2);
        v->lp += mulq15(f, v->bp);
        hpo = w - v->lp - mulq15(v->q, v->bp);
        v->bp += mulq15(f, hpo);
        v->lp = clampi(v->lp, -70000, 70000);
        v->bp = clampi(v->bp, -70000, 70000);
        s = soft(v->lp >> 1) * 2;
        break;
    }
    case EN_CHIP: {
        uint32_t d = v->arp_inc ? v->arp_dph[(v->arp_ph >> 30) % 3u] : v->dph[0], pw = 0x10000000u + (uint32_t)v->e[2] * 0x00700000u;
        int32_t slide = (int32_t)(((int64_t)(1 << 30) - v->env2) >> 14) * (v->e[4] - 50) / 25;   /* +-2 octaves, Q16 */
        v->arp_ph += v->arp_inc;
        v->vib += 5u << 16;                                                   /* 3.4 Hz */
        d = (uint32_t)(((uint64_t)d * exp2s_q16(slide + mulq15(sin32(v->vib), v->e[5] * 20))) >> 16);
        v->env2 = env_step(v->env2, v->dec2);
        v->ph[0] += d;
        s = v->ph[0] < pw ? 22000 : -22000;
        break;
    }
    default: {                                                            /* FM */
        int32_t mod, idx = (int32_t)(((int64_t)v->env2 * (v->e[2] * 10)) >> 30);    /* 0 .. 1000 (x0.01 rad) */
        uint32_t off;
        v->ph[1] += v->dph[1];
        mod = sin32(v->ph[1] + (uint32_t)(v->prev * v->e[5] * 400));
        v->prev = mod;
        off = (uint32_t)((int64_t)idx * mod * 209);                       /* idx / 100 rad: x 2^32 / (2 pi) / 100 / 32768 */
        v->ph[0] += v->dph[0];
        s = sin32(v->ph[0] + off);
        v->env2 = env_step(v->env2, v->dec2);
        break;
    }
    }
    return s;
}

/* the voices into the mix (l, r) and the effects' send, n frames (adds) */
static void voices_render(int32_t *l, int32_t *r, int32_t *snd, int32_t *rsnd, uint32_t n)
{
    uint32_t k, i;
    for (k = 0; k < NTRK; k++) {
        voice_t *v = &vo[k];
        int32_t mask;
        if (!v->on)
            continue;
        mask = v->crush_bits >= 16 ? -1 : (int32_t)(0xFFFFFFFFu << (16u - v->crush_bits));
        for (i = 0; i < n && v->on; i++) {
            int32_t s, a;
            if (v->dcount == 0u) {
                s = voice_sample(v);
                a = v->amp >> 15;
                s = mulq15(s, a);
                v->held = (s & mask);
                v->dcount = v->decim;
            } else {
                if (v->engine == EN_GLITCH || v->engine == EN_CLICK || EN_TONAL(v->engine))
                    (void)voice_sample(v);                                  /* time runs on */
            }
            v->dcount--;
            v->t++;
            v->amp = env_step(v->amp, v->adec);
            s = mulq15(v->held, v->gain);
            l[i] += mulq15(s, v->gl);
            r[i] += mulq15(s, v->gr);
            snd[i] += mulq15(s, v->gs);
            rsnd[i] += mulq15(s, v->grs);
            if (v->amp < (1 << 16) && v->engine != EN_GLITCH && v->engine != EN_CLICK)
                v->on = 0;
            if (v->t > FS * 4u)
                v->on = 0;
        }
    }
}

/* the mix, for the glitch to take slices from (every other sample) */
static void glitch_feed(const int32_t *l, const int32_t *r, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i += 2u) {
        gl_buf[gl_w] = (int16_t)sat16((l[i] + r[i]) >> 1);
        gl_w = (gl_w + 1u) & (GL_N - 1u);
    }
}
